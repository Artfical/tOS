title: Troubleshooting and limits
lede: What to do when something does not work, how to get the information that a bug report needs, and what tOS does not do yet.
---
## Getting information

When something goes wrong, three things help:

- `version` shows the build you run. Say it in a report.
- `dmesg` and `log` show what the kernel logged.
- In a virtual machine, start QEMU with `-serial file:serial.log`: the whole log goes to a file, including what is printed before the desktop starts and when something fails.

## There is no network

1. Start QEMU with a card: `-nic user,model=e1000`.
2. `ifconfig` should show the card and an IPv4 address (from DHCP).
3. `ping 1.1.1.1` tests the connection. If that works but `ping example.com` does not, name lookup is the problem.
4. Check that the host itself has internet access.

A card the stack does not know (not virtio-net, E1000, RTL8139, PCnet or NE2000) is not used.

## A certificate cannot be verified

Linux programs use the `ca-certificates` package: install it (`sudo tpkg install ca-certificates`). For the built-in `wget` and tFire, the list of trusted roots is built in; `tls trust <file>` adds a root until the next restart, and `wget -k` or `tls verify off` turn the check off for a server you know and trust.

## `tpkg look` does not show a package

The server lists a package only when your build can run it. Update tOS with Software Update, then run `sudo tpkg update`. `tpkg search <text>` finds packages by name or description.

## `tpkg install` says it needs root

Installing changes the system: use `sudo tpkg install <name>`. In the live environment the user is `tos` and there is no password.

## `tpkg` refuses a package: signature

The package's signature does not fit. The package was changed or the download was damaged: try again, and if it keeps happening, report it (see [Security](/tos/security)). `--unsigned` installs without the check, for packages you built yourself.

## `tpkg remove` says another package needs it

Remove the package that needs it first, or run `sudo tpkg remove <name> --force` (the others may stop working).

## A program says "command not found"

Check `tpkg list` for the package, and `tpkg which <command>` for where the command comes from. If you installed the package in this session, open a new Terminal window. Scripts that start with `#!/bin/sh`, `#!/bin/bash` or `#!/usr/bin/env name` work once the package for that interpreter is installed.

## A command does something different than I expected

If two packages (or a package and the shell) bring the same name, the latest installed one is used. tpkg warns when it takes a name over. `which <command>` shows which one runs.

## The system runs out of memory

Programs and packages live in memory. Give the machine more (`-m 4096`) and remove packages you do not need: `sudo tpkg remove <name>`. `free` shows the memory use.

## A program fails with "no syscall N"

The program asked the kernel for something tOS does not provide yet. The kernel logs it as `linux64: no syscall N`. Please open an issue with the program, the number and what you did.

## A program crashes right away

A fault in a program ends only that program, with a message. If it is a 32-bit program, check that it is built for x86; if it needs libraries, install the packages for them (the runtime packages: `zlib`, `openssl`, `ncurses`, ...).

## A windowed program does not open

- Run it with `tos-x <program>` from the Terminal, and install `xorg` first.
- The X server writes its messages to `/tmp/Xfbdev.log`: look at it with `cat /tmp/Xfbdev.log`.
- For Tk programs install `tcltk` (and `python3-tk` for tkinter).
- `tclsh` and `wish` currently fail with "can not find channel named stdout": use Tk through Python.

## The desktop does not start

Use `-vga std` in QEMU. The desktop needs a VBE framebuffer; without one, tOS falls back to a text-mode desktop.

## No sound

- QEMU: start with `-soundhw sb16`.
- VirtualBox: choose the ICH AC97 audio controller.
- `soundinfo` shows which backend is in use, and `beep test` plays a click.
- `beep on` turns the system sounds on.

## A USB disk does not show up

Start QEMU with `-usb` and plug the disk in from the monitor (see [Notifications and USB disks](/tos/notifications)). The disk must be formatted FAT16/32, exFAT or ext2/3/4. For other formats use `disk list` and `disk mount`.

## A build fails

- Out of memory: more memory (`-m 4096`) and fewer parallel jobs.
- A tool cannot find the compiler: `CC=/system/pkg/gcc/bin/gcc`.
- A `-dev` package is missing: install it, for example `sudo tpkg install zlib-dev`.
- "no syscall N" in the log: see above.

## Known limits

- `gdb` and `strace` do not work yet (they need `ptrace`).
- Files larger than 2 GB are not supported: programs get an error.
- `tclsh` and `wish` stop with "can not find channel named stdout". Tcl/Tk inside Python (tkinter) works.
- In the Terminal app, `Ctrl+Z` does not suspend a program yet (it does in an SSH session).
- Software that needs containers, kernel modules or most of `/sys` does not run.
- xterm, GTK, Qt and SDL are not available yet.
- tOS is tested in virtual machines; real hardware may not work.

## Reporting a bug

Open an issue at [github.com/Artfical/tOS/issues](https://github.com/Artfical/tOS/issues). Say what you did, what you expected, what happened, the build (`version`), and attach the serial log if you can. For a security problem, do not open an issue: see [Security](/tos/security).
