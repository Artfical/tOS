#include "usb_msd.h"
#include "usb.h"
#include "uhci.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"

static uhci_controller_t uhci_ctrl;

static int usb_get_descriptor(uhci_controller_t *c, int dev, int type, int index, void *buf, int len)
{
    usb_device_request_t req;
    req.bmRequestType = 0x80;
    req.bRequest = USB_REQ_GET_DESCRIPTOR;
    req.wValue = (type << 8) | index;
    req.wIndex = 0;
    req.wLength = len;
    return uhci_control(c, dev, 0, &req, buf, 0x80);
}

static int usb_set_address(uhci_controller_t *c, int addr)
{
    usb_device_request_t req;
    req.bmRequestType = 0x00;
    req.bRequest = USB_REQ_SET_ADDRESS;
    req.wValue = addr;
    req.wIndex = 0;
    req.wLength = 0;
    return uhci_control(c, 0, 0, &req, NULL, 0x00);
}

static int usb_set_config(uhci_controller_t *c, int dev, int config)
{
    usb_device_request_t req;
    req.bmRequestType = 0x00;
    req.bRequest = USB_REQ_SET_CONFIGURATION;
    req.wValue = config;
    req.wIndex = 0;
    req.wLength = 0;
    return uhci_control(c, dev, 0, &req, NULL, 0x00);
}

static int enumerate_usb_msd(uhci_controller_t *c, usb_msd_device_t *dev)
{
    usb_device_descriptor_t dev_desc;
    if (usb_get_descriptor(c, 0, USB_DEVICE_DESCRIPTOR_TYPE, 0, &dev_desc, 8) != 0) {
        terminal_writestring("USB: No device on port\n");
        return -1;
    }

    usb_set_address(c, 1);

    usb_config_descriptor_t cfg_desc;
    if (usb_get_descriptor(c, 1, USB_CONFIG_DESCRIPTOR_TYPE, 0, &cfg_desc, 9) != 0) {
        terminal_writestring("USB: Get config failed\n");
        return -1;
    }

    unsigned char *cfg_buf = (unsigned char *)malloc(cfg_desc.wTotalLength);
    if (!cfg_buf) return -1;

    if (usb_get_descriptor(c, 1, USB_CONFIG_DESCRIPTOR_TYPE, 0, cfg_buf, cfg_desc.wTotalLength) != 0) {
        free(cfg_buf);
        return -1;
    }

    int found_msd = 0;
    unsigned int pos = 0;
    while (pos < cfg_desc.wTotalLength) {
        unsigned char *desc = cfg_buf + pos;
        if (desc[0] == 0) break;
        if (desc[1] == USB_INTERFACE_DESCRIPTOR_TYPE && desc[0] >= 9) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)desc;
            if (iface->bInterfaceClass == USB_MSD_CLASS &&
                iface->bInterfaceSubClass == USB_MSD_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_MSD_PROTO_BULK) {
                found_msd = 1;
                dev->dev_addr = 1;

                unsigned char *ep_pos = cfg_buf + pos + desc[0];
                while (ep_pos < cfg_buf + cfg_desc.wTotalLength) {
                    if (ep_pos[0] == 0) break;
                    if (ep_pos[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && ep_pos[0] >= 7) {
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)ep_pos;
                        if ((ep->bmAttributes & 3) == USB_EP_TYPE_BULK) {
                            if (ep->bEndpointAddress & 0x80) {
                                dev->bulk_in_ep = ep->bEndpointAddress & 0x0F;
                                dev->in_max_packet = ep->wMaxPacketSize;
                            } else {
                                dev->bulk_out_ep = ep->bEndpointAddress & 0x0F;
                                dev->out_max_packet = ep->wMaxPacketSize;
                            }
                        }
                    }
                    ep_pos += ep_pos[0];
                }
                break;
            }
        }
        pos += desc[0];
    }

    free(cfg_buf);

    if (!found_msd || !dev->bulk_in_ep || !dev->bulk_out_ep) {
        terminal_writestring("USB: No mass storage device found\n");
        return -1;
    }

    if (usb_set_config(c, 1, 1) != 0) {
        terminal_writestring("USB: Set config failed\n");
        return -1;
    }

    terminal_writestring("USB: Mass storage device ready\n");
    return 0;
}

