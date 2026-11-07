title: What happens when you install
lede: The install pipeline in detail: resolution, download, verification, unpacking, command registration and what can go wrong at each step.
---
## The install pipeline

```
sudo tpkg install <name>
```

runs these steps in order:

```
1. resolve(name)          -> package version, min_build, dependencies
2. download(.tpkg)        -> from update.artfical.com/pkg/<name>/
3. verify_signature()     -> ECDSA P-256 over SHA-256
4. unpack()               -> into /system/pkg/<name>/
5. register_commands()    -> writes /system/path.tmbl
6. show_message()         -> prints tpkg.message if present
```

Each step can fail; tpkg stops at the first error and rolls back nothing (the package is simply not added).

---

## Step by step

### 1. Resolution

- tpkg reads the local index (from `tpkg update` or the built-in one).
- It finds the package entry for your build (`min_build <= current_build`).
- If multiple versions exist, it picks the newest.
- If the package declares `depends:`, tpkg recursively resolves each one **before** downloading the package.

Example dependency chain for `python3-tk`:

```
python3-tk
  -> python3 (3.14.10)
  -> tcltk
     -> xorg
        -> zlib, openssl, pcre2
```

### 2. Download

Each package is a `.tpkg` file (gzipped tar) hosted at:

```
https://update.artfical.com/pkg/<name>/<name>-<version>.tpkg
```

tpkg streams the download to a temporary file in `/tmp/tpkg-XXXXXX`. If the connection breaks, the partial file is removed and the install fails with `download failed`.

### 3. Signature verification

**This is the security gate.** tpkg computes the SHA-256 of the downloaded file, builds the message:

```
tOS-tpkg-v1|<name>|<version>|<size>|<sha256>
```

and verifies the ECDSA P-256 signature against the public key compiled into the kernel (`cmd_tpkg.c`). If the signature is missing, malformed, or does not verify, tpkg aborts with:

```
signature verification failed (ECDSA P-256)
```

Use `--unsigned` to bypass (for packages you built yourself):

```bash
sudo tpkg install mypackage --unsigned
```

**Warning:** `--unsigned` disables the only guarantee that the package is unmodified and from the official server.

### 4. Unpacking

The `.tpkg` is a gzipped tar (ustar format) containing:

```
/system/pkg/<name>/
  bin/           -> commands (or symlinks to the main binary)
  lib/           -> shared libraries
  include/       -> headers (for -dev packages)
  share/         -> man, doc, locale (stripped in our builds)
  lib/pkgconfig/ -> .pc files for pkg-config
  tpkg.commands  -> command name = relative path
  tpkg.depends   -> one dependency per line
  tpkg.message   -> shown after install
  tpkg.services  -> background services (rare)
  LICENSE        -> licence text
  NOTICE.txt     -> attribution
  <source>.tar.gz|.tar.xz  -> hosted beside the package (GPL source obligation)
```

The archive is extracted with `tar -xzf` into `/system/pkg/<name>/`. Existing files are overwritten. Symlinks inside the archive are followed (the tar contains regular files only, no symlinks).

### 5. Command registration

tpkg reads `tpkg.commands` (format `name=relative/path`) and appends to `/system/path.tmbl`:

```
vim=/system/pkg/vim/bin/vim
vi=/system/pkg/vim/bin/vi
```

The shell reads this file on startup and when you run `tpkg` commands, so new commands are available immediately in new Terminal windows. In the current shell, run `hash -r` or start a new Terminal.

### 6. Message

If `tpkg.message` exists, tpkg prints it after a successful install. Example for `vim`:

```
vim installed: run `vim` or `vi` to edit files.
```

---

## Failure modes

| Step | Error | What to do |
|---|---|---|
| Resolution | `package not found` | `tpkg update` then retry; maybe the build is too old |
| Resolution | `min_build 619 > current 618` | Update tOS with Software Update |
| Dependency | `dependency foo not available` | The dep isn't built for your build; update tOS |
| Download | `download failed` | Check network, try again |
| Signature | `signature verification failed` | File corrupted or not from our server; report it |
| Unpack | `disk full` | `df -h` and `tpkg remove` unused packages |
| Register | `command conflict: ls` | tpkg warns; use `--force` to overwrite |

## Rollback

tpkg does **not** automatically roll back a partially installed package. If step 4 fails, the partial directory in `/system/pkg/` remains. Remove it manually:

```bash
rm -rf /system/pkg/<name>
```

Then fix the cause and retry.

## The database

Installed packages are tracked in `/system/tpkg/installed/` (one file per package, containing the version). `tpkg list` reads this directory. `tpkg remove` deletes the entry.

## Related

- [tpkg commands](/tos/tpkg-commands) — all subcommands.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
- [Signatures](/tos/tpkg-signatures) — how signing works.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
