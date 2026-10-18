#include "uhci.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "debugmon.h"

#define FRAME_NUM 1024

unsigned char _uhci_dma[65536] __attribute__((aligned(4096)));
static uint32_t _dma_pos;
static uint32_t _dma_base;        /* past the frame list and queue head, which stay for good */

static uhci_controller_t _saved_ctrl;
static uhci_qh_t *intr_qh[UHCI_INTR_SLOTS];
static uhci_td_t *intr_td[UHCI_INTR_SLOTS];
static int intr_armed[UHCI_INTR_SLOTS];
static uint8_t intr_buf[UHCI_INTR_SLOTS][64] __attribute__((aligned(16)));
static int port_addr[2];           /* the address each root port's device was given, 0 = none */

/* waits for a descriptor to finish: at most `ms` milliseconds (and a bounded number of polls,
 * should the clock not run yet) */
static int td_wait(uhci_td_t *td, uint32_t ms)
{
    uint32_t start = debugmon_uptime_ms();
    for (int n = 0; n < 4000000; n++) {
        if (!(td->status & 0x00800000)) return 0;
        if ((n & 255) == 0 && debugmon_uptime_ms() - start > ms) break;
        inb(0x80);
    }
    return -1;
}
static int _uhci_ready = 0;

static void *dma_zalloc(uint32_t sz, uint32_t *phys)
{
    _dma_pos = (_dma_pos + 15) & ~15;
    /* transfer descriptors are only alive for one transfer at a time: once the area is used up,
     * start again from the beginning instead of running off its end */
    if (_dma_pos + ((sz + 15) & ~15) > sizeof(_uhci_dma)) _dma_pos = _dma_base;
    *phys = (uint32_t)(_uhci_dma + _dma_pos);
    void *p = _uhci_dma + _dma_pos;
    _dma_pos += (sz + 15) & ~15;
    memset(p, 0, (sz + 15) & ~15);
    return p;
}

static void klog_hex(uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    klog_write(b);
}

#define r16(c, r) inw((c)->io_base + (r))
#define w16(c, r, v) outw((c)->io_base + (r), (v))
#define r32(c, r) inl((c)->io_base + (r))
#define w32(c, r, v) outl((c)->io_base + (r), (v))

int uhci_init(uhci_controller_t *c)
{
    if (_uhci_ready) {
        *c = _saved_ctrl;
        return 0;
    }
    memset(c, 0, sizeof(*c));
    _dma_pos = 0;

    pci_device_t devs[4];
    int n = pci_find_devices(0x0C, 0x03, devs, 4);
    /* USB controllers share the class: UHCI is programming interface 0x00 (OHCI 0x10, EHCI 0x20,
     * xHCI 0x30), and with -usb plus an EHCI the UHCI is not necessarily the first one */
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++) if (devs[i].prog_if == 0x00) pick = i;
    if (pick < 0) { klog_write("uhci: no UHCI controller on the PCI bus\n"); return -1; }

    c->pci = devs[pick];
    uint32_t bar4 = pci_get_bar(c->pci.bus, c->pci.device, c->pci.func, 4);
    c->io_base = bar4 & 0xFFF0;

    /* take the controller from the BIOS: its USB legacy support (keyboard and mouse emulated
     * through SMIs) must be off before the controller is reset, or the firmware and tOS both
     * drive it and the machine can hang (VMware's BIOS has it on) */
    pci_write_config(c->pci.bus, c->pci.device, c->pci.func, 0xC0, 0x8F00);

    pci_write_config(c->pci.bus, c->pci.device, c->pci.func, 4, 5 | (1u << 10));

    w16(c, 4, 0);          /* no interrupts: everything here is polled */
    w16(c, 0, 0);          /* stop it first */
    for (volatile int d = 0; d < 20000; d++);
    w16(c, 0, 2);
    volatile int t = 50000;
    while (r16(c, 0) & 2 && --t);

    uint32_t flp;
    c->frame_list = dma_zalloc(FRAME_NUM * 4, &flp);
    c->frame_list_phys = flp;

    uint32_t qp;
    c->async_qh = dma_zalloc(32, &qp);

    c->async_qh->link = 1;
    c->async_qh->element = 1;

    /* interrupt pipes (mouse, keyboard) each get a queue head and a descriptor of their own that
     * stay in the schedule, in front of the one control and bulk transfers use */
    uint32_t head = qp;
    for (int i = UHCI_INTR_SLOTS - 1; i >= 0; i--) {
        uint32_t p;
        intr_qh[i] = dma_zalloc(32, &p);
        intr_qh[i]->link = head | 2;
        intr_qh[i]->element = 1;
        intr_td[i] = dma_zalloc(sizeof(uhci_td_t), &p);
        head = (uint32_t)intr_qh[i];
    }

    for (int i = 0; i < FRAME_NUM; i++)
        c->frame_list[i] = head | 2;

    w32(c, 8, flp);
    w16(c, 12, 0x40);
    w16(c, 4, 0);
    w16(c, 0, 1);

    t = 50000;
    while (r16(c, 2) & 0x20 && --t);

    if (!t) {
        klog_write("uhci: controller did not start, io ");
        klog_hex(c->io_base);
        klog_write(" status ");
        klog_hex(r16(c, 2));
        klog_write("\n");
        return -1;
    }
    _dma_base = _dma_pos;
    _saved_ctrl = *c;
    _uhci_ready = 1;
    return 0;
}

