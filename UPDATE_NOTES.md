# Editors, tmux, and a computer that talks to itself

## terminal | Programs (tpkg install ...)
- New packages: vim, nano, tmux (several shells in one SSH login, kept when you disconnect), htop, ripgrep (rg), jq, and terminfo (the terminal database they use). They work over SSH with colours and full screens.
- /proc exists: cpuinfo, meminfo, uptime, a folder for every program (status, exe, fd...). htop, ps-like tools and programs that look at their own machine find what they ask for.
- Programs can use sockets that live in a file (AF_UNIX): a server and a client in one computer, abstract names, a descriptor sent along, the other end's credentials. tmux and many tools need them.
- Timers: alarm, setitimer and POSIX timers wake a program and interrupt its waits (sleep, poll, pause).
- tty names the terminal of an SSH login; folders made by programs get the permissions they asked for (tmux wants private ones); closefrom/close_range, fcntl F_DUPFD and madvise work.

## settings | Network
- A computer can talk to itself: 127.0.0.1 (and its own address) works for TCP and UDP; a megabyte sent over it arrives intact. Connecting to a port nobody listens on fails at once with "connection refused".
