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

/* tools/gen_bootvideo.py: count, fps, offsets[count + 1], JPEG frames. It travels in the initrd,
 * not in the kernel: Software Update reads the new kernel into the old one's heap, which starts
 * where the old kernel ends, so a kernel that grows by megabytes cannot be started that way. */
static const uint8_t *boot_video;

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

/* a pixel mix, a:b = 3:1, on all three colour lanes at once */
static inline uint32_t mix31(uint32_t a, uint32_t b)
{
    return ((((a & 0xFF00FF) * 3 + (b & 0xFF00FF)) >> 2) & 0xFF00FF) | ((((a & 0x00FF00) * 3 + (b & 0x00FF00)) >> 2) & 0x00FF00);
}

static uint32_t *hrows;                    /* the frame's rows, already widened to 1280 */

/* one frame of the animation (640x360) smoothly doubled onto the screen */
static void draw_frame(int n)
{
    const uint32_t *hdr = (const uint32_t *)boot_video;
    if (n < 0 || n >= (int)hdr[0]) return;
    uint8_t *rgb;
    uint32_t w, h;
    const char *err;
    jpeg_decode(boot_video + hdr[2 + n], hdr[3 + n] - hdr[2 + n], &rgb, &w, &h, &err);
    if (!rgb) return;
    if (w == 640 && h == 360) {
        if (!hrows) hrows = malloc((size_t)360 * SW * 4);
        if (hrows) {
            for (int y = 0; y < 360; y++) {
                const uint8_t *row = rgb + y * 640 * 3;
                uint32_t *o = hrows + y * SW;
                uint32_t prev = ((uint32_t)row[0] << 16) | ((uint32_t)row[1] << 8) | row[2];
                for (int x = 0; x < 640; x++) {
                    uint32_t c = ((uint32_t)row[3 * x] << 16) | ((uint32_t)row[3 * x + 1] << 8) | row[3 * x + 2];
                    uint32_t nx = x + 1 < 640 ? ((uint32_t)row[3 * x + 3] << 16) | ((uint32_t)row[3 * x + 4] << 8) | row[3 * x + 5] : c;
                    o[2 * x] = mix31(c, prev);
                    o[2 * x + 1] = mix31(c, nx);
                    prev = c;
                }
            }
            for (int Y = 0; Y < SH; Y++) {
                int k = Y >> 1;
                int other = (Y & 1) ? (k + 1 < 360 ? k + 1 : k) : (k > 0 ? k - 1 : 0);
                const uint32_t *a = hrows + k * SW, *b = hrows + other * SW;
                volatile uint32_t *d = fb + Y * SW;
                for (int X = 0; X < SW; X++) d[X] = mix31(a[X], b[X]);
            }
        }
    }
    free(rgb);
}

/* plays the whole animation at its own speed, by the clock: a frame that cannot be ready in time
 * is skipped, so it always takes as long as the video itself */
static void play_video(void)
{
    const uint32_t *hdr = (const uint32_t *)boot_video;
    uint32_t count = hdr[0], fps = hdr[1] ? hdr[1] : 20;
    uint32_t start = debugmon_uptime_ms();
    int last = -1;
    for (uint32_t spins = 0; spins < 400000000u; spins++) {
        uint32_t idx = (debugmon_uptime_ms() - start) * fps / 1000;
        if (idx >= count) idx = count - 1;
        if ((int)idx != last) {
            draw_frame((int)idx);
            bochs_update_rect(&dev, 0, 0, SW, SH);
            last = (int)idx;
        }
        if (idx >= count - 1) break;
        __asm__ volatile("pause");
    }
    cur_frame = last;
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

/* finds assets/boot/bootvideo.bin in the initrd (a ustar archive), still where it was loaded */
void bootsplash_set_initrd(uint32_t start, uint32_t end)
{
    static const char want[] = "bootvideo.bin";
    uint32_t at = start;
    while (at + 512 <= end) {
        const char *h = (const char *)at;
        if (!h[0]) break;
        uint32_t size = 0;
        for (int i = 124; i < 136 && h[i] >= '0' && h[i] <= '7'; i++) size = size * 8 + (uint32_t)(h[i] - '0');
        int n = 0;
        while (n < 100 && h[n]) n++;
        int w = (int)sizeof(want) - 1;
        if (n >= w && !memcmp(h + n - w, want, (size_t)w) && at + 512 + size <= end && size > 8) {
            boot_video = (const uint8_t *)(at + 512);
            return;
        }
        at += 512 + ((size + 511) & ~511u);
    }
}

void bootsplash_paging_ready(void)
{
    if (active) {
        paging_map_range(dev.lfb, dev.lfb, 0x1000000, PTE_PRESENT | PTE_WRITABLE);
        if (dev.is_vmware && dev.fifo)
            paging_map_range(dev.fifo, dev.fifo, dev.fifo_size, PTE_PRESENT | PTE_WRITABLE);
        video_ok = boot_video != 0;     /* the heap exists now, so frames can be decoded */
        if (video_ok) play_video();
    }
}

void bootsplash_progress(int percent)
{
    if (!active) return;
    if (percent > 100) percent = 100;
    if (percent > shown) shown = percent;
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
