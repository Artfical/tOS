#ifndef PCNET_H
#define PCNET_H

#include <stdint.h>

#define PCNET_VENDOR_AMD  0x1022
#define PCNET_DEVICE_PCI_II   0x2000
#define PCNET_DEVICE_FAST_III 0x2001
#define PCNET_DEVICE_HOME     0x2002

int  pcnet_init(void);
void pcnet_send(void *data, int len);
int  pcnet_poll(uint8_t *buf, int max_len);

#endif
