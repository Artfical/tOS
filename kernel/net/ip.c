#include "ip.h"
#include "arp.h"
#include "net.h"
#include "rtl8139.h"
#include "string.h"
#include "memory.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"

static uint16_t ip_id = 0;

static uint16_t ip_checksum(uint16_t *buf, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++) sum += ntohs(buf[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

int ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len)
{
    uint8_t mac[6];
    if (arp_resolve(dst_ip, mac) != 0) return -1;

    int total = sizeof(ip_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total + 14);
    if (!buf) return -1;

    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(buf + 14);
    memset(ip, 0, sizeof(ip_hdr_t));
    ip->ver_ihl = 0x45;
    ip->total_len = htons(total);
    ip->id = htons(ip_id);
    ip_id++;
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = net_ip;
    ip->dst_ip = dst_ip;
    ip->checksum = 0;
    ip->checksum = ip_checksum((uint16_t *)ip, sizeof(ip_hdr_t));

    memcpy(buf + 14 + sizeof(ip_hdr_t), data, len);
    rtl8139_send(buf, total + 14);
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
    if (ip_checksum((uint16_t *)ip, sizeof(ip_hdr_t)) != sum) return;
    ip->checksum = sum;

    int payload_len = len - ihl;
    void *payload = (uint8_t *)data + ihl;

    switch (ip->protocol) {
        case IPPROTO_ICMP: icmp_handle(ip, payload, payload_len); break;
        case IPPROTO_UDP:  udp_handle(ip, payload, payload_len); break;
        case IPPROTO_TCP:  tcp_handle(ip, payload, payload_len); break;
    }
}
