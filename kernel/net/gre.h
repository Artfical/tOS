#ifndef GRE_H
#define GRE_H

#include <stdint.h>
#include "ip.h"

#define IPPROTO_GRE    47
#define GRE_TUNNEL_MAX  4

/* GRE header (minimal — checksum and routing bits clear) */
typedef struct {
    uint16_t flags;       /* C|R|K|S|s|Recur|Flags|Ver */
    uint16_t proto;       /* EtherType of inner payload  */
    /* optional: checksum(2)+reserved(2), key(4), seq(4) */
} __attribute__((packed)) gre_hdr_t;

#define GRE_FLAG_KEY  0x2000  /* Key present  */
#define GRE_FLAG_SEQ  0x1000  /* Seq present  */
#define GRE_PROTO_IP  0x0800  /* inner IPv4   */

typedef struct {
    uint32_t local_ip;   /* outer src IP          */
    uint32_t remote_ip;  /* outer dst IP          */
    uint32_t key;        /* tunnel key (0=none)   */
    int      use_key;
    int      valid;
} gre_tunnel_t;

void gre_init(void);
int  gre_tunnel_add(uint32_t local_ip, uint32_t remote_ip,
                    uint32_t key, int use_key);
int  gre_tunnel_del(int idx);
void gre_tunnel_list(void);

/* Encapsulate inner_data (IP packet) and send via GRE tunnel idx */
int  gre_send(int idx, const void *inner_data, int inner_len);

/* Called from ip_handle() when proto==47 */
void gre_handle(ip_hdr_t *outer_ip, void *pkt, int len);

#endif
