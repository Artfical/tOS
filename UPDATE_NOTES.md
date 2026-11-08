# Debugging: ptrace, gdb and strace; job control in the Terminal

## terminal | Programs (tpkg install ...)
- gdb 17.2 (`sudo tpkg install gdb`): breakpoints, stepping, backtraces, registers and memory of programs built with `gcc -g`.
- strace 7.3 (`sudo tpkg install strace`): the system calls of a program (`strace ls`, `strace -c`, `strace -f`).

## terminal | New
- ptrace for 64-bit programs: breakpoints (int3), single steps, stops at system calls, signals and crashes, reading and writing registers and memory, forks followed on request. /proc/PID/maps, /proc/PID/auxv and /proc/PID/exe.
- Job control in the Terminal app: Ctrl+Z stops a program, `jobs`, `fg` and `bg` bring it back or let it run. Ctrl+C now ends a program that never reads the keyboard (a sleep).

## terminal | Fixed
- personality() is kept (a debugger turns address randomization off and checks it); a forked child no longer starts as a traced program unless its tracer asked for forks.
