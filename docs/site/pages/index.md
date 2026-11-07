title: tOS
lede: tOS is a 64-bit operating system written from scratch. It boots to a desktop, has its own network stack, file systems and web browser, and runs real Linux programs.
---
> New to tOS? Read [Installation](/tos/installation) to get it running, then the [Quickstart](/tos/quickstart). This page explains what tOS is and how the documentation is laid out.

![The tOS desktop](shot:desktop.png)

## What tOS is

tOS is an operating system for x86-64 PCs, made by Artfical DT. The kernel, the desktop, the network stack, the file systems, the drivers and the web browser are all written for tOS. Only a few well-known libraries come from elsewhere (for example MicroPython, QuickJS and NetSurf's HTML and CSS libraries); each one is listed with its licence in the notices that ship with every copy of tOS.

tOS is **not Linux** and does not use the Linux kernel. It does have a Linux compatibility layer, so programs built for Linux on x86-64 (and 32-bit x86) run on it without changes. That is what lets a system this small have Python, Node.js, Git, gcc, vim and thousands of everyday tools.

## A tour

### The desktop

The desktop looks and behaves like the desktop you know from a modern computer: a top bar with the clock, a dock with the apps, windows that you drag, resize and minimise, a launcher, notifications and a Trash. The default screen is 1280x720, and Settings can switch it up to 1920x1080, light or dark, with an accent colour.

![The launcher shows every app](shot:launcher.png)

See [The desktop](/tos/desktop), [The apps](/tos/apps), [Files](/tos/files-app) and [Settings](/tos/settings).

### A real terminal

The Terminal app gives you a shell with dozens of built-in commands, tab completion, history and a manual (`man`). Packages add more: after `sudo tpkg install git`, `git` is just there.

![The Terminal](shot:terminal.png)

See [Terminal and shell](/tos/terminal) and [Packages](/tos/packages).

### A web browser of its own

**tFire** opens pages over HTTPS and HTTP, with HTML, CSS and JavaScript, pictures, downloads and back and forward history. It is built into tOS, not a port of another browser.

![tFire showing Wikipedia](shot:tfire/u3-wikipedia.png)

See [tFire](/tos/tfire).

### Linux programs

Programs built for Linux run unchanged. You install them from the package server with `tpkg`, and they get a real terminal, real processes, signals, sockets and a file system. Windowed programs run on an X server in a tOS window.

See [Linux programs](/tos/linux-programs) and [Developing on tOS](/tos/developing).

### Games and demos

DOOM and Wolfenstein 3D (their shareware episodes), a software 3D renderer, Snake and 2048 are built in.

![DOOM running on tOS](shot:doom.png)

See [Games and demos](/tos/games).

## What is inside

| Part | What it is |
|---|---|
| Kernel | A 64-bit kernel with processes, threads, a scheduler, paging, signals, pipes and sockets |
| Linux layer | Runs Linux ELF programs (64-bit and 32-bit), with `/proc`, pseudo-terminals, shared memory, timers and `epoll` |
| File systems | A RAM file system, tFS (tOS's own), FAT16/32, exFAT, ext2/3/4, NTFS, Btrfs and XFS, with Linux-style permissions |
| Network | IPv4 and IPv6, TCP, UDP, SCTP, DCCP, TLS 1.2, DHCP, DNS, a firewall with NAT, VLANs, bridges, tunnels |
| Drivers | Network cards (virtio, E1000, RTL8139, PCnet, NE2000), IDE, AHCI, NVMe, USB storage, PS/2 keyboard and mouse, Sound Blaster 16 and AC97 audio, a VBE framebuffer |
| Desktop | A window manager, a dock, Files, Settings, Terminal, Paint, Notepad and more |
| tFire | The web browser |
| Scripting | MicroPython and T# in the kernel, with an API to the desktop |
| Packages | `tpkg`, with signed packages from the update server |
| Updates | Software Update installs newer builds |

## How tOS is released

Every commit on the main branch is built from a clean tree, started in a virtual machine as a test, and published as a release with the installable disc image (`tOS.iso`) attached. A running system updates itself with **Software Update**, and the package server offers each package to the builds that can run it. There are no big numbered releases to wait for: the newest build is the release.

## Who it is for

tOS is for people who like to understand a whole computer: students and hobbyists who want to read and change a real operating system, and people who want to try a small system that still runs the tools they already know. It is tested in virtual machines (QEMU, VirtualBox and VMware), and it is not made for production use.

## How this documentation is organised

| Section | What you will find |
|---|---|
| [Getting started](/tos/installation) | Installing tOS, the Recovery screen and the installer, your first ten minutes |
| [Desktop](/tos/desktop) and [Apps](/tos/tfire) | The desktop, Files, Settings, notifications, tFire, the editors, the system apps, games and audio |
| [System](/tos/terminal) | The shell, packages, Linux programs, file systems and users, networking |
| [Developing](/tos/developing) | Compilers, build systems, Python, Node.js, Git and SSH, X11, scripting, building tOS itself and how it works inside |
| [Reference](/tos/commands) | Every shell command and every package |
| [Security](/tos/security) and [Help](/tos/troubleshooting) | How tOS protects you, how to report a problem, troubleshooting and the FAQ |

## Licence and source

tOS is free software under the GNU Affero General Public License v3.0, and the source is on [GitHub](https://github.com/Artfical/tOS). The code that comes from other projects is under its own licences, listed in `THIRD_PARTY_NOTICES.md`; tFire shows the list at `tfire:licenses`. The app icons shown in these pages are from the WhiteSur icon theme (GPL-3.0).

![tFire's licence page lists everything tOS contains from other projects](shot:tfire/u3-licenses.png)
