#include "dhcp.h"
#include "net.h"
#include "nic.h"
#include "ip.h"
#include "udp.h"
#include "string.h"
#include "scheduler.h"
#include "debugmon.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC        0x63825363U

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} __attribute__((packed)) dhcp_hdr_t;

static uint16_t ip_checksum(const uint8_t *buf, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len - 1; i += 2)
        sum += ((uint16_t)buf[i] << 8) | buf[i + 1];
    if (len & 1) sum += (uint16_t)buf[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

/* Scans a DHCP options blob for `tag`; returns its length and copies up
 * to max_out bytes into out, or -1 if not present/truncated. Options
 * come straight off the wire from whatever answered the broadcast, so
 * every step is bounds-checked against the packet's actual length. */
static int dhcp_find_option(uint8_t *opts, int len, uint8_t tag, uint8_t *out, int max_out)
{
    int i = 0;
    while (i < len) {
        uint8_t t = opts[i];
        if (t == 255) break;
        if (t == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (i + 2 + l > len) break;
        if (t == tag) {
            int n = l < max_out ? l : max_out;
            memcpy(out, &opts[i + 2], n);
            return l;
        }
        i += 2 + l;
    }
    return -1;
}

/* Builds and sends a DISCOVER or REQUEST directly over nic_send(),
 * bypassing ip_send()/udp_send() entirely: this runs before net_ip is
 * known good, so the packet needs src_ip=0.0.0.0 and a broadcast
 * destination MAC that arp_resolve() has no reason to ever produce. */
static void dhcp_send(uint8_t msg_type, uint32_t xid, uint32_t requested_ip, uint32_t server_id)
{
    uint8_t pkt[14 + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t) + 32];
    memset(pkt, 0, sizeof(pkt));

    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(pkt + 14);
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    dhcp_hdr_t *dhcp = (dhcp_hdr_t *)((uint8_t *)udp + sizeof(udp_hdr_t));
    uint8_t *opt = (uint8_t *)dhcp + sizeof(dhcp_hdr_t);
    int oi = 0;

    dhcp->op = 1; dhcp->htype = 1; dhcp->hlen = 6; dhcp->hops = 0;
    dhcp->xid = xid;
    dhcp->flags = htons(0x8000); /* ask for a broadcast reply -- we have no IP to receive a unicast one at */
    memcpy(dhcp->chaddr, net_mac, 6);
    dhcp->magic = htonl(DHCP_MAGIC);

    opt[oi++] = 53; opt[oi++] = 1; opt[oi++] = msg_type;
    if (msg_type == DHCPREQUEST) {
        opt[oi++] = 50; opt[oi++] = 4; memcpy(&opt[oi], &requested_ip, 4); oi += 4;
        opt[oi++] = 54; opt[oi++] = 4; memcpy(&opt[oi], &server_id, 4); oi += 4;
    }
    opt[oi++] = 55; opt[oi++] = 3; opt[oi++] = 1; opt[oi++] = 3; opt[oi++] = 6;
    opt[oi++] = 255;

    int dhcp_len = (int)sizeof(dhcp_hdr_t) + oi;
    int udp_len = (int)sizeof(udp_hdr_t) + dhcp_len;
    int ip_total = (int)sizeof(ip_hdr_t) + udp_len;

    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length = htons((uint16_t)udp_len);
    udp->checksum = 0; /* optional for IPv4/UDP; left unset like the rest of this stack's fixed-size sends */

    ip->ver_ihl = 0x45;
    ip->total_len = htons((uint16_t)ip_total);
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->src_ip = 0;
    ip->dst_ip = 0xFFFFFFFFU;
    ip->checksum = 0;
    ip->checksum = ip_checksum((uint8_t *)ip, sizeof(ip_hdr_t));

    int total = 14 + ip_total;
    nic_tx_packets++;
    nic_tx_bytes += (uint32_t)total;
    nic_send(pkt, total);
}

/* Polls for the next DHCP reply matching `xid`, up to `deadline_ms`.
 * Returns the DHCP message type (option 53) found, or 0 on timeout.
 * Non-DHCP traffic seen while waiting is silently dropped -- this runs
 * before net_ip is finalized, so routing it through ip_handle()'s
 * normal dispatch (which filters on dst_ip == net_ip) would drop the
 * very reply being waited for. */
static uint8_t dhcp_wait(uint32_t xid, uint32_t deadline_ms, uint32_t *yiaddr,
                          uint32_t *server_id, uint32_t *gw, uint32_t *dns, uint32_t *mask)
{
    while (debugmon_uptime_ms() < deadline_ms) {
        uint8_t buf[1536];
        int len = nic_poll(buf, sizeof(buf));
        if (len <= 0) { task_yield(); continue; }
        if (len < 14 + (int)sizeof(ip_hdr_t) + (int)sizeof(udp_hdr_t) + (int)sizeof(dhcp_hdr_t)) continue;

        eth_hdr_t *eth = (eth_hdr_t *)buf;
        if (ntohs(eth->type) != ETHERTYPE_IP) continue;

        ip_hdr_t *ip = (ip_hdr_t *)(buf + 14);
        if (ip->protocol != IPPROTO_UDP) continue;

        udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
        if (ntohs(udp->dst_port) != DHCP_CLIENT_PORT) continue;

        dhcp_hdr_t *dhcp = (dhcp_hdr_t *)((uint8_t *)udp + sizeof(udp_hdr_t));
        if (dhcp->xid != xid) continue;
        if (ntohl(dhcp->magic) != DHCP_MAGIC) continue;

        uint8_t *opts = (uint8_t *)dhcp + sizeof(dhcp_hdr_t);
        int opt_len = len - (14 + (int)sizeof(ip_hdr_t) + (int)sizeof(udp_hdr_t) + (int)sizeof(dhcp_hdr_t));

        uint8_t mtype = 0;
        if (dhcp_find_option(opts, opt_len, 53, &mtype, 1) < 0) continue;

        *yiaddr = dhcp->yiaddr;
        uint8_t tmp[4];
        if (dhcp_find_option(opts, opt_len, 54, tmp, 4) == 4) memcpy(server_id, tmp, 4);
        if (dhcp_find_option(opts, opt_len, 1,  tmp, 4) == 4) memcpy(mask, tmp, 4);
        if (dhcp_find_option(opts, opt_len, 3,  tmp, 4) == 4) memcpy(gw, tmp, 4);
        if (dhcp_find_option(opts, opt_len, 6,  tmp, 4) == 4) memcpy(dns, tmp, 4);
        return mtype;
    }
    return 0;
}

int dhcp_configure(void)
{
    if (!nic_send || !nic_poll) return DHCP_ERR_NO_NIC;

    uint32_t xid = 0x44484350U ^ debugmon_uptime_ms(); /* "DHCP" xor'd with uptime -- just needs to be unlikely to collide, not cryptographically random */

    dhcp_send(DHCPDISCOVER, xid, 0, 0);

    uint32_t offered_ip = 0, server_id = 0, gw = 0, dns = 0, mask = 0;
    uint32_t deadline = debugmon_uptime_ms() + 4000;
    uint8_t mtype = dhcp_wait(xid, deadline, &offered_ip, &server_id, &gw, &dns, &mask);
    if (mtype != DHCPOFFER) return DHCP_ERR_TIMEOUT;

    dhcp_send(DHCPREQUEST, xid, offered_ip, server_id);

    uint32_t final_ip = 0, final_gw = 0, final_dns = 0, final_mask = 0;
    deadline = debugmon_uptime_ms() + 4000;
    mtype = dhcp_wait(xid, deadline, &final_ip, &server_id, &final_gw, &final_dns, &final_mask);
    if (mtype == DHCPNAK) return DHCP_ERR_NAK;
    if (mtype != DHCPACK) return DHCP_ERR_TIMEOUT;

    net_ip = final_ip;
    if (final_mask) net_netmask = final_mask;
    if (final_gw)   net_gateway = final_gw;
    if (final_dns)  net_dns = final_dns;
    return 0;
}

const char *dhcp_strerror(int err)
{
    switch (err) {
        case DHCP_ERR_NO_NIC:  return "no network card found";
        case DHCP_ERR_TIMEOUT: return "no DHCP server responded";
        case DHCP_ERR_NAK:     return "DHCP server refused the lease";
        default:                return "unknown error";
    }
}
