#ifndef VGA_FONT_H
#define VGA_FONT_H

#include <stdint.h>

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

/* Returns the 16-byte, one-bit-per-pixel glyph bitmap for character
 * c from the real BIOS/GRUB-loaded font captured at boot (row 0
 * first, MSB is the leftmost pixel of each row) -- calls
 * vga_font_capture_base() itself if that hasn't happened yet, so
 * this is always safe to call first. Used by framebuffer text
 * rendering (fbconsole.c), which has no VGA plane-2 concept of its
 * own to draw from. */
const uint8_t *vga_font_get_glyph(unsigned char c);

void vga_font_set_style(int style);
int vga_font_get_style(void);
const char *vga_font_style_name(int style);

#endif
