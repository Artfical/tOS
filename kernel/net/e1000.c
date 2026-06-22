#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"

#define E1000_NUM_RX_DESC 8
#define E1000_NUM_TX_DESC 8
#define E1000_BUF_SIZE    2048

static const uint16_t e1000_ids[][2] = {
    {0x8086, 0x1000}, {0x8086, 0x1001}, {0x8086, 0x1004},
    {0x8086, 0x1008}, {0x8086, 0x1009}, {0x8086, 0x100C},
    {0x8086, 0x100D}, {0x8086, 0x100E}, {0x8086, 0x100F},
    {0x8086, 0x1010}, {0x8086, 0x1011}, {0x8086, 0x1012},
    {0x8086, 0x1013}, {0x8086, 0x1014}, {0x8086, 0x1015},
    {0x8086, 0x1016}, {0x8086, 0x1017}, {0x8086, 0x1018},
    {0x8086, 0x1019}, {0x8086, 0x101A}, {0x8086, 0x101D},
    {0x8086, 0x105E}, {0x8086, 0x1075}, {0x8086, 0x1076},
    {0x8086, 0x1077}, {0x8086, 0x1078}, {0x8086, 0x1079},
    {0x8086, 0x107A}, {0x8086, 0x107B}, {0x8086, 0x107C},
    {0x8086, 0x108A}, {0x8086, 0x1099}, {0x8086, 0x10B9},
    {0x8086, 0x10C0}, {0x8086, 0x10C9}, {0x8086, 0x10D5},
    {0x8086, 0x10D6}, {0x8086, 0x10D9}, {0x8086, 0x10DA},
    {0x8086, 0x10E5}, {0x8086, 0x10E6}, {0x8086, 0x10E7},
    {0x8086, 0x10E8}, {0x8086, 0x1501}, {0x8086, 0x1502},
    {0x8086, 0x1503}, {0x8086, 0x1521}, {0x8086, 0x1522},
    {0x8086, 0x1523}, {0x8086, 0x1524}, {0x8086, 0x1525},
    {0x8086, 0x1526}, {0x8086, 0x1527}, {0x8086, 0x150C}, /* 82583V */
};

#define E1000_CTRL     0x00000
#define E1000_STATUS   0x00008
#define E1000_EECD     0x00010
#define E1000_ICR      0x000C0
#define E1000_IMS      0x000D0
#define E1000_RCTL     0x00100
#define E1000_TCTL     0x00400
#define E1000_TIPG     0x00410
#define E1000_RDBAL    0x02800
#define E1000_RDBAH    0x02804
#define E1000_RDLEN    0x02808
#define E1000_RDH      0x02810
#define E1000_RDT      0x02818
#define E1000_TDBAL    0x03800
#define E1000_TDBAH    0x03804
#define E1000_TDLEN    0x03808
#define E1000_TDH      0x03810
#define E1000_TDT      0x03818
#define E1000_MTA      0x05200
#define E1000_RA       0x05400

#define E1000_CTRL_FD      0x00000001
#define E1000_CTRL_ASDE    0x00000200
#define E1000_CTRL_SLU     0x00000040
#define E1000_CTRL_RST     0x04000000
#define E1000_RCTL_EN      0x00000002
#define E1000_RCTL_SBP     0x00000004
#define E1000_RCTL_UPE     0x00000008
#define E1000_RCTL_MPE     0x00000010
#define E1000_RCTL_LPE     0x00000020
#define E1000_RCTL_LBM_NONE 0x00000000
#define E1000_RCTL_RDMTS_1_2 0x00000000
#define E1000_RCTL_BAM     0x00008000
#define E1000_RCTL_SECRC   0x04000000
#define E1000_RCTL_BSIZE   0x00000400
#define E1000_TCTL_EN      0x00000002
#define E1000_TCTL_PSP     0x00000008
#define E1000_TCTL_CT      0x00000FF0
#define E1000_TCTL_COLD    0x003FF000

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t cksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

static volatile uint8_t *mmio = 0;
static e1000_tx_desc_t *tx_ring = 0;
static e1000_rx_desc_t *rx_ring = 0;
static uint8_t *tx_bufs[E1000_NUM_TX_DESC];
static uint8_t *rx_bufs[E1000_NUM_RX_DESC];
static int tx_cur = 0;
static int rx_cur = 0;
static int initialized = 0;

