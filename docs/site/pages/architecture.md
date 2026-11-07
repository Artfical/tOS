title: How tOS works inside
lede: A tour of the system from power-on to a running program: boot, memory, tasks and processes, the file systems, the Linux compatibility layer, the network stack, drivers, the desktop, and how updates and packages fit in.
---
## The big picture

tOS is a **monolithic kernel**: the scheduler, memory manager, file systems, network stack, drivers, the Linux compatibility layer, the window manager and the desktop apps all run in one 64-bit kernel image. Programs run in user mode, each in its own address space, and talk to the kernel through system calls.

| Layer | What it holds |
|---|---|
| Programs | Linux programs (64-bit and 32-bit), tOS programs (`.t`), packages |
| System calls | The Linux system-call interface for Linux programs, tOS's own for `.t` programs |
| Kernel services | Processes and threads, memory, VFS and file systems, sockets and the network stack, pseudo-terminals, `/proc`, the window manager |
| Drivers | Storage, network cards, USB, input, video, audio |
| Boot | GRUB loads the kernel; the kernel starts everything else |

## Boot

1. **GRUB** loads the kernel at 2 MB through Multiboot (1 or 2) and starts it.
2. The kernel sets up the **descriptor tables** (GDT and IDT), **exception and interrupt handlers**, and the timer.
3. It reads the **memory map**, starts a **page-frame allocator** and the **kernel heap**.
4. The **initial file system image** (initrd) is imported into the RAM file system, which becomes `/`.
5. The **VFS** starts and the file systems are mounted. Disks are probed.
6. **Input** (PS/2 keyboard and mouse), the **display** (a VBE framebuffer if there is one) and the **system-call interface** start.
7. The **network stack** starts: card detection, ARP, IPv4 and IPv6, ICMP, UDP, TCP, SCTP, DCCP, DNS, DHCP, HTTP and HTTPS (TLS 1.2), IPsec and the tunnels.
8. The **MicroPython** runtime starts (128 KB heap).
9. The first screen: the **login window** on an installed system, **tOS Recovery** otherwise (or on request with `bootrec` or Shift). See [Recovery](/tos/recovery).
10. The desktop (or, without a display, the shell) starts.

## Memory

- **Paging:** every process has its own page directory. The kernel's half of the address space is shared but is not accessible to user code; the user half holds only that program's code, heap, stacks and mappings, and all of it is freed when the program ends.
- **System-call pointers are checked:** a pointer a program passes to the kernel is checked against that program's own page tables, so a pointer into the kernel, or into a hole, is refused instead of followed.
- **The kernel heap** is a first-fit allocator that checks itself: links and block headers are bounds-checked, a canary after every block catches overruns when it is freed, and double frees and frees of non-heap pointers are reported instead of acted on. A corrupted free list is reported once and the allocator carries on.
- **`mmap`** and friends give Linux programs anonymous memory, file mappings and shared memory.

## Tasks and processes

The **scheduler** runs tasks round-robin, driven by a timer at about 100 Hz, with tasks that yield, sleep and wake. A **process** (a program, from `proc.c`) owns an address space, a table of open files, a working folder, credentials and signal state; **threads** are tasks that share a process.

- Each process runs as the user who started it. A program with the setuid or setgid bit runs as its owner or group, if nobody else can change the file.
- A fault in a program (a bad pointer, an illegal instruction) ends **that program only**, with a message; the system goes on. A fault in the kernel itself is a panic, shown on the [crash screen](/tos/games).
- `fork`, `exec`, `clone`, `wait`, signals, pipes, sockets and job control work as on Linux.

## The Linux layer

Linux programs call the kernel with the Linux system-call numbers and calling convention. The compatibility layer (mostly `kernel/fs/linux64.c` and `syscall.c`) turns those calls into tOS's own services: it loads ELF files (static and dynamic, 64-bit and 32-bit, with the glibc loader), sets up the stack, the auxiliary vector and thread-local storage, and provides `/proc`, pseudo-terminals, `epoll`, futexes, timers, local sockets and the rest. Where tOS lacks a call, the program gets a clean error and the kernel logs `linux64: no syscall N`, so missing features are easy to find. See [Linux programs](/tos/linux-programs).

## The file systems

All file systems sit behind a **VFS** layer that gives each one the same operations (open, read, write, seek, stat, readdir, mkdir, unlink, rename, symlink, chmod). ramfs (the root) is a tree of inodes in memory with symbolic links, modification times and permission bits. tFS, FAT, exFAT, ext2/3/4, NTFS, Btrfs and XFS are the on-disk drivers, over the block layer of the IDE, AHCI, NVMe and USB-storage drivers. Permissions are checked in the VFS, against the credentials of the process. See [Files, disks and users](/tos/filesystems).

## The network stack

Written from scratch: a driver per card, then link (Ethernet, ARP, VLAN, bridges, bonds), network (IPv4, IPv6, ICMP, routing, the firewall with connection tracking and NAT), transport (TCP, UDP, UDP-Lite, SCTP, DCCP), and the application protocols (DNS, DHCP, HTTP, TLS). Linux programs reach it through the socket calls of the compatibility layer; the shell tools and tFire use it directly. See [Networking](/tos/networking).

## The desktop

The **window manager** (`kernel/display/wm.c`) draws windows, the dock and the top bar on the framebuffer, and delivers the mouse and keyboard. Apps are small programs in `kernel/display/apps/` that draw into their window through one interface. The fonts and icons are generated at build time. Linux programs that draw windows run on an X server inside a window of their own: the window gives the program a framebuffer (`/dev/fb0`) and input devices. See [The desktop](/tos/desktop).

## Updates and packages

**Software Update** asks the update server for the newest build and, if there is one, loads it. **tpkg** fetches packages from the same server, checks their ECDSA signature against the key built into the kernel, and unpacks them into `/system/pkg`. Each package says the oldest build it needs, so a package is offered only to the systems that can run it. See [Packages](/tos/packages).

## Where to read the code

| To understand | Start in |
|---|---|
| The boot | `kernel/core/kernel.c` |
| A system call | `kernel/fs/syscall.c`, `kernel/fs/linux64.c` |
| Processes | `kernel/core/proc.c`, `kernel/core/scheduler.c` |
| A file system | `kernel/fs/ramfs.c`, `kernel/fs/vfs.c`, then `ext2.c`, `fat32.c`, ... |
| TCP | `kernel/net/tcp.c`, `kernel/net/lsock.c` |
| The desktop | `kernel/display/wm.c`, `kernel/display/apps/` |
| The shell | `kernel/shell/shell.c`, `kernel/shell/commands/` |
| Packages | `kernel/shell/commands/cmd_tpkg.c` |

## Related

- [Building tOS from source](/tos/build-tos).
