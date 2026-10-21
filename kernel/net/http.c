#include "http.h"

/* told how many bytes have arrived, while a response comes in (Software Update shows it) */
void (*http_progress)(int received);
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"
#include "scheduler.h"

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

const http_extra_t *http_extra;

static int build_request(char *req, int cap, const char *host, const char *path, uint32_t from)
{
    int off = 0;
    const http_extra_t *x = http_extra;
    if (req_put(req, cap, &off, x && x->method ? x->method : "GET", 1) || req_put(req, cap, &off, " ", 0) || req_put(req, cap, &off, path, 1) ||
        req_put(req, cap, &off, " HTTP/1.0\r\nHost: ", 0) || req_put(req, cap, &off, host, 1))
        return -1;
    if (from) {
        char n[12]; int o = 0; char r[12]; int ro = 0;
        do { r[ro++] = (char)('0' + from % 10); from /= 10; } while (from);
        while (ro) n[o++] = r[--ro];
        n[o] = 0;
        if (req_put(req, cap, &off, "\r\nRange: bytes=", 0) || req_put(req, cap, &off, n, 0) || req_put(req, cap, &off, "-", 0))
            return -1;
    }
    if (req_put(req, cap, &off, "\r\nUser-Agent: Lynx/2.9.0 libwww-FM/2.14 tFire/1 (tOS)\r\nAccept: */*\r\nConnection: close\r\n", 0)) return -1;
    if (x && x->headers) {
        /* the asker's own header lines, each ending in \r\n: no bare line breaks that would start a new request */
        for (const char *h = x->headers; *h; h++)
            if ((*h == '\n' && (h == x->headers || h[-1] != '\r')) || (*h == '\r' && h[1] != '\n')) return -1;
        if (req_put(req, cap, &off, x->headers, 0)) return -1;
    }
    if (x && x->body) {
        char n[12]; int o = 0; char r[12]; int ro = 0;
        uint32_t v = (uint32_t)x->body_len;
        do { r[ro++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (ro) n[o++] = r[--ro];
        n[o] = 0;
        if (req_put(req, cap, &off, "Content-Length: ", 0) || req_put(req, cap, &off, n, 0) || req_put(req, cap, &off, "\r\n", 0)) return -1;
    }
    if (req_put(req, cap, &off, "\r\n", 0)) return -1;
    req[off] = 0;
    return off;
}

int http_build_request(char *req, int cap, const char *host, const char *path)
{
    const http_extra_t *x = http_extra;
    http_extra = 0;
    int n = build_request(req, cap, host, path, 0);
    http_extra = x;
    return n;
}

int http_build_full_request(char *req, int cap, const char *host, const char *path)
{
    return build_request(req, cap, host, path, 0);
}

static int hdr_end(const uint8_t *r, int total)
{
    for (int i = 0; i + 3 < total; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') return i + 4;
    return -1;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Finds header `name` (lower case, with colon) inside the header block and
 * returns a pointer to its value, or 0. */
static const uint8_t *hdr_value(const uint8_t *r, int hlen, const char *name)
{
    int nl = (int)strlen(name);
    for (int i = 0; i + nl < hlen; i++) {
        if (i != 0 && r[i - 1] != '\n') continue;
        int k = 0;
        while (k < nl && lc(r[i + k]) == name[k]) k++;
        if (k == nl) {
            const uint8_t *v = r + i + nl;
            while (v < r + hlen && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
    }
    return 0;
}

const char *http_header_value(const char *resp, int len, const char *name)
{
    const uint8_t *r = (const uint8_t *)resp;
    int he = hdr_end(r, len);
    if (he < 0) he = len;                       /* headers still incomplete: search what there is */
    return (const char *)hdr_value(r, he, name);
}

int http_response_check(const uint8_t *r, int *total)
{
    int he = hdr_end(r, *total);
    if (he < 0) return *total > HTTP_MAX_HEADER ? HTTP_ERR_HEADER : 0;
    if (he > HTTP_MAX_HEADER) return HTTP_ERR_HEADER;
    const uint8_t *v = hdr_value(r, he, "content-length:");
    if (!v) return 0;
    long cl = 0;
    int digits = 0;
    while (v < r + he && *v >= '0' && *v <= '9') {
        cl = cl * 10 + (*v - '0');
        if (++digits > 9) return HTTP_ERR_HEADER;   /* absurd length: no overflow */
        v++;
    }
    if (digits == 0) return 0;
    if ((long)(*total - he) >= cl) { *total = he + (int)cl; return 1; }
    return 0;
}

int http_dechunk(uint8_t *r, int total)
{
    int he = hdr_end(r, total);
    if (he < 0) return total;
    const uint8_t *te = hdr_value(r, he, "transfer-encoding:");
    if (!te || lc(te[0]) != 'c') return total;       /* not chunked */
    int in = he, out = he;
    for (;;) {
        long sz = 0;
        int digits = 0;
        while (in < total) {
            int c = lc(r[in]);
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) break;
            sz = sz * 16 + d;
            if (++digits > 7) return HTTP_ERR_HEADER;
            in++;
        }
        if (digits == 0) return HTTP_ERR_HEADER;
        while (in < total && r[in] != '\n') in++;       /* skip chunk extensions */
        if (in >= total) return HTTP_ERR_HEADER;
        in++;
        if (sz == 0) return out;
        if (sz > total - in) return HTTP_ERR_HEADER;   /* truncated chunk */
        memmove(r + out, r + in, (size_t)sz);
        out += (int)sz;
        in += (int)sz;
        if (in + 1 < total && r[in] == '\r' && r[in + 1] == '\n') in += 2;
        else return HTTP_ERR_HEADER;
    }
}

int http_last_recv;

int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    return http_get_from(ip, host, port, path, 0, response, max_len);
}

/* There is one TCP connection (tcp_connect() has no handle), so one request at a time: Software Update and
 * tFire take turns. The owner is a task, so a request made while holding it does not wait for itself. */
static volatile uint32_t net_owner;
static int net_depth;

void http_lock(void)
{
    uint32_t me = task_get_pid();
    for (;;) {
        uint32_t fl;
        asm volatile("pushfl; popl %0; cli" : "=r"(fl));
        task_t *o = net_owner ? task_by_pid(net_owner) : 0;
        if (!net_owner || net_owner == me || !o || o->state == TASK_STATE_ZOMBIE) {   /* (an owner that died) */
            if (net_owner != me) net_depth = 0;
            net_owner = me;
            net_depth++;
            asm volatile("pushl %0; popfl" : : "r"(fl));
            return;
        }
        asm volatile("pushl %0; popfl" : : "r"(fl));
        task_yield();
    }
}

void http_unlock(void)
{
    if (net_owner == task_get_pid() && --net_depth <= 0) { net_depth = 0; net_owner = 0; }
}

static int http_get_from_locked(uint32_t ip, const char *host, uint16_t port, const char *path, uint32_t from, uint8_t *response, int max_len);

int http_get_from(uint32_t ip, const char *host, uint16_t port, const char *path, uint32_t from, uint8_t *response, int max_len)
{
    http_lock();
    int n = http_get_from_locked(ip, host, port, path, from, response, max_len);
    http_unlock();
    return n;
}

static int http_get_from_locked(uint32_t ip, const char *host, uint16_t port, const char *path, uint32_t from, uint8_t *response, int max_len)
{
    http_last_recv = 0;
    int rc = tcp_connect(ip, port);
    if (rc != 0) return rc;

    static char req[HTTP_MAX_REQUEST];              /* (only the holder of http_lock() is here) */
    int off = build_request(req, sizeof(req), host, path, from);
    if (off < 0) { tcp_close(); return HTTP_ERR_REQUEST; }

    if (tcp_send(req, off) != 0) { tcp_close(); return TCP_ERR_TIMEOUT; /* connection dropped between connect() and send() */ }
    if (http_extra && http_extra->body && http_extra->body_len > 0 &&
        tcp_send((void *)http_extra->body, http_extra->body_len) != 0) { tcp_close(); return TCP_ERR_TIMEOUT; }

    int total = 0;
    while (total < max_len) {
        int n = tcp_recv(response + total, max_len - total);
        if (n <= 0) { http_last_recv = n; break; }
        total += n;
        if (http_progress) http_progress(total);
        int st = http_response_check(response, &total);
        if (st < 0) { tcp_close(); return st; }
        if (st == 1) break;
    }

    tcp_close();
    return http_dechunk(response, total);
}

const char *http_strerror(int err)
{
    /* DNS_ERR_* occupies exactly -20..-24 (see dns.h); anything in that
     * band is dns_resolve()'s own code, everything else -- TCP_ERR_*,
     * or an ARP/IP code tcp_connect() propagated verbatim -- belongs to
     * tcp_connect_strerror(), which already falls back to
     * arp_resolve_strerror() for those. */
    if (err == HTTP_ERR_HEADER) return "malformed or oversized HTTP response headers";
    if (err == HTTP_ERR_REQUEST) return "request too long or invalid characters in URL";
    if (err <= -20 && err >= -25) return dns_strerror(err);
    return tcp_connect_strerror(err);
}
