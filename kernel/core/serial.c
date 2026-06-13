#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int serial_initialized = 0;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0F);
    serial_initialized = 1;
}

static int serial_is_transmit_empty(void)
{
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c)
{
    if (!serial_initialized) return;
    int timeout = 100000;
    while (!serial_is_transmit_empty() && timeout-- > 0);
    outb(COM1, c);
}

void serial_write(const char *data)
{
    if (!serial_initialized) return;
    while (*data) {
        if (*data == '\n') serial_putchar('\r');
        serial_putchar(*data);
        data++;
    }
}
