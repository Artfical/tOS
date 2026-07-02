# tOS — Linux Like Operating System

tOS is a from-scratch x86 hobby operating system with a Linux-like command environment. It features a monolithic kernel with preemptive multitasking, a virtual filesystem layer, a multi-protocol TCP/IP network stack with IPv4 and IPv6, a graphical UI, and an embedded MicroPython interpreter.

**Current version: v0.9.39**

## System Requirements

- **CPU:** i586 compatible (32-bit x86)
- **Memory:** Minimum 32 MB recommended (3 MB absolute minimum)
- **Boot:** GRUB (Multiboot1 or Multiboot2, auto-detected) — bootable via ISO or PXE
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

1. GRUB loads the kernel at 0x200000 (2 MB) via Multiboot1 or Multiboot2 (the kernel detects which one was used from the magic value passed by the bootloader).
2. The kernel sets up the Global Descriptor Table (GDT) and Interrupt Descriptor Table (IDT).
3. Interrupt Service Routines (ISR) and IRQ handlers are installed for exceptions 0--31 and hardware IRQs 0--15.
4. Memory detection occurs via Multiboot information; a page bitmap is initialized and a kernel heap is carved out.
5. The initrd (if present) is imported into ramfs; otherwise an empty ramfs is used.
6. The Virtual Filesystem (VFS) layer is initialized and ramfs is mounted at `/`.
7. The PS/2 keyboard driver is initialized.
8. An optional GUI prompt is shown; if declined, the system boots in CLI mode.
9. System calls (int 0x80) are initialized.
10. Networking subsystems are initialized: NIC detection, ARP, IPv4/IPv6, ICMP/ICMPv6, UDP, UDP-Lite, TCP, SCTP, DCCP, DNS, HTTP, IPsec. A link-local IPv6 address is derived from the MAC address (EUI-64).
11. The MicroPython runtime is initialized (128 KB GC heap, VM state, all modules registered).
12. The shell starts.

## Shell Commands

### File System

| Command | Description |
|---|---|
| `ls [path]` | List directory contents |
| `cd <path>` | Change current directory |
| `pwd` | Print current working directory |
| `mkdir <path>` | Create a directory |
| `rmdir <path>` | Remove an empty directory |
| `rm <path>` | Remove a file |
| `mv <src> <dst>` | Move or rename a file |
| `cp <src> <dst>` | Copy a file |
| `touch <path>` | Create an empty file |
| `cat <path>` | Display file contents |
| `head [-n N] <path>` | Show the first N lines of a file |
| `tail [-n N] <path>` | Show the last N lines of a file |
| `wc <path>` | Count lines, words, and characters |
| `sort <path>` | Sort lines of a file alphabetically |
| `grep <pattern> <path>...` | Search for a pattern in one or more files |
| `find <path>` | Find files in a directory tree |
| `rev <path>` | Reverse the characters of each line |
| `uniq <path>` | Filter out adjacent duplicate lines |
| `hexdump <path>` | Hex dump of a file |
| `tee <path>` | Write stdin to both terminal and file |
| `edit <path>` | Simple line-based text editor |
| `exec <path>` | Execute an ELF binary |
| `chmod <mode> <path>` | Change file permissions |
| `disk` | Manage disks (list/info/mount/umount/format) |
| `tar c\|x\|t <archive> ...` | Create/extract/list a ustar archive |
| `zip <archive> <file...>` | Create a zip archive |
| `unzip <archive> [dest]` | Extract a zip archive |

### System

| Command | Description |
|---|---|
| `help` | Display command summary |
| `echo` | Echo arguments to the terminal |
| `clear` | Clear the terminal screen |
| `date` | Show the current date/time |
| `cal` | Show a calendar |
| `whoami` | Show the current user |
| `hostname` | Show the system hostname |
| `df` | Show filesystem disk usage |
| `free` | Show memory usage |
| `dmesg` | Show kernel log messages |
| `uptime` | Show system uptime |
| `ps` | List running processes |
| `htop` | Live process monitor |
| `kill <pid>` | Kill a process |
| `log` | Running tasks + full kernel/operation log |
| `yes [str]` | Print a string repeatedly |
| `seq [start] [step] <end>` | Print a sequence of numbers |
| `sleep <seconds>` | Delay for N seconds |
| `basename <path>` | Strip the directory from a path |
| `dirname <path>` | Strip the filename from a path |
| `which <cmd>` | Locate a command |
| `env` | Print environment variables |
| `alias [name=value]` | Set or list command aliases |
| `unalias <name>` | Remove a command alias |
| `history` | Show command history |
| `font` | List/change the terminal font style |
| `version` | Show kernel version |
| `about` | About tOS |
| `uname` | System information |
| `reboot` | Reboot the system |
| `shutdown` | Halt the system |

