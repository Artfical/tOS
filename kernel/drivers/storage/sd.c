#include "sd.h"
#include "spi.h"
#include "io.h"
int sd_init(sd_device_t *dev)
{
    dev->present = 0;
    dev->sdhc = 0;
    dev->capacity = 0;
    dev->rca = 0;
    return -1;
}
int sd_read_block(sd_device_t *dev, uint32_t lba, void *buf)
{
    (void)dev;
    (void)lba;
    (void)buf;
    return -1;
}
int sd_write_block(sd_device_t *dev, uint32_t lba, const void *buf)
{
    (void)dev;
    (void)lba;
    (void)buf;
    return -1;
}
