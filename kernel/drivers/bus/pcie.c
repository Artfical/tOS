#include "pcie.h"
#include "pci.h"
#include "io.h"

int pcie_detect(void)
{
    uint32_t ecap = pci_read_config(0, 0, 0, 0x34);
    uint8_t cap_ptr = ecap & 0xFF;
    while (cap_ptr) {
        uint32_t cap = pci_read_config(0, 0, 0, cap_ptr);
        if ((cap & 0xFF) == 0x10) return 1;
        cap_ptr = (cap >> 8) & 0xFF;
    }
    return 0;
}

int pcie_find_capability(uint8_t bus, uint8_t device, uint8_t func, pcie_cap_t *cap)
{
    uint32_t status = pci_read_config(bus, device, func, 4);
    if (!(status & (1 << 4))) return -1;
    uint8_t cap_ptr = (pci_read_config(bus, device, func, 0x34) & 0xFF);
    while (cap_ptr) {
        uint32_t cap_reg = pci_read_config(bus, device, func, cap_ptr);
        uint8_t cap_id = cap_reg & 0xFF;
        if (cap_id == 0x10) {
            cap->cap_id = cap_id;
            cap->cap_version = (cap_reg >> 16) & 0x0F;
            cap->device_type = (cap_reg >> 20) & 0x0F;
            uint32_t cap2 = pci_read_config(bus, device, func, cap_ptr + 4);
            cap->link_speed = cap2 & 0x0F;
            cap->link_width = (cap2 >> 4) & 0x3F;
            return 0;
        }
        cap_ptr = (cap_reg >> 8) & 0xFF;
    }
    return -1;
}

uint32_t pcie_mmio_read(uint32_t mmio_base, uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t addr = mmio_base | ((uint32_t)bus << 20) | ((uint32_t)dev << 15) | ((uint32_t)func << 12) | offset;
    return *(volatile uint32_t *)addr;
}
