#ifndef PARALLEL_H
#define PARALLEL_H
#include <stdint.h>
#define PARALLEL_DATA 0x00
#define PARALLEL_STATUS 0x01
#define PARALLEL_CONTROL 0x02
#define PARALLEL_LPT1 0x378
#define PARALLEL_LPT2 0x278
#define PARALLEL_LPT3 0x3BC
#define PARALLEL_STATUS_BUSY 0x80
#define PARALLEL_STATUS_ACK 0x40
#define PARALLEL_STATUS_PAPER 0x20
#define PARALLEL_STATUS_SELECT 0x10
#define PARALLEL_STATUS_ERROR 0x08
#define PARALLEL_CTRL_STROBE 0x01
#define PARALLEL_CTRL_AUTO 0x02
#define PARALLEL_CTRL_INIT 0x04
#define PARALLEL_CTRL_SELECT 0x08
#define PARALLEL_CTRL_IRQ 0x10
typedef struct {
    int present;
    uint16_t io_base;
} parallel_device_t;
int parallel_init(parallel_device_t *dev);
int parallel_write(parallel_device_t *dev, uint8_t data);
uint8_t parallel_read(parallel_device_t *dev);
#endif
