/* tOS backend for the SDL.h/SDL_mixer.h compatibility shims -- not
 * part of Wolf4SDL/id Software's source. Implements the actual
 * behavior those headers declare, backed by tOS's own
 * kernel/drivers/video/bochs.c (the same real Bochs/VBE linear
 * framebuffer DOOM and the 3d rasterizer already use),
 * kernel/drivers/input/keyboard.c's raw press/release queue,
 * kernel/drivers/input/mouse.c, kernel/core/debugmon.c, and
 * kernel/audio/audio.c.
 *
 * Scope actually implemented for real: window/renderer/texture
 * (mapped onto one fixed 320x200x32 Bochs mode -- the same
 * resolution/depth DOOM already uses), the 8bpp-palette-to-32bpp
 * surface blit VL_Flip()'s present path depends on, the keyboard/
 * mouse-button event queue, and timing.
 *
 * Deliberately NOT implemented for real (safe no-ops/stubs instead),
 * documented in the README's Wolfenstein 3D section:
 * - Joysticks (no real joystick driver wired up to this shim yet).
 * - Mouselook (tOS's mouse.c only exposes quantized text-cell
 *   position, not raw pixel-precision relative deltas -- keyboard-
 *   only turning still works).
 * - The full SDL_mixer multichannel/panning mixer and the engine's
 *   own AdLib/PC-speaker software synth hooked through
 *   Mix_SetPostMix() -- tOS's own audio.c (see kernel/doom/port/
 *   i_sound_tos.c's identical caveat for DOOM) only supports one
 *   active playback buffer at a time, nowhere near enough for a
 *   real-time synthesized music mixer.
 */
#include "SDL.h"
#include "SDL_mixer.h"
#include "wolf_compat.h"
/* These are tOS's real driver headers, not one of this directory's
 * own C++-aware compat wrappers -- none of them guard their
 * declarations in extern "C", so without this, g++ would give every
 * function here C++ (mangled) linkage while the actual kernel/
 * drivers/.../*.c implementations are compiled as plain C
 * (unmangled), an unresolved-symbol link failure (see string.h in
 * this same directory for the general version of this problem). */
extern "C" {
#include "bochs.h"
#include "keyboard.h"
#include "mouse.h"
#include "debugmon.h"
#include "audio.h"
}
#include <stdlib.h>
#include <string.h>

#define WOLF_W 320
#define WOLF_H 200

static bochs_device_t g_bochs;
static int g_have_video = 0;

extern "C" {

int SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) { if (g_have_video) bochs_disable(); }

static const char *g_last_error = "";
const char *SDL_GetError(void) { return g_last_error; }
void SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; }

/* ---- Window / renderer / texture: one fixed real Bochs/VBE mode ---- */

struct SDL_Window { int w, h; };
struct SDL_Renderer { int dummy; };
struct SDL_Texture { int w, h; };

static SDL_Window g_window;
static SDL_Renderer g_renderer;
static SDL_Texture g_texture;

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    if (bochs_init(&g_bochs) == 0 && g_bochs.lfb) {
        bochs_set_mode(&g_bochs, WOLF_W, WOLF_H, 32);
        g_have_video = 1;
    }
    g_window.w = WOLF_W;
    g_window.h = WOLF_H;
    return &g_window;
}

void SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) { (void)window; (void)flags; }
void SDL_SetWindowSize(SDL_Window *window, int w, int h) { (void)window; (void)w; (void)h; }
void SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h) { (void)window; (void)min_w; (void)min_h; }
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    if (w) *w = window ? window->w : WOLF_W;
    if (h) *h = window ? window->h : WOLF_H;
}
Uint32 SDL_GetWindowID(SDL_Window *window) { (void)window; return 1; }

#define TOS_PIXELFORMAT_ARGB8888 1
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window) { (void)window; return TOS_PIXELFORMAT_ARGB8888; }

int SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *Rmask, Uint32 *Gmask, Uint32 *Bmask, Uint32 *Amask)
{
    (void)format;
    if (bpp) *bpp = 32;
    if (Rmask) *Rmask = 0x00FF0000u;
    if (Gmask) *Gmask = 0x0000FF00u;
    if (Bmask) *Bmask = 0x000000FFu;
    if (Amask) *Amask = 0xFF000000u;
    return 0;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    (void)window; (void)index; (void)flags;
    return &g_renderer;
}
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) { (void)renderer; (void)w; (void)h; return 0; }
int SDL_RenderClear(SDL_Renderer *renderer) { (void)renderer; return 0; }
void SDL_RenderPresent(SDL_Renderer *renderer) { (void)renderer; }

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h)
{
    (void)renderer; (void)format; (void)access;
    g_texture.w = w;
    g_texture.h = h;
    return &g_texture;
}

