#ifndef NET_H
#define NET_H

#include <stdint.h>

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

extern uint8_t  net_mac[6];
extern uint32_t net_ip;
extern uint32_t net_gateway;
extern uint32_t net_dns;

#define htons(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t)(((x) >> 24) | (((x) >> 8) & 0xFF00) | (((x) << 8) & 0xFF0000) | ((x) << 24)))
#define ntohl(x) htonl(x)

#define IP4(a,b,c,d) ((uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

void net_init(void);
void net_poll(void);

#endif
