#include "ne2000.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

#define NE_VENDOR_ID  0x10EC
#define NE_DEVICE_ID  0x8029  /* Realtek RTL8029 / NE2000 PCI clone (QEMU ne2k_pci) */

/* NS8390 core register offsets (page 0, write side unless noted) */
#define NE_CR     0x00
#define NE_PSTART 0x01
#define NE_PSTOP  0x02
#define NE_BNRY   0x03
#define NE_TPSR   0x04
#define NE_TBCR0  0x05
#define NE_TBCR1  0x06
#define NE_ISR    0x07
#define NE_RSAR0  0x08
#define NE_RSAR1  0x09
#define NE_RBCR0  0x0A
#define NE_RBCR1  0x0B
#define NE_RCR    0x0C
#define NE_TCR    0x0D
#define NE_DCR    0x0E
#define NE_IMR    0x0F
#define NE_DATA   0x10
#define NE_RESET  0x1F

/* Page 1 register offsets */
#define NE_PAR0   0x01
#define NE_CURR   0x07

#define CR_STP  0x01
#define CR_STA  0x02
#define CR_TXP  0x04
#define CR_RD0  0x08
#define CR_RD1  0x10
#define CR_RD2  0x20
#define CR_PS0  0x40

#define ISR_PRX 0x01
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define NE_TX_PAGE   0x40
#define NE_RX_START  0x46
#define NE_RX_STOP   0x80

#define TX_BUF_SIZE 1536

static uint16_t io_base = 0;
static uint8_t next_pkt = NE_RX_START + 1;
static int ready = 0;

static void page0(void) { outb(io_base + NE_CR, CR_STA | CR_RD2); }
static void page1(void) { outb(io_base + NE_CR, CR_STA | CR_PS0 | CR_RD2); }

static void rdma_read(uint16_t addr, uint16_t len, uint8_t *dst)
{
    outb(io_base + NE_ISR, ISR_RDC);
    outb(io_base + NE_RBCR0, len & 0xFF);
    outb(io_base + NE_RBCR1, (len >> 8) & 0xFF);
    outb(io_base + NE_RSAR0, addr & 0xFF);
    outb(io_base + NE_RSAR1, (addr >> 8) & 0xFF);
    outb(io_base + NE_CR, CR_STA | CR_RD0);

    int words = (len + 1) / 2;
    for (int i = 0; i < words; i++) {
        uint16_t w = inw(io_base + NE_DATA);
        dst[i * 2] = w & 0xFF;
        if (i * 2 + 1 < len) dst[i * 2 + 1] = (w >> 8) & 0xFF;
    }

    int timeout = 0;
    while (!(inb(io_base + NE_ISR) & ISR_RDC) && timeout < 100000) timeout++;
    outb(io_base + NE_ISR, ISR_RDC);
}

static void rdma_write(const uint8_t *src, uint16_t len)
{
    outb(io_base + NE_ISR, ISR_RDC);
    outb(io_base + NE_RBCR0, len & 0xFF);
    outb(io_base + NE_RBCR1, (len >> 8) & 0xFF);
    outb(io_base + NE_RSAR0, 0x00);
    outb(io_base + NE_RSAR1, NE_TX_PAGE);
    outb(io_base + NE_CR, CR_STA | CR_RD1);

    int words = (len + 1) / 2;
    for (int i = 0; i < words; i++) {
        uint16_t lo = src[i * 2];
        uint16_t hi = (i * 2 + 1 < len) ? src[i * 2 + 1] : 0;
        outw(io_base + NE_DATA, lo | (hi << 8));
    }

    int timeout = 0;
    while (!(inb(io_base + NE_ISR) & ISR_RDC) && timeout < 100000) timeout++;
    outb(io_base + NE_ISR, ISR_RDC);
}

