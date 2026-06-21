#ifndef NE2000_H
#define NE2000_H

#include <stdint.h>

int  ne2000_init(void);
void ne2000_send(void *data, int len);
int  ne2000_poll(uint8_t *buf, int max_len);

#endif
