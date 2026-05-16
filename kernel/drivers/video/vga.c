#include "vga.h"
#include "io.h"
void vga_init(void)
{
    vga_set_mode(VGA_MODE_TEXT);
}
void vga_set_mode(uint8_t mode)
{
    (void)mode;
}
void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    outb(VGA_AC_ADDR, index);
    outb(VGA_AC_DATA, r >> 2);
    outb(VGA_AC_DATA, g >> 2);
    outb(VGA_AC_DATA, b >> 2);
}
void vga_write_pixel(int x, int y, uint8_t color)
{
    (void)x;
    (void)y;
    (void)color;
}
