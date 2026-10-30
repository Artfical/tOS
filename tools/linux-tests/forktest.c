/* fork, exec, pipes, signals, wait: what Node's child_process and git need from tOS. Run as /programs/forktest;
 * it starts itself again as "child", "sleep" and "arg" (a #! script, /programs/ft.sh, names it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static volatile int got_chld;
static void on_chld(int s) { (void)s; got_chld++; }
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) { printf("hello-from-child pid=%d\n", getpid()); return 7; }
    if (argc > 1 && !strcmp(argv[1], "sleep")) { for (;;) pause(); }
    if (argc > 1 && strstr(argv[1], "ft-env")) { printf("[FT] env shebang ran %s with %s\n", argv[1], argv[0]); return 0; }
    if (argc > 2 && !strcmp(argv[1], "arg")) { printf("[FT] shebang argv: %s %s %s\n", argv[0], argv[1], argv[2]); return 0; }
    setvbuf(stdout, 0, _IONBF, 0);
    int st = 0, x = 1;
    char *big = malloc(8 << 20); memset(big, 5, 8 << 20);
    pid_t p = fork();
    if (p == 0) { x = 2; big[100] = 9; _exit(40 + x + big[100]); }
    waitpid(p, &st, 0);
    printf("[FT] cow: parent x=%d big=%d child exit=%d (want 1 5 51)\n", x, big[100], WEXITSTATUS(st));
    int fds[2]; if (pipe(fds)) return 1;
    p = fork();
    if (p == 0) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); execl(argv[0], argv[0], "child", (char *)0); _exit(99); }
    close(fds[1]);
    char buf[200]; int n = 0, r;
    while ((r = read(fds[0], buf + n, sizeof(buf) - 1 - n)) > 0) n += r;
    buf[n] = 0;
    waitpid(p, &st, 0);
    printf("[FT] pipe+exec: got '%s' exit=%d\n", strtok(buf, "\n"), WEXITSTATUS(st));
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_chld; sigaction(SIGCHLD, &sa, 0);
    p = fork();
    if (p == 0) _exit(3);
    for (int i = 0; i < 200 && !got_chld; i++) usleep(10000);
    pid_t w = waitpid(-1, &st, 0);
    printf("[FT] sigchld handler ran=%d wait=%d exit=%d\n", got_chld, w == p, WEXITSTATUS(st));
    char *sargv[] = { argv[0], "child", 0 };
    int rc = posix_spawn(&p, argv[0], 0, 0, sargv, environ);
    waitpid(p, &st, 0);
    printf("[FT] posix_spawn rc=%d exit=%d\n", rc, WEXITSTATUS(st));
    p = fork();
    if (p == 0) { execl("/programs/ft.sh", "ft.sh", (char *)0); _exit(98); }
    waitpid(p, &st, 0);
    printf("[FT] shebang exit=%d\n", WEXITSTATUS(st));
    p = fork();                    /* "#!/usr/bin/env name": name from path.tmbl, and from an installed package's bin folder */
    if (p == 0) { execl("/programs/ft-env.sh", "ft-env.sh", (char *)0); _exit(96); }
    waitpid(p, &st, 0);
    printf("[FT] env via path.tmbl exit=%d\n", WEXITSTATUS(st));
    p = fork();
    if (p == 0) { execl("/programs/ft-env2.sh", "ft-env2.sh", (char *)0); _exit(95); }
    waitpid(p, &st, 0);
    printf("[FT] env via package bin exit=%d\n", WEXITSTATUS(st));
    p = fork();
    if (p == 0) { execl(argv[0], argv[0], "sleep", (char *)0); _exit(97); }
    usleep(200000);
    kill(p, SIGTERM);
    waitpid(p, &st, 0);
    printf("[FT] kill: signaled=%d sig=%d\n", WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    printf("[FT] wnohang-none=%d\n", waitpid(-1, &st, WNOHANG));
    printf("[FT] done\n");
    return 0;
}
