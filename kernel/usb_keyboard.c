#include "usb.h"
#include "uhci.h"
#include "hid.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"

static uhci_controller_t uhci_ctrl;
static int usb_available = 0;
static int usb_keyboard_found = 0;
static int keyboard_dev_addr = 0;
static int keyboard_in_endpoint = 0;
static int keyboard_max_packet = 0;

static const char usb_kc_ascii[128] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\n', 27, '\b', '\t',
    ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0
};

static const char usb_kc_shift[128] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 27, '\b', '\t',
    ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0
};

char usb_keycode_to_ascii(uint8_t keycode, uint8_t modifiers)
{
    if (keycode >= 128) return 0;
    if (modifiers & (USB_MOD_LSHIFT | USB_MOD_RSHIFT)) {
        char c = usb_kc_shift[keycode];
        if (c) return c;
    }
    return usb_kc_ascii[keycode];
}

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

static int enumerate_usb_keyboard(uhci_controller_t *c)
{
    usb_device_descriptor_t dev_desc;
    if (usb_get_descriptor(c, 0, USB_DEVICE_DESCRIPTOR_TYPE, 0, &dev_desc, 8) != 0) {
        terminal_writestring("USB: No device on port\n");
        return -1;
    }

    terminal_writestring("USB: Device detected\n");

    usb_set_address(c, 1);

    dev_desc.bMaxPacketSize0 = 8;

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

    int found_keyboard = 0;
    unsigned int pos = 0;
    while (pos < cfg_desc.wTotalLength) {
        unsigned char *desc = cfg_buf + pos;
        if (desc[0] == 0) break;
        if (desc[1] == USB_INTERFACE_DESCRIPTOR_TYPE && desc[0] >= 9) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)desc;
            if (iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == 1) {
                found_keyboard = 1;
                keyboard_dev_addr = 1;

                unsigned char *ep_pos = cfg_buf + pos + desc[0];
                while (ep_pos < cfg_buf + cfg_desc.wTotalLength) {
                    if (ep_pos[0] == 0) break;
                    if (ep_pos[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && ep_pos[0] >= 7) {
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)ep_pos;
                        if ((ep->bEndpointAddress & 0x80) &&
                            (ep->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT) {
                            keyboard_in_endpoint = ep->bEndpointAddress & 0x0F;
                            keyboard_max_packet = ep->wMaxPacketSize;
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

    if (!found_keyboard) {
        terminal_writestring("USB: No keyboard found\n");
        return -1;
    }

    if (usb_set_config(c, 1, 1) != 0) {
        terminal_writestring("USB: Set config failed\n");
        return -1;
    }

    usb_set_protocol(c, 1, HID_BOOT_PROTOCOL);
    usb_set_idle(c, 1, 0);

    terminal_writestring("USB: Keyboard ready\n");
    usb_keyboard_found = 1;
    return 0;
}

static hid_keyboard_report_t last_report;
static hid_keyboard_report_t current_report;

int usb_keyboard_read(char *c)
{
    if (!usb_keyboard_found) return 0;

    if (uhci_interrupt_read(&uhci_ctrl, keyboard_dev_addr,
                            keyboard_in_endpoint, keyboard_max_packet,
                            &current_report) != 0)
        return 0;

    for (int i = 0; i < 6; i++) {
        if (current_report.keys[i] && current_report.keys[i] != last_report.keys[i]) {
            uint8_t mods = current_report.modifiers;
            char ascii = usb_keycode_to_ascii(current_report.keys[i], mods);
            if (ascii) {
                last_report = current_report;
                *c = ascii;
                return 1;
            }
        }
    }

    last_report = current_report;
    return 0;
}

void usb_keyboard_init(void)
{
    if (uhci_init(&uhci_ctrl) != 0) {
        terminal_writestring("USB: No UHCI controller\n");
        usb_available = 0;
        return;
    }
    usb_available = 1;

    for (int p = 0; p < 2; p++) {
        if (uhci_port_detect(&uhci_ctrl, p)) {
            if (enumerate_usb_keyboard(&uhci_ctrl) == 0)
                break;
        }
    }
}

int usb_keyboard_available(void)
{
    return usb_keyboard_found;
}
