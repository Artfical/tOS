#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>

#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

int  rtl8139_init(void);
void rtl8139_send(void *data, int len);
int  rtl8139_poll(uint8_t *buf, int max_len);

#endif
