#ifndef IP_H
#define IP_H

#include <stdint.h>

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip_hdr_t;

int  ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len);
void ip_handle(uint8_t *data, int len);

#endif
