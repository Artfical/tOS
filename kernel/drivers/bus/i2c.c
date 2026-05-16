#include "i2c.h"
#include "io.h"

static void i2c_delay(void)
{
    for (volatile int i = 0; i < 10; i++);
}

int i2c_init(i2c_bus_t *bus, uint16_t io_base)
{
    bus->io_base = io_base;
    bus->speed_khz = 100;
    bus->initialized = 1;
    outb(io_base, 0xFF);
    i2c_delay();
    return 0;
}

int i2c_start(i2c_bus_t *bus)
{
    (void)bus;
    return 0;
}

void i2c_stop(i2c_bus_t *bus)
{
    (void)bus;
}

int i2c_write_byte(i2c_bus_t *bus, uint8_t data)
{
    (void)bus;
    (void)data;
    return 0;
}

uint8_t i2c_read_byte(i2c_bus_t *bus, int ack)
{
    (void)bus;
    (void)ack;
    return 0;
}

int i2c_write(i2c_bus_t *bus, uint8_t addr, const uint8_t *data, int len)
{
    if (i2c_start(bus)) return -1;
    if (i2c_write_byte(bus, addr << 1 | I2C_WRITE)) return -1;
    for (int i = 0; i < len; i++)
        if (i2c_write_byte(bus, data[i])) return -1;
    i2c_stop(bus);
    return 0;
}

int i2c_read(i2c_bus_t *bus, uint8_t addr, uint8_t *data, int len)
{
    if (i2c_start(bus)) return -1;
    if (i2c_write_byte(bus, addr << 1 | I2C_READ)) return -1;
    for (int i = 0; i < len; i++)
        data[i] = i2c_read_byte(bus, i < len - 1);
    i2c_stop(bus);
    return 0;
}
