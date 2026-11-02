/* alarm, setitimer and POSIX timers: they signal the program, and interrupt waits */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
static int fails;
static volatile int hits;
#define CHECK(c, what) do { if (!(c)) { printf("timertest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("timertest: ok %s\n", what); } while (0)
static void on_sig(int s) { (void)s; hits++; }
int main(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_sig;
    sigaction(SIGALRM, &sa, NULL); sigaction(SIGUSR1, &sa, NULL);
    hits = 0; alarm(1);
    int r = pause();
    CHECK(r == -1 && errno == EINTR && hits == 1, "alarm(1) interrupts pause");
    struct itimerval it = { {0, 0}, {0, 300000} }; hits = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    r = poll(NULL, 0, 10000);
    CHECK(r == -1 && errno == EINTR && hits == 1, "setitimer interrupts poll");
    timer_t t; struct sigevent ev; memset(&ev, 0, sizeof(ev)); ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGUSR1;
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0, "timer_create");
    struct itimerspec its = { {0, 200000000}, {0, 200000000} }; hits = 0;
    CHECK(timer_settime(t, 0, &its, NULL) == 0, "timer_settime");
    for (int i = 0; i < 100 && hits < 3; i++) usleep(50000);
    CHECK(hits >= 3, "a repeating timer fires several times");
    struct itimerspec cur; CHECK(timer_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 200000000, "timer_gettime");
    CHECK(timer_delete(t) == 0, "timer_delete");
    hits = 0; usleep(500000);
    CHECK(hits == 0, "a deleted timer is quiet");
    struct timespec a, b, ts = {0, 100000000}; clock_gettime(CLOCK_MONOTONIC, &a); nanosleep(&ts, NULL); clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 90 && ms < 1500, "nanosleep sleeps about 100 ms");
    hits = 0; alarm(1); ts.tv_sec = 20; ts.tv_nsec = 0;
    r = nanosleep(&ts, NULL);
    CHECK(r == -1 && errno == EINTR && hits == 1, "alarm interrupts nanosleep");
    printf(fails ? "timertest: %d FAILED\n" : "timertest: all passed\n", fails);
    return fails != 0;
}
