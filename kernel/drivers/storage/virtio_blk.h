#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H
#include <stdint.h>
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
typedef struct {
    uint16_t io_base;
    int present;
    uint64_t capacity;
    uint32_t sector_size;
} virtio_blk_device_t;
int virtio_blk_init(virtio_blk_device_t *dev);
int virtio_blk_read(virtio_blk_device_t *dev, uint64_t sector, void *buf);
int virtio_blk_write(virtio_blk_device_t *dev, uint64_t sector, const void *buf);
#endif
