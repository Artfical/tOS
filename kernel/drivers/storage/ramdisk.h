#ifndef RAMDISK_H
#define RAMDISK_H
#include <stdint.h>
typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t block_size;
    int present;
} ramdisk_device_t;
int ramdisk_create(ramdisk_device_t *dev, uint32_t size, uint32_t block_size);
int ramdisk_read(ramdisk_device_t *dev, uint32_t block, void *buf);
int ramdisk_write(ramdisk_device_t *dev, uint32_t block, const void *buf);
void ramdisk_destroy(ramdisk_device_t *dev);
#endif