/* This is where the actual frame reaches real hardware: VL_Flip()
 * (kernel/wolf3d/id_vl.cpp) calls this with the already-32bpp-
 * converted `screen` surface's pixels (see SDL_BlitSurface below for
 * where the 8bpp palette conversion into that surface happens), then
 * RenderClear()/RenderCopy()/RenderPresent() as pure no-ops -- so
 * copying straight to the Bochs linear framebuffer here is the entire
 * "present". */
int SDL_UpdateTexture(SDL_Texture *texture, const void *rect, const void *pixels, int pitch)
{
    (void)rect;
    if (!g_have_video || !pixels) return -1;
    int w = texture ? texture->w : WOLF_W;
    int h = texture ? texture->h : WOLF_H;
    volatile uint32_t *fb = (volatile uint32_t *)g_bochs.lfb;
    const uint8_t *src = (const uint8_t *)pixels;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * pitch);
        for (int x = 0; x < w; x++) fb[y * WOLF_W + x] = row[x];
    }
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const void *srcrect, const void *dstrect)
{
    (void)renderer; (void)texture; (void)srcrect; (void)dstrect;
    return 0;
}

/* ---- Surfaces / palettes ---- */

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                   Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    SDL_Surface *s = (SDL_Surface *)malloc(sizeof(SDL_Surface));
    SDL_PixelFormat *fmt = (SDL_PixelFormat *)malloc(sizeof(SDL_PixelFormat));
    if (!s || !fmt) { free(s); free(fmt); return NULL; }
    memset(fmt, 0, sizeof(*fmt));
    fmt->BitsPerPixel = depth;
    fmt->BytesPerPixel = (depth + 7) / 8;
    fmt->palette = NULL;
    s->flags = 0;
    s->format = fmt;
    s->w = width;
    s->h = height;
    s->pitch = width * fmt->BytesPerPixel;
    s->pixels = malloc((size_t)s->pitch * height);
    s->refcount = 1;
    return s;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface) return;
    if (surface->format) free(surface->format);
    if (surface->pixels) free(surface->pixels);
    free(surface);
}

int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
    if (!surface || !surface->format) return -1;
    surface->format->palette = palette;
    return 0;
}

int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }

int SDL_FillRect(SDL_Surface *dst, const void *rect, Uint32 color)
{
    (void)rect;
    if (!dst || !dst->pixels) return -1;
    int bpp = dst->format->BytesPerPixel;
    for (int y = 0; y < dst->h; y++) {
        uint8_t *row = (uint8_t *)dst->pixels + (size_t)y * dst->pitch;
        for (int x = 0; x < dst->w; x++)
            memcpy(row + (size_t)x * bpp, &color, bpp);
    }
    return 0;
}

/* Only the whole-surface case (srcrect == dstrect == NULL) is ever
 * used in this engine -- the two real shapes it needs are an 8bpp
 * palette-indexed source onto a 32bpp destination (the palette ->
 * true-color conversion VL_Flip()'s frame depends on) and a plain
 * same-depth copy. */
