#ifndef ATA_H
#define ATA_H
#include <stdint.h>
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_SECTORS 2
#define ATA_REG_LBA_LOW 3
#define ATA_REG_LBA_MID 4
#define ATA_REG_LBA_HIGH 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_CMD 7
#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH 0xE7
#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_BSY 0x80
typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    int present;
    int lba48;
    int sectors_28;
    uint64_t sectors_48;
    char model[41];
} ata_device_t;
#define ATA_MAX_DEVICES 4

extern ata_device_t ata_devices[ATA_MAX_DEVICES];
extern int ata_device_count;

int ata_init(void);
int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint8_t count, void *buf);
int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint8_t count, const void *buf);
int ata_identify(ata_device_t *dev, int is_slave);
#endif
