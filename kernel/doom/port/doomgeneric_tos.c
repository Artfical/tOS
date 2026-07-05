/* tOS platform backend for doomgeneric (see doomgeneric.h) -- the
 * only file in kernel/doom/ that isn't vendored id Software/
 * doomgeneric source. Mirrors the shape of doomgeneric's own
 * doomgeneric_linuxvt.c reference backend (framebuffer memcpy +
 * a small packed press/release key queue) but drives tOS's own
 * bochs.c VBE framebuffer and keyboard.c raw event queue instead of
 * Linux's /dev/fb0 and evdev.
 */
#include "doomgeneric.h"
#include "doomkeys.h"
#include "bochs.h"
#include "keyboard.h"
#include "debugmon.h"
#include "terminal.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"

static bochs_device_t g_bochs;

/* Set by doom_window_run() (kernel/doom/port/doom_main.c) before
 * doomgeneric_tos_run() -- when running as a normal desktop window
 * instead of the CLI `doom` command's fullscreen takeover, there's no
 * real hardware mode switch at all (DG_Init()/DG_DrawFrame() render
 * into the window's own text-cell surface instead, the same
 * downsample-to-16-VGA-colors technique kernel/display/viewer.c
 * already uses for PNGs), which sidesteps the framebuffer
 * one-way-trip-to-reboot limitation entirely for this mode. */
static int g_windowed = 0;

void doomgeneric_tos_set_windowed(int windowed)
{
    g_windowed = windowed;
}

/* Same 16-color VGA palette and nearest-color search as
 * kernel/display/viewer.c uses for downsampling PNGs -- duplicated
 * rather than shared since viewer.c's version is static/internal and
 * pulling in a whole extra header for one small helper isn't worth
 * it. */
