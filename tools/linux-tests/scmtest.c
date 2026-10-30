/* Descriptors sent over a socketpair (SCM_RIGHTS), one per message as OpenSSH's monitor does */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int send_fd(int sock, int fd)
{
    struct msghdr msg; struct iovec vec; char ch = 0;
    union { struct cmsghdr hdr; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&msg, 0, sizeof(msg)); memset(&u, 0, sizeof(u));
    vec.iov_base = &ch; vec.iov_len = 1;
    msg.msg_iov = &vec; msg.msg_iovlen = 1;
    msg.msg_control = u.buf; msg.msg_controllen = sizeof(u.buf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_len = CMSG_LEN(sizeof(int)); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    *(int *)CMSG_DATA(c) = fd;
    return sendmsg(sock, &msg, 0);
}

static int recv_fd(int sock)
{
    struct msghdr msg; struct iovec vec; char ch;
    union { struct cmsghdr hdr; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&msg, 0, sizeof(msg)); memset(&u, 0, sizeof(u));
    vec.iov_base = &ch; vec.iov_len = 1;
    msg.msg_iov = &vec; msg.msg_iovlen = 1;
    msg.msg_control = u.buf; msg.msg_controllen = sizeof(u.buf);
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -2;
    return *(int *)CMSG_DATA(c);
}

int main(void)
{
    int fails = 0;
    for (int round = 0; round < 12; round++) {
        int sv[2], p1[2], p2[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pipe(p1); pipe(p2);
        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            int a = recv_fd(sv[1]), b = recv_fd(sv[1]);
            if (a < 0 || b < 0) _exit(10 + (a < 0) + 2 * (b < 0));
            write(a, "one", 3); write(b, "two", 3);
            _exit(0);
        }
        close(sv[1]);
        send_fd(sv[0], p1[1]);
        send_fd(sv[0], p2[1]);
        close(p1[1]); close(p2[1]);
        char x[8] = {0}, y[8] = {0};
        int st = 0;
        waitpid(pid, &st, 0);
        read(p1[0], x, 7); read(p2[0], y, 7);
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && !strcmp(x, "one") && !strcmp(y, "two");
        if (!ok) { printf("scmtest: round %d FAIL exit=%d x=%s y=%s\n", round, WEXITSTATUS(st), x, y); fails++; }
        close(p1[0]); close(p2[0]); close(sv[0]);
    }
    printf(fails ? "scmtest: %d FAILED\n" : "scmtest: all passed\n", fails);
    return fails != 0;
}
