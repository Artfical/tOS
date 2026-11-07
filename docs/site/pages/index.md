title: tOS
lede: tOS is a 64-bit operating system written from scratch. It boots to a desktop, has its own network stack and web browser, and runs real Linux programs.
---
> New to tOS? Start with [Installation](/tos/installation), then the [Quickstart](/tos/quickstart).

## What tOS is



tOS is a hobby operating system for x86-64 PCs, made by Artfical DT. The kernel, the desktop, the network stack, the file systems and the web browser are all written for tOS; only a few well-known libraries (listed in the licence notices) come from elsewhere.

It is not Linux and does not use the Linux kernel. What it has is a Linux compatibility layer: programs built for Linux on x86-64 run on it unchanged.

## What you can do with it


![The tOS desktop](shot:desktop.png)

- **Use a desktop.** Windows, a dock, Files, Settings, Terminal, Paint, Notepad, notifications, and Disk Utility. See [The desktop](/tos/desktop).
- **Run Linux programs.** Python, Node.js, Git, OpenSSH, vim, tmux, gcc, make and many more, installed from the package server. See [Linux programs](/tos/linux-programs).
- **Install packages.** `tpkg` installs signed packages and the packages they need. See [Packages](/tos/packages).
- **Develop software.** Compile C and C++ with gcc, build with make, cmake and the autotools, script in Python, Perl and bash. See [Developing on tOS](/tos/developing).
- **Browse the web.** tFire opens web pages over HTTPS, with pictures and JavaScript.
- **Connect.** IPv4 and IPv6, TCP and UDP, TLS, SSH. See [Networking](/tos/networking).
- **Write scripts.** MicroPython and T# are built in.

## How it is released

Every commit on the main branch is built, started in a virtual machine as a test, and published as a release with the installable disc image (`tOS.iso`) attached. Running systems update themselves with **Software Update**, and new packages appear on the package server for the builds that can use them.

## Where to go next

- [Installation](/tos/installation): get the image and start it.
- [Quickstart](/tos/quickstart): the first ten minutes.
- [Shell commands](/tos/commands) and the [package list](/tos/package-list).
- [Building tOS from source](/tos/build-tos) and the [contribution guide](https://github.com/Artfical/tOS/blob/main/CONTRIBUTING.md).

## Licence

tOS is free software under the GNU Affero General Public License v3.0. The third-party code it contains is listed, with its licences, in `THIRD_PARTY_NOTICES.md` in the source tree.
