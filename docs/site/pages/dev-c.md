title: C and C++
lede: gcc, g++, the GNU binutils and make build programs on tOS. This page shows how to install them, how to compile, how libraries are found, and how to link.
---
## Install the compiler

```
sudo tpkg install gcc
```

installs the compilers `gcc` and `g++` (GCC 15; also as `cc`, `c++` and `cpp`), and what they need: `binutils` (the assembler `as`, the linker `ld`, `ar`, `nm`, `objdump`, `strip` and more), `make`, and `glibc-dev` (the C library headers and link files, for glibc 2.35).

## Compile and run

Save this as `hello.c`:

```
#include <stdio.h>

int main(void)
{
    printf("hello from tOS\n");
    return 0;
}
```

```
gcc hello.c -o hello
./hello
```

The same for C++ (`hello.cc`):

```
g++ hello.cc -o hello
./hello
```

Useful options are the usual ones: `-O2` to optimise, `-Wall -Wextra` for warnings, `-g` for debug information, `-c` to compile without linking, `-I<dir>` and `-L<dir>` for header and library folders, `-l<name>` to link a library, `-std=c11` or `-std=c++17` to choose a language version, and `-static` for a program that has no shared-library dependencies.

## Several files, and make

```
gcc -c main.c
gcc -c util.c
gcc main.o util.o -o app
```

or a `Makefile`:

```
CC = gcc
CFLAGS = -O2 -Wall

app: main.o util.o
	$(CC) $^ -o $@

clean:
	rm -f app *.o
```

and `make` builds it. GNU make is version 4.4. For bigger projects see [Build systems](/tos/dev-build).

## Libraries: the `-dev` packages

Libraries come in two packages: the **runtime** (`zlib`, `openssl`) with the shared library that programs load, and the **development** package (`zlib-dev`, `openssl-dev`) with the headers and link files that the compiler needs. The development packages depend on `gcc`; the runtime packages do not.

| Library | Install | Link with |
|---|---|---|
| zlib | `zlib-dev` | `-lz` |
| OpenSSL 3 | `openssl-dev` | `-lssl -lcrypto` |
| liblzma (xz) | `liblzma-dev` | `-llzma` |
| The C library | `glibc-dev` (with `gcc`) | automatic; `-lm` for maths, `-lpthread` for threads, `-ldl`, `-lrt` |

A program that uses zlib:

```
#include <stdio.h>
#include <zlib.h>

int main(void)
{
    printf("zlib %s\n", zlibVersion());
    return 0;
}
```

```
sudo tpkg install zlib-dev
gcc zlibtest.c -o zlibtest -lz
./zlibtest
```

`pkgconf` (also installed as `pkg-config`) finds the flags for a library:

```
sudo tpkg install pkgconf
pkg-config --cflags --libs zlib
```

## Programs are linked against glibc 2.35

What you build runs on tOS and needs nothing else installed. It links against the same glibc that the packages use. To make a program with no shared-library dependencies at all, add `-static`.

## Tools

| Tool | What it does |
|---|---|
| `as`, `ld` | The assembler and the linker |
| `ar`, `ranlib` | Make static libraries |
| `nm`, `objdump`, `readelf`, `strings` | Look inside programs and libraries |
| `strip` | Remove symbols |
| `make` | Build from a Makefile |
| `pkg-config` | Find compile flags for libraries |

For debugging, install `gdb` and `strace`: see [Debugging: gdb and strace](/tos/dev-debug).

## If the compiler is not found by a build tool

Point the build at it: `CC=/system/pkg/gcc/bin/gcc` (and `CXX=/system/pkg/gcc/bin/g++`).

## Related

- [Build systems](/tos/dev-build): CMake, Ninja and the autotools.
- [Packages](/tos/packages).
