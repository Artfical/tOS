title: Making your own package
lede: The spec fields make_pkg.py takes, a worked example of staging files, writing tpkg.commands/depends, building, installing it with --unsigned on tOS, testing, and why it is not signed by the server.
---
## The spec file

A package starts with a JSON spec. Example for `hello` (a simple C program):

```json
{
  "name": "hello",
  "version": "1.0",
  "license": "MIT",
  "homepage": "https://example.com/hello",
  "desc": "A tiny hello world program",
  "min_build": 619,
  "stage": "/home/user/hello-stage",
  "depends": [],
  "commands": {
    "hello": "bin/hello"
  },
  "licenses": [
    ["LICENSE.txt", "LICENSE"]
  ],
  "sources": [
    ["hello-1.0.tar.gz", "hello-1.0.tar.gz"]
  ],
  "notice": "Hello 1.0 for tOS\n==================\n\nBuilt from hello-1.0.tar.gz (beside this package).\nLicense: MIT (LICENSE).\n",
  "message": "hello installed: run `hello` to say hello.\n"
}
```

Save as `spec-hello.json`.

## The stage directory

Create the staging tree exactly as it should appear under `/system/pkg/hello/`:

```
hello-stage/
├── bin/
│   └── hello          (executable, built for x86-64 tOS)
├── LICENSE.txt
├── hello-1.0.tar.gz   (source tarball, beside this package)
```

The `hello` binary must be a statically linked or dynamically linked (against tOS's glibc 2.35) x86-64 ELF. Build it on a Linux host with `gcc -static` or link against the tOS toolchain.

## Building the package

```bash
cd /home/user
python3 /root/tos-update/pkg/make_pkg.py spec-hello.json
```

This produces in `/root/tos-update/pkg/hello/`:

```
hello-1.0.tpkg
hello-1.0.tar.gz
LICENSE
NOTICE.txt
meta.json
```

## Installing on tOS (unsigned)

Copy the `.tpkg` to tOS (or serve it from a local HTTP server) and install:

```bash
sudo tpkg install hello --unsigned
```

You will see:

```
WARNING: installing unsigned package!
hello installed: run `hello` to say hello.
```

Then run it:

```bash
hello
# hello from tOS
```

## Testing

Run the program in various ways:

- From the Terminal: `hello`
- With arguments: `hello --help`
- From a script: `./myscript.sh` that calls `hello`
- Check the command table: `tpkg which hello`

Check that dependencies work (if any) by removing a dependency and confirming it fails.

## Why it is not signed by the server

The server only signs packages that are part of the official tOS repository. Your package is **yours**; you distribute it yourself (via a file share, a local HTTP server, or by copying the `.tpkg` to the target machine).

The `--unsigned` flag exists precisely for this: it lets you install packages that the server does not know about. The warning reminds you that the package has not been vetted by the tOS team.

## Spec field reference

| Field | Required? | Description |
|---|---|---|
| `name` | Yes | Package name (lowercase, alphanumeric, hyphens) |
| `version` | Yes | Upstream version + optional tOS revision |
| `license` | Yes | SPDX identifier or name |
| `homepage` | No | Upstream URL |
| `desc` | Yes | One-line description |
| `min_build` | Yes | Oldest tOS build that can install this |
| `stage` | Yes | Path to the staged directory |
| `depends` | No | Array of package names |
| `commands` | Yes | `{ "cmd": "relative/path" }` |
| `licenses` | Yes | `[ [source, dest], ... ]` |
| `sources` | No | `[ [source, dest], ... ]` for source tarballs |
| `notice` | Yes | Free text for NOTICE.txt |
| `message` | No | Shown after install |
| `services` | No | Array of `name|command|desc` |

## Tips

- Test on a clean tOS (live environment) before distributing.
- Keep the stage directory clean: no `.git`, no build artefacts.
- Use `strip` on binaries to reduce size.
- The source tarball must be the exact upstream release (or a tagged Git archive).

## Related

- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
- [Signatures](/tos/tpkg-signatures) — how signing works.
