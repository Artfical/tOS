#ifndef XHCI_H
#define XHCI_H
#include <stdint.h>
#include "usb.h"
#define XHCI_CAPLENGTH 0x00
#define XHCI_HCIVERSION 0x02
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCSPARAMS3 0x0C
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF 0x14
#define XHCI_RTSOFF 0x18
#define XHCI_MAX_SLOTS 64
#define XHCI_MAX_PORTS 64
typedef struct {
    uint32_t mmio_base;
    int initialized;
    int max_slots;
    int max_ports;
} xhci_controller_t;
int xhci_init(xhci_controller_t *ctrl);
int xhci_port_detect(xhci_controller_t *ctrl, int port);
#endif
