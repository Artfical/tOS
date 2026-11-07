# Contributing to tOS

Thank you for helping. This page tells you how.

## Before you start

- **Bugs and ideas:** open an issue. For a bug, say what you did, what you expected, what happened, and the build (`version` in the Terminal, or the commit of your ISO). A serial log (`-serial file:serial.log` in QEMU) helps a lot.
- **Bigger changes:** open an issue first so we can agree on the direction before you spend time on it.
- **Security problems:** do not use issues. See [SECURITY.md](SECURITY.md).

## Building and testing

```bash
make                       # builds tOS.iso
make run                   # starts it in QEMU
```

The kernel has a self-test that boots in QEMU and runs the Linux test programs in `tools/linux-tests/`:

```bash
EXTRA_PKGS="..." tools/linux-tests/build-initrd.sh initrd.tar /tmp/selftest.tar
make NODE_STATIC_SELFTEST=1 kernel/tOS.elf
qemu-system-x86_64 -kernel kernel/tOS.elf -initrd /tmp/selftest.tar -m 4096 -display none -serial file:serial.log -no-reboot
```

Every run prints `[X64-TEST] exit=...`; a run that exits with anything but 0, a line containing `FAIL`, or a `PANIC` is a failure. Run it before you send a change that touches the kernel, the file systems or the system calls. Add a test for what you fix, in `tools/linux-tests/` (a C program or a script) and in the list of runs in `kernel/core/kernel.c`.

Host-side tests for single parts are in `tools/fs-test`, `tools/net-test`, `tools/ntfs-test` and `tools/tfire-test`.

## Writing code

- Match the code around you: naming, layout, and how much it is commented. Comment what is not obvious from the code, not what it does.
- The kernel is C, built with `-Wall -Wextra -Werror`: no warnings.
- Keep a change to one subject. Do not mix a fix with a clean-up of other files.
- 32-bit programs must keep working next to 64-bit ones.
- New third-party code needs a compatible licence. Put it in `third_party/` with its licence files and a `README.tOS` (where it came from, what was changed), and add it to `THIRD_PARTY_NOTICES.md` (`tools/gen_notices.py` makes it).

## Pull requests

1. Fork, and make a branch from `main`.
2. Make your change and test it as above.
3. Write a commit message that says what changed and why, in one or two lines.
4. Open the pull request and say how you tested it. If you could not test something, say so.

By contributing you agree that your work is licensed under the [AGPL-3.0](LICENSE), like the rest of tOS.

Please follow the [Code of Conduct](CODE_OF_CONDUCT.md).
