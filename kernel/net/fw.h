#ifndef FW_H
#define FW_H

#include <stdint.h>
#include "ip.h"

/* -----------------------------------------------------------------------
 * Firewall actions
 * ----------------------------------------------------------------------- */
#define FW_ACCEPT  0
#define FW_DROP    1
#define FW_REJECT  2

/* -----------------------------------------------------------------------
 * Firewall rule
 * 0 in a field = wildcard (match any)
 * ----------------------------------------------------------------------- */
#define FW_RULE_MAX 32

typedef struct {
    uint8_t  proto;       /* IPPROTO_*; 0 = any  */
    uint32_t src_ip;
    uint32_t src_mask;
    uint32_t dst_ip;
    uint32_t dst_mask;
    uint16_t src_port;    /* 0 = any              */
    uint16_t dst_port;    /* 0 = any              */
    uint8_t  action;      /* FW_ACCEPT/DROP/REJECT */
    int      valid;
} fw_rule_t;

/* -----------------------------------------------------------------------
 * Connection tracking (conntrack)
 * Tracks established TCP/UDP sessions for stateful filtering and NAT.
 * ----------------------------------------------------------------------- */
#define CT_MAX 64

typedef struct {
    uint8_t  proto;
    uint32_t src_ip,   dst_ip;
    uint16_t src_port, dst_port;
    /* NAT translation fields (non-zero = translated) */
    uint32_t nat_src_ip,  nat_dst_ip;
    uint16_t nat_src_port, nat_dst_port;
    int      valid;
} ct_entry_t;

/* -----------------------------------------------------------------------
 * NAT rules
 * ----------------------------------------------------------------------- */
#define NAT_MAX 16

typedef enum { NAT_SNAT = 0, NAT_DNAT = 1 } nat_type_t;

typedef struct {
    nat_type_t type;
    uint32_t   match_ip;   /* source IP to SNAT / destination IP to DNAT */
    uint32_t   match_mask;
    uint16_t   match_port; /* 0 = any */
    uint32_t   new_ip;     /* replacement IP   */
    uint16_t   new_port;   /* replacement port (0 = keep) */
    int        valid;
} nat_rule_t;

void fw_init(void);

/* Add/remove firewall rules */
int  fw_rule_add(uint8_t proto,
                 uint32_t src_ip, uint32_t src_mask,
                 uint32_t dst_ip, uint32_t dst_mask,
                 uint16_t src_port, uint16_t dst_port,
                 uint8_t action);
int  fw_rule_del(int index);
void fw_rule_list(void);

/* Add/remove NAT rules */
int  nat_rule_add(nat_type_t type,
                  uint32_t match_ip, uint32_t match_mask, uint16_t match_port,
                  uint32_t new_ip, uint16_t new_port);
int  nat_rule_del(int index);
void nat_rule_list(void);

/*
 * Run a packet through the firewall + NAT on receive.
 * Returns FW_ACCEPT / FW_DROP / FW_REJECT.
 * May modify packet in-place for DNAT.
 */
int fw_rx(ip_hdr_t *ip, void *payload, int payload_len);

/*
 * Run a packet through SNAT on transmit (modifies ip in-place).
 */
void fw_tx(ip_hdr_t *ip, void *payload, int payload_len);

/* Dump conntrack table */
void ct_dump(void);

#endif
