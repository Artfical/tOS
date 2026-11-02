#include "ip.h"
#include "arp.h"
#include "net.h"
#include "nic.h"
#include "route.h"
#include "fw.h"
#include "gre.h"
#include "ipip.h"
#include "string.h"
#include "memory.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "sctp.h"
#include "dccp.h"
#include "udplite.h"
#include "ipsec.h"

static uint16_t ip_id = 0;

static uint16_t ip_checksum(const uint8_t *buf, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len - 1; i += 2)
        sum += ((uint16_t)buf[i] << 8) | buf[i + 1];
    if (len & 1) sum += (uint16_t)buf[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

#define LOOPQ 256
static struct { uint8_t *buf; int len; } loopq[LOOPQ];
static int loop_head, loop_tail;

int ip_loop_enqueue(const uint8_t *pkt, int len)
{
    int next = (loop_head + 1) % LOOPQ;
    if (next == loop_tail) return IP_ERR_NOMEM;                  /* full: dropped, as a busy network would */
    uint8_t *b = (uint8_t *)malloc((size_t)len);
    if (!b) return IP_ERR_NOMEM;
    memcpy(b, pkt, (size_t)len);
    loopq[loop_head].buf = b; loopq[loop_head].len = len;
    loop_head = next;
    return 0;
}

int ip_loop_poll(void)
{
    if (loop_tail == loop_head) return 0;
    uint8_t *b = loopq[loop_tail].buf; int len = loopq[loop_tail].len;
    loopq[loop_tail].buf = 0;
    loop_tail = (loop_tail + 1) % LOOPQ;
    ip_handle(b, len);
    free(b);
    return 1;
}

int ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len)
{
    /* A negative len would wrap into a huge malloc()/memcpy(); anything
     * above the MTU would be sent as an oversized, unfragmented frame. */
    if (len < 0 || len > IP_MAX_PAYLOAD) return IP_ERR_TOOBIG;

    if (ip_is_loopback(dst_ip)) {                                 /* to this machine: no ARP, no network card */
        int ltotal = (int)sizeof(ip_hdr_t) + len;
        uint8_t *lb = (uint8_t *)malloc((size_t)ltotal);
        if (!lb) return IP_ERR_NOMEM;
        ip_hdr_t *lip = (ip_hdr_t *)lb;
        memset(lip, 0, sizeof(ip_hdr_t));
        lip->ver_ihl = 0x45; lip->total_len = htons((uint16_t)ltotal); lip->id = htons(ip_id); ip_id++;
        lip->flags_frag = htons(0x4000); lip->ttl = 64; lip->protocol = protocol;
        lip->src_ip = ip_loopback_addr(); lip->dst_ip = dst_ip;
        lip->checksum = ip_checksum((uint8_t *)lip, sizeof(ip_hdr_t));
        memcpy(lb + sizeof(ip_hdr_t), data, (size_t)len);
        int lr = ip_loop_enqueue(lb, ltotal);
        free(lb);
        return lr;
    }

    /* Route lookup: resolve next-hop gateway */
    uint32_t nh = route_lookup(net_ip, dst_ip);
    if (!nh) nh = dst_ip; /* no route configured — try direct */

    uint8_t mac[6];
    int arc = arp_resolve(nh, mac);
    if (arc != 0) return arc;

    int total = sizeof(ip_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total + 14);
    if (!buf) return IP_ERR_NOMEM;

    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(buf + 14);
    memset(ip, 0, sizeof(ip_hdr_t));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons((uint16_t)total);
    ip->id        = htons(ip_id); ip_id++;
    ip->flags_frag = htons(0x4000);
    ip->ttl       = 64;
    ip->protocol  = protocol;
    ip->src_ip    = net_ip;
    ip->dst_ip    = dst_ip;
    ip->checksum  = 0;
    ip->checksum  = ip_checksum((uint8_t *)ip, sizeof(ip_hdr_t));

    memcpy(buf + 14 + sizeof(ip_hdr_t), data, len);

    /* Apply SNAT if configured */
    fw_tx(ip, buf + 14 + sizeof(ip_hdr_t), len);

    nic_transmit(buf, total + 14);
    free(buf);
    return 0;
}

void ip_handle(uint8_t *data, int len)
{
    if (len < (int)sizeof(ip_hdr_t)) return;
    ip_hdr_t *ip = (ip_hdr_t *)data;
    if ((ip->ver_ihl >> 4) != 4) return;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || len < ihl) return;
    if (ip->dst_ip != net_ip && !ip_is_loopback(ip->dst_ip)) return;

    /* The header checksum covers the whole header including options, not
     * just the fixed 20 bytes. */
    uint16_t sum = ip->checksum;
    ip->checksum = 0;
    if (ip_checksum((uint8_t *)ip, ihl) != sum) return;
    ip->checksum = sum;

    /* There is no reassembly: a fragment (MF set or non-zero offset) is not
     * a complete datagram, and handing its bytes to the UDP/TCP parsers
     * would treat arbitrary mid-datagram data as a header. Drop them. */
    if (ntohs(ip->flags_frag) & 0x3FFF) return;

    /* Use the IP header's own declared total_len, not the raw Ethernet
     * frame length -- small packets (e.g. a bare ACK, or a short data
     * segment) routinely get padded up to the 60-byte Ethernet minimum
     * by real switches/NICs, and `len` here is that padded frame size.
     * Using it directly turned that trailing zero-padding into fake
     * extra TCP/UDP payload bytes appended to whatever real data the
     * segment carried -- e.g. a handful of real response bytes coming
     * out the other end as a chunk of zero bytes. Clamp against `len`
     * too, since total_len is attacker/peer-controlled and must never
     * be trusted to read past what was actually received. */
    int ip_total = ntohs(ip->total_len);
    if (ip_total < ihl || ip_total > len) return;
    int payload_len = ip_total - ihl;
    void *payload = (uint8_t *)data + ihl;

    /* Firewall + DNAT check */
    int verdict = fw_rx(ip, payload, payload_len);
    if (verdict == FW_DROP)   return;
    if (verdict == FW_REJECT) return; /* TODO: send ICMP unreachable */

    switch (ip->protocol) {
        case IPPROTO_ICMP:    icmp_handle(ip, payload, payload_len);     break;
        case IPPROTO_UDP:     udp_handle(ip, payload, payload_len);      break;
        case IPPROTO_TCP:     tcp_handle(ip, payload, payload_len);      break;
        case IPPROTO_SCTP:    sctp_handle(ip, payload, payload_len);     break;
        case IPPROTO_DCCP:    dccp_handle(ip, payload, payload_len);     break;
        case IPPROTO_UDPLITE: udplite_handle(ip, payload, payload_len);  break;
        case IPPROTO_AH:      ipsec_ah_handle(ip, payload, payload_len); break;
        case IPPROTO_ESP:     ipsec_esp_handle(ip, payload, payload_len);break;
        case IPPROTO_GRE:     gre_handle(ip, payload, payload_len);      break;
        case IPPROTO_IPIP:    ipip_handle(ip, payload, payload_len);     break;
    }
}
