#ifndef NIC_H
#define NIC_H

#include <stdint.h>

extern void (*nic_send)(void *data, int len);
extern int  (*nic_poll)(uint8_t *buf, int max_len);

int nic_init(void);

#endif
