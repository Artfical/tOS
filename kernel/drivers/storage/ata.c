#include "ata.h"
#include "io.h"
#include "string.h"
#include "terminal.h"
#include "serial.h"
ata_device_t ata_devices[ATA_MAX_DEVICES];
int ata_device_count = 0;

#define ATA_LBA28_LIMIT 0x10000000ULL
#define ATA_MAX_PER_CMD 256

static void ata_delay400(ata_device_t *dev)
{
    for (int i = 0; i < 4; i++) inb(dev->ctrl_base);
}

static int ata_wait(ata_device_t *dev, int timeout)
{
    (void)timeout;
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (!(s & ATA_STATUS_BSY)) return 0;
    }
    serial_write("ata_wait: TIMEOUT\n");
    return -1;
}

/* Waits for BSY to clear; fails on ERR/DF. With need_drq, also waits for DRQ. */
static int ata_poll(ata_device_t *dev, int need_drq)
{
    ata_delay400(dev);
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(dev->io_base + ATA_REG_STATUS);
        if (s & ATA_STATUS_BSY) continue;
        if (s & (ATA_STATUS_ERR | ATA_STATUS_DF)) return -1;
        if (!need_drq || (s & ATA_STATUS_DRQ)) return 0;
    }
    return -1;
}

static void ata_hex(char *b, int *n, uint64_t v, int digits)
{
    const char *hex = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) b[(*n)++] = hex[(v >> (i * 4)) & 0xF];
}

static void ata_report(const char *what, ata_device_t *dev, uint64_t lba, uint32_t count)
{
    char b[96]; int n = 0;
    while (*what && n < 20) b[n++] = *what++;
    const char *p = " ERR status=";
    while (*p) b[n++] = *p++;
    ata_hex(b, &n, inb(dev->io_base + ATA_REG_STATUS), 2);
    p = " error=";
    while (*p) b[n++] = *p++;
    ata_hex(b, &n, inb(dev->io_base + ATA_REG_ERROR), 2);
    p = " lba=";
    while (*p) b[n++] = *p++;
    ata_hex(b, &n, lba, 10);
    p = " count=";
    while (*p) b[n++] = *p++;
    ata_hex(b, &n, count, 4);
    b[n++] = '\n'; b[n] = 0;
    serial_write(b);
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
    dev->slave = is_slave ? 1 : 0;
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
            dev->slave = slave;

            if (ata_identify(dev, slave) == 0) {
                outb(dev->ctrl_base, 0x02); /* nIEN: polled PIO only, no IRQ14/15 */
                ata_device_count++;
            }
        }
    }
    return ata_device_count;
}

static uint64_t ata_capacity(ata_device_t *dev)
{
    return dev->lba48 ? dev->sectors_48 : (uint64_t)(uint32_t)dev->sectors_28;
}

/* Issues one READ/WRITE command for 1..256 sectors and leaves the device at
 * the first DRQ-ready sector. Chooses LBA28 or LBA48 by address range. */
static int ata_issue(ata_device_t *dev, uint64_t lba, uint32_t count, int write)
{
    int ext = dev->lba48 && (lba + count > ATA_LBA28_LIMIT);
    if (!ext && lba + count > ATA_LBA28_LIMIT) return -1;

    if (ata_wait(dev, 10000)) return -1;
    outb(dev->io_base + ATA_REG_DRIVE, 0xE0 | (dev->slave ? 0x10 : 0) | (ext ? 0 : (uint8_t)((lba >> 24) & 0x0F)));
    ata_delay400(dev);
    if (ata_wait(dev, 10000)) return -1;

    if (ext) {
        outb(dev->io_base + ATA_REG_SECTORS, (uint8_t)(count >> 8));
        outb(dev->io_base + ATA_REG_LBA_LOW, (uint8_t)(lba >> 24));
        outb(dev->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 32));
        outb(dev->io_base + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));
    }
    outb(dev->io_base + ATA_REG_SECTORS, (uint8_t)count); /* 256 encodes as 0 */
    outb(dev->io_base + ATA_REG_LBA_LOW, (uint8_t)lba);
    outb(dev->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(dev->io_base + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    uint8_t cmd = write ? (ext ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO)
                        : (ext ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO);
    outb(dev->io_base + ATA_REG_CMD, cmd);
    return ext;
}

int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->present || count == 0) return -1;
    if (lba + count > ata_capacity(dev)) return -1;
    uint8_t *out = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > ATA_MAX_PER_CMD ? ATA_MAX_PER_CMD : count;
        if (ata_issue(dev, lba, n, 0) < 0) { ata_report("ata_read", dev, lba, n); return -1; }
        uint16_t *ptr = (uint16_t *)out;
        for (uint32_t s = 0; s < n; s++) {
            if (ata_poll(dev, 1)) { ata_report("ata_read", dev, lba + s, n); return -1; }
            for (int i = 0; i < 256; i++)
                ptr[s * 256 + i] = inw(dev->io_base + ATA_REG_DATA);
        }
        lba += n; out += n * 512; count -= n;
    }
    return 0;
}

int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->present || count == 0) return -1;
    if (lba + count > ata_capacity(dev)) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > ATA_MAX_PER_CMD ? ATA_MAX_PER_CMD : count;
        int ext = ata_issue(dev, lba, n, 1);
        if (ext < 0) { ata_report("ata_write", dev, lba, n); return -1; }
        const uint16_t *ptr = (const uint16_t *)in;
        for (uint32_t s = 0; s < n; s++) {
            if (ata_poll(dev, 1)) { ata_report("ata_write", dev, lba + s, n); return -1; }
            for (int i = 0; i < 256; i++)
                outw(dev->io_base + ATA_REG_DATA, ptr[s * 256 + i]);
        }
        if (ata_poll(dev, 0)) { ata_report("ata_write", dev, lba, n); return -1; }
        outb(dev->io_base + ATA_REG_CMD, dev->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH);
        if (ata_poll(dev, 0)) { ata_report("ata_flush", dev, lba, n); return -1; }
        lba += n; in += n * 512; count -= n;
    }
    return 0;
}
