#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR_INTEL 0x8086
#define PCI_VENDOR_VIA 0x1106
#define PCI_VENDOR_NVIDIA 0x10DE
#define PCI_VENDOR_ATI 0x1002

#define PCI_CLASS_SERIAL 0x0C
#define PCI_SUBCLASS_UHCI 0x00
#define PCI_SUBCLASS_OHCI 0x01
#define PCI_SUBCLASS_EHCI 0x02
#define PCI_SUBCLASS_XHCI 0x03

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t rev_id;
    uint8_t bus;
    uint8_t device;
    uint8_t func;
} pci_device_t;

uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
int pci_find_devices(uint8_t class_code, uint8_t subclass, pci_device_t *devices, int max_devices);
uint32_t pci_get_bar(uint8_t bus, uint8_t device, uint8_t func, int bar_num);

#endif
