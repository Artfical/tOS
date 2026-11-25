/* What Wine asks of the kernel: a fault that reaches the program's own SIGSEGV handler (with the address, and the context it may change),
 * a GS base that stays through interrupts, a signal for one thread (tgkill), a bound socket that stat calls a socket, a read-only
 * shared mapping that sees what a writable one writes (even after the file is unlinked), and more than 64 open files. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>

#define CHECK(c, what) do { if (!(c)) { printf("wintest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("wintest: ok %s\n", what); } while (0)
static int fails;

static volatile int segv_seen;
static volatile void *segv_addr;
static volatile int segv_code, segv_trap;
static char altstack[65536];
static void on_segv(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    segv_seen = sig;
    segv_addr = si->si_addr;
    segv_code = si->si_code;
    segv_trap = (int)uc->uc_mcontext.gregs[REG_TRAPNO];
    uc->uc_mcontext.gregs[REG_RIP] += 2;                              /* as Wine does: the handler changes where the program goes on (past "movl (%rax), %ecx") */
}

static volatile pid_t usr1_tid;
static void on_usr1(int sig) { (void)sig; usr1_tid = (pid_t)syscall(SYS_gettid); }
static pid_t thread_tid;
static volatile int thread_stop;
static void *thread_main(void *arg)
{
    (void)arg;
    thread_tid = (pid_t)syscall(SYS_gettid);
    while (!thread_stop) { struct timespec ts = { 0, 5000000 }; nanosleep(&ts, 0); }
    return 0;
}

static unsigned long gs_buf[8];
int main(void)
{
    /* a page that is not there, reaching the handler (on an alternate stack) */
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    {
        __asm__ volatile("movl (%%rax), %%ecx" : : "a"((volatile int *)0x30) : "ecx");
    }
    CHECK(segv_seen == SIGSEGV && segv_addr == (void *)0x30 && segv_code == 1, "a NULL read reaches the SIGSEGV handler with its address");
    CHECK(segv_trap == 14, "the context says it was a page fault");

    /* the GS base, through a lot of interrupts */
    gs_buf[0] = 0x1122334455667788ul;
    int r = (int)syscall(SYS_arch_prctl, 0x1001, (unsigned long)gs_buf);
    CHECK(r == 0, "arch_prctl ARCH_SET_GS");
    unsigned long v = 0;
    for (int i = 0; i < 200; i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, 0); __asm__ volatile("movq %%gs:0, %0" : "=r"(v)); if (v != gs_buf[0]) break; }
    CHECK(v == gs_buf[0], "the GS base survives timer interrupts and sleeping");
    unsigned long g = 0;
    syscall(SYS_arch_prctl, 0x1004, (unsigned long)&g);
    CHECK(g == (unsigned long)gs_buf, "arch_prctl ARCH_GET_GS");

    /* a signal for one thread */
    struct sigaction su;
    memset(&su, 0, sizeof(su));
    su.sa_handler = on_usr1;
    sigaction(SIGUSR1, &su, 0);
    pthread_t th;
    pthread_create(&th, 0, thread_main, 0);
    while (!thread_tid) usleep(1000);
    syscall(SYS_tgkill, getpid(), thread_tid, SIGUSR1);
    for (int i = 0; i < 400 && !usr1_tid; i++) usleep(5000);
    CHECK(usr1_tid == thread_tid, "tgkill runs the handler in that thread");
    thread_stop = 1;
    pthread_join(th, 0);
    CHECK(syscall(SYS_tgkill, getpid(), 999999, SIGUSR1) == -1 && errno == ESRCH, "tgkill of a thread that is not there says ESRCH");

    /* a bound socket's file is a socket */
    const char *sp = "/tmp/wintest.sock";
    unlink(sp);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, sp);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 4) == 0, "bind and listen on a path");
    struct stat st;
    CHECK(stat(sp, &st) == 0 && S_ISSOCK(st.st_mode), "stat says the bound path is a socket");
    close(s);
    unlink(sp);

    /* a read-only shared mapping sees what a writable one writes, even when the file has been unlinked */
    const char *fp = "/tmp/wintest.map";
    int fd = open(fp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (ftruncate(fd, 4096)) return 2;
    volatile unsigned *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unlink(fp);
    volatile unsigned *ro = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(w != MAP_FAILED && ro != MAP_FAILED, "two shared mappings of an unlinked file");
    w[0] = 0xCAFE0001u;
    CHECK(ro[0] == 0xCAFE0001u, "the read-only mapping sees the writes");
    close(fd);

    /* many open files */
    int fds[300], n = 0;
    for (; n < 300; n++) { fds[n] = open("/dev/null", O_RDONLY); if (fds[n] < 0) break; }
    CHECK(n == 300, "300 files open at once");
    for (int i = 0; i < n; i++) close(fds[i]);

    printf(fails ? "wintest: %d FAILED\n" : "wintest: all passed\n", fails);
    return fails != 0;
}
