title: SDL 2
lede: SDL 2.30 gives a program a window, the keyboard, the mouse, timers and 2D drawing. On tOS it draws in the window of the X server (tos-x). This page shows how to install it, build a program and run it.
---
## Install

```
sudo tpkg install sdl2
```

`sdl2` needs the `xorg` package (it loads `libX11` and `libXext` from there when a program starts) and brings the headers, `libSDL2` (shared and static), `sdl2-config` and a small example, `hello_sdl.c`. To build programs you also need `gcc`.

## Build and run a program

```
sudo tpkg install gcc sdl2 xorg
gcc /system/pkg/sdl2/hello_sdl.c $(sdl2-config --cflags --libs) -o hello_sdl
tos-x ./hello_sdl
```

`tos-x` starts the X server in a tOS window and runs the program there. The program is linked with a run-time search path that points at the `xorg` package, so nothing needs to be set.

## What this build has

| Part | Support |
|---|---|
| Video | The X11 driver (windows, the software renderer, `SDL_GetWindowSurface`), plus `dummy` and `offscreen` (no window) |
| Input | Keyboard, mouse and the wheel, through the X server |
| Timers, threads, events | Yes |
| Audio | The `dummy` and `disk` drivers only (no sound yet) |
| OpenGL, Vulkan, Wayland, KMS | No |

Choose a driver with `SDL_VIDEODRIVER=x11` (the default when `DISPLAY` is set), `dummy` or `offscreen`. Use `SDL_RENDERER_SOFTWARE` with `SDL_CreateRenderer`: there is no GPU.

## Status

On tOS itself the library loads, the `dummy` driver opens a window and draws into it, timers work, and `libX11` and `libXext` load from the `xorg` package. Drawing in a real X window (`tos-x`) was tried on a Linux host with a virtual X server; it has not been checked in a tOS window yet.

## Source and licence

SDL is under the zlib licence. The package carries the licence, a `NOTICE.txt` that says how it was built, and the SDL 2.30.9 source tarball it was built from. Its repository: [github.com/Artfical/tpkg-sdl2](https://github.com/Artfical/tpkg-sdl2).

## Related

- [Windowed programs (X11, Tk)](/tos/dev-x11) and [C and C++](/tos/dev-c).
