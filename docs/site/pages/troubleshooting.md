title: Troubleshooting and limits
lede: Common problems, and what tOS does not do yet.
---
## There is no network

Start QEMU with a network card: `-nic user,model=e1000`. Check with `ifconfig` that the card has an address, then `ping` a host.

## `tpkg look` does not show a package

The server lists a package only when your build can run it. Update tOS with Software Update, then run `sudo tpkg update`. `tpkg search <text>` finds packages by name or description.

## `tpkg install` says it needs root

Installing changes the system: use `sudo tpkg install <name>`. In the live environment the user is `tos` and the password is empty.

## `tpkg` refuses a package: signature

The package's signature does not fit. That means the package was changed or the download was damaged: try again, and if it persists, report it (see [Security](/tos/security)). `--unsigned` installs without the check, for packages you built yourself.

## A program says "command not found"

Check `tpkg list` for the package, and `tpkg which <command>` for where the command comes from. Scripts that start with `#!/bin/sh`, `#!/bin/bash` or `#!/usr/bin/env name` work once the package for that interpreter is installed.

## The system runs out of memory

Programs and packages live in memory. Give the machine more (`-m 4096`), and remove packages you do not need (`sudo tpkg remove <name>`).

## A program fails with "no syscall N"

The program asked the kernel for something tOS does not provide yet. The kernel logs it as `linux64: no syscall N`. In QEMU, start with `-serial file:serial.log` to keep the log. Please open an issue with the program, the number and what you did.

## The desktop does not start

Use `-vga std` in QEMU. The desktop needs a VBE framebuffer; without one, tOS falls back to a text-mode desktop.

## Known limits

- `gdb` and `strace` do not work yet (they need `ptrace`).
- Files larger than 2 GB are not supported: programs get an error.
- `tclsh` and `wish` stop with "can not find channel named stdout"; Tcl/Tk inside Python (tkinter) works.
- In the Terminal app, `Ctrl Z` does not suspend a program yet (it does in an SSH session).
- Software that needs containers, kernel modules or most of `/sys` does not run.
- Tested in virtual machines; real hardware may not work.
