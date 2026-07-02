#ifndef IPIP_H
#define IPIP_H

#include <stdint.h>
#include "ip.h"

#define IPPROTO_IPIP   4
#define IPIP_MAX       4

typedef struct {
    uint32_t local_ip;
    uint32_t remote_ip;
    int      valid;
} ipip_tunnel_t;

void ipip_init(void);
int  ipip_tunnel_add(uint32_t local_ip, uint32_t remote_ip);
int  ipip_tunnel_del(int idx);
void ipip_tunnel_list(void);
int  ipip_send(int idx, const void *inner_ip, int inner_len);
void ipip_handle(ip_hdr_t *outer_ip, void *pkt, int len);

#endif
