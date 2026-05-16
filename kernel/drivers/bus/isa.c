#include "isa.h"
#include "io.h"

int isa_detect_device(isa_device_t *dev, uint16_t io_port, uint8_t expected_id)
{
    uint8_t id = inb(io_port);
    if (id == 0xFF || id == 0x00) return -1;
    if (expected_id && id != expected_id) return -1;
    if (dev) {
        dev->io_base = io_port;
        dev->irq = 0;
        dev->dma = 0;
    }
    return 0;
}

void isa_system_reset(void)
{
    outb(0x64, 0xFE);
}
