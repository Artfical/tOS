#ifndef ICMPV6_H
#define ICMPV6_H

#include <stdint.h>
#include "ip6.h"

/* ICMPv6 type values */
#define ICMPV6_DEST_UNREACH     1
#define ICMPV6_PKT_TOO_BIG      2
#define ICMPV6_TIME_EXCEEDED    3
#define ICMPV6_PARAM_PROBLEM    4
#define ICMPV6_ECHO_REQUEST   128
#define ICMPV6_ECHO_REPLY     129
#define ICMPV6_MCAST_LISTENER 130
#define ICMPV6_ROUTER_SOLICIT 133
#define ICMPV6_ROUTER_ADV     134
#define ICMPV6_NEIGH_SOLICIT  135
#define ICMPV6_NEIGH_ADV      136

/* ICMPv6 common header */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} __attribute__((packed)) icmpv6_hdr_t;

/* Echo request/reply body */
typedef struct {
    uint16_t id;
    uint16_t seq;
    /* data follows */
} __attribute__((packed)) icmpv6_echo_t;

/* Neighbor Solicitation / Advertisement */
typedef struct {
    uint32_t reserved;
    uint8_t  target[16];
} __attribute__((packed)) icmpv6_nd_t;

/* NDP option: link-layer address */
typedef struct {
    uint8_t  type;    /* 1=source, 2=target */
    uint8_t  length;  /* in units of 8 bytes */
    uint8_t  addr[6];
} __attribute__((packed)) icmpv6_opt_lladdr_t;

/* Public API */
void icmpv6_handle(ip6_hdr_t *ip6, void *pkt, int len);
int  icmpv6_ping6(const uint8_t *dst_ip6);
int  icmpv6_ndp_resolve(const uint8_t *dst_ip6, uint8_t *mac_out);

#endif
