#include "webfetch.h"
#include "http.h"
#include "https.h"
#include "tls.h"
#include "dns.h"
#include "scheduler.h"
#include "memory.h"
#include "string.h"

#define WEBFETCH_MAX   (6 * 1024 * 1024)
#define WEBFETCH_HOPS  8

/* jobs waiting for their task to pick them up (task_spawn() passes no argument) */
#define QLEN 8
static webfetch_t *queue[QLEN];

static int parse_ip(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t n = 0;
        int d = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (uint32_t)(*s++ - '0'); if (++d > 3) return -1; }
        if (n > 255) return -1;
        v |= n << (8 * part);
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

/* appends src to the string in dst (cap bytes in all), cutting it short if need be */
static void cat(char *dst, const char *src, int cap)
{
    int l = (int)strlen(dst);
    while (*src && l < cap - 1) dst[l++] = *src++;
    dst[l] = 0;
}

static void fail(webfetch_t *f, const char *a, const char *b)
{
    strncpy(f->error, a, sizeof(f->error) - 1);
    if (b) {
        int l = (int)strlen(f->error);
        strncpy(f->error + l, b, sizeof(f->error) - 1 - l);
    }
    f->error[sizeof(f->error) - 1] = 0;
}

/* "http(s)://host[:port]/path" -> its parts; 0 = fine */
static int split(const char *url, int *tls, char *host, int hcap, uint16_t *port, char *path, int pcap)
{
    if (!strncmp(url, "https://", 8)) { *tls = 1; url += 8; }
    else if (!strncmp(url, "http://", 7)) { *tls = 0; url += 7; }
    else return -1;
    int i = 0;
    while (*url && *url != '/' && *url != ':' && *url != '?' && *url != '#' && i < hcap - 1) host[i++] = *url++;
    host[i] = 0;
    if (!i) return -1;
    *port = *tls ? 443 : 80;
    if (*url == ':') {
        url++;
        uint32_t v = 0;
        int d = 0;
        while (*url >= '0' && *url <= '9') { v = v * 10 + (uint32_t)(*url++ - '0'); if (++d > 5) return -1; }
        if (!d || !v || v > 65535) return -1;
        *port = (uint16_t)v;
    }
    int j = 0;
    if (*url != '/') path[j++] = '/';
    while (*url && *url != '#' && j < pcap - 1) path[j++] = *url++;
    path[j] = 0;
    return 0;
}

