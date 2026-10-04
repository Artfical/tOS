#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"

static int req_put(char *req, int cap, int *off, const char *s, int check)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (check && (c <= 0x20 || c == 0x7F)) return -1;
        if (*off >= cap - 1) return -1;
        req[(*off)++] = (char)c;
    }
    return 0;
}

int http_build_request(char *req, int cap, const char *host, const char *path)
{
    int off = 0;
    if (req_put(req, cap, &off, "GET ", 0) || req_put(req, cap, &off, path, 1) ||
        req_put(req, cap, &off, " HTTP/1.0\r\nHost: ", 0) || req_put(req, cap, &off, host, 1) ||
        req_put(req, cap, &off, "\r\nConnection: close\r\n\r\n", 0))
        return -1;
    req[off] = 0;
    return off;
}

int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    int rc = tcp_connect(ip, port);
    if (rc != 0) return rc;

    char req[1024];
    int off = http_build_request(req, sizeof(req), host, path);
    if (off < 0) { tcp_close(); return HTTP_ERR_REQUEST; }

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
    if (err == HTTP_ERR_REQUEST) return "request too long or invalid characters in URL";
    if (err <= -20 && err >= -24) return dns_strerror(err);
    return tcp_connect_strerror(err);
}
