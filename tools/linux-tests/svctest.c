/* A service for the self-test: says something every second until it is told to stop (SIGTERM). */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
static volatile int stop;
static void on_term(int s) { (void)s; stop = 1; }
int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    signal(SIGTERM, on_term);
    printf("svctest: up, pid %d\n", getpid());
    for (int i = 1; !stop; i++) { printf("svctest: tick %d\n", i); sleep(1); }
    printf("svctest: stopping\n");
    return 0;
}
