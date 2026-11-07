title: Building tOS from source
lede: How to build tOS yourself, run it, and run its tests.
---
## What you need

A host with GCC for x86-64 (with `gcc-multilib` for the 32-bit example programs), GNU Make, `xorriso` and the GRUB tools (`grub-mkrescue`). QEMU to run it.

## Build and run

```
git clone https://github.com/Artfical/tOS.git
cd tOS
make          # builds tOS.iso
make run      # starts it in QEMU
```

`make run-audio` starts it with a Sound Blaster 16. Other useful flags: `-m 2048` for memory, `-nic user,model=e1000` for networking and `-serial file:serial.log` to keep the kernel's log.

## The source tree

| Folder | What is in it |
|---|---|
| `kernel/` | The kernel: core, memory, file systems, network, drivers, the Linux layer, the desktop and its apps |
| `boot/` | The boot code |
| `tools/` | Host-side tools and tests |
| `third_party/` | Code from other projects, each with its licence and a `README.tOS` |
| `programs/`, `assets/` | Programs and pictures that go into the image |
| `docs/` | Screenshots and these documentation sources |

## The self-test

The kernel can boot in QEMU and run the Linux test programs in `tools/linux-tests/` against a disc image that has the packages they use:

```
EXTRA_PKGS="gcc glibc-dev binutils make python3" tools/linux-tests/build-initrd.sh initrd.tar /tmp/selftest.tar
make NODE_STATIC_SELFTEST=1 kernel/tOS.elf
qemu-system-x86_64 -kernel kernel/tOS.elf -initrd /tmp/selftest.tar -m 4096 \
  -display none -serial file:serial.log -no-reboot
```

Each run prints `[X64-TEST] exit=...` in the log. A run that does not exit with 0, a line with `FAIL`, or a `PANIC` is a failure. A full run takes about an hour under emulation. Host-side tests for single parts are in `tools/fs-test`, `tools/net-test`, `tools/ntfs-test` and `tools/tfire-test`.

## Contributing

Read [CONTRIBUTING.md](https://github.com/Artfical/tOS/blob/main/CONTRIBUTING.md) before you send a change, and the [Code of Conduct](https://github.com/Artfical/tOS/blob/main/CODE_OF_CONDUCT.md). tOS is under the AGPL-3.0: your work is under it too.
