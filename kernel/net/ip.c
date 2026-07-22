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

int ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len)
{
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

    nic_tx_packets++;
    nic_tx_bytes += (uint32_t)(total + 14);
    nic_send(buf, total + 14);
    free(buf);
    return 0;
}

void ip_handle(uint8_t *data, int len)
{
    if (len < (int)sizeof(ip_hdr_t)) return;
    ip_hdr_t *ip = (ip_hdr_t *)data;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || len < ihl) return;
    if (ip->dst_ip != net_ip) return;

    uint16_t sum = ip->checksum;
    ip->checksum = 0;
    if (ip_checksum((uint8_t *)ip, sizeof(ip_hdr_t)) != sum) return;
    ip->checksum = sum;

    int payload_len = len - ihl;
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
