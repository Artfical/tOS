#include "bootsplash.h"
#include "bochs.h"
#include "paging.h"
#include "vga.h"
#include "vga_font.h"
#include "terminal.h"
#include "debugmon.h"
#include "string.h"

extern const int boot_logo_w, boot_logo_h;
extern const uint32_t boot_logo_rle_len;
extern const uint8_t boot_logo_rle[];

#define SW 1280
#define SH 720
#define BAR_W 220
#define BAR_H 6

static bochs_device_t dev;
static volatile uint32_t *fb;
static int active, shown, bar_x, bar_y;

static void fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) fb[j * SW + i] = c;
}

/* a pill shape: the bar's ends are round */
static void pill(int x, int y, int w, int h, uint32_t c, uint32_t bg)
{
    int r = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0;
            int dy = 2 * j - (h - 1);                       /* doubled, centred */
            int d2 = dx * dx * 4 + dy * dy;
            int rr = (2 * r) * (2 * r);
            if (d2 <= rr - 4 * r) fb[(y + j) * SW + x + i] = c;
            else if (d2 <= rr + 4 * r) {                    /* one pixel of softening on the curve */
                uint32_t a = 128;
                uint32_t rb = (((c & 0xFF00FF) * a + (bg & 0xFF00FF) * (255 - a)) >> 8) & 0xFF00FF;
                uint32_t g = (((c & 0x00FF00) * a + (bg & 0x00FF00) * (255 - a)) >> 8) & 0x00FF00;
                fb[(y + j) * SW + x + i] = rb | g;
            }
        }
}

static void draw_logo(int x0, int y0)
{
    int x = 0, y = 0;
    for (uint32_t k = 0; k + 1 < boot_logo_rle_len; k += 2) {
        int n = boot_logo_rle[k];
        uint32_t a = boot_logo_rle[k + 1];
        uint32_t c = 255 - a;                               /* black ink on white */
        uint32_t px = (c << 16) | (c << 8) | c;
        for (int i = 0; i < n; i++) {
            if (a) fb[(y0 + y) * SW + x0 + x] = px;
            if (++x == boot_logo_w) { x = 0; y++; }
        }
    }
}

static void draw_bar(int pct)
{
    int w = BAR_W * pct / 100;
    pill(bar_x, bar_y, BAR_W, BAR_H, 0xD9D9DC, 0xFFFFFF);
    if (w >= BAR_H) pill(bar_x, bar_y, w, BAR_H, 0x1D1D1F, 0xD9D9DC);
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
    fill(0, 0, SW, SH, 0xFFFFFF);
    int lx = (SW - boot_logo_w) / 2, ly = SH / 2 - boot_logo_h / 2 - 40;
    draw_logo(lx, ly);
    bar_x = (SW - BAR_W) / 2;
    bar_y = ly + boot_logo_h + 90;
    shown = 0;
    draw_bar(0);
    terminal_set_quiet(1);
    active = 1;
    return 0;
}

int bootsplash_active(void) { return active; }

void bootsplash_paging_ready(void)
{
    if (active) paging_map_range(dev.lfb, dev.lfb, 0x1000000, PTE_PRESENT | PTE_WRITABLE);
}

void bootsplash_progress(int percent)
{
    if (!active) return;
    if (percent > 100) percent = 100;
    /* glide to the new value instead of jumping */
    while (shown < percent) {
        shown++;
        draw_bar(shown);
        uint32_t t = debugmon_uptime_ms();
        for (int n = 0; n < 200000 && debugmon_uptime_ms() - t < 4; n++) __asm__ volatile("pause");
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
