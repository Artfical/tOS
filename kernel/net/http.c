#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"

static uint32_t http_parse_ip(const char *s)
{
    uint32_t ip = 0;
    int shift = 0, val = 0;
    while (*s) {
        if (*s == '.') { ip |= (val & 0xFF) << shift; shift += 8; val = 0; }
        else if (*s >= '0' && *s <= '9') val = val * 10 + (*s - '0');
        else return 0;
        s++;
    }
    ip |= (val & 0xFF) << shift;
    return ip;
}

int http_get(const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    uint32_t ip;
    int host_is_ip = 1;
    for (const char *p = host; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { host_is_ip = 0; break; }

    if (host_is_ip) ip = http_parse_ip(host);
    else if (dns_resolve(host, &ip) != 0) return -1;

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
