#ifndef UDPLITE_H
#define UDPLITE_H

#include <stdint.h>
#include "ip.h"

/* IANA protocol number (RFC 3828) */
#define IPPROTO_UDPLITE 136

/*
 * UDP-Lite header — identical byte layout to UDP.
 * The key difference: the "length" field is reinterpreted
 * as "checksum coverage" (how many bytes the checksum covers).
 * A coverage of 0 means "full datagram".  Minimum coverage = 8
 * (just the header).
 */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t checksum_coverage; /* bytes protected by checksum; 0 = full */
    uint16_t checksum;
} __attribute__((packed)) udplite_hdr_t;

/* Socket table size */
#define UDPLITE_SOCKETS 4

/* Public API */
int  udplite_open(uint16_t port);
int  udplite_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                  void *data, int len, uint16_t coverage);
int  udplite_listen(uint16_t port, uint8_t *buf, int max_len,
                    uint32_t *src_ip, uint16_t *src_port);
void udplite_handle(ip_hdr_t *ip, void *pkt, int len);

#endif