/* is something plugged into the root port? (no reset, just the connect status bit) */
int uhci_port_connected(uhci_controller_t *c, int port)
{
    uint16_t s = r16(c, 16 + port * 2);
    /* acknowledge a connect change (write 1 to it) without touching port enable */
    if (s & 2) w16(c, 16 + port * 2, (uint16_t)((s & ~0x0008) | 0x0002));
    if (!(s & 1)) port_addr[port] = 0;      /* unplugged: it gets a fresh address when it is back */
    return s & 1;
}

int uhci_port_detect(uhci_controller_t *c, int port)
{
    int re = 16 + port * 2;
    uint16_t s = r16(c, re);
    if (!(s & 1)) return 0;

    w16(c, re, 0x200);
    for (volatile int d = 0; d < 80000; d++);
    w16(c, re, 0);
    for (volatile int d = 0; d < 80000; d++);

    s = r16(c, re);
    if (s & 2) w16(c, re, s | 2);
    if (!(s & 4)) w16(c, re, s | 4);

    return 1;
}

static inline uint32_t uhci_link_td(void *td)
{
    return (uint32_t)td & 0xFFFFFFF0;
}

#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

#define UHCI_TD_ACTIVE      0x00800000
#define UHCI_TD_ERROR_MASK  0x007E0000
#define UHCI_TD_CERR3       0x18000000
#define UHCI_TD_INIT_STATUS (UHCI_TD_ACTIVE | UHCI_TD_CERR3)

static inline uint32_t uhci_token(uint32_t pid, int dev, int ep, int toggle, int len)
{
    uint32_t maxlen = ((uint32_t)(len - 1)) & 0x7FF;
    return pid | ((uint32_t)(dev & 0x7F) << 8) | ((uint32_t)(ep & 0x0F) << 15) |
           ((uint32_t)(toggle & 1) << 19) | (maxlen << 21);
}

