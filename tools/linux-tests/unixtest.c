/* AF_UNIX stream sockets with a path: server and client, big data, end of file, refused, in use, a descriptor sent along */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("unixtest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("unixtest: ok %s\n", what); } while (0)
static uint32_t sum(const unsigned char *b, size_t n) { uint32_t h = 2166136261u; while (n--) h = (h ^ *b++) * 16777619u; return h; }
static int full_read(int fd, void *buf, size_t n) { size_t got = 0; while (got < n) { ssize_t k = read(fd, (char *)buf + got, n - got); if (k <= 0) return -1; got += (size_t)k; } return 0; }
static int full_write(int fd, const void *buf, size_t n) { size_t off = 0; while (off < n) { ssize_t k = write(fd, (const char *)buf + off, n - off); if (k <= 0) return -1; off += (size_t)k; } return 0; }
static int send_fd(int s, int fd)
{
    char data = 'F'; struct iovec iov = { &data, 1 };
    char ctl[CMSG_SPACE(sizeof(int))]; memset(ctl, 0, sizeof(ctl));
    struct msghdr m = { 0 }; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof(ctl);
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}
static int recv_fd(int s)
{
    char data; struct iovec iov = { &data, 1 };
    char ctl[CMSG_SPACE(sizeof(int))]; memset(ctl, 0, sizeof(ctl));
    struct msghdr m = { 0 }; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof(ctl);
    if (recvmsg(s, &m, 0) != 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int)); return fd;
}
int main(void)
{
    const char *path = "/tmp/unixtest.sock";
    unlink(path);
    struct sockaddr_un a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket(AF_UNIX)");
    CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0, "bind to a path");
    struct stat st; CHECK(stat(path, &st) == 0, "the socket file exists");
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(s2, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EADDRINUSE, "a second bind: EADDRINUSE");
    close(s2);
    CHECK(listen(s, 5) == 0, "listen");
    int cn = socket(AF_UNIX, SOCK_STREAM, 0); fcntl(cn, F_SETFL, O_NONBLOCK);
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    CHECK(accept(s, NULL, NULL) == -1 && errno == EAGAIN, "accept with nobody waiting: EAGAIN");
    fcntl(s, F_SETFL, fl); close(cn);
    int nl = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un none; memset(&none, 0, sizeof(none)); none.sun_family = AF_UNIX; strcpy(none.sun_path, "/tmp/unixtest.nothing");
    CHECK(connect(nl, (struct sockaddr *)&none, sizeof(none)) == -1 && errno == ENOENT, "connect to a missing path: ENOENT");
    close(nl);
    pid_t child = fork();
    if (child == 0) {
        close(s);
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof(a)) != 0) _exit(10);
        char hello[6];
        if (full_read(c, hello, 6) || memcmp(hello, "hello\n", 6)) _exit(11);
        write(c, "world\n", 6);
        size_t n = 1u << 20; unsigned char *b = malloc(n);
        for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(i * 31 + (i >> 8));
        if (full_write(c, b, n)) _exit(12);
        uint32_t h = sum(b, n); write(c, &h, 4);
        int pfd[2]; if (pipe(pfd)) _exit(13);
        write(pfd[1], "via-pipe", 8);
        if (send_fd(c, pfd[0])) _exit(14);
        char ack; if (full_read(c, &ack, 1) || ack != 'K') _exit(15);
        close(c);
        _exit(0);
    }
    struct pollfd p = { s, POLLIN, 0 };
    CHECK(poll(&p, 1, 30000) == 1 && (p.revents & POLLIN), "the listener becomes readable");
    int c = accept(s, NULL, NULL);
    CHECK(c >= 0, "accept");
    struct ucred cr; socklen_t cl = sizeof(cr);
    CHECK(getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cl == sizeof(cr) && cr.uid == 0, "SO_PEERCRED");
    struct sockaddr_un sn; socklen_t sl = sizeof(sn);
    CHECK(getsockname(s, (struct sockaddr *)&sn, &sl) == 0 && sn.sun_family == AF_UNIX && !strcmp(sn.sun_path, path), "getsockname of the listener is the path");
    write(c, "hello\n", 6);
    char w[6]; CHECK(full_read(c, w, 6) == 0 && !memcmp(w, "world\n", 6), "a line comes back");
    size_t n = 1u << 20; unsigned char *b = malloc(n);
    CHECK(full_read(c, b, n) == 0, "a megabyte arrives");
    uint32_t hs; CHECK(full_read(c, &hs, 4) == 0 && hs == sum(b, n), "its checksum matches");
    int rfd = recv_fd(c);
    char pb[9] = { 0 };
    CHECK(rfd >= 0 && read(rfd, pb, 8) == 8 && !strcmp(pb, "via-pipe"), "a descriptor sent over the socket works");
    write(c, "K", 1);
    int stt; waitpid(child, &stt, 0);
    CHECK(WIFEXITED(stt) && WEXITSTATUS(stt) == 0, "the client ended well");
    char eof; CHECK(read(c, &eof, 1) == 0, "end of file after the peer closed");
    close(c);
    close(s); unlink(path);
    int r = socket(AF_UNIX, SOCK_STREAM, 0);
    int fd2 = open(path, O_CREAT | O_WRONLY, 0600); close(fd2);
    CHECK(connect(r, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == ECONNREFUSED, "a path nobody listens on: ECONNREFUSED");
    unlink(path);
    /* an abstract socket */
    struct sockaddr_un ab; memset(&ab, 0, sizeof(ab)); ab.sun_family = AF_UNIX; memcpy(ab.sun_path + 1, "tos-abs", 7);
    socklen_t abl = offsetof(struct sockaddr_un, sun_path) + 8;
    int as = socket(AF_UNIX, SOCK_STREAM, 0), ac = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(as, (struct sockaddr *)&ab, abl) == 0 && listen(as, 1) == 0, "an abstract socket listens");
    CHECK(connect(ac, (struct sockaddr *)&ab, abl) == 0, "and takes a connection");
    int ax = accept(as, NULL, NULL);
    write(ac, "xy", 2); char xb[2]; CHECK(ax >= 0 && full_read(ax, xb, 2) == 0 && xb[0] == 'x', "with data");
    printf(fails ? "unixtest: %d FAILED\n" : "unixtest: all passed\n", fails);
    return fails != 0;
}
