#ifndef VGA_FONT_H
#define VGA_FONT_H

#define VGA_FONT_STYLE_COUNT 10

void vga_font_load_turkish(void);

void vga_font_set_style(int style);
int vga_font_get_style(void);
const char *vga_font_style_name(int style);

#endif
