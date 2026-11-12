title: Licences and source code
lede: The LICENCE, NOTICE and source tarball beside each package, why, GPL obligations, how to find a package's source, how builds are made.
---
## What ships with every package

Every package directory on the server contains:

| File | Purpose |
|---|---|
| `LICENSE` | Full licence text (e.g. GPL-3.0) |
| `NOTICE.txt` | Attribution, build notes, local changes |
| `<name>-<version>.tar.gz` | Source tarball (GPL source obligation) |

These are listed in the package's `meta.json` under `licenses` and `sources`.

## Why

Free software licences (GPL, LGPL, AGPL, MPL, Apache-2.0) require that the **corresponding source code** be available to anyone who receives the binary. tOS complies by hosting the exact source tarball used to build the package, unmodified.

The `NOTICE.txt` documents:

- The upstream project and version.
- Any patches applied (with links to the patches).
- Build flags and configuration.
- The tOS-specific packaging changes.

## GPL obligations

If a package is under GPL-2.0, GPL-3.0, LGPL-2.1, LGPL-3.0, or AGPL-3.0, the source tarball **must** be offered alongside the binary. tOS does this automatically: the build script uploads the source tarball next to the `.tpkg`.

For LGPL libraries (e.g. `glibc`), the dynamic linking exception applies: you may link your own code without it becoming GPL, provided you use dynamic linking and allow the user to swap the library.

## Every package has its own repository on GitHub

Each package is also published as a repository of its own: **`github.com/Artfical/tpkg-<name>`** (for example [tpkg-gcc](https://github.com/Artfical/tpkg-gcc), [tpkg-qt6-base](https://github.com/Artfical/tpkg-qt6-base), [tpkg-sdl2](https://github.com/Artfical/tpkg-sdl2)).

- The repository holds the text: `README.md` (what it is, its licence, what it needs, the SHA-256 of the package), `meta.json`, the licence files and `NOTICE.txt` (how it was built, what was changed).
- Each version is a **release** (`v<version>`) that carries the `.tpkg` and the upstream source tarballs it was built from, so the corresponding source is always one click away.
- Signatures are not kept there: the package server signs a package when it hands it out, and the signing key is never published. `tpkg install` is the supported way to install and checks the signature; a `.tpkg` downloaded from GitHub has the hash from the README but not the server's signature.
- Problems with a package (a build, a missing file, a version) belong in that repository's issues; problems in the software itself belong upstream.

## How to find a package's source

On the server, in `/root/tos-update/pkg/<name>/`:

```
vim-9.2.1254.tpkg
vim-9.2.1254.tar.gz          <- source
LICENSE
NOTICE.txt
meta.json
```

The source tarball is the **exact upstream release tarball** (or a tag archive from the upstream Git repo), unmodified. The `NOTICE.txt` says if any patches were applied.

## How builds are made

Every package is built on a clean Linux host (Ubuntu 22.04) with:

- Prefix: `/system/pkg/<name>`
- Host compiler: host GCC (not the tOS cross-compiler).
- Dependencies: installed from the tOS package server (`tpkg install <dep>`).

The build script (`tools/tos-update/pkg/make_pkg.py`):

1. Creates a stage directory.
2. Runs `./configure --prefix=/system/pkg/<name> ...`.
3. Runs `make -jN` and `make install`.
4. Strips binaries, removes docs/man/locale.
5. Copies LICENSE, NOTICE.txt, source tarball into the stage.
6. Runs `make_pkg.py` to create the `.tpkg`.

The build machine is a standard Linux host; tOS is not self-hosting for package builds (yet).

## Verifying a build

You can verify a package's source matches its binary:

```bash
tpkg info <name>  # shows the sha256 of the .tpkg
# Download the source tarball from the server
# Build it yourself with the same flags (see NOTICE.txt)
# Compare the resulting binaries
```

## Related

- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [Making your own package](/tos/tpkg-creating) — how to package your own software.
- [Licence compliance](https://www.gnu.org/licenses/gpl-faq.html) — GPL FAQ.
