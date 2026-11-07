title: Packages (tpkg)
lede: tpkg installs software from the tOS package server. Packages are signed, and the ones they depend on come with them. This page explains how it works and how to use it well.
---
## The commands

```
tpkg look                 # the packages on the server for your build
tpkg search <text>        # packages whose name or description has the text
tpkg info <name>          # what a package is, its version, licence, signature and commands
tpkg install <name>       # install it (and what it needs)
tpkg list                 # what is installed
tpkg which <command>      # the package a command comes from
tpkg remove <name>        # uninstall (not while another package needs it)
tpkg update               # refresh the list from the server
tpkg upgrade              # install newer versions of what is installed
```

Looking things up (`look`, `search`, `info`, `list`, `which`) works for everyone. Anything that changes the system (`install`, `remove`, `upgrade`) needs root: use `sudo`.

## A first example

```
tpkg search editor
tpkg info vim
sudo tpkg install vim
vim hello.txt
```

`tpkg info vim` shows what the package is, its version, its licence, its signature status, what it needs and the commands it brings.

## What a package is

A package is a compressed archive of files with a few small description files. When you install it:

1. tpkg **downloads** it from the package server (`update.artfical.com`).
2. It **checks the signature** (see below).
3. It installs the **packages it depends on** first.
4. It **unpacks** the files into the package's own folder, `/system/pkg/<name>`.
5. It puts the **commands** the package brings on your PATH, so you can type them by name.
6. It shows the package's **message**, if it has one (a hint on how to start it).

Everything a package needs is in its own folder: you can look at it with `ls /system/pkg/<name>`. Removing a package deletes that folder and its commands.

## Commands from packages

A package says which commands it brings and what they run. Some are one program; some are a name for a program with an argument. For example, `busybox` brings `sh`, `ls`, `cp` and more as names for `busybox sh`, `busybox ls`, and so on; `coreutils` brings its tools as names for one program with the tool's name.

Commands are found in two ways:

- By **name** in the Terminal: type `git`.
- By **path** that Linux programs and scripts use: `/usr/bin/git` and `/bin/git` work too, so scripts that start with `#!/bin/sh`, `#!/bin/bash` or `#!/usr/bin/env python3` run.

If two packages bring the same command, tpkg warns you before it takes the name over.

## Dependencies

tpkg installs what a package needs first and refuses to remove a package that another one needs. `tpkg info <name>` shows the dependencies. To remove a package that others need anyway, run `sudo tpkg remove <name> --force` (those others may stop working).

The packages are arranged so that **programs do not depend on the compiler**. The development packages are separate:

- `gcc`, `binutils`, `make` and `glibc-dev` (the C library headers) build programs.
- Library headers and link files are in packages named `-dev` (`zlib-dev`, `openssl-dev`, `liblzma-dev`, ...). They need `gcc`. The matching runtime packages (`zlib`, `openssl`, ...) do not.

## Signatures

Every package is signed by the package server with an **ECDSA** key (curve P-256, over a SHA-256 of the package's name, version, size and hash). tOS has the matching public key built in, and `tpkg install` checks the signature **before anything is unpacked**. A package whose signature does not fit, or that has no signature, is refused.

```
sudo tpkg install somepackage --unsigned
```

installs a package without a valid signature, after a warning. Use it only for a package you built yourself and trust. See [Security](/tos/security).

## Builds and versions

The server lists a package only when your tOS build can run it: each package says the oldest build it needs. If `tpkg look` does not show something you read about here, update tOS first (Software Update), then run `sudo tpkg update`.

New versions of a package are published as the software is updated. `sudo tpkg upgrade` installs the newer versions of everything you have installed.

## Where things are

| What | Where |
|---|---|
| A package's files | `/system/pkg/<name>` |
| What is installed | The package database in `/system` |
| Which commands go where | A table of command names in `/system`, which the shell and the Linux layer both read |

## Source code and licences

Each package on the server has its licence texts and a NOTICE next to it. For free software that requires it, the **source tarball** that the package was built from is hosted next to the package, so you can get the source of everything you install. Packages built from sources are built without changes, except where the NOTICE says otherwise.

## Making your own package

A package is a `.tpkg` file: a compressed archive of the files, with `tpkg.commands` (the command names), `tpkg.depends` (the dependencies) and an optional `tpkg.message`. The package server signs the packages it publishes; a package you make yourself is installed with `--unsigned`.

## Related

- [Package list](/tos/package-list): everything that is available.
- [Developing on tOS](/tos/developing): the compilers and tools.
- [Linux programs](/tos/linux-programs): what runs.
