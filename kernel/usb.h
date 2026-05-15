#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_DEVICE_DESCRIPTOR_TYPE 1
#define USB_CONFIG_DESCRIPTOR_TYPE 2
#define USB_STRING_DESCRIPTOR_TYPE 3
#define USB_INTERFACE_DESCRIPTOR_TYPE 4
#define USB_ENDPOINT_DESCRIPTOR_TYPE 5
#define USB_HID_DESCRIPTOR_TYPE 0x21
#define USB_HID_REPORT_DESCRIPTOR_TYPE 0x22

#define USB_REQ_STANDARD 0
#define USB_REQ_CLASS 1
#define USB_REQ_VENDOR 2

#define USB_DIR_DEV_TO_HOST 0x80
#define USB_DIR_HOST_TO_DEV 0x00

#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_SET_CONFIGURATION 9

#define USB_EP_TYPE_CONTROL 0
#define USB_EP_TYPE_ISOCHRONOUS 1
#define USB_EP_TYPE_BULK 2
#define USB_EP_TYPE_INTERRUPT 3

#define USB_CLASS_HID 3

#define USB_PID_OUT 0xE1
#define USB_PID_IN 0x69
#define USB_PID_SETUP 0x2D
#define USB_PID_DATA0 0xC3
#define USB_PID_DATA1 0x4B
#define USB_PID_ACK 0xD2
#define USB_PID_NAK 0x5A
#define USB_PID_STALL 0x1E

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_device_request_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

void usb_init(void);

#endif
