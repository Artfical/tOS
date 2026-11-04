# Graphical Linux programs: X11, tkinter, Python 3.9 to 3.14, signed packages

## terminal | Programs (tpkg install ...)
- Linux programs can have a window: the new xorg package brings an X server (tos-x starts it in a window of the desktop) and the X11 libraries, so X programs run with DISPLAY=:0. The pointer, the wheel and the keyboard reach them. tcltk brings Tcl/Tk 8.6.
- Python: python3.9, python3.10, python3.11, python3.12 and python3.13 beside python3 (3.14), every one with pip. The python-tk packages (python3-tk, python3.13-tk ...) add tkinter, so Python programs can open windows.
- tpkg checks a signature: every package is signed by the package server (ECDSA P-256) and tpkg refuses one whose signature does not fit; `tpkg info NAME` shows it, and `tpkg install NAME --unsigned` installs an unsigned package after a warning.
- Programs that wait with a time limit on a condition (pthread_cond_timedwait: Tk, many thread pools) no longer wait forever.
- Fixed: close(0) followed by open() gives 0 (a shell's background jobs), small files can be linked (lock files), folders asked for by full path, /dev/tty0 and /dev/console answer the virtual-terminal calls an X server makes.
- Programs get a framebuffer window of their own (/dev/fb0, with the pointer on /dev/input/event0 and the keys on event1): the base for X11, SDL and Qt.