### Scripting

| Command | Description |
|---|---|
| `tsharp [file]` | Run T# 4.1 Lite (interactive or from file) |
| `python [file]` | Enter the MicroPython REPL or run a `.py` file |

### Networking

| Command | Description |
|---|---|
| `ping <host>` | ICMP ping (IPv4) — hostname or dotted-decimal IP |
| `ping6 <addr>` | ICMPv6 Echo Request/Reply (IPv6) |
| `ip6addr` | Show the kernel's link-local IPv6 address |
| `wget <url>` | Download a file over HTTP/1.0 |
| `sctp_connect <ip> <port>` | Open an SCTP association (blocking handshake) |
| `sctp_send <data>` | Send a DATA chunk on the current SCTP association |
| `sctp_close` | Close the current SCTP association (SHUTDOWN sequence) |
| `dccp_connect <ip> <port>` | Open a DCCP connection (REQUEST/RESPONSE handshake) |
| `dccp_send <data>` | Send a DCCP DATA datagram |
| `udplite_send <ip> <port> <data> [coverage]` | Send a UDP-Lite datagram; `coverage` = bytes covered by checksum (0 = full, 8 = header only) |
| `ipsec_sa` | Dump the IPsec Security Association (SA) table |

## Filesystem

tOS boots with two filesystems active by default, and can mount several more on demand:

- **ramfs** — Memory-resident filesystem mounted at `/`. Volatile — all data is lost on reboot.
- **tFS** — Persistent on-disk filesystem mounted at `/mnt` when an IDE disk is detected. Built on a custom on-disk format with 4 KB blocks, bitmap-based allocation, inline data for small files, and indirect/double-indirect block pointers.

At first boot with a blank IDE disk, an interactive installer (Turkish UI) formats the disk and copies system files. On subsequent boots, tFS is auto-detected and mounted.

### Mountable Disk Filesystems

The `disk` shell command can probe, mount, and format the following on-disk filesystems on any detected block device (IDE, AHCI, NVMe, or USB mass storage), all with full read/write support through the VFS layer:

| Filesystem | File | Notes |
|---|---|---|
| FAT16 | `fat16.c` | |
| FAT32 | `fat32.c` | |
| exFAT | `exfat.c` | |
| ext2 | `ext2.c` | |
| ext3 | `ext3.c` | Journaling |
| ext4 | `ext4.c` | Extents |
| NTFS | `ntfs.c` | |
| **Btrfs** | `btrfs.c` | B-tree leaf, inline + regular extents, CRC32 checksums |
| **XFS** | `xfs.c` | Shortform dirs, B-tree extents |

