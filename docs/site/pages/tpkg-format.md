title: The package format
lede: Inside a .tpkg: the archive layout, control files, license/NOTICE/source files, the meta.json the server serves, and limits.
---
## What a .tpkg is

A `.tpkg` file is a **gzipped tar archive** (ustar format) with a fixed internal layout. It is created by `tools/tos-update/pkg/make_pkg.py` from a staged directory.

## Internal layout

```
<name>-<version>.tpkg
├── bin/
│   └── <commands or symlinks>
├── lib/
│   └── *.so
├── include/
│   └── *.h
├── share/
│   └── man, doc, locale (stripped in our builds)
├── lib/pkgconfig/
│   └── *.pc
├── tpkg.commands      # name=relative/path (one per line)
├── tpkg.depends       # dependency names, one per line
├── tpkg.message       # shown after install (optional)
├── tpkg.services      # background services (rare)
├── LICENSE            # full licence text
├── NOTICE.txt         # attribution text
├── <source>.tar.gz    # source tarball (GPL source obligation)
└── meta.json          # served by the server (see below)
```

**No symlinks** are stored in the archive; the tar contains regular files only. The unpacker (`make_pkg.py` / tpkg) extracts with `tar -xzf`, which creates regular files.

## Control files

| File | Format | Required? |
|---|---|---|
| `tpkg.commands` | `name=relative/path` (one per line) | Yes |
| `tpkg.depends` | One dependency name per line | No |
| `tpkg.message` | Free text, shown after install | No |
| `tpkg.services` | `name|command|description` (one per line) | No |

Example `tpkg.commands` for `vim`:

```
vim=bin/vim
vi=bin/vi
view=bin/view
vimdiff=bin/vimdiff
xxd=bin/xxd
```

Example `tpkg.depends` for `python3-tk`:

```
python3
tcltk
```

## Licence and notice files

- `LICENSE`: the full licence text (e.g. GPL-3.0 text).
- `NOTICE.txt`: attribution, build notes, any local changes.

These are copied into the package directory and are visible with `tpkg info <name>`.

## Source tarball

For packages under licences that require source distribution (GPL, LGPL, AGPL, MPL, etc.), the original source tarball is placed beside the `.tpkg` on the server and listed in the package's `meta.json`. The filename is the original tarball name (e.g. `vim-9.2.1254.tar.gz`).

## meta.json (served by the server)

The server serves a `meta.json` for each package:

```json
{
  "name": "vim",
  "version": "9.2.1254",
  "license": "Vim",
  "homepage": "https://www.vim.org",
  "desc": "Vi IMproved editor",
  "file": "vim-9.2.1254.tpkg",
  "size": 1234567,
  "sha256": "a1b2c3d4...",
  "unpacked": 3456789,
  "min_build": 619,
  "depends": ["ncurses", "terminfo"],
  "commands": {
    "vim": "bin/vim",
    "vi": "bin/vi",
    "view": "bin/view",
    "vimdiff": "bin/vimdiff",
    "xxd": "bin/xxd"
  },
  "licenses": [
    ["LICENSE", "LICENSE"],
    ["NOTICE.txt", "NOTICE.txt"]
  ],
  "sources": [
    ["vim-9.2.1254.tar.gz", "vim-9.2.1254.tar.gz"]
  ],
  "notice": "Vim is a highly configurable text editor...\n",
  "message": "vim installed: run `vim` or `vi` to edit files.\n"
}
```

| Field | Meaning |
|---|---|
| `min_build` | Oldest tOS build that can install this package |
| `depends` | Runtime dependencies (from `tpkg.depends`) |
| `commands` | Map of command name -> relative path |
| `licenses` | Array of `[source_path, dest_name]` |
| `sources` | Array of `[source_path, dest_name]` for source tarballs |
| `notice` | The NOTICE text |
| `message` | The `tpkg.message` text |

## Limits

| Limit | Value |
|---|---|
| Max path length in archive | 255 bytes (ustar limit) |
| Symlinks | Not allowed (must be regular files) |
| File size | Limited by available RAM (archive is streamed) |
| `tpkg.commands` entries | No hard limit, but kept small |
| `tpkg.depends` lines | No hard limit |

## Related

- [What happens when you install](/tos/tpkg-install) — the install pipeline.
- [Signatures](/tos/tpkg-signatures) — how signing works.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
- [How commands reach your PATH](/tos/tpkg-commands-path).
