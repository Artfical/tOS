#include "icmpv6.h"
#include "ip6.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "debugmon.h"

/* -----------------------------------------------------------------------
 * NDP (Neighbor Discovery Protocol) cache — IPv6 equivalent of ARP cache
 * ----------------------------------------------------------------------- */
#define NDP_CACHE_SIZE 8
typedef struct {
    uint8_t  ip6[16];
    uint8_t  mac[6];
    int      valid;
    uint32_t stamp;
} ndp_entry_t;

#define NDP_TTL_MS 120000
/* target of the Neighbor Solicitation we are waiting on; only an advertisement
 * for it may create or change a cache entry. */
static uint8_t ndp_pending[16];
static int ndp_pending_valid;
static ndp_entry_t ndp_cache[NDP_CACHE_SIZE];

static int ndp_fresh(const ndp_entry_t *e) {
    return e->valid && (uint32_t)(debugmon_uptime_ms() - e->stamp) < NDP_TTL_MS;
}

static void ndp_cache_store(const uint8_t *ip6, const uint8_t *mac) {
    uint32_t now = debugmon_uptime_ms();
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid && memcmp(ndp_cache[i].ip6, ip6, 16) == 0) {
            memcpy(ndp_cache[i].mac, mac, 6);
            ndp_cache[i].stamp = now;
            return;
        }
    }
    /* free or expired slot, else evict the oldest (not always slot 0) */
    int victim = 0;
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (!ndp_fresh(&ndp_cache[i])) { victim = i; break; }
        if ((uint32_t)(now - ndp_cache[i].stamp) > (uint32_t)(now - ndp_cache[victim].stamp)) victim = i;
    }
    memcpy(ndp_cache[victim].ip6, ip6, 16);
    memcpy(ndp_cache[victim].mac, mac, 6);
    ndp_cache[victim].valid = 1;
    ndp_cache[victim].stamp = now;
}

