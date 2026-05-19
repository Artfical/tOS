#include "uhci.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "terminal.h"
#include "string.h"

#define FRAME_NUM 1024

unsigned char _uhci_dma[65536] __attribute__((aligned(4096)));
static uint32_t _dma_pos;

static uhci_controller_t _saved_ctrl;
static int _uhci_ready = 0;

static void *dma_zalloc(uint32_t sz, uint32_t *phys)
{
    _dma_pos = (_dma_pos + 15) & ~15;
    *phys = (uint32_t)(_uhci_dma + _dma_pos);
    void *p = _uhci_dma + _dma_pos;
    _dma_pos += (sz + 15) & ~15;
    memset(p, 0, (sz + 15) & ~15);
    return p;
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
    int n = pci_find_devices(0x0C, 0x00, devs, 4);
    if (!n) return -1;

    c->pci = devs[0];
    uint32_t bar4 = pci_get_bar(c->pci.bus, c->pci.device, c->pci.func, 4);
    c->io_base = bar4 & 0xFFF0;

    pci_write_config(c->pci.bus, c->pci.device, c->pci.func, 4, 5);

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

    for (int i = 0; i < FRAME_NUM; i++)
        c->frame_list[i] = qp | 2;

    w32(c, 8, flp);
    w16(c, 12, 0x40);
    w16(c, 0, 1);

    t = 50000;
    while (r16(c, 2) & 0x20 && --t);

    _saved_ctrl = *c;
    _uhci_ready = 1;
    return t ? 0 : -1;
}

int uhci_port_detect(uhci_controller_t *c, int port)
{
    int re = 16 + port * 2;
    uint16_t s = r16(c, re);
    if (!(s & 1)) return 0;

    w16(c, re, s | 0x100);
    for (volatile int d = 0; d < 80000; d++);
    w16(c, re, (s | 0x100) & ~0x100);
    for (volatile int d = 0; d < 80000; d++);

    s = r16(c, re);
    if (!(s & 4)) w16(c, re, s | 4);
    if (s & 2) w16(c, re, s | 2);

    return 1;
}

static inline uint32_t uhci_link_td(void *td)
{
    return (uint32_t)td & 0xFFFFFFF0;
}

int uhci_control(uhci_controller_t *c, int dev, int ep, usb_device_request_t *req, void *data, int dir)
{
    uhci_td_t td[3] __attribute__((aligned(16)));
    memset(td, 0, sizeof(td));

    td[0].link = uhci_link_td(&td[1]);
    td[0].status = 0x80 | 0x700000;
    td[0].token = (0 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (0 << 19) | (1 << 21);
    td[0].buffer = (uint32_t)req;

    td[1].link = uhci_link_td(&td[2]);
    td[1].status = 0x80 | 0x700000;

    if (data && (dir & 0x80)) {
        td[1].token = (1 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (1 << 19);
        td[1].buffer = (uint32_t)data;
    } else if (data) {
        td[1].token = (2 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (0 << 19);
        td[1].buffer = (uint32_t)data;
    } else {
        td[1].token = (1 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (1 << 19);
        td[1].buffer = 0;
    }

    td[2].link = 1;
    td[2].status = 0x80 | 0x700000;
    td[2].buffer = 0;
    if ((dir & 0x80) || !data)
        td[2].token = (2 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (1 << 19);
    else
        td[2].token = (1 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (1 << 19);

    c->async_qh->element = (uint32_t)&td[0];

    int t = 2000000;
    while (--t) {
        if (!(td[2].status & 0x80)) break;
    }

    c->async_qh->element = 1;

    return (td[2].status & 0x80) ? -1 : 0;
}

int uhci_interrupt_read(uhci_controller_t *c, int dev, int ep, int max_len, void *buf)
{
    uhci_td_t td __attribute__((aligned(16)));
    memset(&td, 0, sizeof(td));

    td.link = 1;
    td.status = 0x80 | 0x700000;
    td.token = (1 << 29) | ((dev & 0x7F) << 8) | ((ep & 0x0F) << 15) | (0 << 19) | (((max_len / 4) - 1) << 21);
    td.buffer = (uint32_t)buf;

    c->async_qh->element = (uint32_t)&td;

    int t = 500000;
    while (--t) {
        if (!(td.status & 0x80)) break;
    }

    c->async_qh->element = 1;

    if (td.status & 0x80) return -1;
    if (td.status & 0x700000) return -1;
    return 0;
}
