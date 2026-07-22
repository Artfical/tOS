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

/* icmp_ping()'s negative return codes -- see dns.h's DNS_ERR_* for
 * the same reasoning: distinguishing "couldn't even send" (no route,
 * ARP never resolved the next hop) from "sent fine, nothing ever
 * replied" (the far more common case) tells a caller where to look
 * instead of a single opaque failure. */
#define ICMP_ERR_SEND    -1  /* couldn't send (no route/ARP failure for the next hop) */
#define ICMP_ERR_TIMEOUT -2  /* request sent, but no echo reply arrived before the deadline */

void icmp_handle(ip_hdr_t *ip, void *pkt, int len);
int  icmp_ping(uint32_t dst_ip);
const char *icmp_ping_strerror(int err);

#endif