int SDL_BlitSurface(SDL_Surface *src, const void *srcrect, SDL_Surface *dst, void *dstrect)
{
    (void)srcrect; (void)dstrect;
    if (!src || !dst) return -1;
    int w = src->w < dst->w ? src->w : dst->w;
    int h = src->h < dst->h ? src->h : dst->h;

    if (src->format->BitsPerPixel == 8 && dst->format->BytesPerPixel == 4) {
        SDL_Color *pal = src->format->palette ? src->format->palette->colors : NULL;
        for (int y = 0; y < h; y++) {
            const uint8_t *srow = (const uint8_t *)src->pixels + (size_t)y * src->pitch;
            uint32_t *drow = (uint32_t *)((uint8_t *)dst->pixels + (size_t)y * dst->pitch);
            for (int x = 0; x < w; x++) {
                uint8_t idx = srow[x];
                if (pal) {
                    SDL_Color c = pal[idx];
                    drow[x] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
                } else {
                    drow[x] = ((uint32_t)idx << 16) | ((uint32_t)idx << 8) | idx;
                }
            }
        }
        return 0;
    }

    for (int y = 0; y < h; y++) {
        memcpy((uint8_t *)dst->pixels + (size_t)y * dst->pitch,
               (uint8_t *)src->pixels + (size_t)y * src->pitch,
               (size_t)w * src->format->BytesPerPixel);
    }
    return 0;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
    (void)format;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int SDL_SaveBMP(SDL_Surface *surface, const char *file) { (void)surface; (void)file; return -1; }

SDL_Palette *SDL_AllocPalette(int ncolors)
{
    SDL_Palette *p = (SDL_Palette *)malloc(sizeof(SDL_Palette));
    if (!p) return NULL;
    p->ncolors = ncolors;
    p->colors = (SDL_Color *)calloc((size_t)ncolors, sizeof(SDL_Color));
    return p;
}

void SDL_FreePalette(SDL_Palette *palette)
{
    if (!palette) return;
    free(palette->colors);
    free(palette);
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors)
{
    if (!palette || !palette->colors) return -1;
    for (int i = 0; i < ncolors && firstcolor + i < palette->ncolors; i++)
        palette->colors[firstcolor + i] = colors[i];
    return 0;
}

/* ---- Events: drained from keyboard.c's raw queue + mouse.c ---- */

static uint8_t rawkey_to_sdlk(uint8_t rawkey)
{
    switch (rawkey) {
        case RAWKEY_LEFT:  return SDLK_LEFT & 0xFF;
        case RAWKEY_RIGHT: return SDLK_RIGHT & 0xFF;
        case RAWKEY_UP:    return SDLK_UP & 0xFF;
        case RAWKEY_DOWN:  return SDLK_DOWN & 0xFF;
        default:           return rawkey; /* printable ASCII already matches SDLK_<letter> */
    }
}
static Uint32 rawkey_to_sym(uint8_t rawkey)
{
    switch (rawkey) {
        case RAWKEY_LEFT:  return SDLK_LEFT;
        case RAWKEY_RIGHT: return SDLK_RIGHT;
        case RAWKEY_UP:    return SDLK_UP;
        case RAWKEY_DOWN:  return SDLK_DOWN;
        default:           return rawkey;
    }
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!event) return 0;

    /* Ctrl+C (see wolf_main.cpp's interrupt_callback hook) can't
     * longjmp() directly from IRQ context -- it just sets a flag,
     * checked here since this is the one place Wolf4SDL's main loop
     * reliably calls every frame regardless of game state (unlike
     * DOOM's doomgeneric_Tick(), this engine has no other per-frame
     * hook to check it from). */
    if (g_wolf_ctrlc_requested) {
        wolf_run_atexit_handlers();
        wolf_longjmp(&g_wolf_quit_jmp, 1);
    }

    uint8_t rawkey; int pressed;
    if (keyboard_get_raw_event(&rawkey, &pressed)) {
        event->type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.type = event->type;
        event->key.windowID = 1;
        event->key.keysym.scancode = rawkey_to_sdlk(rawkey);
        event->key.keysym.sym = (int)rawkey_to_sym(rawkey);
        event->key.keysym.mod = 0;
        return 1;
    }

    int cx, cy;
    if (mouse_get_click(&cx, &cy)) {
        (void)cx; (void)cy;
        event->type = SDL_MOUSEBUTTONDOWN;
        event->button.type = event->type;
        event->button.windowID = 1;
        event->button.button = SDL_BUTTON_LEFT;
        event->button.state = 1;
        event->button.x = 0;
        event->button.y = 0;
        return 1;
    }

    int wheel = mouse_get_wheel_delta();
    if (wheel != 0) {
        event->type = SDL_MOUSEWHEEL;
        event->wheel.type = event->type;
        event->wheel.windowID = 1;
        event->wheel.x = 0;
        event->wheel.y = wheel;
        return 1;
    }

    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    while (!SDL_PollEvent(event)) { /* spin -- no real blocking wait source here */ }
    return 1;
}

int SDL_PushEvent(SDL_Event *event) { (void)event; return 1; }

SDL_Keymod SDL_GetModState(void) { return 0; }

/* Real mouselook needs raw, unquantized relative pixel deltas;
 * mouse.c only exposes an already-quantized text-cell position (see
 * its MOUSE_SENS_DIV-based accumulator), so this always reports "no
 * motion" -- keyboard-only turning still works fully. */
Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}
void SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; }
void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) { (void)window; (void)x; (void)y; }

/* ---- Joysticks: no real driver wired up here yet ---- */

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
void SDL_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }
void SDL_JoystickEventState(int state) { (void)state; }
void SDL_JoystickUpdate(void) {}
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis) { (void)joystick; (void)axis; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button) { (void)joystick; (void)button; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat) { (void)joystick; (void)hat; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *joystick) { (void)joystick; return 0; }

/* ---- Mutex: this engine runs single-tasked under tOS ---- */

SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)1; }
void SDL_DestroyMutex(SDL_mutex *mutex) { (void)mutex; }
int SDL_LockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }

/* ---- Timing ---- */

Uint32 SDL_GetTicks(void) { return debugmon_uptime_ms(); }

