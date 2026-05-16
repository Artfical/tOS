#include "atapi.h"
#include "io.h"
#include "string.h"
#include "ata.h"
static int atapi_packet_send(atapi_device_t *dev, uint8_t *cmd, int cmd_len, void *buf, int buf_len, int write)
{
    (void)write;
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (!(s & ATA_STATUS_BSY)) break;
    }
    outb(dev->io_base + ATA_REG_DRIVE, 0xA0);
    for (int i = 0; i < 10000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (s & ATA_STATUS_DRQ) break;
        if (s & ATA_STATUS_ERR) return -1;
    }
    for (int i = 0; i < cmd_len; i += 2)
        outw(dev->io_base + ATA_REG_DATA, *(uint16_t *)(cmd + i));
    if (!buf || !buf_len) return 0;
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (s & ATA_STATUS_DRQ) break;
        if (s & ATA_STATUS_ERR) return -1;
    }
    uint16_t *ptr = (uint16_t *)buf;
    for (int i = 0; i < buf_len / 2; i++)
        ptr[i] = inw(dev->io_base + ATA_REG_DATA);
    return 0;
}
int atapi_init(atapi_device_t *dev, uint16_t io_base)
{
    memset(dev, 0, sizeof(*dev));
    dev->io_base = io_base;
    dev->ctrl_base = io_base == ATA_PRIMARY_IO ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    outb(dev->io_base + ATA_REG_DRIVE, 0xA0);
    uint8_t mid = inb(dev->io_base + ATA_REG_LBA_MID);
    uint8_t high = inb(dev->io_base + ATA_REG_LBA_HIGH);
    if (mid == 0xEB && high == 0x14) {
        dev->present = 1;
        uint8_t inquiry[36];
        uint8_t cmd[12] = {ATAPI_CMD_INQUIRY, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0};
        atapi_packet_send(dev, cmd, 12, inquiry, 36, 0);
    }
    return dev->present ? 0 : -1;
}
int atapi_read_sectors(atapi_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    uint8_t cmd[12] = {ATAPI_CMD_READ, 0, (lba >> 24) & 0xFF, (lba >> 16) & 0xFF, (lba >> 8) & 0xFF, lba & 0xFF, 0, 0, 0, count, 0, 0};
    return atapi_packet_send(dev, cmd, 12, buf, count * 2048, 0);
}
