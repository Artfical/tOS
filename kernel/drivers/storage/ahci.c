#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "paging.h"

#define HBA_PORT_DET_PRESENT 0x03
#define HBA_PORT_IPM_ACTIVE  0x01

#define PORT_CMD_ST   (1 << 0)
#define PORT_CMD_FRE  (1 << 4)
#define PORT_CMD_FR   (1 << 14)
#define PORT_CMD_CR   (1 << 15)

#define ATA_CMD_IDENTIFY 0xEC

static int ahci_find_hba(pci_device_t *out_dev, ahci_hba_t *hba)
{
    pci_device_t devs[4];
    int n = pci_find_devices(0x01, 0x06, devs, 4);
    if (!n) return -1;
    *out_dev = devs[0];
    uint32_t bar5 = pci_get_bar(devs[0].bus, devs[0].device, devs[0].func, 5);
    hba->base = bar5 & 0xFFFFFFF0;
    hba->port_count = 0;
    return 0;
}

static void ahci_port_stop(ahci_port_t *port)
{
    port->cmd &= ~PORT_CMD_ST;
    int t = 1000000;
    while ((port->cmd & PORT_CMD_CR) && --t);
    port->cmd &= ~PORT_CMD_FRE;
}

static void ahci_port_start(ahci_port_t *port)
{
    int t = 1000000;
    while ((port->cmd & PORT_CMD_CR) && --t);
    port->cmd |= PORT_CMD_FRE;
    port->cmd |= PORT_CMD_ST;
}

static int ahci_setup_port(ahci_hba_t *hba, int i, ahci_port_t *port)
{
    ahci_port_stop(port);

    void *cl = malloc(4096);
    if (!cl) return -1;
    uint32_t cl_phys = ((uint32_t)cl + 1023) & ~1023u;
    memset((void *)cl_phys, 0, 1024);

    uint32_t fb_phys = (cl_phys + 1024 + 255) & ~255u;
    memset((void *)fb_phys, 0, 256);

    uint32_t ct_phys = (fb_phys + 256 + 127) & ~127u;
    memset((void *)ct_phys, 0, sizeof(ahci_cmd_tbl_t));

    hba->cmd_list[i] = (ahci_cmd_header_t *)cl_phys;
    hba->fis_base[i] = (uint8_t *)fb_phys;
    hba->cmd_tbl[i] = (ahci_cmd_tbl_t *)ct_phys;

    port->clb = cl_phys;
    port->clbu = 0;
    port->fb = fb_phys;
    port->fbu = 0;
    port->serr = port->serr;
    port->is = 0xFFFFFFFF;

    hba->cmd_list[i][0].ctba = ct_phys;
    hba->cmd_list[i][0].ctbau = 0;

    ahci_port_start(port);
    return 0;
}

int ahci_init(ahci_hba_t *hba)
{
    pci_device_t dev;
    if (ahci_find_hba(&dev, hba)) return -1;

    uint32_t cmd = pci_read_config(dev.bus, dev.device, dev.func, 4) & 0xFFFF;
    pci_write_config(dev.bus, dev.device, dev.func, 4, cmd | 0x06);

    paging_map_range(hba->base, hba->base, 0x2000, PTE_PRESENT | PTE_WRITABLE);

    volatile uint32_t *ghc = (volatile uint32_t *)(hba->base + 0x04);
    *ghc |= (1u << 31);

    volatile uint32_t *cap = (volatile uint32_t *)(hba->base);
    int max_ports = ((*cap >> 8) & 0x1F) + 1;
    volatile uint32_t *pi = (volatile uint32_t *)(hba->base + 0x0C);
    uint32_t ports_implemented = *pi;

    for (int i = 0; i < max_ports && i < AHCI_MAX_PORTS; i++) {
        if (!(ports_implemented & (1u << i))) continue;
        ahci_port_t *port = (ahci_port_t *)(hba->base + 0x100 + i * 0x80);
        uint32_t ssts = port->ssts;
        uint8_t det = ssts & 0x0F;
        uint8_t ipm = (ssts >> 8) & 0x0F;
        if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) continue;
        if (port->sig != AHCI_SIG_ATA) continue;

        if (ahci_setup_port(hba, i, port) != 0) continue;

        hba->ports[i] = port;
        hba->port_count++;
    }

    return hba->port_count > 0 ? 0 : -1;
}

static int ahci_wait_idle(ahci_hba_t *hba, int port, int slot)
{
    int t = 5000000;
    while ((hba->ports[port]->ci & (1u << slot)) && --t) {
        if (hba->ports[port]->is & (1u << 30)) return -1;
    }
    return t ? 0 : -1;
}

static void ahci_build_fis(uint8_t *cfis, uint8_t cmd, uint64_t lba, uint16_t count)
{
    memset(cfis, 0, 64);
    cfis[0] = 0x27;
    cfis[1] = 0x80;
    cfis[2] = cmd;
    cfis[3] = 0;
    cfis[4] = (uint8_t)(lba & 0xFF);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFF);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFF);
    cfis[7] = 0x40;
    cfis[8] = (uint8_t)((lba >> 24) & 0xFF);
    cfis[9] = (uint8_t)((lba >> 32) & 0xFF);
    cfis[10] = (uint8_t)((lba >> 40) & 0xFF);
    cfis[11] = 0;
    cfis[12] = (uint8_t)(count & 0xFF);
    cfis[13] = (uint8_t)((count >> 8) & 0xFF);
    cfis[14] = 0;
    cfis[15] = 0;
}

static int ahci_do_cmd(ahci_hba_t *hba, int port, uint64_t lba, int count, void *buf, uint8_t ata_cmd, int is_write)
{
    if (port < 0 || port >= AHCI_MAX_PORTS || !hba->ports[port]) return -1;
    ahci_port_t *p = hba->ports[port];

    int t = 1000000;
    while ((p->tfd & 0x88) && --t);

    ahci_cmd_header_t *hdr = &hba->cmd_list[port][0];
    ahci_cmd_tbl_t *tbl = hba->cmd_tbl[port];
    memset(tbl, 0, sizeof(*tbl));

    hdr->flags = 5 | (is_write ? (1 << 6) : 0);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    ahci_build_fis(tbl->cfis, ata_cmd, lba, (uint16_t)count);

    tbl->prdt[0].dba = (uint32_t)buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc_flags = (uint32_t)(count * 512 - 1) & 0x3FFFFF;

    p->is = 0xFFFFFFFF;
    p->ci |= 1;

    if (ahci_wait_idle(hba, port, 0) != 0) return -1;
    if (p->tfd & 0x01) return -1;

    return 0;
}

int ahci_read(ahci_hba_t *hba, int port, uint64_t lba, int count, void *buf)
{
    return ahci_do_cmd(hba, port, lba, count, buf, AHCI_CMD_READ_DMA, 0);
}

int ahci_write(ahci_hba_t *hba, int port, uint64_t lba, int count, const void *buf)
{
    return ahci_do_cmd(hba, port, lba, count, (void *)buf, AHCI_CMD_WRITE_DMA, 1);
}
