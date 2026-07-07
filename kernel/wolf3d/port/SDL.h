#ifndef TOS_WOLF3D_SDL_H
#define TOS_WOLF3D_SDL_H
/* tOS compatibility shim for the subset of SDL2 kernel/wolf3d/ (a
 * vendored Wolf4SDL, itself an SDL2 port of id Software's original
 * Wolfenstein 3D) actually calls. Unlike doomgeneric, Wolf4SDL calls
 * SDL2 directly from within its own engine source rather than
 * through a small platform-specific shim, so this header (plus
 * SDL_mixer.h/SDL_syswm.h alongside it) has to stand in for the real
 * library -- backed by tOS's own Bochs/VBE framebuffer, keyboard.c/
 * mouse.c raw event queues, and debugmon_uptime_ms(), rather than a
 * real windowing system.
 *
 * Scope note: video (window/renderer/texture -> tOS's own
 * framebuffer), the core event queue, and timing are implemented for
 * real. Joystick and mutex calls are safe no-ops (tOS's DOOM/3d code
 * doesn't have real per-object mutexes either, and this engine is
 * single-tasked the same way) -- there's no real joystick driver
 * wired up here yet, so joystick-related calls simply report "no
 * joysticks present".
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;
typedef int SDL_bool;
#define SDL_FALSE 0
#define SDL_TRUE 1

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
#define SDL_INIT_VIDEO    0x00000020u
#define SDL_INIT_AUDIO    0x00000010u
#define SDL_INIT_JOYSTICK 0x00000200u

const char *SDL_GetError(void);
void SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

/* ---- Color / palette / surface ---- */

typedef struct { Uint8 r, g, b, a; } SDL_Color;

typedef struct {
    int ncolors;
    SDL_Color *colors;
} SDL_Palette;

/* Only ever used at 8bpp (palettized, the original game's native
 * depth) in this engine -- format-negotiation calls below are
 * stubbed to always report exactly that, which is all Wolf4SDL's own
 * code paths here ever actually need. */
typedef struct {
    Uint32 format;
    SDL_Palette *palette;
    int BitsPerPixel;
    int BytesPerPixel;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    int refcount;
} SDL_Surface;

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                   Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
#define SDL_MUSTLOCK(s) (0)
int SDL_FillRect(SDL_Surface *dst, const void *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const void *srcrect, SDL_Surface *dst, void *dstrect);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);

SDL_Palette *SDL_AllocPalette(int ncolors);
void SDL_FreePalette(SDL_Palette *palette);
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors);

/* ---- Window / renderer / texture ---- */

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOW_RESIZABLE 0x00000020u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u
#define SDL_TEXTUREACCESS_STREAMING 1

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
void SDL_SetWindowSize(SDL_Window *window, int w, int h);
void SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
Uint32 SDL_GetWindowID(SDL_Window *window);
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const void *srcrect, const void *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
int SDL_UpdateTexture(SDL_Texture *texture, const void *rect, const void *pixels, int pitch);

int SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *Rmask, Uint32 *Gmask, Uint32 *Bmask, Uint32 *Amask);

/* ---- Events ---- */

#define SDL_QUIT 0x100
#define SDL_KEYDOWN 0x300
#define SDL_KEYUP 0x301
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP 0x402
#define SDL_MOUSEWHEEL 0x403
#define SDL_WINDOWEVENT 0x200
/* Real SDL2 SDL_WindowEventID enum values, reproduced exactly (same
 * reasoning as the SDLK_* keycodes above). */
#define SDL_WINDOWEVENT_MOVED 4
#define SDL_WINDOWEVENT_RESIZED 5
#define SDL_WINDOWEVENT_SIZE_CHANGED 6
#define SDL_WINDOWEVENT_MINIMIZED 7
#define SDL_WINDOWEVENT_MAXIMIZED 8
#define SDL_WINDOWEVENT_RESTORED 9
#define SDL_WINDOWEVENT_FOCUS_GAINED 12
#define SDL_WINDOWEVENT_FOCUS_LOST 13

#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON(x) (1 << ((x) - 1))

#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_KP_ENTER 88

/* Real SDL2's actual SDLK_* numeric values (from SDL_keycode.h) --
 * reproduced exactly rather than invented, on the theory that these
 * are stable, publicly documented ABI constants (like POSIX errno
 * numbers), not creative content, and matching them exactly is safer
 * than picking arbitrary numbers that only need to be internally
 * consistent. Printable-character codes are just their ASCII value;
 * "special" keys OR in SDLK_SCANCODE_MASK with their USB HID-derived
 * SDL scancode. */
#define SDLK_SCANCODE_MASK (1 << 30)
#define SDL_SCANCODE_TO_KEYCODE(X) ((X) | SDLK_SCANCODE_MASK)

