#ifndef E1000_H
#define E1000_H

#include <stdint.h>

int  e1000_init(void);
void e1000_send(void *data, int len);
int  e1000_poll(uint8_t *buf, int max_len);

#endif
