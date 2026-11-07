title: FAQ
lede: Short answers to the questions people ask first.
---
## About tOS

### Is tOS Linux?

No. tOS has its own kernel, written from scratch. It can run programs built for Linux (x86-64 and 32-bit x86) through a compatibility layer, so most of what you know from Linux works.

### Can it run Windows programs?

No.

### Who makes tOS?

tOS is made by Artfical DT.

### What licence is it under?

The GNU Affero General Public License v3.0. The code that comes from other projects is under its own licences, listed in `THIRD_PARTY_NOTICES.md` (and in tFire at `tfire:licenses`).

### Is it for production use?

No. It is for learning and experiments.

## Using it

### Can I run it on a real computer?

It is tested in QEMU, VirtualBox and VMware. It may start on a real PC, but that is not what it is tested on.

### How much memory does it need?

1 GB or more is recommended. Programs and packages live in memory, so give more (2 GB, or 4 GB for big builds) if you install a lot.

### Is there a login password in the live environment?

No: the user is `tos` and the password is empty, so `sudo` works without one. On an installed system you choose the password in the installer.

### Where do my files go?

The root file system is in memory; disks (tFS, ext4, FAT, ...) are mounted as folders. See [Files, disks and users](/tos/filesystems).

### How do I get back to Recovery?

Run `bootrec` in the Terminal, or hold Shift while the machine starts. See [Recovery and the installer](/tos/recovery).

## Software

### How do I get software?

With `tpkg`: `tpkg look` lists what there is and `sudo tpkg install <name>` installs it. See [Packages](/tos/packages) and the [package list](/tos/package-list).

### Can I install any Linux program?

Programs built for Linux on x86-64 usually run. They have to be built for glibc (not musl, mostly), and tOS does not provide everything Linux does: see [Linux programs](/tos/linux-programs) for the limits. Anything that needs `ptrace`, containers or kernel modules does not work yet.

### Where is the source code of the packages?

Next to each package on the server: the source tarball a package was built from is hosted for the free software that requires it. The licences and a NOTICE are there too.

### Why are some packages not in the list?

The server offers a package only to builds that can run it. Update tOS, then `sudo tpkg update`. If a package is really not there, ask for it in an issue.

### Does tOS have a compiler?

Yes: `sudo tpkg install gcc`. See [C and C++](/tos/dev-c).

### Can I program in Python, JavaScript, ...?

Yes. See [Python](/tos/dev-python), [Node.js](/tos/dev-node) and [Shell scripting](/tos/dev-shell). MicroPython and T# are built in.

## Updates

### How do I update tOS?

Software Update (in Settings) installs the newest build. You can also download a newer image from the [Releases page](https://github.com/Artfical/tOS/releases).

### How often is there a new build?

Every commit on the main branch becomes a build, so there are many. Each one is started in a virtual machine as a test before it is published.

## Helping

### How do I report a bug, or a vulnerability?

Bugs: an issue on [GitHub](https://github.com/Artfical/tOS/issues). Vulnerabilities: privately, to security@artfical.com. See [Security](/tos/security) and [Troubleshooting](/tos/troubleshooting).

### How can I help?

Read [CONTRIBUTING.md](https://github.com/Artfical/tOS/blob/main/CONTRIBUTING.md). Bug reports, tests, documentation and code are all welcome. See [Building tOS from source](/tos/build-tos).
