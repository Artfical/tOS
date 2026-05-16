#include "mda.h"
#include "io.h"
int mda_init(mda_device_t *dev)
{
    dev->present = 0;
    uint8_t status = inb(MDA_STATUS);
    if (status != 0xFF) dev->present = 1;
    return dev->present ? 0 : -1;
}
void mda_write_char(mda_device_t *dev, int row, int col, char c, uint8_t attr)
{
    if (!dev->present) return;
    volatile uint16_t *fb = (volatile uint16_t *)MDA_FB;
    fb[row * MDA_COLS + col] = (uint16_t)(attr << 8) | (uint8_t)c;
}
