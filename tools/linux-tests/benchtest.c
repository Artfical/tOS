/* How fast things are: [BENCH] name=value unit. benchtest [host:port] also downloads /big.bin from a web server. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv)
{
    double t0 = now();
    volatile unsigned long x = 1;
    for (unsigned long i = 0; i < 40000000ul; i++) x = x * 2862933555777941757ul + 3037000493ul;
    double t = now() - t0;
    printf("[BENCH] cpu_loop=%.0f ms (40M multiply-adds)\n", t * 1000);

    size_t n = 32u << 20; char *a = malloc(n), *b = malloc(n);
    memset(a, 1, n); t0 = now();
    for (int i = 0; i < 4; i++) memcpy(b, a, n);
    t = now() - t0;
    printf("[BENCH] memcpy=%.0f MB/s\n", 4.0 * (n >> 20) / t);

    t0 = now();
    for (int i = 0; i < 100000; i++) getppid();
    t = now() - t0;
    printf("[BENCH] syscall=%.0f ns each\n", t / 100000 * 1e9);

    t0 = now();
    for (int i = 0; i < 40; i++) { pid_t c = fork(); if (c == 0) _exit(0); waitpid(c, 0, 0); }
    t = now() - t0;
    printf("[BENCH] fork_wait=%.2f ms each\n", t / 40 * 1000);

    int fd = open("/tmp/bench.dat", O_CREAT | O_TRUNC | O_RDWR, 0600);
    t0 = now();
    for (int i = 0; i < 8; i++) write(fd, a, 1u << 20);
    t = now() - t0;
    printf("[BENCH] file_write=%.1f MB/s\n", 8 / t);
    lseek(fd, 0, SEEK_SET); t0 = now();
    for (int i = 0; i < 8; i++) read(fd, b, 1u << 20);
    t = now() - t0;
    printf("[BENCH] file_read=%.1f MB/s\n", 8 / t);
    close(fd); unlink("/tmp/bench.dat");

    if (argc > 1) {
        char host[64]; int port = 8000;
        strncpy(host, argv[1], sizeof(host) - 1); host[sizeof(host) - 1] = 0;
        char *colon = strchr(host, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
        inet_pton(AF_INET, host, &sa.sin_addr);
        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) { printf("[BENCH] download=failed (connect errno %d)\n", errno); return 0; }
        const char *req = "GET /big.bin HTTP/1.0\r\nHost: x\r\n\r\n";
        write(s, req, strlen(req));
        t0 = now();
        long total = 0; ssize_t k;
        while ((k = read(s, b, n)) > 0) total += k;
        t = now() - t0;
        printf("[BENCH] download=%.2f MB/s (%ld bytes in %.1f s)\n", total / 1048576.0 / t, total, t);
    }
    if (argc > 2) {                                        /* upload: 16 MB to host:port, which reads until the end */
        char host[64]; int port = 8001;
        strncpy(host, argv[2], sizeof(host) - 1); host[sizeof(host) - 1] = 0;
        char *colon = strchr(host, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
        inet_pton(AF_INET, host, &sa.sin_addr);
        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) { printf("[BENCH] upload=failed (connect errno %d)\n", errno); return 0; }
        long total = 0; t0 = now();
        while (total < (16L << 20)) { ssize_t k = write(s, a, 1u << 16); if (k <= 0) break; total += k; }
        shutdown(s, SHUT_WR);
        char ack[8]; read(s, ack, sizeof(ack));
        t = now() - t0;
        printf("[BENCH] upload=%.2f MB/s (%ld bytes in %.1f s)\n", total / 1048576.0 / t, total, t);
    }
    printf("[BENCH] done\n");
    return 0;
}
