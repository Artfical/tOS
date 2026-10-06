#include "ip6.h"
#include "net.h"
#include "nic.h"
#include "icmpv6.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

uint8_t net_ip6[16];  /* our link-local IPv6 address */

/* -----------------------------------------------------------------------
 * Generate link-local address from MAC (EUI-64 method, RFC 4291 §2.5.1)
 * fe80::/10 prefix + 54 zero bits + 64-bit EUI-64 modified from MAC
 * ----------------------------------------------------------------------- */
void ip6_init(const uint8_t *mac) {
    /* fe80::0 */
    memset(net_ip6, 0, 16);
    net_ip6[0]  = 0xFE;
    net_ip6[1]  = 0x80;
    /* EUI-64: insert FF:FE in the middle of the 6-byte MAC */
    net_ip6[8]  = mac[0] ^ 0x02;   /* flip Universal/Local bit */
    net_ip6[9]  = mac[1];
    net_ip6[10] = mac[2];
    net_ip6[11] = 0xFF;
    net_ip6[12] = 0xFE;
    net_ip6[13] = mac[3];
    net_ip6[14] = mac[4];
    net_ip6[15] = mac[5];
}

/* -----------------------------------------------------------------------
 * Format IPv6 address to string (full form, not abbreviated)
 * ----------------------------------------------------------------------- */
void ip6_fmt(const uint8_t *addr, char *buf) {
    /* Simple hex formatter: each group of 2 bytes as 4 hex digits */
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    for (int g = 0; g < 8; g++) {
        uint8_t hi = addr[g * 2];
        uint8_t lo = addr[g * 2 + 1];
        buf[pos++] = hex[(hi >> 4) & 0xF];
        buf[pos++] = hex[hi & 0xF];
        buf[pos++] = hex[(lo >> 4) & 0xF];
        buf[pos++] = hex[lo & 0xF];
        if (g < 7) buf[pos++] = ':';
    }
    buf[pos] = '\0';
}

/* -----------------------------------------------------------------------
 * Parse "xxxx:xxxx:...:xxxx" into 16-byte array (simplified, no ::)
 * ----------------------------------------------------------------------- */
