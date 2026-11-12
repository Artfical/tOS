title: Package list
lede: Every package on the server, with its version and licence. Install one with `sudo tpkg install <name>`.
---
A package is offered to your system only when its build can run it, so the list on your system (`tpkg look`) can be shorter than this one until you have updated tOS. Packages marked `-dev` hold library headers and link files, and need `gcc`.

## Base system packages

| Package | Version | Licence | Description |
|---|---|---|---|
| busybox | 1.36.1-3 | GPL-2.0 | Core utilities and shell (ash) |
| zlib | 1.2.13 | Zlib | Compression library |
| openssl | 3.0.2 | Apache-2.0 | TLS/SSL and crypto |
| ca-certificates | 20260816 | MPL-2.0 | Trusted root certificates |
| ncurses | 6.5 | MIT | Terminal handling |
| readline | 8.2 | GPL-3.0 | Line editing |
| sqlite | 3.46.1 | Public domain | Embedded SQL database |
| libffi | 3.4.6 | MIT | Foreign function interface |
| liblzma | 5.8.4 | Public domain | LZMA compression |
| expat | 2.6.2 | MIT | XML parser |
| pcre2 | 10.44 | BSD-3 | Perl-compatible regex |
| libyaml | 0.2.5 | MIT | YAML parser |
| terminfo | 6.5 | MIT | Terminal capability database |

## Development packages (need gcc)

| Package | Version | Licence | Description |
|---|---|---|---|
| gcc | 15.3.0-3 | GPL-3.0+ | C/C++ compiler, includes binutils, make, glibc-dev |
| binutils | 2.46.1 | GPL-3.0+ | as, ld, ar, nm, objdump, strip, readelf |
| make | 4.4 | GPL-3.0 | Build automation |
| cmake | 3.30.5 | BSD-3 | Build system generator |
| ninja | 1.12.1 | Apache-2.0 | Fast build system |
| autoconf | 2.72 | GPL-3.0+ | Configure script generator |
| automake | 1.17 | GPL-2.0+ | Makefile generator |
| libtool | 2.5.4 | GPL-2.0+ | Shared library support |
| m4 | 1.4.19 | GPL-3.0+ | Macro processor |
| bison | 3.8.2 | GPL-3.0+ | Parser generator |
| flex | 2.6.4 | BSD-2 | Scanner generator |
| pkgconf | 2.3.0 | ISC | pkg-config implementation |
| pkgconf-dev | 2.3.0 | ISC | pkg-config headers |

## Languages and runtimes

| Package | Version | Licence | Description |
|---|---|---|---|
| python3 | 3.14.10 | PSF-2.0 | Python 3.14 with pip |
| python3.9 | 3.9.25 | PSF-2.0 | Python 3.9 with pip |
| python3.10 | 3.10.22 | PSF-2.0 | Python 3.10 with pip |
| python3.11 | 3.11.18 | PSF-2.0 | Python 3.11 with pip |
| python3.12 | 3.12.16 | PSF-2.0 | Python 3.12 with pip |
| python3.13 | 3.13.18 | PSF-2.0 | Python 3.13 with pip |
| python3-tk | 3.14.10 | PSF-2.0 | tkinter for Python 3.14 |
| python3.9-tk | 3.9.25 | PSF-2.0 | tkinter for Python 3.9 |
| python3.10-tk | 3.10.22 | PSF-2.0 | tkinter for Python 3.10 |
| python3.11-tk | 3.11.18 | PSF-2.0 | tkinter for Python 3.11 |
| python3.12-tk | 3.12.16 | PSF-2.0 | tkinter for Python 3.12 |
| python3.13-tk | 3.13.18 | PSF-2.0 | tkinter for Python 3.13 |
| sdl2 | 2.30.9-2 | Zlib | SDL 2: windows, input and 2D drawing for games (X11 backend), with headers |
| qt6-base | 6.8.2-2 | LGPL-3.0 | Qt 6: Core, Gui, Widgets, Network, Xml, Sql; xcb, linuxfb and offscreen platforms; headers, qmake, moc, cmake |
| xcb-libs | 1.17.0 | MIT | The libxcb extension libraries and headers (what Qt needs) |
| xcb-util | 0.4.1 | MIT | xcb-util: icccm, ewmh, image, keysyms, render-util, cursor |
| xkbcommon | 1.7.0 | MIT | Keyboard layouts for Qt |
| fontconfig | 2.15.0 | MIT | Finds fonts for Qt and other programs |
| fonts-dejavu | 2.37 | Bitstream Vera | DejaVu Sans, Serif and Mono (TrueType) |
| node24 | 24.22.1 | MIT | Node.js 24 with npm |
| go | 1.27.3 | BSD-3-Clause | Go compiler, go command and standard library (with a ready-made build cache) |
| rust | 1.99.1 | MIT OR Apache-2.0 | rustc, cargo, rustdoc and the standard library (needs gcc) |
| node22 | 22.23.4 | MIT | Node.js 22 LTS |
| node | 8.17.0-2 | MIT | Legacy Node 8 |
| perl | 5.40.0 | Artistic/GPL | Perl 5.40 with standard modules |
| tcltk | 8.6.16 | TCL | Tcl/Tk 8.6 |
| tsharp | built-in | custom | T# 4.1 Lite |
| python (MicroPython) | built-in | MIT | MicroPython REPL |

