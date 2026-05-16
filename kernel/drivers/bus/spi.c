#include "spi.h"
#include "io.h"
#include <stddef.h>

int spi_init(spi_bus_t *bus, uint16_t io_base, int mode)
{
    bus->io_base = io_base;
    bus->mode = mode;
    bus->speed_hz = 1000000;
    bus->initialized = 1;
    return 0;
}

int spi_transfer(spi_bus_t *bus, uint8_t tx, uint8_t *rx)
{
    (void)bus;
    outb(bus->io_base, tx);
    if (rx) *rx = inb(bus->io_base);
    return 0;
}

int spi_write(spi_bus_t *bus, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
        spi_transfer(bus, data[i], NULL);
    return 0;
}

int spi_read(spi_bus_t *bus, uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
        spi_transfer(bus, 0xFF, &data[i]);
    return 0;
}

void spi_chip_select(spi_bus_t *bus, int select)
{
    (void)bus;
    (void)select;
}
