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
#define IP_ERR_TOOBIG -4   /* negative length, or payload above the 1500-byte MTU (no fragmentation on TX) */
#define IP_MAX_PAYLOAD 1480

int  ip_send(uint32_t dst_ip, uint8_t protocol, void *data, int len);

/* 127.0.0.0/8: packets to it never leave the machine; they wait in a queue that net_poll() empties */
static inline int ip_is_loopback(uint32_t ip) { return (ip & 0xFFu) == 127u; }
static inline uint32_t ip_loopback_addr(void) { return 0x0100007Fu; }          /* 127.0.0.1, in net_ip's byte order */
int  ip_loop_enqueue(const uint8_t *ip_packet, int len);                        /* 0, or an error */
int  ip_loop_poll(void);                                                       /* handles one queued packet: 1, or 0 when none */
void ip_handle(uint8_t *data, int len);

#endif