int uhci_control(uhci_controller_t *c, int dev, int ep, usb_device_request_t *req, void *data, int dir)
{
    uint32_t tdp;
    uhci_td_t *td = (uhci_td_t *)dma_zalloc(3 * sizeof(uhci_td_t), &tdp);

    int has_data = data && req->wLength != 0;
    int status_idx = has_data ? 2 : 1;

    td[0].link = uhci_link_td(&td[1]);
    td[0].status = UHCI_TD_INIT_STATUS;
    td[0].token = uhci_token(UHCI_PID_SETUP, dev, ep, 0, 8);
    td[0].buffer = (uint32_t)req;

    if (has_data) {
        td[1].link = uhci_link_td(&td[2]);
        td[1].status = UHCI_TD_INIT_STATUS;
        td[1].token = uhci_token((dir & 0x80) ? UHCI_PID_IN : UHCI_PID_OUT, dev, ep, 1, req->wLength);
        td[1].buffer = (uint32_t)data;
    }

    td[status_idx].link = 1;
    td[status_idx].status = UHCI_TD_INIT_STATUS;
    td[status_idx].buffer = 0;
    td[status_idx].token = uhci_token((dir & 0x80) ? UHCI_PID_OUT : UHCI_PID_IN, dev, ep, 1, 0);

    c->async_qh->element = (uint32_t)&td[0];

    /* done when the status stage is; a stage that fails (a stall, no device) halts the queue, so
     * that ends the wait too instead of running into the timeout */
    uint32_t start = debugmon_uptime_ms();
    int rc = -1;
    for (int n = 0; n < 4000000; n++) {
        if (!(td[status_idx].status & UHCI_TD_ACTIVE)) { rc = (td[status_idx].status & UHCI_TD_ERROR_MASK) ? -1 : 0; break; }
        int failed = 0;
        for (int i = 0; i < status_idx; i++)
            if (!(td[i].status & UHCI_TD_ACTIVE) && (td[i].status & UHCI_TD_ERROR_MASK)) failed = 1;
        if (failed) break;
        if ((n & 255) == 0 && debugmon_uptime_ms() - start > 200) break;
        inb(0x80);
    }

    c->async_qh->element = 1;
    return rc;
}

int uhci_interrupt_read(uhci_controller_t *c, int dev, int ep, int max_len, void *buf)
{
    uint32_t tdp;
    uhci_td_t *td = (uhci_td_t *)dma_zalloc(sizeof(uhci_td_t), &tdp);

    td->link = 1;
    td->status = UHCI_TD_INIT_STATUS;
    td->token = uhci_token(UHCI_PID_IN, dev, ep, 0, max_len);
    td->buffer = (uint32_t)buf;

    c->async_qh->element = (uint32_t)td;
    td_wait(td, 20);
    c->async_qh->element = 1;

    if (td->status & UHCI_TD_ACTIVE) return -1;
    if (td->status & UHCI_TD_ERROR_MASK) return -1;
    return 0;
}

/* Bulk data, up to 32 packets queued at once: the descriptors are linked depth first, so the
 * controller runs as many as fit in each 1 ms frame instead of one packet per frame. */
#define BULK_BATCH 32

int uhci_bulk_transfer(uhci_controller_t *c, int dev, int ep, int dir, void *buf, int len, int maxpacket, int *toggle)
{
    uint8_t *p = (uint8_t *)buf;
    int remaining = len;

    if (maxpacket <= 0) maxpacket = 64;

    do {
        int n = len == 0 ? 1 : (remaining + maxpacket - 1) / maxpacket;
        if (n > BULK_BATCH) n = BULK_BATCH;
        uint32_t tdp;
        uhci_td_t *td = (uhci_td_t *)dma_zalloc((uint32_t)n * sizeof(uhci_td_t), &tdp);
        int t = *toggle;
        uint8_t *q = p;
        int left = remaining;
        for (int i = 0; i < n; i++) {
            int chunk = left > maxpacket ? maxpacket : left;
            td[i].link = i < n - 1 ? (((uint32_t)&td[i + 1]) | 4) : 1;     /* 4: depth first */
            td[i].status = UHCI_TD_INIT_STATUS;
            td[i].token = uhci_token(dir ? UHCI_PID_IN : UHCI_PID_OUT, dev, ep, t, chunk);
            td[i].buffer = (uint32_t)q;
            t ^= 1;
            q += chunk;
            left -= chunk;
        }

        c->async_qh->element = (uint32_t)&td[0];
        /* done when the last packet is; an error halts the queue and ends the wait too */
        uint32_t start = debugmon_uptime_ms();
        int failed = 0, done = 0;
        for (int k = 0; k < 4000000 && !done && !failed; k++) {
            if (!(td[n - 1].status & UHCI_TD_ACTIVE)) { done = 1; break; }
            for (int i = 0; i < n; i++)
                if (!(td[i].status & UHCI_TD_ACTIVE) && (td[i].status & UHCI_TD_ERROR_MASK)) failed = 1;
            if ((k & 255) == 0 && debugmon_uptime_ms() - start > 2000) break;
            inb(0x80);
        }
        c->async_qh->element = 1;

        for (int i = 0; i < n; i++) {
            if ((td[i].status & UHCI_TD_ACTIVE) || (td[i].status & UHCI_TD_ERROR_MASK)) return -1;
            *toggle ^= 1;
        }
        p = q;
        remaining = left;
    } while (remaining > 0);

    return 0;
}

