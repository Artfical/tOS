#ifndef ARP_H
#define ARP_H

#include <stdint.h>

/* arp_resolve()'s negative return codes. Both used to collapse to a
 * single -1, making "no NIC driver was ever found" (nic_send/nic_poll
 * still NULL -- networking can never work at all) indistinguishable
 * from "a real NIC sent the request but nothing ever answered it"
 * (which usually means something's actually reachable but not
 * replying, or a driver RX bug) -- two completely different problems
 * that need looking in very different places. */
#define ARP_ERR_NO_NIC   -1  /* no NIC driver was found at boot; nic_send/nic_poll are NULL */
#define ARP_ERR_TIMEOUT  -2  /* request sent, but no ARP reply arrived before the deadline */

void arp_init(void);
int  arp_resolve(uint32_t ip, uint8_t *mac_out);
void arp_handle(uint8_t *data, int len);
const char *arp_resolve_strerror(int err);

#endif
