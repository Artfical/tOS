#include "pcnet.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "terminal.h"

#define RX_RING 4
#define TX_RING 4
#define BUF_SZ 1536
#define TMO 5000

/* Same defense as the E1000/RTL8139 drivers (see HEAP_DEBUG_LOG.md):
 * rxb[] is only ever written by the NIC's own DMA engine, and the
 * receive descriptor's buffer-length field (below) is the only thing
 * telling the chip how big each buffer is -- give it zero slack over
 * that and a single stray byte of overrun lands directly in the next
 * heap block's header. This padding costs nothing at runtime and
 * isn't advertised to the chip. */
#define RX_PAD 128

typedef struct {
    uint16_t mode;
    uint16_t tlen_rlen;
    uint8_t  phys_addr[6];
    uint16_t reserved;
    uint32_t filter[2];
    uint32_t rx_ring;
    uint32_t tx_ring;
} __attribute__((packed)) pcnet_init_block_t;

typedef struct {
    uint32_t base;
    int16_t  length;  /* two's complement of buffer/frame length */
    uint16_t status;
    uint32_t misc;    /* msg_length (RX) / misc error flags (TX), written by chip */
    uint32_t reserved;
} __attribute__((packed)) pcnet_desc_t;

static uint16_t io = 0;
static uint8_t *rxb[RX_RING];
static uint8_t *txb[TX_RING];
static void *rxm = 0, *txm = 0;
static volatile pcnet_desc_t *rxd = 0, *txd = 0;
static int txi = 0, rxi = 0;
static int ok = 0;

/* WIO (16-bit) port map: RDP=+0x10, RAP=+0x12, RESET=+0x14, BDP=+0x16 */
static uint16_t csr_rd(int r)
{
    outw(io + 0x12, r); io_wait();
    return inw(io + 0x10); io_wait();
}
static void csr_wr(int r, uint16_t v)
{
    outw(io + 0x12, r); io_wait();
    outw(io + 0x10, v); io_wait();
}
static void bcr_wr(int r, uint16_t v)
{
    outw(io + 0x12, r); io_wait();
    outw(io + 0x16, v); io_wait();
}

