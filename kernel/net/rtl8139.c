#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "terminal.h"

#define RX_BUF_SIZE  8192
#define TX_BUF_SIZE  1536

static uint16_t io_base = 0;
static uint8_t *rx_buf = 0;
static int rx_ptr = 0;

#define RTL_REG_RBSTART  0x30
#define RTL_REG_CR       0x34
#define RTL_REG_CAPR     0x38
#define RTL_REG_ISR      0x3E
#define RTL_REG_RCR      0x3C
#define RTL_REG_TCR      0x40
#define RTL_REG_TSAD0    0x48
#define RTL_REG_TSAD1    0x4C

int rtl8139_init(void)
{
    serial_write("rtl8139: probing...\n");
    serial_write("rtl8139: pci cfg test b0d0f0=");
    uint32_t t = pci_read_config(0, 0, 0, 0);
    for (int k = 28; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(t >> k) & 0xF]);
    serial_write("\n");

    pci_device_t devs[16];
    int n = pci_find_devices(0x02, 0x00, devs, 16);
    serial_write("rtl8139: pci netdev count=");
    serial_putchar('0' + n);
    serial_write("\n");
    for (int i = 0; i < n; i++) {
        serial_write("  netdev: v=0x");
        for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(devs[i].vendor_id >> k) & 0xF]);
        serial_write(" d=0x");
        for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(devs[i].device_id >> k) & 0xF]);
        serial_write("\n");
    }

    serial_write("rtl8139: scanning bus 0 PCI devices...\n");
    int all_count = 0;
    for (int d = 0; d < 32 && all_count < 48; d++) {
        for (int f = 0; f < 8 && all_count < 48; f++) {
            uint32_t vd = pci_read_config(0, d, f, 0);
            uint16_t vid = vd & 0xFFFF;
            if (vid == 0xFFFF) { if (f == 0) break; continue; }
            serial_write("  d");
            serial_putchar('0' + d / 10);
            serial_putchar('0' + d % 10);
            serial_write("f");
            serial_putchar('0' + f);
            serial_write(": v=0x");
            for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(vid >> k) & 0xF]);
            serial_write(" d=0x");
            uint16_t did = (vd >> 16) & 0xFFFF;
            for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(did >> k) & 0xF]);
            uint32_t cr = pci_read_config(0, d, f, 8);
            serial_write(" c=0x");
            for (int k = 20; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[((cr >> 8) & 0xFFFFFF) >> k & 0xF]);
            serial_write("\n");
            all_count++;
            if (f == 0 && !(vd & 0x800000)) break;
        }
    }
    serial_write("rtl8139: end of PCI dump\n");

    if (n == 0) return -1;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == RTL8139_VENDOR_ID && devs[i].device_id == RTL8139_DEVICE_ID) {
            uint32_t bar = pci_get_bar(devs[i].bus, devs[i].device, devs[i].func, 0);
            if (bar & 1) io_base = bar & 0xFFFC;
            else io_base = (bar & 0xFFFE) + 0xC000; // fallback
            for (int j = 0; j < 6; j++) net_mac[j] = inb(io_base + j);
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    outb(io_base + 0x52, 0x00);
    outl(io_base + RTL_REG_CR, 0x10);
    int timeout = 0;
    while ((inb(io_base + RTL_REG_CR) & 0x10) && timeout < 1000) { io_wait(); timeout++; }

    rx_buf = (uint8_t *)malloc(RX_BUF_SIZE + 16);
    if (!rx_buf) return -1;
    memset(rx_buf, 0, RX_BUF_SIZE + 16);

    outl(io_base + RTL_REG_RBSTART, (uint32_t)(uintptr_t)rx_buf);
    outl(io_base + RTL_REG_RCR, 0x00000F0E);
    outl(io_base + RTL_REG_TCR, 0x00000A00);
    outb(io_base + RTL_REG_CR, 0x0C);

    outb(io_base + 0x37, 0x00);
    outw(io_base + RTL_REG_ISR, 0xFFFF);

    terminal_writestring("[OK] RTL8139 initialized (I/O: 0x");
    char buf[9];
    for (int k = 0; k < 8; k++) buf[7 - k] = "0123456789ABCDEF"[(io_base >> (4 * k)) & 0xF];
    buf[8] = 0;
    terminal_writestring(buf);
    terminal_writestring(")\n");
    return 0;
}

void rtl8139_send(void *data, int len)
{
    if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;
    while (!(inb(io_base + 0x36) & 0x04)) io_wait();
    for (int i = 0; i < len; i++) outb(io_base + 0x20 + i, ((uint8_t *)data)[i]);
    outl(io_base + RTL_REG_TSAD0, len);
    int timeout = 0;
    while (!(inl(io_base + RTL_REG_TSAD0) & 0x80000000) && timeout < 100000) timeout++;
}

int rtl8139_poll(uint8_t *buf, int max_len)
{
    uint16_t status = inw(io_base + RTL_REG_ISR);
    if (!(status & 0x0001)) return 0;
    outw(io_base + RTL_REG_ISR, status);

    int avail = inb(io_base + RTL_REG_CR) & 0x01;
    if (!avail) { rx_ptr = 0; return 0; }

    uint16_t rx_size   = *(volatile uint16_t *)(rx_buf + rx_ptr + 2) & 0x3FFF;
    if (rx_size < 4) { rx_ptr = 0; return 0; }
    int data_len = rx_size - 4;
    if (data_len > max_len) data_len = max_len;
    for (int i = 0; i < data_len; i++)
        buf[i] = rx_buf[rx_ptr + 4 + i];

    rx_ptr = (rx_ptr + rx_size + 3) & ~3;
    if (rx_ptr >= RX_BUF_SIZE) rx_ptr = 0;
    outw(io_base + RTL_REG_CAPR, rx_ptr - 0x10);

    return data_len;
}
