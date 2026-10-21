#ifndef WEBFETCH_H
#define WEBFETCH_H

#include <stdint.h>

/*
 * Fetching a web page in the background (for tFire): a task of its own resolves the name, connects (TLS for
 * https), follows redirects and keeps the body, while the desktop goes on drawing. Poll state until it is not
 * WEBFETCH_RUNNING.
 */

#define WEBFETCH_RUNNING 0
#define WEBFETCH_DONE    1
#define WEBFETCH_FAILED  2

#define WEBFETCH_URL_LEN 512

typedef struct webfetch {
    volatile int state;
    int abandoned;                     /* the asker went away: the task frees it when it ends */
    char url[WEBFETCH_URL_LEN];        /* what was asked for */
    char final_url[WEBFETCH_URL_LEN];  /* where it ended up after redirects */
    int status;                        /* HTTP status code */
    char ctype[64];                    /* Content-Type, lower case, without parameters */
    char error[96];                    /* WEBFETCH_FAILED: why */
    uint8_t *raw;                      /* the whole response; body points into it */
    uint8_t *body;
    uint32_t body_len;
    uint32_t head_len;                 /* the response's header block: raw[0 .. head_len) */
    /* the request, beyond GET (webfetch_request) */
    char method[8];
    char *headers;                     /* extra header lines, each ending in \r\n (malloc'd) */
    uint8_t *req_body;                 /* (malloc'd) */
    int req_body_len;
} webfetch_t;

/* starts fetching url (http:// or https://); NULL when out of memory or tasks */
webfetch_t *webfetch_start(const char *url);
/* the same with a method, header lines of its own (each ending in \r\n, or NULL) and a body (or NULL) */
webfetch_t *webfetch_request(const char *url, const char *method, const char *headers, const void *body, int body_len);
/* gives it back; a fetch still running is left to finish and free itself */
void webfetch_free(webfetch_t *f);

/* The cookie jar, shared by every fetch: what servers set (Set-Cookie) is sent back with each request to
 * them. Scripts read and write it too (document.cookie): for_script leaves out HttpOnly cookies. */
void webfetch_cookie_set(const char *url, const char *set_cookie, int for_script);
/* the cookies for url as "a=1; b=2" into out (cap bytes); its length */
int webfetch_cookie_header(const char *url, char *out, int cap, int for_script);

#endif