int pcnet_init(void)
{
    pci_device_t devs[4];
    int n = pci_find_devices(0x02, 0x00, devs, 4);
    if (n == 0) return -1;

    int f = 0;
    uint8_t bus = 0, dev = 0, func = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == 0x1022 &&
            (devs[i].device_id == 0x2000 ||
             devs[i].device_id == 0x2001 ||
             devs[i].device_id == 0x2002 ||
             devs[i].device_id == 0x2625)) { /* 0x2625 = Am79C973 PCnet-FAST III */
            bus = devs[i].bus; dev = devs[i].device; func = devs[i].func; f = 1; break;
        }
    }
    if (!f) return -1;

    uint32_t bar = pci_get_bar(bus, dev, func, 0);
    serial_write("pcnet: bar0=0x");
    for (int k = 28; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(bar >> k) & 0xF]);
    serial_write(" ");
    if (!(bar & 1)) {
        serial_write("not I/O, trying BAR1-5\n");
        int found_io = 0;
        for (int b = 1; b <= 5; b++) {
            bar = pci_get_bar(bus, dev, func, b);
            for (int k = 28; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(bar >> k) & 0xF]);
            serial_write(b < 5 ? " " : "\n");
            if (bar & 1) { found_io = 1; break; }
        }
        if (!found_io) return -1;
    }
    io = bar & 0xFFFC;
    serial_write("io=0x");
    for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(io >> k) & 0xF]);
    serial_write("\n");

    pci_write_config(bus, dev, func, 4, 0x05);
    io_wait();

    serial_write("pcnet: soft reset...\n");
    csr_wr(0, 0x0004);
    int initrdy_ok = 0;
    for (int t = 0; t < TMO; t++) {
        uint16_t csr = csr_rd(0);
        if (csr & 0x0004) { initrdy_ok = 1; break; }
    }
    serial_write("pcnet: initrdy=");
    serial_putchar('0' + initrdy_ok);
    serial_write(" csr0=0x");
    uint16_t csr0_pre = csr_rd(0);
    for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(csr0_pre >> k) & 0xF]);
    serial_write("\n");

    if (!initrdy_ok) {
        serial_write("pcnet: trying RESET port...\n");
        inw(io + 0x14);
        io_wait();
        for (int t = 0; t < TMO; t++) {
            uint16_t csr = csr_rd(0);
            if (csr & 0x0004) { initrdy_ok = 1; break; }
        }
        serial_write("pcnet: after RESET initrdy=");
        serial_putchar('0' + initrdy_ok);
        serial_write(" csr0=0x");
        csr0_pre = csr_rd(0);
        for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(csr0_pre >> k) & 0xF]);
        serial_write("\n");
    }

    uint8_t mac[6];
    for (int j = 0; j < 3; j++) {
        uint16_t w = inw(io + j * 2);
        mac[j*2] = w & 0xFF;
        mac[j*2+1] = (w >> 8) & 0xFF;
    }
    for (int j = 0; j < 6; j++) net_mac[j] = mac[j];
    serial_write("pcnet: mac=");
    for (int j = 0; j < 6; j++) { for (int k = 4; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(mac[j] >> k) & 0xF]); if (j<5) serial_write(":"); }
    serial_write("\n");

    bcr_wr(20, 2); /* SWSTYLE=2: 32-bit PCnet-PCI init block/descriptor format */

    pcnet_init_block_t *ib = malloc(sizeof(pcnet_init_block_t));
    if (!ib) return -1;
    memset(ib, 0, sizeof(*ib));
    ib->mode = 0;
    ib->tlen_rlen = (2 << 12) | (2 << 4); /* log2(TX_RING)=log2(RX_RING)=2 */
    memcpy(ib->phys_addr, mac, 6);
    ib->reserved = 0;
    ib->filter[0] = 0;
    ib->filter[1] = 0;

    for (int i = 0; i < RX_RING; i++) {
        rxb[i] = malloc(BUF_SZ + RX_PAD);
        if (!rxb[i]) { free(ib); return -1; }
    }
    for (int i = 0; i < TX_RING; i++) {
        txb[i] = malloc(BUF_SZ);
        if (!txb[i]) { free(ib); return -1; }
    }

    rxm = malloc(RX_RING * sizeof(pcnet_desc_t) + 16);
    txm = malloc(TX_RING * sizeof(pcnet_desc_t) + 16);
    if (!rxm || !txm) { free(ib); return -1; }

    uintptr_t rra = ((uintptr_t)rxm + 15) & ~15;
    uintptr_t tra = ((uintptr_t)txm + 15) & ~15;
    rxd = (volatile pcnet_desc_t *)rra;
    txd = (volatile pcnet_desc_t *)tra;
    memset((void *)rra, 0, RX_RING * sizeof(pcnet_desc_t));
    memset((void *)tra, 0, TX_RING * sizeof(pcnet_desc_t));

    for (int i = 0; i < RX_RING; i++) {
        rxd[i].base = (uint32_t)(uintptr_t)rxb[i];
        rxd[i].length = (int16_t)(-BUF_SZ);
        rxd[i].status = 0x8000;
        rxd[i].misc = 0;
        rxd[i].reserved = 0;
    }
    for (int i = 0; i < TX_RING; i++) {
        txd[i].base = (uint32_t)(uintptr_t)txb[i];
        txd[i].length = 0;
        txd[i].status = 0;
        txd[i].misc = 0;
        txd[i].reserved = 0;
    }

    ib->rx_ring = (uint32_t)rra;
    ib->tx_ring = (uint32_t)tra;

    csr_wr(1, (uint32_t)(uintptr_t)ib & 0xFFFF);
    csr_wr(2, ((uint32_t)(uintptr_t)ib >> 16) & 0xFFFF);
    csr_wr(3, 0);
    csr_wr(4, 0);

    serial_write("pcnet: clearing errors...\n");
    uint32_t cmd_reg = pci_read_config(bus, dev, func, 4);
    serial_write("pcnet: cmd reg=0x");
    for (int k = 28; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(cmd_reg >> k) & 0xF]);
    serial_write("\n");
    for (int attempt = 0; attempt < 3; attempt++) {
        csr_wr(0, 0x7F7F);
        io_wait();
        uint16_t csr_cleared = csr_rd(0);
        serial_write("pcnet: attempt");
        serial_putchar('0' + attempt);
        serial_write(" csr0=0x");
        for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(csr_cleared >> k) & 0xF]);
        serial_write("\n");
        if (csr_cleared != 0x027D) { serial_write("pcnet: clear worked!\n"); break; }
    }

    serial_write("pcnet: issuing INIT...\n");
    csr_wr(0, 0x0001);
    for (int t = 0; t < TMO; t++) {
        uint16_t cs = csr_rd(0);
        if (cs & 0x0100) break;
    }
    uint16_t csr0 = csr_rd(0);
    serial_write("pcnet: post-INIT csr0=0x");
    for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(csr0 >> k) & 0xF]);
    serial_write(" idon=");
    serial_putchar('0' + ((csr0 >> 8) & 1));
    serial_write("\n");
    if (!(csr0 & 0x0100)) { free(ib); return -1; }

    bcr_wr(58, 2);

    serial_write("pcnet: issuing STRT...\n");
    csr_wr(0, 0x0002);
    for (int t = 0; t < TMO; t++) {
        uint16_t s = csr_rd(0);
        if ((s & 0x0030) == 0x0030) break;
    }
    csr0 = csr_rd(0);
    serial_write("pcnet: post-STRT csr0=0x");
    for (int k = 12; k >= 0; k -= 4) serial_putchar("0123456789ABCDEF"[(csr0 >> k) & 0xF]);
    serial_write(" rxontxon=");
    serial_putchar('0' + ((csr0 >> 4) & 1));
    serial_putchar('0' + ((csr0 >> 5) & 1));
    serial_write("\n");

    ok = 1;
    free(ib);
    serial_write("pcnet: done\n");
    return 0;
}

void pcnet_send(void *data, int len)
{
    if (!ok) return;
    if (len > BUF_SZ) len = BUF_SZ;
    int i = txi;
    volatile pcnet_desc_t *d = &txd[i];
    for (int t = 0; t < TMO; t++) { if (!(d->status & 0x8000)) break; }
    if (d->status & 0x8000) return;
    memcpy(txb[i], data, len);
    d->length = (int16_t)(-len);
    d->status = 0x8300; /* OWN | STP | ENP */
    txi = (i + 1) % TX_RING;
    csr_wr(0, 0x0008); /* TDMD: poke chip to fetch ready descriptor now (polling mode, no IENA) */
    for (int t = 0; t < TMO; t++) { if (!(d->status & 0x8000)) break; }
}

int pcnet_poll(uint8_t *buf, int max)
{
    if (!ok) return 0;
    int i = rxi;
    volatile pcnet_desc_t *d = &rxd[i];
    if (d->status & 0x8000) return 0;
    int l = d->misc & 0x0FFF;
    if (l > BUF_SZ) l = BUF_SZ;
    if (l > max) l = max;
    if (l > 0) memcpy(buf, rxb[i], l);
    d->length = (int16_t)(-BUF_SZ);
    d->status = 0x8000;
    rxi = (i + 1) % RX_RING;
    return l;
}
