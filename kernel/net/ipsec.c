#include "ipsec.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "sha256.h"

/* -----------------------------------------------------------------------
 * Security Association table
 * ----------------------------------------------------------------------- */
ipsec_sa_t ipsec_sa_table[IPSEC_SA_MAX];

static void print_hex8(uint8_t v) {
    static const char h[] = "0123456789abcdef";
    char s[3]; s[0] = h[v >> 4]; s[1] = h[v & 0xF]; s[2] = '\0';
    terminal_writestring(s);
}

static void print_u32(uint32_t v) {
    char buf[12]; int i = 10; buf[11] = '\0';
    if (v == 0) { terminal_writestring("0"); return; }
    while (v && i >= 0) { buf[i--] = '0' + (char)(v % 10); v /= 10; }
    terminal_writestring(buf + i + 1);
}

/* -----------------------------------------------------------------------
 * Public API — add/remove SA entries
 * ----------------------------------------------------------------------- */
int ipsec_sa_add(uint32_t peer_ip, uint32_t spi, uint8_t proto) {
    for (int i = 0; i < IPSEC_SA_MAX; i++)                 /* one SA per (peer, SPI) */
        if (ipsec_sa_table[i].valid && ipsec_sa_table[i].spi == spi && ipsec_sa_table[i].peer_ip == peer_ip)
            return -1;
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (!ipsec_sa_table[i].valid) {
            memset(&ipsec_sa_table[i], 0, sizeof(ipsec_sa_table[i]));
            ipsec_sa_table[i].valid    = 1;
            ipsec_sa_table[i].peer_ip  = peer_ip;
            ipsec_sa_table[i].spi      = spi;
            ipsec_sa_table[i].protocol = proto;
            return 0;
        }
    }
    return -1;  /* table full */
}

int ipsec_sa_set_key(uint32_t peer_ip, uint32_t spi, const uint8_t *key, int key_len) {
    if (!key || key_len < 1 || key_len > IPSEC_KEY_MAX) return -1;
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        ipsec_sa_t *sa = &ipsec_sa_table[i];
        if (sa->valid && sa->spi == spi && sa->peer_ip == peer_ip) {
            memset(sa->key, 0, sizeof(sa->key));
            memcpy(sa->key, key, (size_t)key_len);
            sa->key_len = key_len;
            return 0;
        }
    }
    return -1;
}

void ipsec_sa_remove(uint32_t spi) {
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (ipsec_sa_table[i].valid && ipsec_sa_table[i].spi == spi) {
            volatile uint8_t *k = ipsec_sa_table[i].key;          /* wipe the key */
            for (int j = 0; j < IPSEC_KEY_MAX; j++) k[j] = 0;
            ipsec_sa_table[i].valid = 0;        /* keep going: the same SPI can exist for several peers */
        }
    }
}

/* -----------------------------------------------------------------------
 * Lookup SA by SPI and peer IP
 * ----------------------------------------------------------------------- */
