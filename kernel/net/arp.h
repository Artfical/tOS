#ifndef ARP_H
#define ARP_H

#include <stdint.h>

void arp_init(void);
int  arp_resolve(uint32_t ip, uint8_t *mac_out);
void arp_handle(uint8_t *data, int len);

#endif
