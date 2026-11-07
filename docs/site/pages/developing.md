title: Developing on tOS
lede: tOS can build software on itself: compilers, build tools and languages are packages you install.
---
## C and C++

```
sudo tpkg install gcc
```

installs `gcc` and `g++` with `binutils`, `make` and the C library headers (`glibc-dev`). Library headers come as `-dev` packages (`zlib-dev`, `openssl-dev`, `liblzma-dev`, ...). Their runtime libraries are separate packages without the compiler.

```
gcc -O2 main.c -o main
g++ main.cc -o main
make
```

Programs you build link against the glibc 2.35 that tOS carries, the same as the packages.

## Build systems

| Tool | Package | Notes |
|---|---|---|
| make | `make` | GNU make 4.4 |
| CMake | `cmake` | 3.30, with `ninja` or `make` |
| Ninja | `ninja` | |
| Autotools | `autoconf`, `automake`, `libtool`, `m4` | Need `perl`. `./configure` scripts run with `/bin/sh` |
| Parsers | `bison`, `flex` | |
| pkg-config | `pkgconf` | Finds the libraries of the `-dev` packages. Both `pkgconf` and `pkg-config` work |

A CMake project:

```
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build
```

An Autotools project:

```
libtoolize --copy --force
aclocal && autoconf && automake --add-missing --copy
./configure --prefix=$HOME/inst
make && make install
```

If a build tool cannot find the compiler, point it there: `CC=/system/pkg/gcc/bin/gcc`.

## Languages

| Language | Packages |
|---|---|
| Python | `python3` (3.14), `python3.9` to `python3.13`, each with `pip`; `python3-tk` and the other `-tk` packages add tkinter |
| Node.js | `node` (8), `node22`, `node24`, with npm |
| Perl | `perl` 5.40 with the standard modules |
| Shell | `bash`, and busybox's `sh` |
| Tcl/Tk | `tcltk` |

MicroPython (`python`) and T# (`tsharp`) are built in and need no package.

## Tools

- **Version control:** `git`, including over HTTPS and SSH.
- **Editors:** `vim`, `nano`, `tmux`, and the built-in `edit`.
- **Everyday:** `ripgrep`, `jq`, `htop`, `curl`, `wget`, `rsync`, and the GNU tools (`coreutils`, `sed`, `gawk`, `grep`, `findutils`, `diffutils`, `patch`, `tar`, `gzip`, `less`, `file`).

## Windowed programs

Install `xorg` (and `tcltk` and `python3-tk` for Tk), then start your program with `tos-x`:

```
tos-x python3 my_app.py
```

## Memory

The root file system and everything in it is in RAM, so a build that produces many files needs memory. Give the virtual machine 2 GB or more (`-m 2048`), and 4 GB for large builds.
