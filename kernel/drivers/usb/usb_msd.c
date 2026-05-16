#include "usb_msd.h"
#include "usb.h"
#include "memory.h"
int usb_msd_init(usb_msd_device_t *dev)
{
    dev->present = 0;
    dev->dev_addr = 0;
    dev->bulk_in_ep = 0;
    dev->bulk_out_ep = 0;
    dev->block_count = 0;
    dev->block_size = 512;
    return -1;
}
int usb_msd_read(usb_msd_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
int usb_msd_write(usb_msd_device_t *dev, uint32_t lba, uint8_t count, const void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
