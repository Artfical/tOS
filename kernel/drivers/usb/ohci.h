#ifndef OHCI_H
#define OHCI_H
#include <stdint.h>
#include "usb.h"
#define OHCI_HcRevision 0x00
#define OHCI_HcControl 0x04
#define OHCI_HcCommandStatus 0x08
#define OHCI_HcInterruptStatus 0x0C
#define OHCI_HcInterruptEnable 0x10
#define OHCI_HcHCCA 0x14
#define OHCI_HcPeriodCurrentED 0x18
#define OHCI_HcControlHeadED 0x1C
#define OHCI_HcControlCurrentED 0x20
#define OHCI_HcBulkHeadED 0x24
#define OHCI_HcBulkCurrentED 0x28
#define OHCI_HcDoneHead 0x2C
#define OHCI_HcFmInterval 0x30
#define OHCI_HcFmRemaining 0x34
#define OHCI_HcFmNumber 0x38
#define OHCI_HcPeriodicStart 0x3C
#define OHCI_HcLSThreshold 0x40
#define OHCI_HcRhDescriptorA 0x48
#define OHCI_HcRhDescriptorB 0x4C
#define OHCI_HcRhStatus 0x50
#define OHCI_HcRhPortStatus1 0x54
#define OHCI_ED_ITD 0x00
#define OHCI_ED_QH 0x02
#define OHCI_ED_TD 0x03
typedef struct {
    uint32_t io_base;
    int initialized;
} ohci_controller_t;
int ohci_init(ohci_controller_t *ctrl);
int ohci_port_detect(ohci_controller_t *ctrl, int port);
int ohci_control(ohci_controller_t *ctrl, int dev, usb_device_request_t *req, void *data, int len, int dir);
int ohci_bulk(ohci_controller_t *ctrl, int dev, int ep, void *data, int len, int dir);
#endif
