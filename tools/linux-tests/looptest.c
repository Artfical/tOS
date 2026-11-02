/* The loopback network: a server and a client in one machine, TCP and UDP, small and big, and a refused connection */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("looptest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("looptest: ok %s\n", what); } while (0)
static uint32_t sum(const unsigned char *b, size_t n) { uint32_t h = 2166136261u; while (n--) h = (h ^ *b++) * 16777619u; return h; }
static int full_read(int fd, void *buf, size_t n) { size_t got = 0; while (got < n) { ssize_t k = read(fd, (char *)buf + got, n - got); if (k <= 0) return -1; got += (size_t)k; } return 0; }
int main(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7788), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 5) == 0, "listen on 127.0.0.1:7788");
    pid_t child = fork();
    if (child == 0) {
        close(s);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof(a)) != 0) _exit(10);
        char hello[6];
        if (full_read(c, hello, 6)) _exit(11);
        if (memcmp(hello, "hello\n", 6)) _exit(12);
        write(c, "world\n", 6);
        /* then a megabyte, which the server checks */
        size_t n = 1u << 20; unsigned char *b = malloc(n);
        for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(i * 31 + (i >> 8));
        size_t off = 0;
        while (off < n) { ssize_t k = write(c, b + off, n - off > 30000 ? 30000 : n - off); if (k <= 0) _exit(13); off += (size_t)k; }
        uint32_t h = sum(b, n);
        write(c, &h, 4);
        char ack; if (full_read(c, &ack, 1) || ack != 'K') _exit(14);
        close(c);
        _exit(0);
    }
    struct pollfd p = { s, POLLIN, 0 };
    CHECK(poll(&p, 1, 30000) == 1, "the listener becomes readable when the client connects");
    struct sockaddr_in peer; socklen_t pl = sizeof(peer);
    int c = accept(s, (struct sockaddr *)&peer, &pl);
    CHECK(c >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "accept: the client is 127.0.0.1");
    struct sockaddr_in me; socklen_t ml = sizeof(me);
    CHECK(getsockname(c, (struct sockaddr *)&me, &ml) == 0 && me.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(me.sin_port) == 7788, "getsockname of the accepted socket");
    write(c, "hello\n", 6);
    char w[6]; CHECK(full_read(c, w, 6) == 0 && !memcmp(w, "world\n", 6), "a line comes back");
    size_t n = 1u << 20; unsigned char *b = malloc(n);
    CHECK(full_read(c, b, n) == 0, "a megabyte arrives");
    uint32_t hs; CHECK(full_read(c, &hs, 4) == 0 && hs == sum(b, n), "and its checksum matches");
    write(c, "K", 1);
    int st; waitpid(child, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the client ended well");
    close(c);
    /* UDP */
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_port = htons(7799), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(u1, (struct sockaddr *)&ua, sizeof(ua)) == 0, "bind a UDP socket");
    CHECK(sendto(u2, "ping", 4, 0, (struct sockaddr *)&ua, sizeof(ua)) == 4, "sendto 127.0.0.1");
    struct pollfd up = { u1, POLLIN, 0 };
    CHECK(poll(&up, 1, 20000) == 1, "the datagram arrives");
    char ub[16]; struct sockaddr_in from; socklen_t fl = sizeof(from);
    ssize_t k = recvfrom(u1, ub, sizeof(ub), 0, (struct sockaddr *)&from, &fl);
    CHECK(k == 4 && !memcmp(ub, "ping", 4) && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "its content and sender");
    /* nobody listens on this port: the connection is refused (or at least does not succeed) */
    int r = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in na = { .sin_family = AF_INET, .sin_port = htons(7701), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int fl2 = fcntl(r, F_GETFL); fcntl(r, F_SETFL, fl2 | O_NONBLOCK);
    int cr = connect(r, (struct sockaddr *)&na, sizeof(na));
    int refused = 0;
    if (cr == -1 && errno == EINPROGRESS) { struct pollfd rp = { r, POLLOUT, 0 }; poll(&rp, 1, 15000); int e = 0; socklen_t el = sizeof(e); getsockopt(r, SOL_SOCKET, SO_ERROR, &e, &el); refused = e == ECONNREFUSED; }
    CHECK(cr == -1 && refused, "connecting to a closed port fails with ECONNREFUSED");
    printf(fails ? "looptest: %d FAILED\n" : "looptest: all passed\n", fails);
    return fails != 0;
}
