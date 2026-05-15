#include "icmp.h"
#include "ip.h"
#include "net.h"
#include "rtl8139.h"
#include "arp.h"
#include "string.h"
#include "terminal.h"

static int ping_reply = 0;

static uint16_t icmp_checksum(uint16_t *buf, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++) sum += ntohs(buf[i]);
    if (len & 1) sum += ((uint8_t *)buf)[len - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

void icmp_handle(ip_hdr_t *ip, void *pkt, int len)
{
    icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
    if (len < (int)sizeof(icmp_hdr_t)) return;

    uint16_t sum = icmp->checksum;
    icmp->checksum = 0;
    if (icmp_checksum((uint16_t *)pkt, len) != sum) return;
    icmp->checksum = sum;

    if (icmp->type == ICMP_ECHO) {
        icmp->type = ICMP_ECHO_REPLY;
        icmp->checksum = 0;
        icmp->checksum = icmp_checksum((uint16_t *)pkt, len);
        ip_send(ip->src_ip, IPPROTO_ICMP, pkt, len);
    } else if (icmp->type == ICMP_ECHO_REPLY) {
        ping_reply = 1;
    }
}

int icmp_ping(uint32_t dst_ip)
{
    ping_reply = 0;
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->id = htons(1);
    icmp->seq = htons(1);
    icmp->checksum = icmp_checksum((uint16_t *)pkt, sizeof(pkt));

    if (ip_send(dst_ip, IPPROTO_ICMP, pkt, sizeof(pkt)) != 0)
        return -1;

    for (int i = 0; i < 50000; i++) {
        uint8_t buf[1536];
        int len = rtl8139_poll(buf, sizeof(buf));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(buf, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
        if (ping_reply) return 0;
    }
    return -1;
}
