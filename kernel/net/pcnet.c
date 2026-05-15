#include "pcnet.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"

#define TX_RING_SIZE 8
#define RX_RING_SIZE 8
#define BUF_SIZE 1536

static uint16_t io_base = 0;
static uint8_t *tx_bufs[TX_RING_SIZE];
static uint8_t *rx_bufs[RX_RING_SIZE];
static volatile uint16_t *tx_status;
static volatile uint16_t *rx_status;
static int tx_cur = 0;
static int rx_cur = 0;
static void *init_block = 0;
static int initialized = 0;

static uint16_t pcnet_csr_read(int reg)
{
    outw(io_base + 0x00, reg);
    io_wait();
    return inw(io_base + 0x10);
}

static void pcnet_csr_write(int reg, uint16_t val)
{
    outw(io_base + 0x00, reg);
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
    io_base = bar & 0xFFFC;

    outb(io_base + 0x14, 0x0F);
    io_wait();

    int timeout = 0;
    while (!(pcnet_csr_read(0) & 0x0004) && timeout < 100000) timeout++;

    pcnet_csr_write(0, 0x0004);
    timeout = 0;
    while ((pcnet_csr_read(0) & 0x0004) && timeout < 100000) timeout++;
    pci_write_config(bus, dev, func, 0x04, 0x05);

    for (int j = 0; j < 6; j++)
        net_mac[j] = inb(io_base + 0x10 + j + 0x10);

    init_block = malloc(32);
    if (!init_block) return -1;
    memset(init_block, 0, 32);

    uint8_t *ib = (uint8_t *)init_block;
    *(uint16_t *)(ib + 0) = 0x0000;
    for (int j = 0; j < 6; j++) ib[2 + j] = net_mac[j];
    memset(ib + 8, 0, 8);

    uint32_t rx_ring_addr;
    uint32_t tx_ring_addr;

    for (int i = 0; i < RX_RING_SIZE; i++) {
        rx_bufs[i] = (uint8_t *)malloc(BUF_SIZE);
        if (!rx_bufs[i]) return -1;
    }
    for (int i = 0; i < TX_RING_SIZE; i++) {
        tx_bufs[i] = (uint8_t *)malloc(BUF_SIZE);
        if (!tx_bufs[i]) return -1;
    }

    void *rx_ring = malloc(TX_RING_SIZE * 16 + 16);
    void *tx_ring = malloc(RX_RING_SIZE * 16 + 16);
    if (!rx_ring || !tx_ring) return -1;
    memset(rx_ring, 0, TX_RING_SIZE * 16 + 16);
    memset(tx_ring, 0, RX_RING_SIZE * 16 + 16);

    rx_ring_addr = (uint32_t)(uintptr_t)rx_ring;
    tx_ring_addr = (uint32_t)(uintptr_t)tx_ring;

    for (int i = 0; i < RX_RING_SIZE; i++) {
        volatile uint16_t *desc = (volatile uint16_t *)((uint8_t *)rx_ring + i * 16);
        desc[0] = 0x8000;
        desc[1] = 0xF000;
        *(volatile uint32_t *)(desc + 2) = (uint32_t)(uintptr_t)rx_bufs[i];
        desc[4] = 0;
        rx_status = desc;
    }

    for (int i = 0; i < TX_RING_SIZE; i++) {
        volatile uint16_t *desc = (volatile uint16_t *)((uint8_t *)tx_ring + i * 16);
        desc[0] = 0x0000;
        desc[1] = 0x0000;
        *(volatile uint32_t *)(desc + 2) = (uint32_t)(uintptr_t)tx_bufs[i];
        desc[4] = 0;
        tx_status = desc;
    }

    *(uint32_t *)(ib + 16) = rx_ring_addr;
    *(uint32_t *)(ib + 20) = tx_ring_addr;

    pcnet_csr_write(1, (uint32_t)(uintptr_t)init_block & 0xFFFF);
    pcnet_csr_write(2, ((uint32_t)(uintptr_t)init_block >> 16) & 0xFFFF);
    pcnet_csr_write(3, 0x0000);
    pcnet_csr_write(4, 0x0000);

    pcnet_csr_write(0, 0x0041);
    timeout = 0;
    while (!(pcnet_csr_read(0) & 0x0100) && timeout < 100000) timeout++;
    if (!(pcnet_csr_read(0) & 0x0100)) return -1;

    pcnet_csr_write(58, 0x0000);
    pcnet_csr_write(58, 0x0002);
    pcnet_csr_write(58, 0x0000);
    pcnet_csr_write(58, 0x0002);

    pcnet_csr_write(0, 0x0042);
    timeout = 0;
    while ((pcnet_csr_read(0) & 0x0004) && timeout < 100000) timeout++;

    initialized = 1;
    return 0;
}

void pcnet_send(void *data, int len)
{
    if (!initialized) return;
    if (len > BUF_SIZE) len = BUF_SIZE;

    int idx = tx_cur;
    volatile uint16_t *desc = (volatile uint16_t *)((uintptr_t)tx_status + idx * 16);

    int timeout = 0;
    while ((desc[0] & 0x8000) && timeout < 100000) timeout++;
    if (desc[0] & 0x8000) return;

    memcpy(tx_bufs[idx], data, len);
    desc[0] = 0x8000 | 0x4000 | 0x2000;
    desc[1] = len | 0x3000;
    tx_cur = (idx + 1) % TX_RING_SIZE;

    pcnet_csr_write(0, 0x0048);
    pcnet_csr_write(0, 0x0042);

    timeout = 0;
    while ((desc[0] & 0x8000) && timeout < 50000) timeout++;
}

int pcnet_poll(uint8_t *buf, int max_len)
{
    if (!initialized) return 0;

    int idx = rx_cur;
    volatile uint16_t *desc = (volatile uint16_t *)((uintptr_t)rx_status + idx * 16);

    if (desc[0] & 0x8000) return 0;

    int len = desc[1] & 0x0FFF;
    if (len > max_len) len = max_len;
    memcpy(buf, rx_bufs[idx], len);

    desc[0] = 0x8000;
    desc[1] = 0xF000;
    rx_cur = (idx + 1) % RX_RING_SIZE;

    return len;
}
