# Python, resizable terminals, job control, and a computer that talks to itself

## terminal | Programs (tpkg install ...)
- New packages: python3 (3.14 with pip: ssl, sqlite3, zlib, bz2, lzma, ctypes, asyncio, multiprocessing, curses and the rest of the standard library), vim, nano, tmux, htop, ripgrep, jq and terminfo. pip install fetches packages from PyPI over HTTPS.
- The Terminal window can be resized and maximized: programs see the real number of rows and columns and are told when it changes (SIGWINCH), so vim, tmux and htop fill the window.
- Programs that ask for the mouse (vim, tmux, htop...) get clicks, drags and the wheel in the Terminal.
- Job control: Ctrl-Z stops a program, fg / bg / jobs bring it back (busybox sh with `set -m`); a background program that wants input stops until it is in front.
- Sockets that live in a file (AF_UNIX) and socketpair work like Linux's; shared memory and POSIX semaphores (/dev/shm, MAP_SHARED) are shared between programs and their children.
- /proc exists (cpuinfo, meminfo, uptime, a folder for every program), and timers (alarm, setitimer, POSIX timers) wake programs and interrupt their waits.
- Fixed: rmdir with a full path, the *at calls with a directory descriptor (rm -r, shutil.rmtree), accept in a program with threads, a 64-bit program is told it runs on x86_64, and offsets past 2 GB fail cleanly.

## settings | Network
- A computer can talk to itself: 127.0.0.1 works for TCP and UDP; connecting to a port nobody listens on fails at once with "connection refused".
- Sending is faster (several segments at a time) and a half-closed connection (shutdown) now tells the other side.
