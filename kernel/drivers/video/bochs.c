#include "bochs.h"
#include "io.h"
#include "pci.h"
#include "paging.h"
#include "klog.h"

static int dispi_marked_unusable;
static int vmware_svga_active;
static uint16_t vmware_index_port;
static uint16_t vmware_value_port;

/* Minimal VMware SVGA-II legacy 2D register/FIFO interface. */
#define SVGA_REG_ID              0
#define SVGA_REG_ENABLE          1
#define SVGA_REG_WIDTH           2
#define SVGA_REG_HEIGHT          3
#define SVGA_REG_MAX_WIDTH       4
#define SVGA_REG_MAX_HEIGHT      5
#define SVGA_REG_DEPTH           6
#define SVGA_REG_BPP             7
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_START      18
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22
#define SVGA_ID_2               0x90000002u
#define SVGA_FIFO_MIN           0
#define SVGA_FIFO_MAX           1
#define SVGA_FIFO_NEXT_CMD      2
#define SVGA_FIFO_STOP          3
#define SVGA_CMD_UPDATE         1

static uint32_t svga_read_reg(bochs_device_t *dev, uint32_t index);

static void svga_write_reg(bochs_device_t *dev, uint32_t index, uint32_t value)
{
    outl(dev->vmware_index_port, index);
    outl(dev->vmware_value_port, value);
}

static void svga_update(bochs_device_t *dev, int x, int y, int width, int height)
{
    if (!dev || !dev->is_vmware || !dev->fifo || width <= 0 || height <= 0) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= dev->width || y >= dev->height) return;
    if (x + width > dev->width) width = dev->width - x;
    if (y + height > dev->height) height = dev->height - y;
    if (width <= 0 || height <= 0) return;

    volatile uint32_t *fifo = (volatile uint32_t *)dev->fifo;
    uint32_t min = fifo[0], max = fifo[1], next = fifo[2], stop = fifo[3];
    if (min < 16 || max > dev->fifo_size || max <= min + 24u ||
        next < min || next >= max || stop < min || stop >= max) return;

    uint32_t command_bytes = 5u * sizeof(uint32_t);
    uint32_t available = next >= stop ? (max - next) + (stop - min)
                                      : (stop - next);
    if (available <= command_bytes + sizeof(uint32_t)) {
        /* Legacy sync asks the device to drain pending commands. */
        svga_write_reg(dev, SVGA_REG_SYNC, 1);
        for (uint32_t spin = 0; spin < 100000u && svga_read_reg(dev, SVGA_REG_BUSY); spin++)
            __asm__ volatile("pause");
        next = fifo[2];
        stop = fifo[3];
        available = next >= stop ? (max - next) + (stop - min) : (stop - next);
        if (available <= command_bytes + sizeof(uint32_t)) return;
    }

    uint32_t start = next;
    if (start + command_bytes > max) {
        /* The command is only 20 bytes. Wrap only when the beginning of the
         * queue has room, retaining the mandatory one-word sentinel gap. */
        if (stop <= min + command_bytes + sizeof(uint32_t)) return;
        start = min;
    }
    uint32_t after = start + command_bytes;
    if (after >= max) after = min;
    fifo[start / 4u] = SVGA_CMD_UPDATE;
    fifo[start / 4u + 1u] = (uint32_t)x;
    fifo[start / 4u + 2u] = (uint32_t)y;
    fifo[start / 4u + 3u] = (uint32_t)width;
    fifo[start / 4u + 4u] = (uint32_t)height;
    __asm__ volatile("sfence" ::: "memory");
    fifo[2] = after;
    /* Older SVGA hardware needs the explicit sync register doorbell. */
    svga_write_reg(dev, SVGA_REG_SYNC, 1);
    for (uint32_t spin = 0; spin < 100000u && svga_read_reg(dev, SVGA_REG_BUSY); spin++)
        __asm__ volatile("pause");
}

static uint32_t svga_read_reg(bochs_device_t *dev, uint32_t index)
{
    outl(dev->vmware_index_port, index);
    return inl(dev->vmware_value_port);
}

