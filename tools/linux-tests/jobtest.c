/* job control: SIGSTOP / SIGTSTP stop a program, wait4 reports it (WUNTRACED), SIGCONT goes on (WCONTINUED) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("jobtest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("jobtest: ok %s\n", what); } while (0)
static int drain(int fd) { char b[256]; int total = 0, n; while ((n = read(fd, b, sizeof(b))) > 0) total += n; return total; }
static void spawn(int *rd, pid_t *pid, int handle_tstp)
{
    int p[2]; pipe(p);
    *pid = fork();
    if (*pid == 0) {
        close(p[0]);
        if (handle_tstp) { struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = SIG_IGN; sigaction(SIGTSTP, &sa, NULL); }
        for (;;) { write(p[1], "x", 1); struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL); }
    }
    close(p[1]); fcntl(p[0], F_SETFL, O_NONBLOCK); *rd = p[0];
}
int main(void)
{
    int rd; pid_t c; int st;
    spawn(&rd, &c, 0);
    usleep(500000); CHECK(drain(rd) > 2, "the child runs and writes");
    kill(c, SIGSTOP);
    CHECK(waitpid(c, &st, WUNTRACED) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "wait4 WUNTRACED reports the stop");
    usleep(300000); drain(rd); usleep(600000);
    CHECK(drain(rd) == 0, "a stopped child writes nothing");
    kill(c, SIGCONT);
    CHECK(waitpid(c, &st, WCONTINUED) == c && WIFCONTINUED(st), "wait4 WCONTINUED reports the continue");
    usleep(600000); CHECK(drain(rd) > 2, "it runs again after SIGCONT");
    kill(c, SIGTSTP);
    CHECK(waitpid(c, &st, WUNTRACED) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP, "SIGTSTP stops it too");
    kill(c, SIGKILL);
    CHECK(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "a stopped child can still be killed");
    close(rd);
    spawn(&rd, &c, 1);                                 /* ignores SIGTSTP */
    usleep(300000); drain(rd); kill(c, SIGTSTP); usleep(700000);
    CHECK(drain(rd) > 2, "a program that ignores SIGTSTP keeps running");
    kill(c, SIGKILL); waitpid(c, &st, 0);
    printf(fails ? "jobtest: %d FAILED\n" : "jobtest: all passed\n", fails);
    return fails != 0;
}
