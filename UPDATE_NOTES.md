# Faster programs that wait, and System V shared memory

## terminal | New
- System V shared memory: `shmget`, `shmat`, `shmdt` and `shmctl`. Programs can now share a block of memory by number (the way the X server and its clients share pictures with MIT-SHM).

## terminal | Changed
- A program that waits for another (an X client for its X server, a build tool for its helper) is no longer held up for 10 ms each time: the idle task only runs when nothing else can. A round trip to the X server went from 25 ms to about 1 ms.
- Waiting programs read the network card at most once a millisecond (they did it thousands of times a second, which a virtual machine pays for dearly).
- The desktop leaves room for the programs in its windows: a frame that takes long is followed by a longer pause.

## terminal | Developers
- `tools/gtk-build.sh`, `tools/gtk-pack.py` and `tools/gtk-checklibs.py` build GTK 3 (with GLib, Cairo, Pango, HarfBuzz and the X11 libraries under it) for tOS; `tools/linux-tests/gtkdemo.c`, `xrt.c` and `xtree.c` try it. GTK itself does not draw its window yet, so the packages are not published.
