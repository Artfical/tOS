#include "bochs.h"
#include "io.h"
#include "pci.h"
#include "paging.h"
#include "klog.h"

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

int bochs_init(bochs_device_t *dev)
{
    bochs_write_reg(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    uint16_t id = bochs_read_reg(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID4) return -1;

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
     * (VirtualBox's VBoxVGA deliberately implements the same Bochs
     * VBE interface for exactly this kind of guest compatibility, so
     * everything else here works unchanged once the BAR is found --
     * this was the actual reason DOOM ran fully "headless" and looked
     * hung on VirtualBox: dev->lfb never got set, so bochs_put_pixel()
     * silently no-op'd every frame and the game ran invisibly.) */
    pci_device_t pdevs[4];
    int n = pci_find_devices(0x03, 0x00, pdevs, 4);
    for (int i = 0; i < n; i++) {
        int is_qemu_std = (pdevs[i].vendor_id == 0x1234 && pdevs[i].device_id == 0x1111);
        int is_vbox_vga = (pdevs[i].vendor_id == 0x80EE && pdevs[i].device_id == 0xBEEF);
        if (is_qemu_std || is_vbox_vga) {
            uint32_t bar0 = pci_get_bar(pdevs[i].bus, pdevs[i].device, pdevs[i].func, 0);
            dev->lfb = bar0 & ~0xFU; /* mask off the low BAR type/flag bits */
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

    if (!dev->lfb) {
        /* Log every class 03:00 device's actual vendor/device ID so a
         * third hypervisor's video adapter can be recognized quickly
         * next time instead of guessing -- this is exactly how the
         * VirtualBox VBoxVGA gap above was found. */
        for (int i = 0; i < n; i++) {
            char line[64]; int lp = 0;
            const char *p = "bochs: unmatched class 03:00 dev vendor=0x";
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

    return 0;
}

int bochs_set_mode(bochs_device_t *dev, int width, int height, int bpp)
{
    if (!dev->present) return -1;
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_write_reg(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bochs_write_reg(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bochs_write_reg(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    dev->width = width;
    dev->height = height;
    dev->bpp = bpp;
    return 0;
}

void bochs_disable(void)
{
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

void bochs_put_pixel(bochs_device_t *dev, int x, int y, uint32_t color)
{
    if (!dev->lfb || x >= dev->width || y >= dev->height) return;
    volatile uint32_t *fb = (volatile uint32_t *)dev->lfb;
    fb[y * dev->width + x] = color;
}
