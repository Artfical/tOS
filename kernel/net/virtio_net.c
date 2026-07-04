#include "virtio_net.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

#define VIRTIO_VENDOR_ID      0x1AF4
#define VIRTIO_NET_DEVICE_ID  0x1000

/* Legacy virtio PCI I/O registers (BAR0) */
#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_NUM      0x0C
#define VIRTIO_PCI_QUEUE_SEL      0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_ISR            0x13
#define VIRTIO_PCI_CONFIG         0x14  /* device-specific config (no MSI-X) */

#define VIRTIO_STATUS_ACK         0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04

#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

#define VRING_ALIGN   4096
#define RX_QUEUE      0
#define TX_QUEUE      1
#define NUM_RX_BUFS   8
#define NUM_TX_BUFS   8
#define NET_HDR_LEN   10
#define BUF_SIZE      (NET_HDR_LEN + 1514)

/* Same defense as the other NIC drivers (see HEAP_DEBUG_LOG.md): a
 * virtio-net host backend is expected to never write past the
 * descriptor length it was given, but this costs nothing and isn't
 * advertised to the device, so it's cheap insurance against a
 * misbehaving backend. */
#define RX_PAD 128

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[]; /* + used_event, reserved but unused */
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[]; /* + avail_event, reserved but unused */
} __attribute__((packed)) virtq_used_t;

typedef struct {
    uint16_t num;
    virtq_desc_t *desc;
    virtq_avail_t *avail;
    volatile virtq_used_t *used;
    uint16_t last_used;
} virtq_t;

static uint16_t io_base = 0;
static virtq_t rxq, txq;
static uint8_t *rx_bufs[NUM_RX_BUFS];
static uint8_t *tx_bufs[NUM_TX_BUFS];
static int tx_next = 0;
static uint16_t tx_last_used = 0;
static int ready = 0;

static uint32_t vring_size(uint16_t num)
{
    uint32_t first = 16u * num + 2u * (3 + num);
    first = (first + VRING_ALIGN - 1) & ~(uint32_t)(VRING_ALIGN - 1);
    return first + 2u * 3 + 8u * num;
}

static int virtq_setup(int idx, virtq_t *q)
{
    outw(io_base + VIRTIO_PCI_QUEUE_SEL, (uint16_t)idx);
    uint16_t num = inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (num == 0) return -1;

    uint32_t size = vring_size(num);
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t base = alloc_physical_page();
    if (!base) return -1;
    for (uint32_t i = 1; i < pages; i++) {
        if (!alloc_physical_page()) return -1;
    }
    memset((void *)(uintptr_t)base, 0, (size_t)pages * PAGE_SIZE);

    uint32_t avail_off = 16u * num;
    uint32_t used_off = avail_off + 2u * (3 + num);
    used_off = (used_off + VRING_ALIGN - 1) & ~(uint32_t)(VRING_ALIGN - 1);

    q->num = num;
    q->desc = (virtq_desc_t *)(uintptr_t)base;
    q->avail = (virtq_avail_t *)(uintptr_t)(base + avail_off);
    q->used = (virtq_used_t *)(uintptr_t)(base + used_off);
    q->last_used = 0;

    outl(io_base + VIRTIO_PCI_QUEUE_PFN, base / PAGE_SIZE);
    return 0;
}

int virtio_net_init(void)
{
    pci_device_t devs[8];
    int n = pci_find_devices(0x02, 0x00, devs, 8);
    if (n == 0) return -1;

    int found = 0;
    uint8_t bus = 0, dev = 0, func = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == VIRTIO_VENDOR_ID && devs[i].device_id == VIRTIO_NET_DEVICE_ID) {
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
    cmd |= 0x07; /* I/O space + mem space + bus master */
    pci_write_config(bus, dev, func, 0x04, cmd);

    outb(io_base + VIRTIO_PCI_STATUS, 0);
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, 0);

    for (int i = 0; i < 6; i++)
        net_mac[i] = inb(io_base + VIRTIO_PCI_CONFIG + i);

    if (virtq_setup(RX_QUEUE, &rxq) != 0) return -1;
    if (virtq_setup(TX_QUEUE, &txq) != 0) return -1;

    int nrx = NUM_RX_BUFS < rxq.num ? NUM_RX_BUFS : rxq.num;
    for (int i = 0; i < nrx; i++) {
        rx_bufs[i] = (uint8_t *)malloc(BUF_SIZE + RX_PAD);
        if (!rx_bufs[i]) return -1;
        rxq.desc[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rxq.desc[i].len = BUF_SIZE;
        rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        rxq.desc[i].next = 0;
        rxq.avail->ring[i] = i;
    }
    rxq.avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    rxq.avail->idx = (uint16_t)nrx;

    int ntx = NUM_TX_BUFS < txq.num ? NUM_TX_BUFS : txq.num;
    for (int i = 0; i < ntx; i++) {
        tx_bufs[i] = (uint8_t *)malloc(BUF_SIZE);
        if (!tx_bufs[i]) return -1;
    }
    txq.avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE);

    ready = 1;
    serial_write("virtio-net: ready\n");
    return 0;
}

void virtio_net_send(void *data, int len)
{
    if (!ready) return;
    if (len > BUF_SIZE - NET_HDR_LEN) len = BUF_SIZE - NET_HDR_LEN;

    int slot = tx_next;
    int timeout = 0;
    while ((uint16_t)(txq.used->idx - tx_last_used) >= NUM_TX_BUFS && timeout < 1000000) timeout++;

    memset(tx_bufs[slot], 0, NET_HDR_LEN);
    memcpy(tx_bufs[slot] + NET_HDR_LEN, data, len);

    txq.desc[slot].addr = (uint64_t)(uintptr_t)tx_bufs[slot];
    txq.desc[slot].len = NET_HDR_LEN + len;
    txq.desc[slot].flags = 0;
    txq.desc[slot].next = 0;

    uint16_t avail_idx = txq.avail->idx;
    txq.avail->ring[avail_idx % txq.num] = (uint16_t)slot;
    txq.avail->idx = avail_idx + 1;

    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, TX_QUEUE);

    tx_next = (slot + 1) % NUM_TX_BUFS;

    timeout = 0;
    while (txq.used->idx == tx_last_used && timeout < 1000000) timeout++;
    tx_last_used = txq.used->idx;
}

int virtio_net_poll(uint8_t *buf, int max_len)
{
    if (!ready) return 0;
    if (rxq.used->idx == rxq.last_used) return 0;

    virtq_used_elem_t elem = rxq.used->ring[rxq.last_used % rxq.num];
    rxq.last_used++;

    int desc_id = (int)elem.id;
    if (desc_id < 0 || desc_id >= NUM_RX_BUFS) return 0;
    int data_len = (int)elem.len - NET_HDR_LEN;
    if (data_len < 0) data_len = 0;
    /* Never trust the device-reported length past what the buffer
     * actually holds, regardless of how much padding it has. */
    if (data_len > BUF_SIZE - NET_HDR_LEN) data_len = BUF_SIZE - NET_HDR_LEN;
    if (data_len > max_len) data_len = max_len;
    memcpy(buf, rx_bufs[desc_id] + NET_HDR_LEN, data_len);

    uint16_t avail_idx = rxq.avail->idx;
    rxq.avail->ring[avail_idx % rxq.num] = (uint16_t)desc_id;
    rxq.avail->idx = avail_idx + 1;
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE);

    return data_len;
}
