#ifndef NIC_H
#define NIC_H

#include <stdint.h>

extern void (*nic_send)(void *data, int len);
extern int  (*nic_poll)(uint8_t *buf, int max_len);

/* Pads frames under the Ethernet minimum (60 bytes, excluding the
 * 4-byte FCS hardware appends) before handing them to nic_send() --
 * no driver in this tree does that padding itself. QEMU's slirp
 * networking is lenient about undersized frames, but a real switch
 * (or a stricter virtual one, e.g. VMware's) can silently drop them
 * outright. A bare TCP SYN/ACK (54 bytes) or a small ARP/DHCP packet
 * routinely falls under that threshold; ICMP's 64-byte echo payload
 * happens to already clear it, which is why ping could work while
 * every TCP connection attempt silently vanished on such a network.
 * Callers that build a raw Ethernet frame directly should go through
 * this instead of calling nic_send() themselves. */
void nic_transmit(void *data, int len);

int nic_init(void);

/* Driver name set by nic_init() */
extern char     nic_driver_name[32];

/* Packet / byte counters — incremented by net.c (rx) and ip.c (tx) */
extern uint32_t nic_rx_packets;
extern uint32_t nic_tx_packets;
extern uint32_t nic_rx_bytes;
extern uint32_t nic_tx_bytes;

#endif
