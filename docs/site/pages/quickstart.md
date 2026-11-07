title: Quickstart
lede: From the first boot to compiling and running your own program.
---
## 1. Start tOS

Start the image as shown in [Installation](/tos/installation). On a fresh machine, choose **Try tOS in Live Environment**, pick your keyboard layout, and the desktop opens. (Installing works the same way; the live environment is just quicker.)

## 2. Open the Terminal

Click **Terminal** in the dock. The prompt takes ordinary commands:

```
ls
pwd
hostname
version
man
```

![The Terminal](shot:terminal.png)

`help` lists the commands and `man` is the manual. [Shell commands](/tos/commands) has the full list.

## 3. Look at the packages

```
tpkg look
tpkg search python
tpkg info gcc
```

`tpkg look` lists what the package server has for your build. `tpkg info` shows a package's version, licence, size and the commands it brings.

## 4. Install a compiler

Installing changes the system, so it needs `sudo` (the live user `tos` has no password):

```
sudo tpkg install gcc
```

tpkg installs gcc and everything it needs. Then write a program (in Notepad, or with `edit hello.c`), save it as `hello.c`:

```
#include <stdio.h>
int main(void) { puts("hello from tOS"); return 0; }
```

and build and run it:

```
gcc hello.c -o hello
./hello
```

## 5. Install a language

```
sudo tpkg install python3
python3 -c "print(6 * 7)"
```

Python 3.14 comes with `pip`. Other Python versions (3.9 to 3.13) are separate packages, and so are Node.js, Git, vim, tmux and many more: see the [package list](/tos/package-list).

## 6. Try a window program

Windowed Linux programs need the X server and the toolkit packages:

```
sudo tpkg install xorg tcltk python3-tk
tos-x python3 -m tkinter
```

`tos-x` opens a window, starts the X server in it and runs the program you give it there. [Linux programs](/tos/linux-programs) explains more.

## Next

- [The desktop](/tos/desktop) and [Terminal and shell](/tos/terminal).
- [Developing on tOS](/tos/developing) for build tools and languages.
- [Troubleshooting](/tos/troubleshooting) if something does not work.