int ne2000_init(void)
{
    pci_device_t devs[8];
    int n = pci_find_devices(0x02, 0x00, devs, 8);
    if (n == 0) return -1;

    int found = 0;
    uint8_t bus = 0, dev = 0, func = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == NE_VENDOR_ID && devs[i].device_id == NE_DEVICE_ID) {
            bus = devs[i].bus; dev = devs[i].device; func = devs[i].func;
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
    if (!(bar0 & 1)) return -1;
    io_base = bar0 & 0xFFFC;

    uint32_t cmd = pci_read_config(bus, dev, func, 0x04);
    cmd |= 0x05; /* I/O space + bus master */
    pci_write_config(bus, dev, func, 0x04, cmd);

    uint8_t rv = inb(io_base + NE_RESET);
    outb(io_base + NE_RESET, rv);
    int timeout = 0;
    while (!(inb(io_base + NE_ISR) & ISR_RST) && timeout < 100000) timeout++;
    outb(io_base + NE_ISR, 0xFF);

    outb(io_base + NE_CR, CR_STP | CR_RD2);
    outb(io_base + NE_DCR, 0x49);
    outb(io_base + NE_RBCR0, 0x00);
    outb(io_base + NE_RBCR1, 0x00);
    outb(io_base + NE_RCR, 0x20);
    outb(io_base + NE_TCR, 0x02);
    outb(io_base + NE_PSTART, NE_RX_START);
    outb(io_base + NE_PSTOP, NE_RX_STOP);
    outb(io_base + NE_BNRY, NE_RX_START);

    uint8_t prom[32];
    outb(io_base + NE_RBCR0, 32);
    outb(io_base + NE_RBCR1, 0);
    outb(io_base + NE_RSAR0, 0);
    outb(io_base + NE_RSAR1, 0);
    outb(io_base + NE_CR, CR_STA | CR_RD0);
    for (int i = 0; i < 16; i++) {
        uint16_t w = inw(io_base + NE_DATA);
        prom[i * 2] = w & 0xFF;
        prom[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    timeout = 0;
    while (!(inb(io_base + NE_ISR) & ISR_RDC) && timeout < 100000) timeout++;
    outb(io_base + NE_ISR, ISR_RDC);
    outb(io_base + NE_CR, CR_STP | CR_RD2);

    /* QEMU's ne2k_pci PROM repeats each MAC byte twice (16-bit data path
       quirk inherited from real NE2000/NE1000 ISA hardware), so only every
       other byte is part of the actual address. */
    for (int i = 0; i < 6; i++) net_mac[i] = prom[i * 2];

    outb(io_base + NE_TPSR, NE_TX_PAGE);

    page1();
    for (int i = 0; i < 6; i++) outb(io_base + NE_PAR0 + i, net_mac[i]);
    outb(io_base + NE_CURR, NE_RX_START + 1);
    page0();

    next_pkt = NE_RX_START + 1;

    outb(io_base + NE_RCR, 0x0C); /* accept broadcast + multicast */
    outb(io_base + NE_TCR, 0x00); /* normal operation, no loopback */
    outb(io_base + NE_ISR, 0xFF);
    outb(io_base + NE_IMR, 0x00); /* polling, no IRQ delivery */
    outb(io_base + NE_CR, CR_STA | CR_RD2);

    ready = 1;
    serial_write("ne2000: ready\n");
    return 0;
}

void ne2000_send(void *data, int len)
{
    if (!ready) return;
    if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;

    int timeout = 0;
    while ((inb(io_base + NE_CR) & CR_TXP) && timeout < 100000) timeout++;

    rdma_write((const uint8_t *)data, (uint16_t)len);

    outb(io_base + NE_TBCR0, len & 0xFF);
    outb(io_base + NE_TBCR1, (len >> 8) & 0xFF);
    outb(io_base + NE_TPSR, NE_TX_PAGE);
    outb(io_base + NE_CR, CR_STA | CR_TXP);
}

int ne2000_poll(uint8_t *buf, int max_len)
{
    if (!ready) return 0;

    page1();
    uint8_t curr = inb(io_base + NE_CURR);
    page0();

    if (next_pkt == curr) return 0;

    uint8_t hdr[4];
    rdma_read((uint16_t)next_pkt << 8, 4, hdr);
    uint8_t status = hdr[0];
    uint8_t next_page = hdr[1];
    uint16_t pkt_len = hdr[2] | (hdr[3] << 8);

    if (pkt_len < 4 || pkt_len > 1518 + 4 || next_page < NE_RX_START || next_page >= NE_RX_STOP) {
        outb(io_base + NE_BNRY, curr - 1 >= NE_RX_START ? curr - 1 : NE_RX_STOP - 1);
        next_pkt = curr;
        return 0;
    }

    int data_len = pkt_len - 4;
    if (data_len > max_len) data_len = max_len;

    uint32_t addr = ((uint32_t)next_pkt << 8) + 4;
    uint32_t stop = (uint32_t)NE_RX_STOP << 8;
    if (addr + data_len <= stop) {
        rdma_read((uint16_t)addr, (uint16_t)data_len, buf);
    } else {
        int first = (int)(stop - addr);
        rdma_read((uint16_t)addr, (uint16_t)first, buf);
        rdma_read((uint16_t)(NE_RX_START << 8), (uint16_t)(data_len - first), buf + first);
    }

    next_pkt = next_page;
    outb(io_base + NE_BNRY, (next_page == NE_RX_START) ? (NE_RX_STOP - 1) : (next_page - 1));
    outb(io_base + NE_ISR, ISR_PRX);

    (void)status;
    return data_len;
}
