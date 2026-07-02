#ifndef VLAN_H
#define VLAN_H

#include <stdint.h>

#define ETHERTYPE_VLAN 0x8100

/* 802.1Q tag sits between src MAC and EtherType in the ethernet header */
typedef struct {
    uint16_t tci;        /* PCP(3) | DEI(1) | VID(12) */
    uint16_t inner_type; /* original EtherType         */
} __attribute__((packed)) vlan_tag_t;

#define VLAN_VID(tci)  ((uint16_t)((tci) & 0x0FFF))
#define VLAN_PCP(tci)  ((uint8_t)(((tci) >> 13) & 0x07))
#define VLAN_MAX       16

/* Register a VLAN ID as locally active (0 = untagged / all) */
int  vlan_add(uint16_t vid);
int  vlan_remove(uint16_t vid);
void vlan_list(void);

/*
 * Strip a 4-byte 802.1Q tag from a received frame (in-place).
 * Returns the inner EtherType, or 0 if the frame is not tagged.
 * *len is decreased by 4 on success.
 * out_vid receives the VID (may be NULL).
 */
uint16_t vlan_strip(uint8_t *frame, int *len, uint16_t *out_vid);

/*
 * Insert a 4-byte 802.1Q tag into a frame before transmission.
 * buf must have at least 4 extra bytes of space at the front (i.e. caller
 * allocates ETH_HDR+4+payload).  *len is increased by 4.
 */
void vlan_insert(uint8_t *frame, int *len, uint16_t vid, uint8_t pcp);

/* Returns 1 if vid is registered (or if no VLANs configured = accept all) */
int vlan_allowed(uint16_t vid);

#endif
