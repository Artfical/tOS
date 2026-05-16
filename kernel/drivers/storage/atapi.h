#ifndef ATAPI_H
#define ATAPI_H
#include <stdint.h>
#define ATAPI_SIGNATURE 0xEB14
#define ATAPI_CMD_READ 0xA8
#define ATAPI_CMD_INQUIRY 0x12
#define ATAPI_CMD_CAPACITY 0x25
typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    int present;
    uint32_t block_count;
    uint32_t block_size;
} atapi_device_t;
int atapi_init(atapi_device_t *dev, uint16_t io_base);
int atapi_read_sectors(atapi_device_t *dev, uint32_t lba, uint8_t count, void *buf);
#endif