/* An interrupt IN pipe read without waiting: the descriptor stays queued and is looked at on the
 * next call. 1 = a report arrived (copied to buf, at most max_len bytes), 0 = nothing yet,
 * -1 = the device answered with an error (the pipe is re-armed all the same). */
int uhci_interrupt_poll(uhci_controller_t *c, int slot, int dev, int ep, int max_len, void *buf, int *toggle)
{
    (void)c;
    if (slot < 0 || slot >= UHCI_INTR_SLOTS || !intr_td[slot]) return -1;
    if (max_len > 64) max_len = 64;
    uhci_td_t *td = intr_td[slot];
    if (!intr_armed[slot]) {
        td->link = 1;
        td->buffer = (uint32_t)intr_buf[slot];
        td->token = uhci_token(UHCI_PID_IN, dev, ep, *toggle, max_len);
        td->status = UHCI_TD_INIT_STATUS;
        intr_qh[slot]->element = (uint32_t)td;
        intr_armed[slot] = 1;
        return 0;
    }
    uint32_t st = td->status;
    if (st & UHCI_TD_ACTIVE) return 0;
    intr_armed[slot] = 0;
    if (st & UHCI_TD_ERROR_MASK) return -1;
    int got = (int)((st + 1) & 0x7FF);
    if (got > max_len) got = max_len;
    memcpy(buf, intr_buf[slot], (size_t)got);
    *toggle ^= 1;
    return 1;
}

/* The device on a root port, reset and given an address of its own (port 0 gets 1, port 1 gets
 * 2) the first time it is asked for; later calls hand back the same address, so the drivers
 * looking for a disk, a keyboard and a mouse no longer reset each other's devices.
 * 0 = nothing plugged in there. */
int uhci_port_attach(uhci_controller_t *c, int port)
{
    if (port < 0 || port > 1) return 0;
    if (!(r16(c, 16 + port * 2) & 1)) { port_addr[port] = 0; return 0; }
    if (port_addr[port]) return port_addr[port];
    if (!uhci_port_detect(c, port)) return 0;
    usb_device_request_t req;
    uint8_t desc[8] __attribute__((aligned(16)));
    req.bmRequestType = 0x80; req.bRequest = USB_REQ_GET_DESCRIPTOR;
    req.wValue = USB_DEVICE_DESCRIPTOR_TYPE << 8; req.wIndex = 0; req.wLength = 8;
    if (uhci_control(c, 0, 0, &req, desc, 0x80) != 0) return 0;
    req.bmRequestType = 0x00; req.bRequest = USB_REQ_SET_ADDRESS;
    req.wValue = (uint16_t)(port + 1); req.wIndex = 0; req.wLength = 0;
    if (uhci_control(c, 0, 0, &req, 0, 0x00) != 0) return 0;
    uint32_t t0 = debugmon_uptime_ms();
    for (int n = 0; n < 200000 && debugmon_uptime_ms() - t0 < 3; n++) inb(0x80);   /* the device takes 2 ms to switch */
    port_addr[port] = port + 1;
    return port_addr[port];
}
