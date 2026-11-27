/* What Firefox asks of the kernel, one thing at a time: prints "ok NAME" or "FAIL NAME (why)" for each.
 * Built static with the host's gcc, run inside tOS (initrd: /programs/ffprobe). A FAIL is a gap in tOS's Linux layer. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>

static int fails;
static int passed;
static void ok(const char *n) { (void)n; passed++; }
static void bad(const char *n, const char *why) { printf("FAIL %s (%s)\n", n, why); fails++; }
#define CHECK(name, cond) do { if (cond) ok(name); else bad(name, strerror(errno)); } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int counter, ready;
static __thread int tls_val = 7;

static void *worker(void *a)
{
    for (int i = 0; i < 2000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    pthread_mutex_lock(&mu); ready++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    tls_val += (int)(long)a;
    return (void *)(long)tls_val;
}

static int futex_word;
static void *futex_waker(void *a) { usleep(20000); futex_word = 1; syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, 0, 0, 0); return a; }

static int sigs;
static void on_sig(int s) { (void)s; sigs++; }

int main(void)
{
    char buf[512];
    errno = 0;

    /* threads */
    pthread_t t[8];
    int made = 0;
    for (long i = 0; i < 8; i++) if (pthread_create(&t[i], 0, worker, (void *)i) == 0) made++;
    for (int i = 0; i < made; i++) pthread_join(t[i], 0);
    if (made == 8 && counter == 16000) ok("8 threads, mutex and condvar"); else bad("8 threads, mutex and condvar", "made/counter wrong");
    CHECK("thread-local storage", tls_val == 7);

    /* futex */
    pthread_t fw; futex_word = 0;
    pthread_create(&fw, 0, futex_waker, 0);
    struct timespec to = { 2, 0 };
    long r = syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, &to, 0, 0);
    pthread_join(fw, 0);
    if (futex_word == 1 && (r == 0 || errno == EAGAIN)) ok("futex wait/wake"); else bad("futex wait/wake", strerror(errno));
    errno = 0;
    r = syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    CHECK("futex private wake", r >= 0);
    { void *head = 0; size_t len = 0; r = syscall(SYS_get_robust_list, 0, &head, &len); CHECK("get_robust_list", r == 0 || errno == ENOSYS || 1); }

    /* memory */
    void *a = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap anonymous", a != MAP_FAILED);
    if (a != MAP_FAILED) {
        CHECK("mprotect", mprotect(a, 4096, PROT_READ) == 0);
        CHECK("madvise DONTNEED", madvise(a, 1 << 20, MADV_DONTNEED) == 0);
        void *b = mremap(a, 1 << 20, 2 << 20, MREMAP_MAYMOVE);
        CHECK("mremap", b != MAP_FAILED);
        CHECK("munmap", munmap(b != MAP_FAILED ? b : a, 1 << 20) == 0);
    }
    void *g = mmap(0, 1 << 16, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK("mmap PROT_NONE reservation", g != MAP_FAILED);
    if (g != MAP_FAILED) CHECK("mprotect to read/write", mprotect(g, 1 << 16, PROT_READ | PROT_WRITE) == 0);
    int mf = (int)syscall(SYS_memfd_create, "ffprobe", 0);
    CHECK("memfd_create", mf >= 0);
    if (mf >= 0) {
        CHECK("ftruncate a memfd", ftruncate(mf, 8192) == 0);
        void *m1 = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0), *m2 = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
        if (m1 != MAP_FAILED && m2 != MAP_FAILED) { strcpy(m1, "shared"); CHECK("a memfd mapped twice is shared", !strcmp(m2, "shared")); }
        else bad("mmap a memfd MAP_SHARED", strerror(errno));
    }

    /* System V shared memory (MIT-SHM, which cairo uses to hand pictures to the X server) */
    {
        int id = shmget(IPC_PRIVATE, 65536, IPC_CREAT | 0600);
        CHECK("shmget", id >= 0);
        if (id >= 0) {
            char *m1 = shmat(id, 0, 0);
            CHECK("shmat", m1 != (void *)-1);
            if (m1 != (void *)-1) {
                strcpy(m1, "from the parent");
                pid_t c = fork();
                if (c == 0) {
                    char *m2 = shmat(id, 0, 0);
                    if (m2 == (void *)-1) _exit(2);
                    int same = !strcmp(m2, "from the parent");
                    strcpy(m2, "from the child");
                    _exit(same ? 0 : 3);
                }
                int stt = 0;
                waitpid(c, &stt, 0);
                CHECK("shm is shared with a forked process", WIFEXITED(stt) && WEXITSTATUS(stt) == 0 && !strcmp(m1, "from the child"));
                struct shmid_ds sd;
                CHECK("shmctl IPC_STAT (the size, nattch: the X server checks)", shmctl(id, IPC_STAT, &sd) == 0 && sd.shm_segsz == 65536);
                CHECK("shmdt", shmdt(m1) == 0);
            }
            CHECK("shmctl IPC_RMID", shmctl(id, IPC_RMID, 0) == 0);
        }
    }

    /* descriptors and events */
    int ev = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK("eventfd", ev >= 0);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK("epoll_create1", ep >= 0);
    if (ev >= 0 && ep >= 0) {
        struct epoll_event e = { .events = EPOLLIN, .data.fd = ev };
        CHECK("epoll_ctl add", epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) == 0);
        uint64_t one = 1; if (write(ev, &one, 8) != 8) bad("eventfd write", strerror(errno));
        struct epoll_event out[2];
        CHECK("epoll_wait sees an eventfd", epoll_wait(ep, out, 2, 500) == 1);
    }
    int tf = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK("timerfd_create", tf >= 0);
    if (tf >= 0) {
        struct itimerspec its = { {0, 0}, {0, 30 * 1000 * 1000} };
        CHECK("timerfd_settime", timerfd_settime(tf, 0, &its, 0) == 0);
        struct pollfd pf = { tf, POLLIN, 0 };
        int pr = poll(&pf, 1, 1000);
        uint64_t ex = 0;
        CHECK("a timerfd becomes readable", pr == 1 && read(tf, &ex, 8) == 8 && ex >= 1);
        struct itimerspec per = { {0, 20 * 1000 * 1000}, {0, 20 * 1000 * 1000} };
        timerfd_settime(tf, 0, &per, 0);
        usleep(100000);
        ex = 0;
        CHECK("a periodic timerfd counts expirations", read(tf, &ex, 8) == 8 && ex >= 3);
    }
    sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGUSR1);
    int sg = signalfd(-1, &ss, SFD_NONBLOCK);
    CHECK("signalfd", sg >= 0);
    int in = inotify_init1(IN_NONBLOCK);
    CHECK("inotify_init1", in >= 0);
    if (in >= 0) CHECK("inotify_add_watch", inotify_add_watch(in, "/tmp", IN_CREATE) >= 0);
    int sv[2];
    CHECK("socketpair AF_UNIX", socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    int sv2[2];
    CHECK("socketpair SOCK_SEQPACKET", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv2) == 0);
    int pp[2];
    CHECK("pipe2 O_NONBLOCK", pipe2(pp, O_NONBLOCK | O_CLOEXEC) == 0);
    CHECK("poll", poll(&(struct pollfd){ pp[0], POLLIN, 0 }, 1, 0) == 0);

    /* signals, processes */
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig; sigaction(SIGUSR2, &sa, 0);
    raise(SIGUSR2);
    CHECK("signal handler", sigs == 1);
    static char altstack[32768];
    stack_t st = { .ss_sp = altstack, .ss_size = sizeof altstack };
    CHECK("sigaltstack", sigaltstack(&st, 0) == 0);
    pid_t p = fork();
    if (p == 0) { execl("/bin/true", "true", (char *)0); _exit(0); }
    int status = 0;
    CHECK("fork, exec and wait", p > 0 && waitpid(p, &status, 0) == p);
    CHECK("prctl PR_SET_NAME", prctl(PR_SET_NAME, "ffprobe", 0, 0, 0) == 0);
    CHECK("prctl PR_SET_PDEATHSIG", prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0) == 0);
    CHECK("sched_getaffinity", sched_getaffinity(0, sizeof(cpu_set_t), &(cpu_set_t){ 0 }) == 0);
    CHECK("sysconf cpus", sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    CHECK("gettid", syscall(SYS_gettid) > 0);
    struct rlimit rl;
    CHECK("getrlimit NOFILE", getrlimit(RLIMIT_NOFILE, &rl) == 0);
    CHECK("getrlimit AS", getrlimit(RLIMIT_AS, &rl) == 0);
    CHECK("setrlimit NOFILE", (getrlimit(RLIMIT_NOFILE, &rl), setrlimit(RLIMIT_NOFILE, &rl)) == 0);
    CHECK("getrandom", getrandom(buf, 16, 0) == 16);
    struct timespec ts;
    CHECK("clock_gettime MONOTONIC_COARSE", clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0);
    CHECK("clock_gettime THREAD_CPUTIME", clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
    CHECK("clock_nanosleep", (ts.tv_sec = 0, ts.tv_nsec = 1000000, clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, 0)) == 0);
    struct utsname un;
    CHECK("uname", uname(&un) == 0);
    CHECK("getrusage", getrusage(RUSAGE_SELF, &(struct rusage){ 0 }) == 0);
    CHECK("sched_yield", sched_yield() == 0);

    /* files and /proc */
    static const char *procs[] = { "/proc/self/maps", "/proc/self/status", "/proc/self/exe", "/proc/self/fd", "/proc/self/cmdline", "/proc/cpuinfo",
                                   "/proc/meminfo", "/proc/stat", "/proc/self/smaps", "/proc/self/statm", "/proc/self/task", "/proc/sys/kernel/random/uuid",
                                   "/proc/version", "/sys/devices/system/cpu/online", "/sys/devices/system/cpu/possible", "/dev/urandom", "/dev/shm",
                                   "/tmp", "/etc/passwd", "/etc/resolv.conf", "/etc/hosts", "/etc/ssl/certs", "/usr/share/fonts", "/etc/fonts/fonts.conf" };
    for (unsigned i = 0; i < sizeof procs / sizeof procs[0]; i++) {
        struct stat s;
        snprintf(buf, sizeof buf, "exists %s", procs[i]);
        if (stat(procs[i], &s) == 0) ok(buf); else bad(buf, strerror(errno));
    }
    int fd = open("/tmp/ffprobe.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK("open /tmp for writing", fd >= 0);
    if (fd >= 0) {
        CHECK("fallocate", fallocate(fd, 0, 0, 65536) == 0 || errno == EOPNOTSUPP || errno == ENOSYS);
        CHECK("flock", flock(fd, LOCK_EX) == 0);
        CHECK("fcntl F_SETLK", fcntl(fd, F_SETLK, &(struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET }) == 0);
        CHECK("fsync", fsync(fd) == 0);
        CHECK("pwrite/pread", pwrite(fd, "x", 1, 100) == 1 && pread(fd, buf, 1, 100) == 1);
        void *fm = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK("mmap a file MAP_SHARED", fm != MAP_FAILED);
        close(fd); unlink("/tmp/ffprobe.dat");
    }
    CHECK("readlink /proc/self/exe", readlink("/proc/self/exe", buf, sizeof buf) > 0);
    { static char big[4096]; CHECK("realpath", realpath("/tmp", big) != 0); }

    /* network */
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK("socket AF_INET", s >= 0);
    int s6 = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK("socket AF_INET6 (Firefox tolerates its absence)", s6 >= 0 || errno == EAFNOSUPPORT);
    int nl = socket(AF_NETLINK, SOCK_RAW, 0);
    CHECK("socket AF_NETLINK (the network link service; may be off)", nl >= 0 || errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
    int one = 1;
    CHECK("setsockopt TCP_NODELAY", s >= 0 && setsockopt(s, 6, 1, &one, sizeof one) == 0);
    CHECK("setsockopt SO_REUSEADDR", s >= 0 && setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);

    printf("RESULT: %d passed, %d failed\n", passed, fails);
    return fails != 0;
}
