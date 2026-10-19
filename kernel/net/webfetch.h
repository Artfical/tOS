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
} webfetch_t;

/* starts fetching url (http:// or https://); NULL when out of memory or tasks */
webfetch_t *webfetch_start(const char *url);
/* gives it back; a fetch still running is left to finish and free itself */
void webfetch_free(webfetch_t *f);

#endif
