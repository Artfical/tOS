#ifndef EHCI_H
#define EHCI_H
#include <stdint.h>
#include "usb.h"
#define EHCI_USBCMD 0x00
#define EHCI_USBSTS 0x04
#define EHCI_USBINTR 0x08
#define EHCI_FRINDEX 0x0C
#define EHCI_CTRLDSSEGMENT 0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC 0x44
#define EHCI_PORTSC_SIZE 4
typedef struct {
    uint32_t mmio_base;
    int initialized;
    int port_count;
} ehci_controller_t;
int ehci_init(ehci_controller_t *ctrl);
int ehci_port_detect(ehci_controller_t *ctrl, int port);
int ehci_reset_port(ehci_controller_t *ctrl, int port);
#endif
