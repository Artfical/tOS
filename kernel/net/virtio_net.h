#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

int  virtio_net_init(void);
void virtio_net_send(void *data, int len);
int  virtio_net_poll(uint8_t *buf, int max_len);

#endif
