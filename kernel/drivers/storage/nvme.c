#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
int nvme_init(nvme_device_t *dev)
{
    pci_device_t pci_devs[4];
    int n = pci_find_devices(0x01, 0x08, pci_devs, 4);
    if (!n) {
        n = pci_find_devices(0x01, 0x00, pci_devs, 4);
        if (!n) return -1;
    }
    uint32_t bar0 = pci_get_bar(pci_devs[0].bus, pci_devs[0].device, pci_devs[0].func, 0);
    dev->bar0 = bar0 & 0xFFFFFFF0;
    dev->present = 1;
    dev->total_sectors = 0;
    dev->sector_size = 512;
    return 0;
}
int nvme_read(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
int nvme_write(nvme_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