static int ndp_cache_lookup(const uint8_t *ip6, uint8_t *mac_out) {
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_fresh(&ndp_cache[i]) && memcmp(ndp_cache[i].ip6, ip6, 16) == 0) {
            memcpy(mac_out, ndp_cache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * ICMPv6 checksum (RFC 4443 §2.3)
 * Uses an IPv6 pseudo-header: src(16) + dst(16) + length(4) + zero(3) + next(1)
 * ----------------------------------------------------------------------- */
static uint16_t icmpv6_checksum(const uint8_t *src6, const uint8_t *dst6,
                                 const uint8_t *seg, int seg_len) {
    uint32_t sum = 0;

    /* Pseudo-header */
    const uint16_t *p = (const uint16_t *)src6;
    for (int i = 0; i < 8; i++) sum += ntohs(p[i]);
    p = (const uint16_t *)dst6;
    for (int i = 0; i < 8; i++) sum += ntohs(p[i]);
    sum += (uint32_t)seg_len;       /* upper-layer packet length */
    sum += 58;                      /* next header = ICMPv6 */

    /* ICMPv6 message */
    p = (const uint16_t *)seg;
    for (int i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    if (seg_len & 1) sum += (uint16_t)seg[seg_len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

/* -----------------------------------------------------------------------
 * Build and send a Neighbor Solicitation (NDP request)
 * ----------------------------------------------------------------------- */
static void send_neighbor_solicitation(const uint8_t *target_ip6) {
    /* Solicited-node multicast: ff02::1:ffxx:xxxx */
    uint8_t sol_node[16] = {
        0xFF,0x02,0,0, 0,0,0,0, 0,0,0,1,
        0xFF, target_ip6[13], target_ip6[14], target_ip6[15]
    };

    /* ICMPv6 NS payload: reserved(4) + target(16) + option(8) */
    int payload_len = 4 + 16 + 8;
    uint8_t *msg = (uint8_t *)malloc(payload_len);
    if (!msg) return;

    memset(msg, 0, 4);           /* reserved */
    memcpy(msg + 4, target_ip6, 16);
    /* Source Link-Layer Address option: type=1, len=1 (8 bytes) */
    msg[20] = 1;   /* type */
    msg[21] = 1;   /* length: 1 × 8 = 8 bytes */
    memcpy(msg + 22, net_mac, 6);

    /* ICMPv6 header + body */
    int icmp_len = 4 + payload_len;
    uint8_t *pkt = (uint8_t *)malloc(icmp_len);
    if (!pkt) { free(msg); return; }

    pkt[0] = ICMPV6_NEIGH_SOLICIT;
    pkt[1] = 0;
    *(uint16_t *)(pkt + 2) = 0;   /* checksum placeholder */
    memcpy(pkt + 4, msg, payload_len);
    free(msg);

    *(uint16_t *)(pkt + 2) = icmpv6_checksum(net_ip6, sol_node, pkt, icmp_len);
    ip6_send(sol_node, 58 /* ICMPv6 */, pkt, icmp_len);
    free(pkt);
}

/* -----------------------------------------------------------------------
 * Send Echo Request
 * ----------------------------------------------------------------------- */
static int send_echo_request(const uint8_t *dst_ip6, uint16_t id, uint16_t seq) {
    int icmp_len = 4 + 4 + 8;  /* icmpv6_hdr + echo header + 8-byte payload */
    uint8_t *pkt = (uint8_t *)malloc(icmp_len);
    if (!pkt) return -1;

    pkt[0] = ICMPV6_ECHO_REQUEST;
    pkt[1] = 0;
    *(uint16_t *)(pkt + 2) = 0;
    *(uint16_t *)(pkt + 4) = htons(id);
    *(uint16_t *)(pkt + 6) = htons(seq);
    memcpy(pkt + 8, "tOSping\0", 8);

    *(uint16_t *)(pkt + 2) = icmpv6_checksum(net_ip6, dst_ip6, pkt, icmp_len);
    int r = ip6_send(dst_ip6, 58, pkt, icmp_len);
    free(pkt);
    return r;
}

/* -----------------------------------------------------------------------
 * Public API — send Echo Reply (used internally by icmpv6_handle)
 * ----------------------------------------------------------------------- */
static void send_echo_reply(ip6_hdr_t *req_ip6, const uint8_t *req_body, int body_len) {
    uint8_t *pkt = (uint8_t *)malloc(4 + body_len);
    if (!pkt) return;
    pkt[0] = ICMPV6_ECHO_REPLY;
    pkt[1] = 0;
    *(uint16_t *)(pkt + 2) = 0;
    memcpy(pkt + 4, req_body, body_len);
    *(uint16_t *)(pkt + 2) = icmpv6_checksum(net_ip6, req_ip6->src, pkt, 4 + body_len);
    ip6_send(req_ip6->src, 58, pkt, 4 + body_len);
    free(pkt);
}

/* -----------------------------------------------------------------------
 * Public API — NDP resolve (like arp_resolve for IPv6)
 * ----------------------------------------------------------------------- */
int icmpv6_ndp_resolve(const uint8_t *dst_ip6, uint8_t *mac_out) {
    if (ndp_cache_lookup(dst_ip6, mac_out) == 0) return 0;

    memcpy(ndp_pending, dst_ip6, 16);
    ndp_pending_valid = 1;
    send_neighbor_solicitation(dst_ip6);

    for (int retry = 0; retry < 200; retry++) {
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IPV6)
                ip6_handle(pkt + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
        if (ndp_cache_lookup(dst_ip6, mac_out) == 0) { ndp_pending_valid = 0; return 0; }
    }
    ndp_pending_valid = 0;
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API — ICMPv6 ping6 (blocking)
 * ----------------------------------------------------------------------- */
int icmpv6_ping6(const uint8_t *dst_ip6) {
    static uint16_t ping_id = 0xA5A5;
    uint16_t seq = 1;

    if (send_echo_request(dst_ip6, ping_id, seq) != 0) return -1;

    for (int retry = 0; retry < 400; retry++) {
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IPV6) {
                uint8_t *ip6_data = pkt + sizeof(eth_hdr_t);
                int ip6_len       = len - sizeof(eth_hdr_t);
                if (ip6_len < (int)sizeof(ip6_hdr_t)) continue;
                ip6_hdr_t *ip6   = (ip6_hdr_t *)ip6_data;
                if (ip6->next_header == 58) {
                    uint8_t *icmp    = ip6_data + sizeof(ip6_hdr_t);
                    int icmp_len_rem = ip6_len - sizeof(ip6_hdr_t);
                    if (icmp_len_rem >= 8 && icmp[0] == ICMPV6_ECHO_REPLY) {
                        uint16_t rid = ntohs(*(uint16_t *)(icmp + 4));
                        uint16_t rseq = ntohs(*(uint16_t *)(icmp + 6));
                        if (rid == ping_id && rseq == seq) return 0;
                    }
                }
            }
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Incoming handler (called from ip6_handle)
 * ----------------------------------------------------------------------- */
void icmpv6_handle(ip6_hdr_t *ip6, void *pkt, int len) {
    if (len < 4) return;
    uint8_t *msg = (uint8_t *)pkt;
    uint8_t  type = msg[0];

    /* The ICMPv6 checksum is mandatory (RFC 4443 2.3) and was never checked,
     * so corrupted or hand-forged messages were acted on. */
    {
        uint16_t got = *(uint16_t *)(msg + 2);
        uint8_t *copy = (uint8_t *)malloc((size_t)len);
        if (!copy) return;
        memcpy(copy, msg, (size_t)len);
        *(uint16_t *)(copy + 2) = 0;
        uint16_t want = icmpv6_checksum(ip6->src, ip6->dst, copy, len);
        free(copy);
        if (want != got) return;
    }
    /* Neighbor Discovery is only valid with hop limit 255 (RFC 4861 7.1):
     * anything lower has been routed, i.e. it did not come from the link. */
    if ((type == ICMPV6_NEIGH_SOLICIT || type == ICMPV6_NEIGH_ADV ||
         type == ICMPV6_ROUTER_SOLICIT || type == ICMPV6_ROUTER_ADV) && ip6->hop_limit != 255)
        return;

    switch (type) {
    case ICMPV6_ECHO_REQUEST:
        if (len >= 8)
            send_echo_reply(ip6, msg + 4, len - 4);
        break;

    case ICMPV6_ECHO_REPLY:
        /* handled in ping polling loop */
        break;

    case ICMPV6_NEIGH_SOLICIT:
        if (len >= 4 + 16) {
            uint8_t *target = msg + 4;
            if (memcmp(target, net_ip6, 16) == 0) {
                /* Send Neighbor Advertisement */
                int na_len = 4 + 16 + 8;
                uint8_t *na = (uint8_t *)malloc(4 + na_len);
                if (!na) break;
                na[0] = ICMPV6_NEIGH_ADV;
                na[1] = 0;
                *(uint16_t *)(na + 2) = 0;
                /* Flags: Solicited(S)=1, Override(O)=1 */
                *(uint32_t *)(na + 4) = htonl(0x60000000U);
                memcpy(na + 8, net_ip6, 16);
                /* Target Link-Layer Address option */
                na[24] = 2;   /* type: target */
                na[25] = 1;   /* len: 8 bytes */
                memcpy(na + 26, net_mac, 6);
                *(uint16_t *)(na + 2) = icmpv6_checksum(net_ip6, ip6->src, na, 4 + na_len);
                ip6_send(ip6->src, 58, na, 4 + na_len);
                free(na);
            }
        }
        break;

    case ICMPV6_NEIGH_ADV:
        if (len >= 4 + 16) {
            uint8_t *target = msg + 4;
            /* Only an answer to our own outstanding solicitation is accepted;
             * unsolicited advertisements used to overwrite live entries. */
            if (!ndp_pending_valid || memcmp(target, ndp_pending, 16) != 0) break;
            /* Look for Target Link-Layer Address option (type 2) */
            uint8_t *opt = msg + 4 + 16;
            int optrem   = len - 4 - 16;
            while (optrem >= 8) {
                if (opt[0] == 2 && opt[1] == 1) {
                    ndp_cache_store(target, opt + 2);
                    break;
                }
                int step = opt[1] * 8;
                if (step <= 0) break;
                opt    += step;
                optrem -= step;
            }
        }
        break;

    case ICMPV6_ROUTER_SOLICIT:
    case ICMPV6_ROUTER_ADV:
        /* Not handled — we don't implement SLAAC */
        break;

    default:
        break;
    }
}
