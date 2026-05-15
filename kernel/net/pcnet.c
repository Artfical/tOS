#include "pcnet.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "terminal.h"

#define RX_RING_SIZE 4
#define TX_RING_SIZE 4
#define BUF_SIZE 1536
#define TIMEOUT 10000

static uint16_t io_base = 0;
static uint8_t *tx_bufs[TX_RING_SIZE];
static uint8_t *rx_bufs[RX_RING_SIZE];
static void *rx_ring_mem = 0;
static void *tx_ring_mem = 0;
static volatile uint16_t *rx_ring = 0;
static volatile uint16_t *tx_ring = 0;
static int tx_cur = 0;
static int rx_cur = 0;
static int initialized = 0;

static uint16_t pcnet_csr_read(int reg)
{
    outw(io_base, reg);
    io_wait();
    return inw(io_base + 0x10);
}

static void pcnet_csr_write(int reg, uint16_t val)
{
    outw(io_base, reg);
    io_wait();
    outw(io_base + 0x10, val);
    io_wait();
}

int pcnet_init(void)
{
    pci_device_t devs[4];
    int n = pci_find_devices(0x02, 0x00, devs, 4);
    if (n == 0) return -1;

    int found = 0;
    uint8_t bus = 0, dev = 0, func = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == PCNET_VENDOR_AMD &&
            (devs[i].device_id == PCNET_DEVICE_PCI_II ||
             devs[i].device_id == PCNET_DEVICE_FAST_III ||
             devs[i].device_id == PCNET_DEVICE_HOME)) {
            bus = devs[i].bus;
            dev = devs[i].device;
            func = devs[i].func;
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    uint32_t bar = pci_get_bar(bus, dev, func, 0);
    if (!(bar & 1)) return -1;
    io_base = bar & 0xFFFC;

    pci_write_config(bus, dev, func, 0x04, 0x05);
    io_wait();

    outb(io_base + 0x14, 0x0F);
    io_wait();
    for (int t = 0; t < TIMEOUT; t++) {
        if (pcnet_csr_read(0) & 0x0004) break;
    }

    uint8_t mac[6];
    for (int j = 0; j < 3; j++) {
        uint16_t w = inw(io_base + 0x10 + j * 2);
        mac[j*2] = w & 0xFF;
        mac[j*2+1] = (w >> 8) & 0xFF;
    }
    for (int j = 0; j < 6; j++) net_mac[j] = mac[j];

    void *ib = malloc(32);
    if (!ib) return -1;
    memset(ib, 0, 32);
    for (int j = 0; j < 6; j++) ((uint8_t *)ib)[2 + j] = mac[j];

    for (int i = 0; i < RX_RING_SIZE; i++) {
        rx_bufs[i] = (uint8_t *)malloc(BUF_SIZE);
        if (!rx_bufs[i]) { free(ib); return -1; }
    }
    for (int i = 0; i < TX_RING_SIZE; i++) {
        tx_bufs[i] = (uint8_t *)malloc(BUF_SIZE);
        if (!tx_bufs[i]) { free(ib); return -1; }
    }

    rx_ring_mem = malloc(RX_RING_SIZE * 16 + 16);
    tx_ring_mem = malloc(TX_RING_SIZE * 16 + 16);
    if (!rx_ring_mem || !tx_ring_mem) { free(ib); return -1; }

    uintptr_t rra = ((uintptr_t)rx_ring_mem + 15) & ~15;
    uintptr_t tra = ((uintptr_t)tx_ring_mem + 15) & ~15;
    rx_ring = (volatile uint16_t *)rra;
    tx_ring = (volatile uint16_t *)tra;
    memset((void *)rra, 0, RX_RING_SIZE * 16);
    memset((void *)tra, 0, TX_RING_SIZE * 16);

    for (int i = 0; i < RX_RING_SIZE; i++) {
        rx_ring[i * 8]     = 0x8000;
        rx_ring[i * 8 + 1] = 0xF000;
        *(volatile uint32_t *)(rx_ring + i * 8 + 2) = (uint32_t)(uintptr_t)rx_bufs[i];
    }
    for (int i = 0; i < TX_RING_SIZE; i++) {
        *(volatile uint32_t *)(tx_ring + i * 8 + 2) = (uint32_t)(uintptr_t)tx_bufs[i];
    }

    *(uint32_t *)((uint8_t *)ib + 16) = (uint32_t)rra;
    *(uint32_t *)((uint8_t *)ib + 20) = (uint32_t)tra;

    pcnet_csr_write(1, (uint32_t)(uintptr_t)ib & 0xFFFF);
    pcnet_csr_write(2, ((uint32_t)(uintptr_t)ib >> 16) & 0xFFFF);
    pcnet_csr_write(3, 0);
    pcnet_csr_write(4, 0);

    pcnet_csr_write(0, 0x0041);
    for (int t = 0; t < TIMEOUT; t++) {
        if (pcnet_csr_read(0) & 0x0100) break;
    }
    if (!(pcnet_csr_read(0) & 0x0100)) { free(ib); return -1; }

    pcnet_csr_write(58, 0x0002);

    pcnet_csr_write(0, 0x0042);
    for (int t = 0; t < TIMEOUT; t++) {
        uint16_t s = pcnet_csr_read(0);
        if ((s & 0x0030) == 0x0030) break;
    }

    initialized = 1;
    free(ib);
    return 0;
}

void pcnet_send(void *data, int len)
{
    if (!initialized) return;
    if (len > BUF_SIZE) len = BUF_SIZE;

    int idx = tx_cur;
    volatile uint16_t *desc = tx_ring + idx * 8;

    for (int t = 0; t < TIMEOUT; t++) {
        if (!(desc[0] & 0x8000)) break;
    }
    if (desc[0] & 0x8000) return;

    memcpy(tx_bufs[idx], data, len);
    desc[0] = 0xB000;
    desc[1] = len | 0x3000;
    tx_cur = (idx + 1) % TX_RING_SIZE;

    pcnet_csr_write(0, 0x0048);
    pcnet_csr_write(0, 0x0042);

    for (int t = 0; t < TIMEOUT; t++) {
        if (!(desc[0] & 0x8000)) break;
    }
}

int pcnet_poll(uint8_t *buf, int max_len)
{
    if (!initialized) return 0;

    int idx = rx_cur;
    volatile uint16_t *desc = rx_ring + idx * 8;

    if (desc[0] & 0x8000) return 0;

    int len = desc[1] & 0x0FFF;
    if (len > max_len) len = max_len;
    if (len > 0) memcpy(buf, rx_bufs[idx], len);

    desc[0] = 0x8000;
    desc[1] = 0xF000;
    rx_cur = (idx + 1) % RX_RING_SIZE;

    return len;
}
