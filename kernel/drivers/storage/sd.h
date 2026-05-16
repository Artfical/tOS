#ifndef SD_H
#define SD_H
#include <stdint.h>
#define SD_CMD_GO_IDLE 0
#define SD_CMD_SEND_IF_COND 8
#define SD_CMD_SEND_CSD 9
#define SD_CMD_SEND_CID 10
#define SD_CMD_SEND_OP_COND 41
#define SD_CMD_READ_SINGLE 17
#define SD_CMD_READ_MULTI 18
#define SD_CMD_WRITE_SINGLE 24
#define SD_CMD_WRITE_MULTI 25
#define SD_CMD_APP_CMD 55
#define SD_CMD_READ_OCR 58
typedef struct {
    int present;
    int sdhc;
    uint64_t capacity;
    uint32_t rca;
} sd_device_t;
int sd_init(sd_device_t *dev);
int sd_read_block(sd_device_t *dev, uint32_t lba, void *buf);
int sd_write_block(sd_device_t *dev, uint32_t lba, const void *buf);
#endif
