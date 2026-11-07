title: Builds, versions and upgrades
lede: The build number, min_build gating, versions with revision suffixes, how upgrade decides, how Software Update and packages relate, what happens to packages when tOS is updated.
---
## The build number

Every tOS release is identified by a **build number**: the number of commits on `main` minus an offset (currently 5). The kernel embeds this at compile time:

```
TOS_BUILD = git rev-list --count HEAD - 5
```

The kernel exports this as the environment variable `TOS_BUILD` and prints it at boot.

## min_build gating

Every package declares a `min_build` in its spec (the build number it was first tested on). The server only serves a package to builds where:

```
current_build >= package.min_build
```

If your tOS build is 618 and a package has `min_build: 619`, `tpkg look` will not show it. Update tOS first.

## Version format

Packages use `<upstream-version>-<revision>`:

| Package | Version | Meaning |
|---|---|---|
| `gcc` | `15.3.0-2` | GCC 15.3.0, tOS revision 2 |
| `vim` | `9.2.1254` | Upstream 9.2.1254, tOS revision 1 (omitted) |
| `zlib` | `1.2.13` | Upstream 1.2.13, tOS revision 1 |

The revision is incremented when the tOS packaging changes (patches, build flags, dependency updates) but the upstream version stays the same.

## How upgrade decides

`sudo tpkg upgrade` does:

1. `tpkg update` to refresh the index.
2. For each installed package, checks if the server has a newer `version` string (string comparison, not semantic).
3. If newer, runs `tpkg install` (which resolves dependencies again).

Version comparison is **lexicographic** on the full string. `15.3.0-2` > `15.3.0-1`, `3.46.1` > `3.45.1`.

## Software Update vs packages

- **Software Update** replaces the kernel, initrd and built-in apps. It runs `tpkg update` and then `tpkg upgrade` automatically.
- Packages are separate: you can `sudo tpkg upgrade` without a system update, and vice versa.
- A system update may change the build number, unlocking new packages.

## What happens to packages on update

- **Runtime packages** (`zlib`, `openssl`, `ncurses`, etc.) are upgraded in place.
- **Development packages** (`zlib-dev`, `openssl-dev`, etc.) are upgraded; their headers change.
- **GCC and toolchain** upgrades may require rebuilding local software (ABI changes are rare in glibc 2.35 but possible).
- **User-installed packages** (installed with `--unsigned`) are **not** touched by `tpkg upgrade`. They stay at their installed version until you reinstall them.

## Version pinning

To prevent a package from upgrading, remove it and reinstall the old version manually (download the old `.tpkg` from the server's archive). There is no `hold` command yet.

## Related

- [tpkg commands](/tos/tpkg-commands) — all subcommands.
- [The package server](/tos/tpkg-server) — the server side.
