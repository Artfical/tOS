#ifndef UHCI_H
#define UHCI_H

#include <stdint.h>
#include "pci.h"
#include "usb.h"

typedef struct {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} __attribute__((packed)) uhci_td_t;

typedef struct {
    uint32_t link;
    uint32_t element;
} __attribute__((packed)) uhci_qh_t;

typedef struct {
    pci_device_t pci;
    uint16_t io_base;
    uint32_t frame_list_phys;
    volatile uint32_t *frame_list;
    uhci_qh_t *async_qh;
} uhci_controller_t;

int uhci_init(uhci_controller_t *ctrl);
int uhci_port_detect(uhci_controller_t *ctrl, int port);
int uhci_control(uhci_controller_t *ctrl, int dev, int ep, usb_device_request_t *req, void *data, int dir);

#endif
