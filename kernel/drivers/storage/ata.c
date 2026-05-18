#include "ata.h"
#include "io.h"
#include "string.h"
#include "terminal.h"
ata_device_t ata_devices[ATA_MAX_DEVICES];
int ata_device_count = 0;

static int ata_wait(ata_device_t *dev, int timeout)
{
    (void)timeout;
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (!(s & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}
int ata_identify(ata_device_t *dev, int is_slave)
{
    outb(dev->io_base + ATA_REG_DRIVE, is_slave ? 0xB0 : 0xA0);
    outb(dev->io_base + ATA_REG_SECTORS, 0);
    outb(dev->io_base + ATA_REG_LBA_LOW, 0);
    outb(dev->io_base + ATA_REG_LBA_MID, 0);
    outb(dev->io_base + ATA_REG_LBA_HIGH, 0);
    outb(dev->io_base + ATA_REG_CMD, ATA_CMD_IDENTIFY);
    if (inb(dev->io_base + ATA_REG_STATUS) == 0) return -1;
    if (ata_wait(dev, 10000)) return -1;
    uint16_t buf[256];
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 256; i++)
        buf[i] = inw(dev->io_base + ATA_REG_DATA);
    if (buf[0] == 0 || buf[0] == 0xFFFF) return -1;
    dev->present = 1;
    memcpy(&dev->sectors_28, &buf[60], 4);
    dev->lba48 = (buf[83] & 0x400) ? 1 : 0;
    if (dev->lba48) memcpy(&dev->sectors_48, &buf[100], 8);
    for (int i = 0; i < 40; i += 2) {
        dev->model[i] = buf[27 + i / 2] >> 8;
        dev->model[i + 1] = buf[27 + i / 2] & 0xFF;
    }
    dev->model[40] = 0;
    return 0;
}
int ata_init(void)
{
    int bases[2] = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
    int ctrls[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };
    ata_device_count = 0;

    for (int bus = 0; bus < 2 && ata_device_count < ATA_MAX_DEVICES; bus++) {
        for (int slave = 0; slave < 2 && ata_device_count < ATA_MAX_DEVICES; slave++) {
            uint16_t io = bases[bus];
            outb(io + ATA_REG_DRIVE, slave ? 0xB0 : 0xA0);
            io_wait();
            uint8_t st = inb(io + ATA_REG_STATUS);
            if (st == 0 || st == 0xFF) continue;

            ata_device_t *dev = &ata_devices[ata_device_count];
            dev->io_base = io;
            dev->ctrl_base = ctrls[bus];
            dev->present = 0;

            if (ata_identify(dev, slave) == 0) {
                ata_device_count++;
            }
        }
    }
    return ata_device_count;
}
int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint8_t count, void *buf)
{
    if (ata_wait(dev, 10000)) return -1;
    outb(dev->io_base + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(dev->io_base + ATA_REG_SECTORS, count);
    outb(dev->io_base + ATA_REG_LBA_LOW, lba & 0xFF);
    outb(dev->io_base + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(dev->io_base + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(dev->io_base + ATA_REG_CMD, ATA_CMD_READ_PIO);
    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait(dev, 10000)) return -1;
        uint8_t st = inb(dev->io_base + ATA_REG_STATUS);
        if (st & ATA_STATUS_ERR) return -1;
        for (int i = 0; i < 256; i++)
            ptr[s * 256 + i] = inw(dev->io_base + ATA_REG_DATA);
    }
    return 0;
}
int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint8_t count, const void *buf)
{
    if (ata_wait(dev, 10000)) return -1;
    outb(dev->io_base + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(dev->io_base + ATA_REG_SECTORS, count);
    outb(dev->io_base + ATA_REG_LBA_LOW, lba & 0xFF);
    outb(dev->io_base + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(dev->io_base + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(dev->io_base + ATA_REG_CMD, ATA_CMD_WRITE_PIO);
    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait(dev, 10000)) return -1;
        uint8_t st = inb(dev->io_base + ATA_REG_STATUS);
        if (st & ATA_STATUS_ERR) return -1;
        for (int i = 0; i < 256; i++)
            outw(dev->io_base + ATA_REG_DATA, ptr[s * 256 + i]);
    }
    outb(dev->io_base + ATA_REG_CMD, ATA_CMD_FLUSH);
    return 0;
}
