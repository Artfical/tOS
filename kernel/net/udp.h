#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "ip.h"

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

int  udp_open(uint16_t port);
int  udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, void *data, int len);
void udp_handle(ip_hdr_t *ip, void *pkt, int len);
int  udp_listen(uint16_t port, uint8_t *resp, int max_len, uint32_t *src_ip, uint16_t *src_port);

#endif
