#include "ata.h"
#include "io.h"
#include "string.h"
#include "terminal.h"
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
    return 0;
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
