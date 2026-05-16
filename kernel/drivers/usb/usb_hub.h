#ifndef USB_HUB_H
#define USB_HUB_H
#include <stdint.h>
#include "usb.h"
#define USB_HUB_DESCRIPTOR_TYPE 0x29
#define USB_HUB_FEATURE_PORT_RESET 4
#define USB_HUB_FEATURE_PORT_POWER 8
#define USB_HUB_STATUS_PORT 0x03
#define USB_HUB_PORT_CONNECTION 0
#define USB_HUB_PORT_ENABLE 1
#define USB_HUB_PORT_SUSPEND 2
#define USB_HUB_PORT_OVER_CURRENT 3
#define USB_HUB_PORT_RESET 4
#define USB_HUB_PORT_POWER 8
#define USB_HUB_TT_TIME 9
typedef struct {
    uint8_t bDescLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
    uint8_t DeviceRemovable;
    uint8_t PortPwrCtrlMask;
} __attribute__((packed)) usb_hub_descriptor_t;
int usb_hub_init(void);
int usb_hub_enumerate_ports(void);
#endif
