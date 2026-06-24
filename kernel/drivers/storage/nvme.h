#ifndef NVME_H
#define NVME_H
#include <stdint.h>
#define NVME_MAX_QUEUE_ENTRIES 64
#define NVME_CMD_READ  0x02
#define NVME_CMD_WRITE 0x01
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06

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
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cmd_id;
    uint16_t status;
} __attribute__((packed)) nvme_completion_t;

typedef struct {
    uint32_t bar0;
    volatile uint8_t *regs;
    int present;
    uint64_t total_sectors;
    uint32_t sector_size;
    uint32_t doorbell_stride;

    nvme_command_t *asq;
    volatile nvme_completion_t *acq;
    uint16_t asq_tail;
    uint16_t acq_head;
    uint8_t  acq_phase;

    nvme_command_t *iosq;
    volatile nvme_completion_t *iocq;
    uint16_t iosq_tail;
    uint16_t iocq_head;
    uint8_t  iocq_phase;

    uint8_t *prp_list;
    uint16_t next_cmd_id;
} nvme_device_t;

int nvme_init(nvme_device_t *dev);
int nvme_read(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int nvme_write(nvme_device_t *dev, uint64_t lba, uint32_t count, const void *buf);
#endif
