#include "tls.h"
#include "tcp.h"
#include "arp.h"
#include "sha256.h"
#include "aes.h"
#include "bignum.h"
#include "string.h"
#include "klog.h"

/* TLS 1.2, cipher suite TLS_RSA_WITH_AES_128_CBC_SHA256 (0x003C)
 * No certificate verification (hobby OS, no CA store). */

#define TLS_VER_MAJOR 3
#define TLS_VER_MINOR 3   /* TLS 1.2 */
#define TLS_RT_HANDSHAKE       22
#define TLS_RT_CHANGE_CIPHER   20
#define TLS_RT_ALERT           21
#define TLS_RT_DATA            23
#define TLS_HT_CLIENT_HELLO    1
#define TLS_HT_SERVER_HELLO    2
#define TLS_HT_CERTIFICATE     11
#define TLS_HT_SERVER_DONE     14
#define TLS_HT_CLIENT_KEY_EX   16
#define TLS_HT_FINISHED        20
#define TLS_CIPHER_RSA_AES128_CBC_SHA256  0x003C

/* Simple LCG PRNG seeded from a static counter (sufficient for hobby OS) */
static uint32_t prng_state = 0xdeadbeef;
static uint8_t prng_byte(void)
{
    prng_state = prng_state * 1664525 + 1013904223;
    return (uint8_t)(prng_state >> 16);
}
static void prng_fill(uint8_t *buf, int len)
{
    int i;
    for (i = 0; i < len; i++) buf[i] = prng_byte();
}

/* ---- Record layer helpers ---- */

