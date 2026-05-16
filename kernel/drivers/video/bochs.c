#include "bochs.h"
#include "io.h"
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
    return 0;
}
int bochs_set_mode(bochs_device_t *dev, int width, int height, int bpp)
{
    if (!dev->present) return -1;
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_write_reg(VBE_DISPI_INDEX_XRES, width);
    bochs_write_reg(VBE_DISPI_INDEX_YRES, height);
    bochs_write_reg(VBE_DISPI_INDEX_BPP, bpp);
    bochs_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    dev->width = width;
    dev->height = height;
    dev->bpp = bpp;
    return 0;
}
void bochs_put_pixel(bochs_device_t *dev, int x, int y, uint32_t color)
{
    if (!dev->lfb || x >= dev->width || y >= dev->height) return;
    volatile uint32_t *fb = (volatile uint32_t *)dev->lfb;
    fb[y * dev->width + x] = color;
}
