/* ptrace test (static, not PIE): ptrtest runs the tests; ptrtest run|spin|segv is the program they trace */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name) do { if (c) printf("ptrace: ok %s\n", name); else { printf("ptrace: FAIL %s\n", name); fails++; } } while (0)

__attribute__((noinline)) void bp_target(void) { __asm__ volatile("nop; nop; nop"); }

static int child(const char *mode)
{
    if (!strcmp(mode, "run")) {
        write(1, "hello from the tracee\n", 22);
        bp_target();
        getpid();
        return 7;
    }
    if (!strcmp(mode, "spin")) { for (;;) __asm__ volatile(""); }
    if (!strcmp(mode, "segv")) { volatile int *p = 0; *p = 1; return 0; }
    return 1;
}

static pid_t start(const char *self, const char *mode)
{
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execl(self, self, mode, (char *)0);
        _exit(99);
    }
    return pid;
}

int main(int argc, char **argv)
{
    if (argc > 1) return child(argv[1]);
    int st;
    pid_t pid = start(argv[0], "run");
    CHECK(waitpid(pid, &st, 0) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP, "stop after exec");
    struct user_regs_struct rg;
    CHECK(ptrace(PTRACE_GETREGS, pid, 0, &rg) == 0 && rg.rip != 0 && rg.rsp != 0, "getregs");
    long argc_in = ptrace(PTRACE_PEEKDATA, pid, (void *)rg.rsp, 0);
    CHECK(argc_in == 2, "peek argc from the stack");
    long saved = ptrace(PTRACE_PEEKDATA, pid, (void *)rg.rsp, 0);
    CHECK(ptrace(PTRACE_POKEDATA, pid, (void *)rg.rsp, (void *)0x1234) == 0 &&
          ptrace(PTRACE_PEEKDATA, pid, (void *)rg.rsp, 0) == 0x1234, "poke and peek");
    ptrace(PTRACE_POKEDATA, pid, (void *)rg.rsp, (void *)saved);

    /* single steps: the instruction pointer moves, each step stops with SIGTRAP */
    int steps = 0, moved = 0;
    unsigned long last = rg.rip;
    for (int i = 0; i < 10; i++) {
        if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) != 0) break;
        if (waitpid(pid, &st, 0) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTRAP) break;
        steps++;
        ptrace(PTRACE_GETREGS, pid, 0, &rg);
        if (rg.rip != last) moved++;
        last = rg.rip;
    }
    CHECK(steps == 10 && moved >= 9, "10 single steps");

    /* a breakpoint: int3 at bp_target */
    unsigned long addr = (unsigned long)bp_target;
    long orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)addr, 0);
    ptrace(PTRACE_POKETEXT, pid, (void *)addr, (void *)((orig & ~0xFFL) | 0xCC));
    ptrace(PTRACE_CONT, pid, 0, 0);
    int got = waitpid(pid, &st, 0) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP;
    ptrace(PTRACE_GETREGS, pid, 0, &rg);
    CHECK(got && rg.rip == addr + 1, "breakpoint hit (rip after int3)");
    ptrace(PTRACE_POKETEXT, pid, (void *)addr, (void *)orig);
    rg.rip = addr;
    CHECK(ptrace(PTRACE_SETREGS, pid, 0, &rg) == 0, "setregs (rip back)");

    /* system calls: stops at entry and exit, orig_rax is the number */
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD);
    int entries = 0, exits = 0, saw_getpid = 0, in = 0, getpid_nr = 0, ret_ok = 0, entry_enosys = 0;
    for (;;) {
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        if (waitpid(pid, &st, 0) != pid) break;
        if (WIFEXITED(st)) break;
        if (!WIFSTOPPED(st)) break;
        if (WSTOPSIG(st) == (SIGTRAP | 0x80)) {
            ptrace(PTRACE_GETREGS, pid, 0, &rg);
            if (!in) { entries++; if (rg.orig_rax == 39) { saw_getpid = 1; getpid_nr = 1; entry_enosys = (long)rg.rax == -38; } in = 1; }
            else { exits++; if (getpid_nr) { ret_ok = (long)rg.rax == (long)pid; getpid_nr = 0; } in = 0; }
        }
    }
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "the tracee exits with 7");
    CHECK(entries >= 1 && saw_getpid && (entries == exits || entries == exits + 1), "system call stops (getpid seen)");

    CHECK(entry_enosys, "rax is -ENOSYS at the entry stop");
    CHECK(ret_ok, "rax is the result (the pid) at the exit stop");

    /* a signal: the tracee spinning in user mode stops for it */
    pid = start(argv[0], "spin");
    waitpid(pid, &st, 0);
    ptrace(PTRACE_CONT, pid, 0, 0);
    usleep(200000);
    kill(pid, SIGUSR1);
    CHECK(waitpid(pid, &st, 0) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR1, "a signal stops the spinning tracee");
    ptrace(PTRACE_KILL, pid, 0, 0);
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "PTRACE_KILL");

    /* a fault: stops with SIGSEGV; letting it through ends the tracee */
    pid = start(argv[0], "segv");
    waitpid(pid, &st, 0);
    ptrace(PTRACE_CONT, pid, 0, 0);
    CHECK(waitpid(pid, &st, 0) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSEGV, "a fault stops with SIGSEGV");
    ptrace(PTRACE_CONT, pid, 0, (void *)SIGSEGV);
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "SIGSEGV let through ends it");

    printf("ptrace: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