static const uint8_t vga_palette[16][3] = {
    {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
    {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
    {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

static int nearest_vga_color(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0, best_dist = -1;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga_palette[i][0];
        int dg = g - vga_palette[i][1];
        int db = b - vga_palette[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) { best_dist = dist; best = i; }
    }
    return best;
}

#define WIN_CANVAS_W 78
#define WIN_CANVAS_H 20

#define KEYQUEUE_SIZE 32
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static void push_key(int pressed, unsigned char doomKey)
{
    unsigned int next = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
    if (next == s_KeyQueueReadIndex) return; /* queue full, drop */
    s_KeyQueue[s_KeyQueueWriteIndex] = (unsigned short)((pressed << 8) | doomKey);
    s_KeyQueueWriteIndex = next;
}

/* Translates keyboard.c's raw event codes (RAWKEY_LEFT/RIGHT/UP/DOWN,
 * or a plain lowercase-table ASCII value/27 for Escape -- see
 * keyboard.h) into doomkeys.h codes and queues them. */
static void pump_keys(void)
{
    uint8_t key;
    int pressed;
    while (keyboard_get_raw_event(&key, &pressed)) {
        unsigned char dk;
        switch (key) {
            case RAWKEY_LEFT:  dk = KEY_LEFTARROW; break;
            case RAWKEY_RIGHT: dk = KEY_RIGHTARROW; break;
            case RAWKEY_UP:    dk = KEY_UPARROW; break;
            case RAWKEY_DOWN:  dk = KEY_DOWNARROW; break;
            case '\n':         dk = KEY_ENTER; break;
            case 27:           dk = KEY_ESCAPE; break;
            case '\t':         dk = KEY_TAB; break;
            case '\b':         dk = KEY_BACKSPACE; break;
            case ' ':          dk = KEY_USE; break;   /* space = use */
            case 'z':          dk = KEY_FIRE; break;  /* ctrl is awkward without a held-modifier queue */
            default:           dk = key; break;       /* letters etc: doomkeys.h wants lowercase ASCII */
        }
        push_key(pressed, dk);
    }
}

void DG_Init(void)
{
    if (g_windowed) return; /* no hardware mode switch needed at all */

    if (bochs_init(&g_bochs) != 0 || !g_bochs.lfb) {
        terminal_writestring("doom: no Bochs/VBE-capable display adapter found\n");
        return;
    }
    bochs_set_mode(&g_bochs, DOOMGENERIC_RESX, DOOMGENERIC_RESY, 32);
}

static void draw_frame_windowed(void)
{
    uint32_t *src = (uint32_t *)DG_ScreenBuffer;
    for (int cy = 0; cy < WIN_CANVAS_H; cy++) {
        terminal_setpos(1, (size_t)(cy + 2));
        for (int cx = 0; cx < WIN_CANVAS_W; cx++) {
            uint32_t sx = (uint32_t)((uint64_t)cx * DOOMGENERIC_RESX / WIN_CANVAS_W);
            uint32_t sy = (uint32_t)((uint64_t)cy * DOOMGENERIC_RESY / WIN_CANVAS_H);
            uint32_t px = src[sy * DOOMGENERIC_RESX + sx];
            /* DG_ScreenBuffer is RGBA8888 per doomgeneric_tos.h/bochs
             * mode (red_off=16, green_off=8, blue_off=0 -- see the
             * I_InitGraphics log line). */
            uint8_t r = (uint8_t)(px >> 16);
            uint8_t g = (uint8_t)(px >> 8);
            uint8_t b = (uint8_t)px;
            int vc = nearest_vga_color(r, g, b);
            terminal_setcolor((uint8_t)(vc | (vc << 4)));
            terminal_putchar(' ');
        }
    }
}

void DG_DrawFrame(void)
{
    if (g_windowed) {
        if (!wm_current_task_has_focus()) { pump_keys(); return; }
        draw_frame_windowed();
        pump_keys();
        return;
    }

    if (!g_bochs.lfb) return;
    /* DOOMGENERIC_RESX/RESY are defined to exactly match the mode set
     * in DG_Init(), so this is a single flat copy -- no stride/offset
     * math needed the way a host OS's framebuffer (which can be wider
     * than our render target) would require. */
    volatile uint32_t *fb = (volatile uint32_t *)g_bochs.lfb;
    uint32_t *src = (uint32_t *)DG_ScreenBuffer;
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) fb[i] = src[i];

    pump_keys();
}

void DG_SleepMs(uint32_t ms)
{
    /* DOOM's own frame-pacing loop (i_timer.c's I_GetTime(), used by
     * D_DoomLoop's TryRunTics()) depends on debugmon_uptime_ms()
     * actually advancing -- if that clock is stuck on some particular
     * machine/hypervisor (the exact class of environment-dependent
     * TSC/PIT timing bug already found and partially fixed elsewhere
     * this session, e.g. the beep-sound hang), this busy-wait would
     * never see its deadline arrive and DOOM would hang forever right
     * after startup with no error message. The spin-count fallback
     * below is not a timing source (its real-world duration varies
     * with CPU speed) -- it exists purely as a "give up waiting on the
     * clock and move on anyway" escape hatch, generous enough that it
     * never fires during normal operation. */
    uint32_t deadline = debugmon_uptime_ms() + ms;
    uint32_t spins = 0;
    const uint32_t max_spins = 200000000u;
    while (debugmon_uptime_ms() < deadline) {
        if (++spins > max_spins) break;
    }
}

uint32_t DG_GetTicksMs(void)
{
    /* DOOM's frame-pacing (i_timer.c's I_GetTime()) needs this value
     * to keep increasing, or the game loop freezes on its very first
     * frame forever (TryRunTics() never sees a new tic). If
     * debugmon_uptime_ms() is stuck on some machine (the same class of
     * environment-dependent clock bug already found elsewhere this
     * session), fall back to a plain call counter so time keeps moving
     * even if it is not accurately paced -- a game that runs too fast
     * or slow is a far smaller problem than one that never starts. */
    static uint32_t last_real = 0;
    static uint32_t stuck_calls = 0;
    static uint32_t fallback_ms = 0;
    static int using_fallback = 0;

    uint32_t real = debugmon_uptime_ms();
    if (!using_fallback) {
        if (real == last_real) {
            if (++stuck_calls > 2000000) using_fallback = 1;
        } else {
            stuck_calls = 0;
        }
        last_real = real;
        if (!using_fallback) return real;
    }
    fallback_ms++;
    return real + fallback_ms;
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    pump_keys();
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) return 0;
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = keyData >> 8;
    *doomKey = (unsigned char)(keyData & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* Called by cmd_doom (kernel/doom/port/doom_main.c) once the WAD has
 * been staged where d_iwad.c's search path can find it. Doesn't
 * return until the player quits (I_Quit() -> exits the process, which
 * for us just returns to the caller after bochs_disable() -- see the
 * README's linear framebuffer section for the current one-way-trip
 * caveat on going back to text mode). Windowed mode (g_windowed) never
 * touches hardware graphics mode at all, so that caveat doesn't apply
 * there -- closing the window is a normal return, same as any other
 * app. */
void doomgeneric_tos_run(int argc, char **argv)
{
    if (g_windowed) terminal_clear();
    doomgeneric_Create(argc, argv);
    for (;;) {
        if (g_windowed) {
            gui_poll();
            if (!wm_current_task_has_focus()) { task_yield(); continue; }
        }
        doomgeneric_Tick();
        if (g_windowed) task_yield();
    }
}
