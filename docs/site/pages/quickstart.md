title: Quickstart
lede: From the first start to compiling and running your own program, step by step.
---
This page assumes you have tOS running: see [Installation](/tos/installation) if you have not. Everything below also works in the live environment, where the user is `tos` and there is no password.

## 1. Find your way around

When the desktop opens you see a top bar (the clock, a menu with the name of the app you are in), a dock at the bottom with the apps, and the desktop itself. Click the first icon in the dock, the **Launcher**, to see all the apps.

![The Launcher: Files, Terminal, tFire, Notepad, Calculator, Paint and more](shot:launcher.png)

Click an app to open it, or click anywhere else to close the launcher. The apps in the dock show a small dot when they are running (you can turn this off in [Settings](/tos/settings)). Windows have the three round buttons at the top left: close, minimise and zoom.

## 2. Open the Terminal

Click **Terminal** in the dock. It opens a window with a prompt. Try these:

```
ls
pwd
hostname
whoami
version
man
```

`ls` lists the files, `pwd` shows where you are, `version` shows the tOS build and `man` lists the manual pages. `help` shows the commands in short.

![The Terminal](shot:terminal.png)

The Terminal completes names when you press Tab and remembers your commands: the up and down keys walk through them.

## 3. Look at the packages

tOS keeps its software in packages on a server. Look at what there is:

```
tpkg look
tpkg search python
tpkg info gcc
```

`tpkg look` lists the packages the server has for your build, `tpkg search` finds packages by name or description, and `tpkg info` shows a package's version, licence, size, signature and the commands it brings.

## 4. Install a compiler and build a program

Installing changes the system, so it needs `sudo`:

```
sudo tpkg install gcc
```

tpkg installs gcc and everything it needs (the C library headers, the assembler and linker, `make`). It checks each package's signature first.

Now write a program. Open **Notepad** from the launcher, type this, and save it as `hello.c` in your home folder:

```
#include <stdio.h>
int main(void) { puts("hello from tOS"); return 0; }
```

(Or use the Terminal's `edit hello.c`.) Then build and run it:

```
gcc hello.c -o hello
./hello
```

You should see `hello from tOS`.

## 5. Install a language

```
sudo tpkg install python3
python3 -c "print(6 * 7)"
```

`python3` is Python 3.14 with `pip`. Other versions (3.9 to 3.13) are separate packages. Node.js, Perl, Tcl, and a lot more are packages too: see the [package list](/tos/package-list).

## 6. Browse the web

Open **tFire** from the dock. Type an address in its bar, or words to search the web.

![tFire](shot:tfire/home.png)

## 7. Try a windowed Linux program

Windowed Linux programs need the X server and a toolkit:

```
sudo tpkg install xorg tcltk python3-tk
tos-x python3 -m tkinter
```

`tos-x` opens a window, starts the X server in it and runs the program you give it. A tkinter test window opens inside it. See [Windowed programs](/tos/dev-x11).

## 8. Customise the desktop

Open **Settings** from the launcher. Switch between light and dark, pick an accent colour and a wallpaper, change the screen size.

![Settings: Appearance](shot:settings.png)

## 9. Keep it up to date

Software Update in Settings installs newer tOS builds. For packages, run `sudo tpkg upgrade`.

## Where next

| If you want to... | Read |
|---|---|
| Learn the desktop | [The desktop](/tos/desktop), [The apps](/tos/apps) and [Files](/tos/files-app) |
| Learn the shell | [Terminal and shell](/tos/terminal) and [Shell commands](/tos/commands) |
| Write programs | [Developing on tOS](/tos/developing) |
| Run Linux programs | [Linux programs](/tos/linux-programs) |
| Fix a problem | [Troubleshooting and limits](/tos/troubleshooting) |
