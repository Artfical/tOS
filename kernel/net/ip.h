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

/* ip_send() propagates arp_resolve()'s own ARP_ERR_* code verbatim
 * when address resolution is what failed (see arp.h) -- callers that
 * want a precise reason can pass a nonzero, negative result straight
 * to arp_resolve_strerror(). IP_ERR_NOMEM is ip_send()'s own failure,
 * for the one case that isn't ARP's fault. */
#define IP_ERR_NOMEM -3

int  ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len);
void ip_handle(uint8_t *data, int len);

#endif
