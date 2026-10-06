/* Host test for kernel/net/sctp.c against a real Linux SCTP association (vectors/sctp_linux.hex). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
#include "nic.h"
void (*nic_send)(void *, int);
int (*nic_poll)(uint8_t *, int);
uint8_t net_mac[6]; uint32_t fake_now_ms; void arp_handle(uint8_t *d, int l) { (void)d; (void)l; }
uint32_t net_ip, net_gateway, net_dns, net_netmask;
uint32_t nic_tx_packets, nic_tx_bytes;
/* capture what sctp.c sends */
static uint8_t sent[8][2048]; static int sent_len[8]; static int nsent;
static int scripted_mode;
extern int init_sent_g, echo_sent_g, drop_first_init_g, peer_silent_g, pending_reply_g;
int init_sent_g, echo_sent_g, drop_first_init_g, peer_silent_g, pending_reply_g;
int ip_send(uint32_t dst, uint8_t proto, void *data, int len)
{
    (void)dst; (void)proto;
    if (scripted_mode && len > 12) {
        uint8_t type = ((uint8_t *)data)[12];
        if (type == 1) { init_sent_g++; if (!peer_silent_g && !(drop_first_init_g && init_sent_g == 1)) pending_reply_g = 1; }
        if (type == 10) { echo_sent_g++; if (!peer_silent_g) pending_reply_g = 2; }
    }
    if (nsent < 8 && len <= 2048) { memcpy(sent[nsent], data, len); sent_len[nsent] = len; nsent++; }
    return 0;
}
uint32_t csprng_u32(void) { static uint32_t x = 0x12345678; x = x * 1664525 + 1013904223; return x; }
#include "sctp.c"


/* ---- scripted peer for sctp_connect(): every nic_poll() takes 100 ms of fake time ---- */
#define pending_reply pending_reply_g
static uint8_t (*vec_ptr)[1500];
static int scripted_poll(uint8_t *buf, int max)
{
    (void)max;
    fake_now_ms += 100;
    if (!pending_reply) return 0;
    int idx = pending_reply == 1 ? 1 : 3;
    pending_reply = 0;
    memset(buf, 0, 1536);
    int l = 0; extern int pklen_g[];
    l = pklen_g[idx];
    memcpy(buf + 14, vec_ptr[idx], l);
    buf[12] = 0x08; buf[13] = 0x00;
    uint8_t *sc = buf + 14 + 20;                  /* patch ports / vtag / CRC to the live association */
    sc[2] = (uint8_t)(assoc.src_port >> 8); sc[3] = (uint8_t)assoc.src_port;
    uint32_t vt = htonl(assoc.local_vtag); memcpy(sc + 4, &vt, 4);
    ((sctp_hdr_t *)sc)->checksum = 0;
    sctp_put_crc((sctp_hdr_t *)sc, crc32c(sc, l - 20));
    return 14 + l + (l < 46 ? 46 - l : 0);       /* short packets arrive padded to the Ethernet minimum */
}

