title: Build systems
lede: make, CMake, Ninja and the autotools all run on tOS. This page shows how to build projects with each.
---
## make

GNU make 4.4 is in the `make` package (and comes with `gcc`). Run `make` in a folder with a `Makefile`; `make -C <dir>` builds in another folder; `make -j2` runs two jobs at a time (give the virtual machine enough memory first). Make uses the files' **modification times**, which tOS keeps, so it rebuilds only what changed.

### Makefile tips

- `make -n` shows what would run without running it.
- A rule needs a **tab** before its commands, not spaces.
- `$@` is the target, `$<` the first prerequisite and `$^` all of them.
- A pattern rule builds every object file the same way:

```
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
```

- `make VAR=value` overrides a variable, for example `make CC=gcc CFLAGS=-O2`.
- `make install DESTDIR=/tmp/stage` installs into a staging folder instead of the real one.

## CMake and Ninja

```
sudo tpkg install gcc cmake ninja
```

A small project:

```
cmake_minimum_required(VERSION 3.10)
project(demo C)
add_library(mylib STATIC lib/my.c)
add_executable(app app/main.c)
target_link_libraries(app mylib)
enable_testing()
add_test(NAME run COMMAND app)
```

Configure, build and test, out of the source tree:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build
```

Or with Makefiles instead of Ninja:

```
cmake -S . -B build2 -G "Unix Makefiles"
make -C build2
```

CMake 3.30 finds libraries through `find_package`: for zlib, install `zlib-dev` and use `find_package(ZLIB REQUIRED)` and `target_link_libraries(app ZLIB::ZLIB)`.

CMake options you will use often:

| Option | What it does |
|---|---|
| `-G Ninja` or `-G "Unix Makefiles"` | The build tool to generate for |
| `-DCMAKE_BUILD_TYPE=Release` | An optimised build (`Debug` for debugging) |
| `-DCMAKE_INSTALL_PREFIX=/tmp/inst` | Where `install` puts the files |
| `-DCMAKE_C_COMPILER=...` | A specific compiler |
| `cmake --build build` | Builds with whatever generator was used |
| `cmake --install build` | Installs |

## The autotools

For projects that come with a `configure.ac` and a `Makefile.am`:

```
sudo tpkg install gcc perl m4 autoconf automake libtool
```

Make the `configure` script, then build as usual:

```
libtoolize --copy --force
aclocal
autoconf
automake --add-missing --copy
./configure --prefix=$HOME/inst
make
make install
```

Projects that ship a ready-made `configure` need only the last three lines. `./configure` scripts start with `#!/bin/sh` and run with busybox's shell, and `make install` copies into the prefix you gave.

| Package | What it brings |
|---|---|
| `autoconf` | `autoconf`, `autoheader`, `autom4te`, `autoreconf`, `autoscan`, `autoupdate`, `ifnames` |
| `automake` | `automake`, `aclocal` |
| `libtool` | `libtool`, `libtoolize` |
| `m4` | The macro processor the others use |
| `perl` | Needed by autoconf and automake |
| `bison`, `flex` | Parser and scanner generators |
| `pkgconf` | `pkg-config` |

## bison and flex

```
bison -d calc.y          # makes calc.tab.c and calc.tab.h
flex calc.l              # makes lex.yy.c
gcc calc.tab.c lex.yy.c -o calc
```

## Tips

- If a build runs out of memory, give the machine more (`-m 4096`) and use fewer parallel jobs.
- A build that fails with "no syscall N" in the log asks for something tOS lacks: report it (see [Troubleshooting](/tos/troubleshooting)).

## Related

- [C and C++](/tos/dev-c) and [Shell scripting](/tos/dev-shell).
- [Building tOS from source](/tos/build-tos): the build of tOS itself, on a Linux host.
