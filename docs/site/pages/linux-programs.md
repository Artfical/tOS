title: Linux programs
lede: tOS runs Linux programs for x86-64, and for 32-bit x86, without changes: you install them with tpkg and start them from the Terminal.
---
## What runs

Programs built for Linux run as they are, whether they are static or linked against the GNU C library. Tested on tOS:

- **Languages:** Python (3.9 to 3.14, with pip), Node.js (several versions), Perl, bash, Tcl/Tk, gcc and g++.
- **Tools:** Git, curl, wget, OpenSSH (client and server), rsync, vim, nano, tmux, htop, ripgrep, jq.
- **Build tools:** make, cmake, ninja, autoconf, automake, libtool, bison, flex, pkgconf.
- **Everyday tools:** GNU coreutils, sed, gawk, grep, find, tar, gzip, bzip2, xz, less, file, diffutils, patch.

## How it works

tOS has its own kernel. The compatibility layer in it provides what Linux programs ask of the system: processes (`fork`, `exec`, threads), signals, pipes, sockets (TCP, UDP, local sockets), files and folders, symbolic links with their times and permissions, `/proc`, pseudo-terminals, shared memory, timers and `epoll`. Each program has its own address space, working folder and file table.

Terminals are real terminals: the window size reaches programs and `Ctrl C` stops the foreground program. SSH logins get a terminal too, with `Ctrl Z` and job control.

## Windows

A Linux program that wants a window needs the X server:

```
sudo tpkg install xorg
tos-x <program>
```

`tos-x` opens a tOS window, starts the X server (Xfbdev) inside it and runs the program there. The pointer, the wheel and the keyboard reach the program. With the `tcltk` and `python3-tk` packages, Tk and `tkinter` programs open windows this way:

```
sudo tpkg install tcltk python3-tk
tos-x python3 my_tk_app.py
```

Programs that draw to the framebuffer directly can use `/dev/fb0` for a window of their own, with input from `/dev/input/event0` (pointer) and `/dev/input/event1` (keyboard).

## Where things are

- Packages: `/system/pkg/<name>`.
- Commands: on your PATH by name, and also under `/usr/bin` and `/bin`, so scripts that start with `#!/bin/sh`, `#!/bin/bash` or `#!/usr/bin/env python3` work.
- The C library and its helpers: in the base system.

## What does not work yet

- Debuggers that use `ptrace` (gdb, strace).
- Files larger than 2 GB: programs get a clear error instead of a wrong answer.
- Programs that need a Linux kernel feature tOS lacks (containers, the full set of `/sys`, kernel modules).

If a program fails with a "no syscall N" line in tOS's log, it asks for something tOS does not provide yet. [Troubleshooting](/tos/troubleshooting) shows how to see the log, and bug reports about it are welcome.