int ip6_parse(const char *s, uint8_t *out) {
    uint16_t head[8], tail[8];
    int nh = 0, nt = 0, seen_gap = 0;
    memset(out, 0, 16);
    if (s[0] == ':' && s[1] != ':') return -1;          /* single leading colon */
    if (s[0] == ':' && s[1] == ':') { seen_gap = 1; s += 2; if (*s == 0) return 0; }
    for (;;) {
        uint32_t val = 0;
        int digits = 0;
        while (*s && *s != ':') {
            char c = *s++;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            if (++digits > 4) return -1;                 /* group wider than 16 bits */
            val = (val << 4) | (uint32_t)d;
        }
        if (digits == 0) return -1;
        if (seen_gap) { if (nt >= 8) return -1; tail[nt++] = (uint16_t)val; }
        else          { if (nh >= 8) return -1; head[nh++] = (uint16_t)val; }
        if (*s == 0) break;
        s++;                                             /* the ':' */
        if (*s == ':') {                                 /* "::" */
            if (seen_gap) return -1;                     /* only one allowed */
            seen_gap = 1;
            s++;
            if (*s == 0) break;                          /* trailing "::" */
        } else if (*s == 0) {
            return -1;                                   /* trailing single colon */
        }
    }
    if (seen_gap ? (nh + nt > 7) : (nh != 8)) return -1;
    for (int i = 0; i < nh; i++) { out[i * 2] = (uint8_t)(head[i] >> 8); out[i * 2 + 1] = (uint8_t)head[i]; }
    for (int i = 0; i < nt; i++) {
        int g = 8 - nt + i;
        out[g * 2] = (uint8_t)(tail[i] >> 8); out[g * 2 + 1] = (uint8_t)tail[i];
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Send an IPv6 packet
 * For link-local destinations, use NDP to resolve MAC.
 * For now: use the solicited-node multicast MAC for unknown targets.
 * ----------------------------------------------------------------------- */
int ip6_send(const uint8_t *dst_ip6, uint8_t next_header, void *data, int len) {
    /* No fragmentation on TX: keep within the IPv6 minimum MTU (1280 - 40).
     * A negative length used to wrap into a huge malloc()/memcpy(). */
    if (len < 0 || len > 1280 - (int)sizeof(ip6_hdr_t)) return -1;
    /* Resolve destination MAC */
    uint8_t dst_mac[6];
    /* Multicast destinations map straight to 33:33:<low 32 bits> and must NOT
     * go through NDP: the Neighbor Solicitation itself is sent to a multicast
     * address, so resolving it recursed (ip6_send -> ndp_resolve -> NS ->
     * ip6_send ...) until the kernel stack overflowed and the machine
     * panicked on the first ping6 to a correctly parsed address. */
    int resolved = (dst_ip6[0] == 0xFF) ? -1 : icmpv6_ndp_resolve(dst_ip6, dst_mac);
    if (resolved != 0) {
        /* Fall back to multicast MAC: 33:33:xx:xx:xx:xx */
        dst_mac[0] = 0x33; dst_mac[1] = 0x33;
        dst_mac[2] = dst_ip6[12]; dst_mac[3] = dst_ip6[13];
        dst_mac[4] = dst_ip6[14]; dst_mac[5] = dst_ip6[15];
    }

    int ip6_len = sizeof(ip6_hdr_t) + len;
    int total   = 14 + ip6_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    /* Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IPV6);

    /* IPv6 header */
    ip6_hdr_t *ip6 = (ip6_hdr_t *)(buf + 14);
    memset(ip6, 0, sizeof(ip6_hdr_t));
    ip6->ver_tc_fl   = htonl(0x60000000U);  /* version=6, TC=0, FL=0 */
    ip6->payload_len = htons((uint16_t)len);
    ip6->next_header = next_header;
    /* Neighbor Discovery messages (RS/RA/NS/NA/Redirect) must carry hop limit
     * 255 (RFC 4861 7.1) or conforming neighbours discard them. */
    ip6->hop_limit   = (next_header == 58 && len >= 1 &&
                        ((uint8_t *)data)[0] >= 133 && ((uint8_t *)data)[0] <= 137) ? 255 : 64;
    memcpy(ip6->src, net_ip6, 16);
    memcpy(ip6->dst, dst_ip6, 16);

    memcpy(buf + 14 + sizeof(ip6_hdr_t), data, len);
    nic_send(buf, total);
    free(buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * Dispatch incoming IPv6 packet
 * ----------------------------------------------------------------------- */
#define IP6_MAX_EXT_HEADERS 8   /* longest extension header chain accepted */

/* Is this destination ours: our address, all-nodes, or our solicited-node group? */
static int ip6_dst_is_ours(const uint8_t *dst)
{
    if (memcmp(dst, net_ip6, 16) == 0) return 1;
    static const uint8_t all_nodes[16] = { 0xFF, 0x02, 0,0,0,0,0,0,0,0,0,0,0,0,0, 0x01 };
    if (memcmp(dst, all_nodes, 16) == 0) return 1;
    uint8_t sn[16] = { 0xFF, 0x02, 0,0,0,0,0,0,0,0,0, 0x01, 0xFF, net_ip6[13], net_ip6[14], net_ip6[15] };
    return memcmp(dst, sn, 16) == 0;
}

/* Validates the TLV options of a hop-by-hop / destination options header
 * (RFC 8200 4.2). Only Pad1/PadN are understood; an unknown option is skipped
 * when its two high bits are 00 and makes us discard the packet otherwise. */
static int ip6_options_ok(const uint8_t *opts, int len)
{
    int i = 0;
    while (i < len) {
        uint8_t type = opts[i];
        if (type == 0) { i++; continue; }                    /* Pad1 */
        if (i + 1 >= len) return 0;
        int olen = opts[i + 1];
        if (i + 2 + olen > len) return 0;                    /* option runs past its header */
        if (type != 1 && (type >> 6) != 0) return 0;         /* unknown, 'discard' action */
        i += 2 + olen;
    }
    return 1;
}

void ip6_handle(uint8_t *data, int len) {
    if (len < (int)sizeof(ip6_hdr_t)) return;
    ip6_hdr_t *ip6 = (ip6_hdr_t *)data;

    /* Check version == 6 */
    if ((ntohl(ip6->ver_tc_fl) >> 28) != 6) return;

    /* A zero payload length means a jumbogram (not supported); it must also fit
     * what was actually received. */
    int payload_len = ntohs(ip6->payload_len);
    if (payload_len == 0 || payload_len > len - (int)sizeof(ip6_hdr_t)) return;

    if (!ip6_dst_is_ours(ip6->dst)) return;                  /* we are a host, not a router */
    if (ip6->src[0] == 0xFF) return;                         /* a multicast source is never valid */
    if (memcmp(ip6->src, net_ip6, 16) == 0) return;          /* our own packet echoed back */

    uint8_t *p = data + sizeof(ip6_hdr_t);
    int remain = payload_len;
    uint8_t nh = ip6->next_header;

    /* Walk the extension header chain, bounded in count and by the payload
     * length: the old code looked only at next_header, so a hop-by-hop header in
     * front of an ICMPv6 message made the whole packet invisible. */
    for (int n = 0; n <= IP6_MAX_EXT_HEADERS; n++) {
        switch (nh) {
        case 0:                                              /* hop-by-hop: only valid first */
        case 60: {                                           /* destination options */
            if (nh == 0 && n != 0) return;
            if (remain < 8) return;
            int hl = ((int)p[1] + 1) * 8;
            if (hl > remain) return;
            if (!ip6_options_ok(p + 2, hl - 2)) return;
            nh = p[0]; p += hl; remain -= hl;
            continue;
        }
        case 43: {                                           /* routing */
            if (remain < 8) return;
            int hl = ((int)p[1] + 1) * 8;
            if (hl > remain) return;
            if (p[3] != 0) return;                           /* segments left: not for us to process */
            nh = p[0]; p += hl; remain -= hl;
            continue;
        }
        case 58: /* IPPROTO_ICMPV6 */
            icmpv6_handle(ip6, p, remain);
            return;
        default:
            return;       /* fragments (no reassembly), AH/ESP, no-next-header, TCP/UDP: not handled over IPv6 */
        }
    }
    /* more extension headers than IP6_MAX_EXT_HEADERS: drop */
}
