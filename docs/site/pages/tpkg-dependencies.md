title: Dependencies
lede: How a package declares dependencies, how tpkg resolves and orders them, runtime versus -dev packages, conflicts with removal, and real examples.
---
## Declaration

A package declares its runtime dependencies in `tpkg.depends` (one per line):

```
zlib
openssl
ncurses
```

The package maintainer lists only **direct** dependencies; tpkg resolves the transitive closure.

## Resolution algorithm

1. Start with the requested package.
2. For each dependency in `tpkg.depends`:
   - Resolve the dependency recursively (depth-first).
   - If already resolved, skip.
3. Install in **post-order**: dependencies first, then the package itself.

This guarantees that when a package's files are unpacked, all its dependencies are already on the PATH and in `/system/pkg/`.

## Runtime vs -dev packages

| Kind | Example | Depends on | Who needs it |
|---|---|---|---|
| Runtime | `zlib` | (none) | Programs at runtime |
| Development | `zlib-dev` | `gcc`, `zlib` | Compiling/linking programs |

The `-dev` package **depends on `gcc`** because its headers are only useful when compiling. The runtime package does not.

Example for `python3-tk`:

```
python3-tk
  depends: python3, tcltk
tcltk
  depends: xorg, zlib
xorg
  depends: zlib, openssl, pcre2
```

The `-dev` packages (`zlib-dev`, `openssl-dev`, etc.) are only needed when *building* something that links against that library.

## Removal conflicts

tpkg refuses to remove a package if another installed package depends on it:

```
tpkg remove zlib
# zlib is needed by: python3, openssl, git, curl, vim, ...
```

Override with `--force` (the dependent packages may break):

```bash
sudo tpkg remove zlib --force
```

## Diamond dependencies

If A depends on B and C, and both B and C depend on D, tpkg installs D only once (the first time it is resolved). The install order is:

```
D -> B -> C -> A
```

No duplicate installs, no conflicts.

## Conflicts with command names

If two packages provide the same command name (e.g. `ls` from `coreutils` and from `busybox`), tpkg warns:

```
warning: 'ls' is provided by coreutils; busybox's ls will be hidden
```

The last installed package wins the name. `tpkg which ls` shows the current owner. Use `--force` on install to overwrite silently.

## Real examples

### `gcc` dependency graph

```
gcc
  binutils
  make
  glibc-dev
glibc-dev (no further deps)
binutils (no further deps)
make (no further deps)
```

Install order: `glibc-dev`, `binutils`, `make`, `gcc`.

### `git` dependency graph

```
git
  curl
  zlib
  openssl
  ca-certificates
  busybox
```

Install order: `ca-certificates`, `zlib`, `openssl`, `busybox`, `curl`, `git`.

### `python3-tk` diamond

```
python3-tk
  python3
  tcltk
     xorg
        zlib
        openssl
        pcre2
```

Install order: `zlib`, `openssl`, `pcre2`, `xorg`, `tcltk`, `python3`, `python3-tk`.

## The package database

Installed packages are recorded in `/system/tpkg/installed/` (one file per package, containing the version string). `tpkg list` reads this directory. `tpkg list --deps` shows the dependency tree.

## Related

- [What happens when you install](/tos/tpkg-install) — the install pipeline.
- [Signatures](/tos/tpkg-signatures) — how signing works.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [How commands reach your PATH](/tos/tpkg-commands-path).
