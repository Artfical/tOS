title: Linux programs: what tOS provides
lede: tOS runs ordinary 64-bit Linux programs. This page lists what the kernel gives them, and the test that checks it.
icon: terminal
---
## What a program can use

| Area | Provided |
|---|---|
| Threads | `clone` threads, `futex` (wait, wake, private), thread-local storage, `set_robust_list`, `prctl` names |
| Memory | `mmap` (anonymous, file, `MAP_SHARED`, reservations with `PROT_NONE`), `mprotect`, `mremap`, `madvise`, `munmap` |
| Shared memory | **`memfd_create`**: an anonymous file you can `ftruncate` and map in several processes, and pass to another process with `SCM_RIGHTS` |
| Events | `epoll`, `poll`, `eventfd`, **`timerfd`** (one-shot and periodic), **`inotify`** (the descriptor works; it never reports a change), `pipe2`, `socketpair` |
| Not provided | `signalfd` (answers "function not implemented") |
| Files | `/proc` (`self/maps`, `status`, `cpuinfo`, `meminfo`, `sys/kernel/random/uuid` ...), `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random` (also for `stat`), `flock`, `fcntl` locks, `fallocate` |
| Processes | `fork`, `execve`, `wait`, signals with handlers and alternate stacks, `prctl(PR_SET_PDEATHSIG)` (accepted, not acted on) |
| Network | TCP and UDP sockets, Unix sockets, `/etc/resolv.conf` and `/etc/hosts` written when the network comes up |

## Checking it

`tools/linux-tests/ffprobe.c` tries each of these, the things a big program such as a web browser asks of the kernel, and prints a line for each one that fails:

```
gcc -O2 -static -o ffprobe tools/linux-tests/ffprobe.c -lpthread
```

Put it in the initrd as `/programs/ffprobe` and run it in a Terminal. It ends with `RESULT: N passed, M failed`.

## Next

[Windowed programs](/tos/dev-x11) shows how programs that draw windows run, and [Debugging](/tos/dev-debug) how to find out what a program asked for.
