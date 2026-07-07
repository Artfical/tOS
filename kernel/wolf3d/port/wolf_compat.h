#ifndef WOLF3D_COMPAT_H
#define WOLF3D_COMPAT_H
#include "wolf_jmp.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Runs whatever atexit()-registered functions the engine has queued
 * (in practice just wl_main.cpp's atexit(SDL_Quit)) -- see
 * wolf_compat.cpp for why this can't just happen through a real
 * exit() call. */
void wolf_run_atexit_handlers(void);

/* Set up once per cmd_wolf3d() invocation (kernel/wolf3d/port/
 * wolf_main.cpp) via wolf_setjmp() right before calling the engine's
 * main(). wl_main.cpp's Quit() longjmp()s back here instead of
 * calling exit() (see wl_main.cpp's Quit() and wolf_jmp.h's comment
 * for why). Also the escape hatch kernel/wolf3d/port/sdl_shim.cpp's
 * SDL_PollEvent() uses for Ctrl+C, since Wolf4SDL's main()/DemoLoop()
 * has no per-frame callback to return control through cooperatively
 * the way DOOM's doomgeneric_Tick() does. */
extern wolf_jmp_buf g_wolf_quit_jmp;

/* Set by the Ctrl+C interrupt_callback hook (wolf_main.cpp), checked
 * and acted on inside SDL_PollEvent() (sdl_shim.cpp) since that's
 * called every frame by the engine's own input polling -- longjmp()ing
 * directly from IRQ context would be unsafe. */
extern volatile int g_wolf_ctrlc_requested;

#ifdef __cplusplus
}
#endif
#endif
