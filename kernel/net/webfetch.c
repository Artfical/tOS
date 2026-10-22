#include "webfetch.h"
#include "http.h"
#include "https.h"
#include "tls.h"
#include "dns.h"
#include "scheduler.h"
#include "memory.h"
#include "string.h"

#define WEBFETCH_MAX   (24 * 1024 * 1024)      /* (a download is kept whole too) */
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

/* ------------------------------------------------------------------ cookies */

#define COOKIES 400
typedef struct {
    char name[64];
    char domain[128];                  /* lower case, without a leading dot */
    char path[128];
    char *value;                       /* malloc'd */
    uint8_t host_only, secure, http_only;
} cookie_t;
static cookie_t jar[COOKIES];
static int njar;

static uint32_t jar_lock(void) { uint32_t fl; asm volatile("pushfl; popl %0; cli" : "=r"(fl)); return fl; }
static void jar_unlock(uint32_t fl) { asm volatile("pushl %0; popfl" : : "r"(fl)); }

static int lcase(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* url -> its host (lower case), path and whether it is https */
static void url_parts(const char *url, char *host, int hcap, char *path, int pcap, int *secure)
{
    *secure = !strncmp(url, "https://", 8);
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && i < hcap - 1) host[i++] = (char)lcase(*p++);
    host[i] = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    int j = 0;
    if (*p != '/') path[j++] = '/';
    while (*p && *p != '?' && *p != '#' && j < pcap - 1) path[j++] = *p++;
    path[j] = 0;
}

static int domain_match(const char *host, const cookie_t *c)
{
    if (!strcmp(host, c->domain)) return 1;
    if (c->host_only) return 0;
    int hl = (int)strlen(host), dl = (int)strlen(c->domain);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, c->domain);
}

static int path_match(const char *path, const char *cp)
{
    int l = (int)strlen(cp);
    if (strncmp(path, cp, (size_t)l)) return 0;
    return path[l] == 0 || path[l] == '/' || (l && cp[l - 1] == '/');
}

