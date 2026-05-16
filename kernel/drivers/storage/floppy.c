#include "floppy.h"
#include "io.h"
static int floppy_wait(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(FLOPPY_MSR) & 0x80) return 0;
    return -1;
}
static void floppy_write(uint8_t val)
{
    for (int i = 0; i < 100000; i++)
        if (inb(FLOPPY_MSR) & 0x40) break;
    outb(FLOPPY_FIFO, val);
}
static uint8_t floppy_read(void)
{
    floppy_wait();
    return inb(FLOPPY_FIFO);
}
int floppy_init(floppy_device_t *dev)
{
    dev->present = 0;
    dev->motor_on = 0;
    dev->cur_track = 0xFF;
    outb(FLOPPY_DOR, 0x00);
    outb(FLOPPY_DOR, 0x0C);
    floppy_write(FLOPPY_CMD_SENSEI);
    floppy_read();
    floppy_read();
    floppy_write(FLOPPY_CMD_SPECIFY);
    floppy_write(0xDF);
    floppy_write(0x02);
    floppy_write(FLOPPY_CMD_RECAL);
    floppy_write(0);
    for (int i = 0; i < 2000000; i++);
    floppy_write(FLOPPY_CMD_SENSEI);
    floppy_read();
    uint8_t track = floppy_read();
    if (track == 0) dev->present = 1;
    return dev->present ? 0 : -1;
}
int floppy_read_sector(floppy_device_t *dev, int cyl, int head, int sector, void *buf)
{
    (void)dev;
    (void)cyl;
    (void)head;
    (void)sector;
    (void)buf;
    return -1;
}
int floppy_write_sector(floppy_device_t *dev, int cyl, int head, int sector, const void *buf)
{
    (void)dev;
    (void)cyl;
    (void)head;
    (void)sector;
    (void)buf;
    return -1;
}