void SDL_Delay(Uint32 ms)
{
    uint32_t deadline = debugmon_uptime_ms() + ms;
    while (debugmon_uptime_ms() < deadline) { }
}

/* ---- Audio format conversion (see SDL.h) ---- */

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels, int src_rate,
                       Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    if (!cvt || src_rate <= 0 || dst_rate <= 0) return -1;
    cvt->src_format = src_format;
    cvt->dst_format = dst_format;
    cvt->rate_incr = (double)dst_rate / (double)src_rate;

    int src_sample_bytes = (src_format == AUDIO_U8) ? 1 : 2;
    int dst_sample_bytes = (dst_format == AUDIO_U8) ? 1 : 2;
    /* Generous upper bound on how much bigger the buffer might need to
     * get: resampling can only grow it by rate_incr, format widening
     * by dst/src sample size, and channel count by dst/src channels --
     * multiplying all three worst-cases together and rounding up by
     * one is always enough room, even though real conversions rarely
     * need that much. */
    double mult = cvt->rate_incr * ((double)dst_sample_bytes / src_sample_bytes)
                  * ((double)dst_channels / src_channels);
    cvt->len_mult = (int)mult + 2;
    cvt->len_ratio = mult;
    cvt->needed = 1;
    return 0;
}

/* Only ever called on an 8-bit unsigned mono source (see SD_PrepareSound()
 * in id_sd.cpp) -- nearest-neighbor resample to the destination rate,
 * then widen to 16-bit signed and/or duplicate to stereo if the
 * destination format calls for it. Runs in place within cvt->buf,
 * which SDL_BuildAudioCVT already sized generously enough above. */
int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    if (!cvt || !cvt->buf) return -1;
    int src_len = cvt->len;

    int dst_frames = (int)((double)src_len * cvt->rate_incr);
    if (dst_frames < 1) dst_frames = 1;

    uint8_t *tmp = (uint8_t *)malloc((size_t)dst_frames);
    if (!tmp) return -1;
    for (int i = 0; i < dst_frames; i++) {
        int src_i = (int)((double)i / cvt->rate_incr);
        if (src_i >= src_len) src_i = src_len - 1;
        tmp[i] = cvt->buf[src_i];
    }

    if (cvt->dst_format == AUDIO_U8) {
        memcpy(cvt->buf, tmp, (size_t)dst_frames);
        cvt->len_cvt = dst_frames;
    } else {
        /* 16-bit signed: expand each 8-bit unsigned sample. */
        int16_t *out16 = (int16_t *)cvt->buf;
        for (int i = 0; i < dst_frames; i++)
            out16[i] = (int16_t)(((int)tmp[i] - 128) << 8);
        cvt->len_cvt = dst_frames * 2;
    }
    free(tmp);
    return 0;
}

/* ---- SDL_mixer: audio.c only has one active buffer at a time, so
 * this is a single-channel approximation, same as kernel/doom/port/
 * i_sound_tos.c's identical fix for DOOM. ---- */

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize,
                         const char *device, int allowed_changes)
{
    (void)frequency; (void)format; (void)channels; (void)chunksize;
    (void)device; (void)allowed_changes;
    if (!audio_available()) audio_init();
    return audio_available() ? 0 : -1;
}
void Mix_QuerySpec(int *frequency, Uint16 *format, int *channels)
{
    if (frequency) *frequency = AUDIO_OUT_RATE;
    if (format) *format = 0x0008; /* AUDIO_U8 */
    if (channels) *channels = 1;
}
int Mix_ReserveChannels(int num) { (void)num; return num; }
int Mix_GroupChannels(int from, int to, int tag) { (void)from; (void)to; (void)tag; return 0; }
int Mix_GroupAvailable(int tag) { (void)tag; return 0; }
int Mix_GroupOldest(int tag) { (void)tag; return 0; }

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    (void)loops;
    if (!chunk || !chunk->abuf) return -1;
    uint32_t len = chunk->alen > AUDIO_DMA_SIZE ? AUDIO_DMA_SIZE : chunk->alen;
    audio_submit(chunk->abuf, len);
    return channel;
}
int Mix_HaltChannel(int channel) { (void)channel; audio_stop(); return 0; }
int Mix_SetPanning(int channel, Uint8 left, Uint8 right) { (void)channel; (void)left; (void)right; return 1; }
void Mix_ChannelFinished(void (*channel_finished)(int channel)) { (void)channel_finished; }
void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg) { (void)mix_func; (void)arg; }
void Mix_SetPostMix(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg) { (void)mix_func; (void)arg; }
const char *Mix_GetError(void) { return g_last_error; }

} /* extern "C" */
