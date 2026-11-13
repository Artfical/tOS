title: Windowed programs (X11, Tk)
lede: Linux programs that draw their own windows run on tOS through an X server in a tOS window. This page shows how to start it, what works, and how to write a window program with Tk, tkinter or Xlib.
---
## How it works

tOS has an X server, **Xfbdev**, in the `xorg` package. `tos-x` opens a tOS window, starts the X server inside it and runs the program you give it there. When the program ends, the window closes. The pointer, the mouse wheel and the keyboard reach the program; the window is the screen the X program sees.

```
sudo tpkg install xorg
tos-x <program> [arguments]
```

The `xorg` package brings `Xfbdev`, `tos-x` and `xkbcomp`, and the X11 client libraries.

## Tk and tkinter

Tk (the toolkit behind Tcl/Tk and Python's tkinter) is the quickest way to make a window.

```
sudo tpkg install xorg tcltk python3-tk
```

A tkinter program (`hello_tk.py`):

```
import tkinter as tk

root = tk.Tk()
root.title("tkinter on tOS")
tk.Label(root, text="Hello from tkinter on tOS", font=("fixed", 14)).pack(pady=10)
tk.Button(root, text="Click me", command=lambda: print("clicked")).pack()
entry = tk.Entry(root)
entry.pack(pady=6)
canvas = tk.Canvas(root, width=300, height=100, bg="white")
canvas.create_rectangle(10, 10, 140, 90, fill="#2040c0")
canvas.create_oval(160, 10, 290, 90, fill="#c02020")
canvas.pack()
root.mainloop()
```

```
tos-x python3 hello_tk.py
```

Buttons, entries, canvases, lists, `ttk` widgets and timers (`after`) work, and the pointer and keyboard reach X programs. For other Python versions use `python3.13-tk` and run `tos-x python3.13 ...`.

`tclsh` and `wish` (from `tcltk`) are installed too, but they currently stop with "can not find channel named stdout"; use Tk through Python until that is fixed.

## A Tcl/Tk script

The same window in Tcl (for when `wish` works):

```
label .l -text "Hello from Tk on tOS" -font fixed
button .b -text "Click me" -command {puts clicked}
pack .l .b -padx 20 -pady 10
```

Python's tkinter runs the same Tk, so use the Python form for now.

## The screen

The X screen is the size of the tOS window (800x560 by default); the X program sees that as its screen.

## Other X programs

Any Linux X11 client built for x86-64 and glibc runs the same way: `tos-x <client>`. Programs that need extensions such as GLX or XRender may not. Programs built with [SDL 2](/tos/dev-sdl) and [Qt](/tos/dev-qt) have their own packages (`sdl2`, `qt6-base`). GTK and xterm are not in the package list yet.

## Framebuffer programs

A program that draws to the Linux framebuffer can open `/dev/fb0` and gets a tOS window of its own, with input events from `/dev/input/event0` (pointer) and `/dev/input/event1` (keyboard), as an X server does. This is the base that Xfbdev uses; Qt's `linuxfb` platform draws there too. `/dev/dri/card0` gives KMS programs the same window: see [DRM and KMS](/tos/dev-drm).

## Related

- [SDL 2](/tos/dev-sdl) and [Qt](/tos/dev-qt).
- [Python](/tos/dev-python) and [Linux programs](/tos/linux-programs).
- [Troubleshooting](/tos/troubleshooting) if the window does not open.
