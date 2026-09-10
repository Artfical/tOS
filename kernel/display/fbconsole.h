#ifndef FBCONSOLE_H
#define FBCONSOLE_H

#include <stdint.h>
#include "bochs.h"

/* Text console rendered onto a real linear framebuffer via the boot
 * font (see vga_font_get_glyph()), instead of VGA text mode's 80x25
 * character cells. This is the rendering foundation the pixel-based
 * desktop is being built on: real glyphs blitted to real pixels, at
 * whatever resolution the framebuffer is running, rather than a
 * fixed 80x25 grid of hardware text-mode cells. */

void fbconsole_init(bochs_device_t *dev);
void fbconsole_clear(uint32_t bg);
void fbconsole_putc(int col, int row, char c, uint32_t fg, uint32_t bg);
void fbconsole_puts(int col, int row, const char *s, uint32_t fg, uint32_t bg);
void fbconsole_fill_rect(int x, int y, int w, int h, uint32_t color);
int  fbconsole_cols(void);
int  fbconsole_rows(void);

#endif
