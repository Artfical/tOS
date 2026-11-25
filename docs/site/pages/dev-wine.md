title: Wine (Windows programs)
lede: Wine 10 runs Windows programs on tOS: console programs, windowed programs on the X server, and .msi installers. This page shows how to install it, run a program and install a package, and what does not work.
---
## Install

```
sudo tpkg install wine
```

`wine` brings what it needs: the `xorg` package (a window is drawn on the X server: `tos-x`) and `busybox`. It is large: 70 MB to download, 230 MB unpacked, and tOS keeps installed packages in memory, so give tOS 2 GB or more. It needs tOS build 634 or newer.

## Run a program

| To do this | Do this |
|---|---|
| Run a console program | `wine program.exe` |
| Run a windowed program | `tos-x wine program.exe` |
| Install a `.msi` package | `tos-x wine msiexec /i package.msi` |
| Install one without windows | `wine msiexec /i package.msi /qn` |
| Settings and the registry | `tos-x wine winecfg`, `tos-x wine regedit` |
| Open one from Files | Double-click an `.exe` or `.msi`: it opens in a Terminal with `tos-x wine` |

`tos-x` starts the X server in a tOS window and runs the command there; the window closes when the program ends.

The first time, `wine` makes the Wine folder, `~/.wine` (or the folder in `$WINEPREFIX`): the registry and folders are copied from a ready-made one, and the Windows libraries are linked in. This takes seconds. Programs see `C:` as `~/.wine/drive_c` and `Z:` as the whole tOS file system. A program that was installed is found under `C:\Program Files` (or `Program Files (x86)` for a 32-bit installer), that is `~/.wine/drive_c/Program Files`.

Every time Wine starts, it first runs its own start-up (`wineboot`: services and the registry). That takes seconds on a real computer, and minutes on a slow virtual machine.

## What is in it

| Part | Support |
|---|---|
| Programs | 64-bit Windows programs and console programs; `cmd`, `notepad`, `regedit`, `msiexec`, `winecfg` and the rest of Wine's own programs |
| Installers | `msiexec` and `.msi` packages (a 32-bit installer puts its files in `Program Files (x86)`) |
| Windows | Drawn on the X server (`tos-x`); fonts from Wine's own |
| Not in this package | 32-bit programs (`.exe` built for 32 bits), sound, OpenGL, Direct3D and Vulkan (there is no GPU), printing, the Mono and Gecko add-ons (so .NET programs and the built-in web page control are missing) |

## What was needed in tOS

Wine asks more of the kernel than most programs, so tOS grew for it (build 634): a fault in a program (a page that is not there, a divide by zero, `int3`) reaches the program's own `SIGSEGV`, `SIGFPE`, `SIGILL` or `SIGTRAP` handler with the address and the registers, which the handler may change; the GS base (where Wine keeps a thread's data) is kept for each thread; `tgkill` signals one thread; a bound `AF_UNIX` socket's file is a socket when it is looked at (`stat`); a read-only shared mapping sees what a writable one writes, even after the file is unlinked; a program may have 512 files open and tOS 40 processes; `epoll_create` and `getxattr` exist; and `SCM_RIGHTS` can send a program's standard input and output. `tools/linux-tests/wintest.c` tests these.

## Status

On tOS itself (in QEMU): `wine --version`, a Windows console program (`cmd /c echo`) and `msiexec` installing a small `.msi` run. Wine's start-up (`wineboot`, the services, `winedevice`) runs. Windowed programs on `tos-x` (Notepad and others) are drawn by the same code, but have not been looked at on tOS yet.

## Source and licence

Wine is under the LGPL-2.1 or later. The package carries the licence texts (and those of the libraries Wine includes: zlib, libpng, libjpeg, lcms2, libxml2, libxslt, mpg123, vkd3d and others), a `NOTICE.txt` that says how it was built, and the Wine 10.0 source tarball. Its repository: [github.com/Artfical/tpkg-wine](https://github.com/Artfical/tpkg-wine).

## Related

- [Windowed programs (X11, Tk)](/tos/dev-x11), [Qt and PyQt6](/tos/dev-qt) and [SDL 2](/tos/dev-sdl).
