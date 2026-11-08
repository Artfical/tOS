title: Developing on tOS
lede: tOS can build software on itself. Compilers, build tools, languages, editors and version control are packages you install; this page is the map, and the pages after it go into each.
---
## What you can do

| You want to... | Install | Read |
|---|---|---|
| Compile C and C++ | `gcc` | [C and C++](/tos/dev-c) |
| Build a project with make, CMake or the autotools | `make`, `cmake`, `ninja`, `autoconf`, `automake`, `libtool` | [Build systems](/tos/dev-build) |
| Write Python | `python3` | [Python](/tos/dev-python) |
| Write JavaScript | `node24` (or `node22`, `node`) | [Node.js](/tos/dev-node) |
| Debug a program | `gdb`, `strace` | [Debugging](/tos/dev-debug) |
| Write Go | `go` | [Go](/tos/dev-go) |
| Write Rust | `rust` | [Rust](/tos/dev-rust) |
| Use Git, and connect with SSH | `git`, `openssh` | [Git and SSH](/tos/dev-git) |
| Make a program with a window | `xorg`, and `tcltk` or `python3-tk` | [Windowed programs](/tos/dev-x11) |
| Write shell scripts | `bash`, `busybox`, `perl`, `gawk`, `sed` | [Shell scripting](/tos/dev-shell) |
| Script the desktop | Nothing: MicroPython and T# are built in | [MicroPython and T#](/tos/scripting) |
| Work on tOS itself | `gcc`, `make`, `git` on the host | [Building tOS from source](/tos/build-tos) |

## A first setup

A good set to start with:

```
sudo tpkg install gcc make cmake ninja git vim python3
```

and, if you build software that uses the autotools or Perl:

```
sudo tpkg install autoconf automake libtool bison flex pkgconf perl
```

tpkg installs what each package needs. You can check what is installed with `tpkg list`, and remove what you do not use with `sudo tpkg remove <name>`.

## Editors

| Editor | Package | Notes |
|---|---|---|
| **Notepad** | built in | A window with line numbers and syntax highlighting. See [Notepad, Paint and viewers](/tos/productivity) |
| **edit** | built in | A simple line editor in the Terminal |
| **vim** | `vim` | Also brings `vi`, `view`, `vimdiff` and `xxd` |
| **nano** | `nano` | A friendlier terminal editor |

## Tools you will want

- `tmux` for several terminals in one, `htop` for the processes, `ripgrep` (`rg`) for fast search, `jq` for JSON.
- `curl`, GNU `wget` and `rsync` for moving data.
- `less`, `file`, `diff` and `patch`, and the other GNU tools.

See the [package list](/tos/package-list).

## Where your programs run

A program you build runs like any other Linux program on tOS: it gets its own address space, terminal and file table. Build it, then run it by path (`./myprog`). Libraries that your programs link against are found through the `-dev` packages: see [C and C++](/tos/dev-c).

## Memory

The root file system and everything in it is in RAM, so a big build that makes many files needs memory. Give the virtual machine 2 GB or more (`-m 2048`), and 4 GB for large builds.

## Related

- [Linux programs](/tos/linux-programs): what runs, and how.
- [Packages](/tos/packages): how `tpkg` works.
- [Troubleshooting](/tos/troubleshooting): when a build fails.
