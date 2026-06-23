#include "blockdev.h"
#include "memory.h"
#include "string.h"

#define BLOCKDEV_CHUNK 128

static blockdev_t devices[BLOCKDEV_MAX];
static int dev_count = 0;

void blockdev_init(void)
{
    memset(devices, 0, sizeof(devices));
    dev_count = 0;
}

static blockdev_t *alloc_slot(void)
{
    if (dev_count >= BLOCKDEV_MAX) return 0;
    blockdev_t *bd = &devices[dev_count++];
    memset(bd, 0, sizeof(*bd));
    bd->used = 1;
    return bd;
}

static void set_name(blockdev_t *bd, const char *prefix, int idx)
{
    int i = 0;
    while (prefix[i] && i < BLOCKDEV_NAME_LEN - 4) { bd->name[i] = prefix[i]; i++; }
    bd->name[i++] = '0' + (idx % 10);
    bd->name[i] = 0;
}

int blockdev_register_ata(ata_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_ATA;
    set_name(bd, "ata", dev_count - 1);
    bd->sector_size = 512;
    bd->total_sectors = dev->lba48 ? dev->sectors_48 : (uint64_t)dev->sectors_28;
    bd->driver_data = dev;
    return 0;
}

int blockdev_register_ahci(ahci_hba_t *hba, int port, uint64_t sectors)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_AHCI;
    set_name(bd, "ahci", port);
    bd->sector_size = 512;
    bd->total_sectors = sectors;
    bd->driver_data = hba;
    bd->port = port;
    return 0;
}

int blockdev_register_nvme(nvme_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_NVME;
    set_name(bd, "nvme", dev_count - 1);
    bd->sector_size = dev->sector_size ? dev->sector_size : 512;
    bd->total_sectors = dev->total_sectors;
    bd->driver_data = dev;
    return 0;
}

int blockdev_register_usb(usb_msd_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_USB_MSD;
    set_name(bd, "usb", dev_count - 1);
    bd->sector_size = dev->block_size ? dev->block_size : 512;
    bd->total_sectors = dev->block_count;
    bd->driver_data = dev;
    return 0;
}

int blockdev_count(void) { return dev_count; }

blockdev_t *blockdev_get(int index)
{
    if (index < 0 || index >= dev_count || !devices[index].used) return 0;
    return &devices[index];
}

blockdev_t *blockdev_find(const char *name)
{
    for (int i = 0; i < dev_count; i++) {
        if (devices[i].used && strcmp(devices[i].name, name) == 0) return &devices[i];
    }
    return 0;
}

static int raw_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf)
{
    switch (bd->type) {
    case BLOCKDEV_ATA:
        return ata_read_sectors((ata_device_t *)bd->driver_data, lba, (uint8_t)count, buf);
    case BLOCKDEV_AHCI:
        return ahci_read((ahci_hba_t *)bd->driver_data, bd->port, lba, (int)count, buf);
    case BLOCKDEV_NVME:
        return nvme_read((nvme_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_USB_MSD:
        return usb_msd_read((usb_msd_device_t *)bd->driver_data, (uint32_t)lba, (uint8_t)count, buf);
    default:
        return -1;
    }
}

static int raw_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf)
{
    switch (bd->type) {
    case BLOCKDEV_ATA:
        return ata_write_sectors((ata_device_t *)bd->driver_data, lba, (uint8_t)count, buf);
    case BLOCKDEV_AHCI:
        return ahci_write((ahci_hba_t *)bd->driver_data, bd->port, lba, (int)count, buf);
    case BLOCKDEV_NVME:
        return nvme_write((nvme_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_USB_MSD:
        return usb_msd_write((usb_msd_device_t *)bd->driver_data, (uint32_t)lba, (uint8_t)count, buf);
    default:
        return -1;
    }
}

int blockdev_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf)
{
    if (!bd || !bd->used) return -1;
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > BLOCKDEV_CHUNK ? BLOCKDEV_CHUNK : count;
        if (raw_read(bd, lba, chunk, p) != 0) return -1;
        lba += chunk;
        p += (uint32_t)chunk * bd->sector_size;
        count -= chunk;
    }
    return 0;
}

int blockdev_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf)
{
    if (!bd || !bd->used) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > BLOCKDEV_CHUNK ? BLOCKDEV_CHUNK : count;
        if (raw_write(bd, lba, chunk, p) != 0) return -1;
        lba += chunk;
        p += (uint32_t)chunk * bd->sector_size;
        count -= chunk;
    }
    return 0;
}

int blockdev_read_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, void *buf)
{
    if (!bd || !bd->used || len == 0) return -1;
    uint32_t ss = bd->sector_size;
    uint64_t first_lba = byte_offset / ss;
    uint32_t skip = (uint32_t)(byte_offset % ss);
    uint32_t total_bytes = skip + len;
    uint32_t sectors = (total_bytes + ss - 1) / ss;

    uint8_t *tmp = (uint8_t *)malloc(sectors * ss);
    if (!tmp) return -1;
    if (blockdev_read(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    memcpy(buf, tmp + skip, len);
    free(tmp);
    return 0;
}

int blockdev_write_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, const void *buf)
{
    if (!bd || !bd->used || len == 0) return -1;
    uint32_t ss = bd->sector_size;
    uint64_t first_lba = byte_offset / ss;
    uint32_t skip = (uint32_t)(byte_offset % ss);
    uint32_t total_bytes = skip + len;
    uint32_t sectors = (total_bytes + ss - 1) / ss;

    uint8_t *tmp = (uint8_t *)malloc(sectors * ss);
    if (!tmp) return -1;

    if (skip != 0 || total_bytes % ss != 0) {
        if (blockdev_read(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    }
    memcpy(tmp + skip, buf, len);
    if (blockdev_write(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    free(tmp);
    return 0;
}