Use `disk list` to see detected block devices, `disk mount <device> <mountpoint> <fstype>` to mount one, and `disk format <device> <fstype>` to format it. The graphical Disk Manager app (see [GUI Mode](#gui-mode)) exposes the same operations visually.

> **tFS source:** [github.com/Artfical/tfs](https://github.com/Artfical/tfs)

## Networking

The network stack is implemented from scratch in `kernel/net/`. It supports both IPv4 and IPv6 and spans the following protocols:

### IPv4 Stack

| Protocol | File | Description |
|---|---|---|
| **ARP** | `arp.c` | Address Resolution Protocol; 16-entry cache |
| **IP** | `ip.c` | Internet Protocol (no fragmentation), dispatches to all transport protocols |
| **ICMP** | `icmp.c` | Echo Request/Reply (`ping`) |
| **UDP** | `udp.c` | User Datagram Protocol; 4-socket receive table |
| **TCP** | `tcp.c` | Transmission Control Protocol; single-connection blocking client |
| **DNS** | `dns.c` | A-record resolution |
| **HTTP** | `http.c` | HTTP/1.0 client (`wget`) |
| **SCTP** | `sctp.c` | Stream Control Transmission Protocol (RFC 4960) |
| **DCCP** | `dccp.c` | Datagram Congestion Control Protocol (RFC 4340) |
| **UDP-Lite** | `udplite.c` | Partial-checksum UDP (RFC 3828) |
| **IPsec AH** | `ipsec.c` | Authentication Header (RFC 4302): parse, replay detection, inner payload re-injection |
| **IPsec ESP** | `ipsec.c` | Encapsulating Security Payload (RFC 4303): parse + SA tracking (decryption requires IKE) |

### IPv6 Stack

| Protocol | File | Description |
|---|---|---|
| **IPv6** | `ip6.c` | 40-byte fixed header; link-local address auto-configured from MAC (EUI-64) at boot |
| **ICMPv6** | `icmpv6.c` | Echo Request/Reply (`ping6`), Neighbor Solicitation/Advertisement (NDP — IPv6 equivalent of ARP) |

### SCTP Details

SCTP (`kernel/net/sctp.c`) implements a full association finite state machine:

- CRC32c checksum (Castagnoli polynomial, RFC 4960 §6.8)
- INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK handshake (client side)
- DATA chunks with TSN, stream ID, and fragment flags
- SACK acknowledgement
- HEARTBEAT / HEARTBEAT-ACK echo
- SHUTDOWN / SHUTDOWN-ACK / SHUTDOWN-COMPLETE sequence
- ABORT handling

API: `sctp_connect(ip, port)` / `sctp_send(data, len)` / `sctp_recv(buf, max)` / `sctp_close()`

### DCCP Details

DCCP (`kernel/net/dccp.c`) implements the basic OPEN state:

- Short-sequence header (X=0, 12 bytes), pseudo-header checksum
- REQUEST → RESPONSE → ACK handshake
- DATA / DATAACK / ACK exchange
- CLOSE / RESET

API: `dccp_connect(ip, port)` / `dccp_send(data, len)` / `dccp_recv(buf, max)` / `dccp_close()`

### UDP-Lite Details

UDP-Lite (`kernel/net/udplite.c`) uses the same header layout as UDP but reinterprets the `length` field as `checksum_coverage` (RFC 3828 §3.1):

- `coverage = 0` → checksum covers the full datagram
- `coverage = 8` → only the 8-byte header is checksummed (payload is unprotected)
- Values 1–7 are normalised to 8 (RFC requirement)

### IPsec Details

IPsec (`kernel/net/ipsec.c`) handles inbound AH and ESP packets:

**AH (proto 51):** Parses the `next_header`, `payload_len`, SPI, and sequence number. Performs replay detection against the per-SA sequence counter. Reconstructs a synthetic IP header with `next_header` as the protocol, then re-injects the inner payload into `ip_handle()` so it is dispatched normally.

**ESP (proto 50):** Parses SPI and sequence number. Performs replay detection. Logs the encrypted payload (decryption is algorithm-specific and requires key material from IKE — not implemented). SA entries can be added manually with `ipsec_sa_add()` or are auto-learned from inbound packets.

Both AH and ESP maintain a global Security Association table (`ipsec_sa_table[8]`) accessible from the shell with `ipsec_sa`.

### ICMPv6 / NDP Details

ICMPv6 (`kernel/net/icmpv6.c`) handles:

- **Echo Request (type 128) / Echo Reply (type 129):** `ping6` sends an Echo Request and polls for the matching Reply.
- **Neighbor Solicitation (type 135):** NDP resolution — equivalent to ARP for IPv6. Sends a solicited-node multicast NS; extracts the MAC from the Target Link-Layer Address option in the response.
- **Neighbor Advertisement (type 136):** Responds to NS messages directed at our link-local address; includes our MAC in the Target Link-Layer Address option.

NDP results are stored in an 8-entry NDP cache. Unknown targets fall back to the solicited-node multicast MAC `33:33:ff:xx:xx:xx`.

### NIC Drivers

Five NIC drivers are available, all auto-detected via PCI bus scanning:

| Driver | File | Device |
|---|---|---|
| RTL8139 | `rtl8139.c` | Realtek Fast Ethernet |
| PCnet | `pcnet.c` | AMD PCnet32 / Am79C970A (QEMU default) |
| E1000 | `e1000.c` | Intel PRO/1000 |
| virtio-net | `virtio_net.c` | VirtIO paravirtualized NIC |
| NE2000 | `ne2000.c` | Realtek RTL8029 / NE2000 PCI clone |

## Multitasking

The scheduler uses the PIT (Programmable Interval Timer) at approximately 100 Hz. Tasks are scheduled in a round-robin fashion. The scheduler supports:

- Task spawning (`task_spawn`)
- Cooperative and preemptive yields (`task_yield`)
- Task sleep (`task_sleep`)
- Task exit (`task_exit`)
- Dedicated timer interrupt handler with stack switching

## GUI Mode

At boot, the user is prompted to enter GUI mode. When enabled, a window manager (`kernel/display/wm.c`) takes over:

- A title bar and a taskbar/dock with app icons are drawn.
- The terminal runs in its own movable, focusable window below the title bar.
- A mouse cursor is rendered and tracks PS/2 mouse movement (with IntelliMouse wheel support auto-detected at boot); windows can be dragged by their title bar, resized from the bottom-right corner, focused, and minimized.
- The GUI polls mouse and keyboard events in the shell loop.
- Right-clicking empty desktop space opens a context menu: New Folder, New File (opens straight into Notepad), Open Files, Open Terminal, About This Computer, Change Background (cycles through a few flat colors), and Refresh Desktop.

Several built-in GUI applications are launchable from the dock:

- **Notepad** (`notepad.c`) — text editor window (up to 1000 lines, with a scrollbar), can open/save files on any mounted filesystem, with Find (Ctrl+F), Replace All (Ctrl+R), and text selection (Shift+arrows, including across lines) with Copy/Cut/Paste (Ctrl+C/X/V)
- **Clock** (`clock.c`) — analog/digital clock
- **Calculator** (`calculator.c`) — basic operations plus scientific row: sin/cos/tan, sqrt, log/ln, x², and π, backed by from-scratch Newton's-method/Taylor-series math (no libm in this freestanding kernel)
- **Disk Manager** (`diskmgr.c`) — view and manage attached disks; every mount/unmount/format goes through `diskops.c`, which logs a start line, a result line, and — for format — a hex dump of sector 0 read back from disk afterward, all readable with the `log` shell command
- **Files** (`filemgr.c`) — graphical file manager with full mouse support: browse, copy/cut/paste, delete, rename, create folders, open files directly in Notepad or the Image Viewer, and switch between mounted filesystems (ramfs, tFS, FAT16/32, exFAT, ext2/3/4, NTFS, Btrfs, XFS) via a one-click disk bar. Supports multi-select (Ctrl+click to toggle individual files, Shift+click or Shift+arrows for a range) for bulk copy/cut/delete.
- **Paint** (`paint.c`) — mouse-driven drawing app: pen, eraser, and line/rectangle/circle shape tools (click-drag previews the shape live, release to commit it), 3 brush sizes, a 16-color VGA palette, and a "Save" that exports the canvas as a real, standard PNG file (cell grid rasterized to RGB pixels through a from-scratch PNG/zlib encoder)
- **Image Viewer** (`viewer.c`) — opens real PNG files (decoded through a from-scratch INFLATE/DEFLATE + PNG decoder in `png.c`, supporting stored/fixed/dynamic Huffman blocks and grayscale/RGB/palette/RGBA color types), downsampled and quantized to the nearest of the 16 VGA colors for display, with zoom in/out/fit and arrow-key panning.
- **Task Manager** (`taskmgr.c`) — live process list (scheduler state, uptime, memory usage), select a task with the mouse or arrow keys and kill it with a confirm prompt; refuses to kill the idle task or itself
- **About** (`about.c`) — system information window

## MicroPython Interpreter

MicroPython v1.24.1 is embedded directly into the kernel. The interpreter is available via the `python` shell command.

### Built-in Modules

| Module | Description |
|---|---|
| `math` | Mathematical functions (sin, cos, sqrt, pow, floor, etc.) |
| `random` | Pseudorandom number generation |
| `struct` | Pack and unpack primitive data types |
| `ubinascii` | Binary-to-ASCII conversion (hexlify, unhexlify, base64) |
| `ure` | Regular expressions |
| `array` | Typed arrays |
| `collections` | namedtuple |
| `os` | Filesystem operations: `getcwd`, `chdir`, `listdir`, `mkdir`, `remove`, `rename`, `stat`, `path.exists`, `path.isdir`, `path.isfile`, `path.join`, `path.basename`, `path.dirname` |
| `time` | Timer functions: `ticks_ms`, `ticks_us`, `ticks_diff`, `time`, `sleep`, `sleep_ms`, `sleep_us` |
| `json` | JSON encode/decode: `loads(s)`, `dumps(obj)` — supports dict, list, str, int, float, bool, null |
| `socket` | Network sockets: `socket(AF_INET, SOCK_STREAM/SOCK_DGRAM)`, `connect`, `bind`, `send`, `recv`, `sendto`, `recvfrom`, `close`, `getaddrinfo` |
| `tos` | tOS scripting API (see [Scripting API](#scripting-api)) |
| `tosgui` | Minimal GUI toolkit (see [tosgui](#tosgui)) |

### REPL Usage

```
/> python
MicroPython REPL on tOS
>>> print("hello from tOS")
hello from tOS
>>> import math
>>> math.sqrt(42)
6.480741
>>> import os
>>> os.listdir("/")
['programs', 'mnt']
>>> import time
>>> time.ticks_ms()
1843
>>> import json
>>> json.loads('{"x": 1, "items": [1,2,3]}')
{'x': 1, 'items': [1, 2, 3]}
>>> import socket
>>> s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
>>> s.sendto(b"hi", ("10.0.2.2", 5000))
2
>>> import tos
>>> tos.exec("ls /")
'd  /\nd  programs\n'
```

Press **Ctrl+D** to exit the REPL and return to the shell.

### File I/O

`open(path, mode)` returns a file object backed by the VFS layer:

```python
# Write a file
with open("/mnt/hello.txt", "w") as f:
    f.write("hello tOS\n")

# Read it back
with open("/mnt/hello.txt", "r") as f:
    print(f.read(64))

# Append
with open("/mnt/hello.txt", "a") as f:
    f.write("second line\n")
```

Supported modes: `r` (read), `w` (write + create + truncate), `a` (append + create), `r+`/`w+` (read-write).

### File Imports

Python modules stored on any mounted filesystem can be imported by path:

```python
# /mnt/mylib.py: def greet(n): return "hello " + n
import mylib           # searched on sys.path
print(mylib.greet("tOS"))
```

Module lookup uses `mp_import_stat()` which calls `fsbridge_exists()` / `fsbridge_is_dir()`. The file is read via `fsbridge_read()` and compiled in-memory.

### Implementation Notes

- **GC Heap:** 128 KB statically allocated heap; GC properly scans the C stack on each collection cycle (`gc_collect_root`).
- **Stack limit:** 32 KB soft limit tracked via `mp_stack_ctrl_init()`.
- **Float Format:** IEEE 754 single precision (32-bit `float`).
- **Integer Format:** MPZ arbitrary precision.
- **Threading:** Disabled. MicroPython runs on the kernel's single task.
- **Input/Output:** stdin reads from the PS/2 keyboard, stdout writes to both VGA terminal and serial port (COM1).
- **nlr_jump_fail:** Prints a fatal-error message and halts (no silent hang on unhandled exceptions).

## T# 4.1 Lite

T# is a minimal scripting language implemented within the kernel. It supports basic arithmetic, conditionals, and loops. It is available via the `tsharp` command. T# also has access to the [Scripting API](#scripting-api) below through built-in functions (`calistir`, `dosyaoku`, ...).

## Scripting API

Both MicroPython (the `tos` module) and T# (built-in functions) share a single underlying API (`kernel/shell/tos_api.c`) for running shell commands and touching the filesystem, GUI, and task list from a script.

| Capability | MicroPython | T# | What it does |
|---|---|---|---|
| Run a shell command | `tos.exec(cmd)` | `calistir(cmd)` | Runs any shell builtin with output captured and returned as a string |
| Read a file | `tos.read(path)` | `dosyaoku(path)` | |
| Write a file | `tos.write(path, data)` | `dosyayaz(path, data)` | |
| Create a directory | `tos.mkdir(path)` | `klasoryap(path)` | |
| Delete a file/dir | `tos.delete(path)` | `dosyasil(path)` | |
| Check a path exists | `tos.exists(path)` | `dosyavarmi(path)` | |
| List a directory | `tos.list(path)` | `listele(path)` | MicroPython gets a real list; T# gets newline-joined string |
| Open a GUI app | `tos.open_app(name)` | `uygulamaac(name)` | `name`: notepad, paint, files, viewer, calculator, clock, about, diskutil, taskmgr, terminal |
| List running tasks | `tos.ps()` | `surecler()` | `"pid name state"` lines |
| Kill a task | `tos.kill(pid)` | `sureldur(pid)` | Refuses the idle task and the caller's own task |
| Uptime in seconds | `tos.uptime()` | `calismasuresi()` | |
| Fetch a URL (HTTP only) | `tos.http_get(url)` | `agetir(url)` | Returns just the response body (headers stripped); MicroPython gets up to 8 KB, T# up to 256 bytes |

### tosgui

`tosgui` (`kernel/micropython/ports/tos/modtosgui.c`) lets a MicroPython script open its own GUI window and draw text/buttons into it.

```python
import tosgui as tg

tg.open("My App")
tg.text(2, 2, "Hello from MicroPython!")
tg.button(2, 4, "Quit", tg.WHITE, tg.RED)

while True:
    click = tg.poll_click()
    if click and click[1] == 4 and 2 <= click[0] <= 9:
        break
    tg.update()

tg.close()
```

| Function | What it does |
|---|---|
| `tg.open(title)` | Opens a window |
| `tg.close()` | Closes it and restores the previous context |
| `tg.clear()` | Clears the window |
| `tg.text(x, y, s, fg, bg)` | Draws text at a position |
| `tg.button(x, y, s, fg, bg)` | Draws a `[ s ]` button |
| `tg.poll_click()` | Returns `(x, y)` or `None` |
| `tg.poll_key()` | Returns an ASCII code or `None` |
| `tg.input(x, y, prompt)` | Blocking line input |
| `tg.has_focus()` | Whether this window is focused |
| `tg.update()` | Yields a frame to the rest of the OS |
| `tg.BLACK` … `tg.WHITE` | 16 VGA color constants |

### Running `.py` files

`python <path>` compiles and runs a MicroPython script straight out of tOS's VFS. A working `tosgui` demo ships at `/programs/tosgui_demo.py`:

```
/> python /programs/tosgui_demo.py
```

## `man` — the manual

`man <command>` prints a long-form description and at least one runnable example for every shell command, plus three scripting topics: `man tos_api`, `man tsharp_api`, and `man tosgui`. Run `man` with no arguments for the full list of pages.

## ELF Program Loading

The kernel can load and execute ELF binaries from the ramfs. Programs are loaded into memory and executed in user context. The `exec` shell command loads and runs ELF files.

## System Calls

System calls are invoked via `int 0x80` with the syscall number in `eax`:

| Syscall | Number | Description |
|---|---|---|
| `SYS_EXIT` | 1 | Terminate the current process |
| `SYS_FORK` | 2 | Fork the current process |
| `SYS_READ` | 3 | Read from a file descriptor |
| `SYS_WRITE` | 4 | Write to a file descriptor |
| `SYS_OPEN` | 5 | Open a file |
| `SYS_CLOSE` | 6 | Close a file descriptor |
| `SYS_WAITPID` | 7 | Wait for a child process |
| `SYS_EXECVE` | 11 | Execute a program |
| `SYS_CHDIR` | 12 | Change the current directory |
| `SYS_BRK` | 17 | Adjust the process heap break |
| `SYS_LSEEK` | 19 | Reposition a file descriptor's offset |
| `SYS_GETPID` | 20 | Get the current process ID |
| `SYS_KILL` | 37 | Send a signal to a process |
| `SYS_ISATTY` | 71 | Test whether a file descriptor is a terminal |
| `SYS_FSTAT` | 108 | Get file status |

## License

tOS itself is distributed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See the `LICENSE` file for details.

### MicroPython Licensing

This software includes the **MicroPython** core library (version 1.24.1), which is copyright (c) 2013--2024 Damien P. George and contributors. MicroPython is licensed under the **MIT License**:

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
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

**Compliance Note:** The MicroPython source code included in this distribution is unmodified from the upstream release. A copy of the MIT License is included at `kernel/micropython/LICENSE`. Upstream: https://github.com/micropython/micropython.

## Credits

tOS is developed by Talha Berk. The system is built from scratch with the exception of:
- MicroPython (MIT License) — embedded scripting runtime
- GRUB (GPLv3) — bootloader (not distributed in source, only used to generate the ISO)
- htop (GPL-2.0-only) — inspiration for the `htop` process monitor command (original implementation, see `third_party/htop/`)

## Disclaimer

This software is provided for educational and research purposes. It is not intended for production use. The authors assume no liability for any damages arising from the use of this software.
