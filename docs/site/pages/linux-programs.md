title: Linux programs
lede: tOS runs programs built for Linux on x86-64, and for 32-bit x86, without changes. You install them with tpkg and start them from the Terminal. This page explains what runs, how, and what does not work yet.
---
## What runs

Programs built for Linux run as they are, whether they are **static** or linked against the GNU C library. Tested on tOS:

| Kind | What |
|---|---|
| Languages | Python (3.9 to 3.14, with pip), Node.js (several versions), Perl, Go, Rust, bash, Tcl/Tk, gcc and g++ |
| Version control and network tools | Git, curl, GNU wget (with HTTPS), OpenSSH (client and server), rsync |
| Editors and terminal tools | vim, nano, tmux, htop, ripgrep, jq, less |
| Build tools | make, cmake, ninja, autoconf, automake, libtool, m4, bison, flex, pkgconf |
| Everyday tools | GNU coreutils, sed, gawk, grep, find, tar, gzip, bzip2, xz, file, diffutils, patch |
| Libraries | zlib, OpenSSL 3, SQLite, readline, ncurses, libffi, expat, PCRE2, liblzma, libyaml |

See the [package list](/tos/package-list) for what exists and [Packages](/tos/packages) for how to install it.

## How it works

tOS has its own kernel. A **compatibility layer** in it provides what Linux programs ask of the system, so the programs do not know the difference:

- **Processes and threads:** `fork`, `vfork`, `exec`, `clone` (threads), `wait`, process groups and sessions, the futex calls that threads use to wait.
- **Signals:** handlers, masks, `sigaltstack`, `sigsuspend`, timers (`alarm`, `setitimer`, POSIX timers), `kill` and job control.
- **Files:** the usual calls for opening, reading, writing, seeking, linking, renaming, symbolic links, permissions and times; file locks (`fcntl` and `flock`); a file that is deleted while open stays usable until it is closed; several descriptors for one open file share its position, as on Linux.
- **Memory:** `mmap`, `mprotect`, `brk`, shared memory and semaphores.
- **Sockets:** TCP, UDP and local sockets (`AF_UNIX`, `socketpair`, passing descriptors), `epoll`, `poll` and `select`, loopback.
- **Terminals:** real pseudo-terminals, so programs see a terminal with a window size that changes, line editing and `Ctrl+C`.
- **`/proc`:** the files programs read to learn about themselves and the system.
- **Devices:** `/dev/null`, `/dev/zero`, `/dev/urandom`, the terminals, and the window devices below.

Each program has its **own address space, file table and working folder**, and runs as the user who started it. A program that crashes ends alone.

## Starting a program

```
sudo tpkg install python3
python3 --version
python3 -c "print('hello from Python')"
```

Programs you build or download yourself run by their path: `./myprogram`. They need the executable bit (`chmod +x`).

## Scripts

Scripts that start with `#!` work. The interpreters `/bin/sh`, `/bin/bash`, `/usr/bin/env` and the ones in `/usr/bin` are found through the package commands, so:

```
#!/bin/sh              # busybox sh (package busybox)
#!/bin/bash            # package bash
#!/usr/bin/env python3 # package python3
#!/usr/bin/perl        # package perl
```

all run once the package is installed.

## Windows

A Linux program that wants a window needs the X server:

```
sudo tpkg install xorg
tos-x <program> [arguments]
```

`tos-x` opens a tOS window, starts the X server (Xfbdev) inside it and runs the program there. The pointer, the mouse wheel and the keyboard reach the program. With the `tcltk` and `python3-tk` packages (or `python3.13-tk` and so on), Tk and tkinter programs open windows this way:

```
sudo tpkg install tcltk python3-tk
tos-x python3 my_tk_app.py
```

See [Windowed programs](/tos/dev-x11). Programs that draw to a framebuffer directly can use `/dev/fb0` for a window of their own, with input from `/dev/input/event0` (pointer) and `/dev/input/event1` (keyboard).

## Terminals and SSH

You get job control in an SSH session and in the Terminal app: `Ctrl+Z`, `fg`, `bg` and `jobs` work. In the Terminal app, programs get a terminal too (the window size reaches them, `Ctrl+C` stops them, full-screen programs work). See [Git and SSH](/tos/dev-git).

## 32-bit programs

32-bit x86 Linux programs run too, next to 64-bit ones, and so do the older tOS programs (`.t`).

## What does not work yet

- Debugging several threads of one program apart, hardware watchpoints and `PTRACE_SEIZE`: see [Debugging](/tos/dev-debug).
- **Files larger than 2 GB:** programs get a clear error instead of a wrong answer.
- Programs that need a Linux kernel feature tOS lacks: containers, kernel modules, most of `/sys`.
- `tclsh` and `wish` stop with "can not find channel named stdout". Tcl/Tk inside Python (tkinter) works.

If a program fails with a line `linux64: no syscall N` in tOS's log, it asks for something tOS does not provide yet. [Troubleshooting](/tos/troubleshooting) shows how to see the log. Bug reports about this are welcome: say which program and which number.

## Related

- [Packages](/tos/packages), [Developing on tOS](/tos/developing) and [How tOS works inside](/tos/architecture).
