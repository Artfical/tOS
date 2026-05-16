#include "ohci.h"
#include "pci.h"
#include "io.h"
int ohci_init(ohci_controller_t *ctrl)
{
    ctrl->io_base = 0;
    ctrl->initialized = 0;
    pci_device_t devs[4];
    int n = pci_find_devices(0x0C, 0x01, devs, 4);
    if (!n) return -1;
    uint32_t bar = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 0);
    ctrl->io_base = bar & 0xFFFFFFF0;
    if (!ctrl->io_base) {
        bar = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 1);
        ctrl->io_base = bar & 0xFFFFFFF0;
    }
    pci_write_config(devs[0].bus, devs[0].device, devs[0].func, 4, 0x2);
    volatile uint32_t *base = (volatile uint32_t *)ctrl->io_base;
    base[OHCI_HcControl / 4] = 0;
    base[OHCI_HcCommandStatus / 4] = 0;
    base[OHCI_HcInterruptStatus / 4] = 0x80000000;
    base[OHCI_HcControl / 4] = 0x80;
    ctrl->initialized = 1;
    return 0;
}
int ohci_port_detect(ohci_controller_t *ctrl, int port)
{
    (void)ctrl;
    (void)port;
    return 0;
}
int ohci_control(ohci_controller_t *ctrl, int dev, usb_device_request_t *req, void *data, int len, int dir)
{
    (void)ctrl;
    (void)dev;
    (void)req;
    (void)data;
    (void)len;
    (void)dir;
    return -1;
}
int ohci_bulk(ohci_controller_t *ctrl, int dev, int ep, void *data, int len, int dir)
{
    (void)ctrl;
    (void)dev;
    (void)ep;
    (void)data;
    (void)len;
    (void)dir;
    return -1;
}
