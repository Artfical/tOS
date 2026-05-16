#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>

#define VIRTIO_MAGIC 0x74726976
#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_DEVICE_BLK 0x1001
#define VIRTIO_DEVICE_NET 0x1000
#define VIRTIO_DEVICE_INPUT 0x1050

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_REG_MAGIC 0x00
#define VIRTIO_REG_VERSION 0x04
#define VIRTIO_REG_DEVICE_ID 0x08
#define VIRTIO_REG_VENDOR 0x0C
#define VIRTIO_REG_HOST_FEATURES 0x10
#define VIRTIO_REG_GUEST_FEATURES 0x20
#define VIRTIO_REG_QUEUE_ADDR 0x24
#define VIRTIO_REG_QUEUE_SIZE 0x28
#define VIRTIO_REG_QUEUE_SEL 0x2C
#define VIRTIO_REG_STATUS 0x3A
#define VIRTIO_REG_DEVICE_CFG 0x3C

typedef struct {
    uint16_t io_base;
    uint16_t device_id;
    uint8_t status;
    int initialized;
} virtio_device_t;

typedef struct {
    volatile uint32_t *desc;
    volatile uint32_t *avail;
    volatile uint32_t *used;
    int size;
    int next_desc;
} virtio_queue_t;

int virtio_init(virtio_device_t *dev, uint16_t io_base);
int virtio_init_queue(virtio_device_t *dev, virtio_queue_t *queue, int index, int size);
void virtio_set_status(virtio_device_t *dev, uint8_t status);

#endif
