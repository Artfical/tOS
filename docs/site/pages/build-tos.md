title: Building tOS from source
lede: How to build tOS yourself, run it, run its tests, and send a change. This page is for people who want to read or change the system.
---
## What you need

A Linux host with:

- GCC for x86-64, with `gcc-multilib` (for the 32-bit example programs)
- GNU Make
- `xorriso` and the GRUB tools (`grub-mkrescue`) to make the disc image
- QEMU to run it

## Build and run

```
git clone https://github.com/Artfical/tOS.git
cd tOS
make          # builds tOS.iso
make run      # starts it in QEMU
```

| Make target | What it does |
|---|---|
| `make` (`all`) | Builds the kernel and `tOS.iso` |
| `make kernel/tOS.elf` | Builds only the kernel |
| `make run` | Starts `tOS.iso` in QEMU |
| `make run-audio` | The same, with a Sound Blaster 16 |
| `make clean` | Removes what was built |

Useful QEMU flags: `-m 2048` for memory, `-nic user,model=e1000` for networking, `-serial file:serial.log` to keep the kernel log, `-display none` for tests.

## The source tree

| Folder | What is in it |
|---|---|
| `kernel/core` | Start-up, the scheduler, processes, memory, the clock, Software Update |
| `kernel/fs` | The VFS, ramfs, tFS and the disk file systems, system calls, the Linux layer (`linux64.c`, `syscall.c`), `/proc`, pseudo-terminals |
| `kernel/net` | The network stack and the network card drivers |
| `kernel/drivers` | Storage, USB, input, video, audio and other hardware drivers |
| `kernel/display` | The window manager, the desktop and its apps (Files, Settings, tFire, Paint, ...) |
| `kernel/shell` | The shell and its commands, the scripting API |
| `kernel/micropython` | The MicroPython port |
| `kernel/doom`, `kernel/wolf3d` | The game ports |
| `boot` | The boot code |
| `tools` | Host-side tools: asset generators, tests, benchmarks |
| `third_party` | Code from other projects, each with its licence and a `README.tOS` |
| `docs` | Screenshots, these documentation sources (`docs/site`) and the older reference |

## The self-test

The kernel can boot in QEMU and run Linux test programs against a disc image that has the packages they use. This is how every release is checked.

```
EXTRA_PKGS="glibc-dev binutils make gcc python3" \
  tools/linux-tests/build-initrd.sh initrd.tar /tmp/selftest.tar
make NODE_STATIC_SELFTEST=1 kernel/tOS.elf
qemu-system-x86_64 -kernel kernel/tOS.elf -initrd /tmp/selftest.tar -m 4096 \
  -display none -serial file:serial.log -no-reboot
```

`build-initrd.sh` takes the packages from the update server's folder (`/root/tos-update/pkg` by default) and the test programs in `tools/linux-tests/`. Each test run prints `[X64-TEST] exit=...` in the log. A run that does not exit with 0, a line with `FAIL`, or a `PANIC` is a failure. A full run takes about an hour under emulation.

Host-side tests for single parts are in `tools/fs-test` (file systems), `tools/net-test`, `tools/ntfs-test` and `tools/tfire-test` (the browser's parser and layout).

## How a change gets released

Every push to `main` is built from a clean tree by the release workflow (`.github/workflows/release.yml`), started in QEMU as a test, and published as a release with `tOS.iso` attached. A running system sees the new build through Software Update. The build number is the number of commits on `main` (less an offset), and packages say the oldest build they need.

## Sending a change

1. Read [CONTRIBUTING.md](https://github.com/Artfical/tOS/blob/main/CONTRIBUTING.md) and the [Code of Conduct](https://github.com/Artfical/tOS/blob/main/CODE_OF_CONDUCT.md).
2. Make a branch from `main`, change one thing, and test it: build it, run it, and run the self-test if you touched the kernel, the file systems or the system calls.
3. Add a test for what you fix (a C program or a script in `tools/linux-tests/`, and an entry in the list of runs in `kernel/core/kernel.c`).
4. Open a pull request and say how you tested it.

The kernel is built with `-Wall -Wextra -Werror`: no warnings. Match the code around you: names, layout and how much it is commented.

tOS is under the AGPL-3.0, so your contribution is too. New third-party code needs a compatible licence, its files in `third_party/` with a `README.tOS`, and a line in `THIRD_PARTY_NOTICES.md` (made by `tools/gen_notices.py`).

## These docs

The documentation you are reading is built from `docs/site` by `tools/gen_docs_site.py`: one Markdown file per page, `nav.json` for the sidebar. A pull request that fixes a page is as welcome as one that fixes code.

## Related

- [How tOS works inside](/tos/architecture).
