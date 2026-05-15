#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "ip.h"

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO       8

void icmp_handle(ip_hdr_t *ip, void *pkt, int len);
int  icmp_ping(uint32_t dst_ip);

#endif