/* 1 = the page is in f, 0 = f->error says why not */
static int fetch(webfetch_t *f)
{
    char url[WEBFETCH_URL_LEN];
    strncpy(url, f->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;
    for (int hop = 0; hop <= WEBFETCH_HOPS; hop++) {
        int tls;
        char host[256], path[WEBFETCH_URL_LEN];
        uint16_t port;
        if (split(url, &tls, host, sizeof(host), &port, path, sizeof(path))) { fail(f, "Not a web address: ", url); return 0; }
        uint32_t ip;
        if (parse_ip(host, &ip)) {
            int rc = dns_resolve(host, &ip);
            if (rc) { fail(f, "Can't find the server ", host); return 0; }
        }
        if (!f->raw) f->raw = malloc(WEBFETCH_MAX + 1);
        if (!f->raw) { fail(f, "Not enough memory for the page", 0); return 0; }
        int n = tls ? https_get(ip, host, port, path, f->raw, WEBFETCH_MAX) : http_get(ip, host, port, path, f->raw, WEBFETCH_MAX);
        if (n < 0) { fail(f, "Couldn't connect: ", tls ? tls_connect_strerror(n) : http_strerror(n)); return 0; }
        if (n < 12 || strncmp((const char *)f->raw, "HTTP/1.", 7)) { fail(f, "The server sent no web page", 0); return 0; }
        f->raw[n] = 0;
        f->status = (f->raw[9] - '0') * 100 + (f->raw[10] - '0') * 10 + (f->raw[11] - '0');
        strncpy(f->final_url, url, sizeof(f->final_url) - 1);
        f->final_url[sizeof(f->final_url) - 1] = 0;

        const char *loc = http_header_value((const char *)f->raw, n, "location:");
        if (f->status >= 300 && f->status <= 399 && loc && hop < WEBFETCH_HOPS) {
            char next[WEBFETCH_URL_LEN];
            int l = 0;
            while (loc[l] && loc[l] != '\r' && loc[l] != '\n' && l < (int)sizeof(next) - 1) { next[l] = loc[l]; l++; }
            next[l] = 0;
            if (!strncmp(next, "http://", 7) || !strncmp(next, "https://", 8)) {
                strcpy(url, next);
            } else if (next[0] == '/' && next[1] == '/') {                     /* //host/path */
                strcpy(url, tls ? "https:" : "http:");
                cat(url, next, sizeof(url));
            } else {                                                          /* a path on the same server */
                char base[WEBFETCH_URL_LEN];
                int k = 0;
                const char *sch = tls ? "https://" : "http://";
                while (*sch) base[k++] = *sch++;
                for (int h = 0; host[h] && k < (int)sizeof(base) - 8; h++) base[k++] = host[h];
                if (port != (tls ? 443 : 80)) {
                    char pb[6];
                    int pl = 0;
                    uint32_t pv = port;
                    while (pv) { pb[pl++] = (char)('0' + pv % 10); pv /= 10; }
                    base[k++] = ':';
                    while (pl) base[k++] = pb[--pl];
                }
                base[k] = 0;
                if (next[0] != '/') {                                          /* relative to the current directory */
                    char dir[WEBFETCH_URL_LEN];
                    strcpy(dir, path);
                    char *q = dir;
                    while (*q && *q != '?') q++;
                    *q = 0;
                    char *slash = dir;
                    for (char *c = dir; *c; c++) if (*c == '/') slash = c;
                    slash[1] = 0;
                    cat(base, dir, sizeof(base));
                }
                cat(base, next, sizeof(base));
                strcpy(url, base);
            }
            continue;
        }

        int hdr = 0;
        for (int k = 0; k + 3 < n; k++)
            if (f->raw[k] == '\r' && f->raw[k + 1] == '\n' && f->raw[k + 2] == '\r' && f->raw[k + 3] == '\n') { hdr = k + 4; break; }
        f->body = f->raw + hdr;
        f->body_len = (uint32_t)(n - hdr);
        const char *ct = http_header_value((const char *)f->raw, hdr, "content-type:");
        int c = 0;
        if (ct)
            while (ct[c] && ct[c] != ';' && ct[c] != '\r' && ct[c] != '\n' && c < (int)sizeof(f->ctype) - 1) {
                char ch = ct[c];
                f->ctype[c++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
            }
        f->ctype[c] = 0;
        return 1;
    }
    fail(f, "Too many redirects", 0);
    return 0;
}

static void worker(void)
{
    webfetch_t *f = 0;
    uint32_t fl;
    asm volatile("pushfl; popl %0; cli" : "=r"(fl));
    for (int i = 0; i < QLEN && !f; i++) if (queue[i]) { f = queue[i]; queue[i] = 0; }
    asm volatile("pushl %0; popfl" : : "r"(fl));
    if (f) {
        int ok = fetch(f);
        /* the result and "is anyone still waiting" together, so webfetch_free() cannot slip in between */
        asm volatile("pushfl; popl %0; cli" : "=r"(fl));
        int gone = f->abandoned;
        f->state = ok ? WEBFETCH_DONE : WEBFETCH_FAILED;
        asm volatile("pushl %0; popfl" : : "r"(fl));
        if (gone) { free(f->raw); free(f); }
    }
    task_exit();
}

webfetch_t *webfetch_start(const char *url)
{
    webfetch_t *f = malloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    strncpy(f->url, url, sizeof(f->url) - 1);
    int slot = -1;
    uint32_t fl;
    asm volatile("pushfl; popl %0; cli" : "=r"(fl));
    for (int i = 0; i < QLEN; i++) if (!queue[i]) { queue[i] = f; slot = i; break; }
    asm volatile("pushl %0; popfl" : : "r"(fl));
    if (slot < 0 || task_spawn(worker, "webfetch") < 0) {
        if (slot >= 0) queue[slot] = 0;
        free(f);
        return 0;
    }
    return f;
}

void webfetch_free(webfetch_t *f)
{
    if (!f) return;
    uint32_t fl;
    asm volatile("pushfl; popl %0; cli" : "=r"(fl));
    int running = f->state == WEBFETCH_RUNNING;
    if (running) f->abandoned = 1;
    asm volatile("pushl %0; popfl" : : "r"(fl));
    if (!running) { free(f->raw); free(f); }
}
