#include "virtio.h"
#include "io.h"
#include "memory.h"

static uint16_t virtio_read16(virtio_device_t *dev, uint16_t reg)
{
    return inw(dev->io_base + reg);
}

static uint32_t virtio_read32(virtio_device_t *dev, uint16_t reg)
{
    return inl(dev->io_base + reg);
}

static void virtio_write32(virtio_device_t *dev, uint16_t reg, uint32_t val)
{
    outl(dev->io_base + reg, val);
}

int virtio_init(virtio_device_t *dev, uint16_t io_base)
{
    dev->io_base = io_base;
    dev->initialized = 0;

    uint32_t magic = virtio_read32(dev, VIRTIO_REG_MAGIC);
    if (magic != VIRTIO_MAGIC) return -1;

    dev->device_id = virtio_read16(dev, VIRTIO_REG_DEVICE_ID);
    if (dev->device_id == 0) return -1;

    dev->status = 0;
    virtio_set_status(dev, VIRTIO_STATUS_ACK);
    virtio_set_status(dev, VIRTIO_STATUS_DRIVER);
    dev->initialized = 1;
    return 0;
}

int virtio_init_queue(virtio_device_t *dev, virtio_queue_t *queue, int index, int size)
{
    if (!dev->initialized) return -1;

    outw(dev->io_base + VIRTIO_REG_QUEUE_SEL, index);
    queue->size = virtio_read16(dev, VIRTIO_REG_QUEUE_SIZE);
    if (queue->size > size) queue->size = size;

    uint32_t desc_addr = (uint32_t)malloc(queue->size * 16 + queue->size * 4 + queue->size * 8);
    if (!desc_addr) return -1;

    queue->desc = (uint32_t *)desc_addr;
    queue->avail = (uint32_t *)(desc_addr + queue->size * 16);
    queue->used = (uint32_t *)(desc_addr + queue->size * 16 + queue->size * 4);
    queue->next_desc = 0;

    virtio_write32(dev, VIRTIO_REG_QUEUE_ADDR, desc_addr);
    return 0;
}

void virtio_set_status(virtio_device_t *dev, uint8_t status)
{
    dev->status |= status;
    outb(dev->io_base + VIRTIO_REG_STATUS, dev->status);
}
