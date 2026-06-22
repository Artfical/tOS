# tOS — Linux Like Operating System

tOS is a from-scratch x86 hobby operating system with a Linux-like command environment. It features a monolithic kernel with preemptive multitasking, a virtual filesystem layer, TCP/IP networking, a graphical UI, and an embedded MicroPython interpreter.

## System Requirements

- **CPU:** i586 compatible (32-bit x86)
- **Memory:** Minimum 32 MB recommended (3 MB absolute minimum)
- **Boot:** GRUB (Multiboot1) — bootable via ISO or PXE
- **Display:** VGA text mode (80x25), optional GUI mode with PS/2 mouse
- **Input:** PS/2 keyboard (mandatory), PS/2 mouse (optional)
- **Network:** RTL8139, PCnet (AMD Am79C970A/PCnet32), E1000, virtio-net, or NE2000 (RTL8029) NIC
- **Storage:** No disk required for boot — optional IDE disk for persistent storage via tFS
- **tFS:** [github.com/Artfical/tfs](https://github.com/Artfical/tfs) — custom persistent filesystem

## Building from Source

### Prerequisites

- GCC cross-compiler targeting i686-elf (or a 32-bit capable host GCC)
- GNU Make
- xorriso (for ISO generation)
- GRUB utilities (grub-mkrescue)

### Build Steps

```bash
git clone https://github.com/Artfical/tOS.git
cd tOS
make
```

The build produces `tOS.iso` in the project root.

### Running in QEMU

```bash
make run
```

Or manually:

```bash
qemu-system-i386 -cdrom tOS.iso -m 256
```

For networking (PCnet):

```bash
qemu-system-i386 -cdrom tOS.iso -m 256 -netdev user,id=net0 -device pcnet,netdev=net0
```

## Boot Process

1. GRUB loads the kernel at 0x200000 (2 MB) via Multiboot1.
2. The kernel sets up the Global Descriptor Table (GDT) and Interrupt Descriptor Table (IDT).
3. Interrupt Service Routines (ISR) and IRQ handlers are installed for exceptions 0--31 and hardware IRQs 0--15.
4. Memory detection occurs via Multiboot information; a page bitmap is initialized and a kernel heap is carved out.
5. The initrd (if present) is imported into ramfs; otherwise an empty ramfs is used.
6. The Virtual Filesystem (VFS) layer is initialized and ramfs is mounted at `/`.
7. The PS/2 keyboard driver is initialized.
8. An optional GUI prompt is shown; if declined, the system boots in CLI mode.
9. System calls (int 0x80) are initialized.
10. Networking subsystems are initialized (NIC detection, ARP, IP, ICMP, UDP, TCP, DNS, HTTP).
11. The MicroPython runtime is initialized (GC heap, VM state).
12. The shell starts.

## Shell Commands

| Command       | Description |
|---------------|-------------|
| `help`        | Display this help message |
| `echo`        | Echo arguments to the terminal |
| `clear`       | Clear the terminal screen |
| `pwd`         | Print current working directory |
| `ls [path]`   | List directory contents |
| `cd <path>`   | Change current directory |
| `mkdir <path>`| Create a directory |
| `rmdir <path>`| Remove an empty directory |
| `rm <path>`   | Remove a file |
| `touch <path>`| Create an empty file |
| `cat <path>`  | Display file contents |
| `mv <src> <dst>` | Move or rename a file |
| `cp <src> <dst>` | Copy a file |
| `edit <path>` | Simple line-based text editor |
| `exec <path>` | Execute an ELF program |
| `tsharp [file]` | Run T# 4.1 Lite (interactive or from file) |
| `python`      | Enter the MicroPython REPL |
| `ping <host>` | ICMP ping a host |
| `wget <url>`  | Download a file over HTTP |
| `reboot`      | Reboot the system |
| `shutdown`    | Halt the system |
| `version`     | Show kernel version |
| `about`       | About tOS |
| `uname`       | System information |

## Filesystem

tOS uses two filesystems:

- **ramfs** — Memory-resident filesystem mounted at `/`. Volatile — all data is lost on reboot.
- **tFS** — Persistent on-disk filesystem mounted at `/mnt` when an IDE disk is detected. Built on a custom on-disk format with 4 KB blocks, bitmap-based allocation, inline data for small files, and indirect/double-indirect block pointers.

At first boot with a blank IDE disk, an interactive installer (Turkish UI) formats the disk and copies system files. On subsequent boots, tFS is auto-detected and mounted.

The VFS layer supports:
- Hierarchical directories
- Symbolic links
- Unix-style permissions (uid/gid/mode)
- Standard POSIX-like operations (open, read, write, close, readdir, stat, mkdir, unlink, rename)

> **tFS source:** [github.com/Artfical/tfs](https://github.com/Artfical/tfs)

## Networking

The TCP/IP stack is implemented from scratch and includes:

- **ARP** — Address Resolution Protocol
- **IP** — Internet Protocol (no fragmentation)
- **ICMP** — Internet Control Message Protocol (ping only)
- **UDP** — User Datagram Protocol
- **DNS** — Domain Name System (A-record resolution)
- **TCP** — Transmission Control Protocol (basic window-based implementation)
- **HTTP/1.0** — Hypertext Transfer Protocol (client only, via wget)

Five NIC drivers are available:
- **RTL8139** — Realtek Fast Ethernet
- **PCnet** — AMD PCnet32 / Am79C970A (QEMU default)
- **E1000** — Intel PRO/1000 (qemu e1000 device)
- **virtio-net** — VirtIO paravirtualized NIC (legacy/transitional PCI transport)
- **NE2000** — Realtek RTL8029 / NE2000 PCI clone (QEMU `ne2k_pci` device)

NIC detection is automatic via PCI bus scanning.

## Multitasking

The scheduler uses the PIT (Programmable Interval Timer) at approximately 100 Hz. Tasks are scheduled in a round-robin fashion. The scheduler supports:

- Task spawning (`task_spawn`)
- Cooperative and preemptive yields (`task_yield`)
- Task sleep (`task_sleep`)
- Task exit (`task_exit`)
- Dedicated timer interrupt handler with stack switching

## GUI Mode

At boot, the user is prompted to enter GUI mode. When enabled:

- A title bar is drawn at the top of the screen.
- The terminal is offset below the title bar.
- A mouse cursor is rendered and tracks PS/2 mouse movement.
- The GUI polls mouse and keyboard events in the shell loop.

## MicroPython Interpreter

MicroPython v1.24.1 is embedded directly into the kernel. The interpreter is available via the `python` shell command.

### Built-in Modules

- **math** — Mathematical functions (sin, cos, sqrt, pow, floor, etc.)
- **random** — Pseudorandom number generation
- **struct** — Pack and unpack primitive data types
- **ubinascii** — Binary-to-ASCII conversion (hexlify, unhexlify, base64)
- **ure** — Regular expressions
- **array** — Typed arrays
- **collections** — namedtuple

### REPL Usage

```
/> python
MicroPython REPL on tOS
>>> print("hello from tOS")
hello from tOS
>>> import math
>>> math.sqrt(42)
6.480741
>>>
```

Press **Ctrl+D** to exit the REPL and return to the shell.

### Implementation Notes

- **GC Heap:** 16 KB statically allocated heap for the MicroPython garbage collector.
- **Float Format:** IEEE 754 single precision (32-bit `float`).
- **Integer Format:** MPZ arbitrary precision (no size limit on integers).
- **File I/O:** Not supported — `open()` returns `None`, `import` returns `MP_IMPORT_STAT_NO_EXIST`. Only RAM-backed execution is available.
- **Threading:** Disabled. MicroPython runs on the kernel's single task.
- **Input/Output:** stdin reads from the PS/2 keyboard, stdout writes to both VGA terminal and serial port (COM1).

## T# 4.1 Lite

T# is a minimal scripting language implemented within the kernel. It supports basic arithmetic, conditionals, and loops. It is available via the `tsharp` command.

## ELF Program Loading

The kernel can load and execute ELF binaries from the ramfs. Programs are loaded into memory and executed in user context. The `exec` shell command loads and runs ELF files.

## System Calls

System calls are invoked via `int 0x80` with the syscall number in `eax`. The following syscalls are available:

- `SYS_EXIT` (1) — Terminate the current process
- `SYS_WRITE` (4) — Write to a file descriptor
- `SYS_READ` (3) — Read from a file descriptor

## License

tOS itself is distributed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See the `LICENSE` file for details.

### MicroPython Licensing

This software includes the **MicroPython** core library (version 1.24.1), which is copyright (c) 2013--2024 Damien P. George and contributors. MicroPython is licensed under the **MIT License**, a permissive open-source license. The full terms of the MIT License are as follows:

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHER DEALINGS IN
THE SOFTWARE.
```

**Why MicroPython?**

MicroPython was chosen as the primary scripting language for tOS for the following reasons:

1. **Maturity and reliability.** MicroPython is a well-established, production-grade Python 3 implementation for constrained environments. Its codebase has undergone extensive testing across hundreds of embedded platforms.

2. **Minimal footprint.** The MicroPython core (`py/` directory) compiles to approximately 150--200 KB of machine code, making it suitable for a kernel-resident interpreter without requiring a separate userspace or filesystem.

3. **Standalone architecture.** MicroPython's `py/` directory is designed as a freestanding library with no dependencies on an operating system, libc, or filesystem. This made it feasible to embed directly into a custom kernel without porting an existing libc or POSIX layer.

4. **Familiar syntax.** Python is one of the most widely taught and used programming languages. Providing a Python environment within the OS lowers the barrier to entry for experimenting with kernel scripting, automation, and network utilities.

5. **Permissive licensing.** The MIT License is compatible with the AGPL-3.0 license used by the rest of tOS. No legal conflict or license incompatibility arises from distributing MicroPython alongside AGPL-3.0 code, as the two works remain separate and the MIT License imposes no restrictions on the aggregate work.

**Compliance Note:** The MicroPython source code included in this distribution is unmodified from the upstream release. A copy of the MIT License, as it applies to MicroPython, is included at `kernel/micropython/LICENSE`. The upstream repository is available at https://github.com/micropython/micropython.

## Credits

tOS is developed by Talha Berk. The system is built from scratch with the exception of:
- MicroPython (MIT License) — embedded scripting runtime
- GRUB (GPLv3) — bootloader (not distributed in source, only used to generate the ISO)
- htop (GPL-2.0-only) — inspiration for the `htop` process monitor command (original implementation, see `third_party/htop/`)

## Disclaimer

This software is provided for educational and research purposes. It is not intended for production use. The authors assume no liability for any damages arising from the use of this software.
