# GNU tools and build tools: bash, perl, coreutils... cmake, autotools

## terminal | Programs (tpkg install ...)
- New packages for everyday work and for building software: bash, perl, coreutils, gawk, sed, grep (with -P), findutils, diffutils, patch, tar, gzip, less, file, wget (HTTPS), rsync, m4, autoconf, automake, libtool, bison, flex, pkgconf (pkg-config), cmake and ninja. Scripts that start with #!/bin/bash or #!/bin/sh, ./configure, make, cmake and ninja projects now build and run.
- Linux programs now find the commands of installed packages in /usr/bin and /bin; /bin/sh works as an interpreter.

## terminal | Fixed
- Redirections work as on Linux: `cmd > file` and `cmd > log 2>&1` no longer overwrite each other (dup() and fork() share the file's place), and the output of a shell comes back after `echo x > file`.
- Each program has its own working folder: a program that changes folder no longer moves the shell or other programs.
- Symbolic links: relative links and links in the middle of a path are followed correctly; lstat and rm see the link itself.
- Files carry their modification times (make, tar, rsync, gzip, find -newer work); utime, utimensat and futimens set them.
- A file deleted while open stays usable until it is closed; fchmod, fchdir, symlinkat, fallocate, creat, rt_sigsuspend, /dev/zero and /dev/fd/N (bash <(...)) work; timeout, sleep and sigsuspend based tools behave.
- Python's ssl can wrap a listening socket (servers); glibc-dev has the link script for -lgcc_s.
