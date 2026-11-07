title: tpkg commands in depth
lede: Every subcommand of the tpkg package manager, with exact behaviour, output meaning, exit codes and examples.
---
## Overview

| Subcommand | Changes system? | Needs sudo? | What it does |
|---|---|---|---|
| `look` | No | No | Lists all packages on the server for your build |
| `search <text>` | No | No | Finds packages by name or description |
| `info <name>` | No | No | Shows version, licence, signature, size, commands, dependencies |
| `list` | No | No | Shows what is installed locally |
| `which <cmd>` | No | No | Shows the package a command comes from |
| `update` | No | No | Refreshes the local cache of the server list |
| `install <name>` | **Yes** | **Yes** | Downloads, verifies and installs a package |
| `remove <name>` | **Yes** | **Yes** | Uninstalls a package (if nothing else needs it) |
| `upgrade` | **Yes** | **Yes** | Installs newer versions of installed packages |

> **sudo rule:** Any subcommand that changes the system (`install`, `remove`, `upgrade`) requires root. Use `sudo`. Read-only subcommands (`look`, `search`, `info`, `list`, `which`, `update`) work for everyone.

## Detailed reference

### `tpkg look`

Shows every package the server offers for your build. Output is a table:

```
name       | version      | licence          | description
-----------|--------------|------------------|-----------------------------
bash       | 5.2.37       | GPL-3.0+         | GNU Bash 5.2, the shell...
coreutils  | 9.5          | GPL-3.0+         | GNU coreutils 9.5...
```

Run `tpkg look | grep python` to filter.

### `tpkg search <text>`

Finds packages whose name or description contains the text (case-insensitive):

```bash
tpkg search editor
# vim      9.2.1254  Vim  Vi IMproved editor
# nano     9.3      GPL-3.0+  Friendly editor
```

### `tpkg info <name>`

Shows everything about one package:

```
Package:     vim
Version:     9.2.1254
License:     Vim
Size:        1234567
Unpacked:    3456789
Signature:   valid (ECDSA P-256)
Depends:     ncurses, terminfo
Commands:    vim, vi, view, vimdiff, xxd
Desc:        Vi IMproved editor
Notice:      Vim is a highly configurable text editor...
```

The **Signature** line tells you whether the package's ECDSA P-256 signature verified. A package with an invalid signature will say `invalid` or `missing`.

### `tpkg list`

Shows what is installed on this machine:

```
bash       5.2.37
coreutils  9.5
gcc        15.3.0-2
vim        9.2.1254
```

### `tpkg which <command>`

Shows which package a command comes from:

```bash
tpkg which vim
# vim -> /system/pkg/vim/bin/vim
```

### `tpkg update`

Downloads the fresh package index from the server. Run this after a Software Update or if `tpkg look` seems stale.

```bash
sudo tpkg update
```

### `tpkg install <name>`

Installs a package and its dependencies. Steps:

1. Resolves the name to a package version allowed for your build (`min_build`).
2. Recursively resolves dependencies (depth-first).
3. Downloads each `.tpkg` from the server.
4. Verifies the ECDSA P-256 signature (fails if invalid).
5. Unpacks into `/system/pkg/<name>`.
6. Writes `tpkg.commands` entries to `/system/path.tmbl` so the commands appear on PATH.
7. Runs any `tpkg.message` the package includes.

```bash
sudo tpkg install vim
```

Flags:
- `--unsigned` — skip signature check (shows a warning). Use only for packages you built yourself.
- `--force` — overwrite an existing install.

### `tpkg remove <name>`

Uninstalls a package. Refuses if another installed package depends on it:

```bash
tpkg remove vim
# vim is needed by: git (uses vimdiff)
```

Force removal (other packages may break):

```bash
sudo tpkg remove vim --force
```

### `tpkg upgrade`

Installs newer versions of every installed package that has an update:

```bash
sudo tpkg upgrade
```

Run `tpkg update` first to see what is new.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | General error (bad args, not found, etc.) |
| 2 | Needs root (run with sudo) |
| 3 | Signature verification failed |
| 4 | Dependency not available for this build |
| 5 | Package already installed (install) / not installed (remove) |
| 6 | Dependency conflict (remove) |
| 7 | Disk full / I/O error |
| 8 | Server unreachable |

## Tips

- `tpkg look | less` for paging.
- `tpkg search <text>` finds packages by partial name or description.
- `tpkg info <name> | grep Depends` shows only dependencies.
- After `sudo tpkg install`, the new commands are on PATH immediately (shell hashes the PATH on first use).

## See also

- [Packages](/tos/packages) — overview of the package system.
- [What happens when you install](/tos/tpkg-install) — the install pipeline in detail.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
- [Signatures](/tos/tpkg-signatures) — how signing works.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [How commands reach your PATH](/tos/tpkg-commands-path).
