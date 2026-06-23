#ifndef USB_MSD_H
#define USB_MSD_H
#include <stdint.h>
#define USB_MSD_CLASS 0x08
#define USB_MSD_SUBCLASS_SCSI 0x06
#define USB_MSD_PROTO_BULK 0x50
#define USB_MSD_CMD_READ10 0x28
#define USB_MSD_CMD_WRITE10 0x2A
#define USB_MSD_CMD_INQUIRY 0x12
#define USB_MSD_CMD_READ_CAPACITY 0x25
#define USB_MSD_CMD_TEST_UNIT_READY 0x00
#define USB_MSD_CBW_SIGNATURE 0x43425355
#define USB_MSD_CSW_SIGNATURE 0x53425355

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __attribute__((packed)) usb_msd_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) usb_msd_csw_t;

typedef struct {
    int present;
    int dev_addr;
    int bulk_in_ep;
    int bulk_out_ep;
    int in_max_packet;
    int out_max_packet;
    int in_toggle;
    int out_toggle;
    uint32_t tag;
    uint32_t block_count;
    uint32_t block_size;
} usb_msd_device_t;
int usb_msd_init(usb_msd_device_t *dev);
int usb_msd_read(usb_msd_device_t *dev, uint32_t lba, uint8_t count, void *buf);
int usb_msd_write(usb_msd_device_t *dev, uint32_t lba, uint8_t count, const void *buf);
#endif
