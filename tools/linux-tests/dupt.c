/* dup() of the console descriptors, as ssh uses them */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/select.h>
int main(void)
{
    int in = dup(0), out = dup(1), err = dup(2);
    printf("dupt: dup -> %d %d %d\n", in, out, err);
    fflush(stdout);
    if (out < 0) return 1;
    printf("dupt: isatty in=%d out=%d\n", isatty(in), isatty(out)); fflush(stdout);
    int fl = fcntl(out, F_GETFL); fcntl(out, F_SETFL, fl | O_NONBLOCK);
    struct pollfd p = { out, POLLOUT, 0 };
    printf("dupt: poll=%d revents=%x\n", poll(&p, 1, 0), p.revents); fflush(stdout);
    fd_set w; FD_ZERO(&w); FD_SET(out, &w);
    struct timeval tv = { 0, 0 };
    printf("dupt: select=%d\n", select(out + 1, 0, &w, 0, &tv)); fflush(stdout);
    const char *m = "dupt: write via dup\n";
    printf("dupt: write=%zd\n", write(out, m, strlen(m))); fflush(stdout);
    struct iovec v = { (void *)"dupt: writev via dup\n", 21 };
    printf("dupt: writev=%zd\n", writev(out, &v, 1)); fflush(stdout);
    write(err, "dupt: err via dup\n", 18);
    close(out);
    printf("dupt: still ok after close\n");
    return 0;
}
