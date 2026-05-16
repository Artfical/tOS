#include "ehci.h"
#include "pci.h"
#include "io.h"
int ehci_init(ehci_controller_t *ctrl)
{
    ctrl->initialized = 0;
    pci_device_t devs[4];
    int n = pci_find_devices(0x0C, 0x02, devs, 4);
    if (!n) return -1;
    uint32_t bar = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 0);
    ctrl->mmio_base = bar & 0xFFFFFFF0;
    pci_write_config(devs[0].bus, devs[0].device, devs[0].func, 4, 0x2);
    volatile uint32_t *regs = (volatile uint32_t *)ctrl->mmio_base;
    regs[EHCI_USBCMD / 4] |= 0x02;
    for (int i = 0; i < 100000; i++) {
        if (!(regs[EHCI_USBCMD / 4] & 0x02)) break;
    }
    regs[EHCI_USBCMD / 4] = 0x20000;
    regs[EHCI_CONFIGFLAG / 4] = 1;
    ctrl->initialized = 1;
    ctrl->port_count = 0;
    return 0;
}
int ehci_port_detect(ehci_controller_t *ctrl, int port)
{
    (void)ctrl;
    (void)port;
    return 0;
}
int ehci_reset_port(ehci_controller_t *ctrl, int port)
{
    (void)ctrl;
    (void)port;
    return -1;
}
