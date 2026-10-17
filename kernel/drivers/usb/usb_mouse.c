#include "usb_mouse.h"
#include "usb.h"
#include "uhci.h"
#include "hid.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"

static uhci_controller_t uhci_ctrl;
static int usb_mouse_found = 0;
static int mouse_dev_addr = 0;
static int mouse_in_endpoint = 0;
static int mouse_max_packet = 0;
static int mouse_toggle;

static hid_mouse_report_t last_report;
static hid_mouse_report_t current_report;

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

static int usb_set_protocol(uhci_controller_t *c, int dev, int proto)
{
    usb_device_request_t req;
    req.bmRequestType = 0x21;
    req.bRequest = HID_REQ_SET_PROTOCOL;
    req.wValue = proto;
    req.wIndex = 0;
    req.wLength = 0;
    return uhci_control(c, dev, 0, &req, NULL, 0x00);
}

static int usb_set_idle(uhci_controller_t *c, int dev, int duration)
{
    usb_device_request_t req;
    req.bmRequestType = 0x21;
    req.bRequest = HID_REQ_SET_IDLE;
    req.wValue = (duration << 8) | 0;
    req.wIndex = 0;
    req.wLength = 0;
    return uhci_control(c, dev, 0, &req, NULL, 0x00);
}

static int enumerate_usb_mouse(uhci_controller_t *c, int addr)
{
    usb_config_descriptor_t cfg_desc;
    if (usb_get_descriptor(c, addr, USB_CONFIG_DESCRIPTOR_TYPE, 0, &cfg_desc, 9) != 0) {
        terminal_writestring("USB mouse: Get config failed\n");
        return -1;
    }

    if (cfg_desc.wTotalLength < 9 || cfg_desc.wTotalLength > 1024) return -1;
    unsigned char *cfg_buf = (unsigned char *)malloc(cfg_desc.wTotalLength);
    if (!cfg_buf) return -1;

    if (usb_get_descriptor(c, addr, USB_CONFIG_DESCRIPTOR_TYPE, 0, cfg_buf, cfg_desc.wTotalLength) != 0) {
        free(cfg_buf);
        return -1;
    }

    int found = 0;
    unsigned int pos = 0;
    while (pos < cfg_desc.wTotalLength) {
        unsigned char *desc = cfg_buf + pos;
        if (desc[0] == 0) break;
        if (desc[1] == USB_INTERFACE_DESCRIPTOR_TYPE && desc[0] >= 9) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)desc;
            if (iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == 1 &&
                iface->bInterfaceProtocol == 2) {
                found = 1;
                mouse_dev_addr = addr;

                unsigned char *ep_pos = cfg_buf + pos + desc[0];
                while (ep_pos < cfg_buf + cfg_desc.wTotalLength) {
                    if (ep_pos[0] == 0) break;
                    if (ep_pos[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && ep_pos[0] >= 7) {
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)ep_pos;
                        if ((ep->bEndpointAddress & 0x80) &&
                            (ep->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT) {
                            mouse_in_endpoint = ep->bEndpointAddress & 0x0F;
                            mouse_max_packet = ep->wMaxPacketSize;
                            break;
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

    if (!found) {
        terminal_writestring("USB mouse: Not found\n");
        return -1;
    }

    if (usb_set_config(c, addr, 1) != 0) {
        terminal_writestring("USB mouse: Set config failed\n");
        return -1;
    }

    usb_set_protocol(c, addr, HID_BOOT_PROTOCOL);
    usb_set_idle(c, addr, 0);

    terminal_writestring("USB mouse: Ready\n");
    usb_mouse_found = 1;
    memset(&last_report, 0, sizeof(last_report));
    memset(&current_report, 0, sizeof(current_report));
    return 0;
}

int usb_mouse_read(int *dx, int *dy, uint8_t *buttons)
{
    if (!usb_mouse_found) return 0;

    hid_mouse_report_t report;
    memset(&report, 0, sizeof(report));

    /* never waits: a report is taken when the controller has one, otherwise nothing happens */
    if (uhci_interrupt_poll(&uhci_ctrl, 0, mouse_dev_addr, mouse_in_endpoint,
                            mouse_max_packet > 8 ? 8 : mouse_max_packet, &report, &mouse_toggle) != 1)
        return 0;

    if (report.x == 0 && report.y == 0 && report.buttons == last_report.buttons)
        return 0;

    if (dx) *dx = report.x;
    if (dy) *dy = report.y;
    if (buttons) *buttons = report.buttons;

    last_report = report;
    return 1;
}

int usb_mouse_available(void)
{
    return usb_mouse_found;
}

void usb_mouse_init(void)
{
    if (uhci_init(&uhci_ctrl) != 0) {
        terminal_writestring("USB mouse: No UHCI controller\n");
        return;
    }

    for (int p = 0; p < 2; p++) {
        int addr = uhci_port_attach(&uhci_ctrl, p);
        if (addr) {
            if (enumerate_usb_mouse(&uhci_ctrl, addr) == 0)
                break;
        }
    }
}