static uint8_t pk[11][1500]; static int pklen[11]; /* IP packets */
int pklen_g[11];
static int bad;
static void check(const char *label, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", label); }
static void feed(int i)   /* deliver packet i (IP header + SCTP) to the handler */
{
    ip_hdr_t *ip = (ip_hdr_t *)pk[i];
    sctp_handle(ip, pk[i] + 20, pklen[i] - 20);
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int main(void)
{
    FILE *f = fopen("vectors/sctp_linux.hex", "r");
    char *line = malloc(8192); int n = 0;
    while (n < 11 && fgets(line, 8192, f)) {
        if (line[0] == '#' || strlen(line) < 20) continue;
        int l = (int)strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        pklen[n] = l / 2;
        for (int i = 0; i < pklen[n]; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); pk[n][i] = (uint8_t)v; }
        n++;
    }
    /* the client's state as it was when the capture was made (white-box) */
    const uint8_t *init = pk[0] + 20;
    uint16_t cport = (uint16_t)((init[0] << 8) | init[1]);
    uint32_t cvtag = be32(init + 12 + 4);          /* INIT chunk: initiate tag */
    uint32_t ctsn  = be32(init + 12 + 4 + 8 + 4);   /* init TSN */
    memset(&assoc, 0, sizeof(assoc));
    assoc.dst_ip = *(uint32_t *)(pk[0] + 16); assoc.dst_port = 5001; assoc.src_port = cport;
    assoc.local_vtag = cvtag; assoc.local_tsn = ctsn; assoc.state = SCTP_STATE_COOKIE_WAIT; assoc_active = 1;

    /* 1. checksum: Linux puts the CRC32c on the wire little-endian. Every real packet of the capture must
     *    carry crc32c(packet with a zero checksum field) in that byte order, and so must our own INIT. */
    int crc_ok = 1;
    for (int i = 0; i < 11; i++) {
        uint8_t tmp[1500]; int l = pklen[i] - 20;
        memcpy(tmp, pk[i] + 20, l); memset(tmp + 8, 0, 4);
        uint32_t c = crc32c(tmp, l);
        uint8_t le[4] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24) };
        if (memcmp(pk[i] + 20 + 8, le, 4) != 0) crc_ok = 0;
    }
    check("crc32c() reproduces the checksum of all 11 real Linux packets (little-endian on the wire)", crc_ok);
    nsent = 0; send_init();
    {
        uint8_t tmp[2048]; memcpy(tmp, sent[0], sent_len[0]); memset(tmp + 8, 0, 4);
        uint32_t c = crc32c(tmp, sent_len[0]);
        uint8_t le[4] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24) };
        check("our INIT carries its CRC32c in the same (little-endian) byte order", nsent == 1 && memcmp(sent[0] + 8, le, 4) == 0);
    }

    /* 2. the real INIT-ACK is accepted and answered with a COOKIE-ECHO identical to Linux's */
    nsent = 0; feed(1);
    check("real INIT-ACK accepted (state COOKIE-ECHO)", assoc.state == SCTP_STATE_COOKIE_ECHO);
    int real_cookie = ((pk[2][20 + 14] << 8) | pk[2][20 + 15]) - 4;   /* the client echoed the whole cookie */
    printf("   (real cookie: %d bytes, the old buffer held 64)\n", real_cookie);
    check("the real cookie is kept whole", assoc.cookie_len == real_cookie);
    check("COOKIE-ECHO sent by sctp.c is byte-identical to Linux's", nsent >= 1 && sent_len[0] == pklen[2] - 20 && memcmp(sent[0], pk[2] + 20, sent_len[0]) == 0);

    /* 3. COOKIE-ACK -> established */
    feed(3);
    check("real COOKIE-ACK establishes the association", assoc.state == SCTP_STATE_ESTABLISHED);

    /* 4. our DATA equals Linux's DATA byte for byte */
    nsent = 0; sctp_send("hello-sctp", 10);
    check("DATA built by sctp.c is byte-identical to Linux's", nsent == 1 && sent_len[0] == pklen[4] - 20 && memcmp(sent[0], pk[4] + 20, sent_len[0]) == 0);

    /* 5. real DATA from the server is delivered once, in order, and SACKed */
    nsent = 0; feed(6);
    check("server DATA delivered", assoc.rx_len == 21 && memcmp(assoc.rx_buf, "world-from-linux-sctp", 21) == 0);
    check("SACK sent for it carries the right cumulative TSN", nsent >= 1 && be32(sent[0] + 12 + 4) == be32(pk[6] + 20 + 12 + 4));
    nsent = 0; feed(6);
    check("the same DATA again (duplicate) is not delivered twice", assoc.rx_len == 21);

    /* a chunk ahead of a gap, and flooding beyond the buffer limit, must not be delivered/stored */
    {
        uint8_t t2[1500]; memcpy(t2, pk[6], pklen[6]);
        uint32_t tsn = be32(t2 + 20 + 12 + 4);
        uint32_t future = tsn + 5;                                  /* skips TSNs: a gap */
        t2[20 + 16] = (uint8_t)(future >> 24); t2[20 + 17] = (uint8_t)(future >> 16); t2[20 + 18] = (uint8_t)(future >> 8); t2[20 + 19] = (uint8_t)future;
        ((sctp_hdr_t *)(t2 + 20))->checksum = 0;
        sctp_put_crc((sctp_hdr_t *)(t2 + 20), crc32c(t2 + 20, pklen[6] - 20));
        int before = assoc.rx_len;
        sctp_handle((ip_hdr_t *)t2, t2 + 20, pklen[6] - 20);
        check("DATA ahead of a gap is not delivered", assoc.rx_len == before);
        check("cumulative TSN did not jump over the gap", assoc.peer_cum_tsn == tsn);
        /* consecutive new TSNs until the buffer limit: must stop at SCTP_RX_MAX */
        uint32_t next = tsn + 1;
        for (int i = 0; i < 4000; i++, next++) {
            t2[20 + 16] = (uint8_t)(next >> 24); t2[20 + 17] = (uint8_t)(next >> 16); t2[20 + 18] = (uint8_t)(next >> 8); t2[20 + 19] = (uint8_t)next;
            ((sctp_hdr_t *)(t2 + 20))->checksum = 0;
            sctp_put_crc((sctp_hdr_t *)(t2 + 20), crc32c(t2 + 20, pklen[6] - 20));
            sctp_handle((ip_hdr_t *)t2, t2 + 20, pklen[6] - 20);
        }
        printf("   rx_len after flooding 4000 chunks: %d (limit %d)\n", assoc.rx_len, SCTP_RX_MAX);
        check("receive buffer never exceeds its limit", assoc.rx_len <= SCTP_RX_MAX);
    }

    /* 6. a forged packet with the wrong verification tag is ignored */
    uint8_t t[1500]; memcpy(t, pk[10], pklen[10]);              /* SHUTDOWN-COMPLETE */
    ((sctp_hdr_t *)(t + 20))->vtag ^= 0x01000000u;
    /* recompute the CRC so only the tag is wrong */
    ((sctp_hdr_t *)(t + 20))->checksum = 0;
    ((sctp_hdr_t *)(t + 20))->checksum = crc32c(t + 20, pklen[10] - 20);
    sctp_handle((ip_hdr_t *)t, t + 20, pklen[10] - 20);
    check("SHUTDOWN-COMPLETE with a wrong verification tag does not tear the association down", assoc_active == 1);
    t[20 + 12] = SCTP_CHUNK_ABORT;                                /* a forged ABORT, tag wrong, CRC right */
    ((sctp_hdr_t *)(t + 20))->checksum = 0;
    ((sctp_hdr_t *)(t + 20))->checksum = crc32c(t + 20, pklen[10] - 20);
    sctp_handle((ip_hdr_t *)t, t + 20, pklen[10] - 20);
    check("forged ABORT (wrong tag) is ignored", assoc_active == 1 && assoc.state == SCTP_STATE_ESTABLISHED);

    /* 7. Ethernet pads short frames to 60 bytes: a 16-byte COOKIE-ACK in a 36-byte IP packet arrives with 10
     *    extra zero bytes. The receive path must cut the frame at the IP total length before the CRC check. */
    {
        memset(&assoc, 0, sizeof(assoc));
        assoc.dst_ip = *(uint32_t *)(pk[0] + 16); assoc.dst_port = 5001; assoc.src_port = cport;
        assoc.local_vtag = cvtag; assoc.local_tsn = ctsn; assoc.state = SCTP_STATE_COOKIE_ECHO; assoc_active = 1;
        net_ip = *(uint32_t *)(pk[3] + 16);                               /* our address = the packet's destination */
        uint8_t frame[1600]; memset(frame, 0, sizeof(frame));
        memcpy(frame + 14, pk[3], pklen[3]);                              /* Ethernet header (zeros) + IP packet */
        frame[12] = 0x08; frame[13] = 0x00;
        int padded_len = 14 + 46;                                         /* minimum payload: 10 bytes of padding */
        sctp_rx_frame(frame, padded_len);
        check("COOKIE-ACK in a padded Ethernet frame establishes the association", assoc.state == SCTP_STATE_ESTABLISHED);
        /* a frame shorter than its IP header claims is dropped */
        assoc.state = SCTP_STATE_COOKIE_ECHO;
        sctp_rx_frame(frame, 14 + pklen[3] - 4);
        check("truncated frame is dropped", assoc.state == SCTP_STATE_COOKIE_ECHO);
        /* an IP packet for another host */
        memcpy(frame + 14, pk[3], pklen[3]); frame[14 + 19] ^= 0x7F;
        sctp_rx_frame(frame, padded_len);
        check("packet addressed to another host is ignored", assoc.state == SCTP_STATE_COOKIE_ECHO);
    }
    /* 8. sctp_connect() against a scripted peer: retransmission and wall-clock timeout */
    {
        for (int i = 0; i < 11; i++) pklen_g[i] = pklen[i];
        vec_ptr = pk;
        scripted_mode = 1;
        nic_poll = scripted_poll;
        net_ip = *(uint32_t *)(pk[1] + 16);
        assoc_active = 0;
        init_sent_g = echo_sent_g = 0; pending_reply = 0; peer_silent_g = 0; drop_first_init_g = 1; fake_now_ms = 1000;
        int rc = sctp_connect(*(uint32_t *)(pk[1] + 12), 5001);
        printf("   connect with a lost first INIT: rc=%d, INITs sent=%d, elapsed fake ms=%u\n", rc, init_sent_g, fake_now_ms - 1000);
        check("a lost INIT is retransmitted and the association still comes up", rc == 0 && init_sent_g == 2 && assoc.state == SCTP_STATE_ESTABLISHED);
        assoc_active = 0; sctp_close();
        init_sent_g = 0; peer_silent_g = 1; fake_now_ms = 1000;
        rc = sctp_connect(*(uint32_t *)(pk[1] + 12), 5001);
        printf("   connect to a silent peer: rc=%d, INITs sent=%d, elapsed fake ms=%u\n", rc, init_sent_g, fake_now_ms - 1000);
        check("a silent peer: gives up after about 5 s with INIT retransmissions", rc == -1 && fake_now_ms - 1000 >= 5000 && fake_now_ms - 1000 < 6000 && init_sent_g >= 4);
        /* recv from an established but silent association must time out instead of blocking forever */
        memset(&assoc, 0, sizeof(assoc)); assoc.state = SCTP_STATE_ESTABLISHED; assoc_active = 1; assoc.local_vtag = 7;
        fake_now_ms = 1000; uint8_t rb[64];
        rc = sctp_recv(rb, sizeof(rb));
        check("sctp_recv returns SCTP_ERR_TIMEOUT after ~10 s of silence", rc == SCTP_ERR_TIMEOUT && fake_now_ms - 1000 >= 10000 && fake_now_ms - 1000 < 10500);
    }
    printf("failures: %d\n", bad);
    return bad != 0;
}
