#include "xhci.h"
#include "pci.h"
#include "io.h"
int xhci_init(xhci_controller_t *ctrl)
{
    ctrl->initialized = 0;
    pci_device_t devs[4];
    int n = pci_find_devices(0x0C, 0x03, devs, 4);
    if (!n) return -1;
    uint32_t bar = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 0);
    ctrl->mmio_base = bar & 0xFFFFFFF0;
    pci_write_config(devs[0].bus, devs[0].device, devs[0].func, 4, 0x2);
    volatile uint32_t *cap = (volatile uint32_t *)ctrl->mmio_base;
    ctrl->max_slots = (cap[1] >> 8) & 0xFF;
    ctrl->max_ports = (cap[2] >> 24) & 0xFF;
    if (ctrl->max_ports > XHCI_MAX_PORTS) ctrl->max_ports = XHCI_MAX_PORTS;
    ctrl->initialized = 1;
    return 0;
}
int xhci_port_detect(xhci_controller_t *ctrl, int port)
{
    (void)ctrl;
    (void)port;
    return 0;
}
