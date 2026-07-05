#include "bochs.h"
#include "io.h"
#include "pci.h"
#include "paging.h"

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

    /* The linear framebuffer lives behind the VGA-compatible PCI
     * device's BAR0 (QEMU's std/bochs-display adapter: vendor 0x1234,
     * device 0x1111, class 03:00). Without this, dev->lfb stays 0 and
     * bochs_put_pixel() silently no-ops -- the VBE index/data ports
     * alone are enough to *set* a mode, but not to know where its
     * memory is mapped. */
    pci_device_t pdevs[4];
    int n = pci_find_devices(0x03, 0x00, pdevs, 4);
    for (int i = 0; i < n; i++) {
        if (pdevs[i].vendor_id == 0x1234 && pdevs[i].device_id == 0x1111) {
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
