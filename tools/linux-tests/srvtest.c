/* A TCP server: listens on port 7777, accepts two connections, echoes what each sends in upper case
 * (a test from outside: QEMU forwards a host port to it) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
int main(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7777), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) { printf("srvtest: bind/listen failed\n"); return 1; }
    printf("srvtest: listening\n"); fflush(stdout);
    for (int n = 0; n < 2; n++) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 120000) != 1) { printf("srvtest: no connection\n"); return 2; }
        struct sockaddr_in peer; socklen_t pl = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        if (c < 0) { printf("srvtest: accept failed\n"); return 3; }
        printf("srvtest: connection %d from port %d\n", n + 1, ntohs(peer.sin_port)); fflush(stdout);
        char buf[256];
        int got = read(c, buf, sizeof(buf));
        for (int i = 0; i < got; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
        if (got > 0) write(c, buf, got);
        close(c);
    }
    printf("srvtest: done\n");
    return 0;
}
