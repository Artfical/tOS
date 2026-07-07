#ifndef TOS_WOLF3D_SDL_SYSWM_H
#define TOS_WOLF3D_SDL_SYSWM_H
/* wl_main.cpp's only use of this header (SDL_SysWMinfo/SDL_VERSION/
 * SDL_GetWindowWMInfo, to strip the Windows system menu off the game
 * window) is entirely inside `#if defined _WIN32`, which is never
 * defined in this freestanding cross-build -- so nothing here needs
 * to actually work, the include just needs to resolve. */
#endif
