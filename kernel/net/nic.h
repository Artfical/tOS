#ifndef NIC_H
#define NIC_H

#include <stdint.h>

extern void (*nic_send)(void *data, int len);
extern int  (*nic_poll)(uint8_t *buf, int max_len);

int nic_init(void);

/* Driver name set by nic_init() */
extern char     nic_driver_name[32];

/* Packet / byte counters — incremented by net.c (rx) and ip.c (tx) */
extern uint32_t nic_rx_packets;
extern uint32_t nic_tx_packets;
extern uint32_t nic_rx_bytes;
extern uint32_t nic_tx_bytes;

#endif