static ipsec_sa_t *sa_lookup(uint32_t spi, uint32_t peer_ip) {
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (ipsec_sa_table[i].valid &&
            ipsec_sa_table[i].spi == spi &&
            ipsec_sa_table[i].peer_ip == peer_ip)
            return &ipsec_sa_table[i];
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * AH handler (RFC 4302)
 * Parse the AH header, validate the SA, then pass the inner payload
 * to ip_handle as if AH was never there.
 * -----------------------------------------------------------------------
 * AH header layout:
 *   next_header (1) | payload_len (1) | reserved (2) | spi (4) | seq (4) | ICV (variable)
 * Total AH header size = (payload_len + 2) * 4 bytes
 * ----------------------------------------------------------------------- */
void ipsec_ah_handle(ip_hdr_t *outer_ip, void *pkt, int len) {
    if (len < (int)sizeof(ipsec_ah_hdr_t)) return;
    ipsec_ah_hdr_t *ah = (ipsec_ah_hdr_t *)pkt;

    uint32_t spi     = ntohl(ah->spi);
    uint32_t seq     = ntohl(ah->seq_num);
    uint8_t  next_hdr = ah->next_header;
    int      ah_size = ((int)ah->payload_len + 2) * 4;

    /* The ICV is HMAC-SHA-256-128: 12 header bytes + 16 ICV bytes, optionally
     * followed by padding up to a multiple of 8 (Linux pads to 32 bytes). */
    if (ah_size < (int)sizeof(ipsec_ah_hdr_t) + IPSEC_AH_ICV_LEN || ah_size > len) {
        terminal_writestring("[IPsec AH] malformed header\n");
        return;
    }
    /* The ICV covers the IP header, so we need all of it: IP options are not supported. */
    if ((outer_ip->ver_ihl & 0x0F) != 5) return;

    /* Only a Security Association that was configured on purpose may be used.
     * Learning one from the first packet that mentions an unknown SPI let any
     * host fill the (8 entry) table with junk, and then use the fake SA's
     * sequence state to make real traffic look like a replay. */
    ipsec_sa_t *sa = sa_lookup(spi, outer_ip->src_ip);
    if (!sa || sa->protocol != IPPROTO_AH) {
        terminal_writestring("[IPsec AH] no SA for this SPI/peer, dropped\n");
        return;
    }
    if (sa->key_len == 0) {
        terminal_writestring("[IPsec AH] SA has no key: cannot authenticate, dropped\n");
        return;
    }

    /* Authenticate (RFC 4302 3.3.3): HMAC over the IP header with its mutable
     * fields (TOS, flags/fragment offset, TTL, checksum) zeroed, then the AH
     * header with the ICV field zeroed (padding included as received), then the
     * payload. Nothing is remembered, and nothing is delivered, before this
     * passes. Vector-checked against the Linux kernel's AH implementation. */
    int total = (int)sizeof(ip_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return;
    memcpy(buf, outer_ip, sizeof(ip_hdr_t));
    buf[1] = 0; buf[6] = 0; buf[7] = 0; buf[8] = 0; buf[10] = 0; buf[11] = 0;
    memcpy(buf + sizeof(ip_hdr_t), pkt, (size_t)len);
    memset(buf + sizeof(ip_hdr_t) + sizeof(ipsec_ah_hdr_t), 0, IPSEC_AH_ICV_LEN);
    uint8_t mac[32];
    hmac_sha256(sa->key, (uint32_t)sa->key_len, buf, (uint32_t)total, mac);
    free(buf);
    uint8_t diff = 0;
    for (int i = 0; i < IPSEC_AH_ICV_LEN; i++)
        diff |= (uint8_t)(mac[i] ^ ((uint8_t *)pkt)[sizeof(ipsec_ah_hdr_t) + i]);
    if (diff != 0) {
        terminal_writestring("[IPsec AH] bad ICV, dropped\n");
        return;
    }

    /* Anti-replay (RFC 4302 3.4.3): a 64-packet sliding window, consulted and
     * advanced only for authenticated packets. The old check required a strictly
     * increasing number (so mere reordering dropped good packets) and it was
     * updated by whatever arrived. */
    if (seq == 0) return;
    if (seq > sa->seq) {
        uint32_t shift = seq - sa->seq;
        sa->replay_window = shift >= 64 ? 0 : (sa->replay_window << shift);
        sa->replay_window |= 1;
        sa->seq = seq;
    } else {
        uint32_t diff_seq = sa->seq - seq;
        if (diff_seq >= 64 || (sa->replay_window & ((uint64_t)1 << diff_seq))) {
            terminal_writestring("[IPsec AH] replay attack detected\n");
            return;
        }
        sa->replay_window |= (uint64_t)1 << diff_seq;
    }

    /* Pass inner payload to IP stack */
    uint8_t *inner  = (uint8_t *)pkt + ah_size;
    int inner_len   = len - ah_size;

    if (inner_len > 0) {
        /* Reconstruct a synthetic IP header for the inner payload */
        int fake_total = sizeof(ip_hdr_t) + inner_len;
        uint8_t *fake  = (uint8_t *)malloc(fake_total);
        if (!fake) return;
        memcpy(fake, outer_ip, sizeof(ip_hdr_t));
        ip_hdr_t *fip  = (ip_hdr_t *)fake;
        fip->protocol  = next_hdr;
        fip->total_len = htons((uint16_t)fake_total);
        fip->checksum  = 0;
        memcpy(fake + sizeof(ip_hdr_t), inner, inner_len);
        /* Recalculate IP checksum */
        uint32_t sum = 0;
        uint16_t *wp = (uint16_t *)fake;
        for (int i = 0; i < 10; i++) sum += ntohs(wp[i]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        fip->checksum = htons((uint16_t)(~sum & 0xFFFF));

        extern void ip_handle(uint8_t *, int);
        ip_handle(fake, fake_total);
        free(fake);
    }
}

/* -----------------------------------------------------------------------
 * ESP handler (RFC 4303)
 * Parse the ESP header and log the SA.  Decryption is algorithm-specific
 * and requires key material from IKE — we parse and reject here.
 * -----------------------------------------------------------------------
 * ESP layout:
 *   spi (4) | seq (4) | IV + encrypted(payload+pad+pad_len+next_hdr) | ICV
 * Without the SA's crypto parameters we cannot decrypt.
 * ----------------------------------------------------------------------- */
void ipsec_esp_handle(ip_hdr_t *ip, void *pkt, int len) {
    if (len < (int)sizeof(ipsec_esp_hdr_t)) return;
    ipsec_esp_hdr_t *esp = (ipsec_esp_hdr_t *)pkt;

    uint32_t spi = ntohl(esp->spi);
    uint32_t seq = ntohl(esp->seq_num);

    /* No auto-learning (see ipsec_ah_handle()), and no sequence state is
     * touched: the packet cannot be authenticated here (no ESP crypto), so
     * nothing it says may change what we remember. */
    ipsec_sa_t *sa = sa_lookup(spi, ip->src_ip);
    if (!sa || sa->protocol != IPPROTO_ESP) {
        terminal_writestring("[IPsec ESP] no SA for this SPI/peer, dropped\n");
        return;
    }

    terminal_writestring("[IPsec ESP] encrypted payload SPI=0x");
    print_hex8((spi >> 24) & 0xFF); print_hex8((spi >> 16) & 0xFF);
    print_hex8((spi >>  8) & 0xFF); print_hex8(spi & 0xFF);
    terminal_writestring(" SEQ=");
    print_u32(seq);
    terminal_writestring(" len=");
    print_u32((uint32_t)(len - 8));
    terminal_writestring(" (no key material — cannot decrypt)\n");
}

/* -----------------------------------------------------------------------
 * Diagnostic — dump SA table
 * ----------------------------------------------------------------------- */
void ipsec_dump_sa(void) {
    terminal_writestring("IPsec SA table:\n");
    int any = 0;
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (!ipsec_sa_table[i].valid) continue;
        any = 1;
        terminal_writestring("  [");
        print_u32((uint32_t)i);
        terminal_writestring("] peer=");
        uint32_t ip = ipsec_sa_table[i].peer_ip;
        print_u32(ip & 0xFF);        terminal_writestring(".");
        print_u32((ip >> 8) & 0xFF); terminal_writestring(".");
        print_u32((ip >>16) & 0xFF); terminal_writestring(".");
        print_u32((ip >>24) & 0xFF);
        terminal_writestring(" spi=0x");
        uint32_t spi = ipsec_sa_table[i].spi;
        print_hex8((spi>>24)&0xFF); print_hex8((spi>>16)&0xFF);
        print_hex8((spi>> 8)&0xFF); print_hex8(spi&0xFF);
        terminal_writestring(" proto=");
        terminal_writestring(ipsec_sa_table[i].protocol == IPPROTO_AH ? "AH" : "ESP");
        terminal_writestring(" seq=");
        print_u32(ipsec_sa_table[i].seq);
        terminal_writestring("\n");
    }
    if (!any) terminal_writestring("  (empty)\n");
}