static int vmware_init(bochs_device_t *dev, const pci_device_t *pdev)
{
    uint32_t bar0 = pci_get_bar(pdev->bus, pdev->device, pdev->func, 0);
    uint32_t bar1 = pci_get_bar(pdev->bus, pdev->device, pdev->func, 1);
    uint32_t bar2 = pci_get_bar(pdev->bus, pdev->device, pdev->func, 2);
    if (!(bar0 & 1u) || bar1 == 0 || bar1 == 0xFFFFFFFFu || (bar1 & 1u) ||
        bar2 == 0 || bar2 == 0xFFFFFFFFu || (bar2 & 1u)) {
        klog_write("svga: invalid VMware BAR layout; keeping VGA text mode\n");
        return -1;
    }

    /* SVGA2 BAR0 uses byte-offset ports (INDEX=0, VALUE=1), unlike the
     * older fixed legacy ports which are four bytes apart. */
    dev->vmware_index_port = (uint16_t)(bar0 & ~3u);
    dev->vmware_value_port = dev->vmware_index_port + 1u;
    vmware_index_port = dev->vmware_index_port;
    vmware_value_port = dev->vmware_value_port;
    dev->lfb = bar1 & ~0xFu;
    dev->fifo = bar2 & ~0xFu;
    dev->fifo_size = 0x10000u; /* SVGA BAR2 is 64KB in the current VMware virtual hardware */

    svga_write_reg(dev, SVGA_REG_ID, SVGA_ID_2);
    if (svga_read_reg(dev, SVGA_REG_ID) != SVGA_ID_2) {
        klog_write("svga: VMware device rejected SVGA ID 2; keeping VGA text mode\n");
        dev->lfb = dev->fifo = 0;
        return -1;
    }

    uint32_t fb_start = svga_read_reg(dev, SVGA_REG_FB_START);
    uint32_t fb_size = svga_read_reg(dev, SVGA_REG_FB_SIZE);
    uint32_t fifo_start = svga_read_reg(dev, SVGA_REG_MEM_START);
    uint32_t fifo_size = svga_read_reg(dev, SVGA_REG_MEM_SIZE);
    if (!fb_start || !fb_size || !fifo_start || fifo_size < 64u) {
        klog_write("svga: VMware framebuffer/FIFO registers invalid; keeping VGA text mode\n");
        dev->lfb = dev->fifo = 0;
        return -1;
    }
    dev->lfb = fb_start;
    dev->fifo = fifo_start;
    dev->fifo_size = fifo_size;
    dev->fb_start = fb_start;
    paging_map_range(dev->lfb, dev->lfb, fb_size, PTE_PRESENT | PTE_WRITABLE);
    paging_map_range(dev->fifo, dev->fifo, fifo_size, PTE_PRESENT | PTE_WRITABLE);
    dev->is_vmware = 1;
    dev->present = 1;
    vmware_svga_active = 1;

    /* VMware presents 2D framebuffer writes only after SVGA_CMD_UPDATE.
     * Set up the minimal FIFO header documented for legacy update commands. */
    volatile uint32_t *fifo = (volatile uint32_t *)dev->fifo;
    fifo[0] = 16u;
    fifo[1] = fifo_size;
    fifo[2] = 16u;
    fifo[3] = 16u;
    __asm__ volatile("sfence" ::: "memory");
    klog_write("svga: VMware SVGA II legacy 2D mode enabled\n");
    return 0;
}

static void log_mode_mismatch(int width, int height, int bpp,
                              uint16_t got_x, uint16_t got_y,
                              uint16_t got_bpp, uint16_t got_virt,
                              uint16_t got_enable)
{
    char line[128]; int n = 0;
    const char *s = "bochs: mode readback mismatch wanted=";
    while (*s) line[n++] = *s++;
    const char *hex = "0123456789ABCDEF";
    uint32_t vals[] = { (uint32_t)width, (uint32_t)height, (uint32_t)bpp,
                        got_x, got_y, got_bpp, got_virt, got_enable };
    for (int v = 0; v < 8; v++) {
        if (v == 3) { s = " got="; while (*s) line[n++] = *s++; }
        if (v == 4 || v == 5 || v == 6 || v == 7) line[n++] = '/';
        uint32_t x = vals[v];
        int first = v < 3 ? 7 : 3;
        for (int shift = first * 4; shift >= 0; shift -= 4)
            line[n++] = hex[(x >> shift) & 0xF];
    }
    line[n++] = '\n'; line[n] = 0;
    klog_write(line);
}

