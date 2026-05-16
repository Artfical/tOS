#include "parallel.h"
#include "io.h"
int parallel_init(parallel_device_t *dev)
{
    dev->present = 0;
    uint16_t ports[] = {PARALLEL_LPT1, PARALLEL_LPT2, PARALLEL_LPT3};
    for (int i = 0; i < 3; i++) {
        dev->io_base = ports[i];
        outb(dev->io_base + PARALLEL_CONTROL, PARALLEL_CTRL_INIT);
        uint8_t status = inb(dev->io_base + PARALLEL_STATUS);
        if (status != 0xFF) {
            dev->present = 1;
            return 0;
        }
    }
    return -1;
}
int parallel_write(parallel_device_t *dev, uint8_t data)
{
    if (!dev->present) return -1;
    outb(dev->io_base, data);
    outb(dev->io_base + PARALLEL_CONTROL, PARALLEL_CTRL_INIT | PARALLEL_CTRL_STROBE);
    for (volatile int i = 0; i < 100; i++);
    outb(dev->io_base + PARALLEL_CONTROL, PARALLEL_CTRL_INIT);
    return 0;
}
uint8_t parallel_read(parallel_device_t *dev)
{
    if (!dev->present) return 0xFF;
    return inb(dev->io_base);
}
