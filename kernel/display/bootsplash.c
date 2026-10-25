#include "bootsplash.h"
#include "bochs.h"
#include "paging.h"
#include "vga.h"
#include "vga_font.h"
#include "terminal.h"
#include "debugmon.h"
#include "string.h"
#include "jpeg.h"
#include "memory.h"

extern const int boot_logo_w, boot_logo_h;
extern const uint32_t boot_logo_rle_len;
extern const uint8_t boot_logo_rle[];

#define SW 1280
#define SH 720
#define INK 0xE6E1D2        /* the cream of the animation's lettering */

extern const uint8_t boot_video[];       /* tools/gen_bootvideo.py: count, offsets[count + 1], JPEG frames */

static bochs_device_t dev;
static volatile uint32_t *fb;
static int active, shown, video_ok, cur_frame = -1;
static char status_text[64];

static void fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) fb[j * SW + i] = c;
}

/* the logo, cream on black, where the animation's own lettering will be */
static void draw_logo(int x0, int y0)
{
    int x = 0, y = 0;
    for (uint32_t k = 0; k + 1 < boot_logo_rle_len; k += 2) {
        int n = boot_logo_rle[k];
        uint32_t a = boot_logo_rle[k + 1];
        uint32_t r = ((INK >> 16) & 0xFF) * a / 255, g = ((INK >> 8) & 0xFF) * a / 255, b = (INK & 0xFF) * a / 255;
        uint32_t px = (r << 16) | (g << 8) | b;
        for (int i = 0; i < n; i++) {
            if (a) fb[(y0 + y) * SW + x0 + x] = px;
            if (++x == boot_logo_w) { x = 0; y++; }
        }
    }
}

/* one frame of the animation (640x360) doubled onto the screen */
static void draw_frame(int n)
{
    const uint32_t *hdr = (const uint32_t *)boot_video;
    if (n < 0 || n >= (int)hdr[0]) return;
    uint8_t *rgb;
    uint32_t w, h;
    const char *err;
    jpeg_decode(boot_video + hdr[1 + n], hdr[2 + n] - hdr[1 + n], &rgb, &w, &h, &err);
    if (!rgb) return;
    if (w == 640 && h == 360) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = rgb + y * w * 3;
            volatile uint32_t *d0 = fb + (2 * y) * SW, *d1 = d0 + SW;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t c = ((uint32_t)row[3 * x] << 16) | ((uint32_t)row[3 * x + 1] << 8) | row[3 * x + 2];
                d0[2 * x] = d0[2 * x + 1] = d1[2 * x] = d1[2 * x + 1] = c;
            }
        }
    }
    free(rgb);
}

static void draw_status(void)
{
    if (!status_text[0]) return;
    int n = 0;
    for (const char *c = status_text; *c; c++) if (((unsigned char)*c & 0xC0) != 0x80) n++;
    int x = (SW - n * 8) / 2, y = SH - 56;
    fill(x - 14, y - 6, n * 8 + 28, 28, 0x000000);
    for (const unsigned char *c = (const unsigned char *)status_text; *c; c++) {
        unsigned ch = *c;
        if ((ch & 0xE0) == 0xC0 || (ch & 0xF0) == 0xE0) {          /* the ellipsis and the like: shown as dots */
            while ((c[1] & 0xC0) == 0x80) c++;
            for (int d = 0; d < 3; d++) fill(x + 1 + d * 2, y + 12, 1, 1, 0xB4B4B8);
            x += 8;
            continue;
        }
        const uint8_t *g = vga_font_get_glyph((unsigned char)ch);
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++) if (g[r] & (0x80 >> b)) fb[(y + r) * SW + x + b] = 0xB4B4B8;
        x += 8;
    }
}

int bootsplash_begin(void)
{
    /* the text mode's registers and font are kept now: once VBE has run they can't be read */
    vga_init();
    vga_font_capture_base();
    if (bochs_init(&dev) != 0 || !dev.lfb) return -1;
    /* called before paging is on: the framebuffer is reached at its physical address until
     * bootsplash_paging_ready() maps it */
    if (bochs_set_mode(&dev, SW, SH, 32) != 0) return -1;
    fb = (volatile uint32_t *)dev.lfb;
    fill(0, 0, SW, SH, 0x000000);
    draw_logo((SW - boot_logo_w) / 2, SH * 4 / 9 - boot_logo_h / 2);
    shown = 0;
    bochs_update_rect(&dev, 0, 0, SW, SH);
    terminal_set_quiet(1);
    active = 1;
    return 0;
}

int bootsplash_active(void) { return active; }

/* a few words at the bottom, drawn in the boot screen's 8x16 VGA font from the text mode */
void bootsplash_status(const char *text)
{
    if (!active) return;
    strncpy(status_text, text, sizeof(status_text) - 1);
    status_text[sizeof(status_text) - 1] = 0;
    draw_status();
    bochs_update_rect(&dev, 0, SH - 64, SW, 48);
}

void bootsplash_paging_ready(void)
{
    if (active) {
        paging_map_range(dev.lfb, dev.lfb, 0x1000000, PTE_PRESENT | PTE_WRITABLE);
        if (dev.is_vmware && dev.fifo)
            paging_map_range(dev.fifo, dev.fifo, dev.fifo_size, PTE_PRESENT | PTE_WRITABLE);
        video_ok = 1;                   /* the heap exists now, so frames can be decoded */
    }
}

/* the animation follows the boot: how far the start-up is decides which frame is showing */
void bootsplash_progress(int percent)
{
    if (!active) return;
    if (percent > 100) percent = 100;
    if (percent <= shown) return;
    shown = percent;
    if (!video_ok) return;
    int count = (int)((const uint32_t *)boot_video)[0];
    int target = shown * (count - 1) / 100;
    int drawn = 0;
    /* at most a handful of frames per step, spread over the way to the target: decoding
     * every frame would make the start-up itself slower on a slow machine */
    int step = (target - cur_frame + 4) / 5;
    if (step < 1) step = 1;
    while (cur_frame < target) {
        cur_frame += step;
        if (cur_frame > target) cur_frame = target;
        draw_frame(cur_frame);
        drawn = 1;
    }
    if (drawn) {
        draw_status();
        bochs_update_rect(&dev, 0, 0, SW, SH);
    }
}

void bootsplash_end(void)
{
    if (!active) return;
    bootsplash_progress(100);
    uint32_t t = debugmon_uptime_ms();
    for (int n = 0; n < 20000000 && debugmon_uptime_ms() - t < 250; n++) __asm__ volatile("pause");
    active = 0;
    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_set_quiet(0);
    terminal_clear();
}

/* the desktop takes the screen over: VBE stays on (it sets its own mode), only the text console
 * gets its voice back */
void bootsplash_handoff(void)
{
    if (!active) return;
    bootsplash_progress(100);
    active = 0;
    terminal_set_quiet(0);
}
