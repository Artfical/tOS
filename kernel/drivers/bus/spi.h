#ifndef SPI_H
#define SPI_H

#include <stdint.h>

#define SPI_MODE_0 0
#define SPI_MODE_1 1
#define SPI_MODE_2 2
#define SPI_MODE_3 3

typedef struct {
    uint16_t io_base;
    int mode;
    int speed_hz;
    int initialized;
} spi_bus_t;

int spi_init(spi_bus_t *bus, uint16_t io_base, int mode);
int spi_transfer(spi_bus_t *bus, uint8_t tx, uint8_t *rx);
int spi_write(spi_bus_t *bus, const uint8_t *data, int len);
int spi_read(spi_bus_t *bus, uint8_t *data, int len);
void spi_chip_select(spi_bus_t *bus, int select);

#endif
