#include "fbconsole.h"
#include "vga_font.h"

#define GLYPH_W 8
#define GLYPH_H 16

static bochs_device_t *fb_dev = 0;
static int fb_cols = 0;
static int fb_rows = 0;

void fbconsole_init(bochs_device_t *dev)
{
    fb_dev = dev;
    fb_cols = dev->width / GLYPH_W;
    fb_rows = dev->height / GLYPH_H;
    /* Font capture is the caller's job now, done before the VBE mode
     * switch -- see fbconsole_prepare_font() and its call site. */
}

void fbconsole_prepare_font(void)
{
    /* Reads the real boot font out of VGA plane 2 via the legacy
     * Sequencer/Graphics Controller registers (0x3C4/0x3CE) -- must
     * happen before bochs_set_mode() touches VBE, not after. Once VBE
     * owns the display these legacy registers no longer mean what
     * font_read() assumes they mean, and poking them anyway produced
     * an immediate, unrecoverable hang on real hardware (unnoticed in
     * QEMU's more forgiving Bochs emulation). */
    vga_font_capture_base();
}

int fbconsole_cols(void) { return fb_cols; }
int fbconsole_rows(void) { return fb_rows; }

void fbconsole_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!fb_dev || !fb_dev->lfb) return;
    volatile uint32_t *fb = (volatile uint32_t *)fb_dev->lfb;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb_dev->width ? fb_dev->width : x + w;
    int y1 = y + h > fb_dev->height ? fb_dev->height : y + h;

    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            fb[py * fb_dev->width + px] = color;
}

void fbconsole_clear(uint32_t bg)
{
    if (!fb_dev) return;
    fbconsole_fill_rect(0, 0, fb_dev->width, fb_dev->height, bg);
}

void fbconsole_putc(int col, int row, char c, uint32_t fg, uint32_t bg)
{
    if (!fb_dev || !fb_dev->lfb) return;

    int px = col * GLYPH_W;
    int py = row * GLYPH_H;
    if (px + GLYPH_W > fb_dev->width || py + GLYPH_H > fb_dev->height) return;

    const uint8_t *glyph = vga_font_get_glyph((unsigned char)c);
    volatile uint32_t *fb = (volatile uint32_t *)fb_dev->lfb;

    for (int ry = 0; ry < GLYPH_H; ry++) {
        uint8_t bits = glyph[ry];
        int base = (py + ry) * fb_dev->width + px;
        for (int rx = 0; rx < GLYPH_W; rx++)
            fb[base + rx] = (bits & (0x80 >> rx)) ? fg : bg;
    }
}

void fbconsole_puts(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    int c = col;
    for (; *s; s++) {
        if (*s == '\n') {
            row++;
            c = col;
            continue;
        }
        fbconsole_putc(c, row, *s, fg, bg);
        c++;
    }
}
