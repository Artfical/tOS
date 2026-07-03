#include "https.h"
#include "tls.h"
#include "dns.h"
#include "string.h"

static tls_ctx_t g_tls;  /* static: one HTTPS connection at a time */

static uint32_t https_parse_ip(const char *s)
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

int https_get(const char *host, const char *path,
              uint8_t *response, int max_len)
{
    uint32_t ip;
    int host_is_ip = 1;
    for (const char *p = host; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { host_is_ip = 0; break; }

    if (host_is_ip) ip = https_parse_ip(host);
    else if (dns_resolve(host, &ip) != 0) return -1;
    if (tls_connect(&g_tls, ip, 443) != 0) return -1;

    /* Build HTTP/1.0 request */
    char req[1024];
    int off = 0;
    const char *get  = "GET ";
    const char *ver  = " HTTP/1.0\r\nHost: ";
    const char *conn = "\r\nConnection: close\r\n\r\n";
    while (*get)  req[off++] = *get++;
    while (*path) req[off++] = *path++;
    while (*ver)  req[off++] = *ver++;
    while (*host) req[off++] = *host++;
    while (*conn) req[off++] = *conn++;

    if (tls_write(&g_tls, (const uint8_t*)req, off) != 0) {
        tls_close(&g_tls); return -1;
    }

    int total = 0;
    while (total < max_len) {
        int n = tls_read(&g_tls, response + total, max_len - total);
        if (n <= 0) break;
        total += n;
    }

    tls_close(&g_tls);
    return total;
}
