title: Packages (tpkg)
lede: tpkg installs software from the tOS package server. Packages are signed, and the ones they depend on come with them.
---
## The commands

```
tpkg look                 # the packages on the server for your build
tpkg search text          # packages whose name or description has the text
tpkg info <name>          # what a package is, its version, licence, signature and commands
tpkg install <name>       # install it (and what it needs)
tpkg list                 # what is installed
tpkg which <command>      # the package a command comes from
tpkg remove <name>        # uninstall (not while another package needs it)
tpkg update               # refresh the list from the server
tpkg upgrade              # install newer versions of what is installed
```

Looking things up works for everyone. Anything that changes the system (`install`, `remove`, `upgrade`) needs root: use `sudo`.

## What a package is

A package is a compressed archive of files with a small description. It is unpacked into its own folder, `/system/pkg/<name>`, and the commands it brings are put on your PATH, so you can type them by name. The package server is `update.artfical.com`.

Packages are built for the C library tOS carries (glibc 2.35), so they run without anything else installed. Packages that are programs do not depend on the compiler; the development packages are separate:

- `gcc`, `binutils`, `make` and `glibc-dev` (the C library headers) build programs.
- Library headers and link files are in their own packages, such as `zlib-dev` and `openssl-dev`. They depend on `gcc`; the matching runtime packages (`zlib`, `openssl`) do not.

## Dependencies

tpkg installs what a package needs first, and refuses to remove a package that another one needs. `tpkg info <name>` shows the dependencies.

## Signatures

Every package is signed by the package server (ECDSA with P-256 over a SHA-256 of the package's name, version, size and hash). tOS has the matching public key built in, and `tpkg install` checks the signature before anything is unpacked. A package whose signature does not fit is refused.

```
sudo tpkg install somepackage --unsigned
```

installs a package without a valid signature after a warning. Only use that for a package you built yourself and trust.

## Builds and versions

The server lists a package only when your tOS build can run it. If `tpkg look` does not show something you read about here, update tOS first (Software Update), then run `tpkg update`.

## Source code and licences

Each package page on the server keeps the licence texts and a NOTICE next to the package. For free software with a source-code obligation, the source tarball it was built from is hosted next to the package.

See the [package list](/tos/package-list) for everything that is available.
