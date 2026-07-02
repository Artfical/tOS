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
    memset(out, 0, 16);
    int group = 0;
    uint32_t val = 0;
    int digits = 0;
    while (*s && group < 8) {
        char c = *s++;
        if (c == ':') {
            if (digits == 0 && *s == ':') {
                /* :: — fill remaining with zeros (simplified) */
                s++;
                break;
            }
            out[group * 2]     = (uint8_t)((val >> 8) & 0xFF);
            out[group * 2 + 1] = (uint8_t)(val & 0xFF);
            group++;
            val = 0; digits = 0;
        } else if (c >= '0' && c <= '9') {
            val = (val << 4) | (uint32_t)(c - '0'); digits++;
        } else if (c >= 'a' && c <= 'f') {
            val = (val << 4) | (uint32_t)(c - 'a' + 10); digits++;
        } else if (c >= 'A' && c <= 'F') {
            val = (val << 4) | (uint32_t)(c - 'A' + 10); digits++;
        } else {
            return -1;
        }
    }
    if (digits > 0 && group < 8) {
        out[group * 2]     = (uint8_t)((val >> 8) & 0xFF);
        out[group * 2 + 1] = (uint8_t)(val & 0xFF);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Send an IPv6 packet
 * For link-local destinations, use NDP to resolve MAC.
 * For now: use the solicited-node multicast MAC for unknown targets.
 * ----------------------------------------------------------------------- */
int ip6_send(const uint8_t *dst_ip6, uint8_t next_header, void *data, int len) {
    /* Resolve destination MAC */
    uint8_t dst_mac[6];
    int resolved = icmpv6_ndp_resolve(dst_ip6, dst_mac);
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
    ip6->hop_limit   = 64;
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
void ip6_handle(uint8_t *data, int len) {
    if (len < (int)sizeof(ip6_hdr_t)) return;
    ip6_hdr_t *ip6 = (ip6_hdr_t *)data;

    /* Check version == 6 */
    if ((ntohl(ip6->ver_tc_fl) >> 28) != 6) return;

    int payload_len = ntohs(ip6->payload_len);
    if (payload_len > len - (int)sizeof(ip6_hdr_t)) return;

    void *payload = data + sizeof(ip6_hdr_t);

    switch (ip6->next_header) {
        case 58: /* IPPROTO_ICMPV6 */
            icmpv6_handle(ip6, payload, payload_len);
            break;
        default:
            break;
    }
}
