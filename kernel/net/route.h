#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>

#define ROUTE_MAX        32
#define ROUTE_TABLE_MAX   8   /* 0=main, 1-7=policy tables */

typedef struct {
    uint32_t dst;      /* destination network (host-byte order) */
    uint32_t mask;     /* subnet mask (host-byte order)         */
    uint32_t gw;       /* next-hop gateway; 0 = directly attached */
    uint32_t src;      /* preferred source IP; 0 = auto         */
    int      metric;   /* lower wins                            */
    int      table;    /* routing table id (0=main)             */
    int      valid;
} route_entry_t;

/* Policy rule: match packet -> select table */
#define POLICY_MAX 8
typedef struct {
    uint32_t src_ip;
    uint32_t src_mask;
    uint32_t dst_ip;
    uint32_t dst_mask;
    int      table;    /* route table to use for matched packets */
    int      priority; /* lower = evaluated first                */
    int      valid;
} policy_rule_t;

void     route_init(void);
int      route_add(uint32_t dst, uint32_t mask, uint32_t gw, uint32_t src,
                   int metric, int table);
int      route_del(uint32_t dst, uint32_t mask, int table);
void     route_list(void);

/*
 * Resolve next-hop for dst, considering policy rules based on src.
 * Returns the gateway IP (or dst itself if directly connected).
 * Returns 0 on failure (no route).
 */
uint32_t route_lookup(uint32_t src, uint32_t dst);

/* Policy routing */
int  policy_add(uint32_t src_ip, uint32_t src_mask,
                uint32_t dst_ip, uint32_t dst_mask,
                int table, int priority);
int  policy_del(int priority);
void policy_list(void);

/* Read-only query API (for Network Monitor GUI) */
int route_get_count(void);
int route_get_entry(int i, route_entry_t *out);

#endif