static uint16_t u16be(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static uint32_t u24be(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint32_t u32be(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void put_u8 (uint8_t *p, uint8_t  v) { p[0]=v; }
static void __attribute__((unused)) put_u16(uint8_t *p, uint16_t v) { p[0]=(v>>8)&0xFF; p[1]=v&0xFF; }
static void put_u24(uint8_t *p, uint32_t v) { p[0]=(v>>16)&0xFF; p[1]=(v>>8)&0xFF; p[2]=v&0xFF; }

/* Send a raw TLS record */
static int tls_send_raw(tls_ctx_t *ctx, uint8_t type, const uint8_t *data, int len)
{
    uint8_t hdr[5];
    hdr[0]=type; hdr[1]=TLS_VER_MAJOR; hdr[2]=TLS_VER_MINOR;
    hdr[3]=(uint8_t)(len>>8); hdr[4]=(uint8_t)(len);
    if (tcp_send2(ctx->fd, hdr, 5) != 0) return -1;
    if (len > 0 && tcp_send2(ctx->fd, (void*)data, len) != 0) return -1;
    return 0;
}

/* Append to handshake transcript */
static void hs_append(tls_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if (ctx->hs_len + len <= sizeof(ctx->hs_buf))
        memcpy(ctx->hs_buf + ctx->hs_len, data, len);
    ctx->hs_len += len;
}

/* Send a handshake message (type + 3-byte length + body), also records transcript */
static int tls_send_hs(tls_ctx_t *ctx, uint8_t hs_type, const uint8_t *body, int blen)
{
    uint8_t hdr[4];
    hdr[0] = hs_type;
    put_u24(hdr+1, (uint32_t)blen);
    hs_append(ctx, hdr, 4);
    hs_append(ctx, body, (uint32_t)blen);
    /* wrap in record */
    uint8_t rec[8192];
    if (4 + blen > (int)sizeof(rec)) return -1;
    rec[0]=hs_type; put_u24(rec+1,(uint32_t)blen);
    memcpy(rec+4, body, (uint32_t)blen);
    return tls_send_raw(ctx, TLS_RT_HANDSHAKE, rec, 4+blen);
}

/* ---- TLS PRF (RFC 5246): P_SHA256 ---- */
static void prf_p_sha256(const uint8_t *secret, uint32_t slen,
                          const uint8_t *seed,   uint32_t seedlen,
                          uint8_t *out, uint32_t olen)
{
    uint8_t A[32], tmp[32], hmac_input[32 + 256];
    uint32_t pos = 0;
    int i;
    /* A(0) = seed, A(1) = HMAC(secret, A(0)), ... */
    hmac_sha256(secret, slen, seed, seedlen, A); /* A(1) */
    while (pos < olen) {
        /* HMAC(secret, A(i) || seed) */
        if (32 + seedlen > sizeof(hmac_input)) break;
        memcpy(hmac_input, A, 32);
        memcpy(hmac_input+32, seed, seedlen);
        hmac_sha256(secret, slen, hmac_input, 32+seedlen, tmp);
        for (i = 0; i < 32 && pos < olen; i++, pos++) out[pos] = tmp[i];
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, slen, A, 32, A);
    }
}

static void tls_prf(const uint8_t *secret, uint32_t slen,
                    const char *label, uint32_t llen,
                    const uint8_t *seed, uint32_t seedlen,
                    uint8_t *out, uint32_t olen)
{
    uint8_t ls[256];
    uint32_t lslen = llen + seedlen;
    if (lslen > sizeof(ls)) return;
    memcpy(ls, label, llen);
    memcpy(ls+llen, seed, seedlen);
    prf_p_sha256(secret, slen, ls, lslen, out, olen);
}

/* ---- Receive raw bytes from TCP ---- */
static int raw_recv(tls_ctx_t *ctx, uint8_t *buf, int need)
{
    int got = 0;
    while (got < need) {
        int n = tcp_recv2(ctx->fd, buf+got, need-got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ---- Receive one TLS record (pre-handshake, plaintext) ---- */
/* `cap` is the actual capacity of the caller's `data` buffer -- some
 * call sites pass stack buffers much smaller than TLS_RX_BUF (e.g.
 * an 8-byte ChangeCipherSpec buffer, a 512-byte Finished-message
 * buffer). The record-length field in `hdr` is fully controlled by
 * the remote TLS server; validating it only against the generic
 * TLS_RX_BUF (8192) instead of the real destination size let a
 * malicious/compromised server declare a length up to 8192 and have
 * raw_recv() write that many bytes into a far smaller stack array --
 * a remotely triggerable stack buffer overflow reachable from any
 * outbound HTTPS connection. */
static int recv_record(tls_ctx_t *ctx, uint8_t *type, uint8_t *data, int cap, int *len)
{
    uint8_t hdr[5];
    if (raw_recv(ctx, hdr, 5) != 0) return -1;
    *type = hdr[0];
    int rlen = u16be(hdr+3);
    if (rlen < 0 || rlen > cap) return -1;
    if (raw_recv(ctx, data, rlen) != 0) return -1;
    *len = rlen;
    return 0;
}

/* ---- PKCS#1 v1.5 encrypt (for ClientKeyExchange) ---- */
/* rsa_pub_encrypt: encrypt 48-byte msg using server public key (mod, mod_len bytes) */
static int pkcs1_encrypt(const uint8_t *msg, int mlen,
                          const uint8_t *mod, int mod_len,
                          uint32_t pub_exp,
                          uint8_t *out, int out_len)
{
    /* We only support 256-byte (2048-bit) RSA */
    if (mod_len != 256 || out_len < 256) return -1;
    uint8_t padded[256];
    /* EM = 0x00 || 0x02 || PS || 0x00 || M */
    int ps_len = 256 - 3 - mlen;
    if (ps_len < 8) return -1;
    padded[0] = 0x00;
    padded[1] = 0x02;
    int i;
    for (i = 0; i < ps_len; i++) {
        uint8_t r;
        do { r = prng_byte(); } while (r == 0);
        padded[2+i] = r;
    }
    padded[2+ps_len] = 0x00;
    memcpy(padded+3+ps_len, msg, (uint32_t)mlen);
    rsa2048_pub_encrypt(padded, mod, pub_exp, out);
    return 0;
}

/* ---- Parse ASN.1 DER to extract RSA public key from Certificate ---- */
/* Returns 1 if found, fills mod[256] and exp (usually 65537) */
static int parse_rsa_pubkey(const uint8_t *cert, int clen,
                             uint8_t mod[256], uint32_t *exp)
{
    /* Scan for RSA OID: 2a 86 48 86 f7 0d 01 01 01 */
    static const uint8_t rsa_oid[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    int i;
    for (i = 0; i < clen - 9; i++) {
        if (memcmp(cert+i, rsa_oid, 9) == 0) {
            /* After OID: skip to BIT STRING containing SEQUENCE{modulus, exponent} */
            int pos = i + 9;
            while (pos < clen && cert[pos] != 0x03) pos++; /* find BIT STRING */
            if (pos >= clen) return 0;
            pos++; /* skip tag */
            /* skip length bytes */
            if (cert[pos] & 0x80) {
                int lbytes = cert[pos] & 0x7f;
                pos += 1 + lbytes;
            } else {
                pos++;
            }
            pos++; /* skip 0x00 (unused bits) */
            /* Now at SEQUENCE */
            if (pos >= clen || cert[pos] != 0x30) return 0;
            pos++; /* skip SEQUENCE tag */
            if (cert[pos] & 0x80) {
                int lbytes = cert[pos] & 0x7f;
                pos += 1 + lbytes;
            } else {
                pos++;
            }
            /* INTEGER: modulus */
            if (pos >= clen || cert[pos] != 0x02) return 0;
            pos++;
            uint32_t mlen;
            if (cert[pos] & 0x80) {
                int lbytes = cert[pos] & 0x7f;
                mlen = 0;
                int li;
                for (li = 0; li < lbytes; li++) mlen = (mlen << 8) | cert[pos+1+li];
                pos += 1 + lbytes;
            } else {
                mlen = cert[pos++];
            }
            /* Skip leading zero byte if present */
            if (mlen > 0 && cert[pos] == 0x00) { pos++; mlen--; }
            if (mlen != 256) return 0;
            memcpy(mod, cert+pos, 256);
            pos += 256;
            /* INTEGER: exponent */
            if (pos >= clen || cert[pos] != 0x02) return 0;
            pos++;
            uint32_t elen = cert[pos++];
            *exp = 0;
            uint32_t ei;
            for (ei = 0; ei < elen && ei < 4; ei++) *exp = (*exp << 8) | cert[pos+ei];
            return 1;
        }
    }
    return 0;
}

/* ---- AES-128-CBC encrypt with HMAC-SHA256 MAC (TLS 1.2 record) ---- */
static int tls_encrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *data, int dlen,
                               uint8_t *out, int *out_len)
{
    /* MAC = HMAC-SHA256(client_mac, seq || type || version || length || data) */
    uint8_t mac_input[8+1+2+2+16384];
    int mi = 0;
    int i;
    /* seq (8 bytes big-endian) */
    for (i = 7; i >= 0; i--) { mac_input[mi++] = (uint8_t)(ctx->tx_seq >> (i*8)); }
    mac_input[mi++] = type;
    mac_input[mi++] = TLS_VER_MAJOR; mac_input[mi++] = TLS_VER_MINOR;
    mac_input[mi++] = (uint8_t)(dlen>>8); mac_input[mi++] = (uint8_t)dlen;
    memcpy(mac_input+mi, data, (uint32_t)dlen); mi += dlen;
    uint8_t mac[32];
    hmac_sha256(ctx->client_mac, 32, mac_input, (uint32_t)mi, mac);

    /* IV (random 16 bytes) */
    uint8_t iv[16];
    prng_fill(iv, 16);

    /* Plaintext = data || mac (32 bytes) */
    int pt_len = dlen + 32;
    /* PKCS#7 padding to AES block size (16) */
    int pad_len = 16 - (pt_len % 16);
    int total_pt = pt_len + pad_len;

    uint8_t plaintext[16384 + 32 + 16];
    memcpy(plaintext, data, (uint32_t)dlen);
    memcpy(plaintext+dlen, mac, 32);
    for (i = 0; i < pad_len; i++) plaintext[pt_len+i] = (uint8_t)(pad_len-1);

    /* CBC encrypt */
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    uint8_t *ct = out + 16;
    for (i = 0; i < total_pt; i += 16) {
        uint8_t blk[16];
        int j;
        for (j = 0; j < 16; j++) blk[j] = plaintext[i+j] ^ prev[j];
        aes128_encrypt(ctx->client_write_key, blk, ct+i);
        memcpy(prev, ct+i, 16);
    }
    memcpy(out, iv, 16);
    *out_len = 16 + total_pt;
    ctx->tx_seq++;
    return 0;
}

/* ---- AES-128-CBC decrypt (TLS 1.2 record from server) ---- */
static int tls_decrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *in, int in_len,
                               uint8_t *out, int *out_len)
{
    if (in_len < 16 + 16) return -1;
    const uint8_t *iv = in;
    const uint8_t *ct = in + 16;
    int ct_len = in_len - 16;
    if (ct_len % 16 != 0) return -1;

    uint8_t plaintext[TLS_RX_BUF];
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    int i;
    for (i = 0; i < ct_len; i += 16) {
        aes128_decrypt(ctx->server_write_key, ct+i, plaintext+i);
        int j;
        for (j = 0; j < 16; j++) plaintext[i+j] ^= prev[j];
        memcpy(prev, ct+i, 16);
    }
    /* Remove PKCS#7 padding */
    uint8_t pad = plaintext[ct_len-1];
    if (pad >= 16 || ct_len - 1 - (int)pad < 0) return -1;
    int data_mac_len = ct_len - 1 - pad;
    if (data_mac_len < 32) return -1;
    int data_len = data_mac_len - 32;

    /* Verify MAC */
    uint8_t mac_input[8+1+2+2+16384];
    int mi = 0;
    for (i = 7; i >= 0; i--) mac_input[mi++] = (uint8_t)(ctx->rx_seq >> (i*8));
    mac_input[mi++] = type;
    mac_input[mi++] = TLS_VER_MAJOR; mac_input[mi++] = TLS_VER_MINOR;
    mac_input[mi++] = (uint8_t)(data_len>>8); mac_input[mi++] = (uint8_t)data_len;
    memcpy(mac_input+mi, plaintext, (uint32_t)data_len); mi += data_len;
    uint8_t expected_mac[32];
    hmac_sha256(ctx->server_mac, 32, mac_input, (uint32_t)mi, expected_mac);
    /* Compare (timing-safe enough for hobby OS) */
    int bad = 0;
    for (i = 0; i < 32; i++) bad |= (expected_mac[i] ^ plaintext[data_len+i]);
    if (bad) return -1;

    memcpy(out, plaintext, (uint32_t)data_len);
    *out_len = data_len;
    ctx->rx_seq++;
    return 0;
}

/* ---- Receive encrypted TLS record, decrypt into rx_plain ---- */
static int recv_encrypted_record(tls_ctx_t *ctx)
{
    uint8_t hdr[5], data[TLS_RX_BUF];
    if (raw_recv(ctx, hdr, 5) != 0) return -1;
    uint8_t type = hdr[0];
    int rlen = u16be(hdr+3);
    if (rlen <= 0 || rlen > TLS_RX_BUF) return -1;
    if (raw_recv(ctx, data, rlen) != 0) return -1;

    if (type == TLS_RT_ALERT) return -1;
    if (type != TLS_RT_DATA && type != TLS_RT_HANDSHAKE) return -1;

    int plain_len = 0;
    uint8_t plain[TLS_RX_BUF];
    if (tls_decrypt_record(ctx, type, data, rlen, plain, &plain_len) != 0) return -1;

    if (ctx->rx_plain_pos >= ctx->rx_plain_len) {
        ctx->rx_plain_len = 0; ctx->rx_plain_pos = 0;
    }
    if (ctx->rx_plain_len + (uint32_t)plain_len > TLS_RX_BUF) return -1;
    memcpy(ctx->rx_plain + ctx->rx_plain_len, plain, (uint32_t)plain_len);
    ctx->rx_plain_len += (uint32_t)plain_len;
    return 0;
}

/* ---- Handshake diagnostics ----
 * tls_connect() used to just goto fail from any of a dozen call sites
 * and return -1, so a failed HTTPS fetch gave zero indication of
 * *where* the handshake actually broke down -- whether the server
 * rejected our only cipher suite outright (a fatal Alert record before
 * ServerHello ever arrives, likely for any server that doesn't offer
 * legacy RSA key exchange), our x509 parser couldn't find an RSA key
 * in its certificate, or the exchange completed but Finished
 * verification failed. Each step now logs to klog so dmesg shows
 * exactly how far a failed connection got. */
static void tls_log(const char *s)
{
    klog_write("tls: ");
    klog_write(s);
    klog_write("\n");
}

/* Returns 1 (and logs the alert's level/description) if `rec_type` is
 * a TLS alert record, so callers can tell "server actively refused
 * this" apart from "reply never arrived"/"garbled reply". */
static int tls_log_if_alert(uint8_t rec_type, const uint8_t *data, int len)
{
    if (rec_type != TLS_RT_ALERT) return 0;
    static const char hex[] = "0123456789abcdef";
    char buf[48];
    int i = 0;
    const char *p = "received fatal alert level=";
    while (*p) buf[i++] = *p++;
    uint8_t lvl  = len >= 1 ? data[0] : 0xFF;
    uint8_t desc = len >= 2 ? data[1] : 0xFF;
    buf[i++] = hex[(lvl >> 4) & 0xF]; buf[i++] = hex[lvl & 0xF];
    p = " desc=";
    while (*p) buf[i++] = *p++;
    buf[i++] = hex[(desc >> 4) & 0xF]; buf[i++] = hex[desc & 0xF];
    buf[i] = '\0';
    tls_log(buf);
    return 1;
}

/* ---- TLS handshake ---- */
int tls_connect(tls_ctx_t *ctx, uint32_t ip, uint16_t port)
{
    uint8_t hs_data[8192];
    int hs_len_recv;
    uint8_t rec_type;
    int rc = TLS_ERR_HANDSHAKE; /* default for the fail: label; overridden at the specific sites below */

    memset(ctx, 0, sizeof(*ctx));

    ctx->fd = tcp_socket();
    if (ctx->fd < 0) return TCP_ERR_NOSOCK;
    int trc = tcp_connect2(ctx->fd, ip, port);
    if (trc != 0) {
        tcp_close2(ctx->fd);
        return trc; /* propagate arp_resolve()'s/IP_ERR_NOMEM/TCP_ERR_* verbatim -- see tcp.h */
    }
    tls_log("TCP connected, sending ClientHello");

    /* --- Step 1: ClientHello --- */
    prng_fill(ctx->client_rand, 32);
    /* Timestamp in first 4 bytes */
    uint32_t ts = 0;  /* no real time source, use 0 */
    ctx->client_rand[0]=(uint8_t)(ts>>24); ctx->client_rand[1]=(uint8_t)(ts>>16);
    ctx->client_rand[2]=(uint8_t)(ts>>8);  ctx->client_rand[3]=(uint8_t)ts;

    uint8_t ch[128];
    int cpos = 0;
    ch[cpos++] = TLS_VER_MAJOR; ch[cpos++] = TLS_VER_MINOR; /* client version */
    memcpy(ch+cpos, ctx->client_rand, 32); cpos += 32;
    ch[cpos++] = 0; /* session ID length = 0 */
    /* cipher suites */
    ch[cpos++] = 0; ch[cpos++] = 2; /* 1 suite */
    ch[cpos++] = (TLS_CIPHER_RSA_AES128_CBC_SHA256>>8)&0xFF;
    ch[cpos++] = TLS_CIPHER_RSA_AES128_CBC_SHA256 & 0xFF;
    /* compression: null only */
    ch[cpos++] = 1; ch[cpos++] = 0;
    /* no extensions */
    if (tls_send_hs(ctx, TLS_HT_CLIENT_HELLO, ch, cpos) != 0) {
        tls_log("ClientHello send failed");
        goto fail;
    }

    /* --- Step 2: ServerHello --- */
    if (recv_record(ctx, &rec_type, hs_data, (int)sizeof(hs_data), &hs_len_recv) != 0) {
        tls_log("no reply after ClientHello (connection closed or timed out)");
        goto fail;
    }
    if (tls_log_if_alert(rec_type, hs_data, hs_len_recv)) { rc = TLS_ERR_ALERT; goto fail; }
    if (rec_type != TLS_RT_HANDSHAKE) {
        tls_log("expected ServerHello, got a different record type");
        goto fail;
    }
    tls_log("ServerHello received");
    {
        int pos = 0;
        while (pos < hs_len_recv) {
            uint8_t ht = hs_data[pos];
            uint32_t hlen = u24be(hs_data+pos+1);
            uint8_t *hbody = hs_data+pos+4;
            hs_append(ctx, hs_data+pos, 4+(uint32_t)hlen);
            if (ht == TLS_HT_SERVER_HELLO) {
                /* version (2) + server_random (32) + session_id_len (1) + ... */
                if (hlen < 35) {
                    tls_log("ServerHello message too short to parse");
                    goto fail;
                }
                memcpy(ctx->server_rand, hbody+2, 32);
                /* check cipher suite matches (don't fail if server picks something else) */
            }
            pos += 4 + (int)hlen;
        }
    }

    /* --- Step 3: Certificate --- */
    uint8_t rsa_mod[256];
    uint32_t rsa_exp = 65537;
    int found_key = 0;
    /* May span multiple records until ServerHelloDone */
    int got_done = 0;
    while (!got_done) {
        if (recv_record(ctx, &rec_type, hs_data, (int)sizeof(hs_data), &hs_len_recv) != 0) {
            tls_log("no reply while waiting for Certificate/ServerHelloDone");
            goto fail;
        }
        if (tls_log_if_alert(rec_type, hs_data, hs_len_recv)) { rc = TLS_ERR_ALERT; goto fail; }
        if (rec_type != TLS_RT_HANDSHAKE) {
            tls_log("expected Certificate/ServerHelloDone, got a different record type");
            goto fail;
        }
        int pos = 0;
        while (pos < hs_len_recv) {
            uint8_t ht = hs_data[pos];
            uint32_t hlen = u24be(hs_data+pos+1);
            uint8_t *hbody = hs_data+pos+4;
            hs_append(ctx, hs_data+pos, 4+(uint32_t)hlen);
            if (ht == TLS_HT_CERTIFICATE && !found_key) {
                /* certificates_list: 3-byte total length, then each cert 3-byte length + DER */
                int cp = 3;
                while (cp < (int)hlen) {
                    uint32_t clen = u24be(hbody+cp); cp += 3;
                    if (!found_key && parse_rsa_pubkey(hbody+cp, (int)clen, rsa_mod, &rsa_exp))
                        found_key = 1;
                    cp += (int)clen;
                }
            } else if (ht == TLS_HT_SERVER_DONE) {
                got_done = 1;
            }
            pos += 4 + (int)hlen;
        }
    }
    if (!found_key) {
        tls_log("certificate received, but no RSA key found (server likely doesn't support plain RSA key exchange)");
        goto fail;
    }
    tls_log("certificate received, RSA key found");

    /* --- Step 4: ClientKeyExchange --- */
    uint8_t premaster[48];
    premaster[0] = TLS_VER_MAJOR; premaster[1] = TLS_VER_MINOR;
    prng_fill(premaster+2, 46);

    uint8_t enc_pm[256];
    if (pkcs1_encrypt(premaster, 48, rsa_mod, 256, rsa_exp, enc_pm, 256) != 0) {
        tls_log("RSA encrypt of premaster secret failed");
        goto fail;
    }

    uint8_t cke[258];
    cke[0] = 0x01; cke[1] = 0x00; /* length = 256 */
    memcpy(cke+2, enc_pm, 256);
    if (tls_send_hs(ctx, TLS_HT_CLIENT_KEY_EX, cke, 258) != 0) {
        tls_log("ClientKeyExchange send failed");
        goto fail;
    }
    tls_log("ClientKeyExchange sent");

    /* --- Step 5: Compute master secret --- */
    {
        uint8_t seed[64];
        memcpy(seed, ctx->client_rand, 32);
        memcpy(seed+32, ctx->server_rand, 32);
        tls_prf(premaster, 48, "master secret", 13, seed, 64, ctx->master, 48);
    }

    /* --- Step 6: Derive key material --- */
    {
        uint8_t seed[64];
        memcpy(seed, ctx->server_rand, 32);
        memcpy(seed+32, ctx->client_rand, 32);
        uint8_t km[128]; /* 2*32 (MAC) + 2*16 (key) + 2*16 (IV) = 128 bytes */
        tls_prf(ctx->master, 48, "key expansion", 13, seed, 64, km, 128);
        memcpy(ctx->client_mac,      km,    32);
        memcpy(ctx->server_mac,      km+32, 32);
        memcpy(ctx->client_write_key,km+64, 16);
        memcpy(ctx->server_write_key,km+80, 16);
        memcpy(ctx->client_write_iv, km+96, 16);
        memcpy(ctx->server_write_iv, km+112,16);
    }

    /* --- Step 7: ChangeCipherSpec --- */
    {
        uint8_t ccs = 1;
        if (tls_send_raw(ctx, TLS_RT_CHANGE_CIPHER, &ccs, 1) != 0) {
            tls_log("ChangeCipherSpec send failed");
            goto fail;
        }
    }

    /* --- Step 8: Finished --- */
    {
        /* verify_data = PRF(master, "client finished", SHA256(all_handshake_messages))[0..11] */
        uint8_t hs_hash[32];
        sha256_hash(ctx->hs_buf, ctx->hs_len, hs_hash);
        uint8_t verify[12];
        tls_prf(ctx->master, 48, "client finished", 15, hs_hash, 32, verify, 12);

        /* Finished message body */
        uint8_t fin_body[12];
        memcpy(fin_body, verify, 12);

        /* Build handshake message to add to transcript */
        uint8_t fin_hs[16];
        fin_hs[0] = TLS_HT_FINISHED;
        put_u24(fin_hs+1, 12);
        memcpy(fin_hs+4, fin_body, 12);
        hs_append(ctx, fin_hs, 16);

        /* Encrypt and send */
        uint8_t enc[512];
        int enc_len = 0;
        if (tls_encrypt_record(ctx, TLS_RT_HANDSHAKE, fin_hs, 16, enc, &enc_len) != 0) {
            tls_log("encrypting client Finished failed");
            goto fail;
        }
        if (tls_send_raw(ctx, TLS_RT_HANDSHAKE, enc, enc_len) != 0) {
            tls_log("client Finished send failed");
            goto fail;
        }
        tls_log("client Finished sent, waiting for server ChangeCipherSpec+Finished");
    }

    /* --- Step 9: Receive server ChangeCipherSpec + Finished --- */
    {
        /* ChangeCipherSpec */
        uint8_t ccs_data[8];
        int ccs_len;
        if (recv_record(ctx, &rec_type, ccs_data, (int)sizeof(ccs_data), &ccs_len) != 0) {
            tls_log("no reply after client Finished (connection closed or timed out)");
            goto fail;
        }
        if (tls_log_if_alert(rec_type, ccs_data, ccs_len)) { rc = TLS_ERR_ALERT; goto fail; }
        if (rec_type != TLS_RT_CHANGE_CIPHER) {
            tls_log("expected server ChangeCipherSpec, got a different record type");
            goto fail;
        }

        /* Encrypted Finished */
        uint8_t ef_data[512];
        int ef_len;
        if (recv_record(ctx, &rec_type, ef_data, (int)sizeof(ef_data), &ef_len) != 0) {
            tls_log("no reply after server ChangeCipherSpec (connection closed or timed out)");
            goto fail;
        }
        if (rec_type != TLS_RT_HANDSHAKE) {
            tls_log("expected encrypted server Finished, got a different record type");
            goto fail;
        }

        uint8_t fin_plain[256];
        int fin_plen = 0;
        if (tls_decrypt_record(ctx, TLS_RT_HANDSHAKE, ef_data, ef_len, fin_plain, &fin_plen) != 0) {
            tls_log("decrypting server Finished failed");
            goto fail;
        }

        /* Verify server Finished */
        if (fin_plen < 16) {
            tls_log("decrypted server Finished too short");
            goto fail;
        }
        uint8_t hs_hash[32];
        sha256_hash(ctx->hs_buf, ctx->hs_len, hs_hash);
        uint8_t expected[12];
        tls_prf(ctx->master, 48, "server finished", 15, hs_hash, 32, expected, 12);
        if (memcmp(fin_plain+4, expected, 12) != 0) {
            tls_log("server Finished verification failed (MAC/hash mismatch)");
            goto fail;
        }
    }

    tls_log("handshake complete");
    ctx->handshake_done = 1;
    return 0;
fail:
    tcp_close2(ctx->fd);
    ctx->fd = -1;
    return rc;
}

const char *tls_connect_strerror(int err)
{
    switch (err) {
        case TLS_ERR_ALERT:     return "server sent a fatal TLS alert (see dmesg for level/description)";
        case TLS_ERR_HANDSHAKE: return "TLS handshake failed (see dmesg for which step)";
        case TCP_ERR_NOSOCK:    return "no free TCP socket";
        case TCP_ERR_REFUSED:   return "connection refused (RST received)";
        case TCP_ERR_TIMEOUT:   return "connection timed out, no reply to SYN";
        case IP_ERR_NOMEM:      return "out of memory building packet";
        default:                 return arp_resolve_strerror(err); /* ARP_ERR_* -- couldn't even send the SYN */
    }
}

int tls_write(tls_ctx_t *ctx, const uint8_t *data, int len)
{
    uint8_t enc[TLS_TX_BUF + 256];
    int enc_len = 0;
    if (tls_encrypt_record(ctx, TLS_RT_DATA, data, len, enc, &enc_len) != 0) return -1;
    if (tls_send_raw(ctx, TLS_RT_DATA, enc, enc_len) != 0) return -1;
    return 0;
}

int tls_read(tls_ctx_t *ctx, uint8_t *buf, int max)
{
    /* Return from buffer first */
    if (ctx->rx_plain_pos < ctx->rx_plain_len) {
        int avail = (int)(ctx->rx_plain_len - ctx->rx_plain_pos);
        int n = avail < max ? avail : max;
        memcpy(buf, ctx->rx_plain + ctx->rx_plain_pos, (uint32_t)n);
        ctx->rx_plain_pos += (uint32_t)n;
        return n;
    }
    /* Receive a new record */
    ctx->rx_plain_len = 0; ctx->rx_plain_pos = 0;
    if (recv_encrypted_record(ctx) != 0) return 0;
    int avail = (int)ctx->rx_plain_len;
    int n = avail < max ? avail : max;
    memcpy(buf, ctx->rx_plain, (uint32_t)n);
    ctx->rx_plain_pos = (uint32_t)n;
    return n;
}

void tls_close(tls_ctx_t *ctx)
{
    if (ctx->fd >= 0) {
        /* Send close_notify alert */
        uint8_t alert[2] = {1, 0};
        uint8_t enc[256]; int enc_len = 0;
        tls_encrypt_record(ctx, TLS_RT_ALERT, alert, 2, enc, &enc_len);
        tls_send_raw(ctx, TLS_RT_ALERT, enc, enc_len);
        tcp_close2(ctx->fd);
        ctx->fd = -1;
    }
}

/* Suppress unused warning for u32be */
static uint32_t __attribute__((unused)) _u32 = 0;
static void __attribute__((unused)) _unused_u32be(void) { _u32 = u32be((uint8_t*)&_u32); }
static void __attribute__((unused)) _unused_put_u8(void) { uint8_t x; put_u8(&x,0); }
