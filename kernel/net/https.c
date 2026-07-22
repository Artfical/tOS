#include "https.h"
#include "tls.h"
#include "string.h"

static tls_ctx_t g_tls;  /* static: one HTTPS connection at a time */

int https_get(uint32_t ip, const char *host, uint16_t port, const char *path,
              uint8_t *response, int max_len)
{
    if (tls_connect(&g_tls, ip, port) != 0) return -1;

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
