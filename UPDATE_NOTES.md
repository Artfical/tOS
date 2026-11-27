# Groundwork for bigger Linux programs

## terminal | New
- Linux programs can now use `memfd_create` (anonymous shared memory), `timerfd` (one-shot and periodic timers that work with `poll` and `epoll`) and `inotify` (the descriptor works; it never reports a change). GTK, GLib and web browsers ask for these.
- `stat` finds `/dev/null`, `/dev/zero`, `/dev/urandom` and `/dev/random`, and `/proc/sys/kernel/random/uuid` gives a new identifier at every read. `prctl(PR_SET_PDEATHSIG)` is accepted.

## terminal | Developers
- `tools/linux-tests/ffprobe.c` checks what a browser asks of the kernel (threads, futexes, shared memory, events, `/proc`, files, sockets) and prints a line for each thing that fails. tOS passes 78 of its 86 checks; the rest are `signalfd` (not provided) and files that come with packages or the network.

## terminal | Documentation
- New page: Linux programs: what tOS provides.
