title: Debugging: gdb and strace
lede: Debug programs on tOS itself. gdb stops a program at a breakpoint and shows its code, variables and registers; strace lists the system calls it makes. Both rest on tOS's ptrace support.
---
## Install

```
sudo tpkg install gdb strace
```

> **sudo:** `sudo tpkg install` (and `sudo tpkg remove`) change the system, so they need root. Debugging a program you started yourself needs no root. Attaching to a program that belongs to someone else does: use `sudo gdb -p PID`.

`gdb` is 17.2 (about 5 MB to download), `strace` is 7.3 (about 1 MB). Both are single static programs. `gdb` brings the `busybox` package, which its start-up wrapper uses.

## gdb: a first session

Compile with `-g` so that the program carries line numbers and variable names (and without optimisation, so that the code stays in the order you wrote it):

```
sudo tpkg install gcc gdb
cat > t.c <<'EOF'
#include <stdio.h>
int square(int n)
{
    return n * n;
}
int main(void)
{
    int total = 0;
    for (int i = 1; i <= 3; i++)
        total += square(i);
    printf("total=%d\n", total);
    return 0;
}
EOF
gcc -g -O0 t.c -o t
gdb ./t
```

At the `(gdb)` prompt:

| Command | What it does |
|---|---|
| `break square` | Stop whenever `square` is reached (also `break t.c:9`) |
| `run` | Start the program |
| `bt` | The call stack (backtrace) |
| `next`, `step` | Run one line, over or into a call |
| `finish` | Run until the current function returns, and show the value |
| `print total` | Show a variable or expression |
| `info registers rip` | Show a register |
| `continue` | Go on to the next breakpoint |
| `quit` | Leave (the program is ended) |

A session like that, run on tOS, stopped at `square (n=1) at t.c:4`, showed the stack (`#0 square`, `#1 main`), finished the call with `Value returned is $1 = 1`, stepped on, printed `$2 = 1`, and stopped again at the second call with `n=2`.

To run a script of commands without typing them: `gdb -q -batch -x commands.txt ./t`.

## What the gdb command does

`gdb` is a small script that starts the real program with two settings that suit tOS: its data folder is named by its full path, and the program under test is started directly (without a shell). The real program is `/system/pkg/gdb/bin/gdb`.

You will see one warning when it starts, `could not convert ... to UTF-32`: the static C library inside gdb has no character set conversion tables. It changes nothing you can see.

## strace: the system calls of a program

```
strace ls /tmp
strace -c ls /tmp
strace -o log.txt -f sh -c 'echo one; echo two'
```

- Each line is one system call, with its arguments and its result: `write(1, "hi\n", 3) = 3`.
- `-c` prints a table at the end: how many times each call was made, and how many failed.
- `-o file` writes the trace to a file instead of the screen.
- `-f` follows the programs that the traced program starts. Following a `fork` works (it was tested); following threads does not yet.

## How it works: ptrace

Both tools use the `ptrace` system call. The program being debugged (the tracee) stops inside the kernel, and the debugger (the tracer) is told through `wait`. The tracer can then read and change the tracee's registers and memory, and let it go on. tOS does this for 64-bit programs:

| What | How it is done |
|---|---|
| Breakpoints | The debugger writes an `int3` instruction into the program; reaching it stops the program with SIGTRAP |
| Single steps | The trap flag of the processor, so the program stops after every instruction |
| System calls | `PTRACE_SYSCALL` stops the program at the start and at the end of every call |
| Signals and faults | The tracer sees every signal first and decides whether the program gets it; a fault (a crash) stops the program with SIGSEGV and the tracer can look at it |
| Start | A program that starts under `PTRACE_TRACEME` stops after `exec`, before its first instruction |
| Information | `/proc/PID/maps`, `/proc/PID/auxv` and `/proc/PID/exe` are available |

The calls tOS supports: `PTRACE_TRACEME`, `ATTACH`, `DETACH`, `CONT`, `SINGLESTEP`, `SYSCALL`, `KILL`, `PEEKTEXT/DATA/USER`, `POKETEXT/DATA/USER`, `GETREGS`, `SETREGS`, `GETFPREGS`, `SETFPREGS`, `GETREGSET`, `SETREGSET` (general and floating-point registers), `GETSIGINFO`, `SETOPTIONS` (sysgood, exec event, fork and vfork events, exit-kill) and `GETEVENTMSG`.

## What is tested

These ran on tOS (in a QEMU virtual machine) and worked:

| Test | Result |
|---|---|
| A test program that traces another: stop after exec, registers, reading and writing memory | works |
| 10 single steps, a breakpoint (the program stops just after the `int3`), changing the instruction pointer | works |
| System call stops at the start and the end, with the number and the result | works |
| A signal stops a program that is only running in user mode; `PTRACE_KILL`; a crash stops with SIGSEGV and letting it through ends the program | works |
| `strace` on `busybox echo`, with the results of the calls, `strace -c`, and `strace -f` across a fork | works |
| `gdb`: `break`, `run`, `bt`, `info registers`, `finish`, `next`, `print`, `continue` | works |

## Limits

- **Threads:** a program with several threads is traced as one: you cannot stop or step the threads apart yet.
- **Hardware watchpoints** (`watch` that uses the debug registers) are not available; `gdb` can still check a variable by stepping, which is slow.
- **`PTRACE_SEIZE`** is refused, so tools that can fall back use `PTRACE_ATTACH` and `PTRACE_TRACEME` instead (strace and gdb do).
- **Attaching** (`gdb -p PID`, `strace -p PID`) is implemented but not tested yet.
- **Slow memory reads:** `/proc/PID/mem` and `process_vm_readv` are missing, so debuggers read the program's memory a word at a time. Big dumps are slow.
- A program that waits in a long system call (a sleep, a read) is stopped by a signal only when the call ends.
- `gdb` has no Python scripting and no TUI in this build.

## Related

- [C and C++](/tos/dev-c): the compiler, with `-g`.
- [Go](/tos/dev-go) and [Rust](/tos/dev-rust).
- [Packages](/tos/packages) and [Troubleshooting](/tos/troubleshooting).