static int usb_msd_send_cmd(usb_msd_device_t *dev, const uint8_t *cb, int cb_len,
                             void *data, uint32_t data_len, int data_in)
{
    usb_msd_cbw_t cbw __attribute__((aligned(16)));
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSD_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_len = data_len;
    cbw.flags = data_in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_len = (uint8_t)cb_len;
    memcpy(cbw.cb, cb, cb_len);

    if (uhci_bulk_transfer(&uhci_ctrl, dev->dev_addr, dev->bulk_out_ep, 0,
                            &cbw, sizeof(cbw), dev->out_max_packet, &dev->out_toggle) != 0)
        return -1;

    if (data_len > 0) {
        int dir = data_in ? 1 : 0;
        int ep = data_in ? dev->bulk_in_ep : dev->bulk_out_ep;
        int mp = data_in ? dev->in_max_packet : dev->out_max_packet;
        int *toggle = data_in ? &dev->in_toggle : &dev->out_toggle;
        if (uhci_bulk_transfer(&uhci_ctrl, dev->dev_addr, ep, dir, data, (int)data_len, mp, toggle) != 0)
            return -1;
    }

    usb_msd_csw_t csw __attribute__((aligned(16)));
    memset(&csw, 0, sizeof(csw));
    if (uhci_bulk_transfer(&uhci_ctrl, dev->dev_addr, dev->bulk_in_ep, 1,
                            &csw, sizeof(csw), dev->in_max_packet, &dev->in_toggle) != 0)
        return -1;

    if (csw.signature != USB_MSD_CSW_SIGNATURE) return -1;
    if (csw.status != 0) return -1;

    return 0;
}

static int usb_msd_read_capacity(usb_msd_device_t *dev)
{
    uint8_t cb[16];
    memset(cb, 0, sizeof(cb));
    cb[0] = USB_MSD_CMD_READ_CAPACITY;

    uint8_t resp[8];
    if (usb_msd_send_cmd(dev, cb, 10, resp, 8, 1) != 0) return -1;

    uint32_t last_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                         ((uint32_t)resp[2] << 8) | resp[3];
    uint32_t block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                           ((uint32_t)resp[6] << 8) | resp[7];

    dev->block_count = last_lba + 1;
    dev->block_size = block_size ? block_size : 512;
    return 0;
}

int usb_msd_init(usb_msd_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->block_size = 512;

    if (uhci_init(&uhci_ctrl) != 0) {
        terminal_writestring("USB: No UHCI controller\n");
        return -1;
    }

    for (int p = 0; p < 2; p++) {
        if (uhci_port_detect(&uhci_ctrl, p)) {
            if (enumerate_usb_msd(&uhci_ctrl, dev) == 0) {
                uint8_t cb[16];
                memset(cb, 0, sizeof(cb));
                cb[0] = USB_MSD_CMD_TEST_UNIT_READY;
                usb_msd_send_cmd(dev, cb, 6, NULL, 0, 0);

                if (usb_msd_read_capacity(dev) == 0) {
                    dev->present = 1;
                    return 0;
                }
            }
        }
    }

    return -1;
}

int usb_msd_read(usb_msd_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    if (!dev->present) return -1;

    uint8_t cb[16];
    memset(cb, 0, sizeof(cb));
    cb[0] = USB_MSD_CMD_READ10;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = 0;
    cb[8] = count;

    return usb_msd_send_cmd(dev, cb, 10, buf, (uint32_t)count * dev->block_size, 1);
}

int usb_msd_write(usb_msd_device_t *dev, uint32_t lba, uint8_t count, const void *buf)
{
    if (!dev->present) return -1;

    uint8_t cb[16];
    memset(cb, 0, sizeof(cb));
    cb[0] = USB_MSD_CMD_WRITE10;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = 0;
    cb[8] = count;

    return usb_msd_send_cmd(dev, cb, 10, (void *)buf, (uint32_t)count * dev->block_size, 0);
}
