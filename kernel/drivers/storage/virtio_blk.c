#include "virtio_blk.h"
#include "pci.h"
#include "io.h"
#include "virtio.h"
static int virtio_blk_find(virtio_blk_device_t *dev)
{
    pci_device_t pci_devs[4];
    int n = pci_find_devices(0x01, 0x00, pci_devs, 4);
    if (!n) return -1;
    for (int i = 0; i < n; i++) {
        if (pci_devs[i].vendor_id == 0x1AF4 && pci_devs[i].device_id == 0x1001) {
            dev->io_base = pci_get_bar(pci_devs[i].bus, pci_devs[i].device, pci_devs[i].func, 0) & 0xFFF0;
            return 0;
        }
    }
    return -1;
}
int virtio_blk_init(virtio_blk_device_t *dev)
{
    dev->present = 0;
    if (virtio_blk_find(dev)) return -1;
    virtio_device_t vdev;
    if (virtio_init(&vdev, dev->io_base)) return -1;
    dev->capacity = 0;
    dev->sector_size = 512;
    dev->present = 1;
    return 0;
}
int virtio_blk_read(virtio_blk_device_t *dev, uint64_t sector, void *buf)
{
    (void)dev;
    (void)sector;
    (void)buf;
    return -1;
}
int virtio_blk_write(virtio_blk_device_t *dev, uint64_t sector, const void *buf)
{
    (void)dev;
    (void)sector;
    (void)buf;
    return -1;
}
