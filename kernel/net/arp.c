#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_cache_t;

#define ARP_CACHE_SIZE 16
static arp_cache_t arp_cache[ARP_CACHE_SIZE];

typedef struct {
    eth_hdr_t eth;
    uint16_t  htype;
    uint16_t  ptype;
    uint8_t   hlen;
    uint8_t   plen;
    uint16_t  oper;
    uint8_t   sha[6];
    uint8_t   spa[4];
    uint8_t   tha[6];
    uint8_t   tpa[4];
} __attribute__((packed)) arp_pkt_t;

void arp_init(void)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;
}

static void arp_cache_add(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].valid = 1;
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
}

static void arp_send_request(uint32_t ip)
{
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t *arp = (arp_pkt_t *)buf;
    memset(buf, 0, sizeof(arp_pkt_t));
    memset(arp->eth.dst, 0xFF, 6);
    memcpy(arp->eth.src, net_mac, 6);
    arp->eth.type = htons(ETHERTYPE_ARP);
    arp->htype = htons(1);
    arp->ptype = htons(ETHERTYPE_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1);
    memcpy(arp->sha, net_mac, 6);
    *(uint32_t *)arp->spa = net_ip;
    *(uint32_t *)arp->tpa = ip;
    nic_send(buf, sizeof(arp_pkt_t));
}

int arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 0;
        }
    }
    arp_send_request(ip);
    for (int retry = 0; retry < 200; retry++) {
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(pkt, len);
        }
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && arp_cache[i].ip == ip) {
                memcpy(mac_out, arp_cache[i].mac, 6);
                return 0;
            }
        }
    }
    return -1;
}

void arp_handle(uint8_t *data, int len)
{
    (void)len;
    arp_pkt_t *arp = (arp_pkt_t *)data;
    uint32_t src_ip = *(uint32_t *)arp->spa;
    arp_cache_add(src_ip, arp->sha);

    if (ntohs(arp->oper) == 1 && *(uint32_t *)arp->tpa == net_ip) {
        uint8_t buf[sizeof(arp_pkt_t)];
        arp_pkt_t *reply = (arp_pkt_t *)buf;
        memset(buf, 0, sizeof(arp_pkt_t));
        memcpy(reply->eth.dst, arp->sha, 6);
        memcpy(reply->eth.src, net_mac, 6);
        reply->eth.type = htons(ETHERTYPE_ARP);
        reply->htype = htons(1);
        reply->ptype = htons(ETHERTYPE_IP);
        reply->hlen = 6;
        reply->plen = 4;
        reply->oper = htons(2);
        memcpy(reply->sha, net_mac, 6);
        *(uint32_t *)reply->spa = net_ip;
        memcpy(reply->tha, arp->sha, 6);
        *(uint32_t *)reply->tpa = src_ip;
        nic_send(buf, sizeof(arp_pkt_t));
    }
}
