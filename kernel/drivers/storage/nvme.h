#ifndef NVME_H
#define NVME_H
#include <stdint.h>
#define NVME_MAX_QUEUE_ENTRIES 64
#define NVME_CMD_READ 0x02
#define NVME_CMD_WRITE 0x01
typedef struct {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;
    uint32_t dword4;
    uint32_t dword5;
    uint32_t dword6;
    uint32_t dword7;
    uint32_t dword8;
    uint32_t dword9;
    uint32_t dword10;
    uint32_t dword11;
    uint32_t dword12;
    uint32_t dword13;
    uint32_t dword14;
    uint32_t dword15;
} __attribute__((packed)) nvme_command_t;
typedef struct {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t resv;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cmd_id;
    uint16_t status;
} __attribute__((packed)) nvme_completion_t;
typedef struct {
    uint32_t bar0;
    int present;
    uint64_t total_sectors;
    uint32_t sector_size;
} nvme_device_t;
int nvme_init(nvme_device_t *dev);
int nvme_read(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int nvme_write(nvme_device_t *dev, uint64_t lba, uint32_t count, const void *buf);
#endif
