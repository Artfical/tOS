#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"

int http_get(const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    uint32_t ip;
    if (dns_resolve(host, &ip) != 0) return -1;

    if (tcp_connect(ip, port) != 0) return -1;

    char req[1024];
    int off = 0;
    const char *g = "GET ";
    while (*g) req[off++] = *g++;
    while (*path) req[off++] = *path++;
    const char *h = " HTTP/1.0\r\nHost: ";
    while (*h) req[off++] = *h++;
    while (*host) req[off++] = *host++;
    const char *c = "\r\nConnection: close\r\n\r\n";
    while (*c) req[off++] = *c++;

    if (tcp_send(req, off) != 0) { tcp_close(); return -1; }

    int total = 0;
    while (total < max_len) {
        int n = tcp_recv(response + total, max_len - total);
        if (n <= 0) break;
        total += n;
    }

    tcp_close();
    return total;
}
