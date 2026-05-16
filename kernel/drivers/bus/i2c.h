#ifndef I2C_H
#define I2C_H

#include <stdint.h>

#define I2C_SCL 0
#define I2C_SDA 1
#define I2C_READ 1
#define I2C_WRITE 0

typedef struct {
    uint16_t io_base;
    int speed_khz;
    int initialized;
} i2c_bus_t;

int i2c_init(i2c_bus_t *bus, uint16_t io_base);
int i2c_start(i2c_bus_t *bus);
void i2c_stop(i2c_bus_t *bus);
int i2c_write_byte(i2c_bus_t *bus, uint8_t data);
uint8_t i2c_read_byte(i2c_bus_t *bus, int ack);
int i2c_write(i2c_bus_t *bus, uint8_t addr, const uint8_t *data, int len);
int i2c_read(i2c_bus_t *bus, uint8_t addr, uint8_t *data, int len);

#endif
