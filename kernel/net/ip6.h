#ifndef IP6_H
#define IP6_H

#include <stdint.h>

/* EtherType for IPv6 */
#define ETHERTYPE_IPV6 0x86DD

/* IPv6 header (40 bytes, fixed) */
typedef struct {
    uint32_t ver_tc_fl;   /* version(4) | traffic class(8) | flow label(20) */
    uint16_t payload_len;
    uint8_t  next_header;
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed)) ip6_hdr_t;

/* Our link-local IPv6 address (generated from MAC at init) */
extern uint8_t net_ip6[16];

/* Convert MAC to EUI-64 based link-local address */
void ip6_init(const uint8_t *mac);

/* Send an IPv6 packet (wraps in Ethernet frame using NDP cache or multicast) */
int ip6_send(const uint8_t *dst_ip6, uint8_t next_header, void *data, int len);

/* Dispatch incoming IPv6 packet */
void ip6_handle(uint8_t *data, int len);

/* Format IPv6 address as string (abbreviated) into buf (min 40 bytes) */
void ip6_fmt(const uint8_t *addr, char *buf);

/* Parse "fe80::1" style string into 16-byte array; returns 0 on success */
int  ip6_parse(const char *s, uint8_t *out);

#endif
