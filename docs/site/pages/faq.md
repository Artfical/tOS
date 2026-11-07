title: FAQ
lede: Short answers to the questions people ask first.
---
## Is tOS Linux?

No. tOS has its own kernel, written from scratch. It can run programs built for Linux (x86-64 and 32-bit x86), through a compatibility layer, so most of what you know from Linux works.

## Can it run Windows programs?

No.

## Who makes tOS?

tOS is made by Artfical DT.

## What licence is it under?

The GNU Affero General Public License v3.0. The third-party code it contains is under its own licences, listed in `THIRD_PARTY_NOTICES.md`.

## Can I run it on real hardware?

It is tested in QEMU, VirtualBox and VMware. It may start on a real PC, but that is not what it is tested on.

## How do I get software?

With `tpkg`: `tpkg look` lists what there is and `sudo tpkg install <name>` installs it. See [Packages](/tos/packages) and the [package list](/tos/package-list).

## How do I update tOS?

Software Update, from the desktop, installs the newest build. You can also download a newer image from the [Releases page](https://github.com/Artfical/tOS/releases).

## Where is the source code of the packages?

Next to each package on the server: the source tarball it was built from is hosted for free software that requires it. The licences and a NOTICE are there too.

## How do I report a bug, or a vulnerability?

Bugs: an issue on [GitHub](https://github.com/Artfical/tOS/issues). Vulnerabilities: privately, to security@artfical.com. See [Security](/tos/security).

## How can I help?

Read [CONTRIBUTING.md](https://github.com/Artfical/tOS/blob/main/CONTRIBUTING.md). Bug reports, tests, documentation and code are all welcome.