/* the value of attribute name in a Set-Cookie line's attributes [a, e), copied to out; 1 = there */
static int cookie_attr(const char *a, const char *e, const char *name, char *out, int cap)
{
    int nl = (int)strlen(name);
    const char *p = a;
    while (p < e) {
        while (p < e && (*p == ';' || *p == ' ')) p++;
        const char *s = p;
        while (p < e && *p != ';') p++;
        int k = 0;
        while (k < nl && s + k < p && lcase(s[k]) == name[k]) k++;
        if (k == nl && (s + k == p || s[k] == '=' || s[k] == ' ')) {
            const char *v = s + k;
            while (v < p && (*v == ' ' || *v == '=')) v++;
            int o = 0;
            while (v < p && o < cap - 1) out[o++] = *v++;
            while (o && out[o - 1] == ' ') o--;
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

void webfetch_cookie_set(const char *url, const char *line, int for_script)
{
    char host[128], path[128], tmp[128];
    int secure;
    url_parts(url, host, sizeof(host), path, sizeof(path), &secure);
    const char *e = line;
    while (*e && *e != '\r' && *e != '\n') e++;
    while (*line == ' ') line++;
    const char *eq = line;
    while (eq < e && *eq != '=' && *eq != ';') eq++;
    if (eq >= e || *eq != '=') return;
    cookie_t c;
    memset(&c, 0, sizeof(c));
    int nl = (int)(eq - line);
    while (nl && line[nl - 1] == ' ') nl--;
    if (nl <= 0 || nl >= (int)sizeof(c.name)) return;
    memcpy(c.name, line, (size_t)nl);
    const char *v = eq + 1, *ve = v;
    while (ve < e && *ve != ';') ve++;
    if (ve - v > 4000) return;
    int remove = 0;
    if (cookie_attr(ve, e, "max-age", tmp, sizeof(tmp)) && (tmp[0] == '-' || (tmp[0] == '0' && !tmp[1]))) remove = 1;
    if (cookie_attr(ve, e, "expires", tmp, sizeof(tmp)) && (strstr(tmp, "1970") || strstr(tmp, "1969"))) remove = 1;
    c.http_only = (uint8_t)cookie_attr(ve, e, "httponly", tmp, sizeof(tmp));
    c.secure = (uint8_t)cookie_attr(ve, e, "secure", tmp, sizeof(tmp));
    if (for_script && c.http_only) return;
    if (cookie_attr(ve, e, "domain", tmp, sizeof(tmp)) && tmp[0]) {
        const char *d = tmp[0] == '.' ? tmp + 1 : tmp;
        int i = 0;
        for (; d[i] && i < (int)sizeof(c.domain) - 1; i++) c.domain[i] = (char)lcase(d[i]);
        c.domain[i] = 0;
        cookie_t probe = c;
        if (!domain_match(host, &probe) || !strchr(c.domain, '.')) return;    /* not for another site, not for a whole TLD */
    } else {
        strcpy(c.domain, host);
        c.host_only = 1;
    }
    if (cookie_attr(ve, e, "path", tmp, sizeof(tmp)) && tmp[0] == '/') strcpy(c.path, tmp);
    else {
        strcpy(c.path, path);
        char *sl = 0;
        for (char *q = c.path; *q; q++) if (*q == '/') sl = q;
        if (sl && sl != c.path) *sl = 0; else strcpy(c.path, "/");
    }
    char *val = remove ? 0 : malloc((size_t)(ve - v) + 1);
    if (val) { memcpy(val, v, (size_t)(ve - v)); val[ve - v] = 0; }
    if (!remove && !val) return;
    char *old = 0;
    uint32_t fl = jar_lock();
    int i;
    for (i = 0; i < njar; i++)
        if (!strcmp(jar[i].name, c.name) && !strcmp(jar[i].domain, c.domain) && !strcmp(jar[i].path, c.path)) break;
    if (i < njar) {
        if (for_script && jar[i].http_only) { jar_unlock(fl); free(val); return; }
        old = jar[i].value;
        if (remove) { jar[i] = jar[--njar]; }
        else { c.value = val; jar[i] = c; }
    } else if (!remove) {
        if (njar == COOKIES) { old = jar[0].value; memmove(&jar[0], &jar[1], sizeof(cookie_t) * (COOKIES - 1)); njar--; }
        c.value = val;
        jar[njar++] = c;
    }
    jar_unlock(fl);
    free(old);
}

int webfetch_cookie_header(const char *url, char *out, int cap, int for_script)
{
    char host[128], path[WEBFETCH_URL_LEN];
    int secure, o = 0;
    url_parts(url, host, sizeof(host), path, sizeof(path), &secure);
    out[0] = 0;
    uint32_t fl = jar_lock();
    for (int i = 0; i < njar; i++) {
        cookie_t *c = &jar[i];
        if (!domain_match(host, c) || !path_match(path, c->path) || (c->secure && !secure) || (for_script && c->http_only)) continue;
        int need = (int)strlen(c->name) + 1 + (int)strlen(c->value) + 2;
        if (o + need >= cap) break;
        if (o) { out[o++] = ';'; out[o++] = ' '; }
        strcpy(out + o, c->name); o += (int)strlen(c->name);
        out[o++] = '=';
        strcpy(out + o, c->value); o += (int)strlen(c->value);
        out[o] = 0;
    }
    jar_unlock(fl);
    return o;
}

/* every Set-Cookie in a response's header block [r, r + hlen) goes in the jar */
static void take_cookies(const char *url, const uint8_t *r, int hlen)
{
    for (int i = 0; i + 11 < hlen; i++) {
        if (i && r[i - 1] != '\n') continue;
        static const char sc[] = "set-cookie:";
        int k = 0;
        while (k < 11 && lcase(r[i + k]) == sc[k]) k++;
        if (k < 11) continue;
        webfetch_cookie_set(url, (const char *)r + i + 11, 0);
    }
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
        /* the request's own header lines, and the cookies for this address */
        static char hdrs[6144];                 /* (filled and used only while holding http_lock) */
        http_lock();
        hdrs[0] = 0;
        if (f->headers) cat(hdrs, f->headers, sizeof(hdrs) - 1100);
        static char ck[4096];
        if (webfetch_cookie_header(url, ck, sizeof(ck), 0)) { cat(hdrs, "Cookie: ", sizeof(hdrs)); cat(hdrs, ck, sizeof(hdrs) - 2); cat(hdrs, "\r\n", sizeof(hdrs)); }
        http_extra_t x = { f->method[0] ? f->method : 0, hdrs, f->req_body_len ? f->req_body : 0, f->req_body_len };
        http_extra = &x;
        int n = tls ? https_get(ip, host, port, path, f->raw, WEBFETCH_MAX) : http_get(ip, host, port, path, f->raw, WEBFETCH_MAX);
        http_extra = 0;
        http_unlock();
        if (n < 0) { fail(f, "Couldn't connect: ", tls ? tls_connect_strerror(n) : http_strerror(n)); return 0; }
        if (n < 12 || strncmp((const char *)f->raw, "HTTP/1.", 7)) { fail(f, "The server sent no web page", 0); return 0; }
        f->raw[n] = 0;
        f->status = (f->raw[9] - '0') * 100 + (f->raw[10] - '0') * 10 + (f->raw[11] - '0');
        strncpy(f->final_url, url, sizeof(f->final_url) - 1);
        f->final_url[sizeof(f->final_url) - 1] = 0;

        {
            int he = 0;
            for (int k = 0; k + 3 < n; k++)
                if (f->raw[k] == '\r' && f->raw[k + 1] == '\n' && f->raw[k + 2] == '\r' && f->raw[k + 3] == '\n') { he = k + 4; break; }
            take_cookies(url, f->raw, he ? he : n);
        }
        const char *loc = http_header_value((const char *)f->raw, n, "location:");
        if (f->status >= 300 && f->status <= 399 && loc && hop < WEBFETCH_HOPS) {
            if (f->status != 307 && f->status != 308) {      /* a form sent with POST: the answer is fetched with GET */
                f->method[0] = 0;
                f->req_body_len = 0;
            }
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
        f->head_len = (uint32_t)hdr;
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
        if (gone) { free(f->raw); free(f->headers); free(f->req_body); free(f); }
    }
    task_exit();
}

webfetch_t *webfetch_start(const char *url) { return webfetch_request(url, 0, 0, 0, 0); }

webfetch_t *webfetch_request(const char *url, const char *method, const char *headers, const void *body, int body_len)
{
    webfetch_t *f = malloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    strncpy(f->url, url, sizeof(f->url) - 1);
    if (method && strcmp(method, "GET")) {
        int i = 0;
        for (; method[i] && i < (int)sizeof(f->method) - 1; i++) f->method[i] = method[i] >= 'a' && method[i] <= 'z' ? (char)(method[i] - 32) : method[i];
        f->method[i] = 0;
    }
    if (headers && *headers) {
        f->headers = malloc(strlen(headers) + 1);
        if (!f->headers) { free(f); return 0; }
        strcpy(f->headers, headers);
    }
    if (body && body_len > 0) {
        f->req_body = malloc((size_t)body_len);
        if (!f->req_body) { free(f->headers); free(f); return 0; }
        memcpy(f->req_body, body, (size_t)body_len);
        f->req_body_len = body_len;
    }
    int slot = -1;
    uint32_t fl;
    asm volatile("pushfl; popl %0; cli" : "=r"(fl));
    for (int i = 0; i < QLEN; i++) if (!queue[i]) { queue[i] = f; slot = i; break; }
    asm volatile("pushl %0; popfl" : : "r"(fl));
    if (slot < 0 || task_spawn(worker, "webfetch") < 0) {
        if (slot >= 0) queue[slot] = 0;
        free(f->headers); free(f->req_body); free(f);
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
    if (!running) { free(f->raw); free(f->headers); free(f->req_body); free(f); }
}
