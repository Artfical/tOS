#include "ipsec.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

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
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (!ipsec_sa_table[i].valid) {
            ipsec_sa_table[i].valid    = 1;
            ipsec_sa_table[i].peer_ip  = peer_ip;
            ipsec_sa_table[i].spi      = spi;
            ipsec_sa_table[i].protocol = proto;
            ipsec_sa_table[i].seq      = 0;
            return 0;
        }
    }
    return -1;  /* table full */
}

void ipsec_sa_remove(uint32_t spi) {
    for (int i = 0; i < IPSEC_SA_MAX; i++) {
        if (ipsec_sa_table[i].valid && ipsec_sa_table[i].spi == spi) {
            ipsec_sa_table[i].valid = 0;
            return;
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

    if (ah_size < (int)sizeof(ipsec_ah_hdr_t) || ah_size > len) {
        terminal_writestring("[IPsec AH] malformed header\n");
        return;
    }

    /* Locate (or record) the SA */
    ipsec_sa_t *sa = sa_lookup(spi, outer_ip->src_ip);
    if (!sa) {
        /* Auto-learn inbound SA (no key material — just track it) */
        ipsec_sa_add(outer_ip->src_ip, spi, IPPROTO_AH);
        sa = sa_lookup(spi, outer_ip->src_ip);
    }

    if (sa) {
        /* Anti-replay: accept if seq is newer */
        if (seq <= sa->seq && sa->seq > 0) {
            terminal_writestring("[IPsec AH] replay attack detected\n");
            return;
        }
        sa->seq = seq;
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

    ipsec_sa_t *sa = sa_lookup(spi, ip->src_ip);
    if (!sa) {
        ipsec_sa_add(ip->src_ip, spi, IPPROTO_ESP);
        sa = sa_lookup(spi, ip->src_ip);
    }

    if (sa) {
        if (seq <= sa->seq && sa->seq > 0) {
            terminal_writestring("[IPsec ESP] replay detected\n");
            return;
        }
        sa->seq = seq;
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
