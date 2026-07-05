#ifndef VGA_FONT_H
#define VGA_FONT_H

#define VGA_FONT_STYLE_COUNT 10

/* Captures the current (BIOS/GRUB-loaded, correct) character glyph
 * bitmaps from VGA plane 2 into an internal cache, if not already
 * done -- a no-op the first time this or any other vga_font_*()
 * function has already run. Callers that need this captured before
 * anything else touches VBE/plane 2 (see kernel.c's boot sequence,
 * and vga.c's text-mode restore after DOOM/vgatest/3d) should call
 * this explicitly rather than relying on vga_font_load_turkish()/
 * vga_font_set_style() to do it as a side effect, since those aren't
 * always called (e.g. US keyboard layout skips vga_font_load_turkish()
 * entirely). */
void vga_font_capture_base(void);

void vga_font_load_turkish(void);

void vga_font_set_style(int style);
int vga_font_get_style(void);
const char *vga_font_style_name(int style);

#endif