## Everyday tools

| Package | Version | Licence | Description |
|---|---|---|---|
| bash | 5.2.37 | GPL-3.0+ | GNU Bash 5.2 |
| busybox | 1.36.1-3 | GPL-2.0 | ash, coreutils, networking tools |
| coreutils | 9.5 | GPL-3.0+ | ls, cp, mv, sort, sha256sum, etc. |
| gawk | 5.3.1 | GPL-3.0+ | GNU awk 5.3 |
| sed | 4.9 | GPL-3.0+ | GNU sed 4.9 |
| grep | 3.11 | GPL-3.0+ | GNU grep with -P |
| findutils | 4.10.0 | GPL-3.0+ | find, xargs, locate |
| diffutils | 3.10 | GPL-3.0+ | diff, cmp, diff3, sdiff |
| patch | 2.7.6 | GPL-3.0+ | GNU patch |
| tar | 1.35 | GPL-3.0+ | Archive tool |
| gzip | 1.13 | GPL-3.0+ | Compression |
| bzip2 | 1.0.8 | BSD-3 | bzip2 compression |
| xz | 5.8.4 | Public domain | LZMA compression |
| less | 668 | GPL-3.0+ / BSD-2 | Pager |
| file | 5.46 | BSD-2 | File type detection |
| wget | 1.24.5 | GPL-3.0+ | Downloads with HTTPS |
| rsync | 3.3.0 | GPL-3.0+ | File synchronization |
| curl | 8.23.0 | MIT | Data transfer |
| vim | 9.2.1254 | Vim | Vi IMproved editor |
| nano | 9.3 | GPL-3.0+ | Friendly editor |
| tmux | 3.8 | ISC | Terminal multiplexer |
| htop | 3.5.3 | GPL-2.0 | Process viewer |
| ripgrep | 15.2.0 | MIT | Fast search (rg) |
| jq | 1.8.2 | MIT | JSON processor |
| git | 2.56.0-3 | GPL-2.0 | Version control |
| gdb | 17.2 | GPL-3.0-or-later | The GNU debugger (needs ptrace) |
| strace | 7.3 | LGPL-2.1-or-later | Shows a program's system calls |
| openssh | 10.6p1-5 | BSD-3 | SSH client and server |
| openssl | 3.0.2 | Apache-2.0 | TLS/SSL |
| zlib | 1.2.13 | Zlib | Compression library |
| sqlite | 3.46.1 | Public domain | Embedded database |
| pcre2 | 10.44 | BSD-3 | Regex library |
| libffi | 3.4.6 | MIT | FFI library |

## GUI and desktop

| Package | Version | Licence | Description |
|---|---|---|---|
| xorg | 1.19.7 | MIT | Xfbdev server, tos-x, X11 libs |
| tcltk | 8.6.16 | TCL | Tcl/Tk for windowed programs |
| python3-tk | 3.14.10 | PSF-2.0 | tkinter for Python 3.14 |
| python3.13-tk | 3.13.18 | PSF-2.0 | tkinter for Python 3.13 |

## System services

| Package | Version | Licence | Description |
|---|---|---|---|
| openssh | 10.6p1-5 | BSD-3 | SSH client and server |
| curl | 8.23.0 | MIT | HTTP/HTTPS client |

## GUI applications (built into the kernel image)

| App | Package | What it does |
|---|---|---|
| Files | built-in | File manager (icons, list, drag-drop, trash) |
| Terminal | built-in | Shell with tabs, scrollback, fonts |
| tFire | built-in | Web browser (HTTPS, JS, CSS) |
| Notepad | built-in | Text editor, syntax highlight |
| Paint | built-in | Drawing, layers, PNG export |
| Image Viewer | built-in | PNG/JPEG, zoom, wallpaper |
| PDF Viewer | built-in | Text extraction, search |
| Calculator | built-in | Arithmetic + scientific row |
| Clock | built-in | Analog/digital + stopwatch |
| Sticky Notes | built-in | 8-page yellow pad |
| Activity | built-in | Task manager |
| Network | built-in | Live network monitor |
| Disk Utility | built-in | Format, mount, First Aid |
| System Settings | built-in | Appearance, network, users, updates |
| Music | built-in | WAV/MP3/AAC/M4A player |
| Snake | built-in | Classic snake |
| 2048 | built-in | Sliding tile puzzle |
| DOOM | built-in | Shareware episode |
| Wolfenstein 3D | built-in | Shareware episode |
| 3D cube | built-in | Software rasterizer demo |
| vgatest | built-in | Framebuffer test |

## How to use this list

- Run `tpkg look` on your system to see what is available for *your* build.
- Run `tpkg search <text>` to find packages by name or description.
- Run `tpkg info <name>` for version, licence, size, signature and commands.
- Run `sudo tpkg install <name>` to install (needs root). Packages marked `-dev` need `gcc`.

The server only offers a package to builds that can run it. If a package is missing, update tOS with Software Update, then `sudo tpkg update`. If you still don't see it, ask on GitHub.
