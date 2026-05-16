#include "ramdisk.h"
#include "memory.h"
#include "string.h"
int ramdisk_create(ramdisk_device_t *dev, uint32_t size, uint32_t block_size)
{
    dev->data = (uint8_t *)malloc(size);
    if (!dev->data) return -1;
    dev->size = size;
    dev->block_size = block_size ? block_size : 512;
    dev->present = 1;
    return 0;
}
int ramdisk_read(ramdisk_device_t *dev, uint32_t block, void *buf)
{
    if (!dev->present) return -1;
    uint32_t offset = block * dev->block_size;
    if (offset + dev->block_size > dev->size) return -1;
    memcpy(buf, dev->data + offset, dev->block_size);
    return 0;
}
int ramdisk_write(ramdisk_device_t *dev, uint32_t block, const void *buf)
{
    if (!dev->present) return -1;
    uint32_t offset = block * dev->block_size;
    if (offset + dev->block_size > dev->size) return -1;
    memcpy(dev->data + offset, buf, dev->block_size);
    return 0;
}
void ramdisk_destroy(ramdisk_device_t *dev)
{
    if (dev->data) free(dev->data);
    dev->present = 0;
}
