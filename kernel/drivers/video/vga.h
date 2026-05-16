#ifndef VGA_H
#define VGA_H
#include <stdint.h>
#define VGA_CRTC_ADDR 0x3D4
#define VGA_CRTC_DATA 0x3D5
#define VGA_MISC_OUT 0x3C2
#define VGA_SEQ_ADDR 0x3C4
#define VGA_SEQ_DATA 0x3C5
#define VGA_GC_ADDR 0x3CE
#define VGA_GC_DATA 0x3CF
#define VGA_AC_ADDR 0x3C0
#define VGA_AC_DATA 0x3C1
#define VGA_INSTAT 0x3DA
#define VGA_MODE_TEXT 0x03
#define VGA_MODE_GRAPHICS 0x12
#define VGA_MODE_320x200 0x13
#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25
void vga_init(void);
void vga_set_mode(uint8_t mode);
void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void vga_write_pixel(int x, int y, uint8_t color);
#endif
