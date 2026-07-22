#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"

int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    int rc = tcp_connect(ip, port);
    if (rc != 0) return rc;

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

    if (tcp_send(req, off) != 0) { tcp_close(); return TCP_ERR_TIMEOUT; /* connection dropped between connect() and send() */ }

    int total = 0;
    while (total < max_len) {
        int n = tcp_recv(response + total, max_len - total);
        if (n <= 0) break;
        total += n;
    }

    tcp_close();
    return total;
}

const char *http_strerror(int err)
{
    /* DNS_ERR_* occupies exactly -20..-24 (see dns.h); anything in that
     * band is dns_resolve()'s own code, everything else -- TCP_ERR_*,
     * or an ARP/IP code tcp_connect() propagated verbatim -- belongs to
     * tcp_connect_strerror(), which already falls back to
     * arp_resolve_strerror() for those. */
    if (err <= -20 && err >= -24) return dns_strerror(err);
    return tcp_connect_strerror(err);
}
