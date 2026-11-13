title: DRM and KMS
lede: tOS has a DRM/KMS device, /dev/dri/card0, with dumb buffers: Linux programs that set a video mode through the kernel and draw in memory buffers (KMS programs, compositors that use the DRM backend with software rendering) have a screen to draw on. There is no GPU: everything is drawn by the CPU.
---
## What it is

Opening `/dev/dri/card0` gives a program a tOS window (the same 800x560 window `/dev/fb0` gives; see [Windowed programs](/tos/dev-x11)) and the DRM calls that show a picture in it:

| Call | What it does here |
|---|---|
| `DRM_IOCTL_VERSION`, `GET_CAP` | Driver `tos`; dumb buffers are supported |
| `MODE_GETRESOURCES`, `GETCONNECTOR`, `GETENCODER`, `GETCRTC` | One virtual connector (connected) with one mode, 800x560 at 60 Hz, one encoder and one CRTC |
| `GETPLANERESOURCES`, `GETPLANE`, `OBJ_GETPROPERTIES` | One primary plane (XRGB8888 and ARGB8888) with its `type` property |
| `MODE_CREATE_DUMB`, `MAP_DUMB`, `DESTROY_DUMB` | A buffer of up to 1024x768 pixels, 32 bits, one page per row (pitch 4096); `mmap` it to draw |
| `MODE_ADDFB`, `ADDFB2`, `RMFB` | A framebuffer made of a dumb buffer |
| `MODE_SETCRTC`, `SETPLANE`, `MODE_PAGE_FLIP` | Shows a framebuffer in the window; a flip with `DRM_MODE_PAGE_FLIP_EVENT` queues an event that `read()` and `poll()` deliver |
| `WAIT_VBLANK`, `SET_MASTER`, `DROP_MASTER`, `MODE_DIRTYFB` | Answered at once |

The window shows the framebuffer that was set last, as it is (a program may keep drawing in it: no copy is needed), and the window's size is always 800x560. A program that asks for the atomic API (`DRM_CLIENT_CAP_ATOMIC`) is refused and uses the legacy calls above; there is no hardware cursor (`MODE_CURSOR` fails, as it does on a GPU-less virtual machine: use a software cursor).

## What it is not

There is no GPU: no OpenGL or Vulkan, no `GBM`, no GEM buffers other than dumb ones, no render node (`/dev/dri/renderD128`) and no PRIME. Qt's `eglfs`, and anything else that needs EGL, does not run. Draw with a software renderer (pixman, Qt's raster engine, SDL's software renderer).

## Try it

`tools/linux-tests/drmtest.c` in the tOS source is a program that makes these calls with no library at all: it asks for the resources and the mode, makes a dumb buffer, draws in it, shows it with `SETCRTC`, flips it and waits for the event. Compile it on tOS (`gcc drmtest.c -o drmtest`) and run it from a Terminal on the desktop: the window opens and the program reports each step.

## Status

The calls above were run on tOS itself by that test, with the window and the mapping working (the kernel was started with a test window instead of the desktop). It has not been tried with the desktop's own window on screen, nor with libdrm or a compositor yet.

## Related

- [Windowed programs (X11, Tk)](/tos/dev-x11), [SDL 2](/tos/dev-sdl), [Qt](/tos/dev-qt).
