#include "https.h"
#include "tls.h"
#include "http.h"
#include "tcp.h"
#include "string.h"

static tls_ctx_t g_tls;  /* static: one HTTPS connection at a time */

int https_get(uint32_t ip, const char *host, uint16_t port, const char *path,
              uint8_t *response, int max_len)
{
    /* host gets consumed by the request-building loop below, so grab
     * the SNI value before that happens. */
    const char *sni_host = host;
    int rc = tls_connect(&g_tls, ip, port, sni_host);
    if (rc != 0) return rc;

    /* Build HTTP/1.0 request */
    char req[1024];
    int off = http_build_request(req, sizeof(req), host, path);
    if (off < 0) { tls_close(&g_tls); return HTTP_ERR_REQUEST; }

    if (tls_write(&g_tls, (const uint8_t*)req, off) != 0) {
        tls_close(&g_tls);
        return TCP_ERR_TIMEOUT; /* connection dropped between connect() and send() */
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
