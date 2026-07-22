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

/* icmp_ping() propagates ip_send()'s/arp_resolve()'s own error code
 * verbatim when the request couldn't even be sent (see ip.h/arp.h),
 * so those cases report the *actual* underlying reason (no NIC found
 * vs. ARP simply never got a reply) instead of a single generic
 * "couldn't send". ICMP_ERR_TIMEOUT is icmp_ping()'s own failure --
 * the request really did go out, but no echo reply ever came back --
 * and uses a value far outside the network/ARP layer's range so the
 * two can never be confused with each other. */
#define ICMP_ERR_TIMEOUT -10  /* request sent, but no echo reply arrived before the deadline */

void icmp_handle(ip_hdr_t *ip, void *pkt, int len);
int  icmp_ping(uint32_t dst_ip);
const char *icmp_ping_strerror(int err);

#endif
