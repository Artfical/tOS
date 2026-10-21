/*
 * tFire's memory allocator. A page makes hundreds of thousands of small allocations (the document's nodes, its
 * styles, the JavaScript heap), and the kernel's malloc walks one list of every block for each of them. Small
 * sizes come here from free lists of their own size class instead, carved out of big chunks of kernel memory;
 * big ones go to the kernel's malloc. Every block starts with a header saying which kind it is, so tf_free can
 * also be handed memory the kernel allocated (a fetched page, a decoded picture) and give it back there.
 *
 * Only the desktop's task runs tFire's engine, so there is no locking.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <memory.h>

#define MAGIC_SMALL 0x7F1EA110u
#define MAGIC_BIG   0x7F1EB16Bu
#define HDR 8                              /* keeps blocks 8-byte aligned */
#define CHUNK (512 * 1024)
#define NCLASS 64                          /* 8 bytes to about 70 KB */

typedef struct { uint32_t magic; uint32_t size; } hdr_t;    /* small: size = its class; big: the size asked for */
typedef struct free_blk { struct free_blk *next; } free_blk;

/* class sizes (what the caller can use): 8 bytes to about 70 KB, about 12% apart */
static uint32_t class_size[NCLASS];
static free_blk *free_list[NCLASS];
static uint8_t *chunk_pos, *chunk_end;
static uint32_t live_small, live_big;     /* bytes handed out, for Activity */

static void init_classes(void)
{
    uint32_t s = 8;
    for (int i = 0; i < NCLASS; i++) {
        class_size[i] = s;
        uint32_t n = s + (s >> 3);
        if (n < s + 8) n = s + 8;
        s = (n + 7) & ~7u;
    }
}

static int class_of(size_t n)
{
    if (!class_size[0]) init_classes();
    int lo = 0, hi = NCLASS - 1;
    if (n > class_size[hi]) return -1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (class_size[mid] >= n) hi = mid; else lo = mid + 1;
    }
    return lo;
}

void *tf_malloc(size_t n)
{
    if (n == 0) n = 1;
    int c = class_of(n);
    if (c < 0) {
        hdr_t *h = malloc(n + HDR);
        if (!h) return 0;
        h->magic = MAGIC_BIG;
        h->size = (uint32_t)n;
        live_big += (uint32_t)n;
        return (uint8_t *)h + HDR;
    }
    hdr_t *h;
    if (free_list[c]) {
        h = (hdr_t *)free_list[c];
        free_list[c] = free_list[c]->next;
    } else {
        uint32_t need = class_size[c] + HDR;
        if (!chunk_pos || chunk_pos + need > chunk_end) {
            /* what is left of the old chunk goes to the free lists, the biggest blocks that fit */
            while (chunk_pos && chunk_end - chunk_pos >= (int)(class_size[0] + HDR)) {
                int k = NCLASS - 1;
                while (k > 0 && class_size[k] + HDR > (uint32_t)(chunk_end - chunk_pos)) k--;
                hdr_t *r = (hdr_t *)chunk_pos;
                r->magic = MAGIC_SMALL;
                r->size = (uint32_t)k;
                free_blk *fb = (free_blk *)r;
                fb->next = free_list[k];
                free_list[k] = fb;
                chunk_pos += class_size[k] + HDR;
            }
            chunk_pos = malloc(CHUNK);
            if (!chunk_pos) { chunk_end = 0; return 0; }
            chunk_end = chunk_pos + CHUNK;
        }
        h = (hdr_t *)chunk_pos;
        chunk_pos += need;
    }
    h->magic = MAGIC_SMALL;
    h->size = (uint32_t)c;
    live_small += class_size[c];
    return (uint8_t *)h + HDR;
}

/* how much of a block can be used; 0 = not one of ours */
size_t tf_usable(const void *p)
{
    if (!p) return 0;
    const hdr_t *h = (const hdr_t *)((const uint8_t *)p - HDR);
    if (h->magic == MAGIC_SMALL && h->size < NCLASS) return class_size[h->size];
    if (h->magic == MAGIC_BIG) return h->size;
    return 0;
}

void tf_free(void *p)
{
    if (!p) return;
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic == MAGIC_SMALL && h->size < NCLASS) {
        int c = (int)h->size;
        live_small -= class_size[c];
        free_blk *fb = (free_blk *)h;          /* (the magic is overwritten: a second free is not taken for ours) */
        fb->next = free_list[c];
        free_list[c] = fb;
        return;
    }
    if (h->magic == MAGIC_BIG) {
        live_big -= h->size;
        h->magic = 0;
        free(h);
        return;
    }
    free(p);                                   /* the kernel's own */
}

void *tf_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return 0;
    void *p = tf_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *tf_realloc(void *p, size_t n)
{
    if (!p) return tf_malloc(n);
    if (!n) { tf_free(p); return 0; }
    size_t have = tf_usable(p);
    if (!have) {                               /* the kernel's own block */
        void *q = krealloc(p, n);
        return q;
    }
    if (n <= have && (have <= 64 || n >= have / 2)) return p;
    void *q = tf_malloc(n);
    if (!q) return 0;
    memcpy(q, p, have < n ? have : n);
    tf_free(p);
    return q;
}

char *tf_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = tf_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void tf_alloc_usage(uint32_t *small, uint32_t *big) { *small = live_small; *big = live_big; }