static void bochs_write_reg(uint16_t index, uint16_t val)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, val);
}
static uint16_t bochs_read_reg(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void log_class0300_devices(void)
{
    /* Logs every class 03:00 PCI device's actual vendor/device ID so
     * an unrecognized video adapter can be identified quickly instead
     * of guessing -- this is exactly how the VirtualBox VBoxVGA gap
     * was found; called both when the Bochs DISPI ID check itself
     * fails and when it passes but no PCI BAR match is found, so
     * either failure mode is diagnosable from `dmesg`. */
    pci_device_t pdevs[4];
    int n = pci_find_devices(0x03, 0x00, pdevs, 4);
    for (int i = 0; i < n; i++) {
        char line[64]; int lp = 0;
        const char *p = "bochs: class 03:00 dev vendor=0x";
        while (*p) line[lp++] = *p++;
        for (int k = 12; k >= 0; k -= 4) line[lp++] = "0123456789ABCDEF"[(pdevs[i].vendor_id >> k) & 0xF];
        p = " device=0x";
        while (*p) line[lp++] = *p++;
        for (int k = 12; k >= 0; k -= 4) line[lp++] = "0123456789ABCDEF"[(pdevs[i].device_id >> k) & 0xF];
        line[lp++] = '\n';
        line[lp] = '\0';
        klog_write(line);
    }
}

int bochs_init(bochs_device_t *dev)
{
    if (dispi_marked_unusable) return -1;
    vmware_svga_active = 0;
    dev->is_vmware = 0;
    dev->pitch = 0;
    dev->present = 0;
    dev->lfb = 0;
    pci_device_t vmware_devs[4];
    int vmware_count = pci_find_devices(0x03, 0x00, vmware_devs, 4);
    for (int i = 0; i < vmware_count; i++) {
        if (vmware_devs[i].vendor_id != 0x15AD || vmware_devs[i].device_id != 0x0405)
            continue;
        return vmware_init(dev, &vmware_devs[i]);
    }

    bochs_write_reg(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    uint16_t id = bochs_read_reg(VBE_DISPI_INDEX_ID);
    /* An unimplemented I/O-port read commonly returns 0xFFFF. A range-only
     * check accepts that value, falsely identifies VMware SVGA II as a
     * Bochs DISPI device, switches into an unsupported mode, then silences
     * the text console. Only accept the DISPI revisions whose register
     * layout and LFB mode this driver actually implements. */
    if (id != VBE_DISPI_ID4 && id != VBE_DISPI_ID5) {
        char line[] = "bochs: unsupported DISPI ID 0x0000\n";
        static const char hex[] = "0123456789ABCDEF";
        line[30] = hex[(id >> 12) & 0xF];
        line[31] = hex[(id >> 8) & 0xF];
        line[32] = hex[(id >> 4) & 0xF];
        line[33] = hex[id & 0xF];
        klog_write(line);
        log_class0300_devices();
        return -1;
    }

    dev->present = 1;
    dev->width = 0;
    dev->height = 0;
    dev->bpp = 0;
    dev->lfb = 0;

    /* The linear framebuffer lives behind a PCI device's BAR0 -- the
     * VBE index/data ports alone are enough to *set* a mode, but not
     * to know where its memory is mapped. Different hypervisors
     * expose the same Bochs DISPI register interface through
     * differently-identified PCI video devices, so this needs a
     * vendor/device pair per hypervisor, not just QEMU's:
     *   - QEMU std-vga/bochs-display: vendor 0x1234, device 0x1111
     *   - VirtualBox VBoxVGA:         vendor 0x80EE, device 0xBEEF
     *   - VMware SVGA II:             vendor 0x15AD, device 0x0405
     * (VirtualBox's VBoxVGA deliberately implements the same Bochs
     * VBE interface for exactly this kind of guest compatibility, so
     * everything else here works unchanged once the BAR is found --
     * this was the actual reason DOOM ran fully "headless" and looked
     * hung on VirtualBox: dev->lfb never got set, so bochs_put_pixel()
     * silently no-op'd every frame and the game ran invisibly. The
     * VMware entry is unverified -- added on the strength of VMware
     * SVGA II's documented legacy Bochs-VBE compatibility mode, but
     * not tested against real VMware; the DISPI ID check above still
     * gates everything, so if VMware's adapter doesn't actually answer
     * on those ports this simply won't match and falls through to the
     * same diagnostic logging as any other unrecognized adapter. */
    pci_device_t pdevs[4];
    int n = pci_find_devices(0x03, 0x00, pdevs, 4);
    for (int i = 0; i < n; i++) {
        int is_qemu_std = (pdevs[i].vendor_id == 0x1234 && pdevs[i].device_id == 0x1111);
        int is_vbox_vga = (pdevs[i].vendor_id == 0x80EE && pdevs[i].device_id == 0xBEEF);
        int is_vmware_svga = (pdevs[i].vendor_id == 0x15AD && pdevs[i].device_id == 0x0405);
        if (is_qemu_std || is_vbox_vga || is_vmware_svga) {
            /* VMware SVGA II puts the linear framebuffer in BAR1, not
             * BAR0 -- BAR0 there is an I/O-space BAR for its FIFO/
             * register interface (confirmed on real hardware: BAR0
             * read back as 0x1070, an I/O port range, not a plausible
             * PCI memory address). QEMU's std-vga/bochs-display and
             * VBoxVGA both put it in BAR0. */
            int bar_index = is_vmware_svga ? 1 : 0;
            uint32_t bar = pci_get_bar(pdevs[i].bus, pdevs[i].device, pdevs[i].func, bar_index);
            /* A linear framebuffer must be a nonzero memory BAR. In
             * particular, do not identity-map an I/O BAR as framebuffer. */
            if (bar == 0 || bar == 0xFFFFFFFFu || (bar & 1u)) {
                klog_write("bochs: matching video PCI BAR is not a usable memory aperture\n");
                continue;
            }
            dev->lfb = bar & ~0xFU; /* mask off the low BAR type/flag bits */
            /* This BAR usually sits well above the kernel's normal
             * identity-mapped range (paging_init() only covers the
             * first ~32MB), so it needs its own explicit mapping
             * before it's safe to dereference. 4MB is comfortably
             * more than any mode used here needs (a 1024x768x32bpp
             * frame is ~3MB). */
            paging_map_range(dev->lfb, dev->lfb, 0x400000, PTE_PRESENT | PTE_WRITABLE);
            break;
        }
    }

    if (!dev->lfb) log_class0300_devices();

    return 0;
}

int bochs_set_mode(bochs_device_t *dev, int width, int height, int bpp)
{
    if (!dev->present) return -1;
    if (dev->is_vmware) {
        if (bpp != 32 || width <= 0 || height <= 0 ||
            (uint64_t)(uint32_t)width * (uint32_t)height * 4u > dev->fifo_size * 256u)
            return -1;
        svga_write_reg(dev, SVGA_REG_ENABLE, 0);
        svga_write_reg(dev, SVGA_REG_WIDTH, (uint32_t)width);
        svga_write_reg(dev, SVGA_REG_HEIGHT, (uint32_t)height);
        svga_write_reg(dev, SVGA_REG_BPP, (uint32_t)bpp);
        svga_write_reg(dev, SVGA_REG_ENABLE, 1);
        uint32_t got_width = svga_read_reg(dev, SVGA_REG_WIDTH);
        uint32_t got_height = svga_read_reg(dev, SVGA_REG_HEIGHT);
        uint32_t got_bpp = svga_read_reg(dev, SVGA_REG_BPP);
        uint32_t pitch = svga_read_reg(dev, SVGA_REG_BYTES_PER_LINE);
        uint32_t fb_start = svga_read_reg(dev, SVGA_REG_FB_START);
        uint32_t fb_offset = svga_read_reg(dev, SVGA_REG_FB_OFFSET);
        uint32_t fb_size = svga_read_reg(dev, SVGA_REG_FB_SIZE);
        if (got_width != (uint32_t)width || got_height != (uint32_t)height ||
            got_bpp != (uint32_t)bpp || pitch != (uint32_t)width * 4u ||
            fb_start != dev->fb_start || fb_offset > fb_size ||
            fb_start + fb_offset < fb_start) {
            klog_write("svga: VMware mode readback mismatch; disabling graphics mode\n");
            svga_write_reg(dev, SVGA_REG_ENABLE, 0);
            dev->present = 0;
            dev->lfb = 0;
            return -1;
        }
        dev->fb_offset = fb_offset;
        dev->lfb = fb_start + fb_offset;
        dev->pitch = (int)pitch;
        dev->width = width;
        dev->height = height;
        dev->bpp = bpp;

        volatile uint32_t *fifo = (volatile uint32_t *)dev->fifo;
        fifo[SVGA_FIFO_MIN] = 16u;
        fifo[SVGA_FIFO_MAX] = dev->fifo_size;
        fifo[SVGA_FIFO_NEXT_CMD] = 16u;
        fifo[SVGA_FIFO_STOP] = 16u;
        svga_write_reg(dev, SVGA_REG_CONFIG_DONE, 1);
        return 0;
    }
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_write_reg(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bochs_write_reg(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bochs_write_reg(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    /* Every pixel address this driver computes (here and in
     * fbconsole.c) assumes the scanline pitch is exactly `width`
     * pixels -- true by default on QEMU/Bochs, but never actually
     * requested. Set it explicitly instead of trusting whatever
     * VMware SVGA II's "legacy Bochs compatibility" defaults
     * VIRT_WIDTH to; a real hardware readback of it can be added if
     * this alone doesn't fix the repeating/sheared text seen there. */
    bochs_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);
    bochs_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)height);
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    uint16_t got_x = bochs_read_reg(VBE_DISPI_INDEX_XRES);
    uint16_t got_y = bochs_read_reg(VBE_DISPI_INDEX_YRES);
    uint16_t got_bpp = bochs_read_reg(VBE_DISPI_INDEX_BPP);
    uint16_t got_virt = bochs_read_reg(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t got_enable = bochs_read_reg(VBE_DISPI_INDEX_ENABLE);
    if (got_x != (uint16_t)width || got_y != (uint16_t)height ||
        got_bpp != (uint16_t)bpp || got_virt != (uint16_t)width ||
        (got_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) !=
        (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) {
        log_mode_mismatch(width, height, bpp, got_x, got_y, got_bpp,
                          got_virt, got_enable);
        bochs_disable();
        dispi_marked_unusable = 1;
        dev->present = 0;
        dev->lfb = 0;
        return -1;
    }
    dev->width = width;
    dev->height = height;
    dev->bpp = bpp;
    dev->pitch = width * 4;
    return 0;
}

void bochs_update_rect(bochs_device_t *dev, int x, int y, int width, int height)
{
    if (!dev || !dev->is_vmware || !dev->fifo || width <= 0 || height <= 0) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= dev->width || y >= dev->height) return;
    if (x + width > dev->width) width = dev->width - x;
    if (y + height > dev->height) height = dev->height - y;
    if (width <= 0 || height <= 0) return;

    volatile uint32_t *fifo = (volatile uint32_t *)dev->fifo;
    uint32_t min = fifo[SVGA_FIFO_MIN];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t stop = fifo[SVGA_FIFO_STOP];
    if (min < 16 || max > dev->fifo_size || max <= min + 20u) return;
    uint32_t start = next;
    if (start + 20u > max) start = min;
    uint32_t after = start + 20u;
    if (after >= max) after = min;
    if ((start < stop && after >= stop) || (after < start && (stop > start || stop <= after))) return;

    fifo[start / 4u] = SVGA_CMD_UPDATE;
    fifo[start / 4u + 1u] = (uint32_t)x;
    fifo[start / 4u + 2u] = (uint32_t)y;
    fifo[start / 4u + 3u] = (uint32_t)width;
    fifo[start / 4u + 4u] = (uint32_t)height;
    fifo[SVGA_FIFO_NEXT_CMD] = after;
    svga_write_reg(dev, SVGA_REG_SYNC, 1);
    for (uint32_t spin = 0; spin < 100000u && svga_read_reg(dev, SVGA_REG_BUSY); spin++)
        __asm__ volatile("pause");
}

uint16_t bochs_get_virt_width(void)
{
    if (vmware_svga_active) {
        outl(vmware_index_port, SVGA_REG_WIDTH);
        return (uint16_t)inl(vmware_value_port);
    }
    return bochs_read_reg(VBE_DISPI_INDEX_VIRT_WIDTH);
}

void bochs_disable(void)
{
    if (vmware_svga_active) {
        outl(vmware_index_port, SVGA_REG_ENABLE);
        outl(vmware_value_port, 0);
        vmware_svga_active = 0;
        return;
    }
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

static int g_graphics_active = 0;

void bochs_set_graphics_active(int active) { g_graphics_active = active; }
int  bochs_graphics_active(void) { return g_graphics_active; }

void bochs_put_pixel(bochs_device_t *dev, int x, int y, uint32_t color)
{
    if (!dev->lfb || x >= dev->width || y >= dev->height) return;
    volatile uint32_t *fb = (volatile uint32_t *)dev->lfb;
    fb[((uint32_t)y * (uint32_t)dev->pitch) / 4u + (uint32_t)x] = color;
    svga_update(dev, x, y, 1, 1);
}