#define SDLK_BACKSPACE 8
#define SDLK_TAB 9
#define SDLK_RETURN 13
#define SDLK_ESCAPE 27
#define SDLK_SPACE 32
#define SDLK_EXCLAIM 33
#define SDLK_QUOTEDBL 34
#define SDLK_HASH 35
#define SDLK_DOLLAR 36
#define SDLK_AMPERSAND 38
#define SDLK_QUOTE 39
#define SDLK_LEFTPAREN 40
#define SDLK_RIGHTPAREN 41
#define SDLK_ASTERISK 42
#define SDLK_PLUS 43
#define SDLK_COMMA 44
#define SDLK_MINUS 45
#define SDLK_PERIOD 46
#define SDLK_SLASH 47
#define SDLK_0 48
#define SDLK_1 49
#define SDLK_2 50
#define SDLK_3 51
#define SDLK_4 52
#define SDLK_5 53
#define SDLK_6 54
#define SDLK_7 55
#define SDLK_8 56
#define SDLK_9 57
#define SDLK_COLON 58
#define SDLK_SEMICOLON 59
#define SDLK_LESS 60
#define SDLK_EQUALS 61
#define SDLK_GREATER 62
#define SDLK_QUESTION 63
#define SDLK_AT 64
#define SDLK_LEFTBRACKET 91
#define SDLK_BACKSLASH 92
#define SDLK_RIGHTBRACKET 93
#define SDLK_DELETE 127
#define SDLK_a 97
#define SDLK_b 98
#define SDLK_c 99
#define SDLK_d 100
#define SDLK_e 101
#define SDLK_f 102
#define SDLK_g 103
#define SDLK_h 104
#define SDLK_i 105
#define SDLK_j 106
#define SDLK_k 107
#define SDLK_l 108
#define SDLK_m 109
#define SDLK_n 110
#define SDLK_o 111
#define SDLK_p 112
#define SDLK_q 113
#define SDLK_r 114
#define SDLK_s 115
#define SDLK_t 116
#define SDLK_u 117
#define SDLK_v 118
#define SDLK_w 119
#define SDLK_x 120
#define SDLK_y 121
#define SDLK_z 122
#define SDLK_CAPSLOCK      SDL_SCANCODE_TO_KEYCODE(57)
#define SDLK_F1            SDL_SCANCODE_TO_KEYCODE(58)
#define SDLK_F2            SDL_SCANCODE_TO_KEYCODE(59)
#define SDLK_F3            SDL_SCANCODE_TO_KEYCODE(60)
#define SDLK_F4            SDL_SCANCODE_TO_KEYCODE(61)
#define SDLK_F5            SDL_SCANCODE_TO_KEYCODE(62)
#define SDLK_F6            SDL_SCANCODE_TO_KEYCODE(63)
#define SDLK_F7            SDL_SCANCODE_TO_KEYCODE(64)
#define SDLK_F8            SDL_SCANCODE_TO_KEYCODE(65)
#define SDLK_F9            SDL_SCANCODE_TO_KEYCODE(66)
#define SDLK_F10           SDL_SCANCODE_TO_KEYCODE(67)
#define SDLK_F11           SDL_SCANCODE_TO_KEYCODE(68)
#define SDLK_F12           SDL_SCANCODE_TO_KEYCODE(69)
#define SDLK_PRINTSCREEN   SDL_SCANCODE_TO_KEYCODE(70)
#define SDLK_PRINT         SDLK_PRINTSCREEN
#define SDLK_SCROLLLOCK    SDL_SCANCODE_TO_KEYCODE(71)
#define SDLK_SCROLLOCK     SDLK_SCROLLLOCK
#define SDLK_PAUSE         SDL_SCANCODE_TO_KEYCODE(72)
#define SDLK_INSERT        SDL_SCANCODE_TO_KEYCODE(73)
#define SDLK_HOME          SDL_SCANCODE_TO_KEYCODE(74)
#define SDLK_PAGEUP        SDL_SCANCODE_TO_KEYCODE(75)
#define SDLK_END           SDL_SCANCODE_TO_KEYCODE(77)
#define SDLK_PAGEDOWN      SDL_SCANCODE_TO_KEYCODE(78)
#define SDLK_RIGHT         SDL_SCANCODE_TO_KEYCODE(79)
#define SDLK_LEFT          SDL_SCANCODE_TO_KEYCODE(80)
#define SDLK_DOWN          SDL_SCANCODE_TO_KEYCODE(81)
#define SDLK_UP            SDL_SCANCODE_TO_KEYCODE(82)
#define SDLK_NUMLOCKCLEAR  SDL_SCANCODE_TO_KEYCODE(83)
#define SDLK_KP_ENTER      SDL_SCANCODE_TO_KEYCODE(88)
#define SDLK_KP_2          SDL_SCANCODE_TO_KEYCODE(90)
#define SDLK_KP_4          SDL_SCANCODE_TO_KEYCODE(92)
#define SDLK_KP_5          SDL_SCANCODE_TO_KEYCODE(93)
#define SDLK_KP_6          SDL_SCANCODE_TO_KEYCODE(94)
#define SDLK_KP_8          SDL_SCANCODE_TO_KEYCODE(96)
#define SDLK_F13           SDL_SCANCODE_TO_KEYCODE(104)
#define SDLK_F14           SDL_SCANCODE_TO_KEYCODE(105)
#define SDLK_F15           SDL_SCANCODE_TO_KEYCODE(106)
#define SDLK_F16           SDL_SCANCODE_TO_KEYCODE(107)
#define SDLK_F17           SDL_SCANCODE_TO_KEYCODE(108)
#define SDLK_F18           SDL_SCANCODE_TO_KEYCODE(109)
#define SDLK_F19           SDL_SCANCODE_TO_KEYCODE(110)
#define SDLK_LCTRL         SDL_SCANCODE_TO_KEYCODE(224)
#define SDLK_LSHIFT        SDL_SCANCODE_TO_KEYCODE(225)
#define SDLK_LALT          SDL_SCANCODE_TO_KEYCODE(226)
#define SDLK_LGUI          SDL_SCANCODE_TO_KEYCODE(227)
#define SDLK_RCTRL         SDL_SCANCODE_TO_KEYCODE(228)
#define SDLK_RSHIFT        SDL_SCANCODE_TO_KEYCODE(229)
#define SDLK_RALT          SDL_SCANCODE_TO_KEYCODE(230)
#define SDLK_RGUI          SDL_SCANCODE_TO_KEYCODE(231)
#define SDLK_LAST          0x40000115

