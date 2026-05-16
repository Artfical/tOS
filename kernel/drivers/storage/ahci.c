#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
static int ahci_find_hba(ahci_hba_t *hba)
{
    pci_device_t devs[4];
    int n = pci_find_devices(0x01, 0x06, devs, 4);
    if (!n) return -1;
    uint32_t bar5 = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 5);
    hba->base = bar5 & 0xFFFFFFF0;
    hba->port_count = 0;
    return 0;
}
int ahci_init(ahci_hba_t *hba)
{
    if (ahci_find_hba(hba)) return -1;
    volatile uint32_t *ghc = (volatile uint32_t *)(hba->base + 0x04);
    *ghc |= (1 << 31);
    volatile uint32_t *cap = (volatile uint32_t *)(hba->base);
    int max_ports = ((*cap >> 8) & 0x1F) + 1;
    volatile uint32_t *pi = (volatile uint32_t *)(hba->base + 0x0C);
    uint32_t ports_implemented = *pi;
    for (int i = 0; i < max_ports && i < AHCI_MAX_PORTS; i++) {
        if (!(ports_implemented & (1 << i))) continue;
        ahci_port_t *port = (ahci_port_t *)(hba->base + 0x100 + i * 0x80);
        uint32_t ssts = port->ssts;
        if ((ssts & 0x0F) != 0x03) continue;
        hba->ports[hba->port_count++] = port;
    }
    return hba->port_count > 0 ? 0 : -1;
}
int ahci_read(ahci_hba_t *hba, int port, uint64_t lba, int count, void *buf)
{
    (void)hba;
    (void)port;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
int ahci_write(ahci_hba_t *hba, int port, uint64_t lba, int count, const void *buf)
{
    (void)hba;
    (void)port;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}
