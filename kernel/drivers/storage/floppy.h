#ifndef FLOPPY_H
#define FLOPPY_H
#include <stdint.h>
#define FLOPPY_DOR 0x3F2
#define FLOPPY_MSR 0x3F4
#define FLOPPY_FIFO 0x3F5
#define FLOPPY_CTRL 0x3F7
#define FLOPPY_CMD_SPECIFY 0x03
#define FLOPPY_CMD_READ 0xE6
#define FLOPPY_CMD_WRITE 0xC5
#define FLOPPY_CMD_RECAL 0x07
#define FLOPPY_CMD_SENSEI 0x08
#define FLOPPY_CMD_SEEK 0x0F
typedef struct {
    int present;
    int motor_on;
    uint8_t cur_track;
} floppy_device_t;
int floppy_init(floppy_device_t *dev);
int floppy_read_sector(floppy_device_t *dev, int cyl, int head, int sector, void *buf);
int floppy_write_sector(floppy_device_t *dev, int cyl, int head, int sector, const void *buf);
#endif