/* Real SDL2 KMOD_* bit values. */
#define KMOD_NONE   0x0000
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_LGUI   0x0400
#define KMOD_RGUI   0x0800
#define KMOD_NUM    0x1000
#define KMOD_CAPS   0x2000
#define KMOD_CTRL   (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_ALT    (KMOD_LALT | KMOD_RALT)
#define KMOD_GUI    (KMOD_LGUI | KMOD_RGUI)

typedef int SDL_Keymod;
typedef struct { int scancode; int sym; SDL_Keymod mod; } SDL_Keysym;

typedef struct { Uint32 type; Uint32 timestamp; Uint32 windowID; SDL_Keysym keysym; Uint8 state; Uint8 repeat; } SDL_KeyboardEvent;
typedef struct { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 button; Uint8 state; int x, y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type; Uint32 timestamp; Uint32 windowID; Sint32 x, y; } SDL_MouseWheelEvent;
typedef struct { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 event; Uint8 padding1, padding2, padding3; Sint32 data1, data2; } SDL_WindowEvent;

typedef struct {
    Uint32 type;
    union {
        SDL_KeyboardEvent key;
        SDL_MouseButtonEvent button;
        SDL_MouseWheelEvent wheel;
        SDL_WindowEvent window;
    };
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);

SDL_Keymod SDL_GetModState(void);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
void SDL_SetRelativeMouseMode(SDL_bool enabled);
void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y);

/* ---- Joystick (not backed by a real driver -- always "none present") ---- */

typedef struct SDL_Joystick SDL_Joystick;
int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *joystick);
void SDL_JoystickEventState(int state);
void SDL_JoystickUpdate(void);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button);
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat);
int SDL_JoystickNumButtons(SDL_Joystick *joystick);
int SDL_JoystickNumHats(SDL_Joystick *joystick);
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8
#define SDL_ENABLE 1

/* ---- Mutex (tOS runs this engine single-tasked -- no real locking needed) ---- */

typedef struct SDL_mutex SDL_mutex;
SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

/* ---- Timing ---- */

Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

/* ---- Audio format conversion: only ever used for one exact
 * conversion in this engine (id_sd.cpp's SD_PrepareSound() --
 * 8-bit unsigned mono at ORIGSAMPLERATE to whatever mix_format/
 * mix_channels/param_samplerate the mixer opened with), so this
 * shim doesn't need to be a general-purpose implementation, just a
 * correct one for that case -- see sdl_shim.cpp. ---- */

#define AUDIO_U8 0x0008

typedef struct {
    int needed;
    Uint16 src_format;
    Uint16 dst_format;
    double rate_incr;
    Uint8 *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
} SDL_AudioCVT;

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels, int src_rate,
                       Uint16 dst_format, Uint8 dst_channels, int dst_rate);
int SDL_ConvertAudio(SDL_AudioCVT *cvt);

#ifdef __cplusplus
}
#endif

#endif