static uint32_t e1000_read(uint16_t reg)
{
    return *(volatile uint32_t *)(mmio + reg);
}

static void e1000_write(uint16_t reg, uint32_t val)
{
    *(volatile uint32_t *)(mmio + reg) = val;
}

int e1000_init(void)
{
    pci_device_t devs[4];
    int n = pci_find_devices(0x02, 0x00, devs, 4);
    if (n == 0) return -1;

    int found = 0;
    uint8_t bus = 0, dev = 0, func = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id != 0x8086) continue;
        for (size_t j = 0; j < sizeof(e1000_ids) / sizeof(e1000_ids[0]); j++) {
            if (devs[i].device_id == e1000_ids[j][1]) {
                bus = devs[i].bus;
                dev = devs[i].device;
                func = devs[i].func;
                found = 1;
                break;
            }
        }
        if (found) break;
    }
    if (!found) return -1;

    uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
    if (bar0 & 1) return -1;
    uint32_t mmio_addr = bar0 & 0xFFFFFFF0;

    uint32_t cmd = pci_read_config(bus, dev, func, 0x04);
    cmd |= 0x06;
    pci_write_config(bus, dev, func, 0x04, cmd);

    paging_map_range(mmio_addr, mmio_addr, 0x20000, PTE_WRITABLE);
    mmio = (volatile uint8_t *)(uintptr_t)mmio_addr;

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    int timeout = 0;
    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) && timeout < 1000) timeout++;

    uint32_t mac_low = e1000_read(E1000_RA);
    uint32_t mac_high = e1000_read(E1000_RA + 4);
    net_mac[0] = mac_low & 0xFF;
    net_mac[1] = (mac_low >> 8) & 0xFF;
    net_mac[2] = (mac_low >> 16) & 0xFF;
    net_mac[3] = (mac_low >> 24) & 0xFF;
    net_mac[4] = mac_high & 0xFF;
    net_mac[5] = (mac_high >> 8) & 0xFF;

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_bufs[i] = (uint8_t *)malloc(E1000_BUF_SIZE);
        if (!tx_bufs[i]) return -1;
    }
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_bufs[i] = (uint8_t *)malloc(E1000_BUF_SIZE);
        if (!rx_bufs[i]) return -1;
    }

    void *tx_mem = malloc(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC + 16);
    void *rx_mem = malloc(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC + 16);
    if (!tx_mem || !rx_mem) return -1;
    tx_ring = (e1000_tx_desc_t *)(((uintptr_t)tx_mem + 15) & ~15);
    rx_ring = (e1000_rx_desc_t *)(((uintptr_t)rx_mem + 15) & ~15);
    memset(tx_ring, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    memset(rx_ring, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);

    e1000_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    e1000_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, 0);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                (0x10 << 4) | (0x40 << 12));
    e1000_write(E1000_TIPG, 0x0060200A);

    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_SBP |
                E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM |
                E1000_RCTL_SECRC); /* BSIZE=00 -> 2048-byte buffers, matches E1000_BUF_SIZE */

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_FD);

    initialized = 1;
    return 0;
}

void e1000_send(void *data, int len)
{
    if (!initialized) return;
    if (len > E1000_BUF_SIZE) len = E1000_BUF_SIZE;

    int idx = tx_cur;
    int timeout = 0;
    while ((tx_ring[idx].status & 0xFF) && timeout < 100000) timeout++;

    memcpy(tx_bufs[idx], data, len);
    tx_ring[idx].addr = (uint64_t)(uintptr_t)tx_bufs[idx];
    tx_ring[idx].length = len;
    tx_ring[idx].cmd = 0x0B; /* EOP | IFCS | RS */
    tx_ring[idx].status = 0;

    tx_cur = (idx + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_cur);

    timeout = 0;
    while (!(tx_ring[idx].status & 0xFF) && timeout < 100000) timeout++;
}

int e1000_poll(uint8_t *buf, int max_len)
{
    if (!initialized) return 0;

    int idx = rx_cur;
    if (!(rx_ring[idx].status & 0x01)) return 0;

    int len = rx_ring[idx].length;
    if (len > max_len) len = max_len;
    memcpy(buf, rx_bufs[idx], len);

    rx_ring[idx].status = 0;
    rx_cur = (idx + 1) % E1000_NUM_RX_DESC;
    e1000_write(E1000_RDT, idx);

    return len;
}
