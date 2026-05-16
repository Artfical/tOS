#include "sb16.h"
#include "io.h"
int sb16_reset(sb16_device_t *dev)
{
    outb(dev->base + SB16_DSP_RESET, 1);
    for (volatile int i = 0; i < 1000; i++);
    outb(dev->base + SB16_DSP_RESET, 0);
    for (volatile int i = 0; i < 100000; i++) {
        if (inb(dev->base + SB16_DSP_READ) == 0xAA) return 0;
    }
    return -1;
}
void sb16_write(sb16_device_t *dev, uint8_t val)
{
    for (volatile int i = 0; i < 100000; i++)
        if (!(inb(dev->base + SB16_DSP_WRITE) & 0x80)) break;
    outb(dev->base + SB16_DSP_WRITE, val);
}
uint8_t sb16_read(sb16_device_t *dev)
{
    for (volatile int i = 0; i < 100000; i++)
        if (inb(dev->base + SB16_DSP_DATA_AVAIL) & 0x80) break;
    return inb(dev->base + SB16_DSP_READ);
}
int sb16_init(sb16_device_t *dev)
{
    dev->present = 0;
    dev->base = 0x220;
    if (sb16_reset(dev)) {
        dev->base = 0x210;
        if (sb16_reset(dev)) {
            dev->base = 0x230;
            if (sb16_reset(dev)) return -1;
        }
    }
    sb16_write(dev, SB16_CMD_DSP_VERSION);
    dev->major_ver = sb16_read(dev);
    dev->minor_ver = sb16_read(dev);
    dev->present = 1;
    dev->irq = 5;
    dev->dma8 = 1;
    dev->dma16 = 5;
    return 0;
}
