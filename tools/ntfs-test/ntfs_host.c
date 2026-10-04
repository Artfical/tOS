/* Host-side harness: compiles the kernel's ntfs.c against an image file and
 * drives it with a scripted workload or a model-checked random fuzz run.
 *   ntfs_host IMAGE script CMDS...   run commands (see below)
 *   ntfs_host IMAGE fuzz SEED NOPS   random operations verified against a model
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include "blockdev.h"
#include "vfs.h"

static blockdev_t g_bd;

int blockdev_read_bytes(blockdev_t *bd, uint64_t off, uint32_t len, void *buf)
{
    if (off + len > bd->total_sectors * bd->sector_size) return -1;
    return pread(bd->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

int blockdev_write_bytes(blockdev_t *bd, uint64_t off, uint32_t len, const void *buf)
{
    if (off + len > bd->total_sectors * bd->sector_size) return -1;
    if (bd->fail_after >= 0 && bd->writes >= bd->fail_after) return -1;
    bd->writes++;
    return pwrite(bd->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static vfs_ops_t *g_ops;
static void *g_ctx;
int vfs_mount(const char *p, vfs_ops_t *o, void *d) { (void)p; g_ops = o; g_ctx = d; return 0; }

static int g_trace;
static void trace_goto(int line) { if (g_trace) fprintf(stderr, "  [goto @ntfs.c:%d]\n", line); }
#define goto if (trace_goto(__LINE__), 1) goto
#include "ntfs.c"
#undef goto

static ntfs_t g_fs;

static int open_image(const char *path)
{
    memset(&g_bd, 0, sizeof(g_bd));
    g_bd.fd = open(path, O_RDWR);
    if (g_bd.fd < 0) { perror(path); return -1; }
    struct stat st;
    fstat(g_bd.fd, &st);
    g_bd.used = 1;
    g_bd.sector_size = 512;
    g_bd.total_sectors = (uint64_t)st.st_size / 512;
    g_bd.fail_after = -1;
    return 0;
}

static int do_mount(void)
{
    if (ntfs_probe_and_mount(&g_fs, &g_bd) != 0) { fprintf(stderr, "mount failed\n"); return -1; }
    ntfs_mount_vfs(&g_fs, "/x");
    if (!g_fs.rw) fprintf(stderr, "note: mounted read-only\n");
    return 0;
}

/* ---------- VFS convenience ---------- */

static int op_open(const char *path, int flags) { return g_ops->open(g_ctx, path, flags); }

static int write_file(const char *path, const void *data, size_t n, int flags, uint32_t offset)
{
    int fd = op_open(path, flags);
    if (fd < 0) return -1;
    if (offset) g_ops->lseek(g_ctx, fd, offset, VFS_SEEK_SET);
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done > 65536 ? 65536 : n - done;
        int w = g_ops->write(g_ctx, fd, (const uint8_t *)data + done, (uint32_t)chunk);
        if (w <= 0) { g_ops->close(g_ctx, fd); return -1; }
        done += (size_t)w;
    }
    g_ops->close(g_ctx, fd);
    return 0;
}

static ssize_t read_file(const char *path, uint8_t **out)
{
    int fd = op_open(path, 0);
    if (fd < 0) return -1;
    size_t cap = 4096, n = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; buf = (uint8_t *)realloc(buf, cap); }
        int r = g_ops->read(g_ctx, fd, buf + n, 4096);
        if (r < 0) { free(buf); g_ops->close(g_ctx, fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    g_ops->close(g_ctx, fd);
    *out = buf;
    return (ssize_t)n;
}

/* ---------- deterministic content ---------- */

static uint32_t rng_state = 1;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void fill_pattern(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 12345;
    for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345; p[i] = (uint8_t)(s >> 16); }
}

/* ---------- model ---------- */

typedef struct node {
    char name[300];
    int is_dir;
    uint8_t *data;
    size_t size;
    struct node *parent;
    struct node **kids;
    int nkids, capkids;
} node_t;

static node_t *root_node;

static node_t *node_new(const char *name, int is_dir, node_t *parent)
{
    node_t *n = (node_t *)calloc(1, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->is_dir = is_dir;
    n->parent = parent;
    if (parent) {
        if (parent->nkids == parent->capkids) {
            parent->capkids = parent->capkids ? parent->capkids * 2 : 8;
            parent->kids = (node_t **)realloc(parent->kids, parent->capkids * sizeof(node_t *));
        }
        parent->kids[parent->nkids++] = n;
    }
    return n;
}

static void node_unlink(node_t *n)
{
    node_t *p = n->parent;
    for (int i = 0; i < p->nkids; i++)
        if (p->kids[i] == n) { memmove(&p->kids[i], &p->kids[i + 1], (size_t)(p->nkids - i - 1) * sizeof(node_t *)); p->nkids--; break; }
}

static void node_path(node_t *n, char *out, size_t cap)
{
    if (!n->parent) { out[0] = 0; return; }
    char tmp[2048];
    node_path(n->parent, tmp, sizeof(tmp));
    snprintf(out, cap, "%s%s%s", tmp, tmp[0] ? "/" : "", n->name);
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static node_t *find_kid(node_t *d, const char *name)
{
    for (int i = 0; i < d->nkids; i++) if (ieq(d->kids[i]->name, name)) return d->kids[i];
    return 0;
}

static int is_under(node_t *n, node_t *anc)
{
    for (; n; n = n->parent) if (n == anc) return 1;
    return 0;
}

static void collect_nodes(node_t *n, node_t **out, int *cnt, int dirs_only)
{
    if (!dirs_only || n->is_dir) out[(*cnt)++] = n;
    for (int i = 0; i < n->nkids; i++) collect_nodes(n->kids[i], out, cnt, dirs_only);
}

static int verify_node(node_t *n)
{
    char path[2048];
    node_path(n, path, sizeof(path));
    int bad = 0;
    if (n->is_dir) {
        vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 4096);
        int cnt = g_ops->readdir(g_ctx, path, e, 4096);
        if (cnt < 0) { printf("VERIFY: readdir(%s) failed\n", path); free(e); return 1; }
        if (cnt != n->nkids) {
            printf("VERIFY: %s has %d entries, model %d\n", path, cnt, n->nkids);
            bad = 1;
        }
        for (int i = 0; i < n->nkids && !bad; i++) {
            int f = 0;
            for (int k = 0; k < cnt; k++) if (strcmp(e[k].name, n->kids[i]->name) == 0) { f = 1; if (e[k].is_dir != n->kids[i]->is_dir) { printf("VERIFY: %s/%s type mismatch\n", path, e[k].name); bad = 1; } }
            if (!f) { printf("VERIFY: %s missing entry %s\n", path, n->kids[i]->name); bad = 1; }
        }
        free(e);
        for (int i = 0; i < n->nkids && !bad; i++) bad |= verify_node(n->kids[i]);
    } else {
        uint8_t *buf = 0;
        ssize_t r = read_file(path, &buf);
        if (r < 0) { printf("VERIFY: read %s failed\n", path); return 1; }
        if ((size_t)r != n->size) { printf("VERIFY: %s size %zd, model %zu\n", path, r, n->size); bad = 1; }
        else if (n->size && memcmp(buf, n->data, n->size) != 0) {
            size_t k = 0; while (buf[k] == n->data[k]) k++;
            printf("VERIFY: %s content differs at byte %zu (size %zu)\n", path, k, n->size); bad = 1;
        }
        free(buf);
    }
    return bad;
}

/* ---------- name generation ---------- */

static void gen_name(char *out, size_t cap)
{
    static const char *tr[] = { "ğ", "ü", "ş", "ı", "ö", "ç", "é", "ñ" };
    int kind = (int)(rnd() % 10);
    int len;
    if (kind < 5) len = 1 + (int)(rnd() % 8);
    else if (kind < 8) len = 9 + (int)(rnd() % 40);
    else len = 60 + (int)(rnd() % 100);
    size_t n = 0;
    int uni = (rnd() % 6) == 0;
    for (int i = 0; i < len && n + 6 < cap; i++) {
        uint32_t r = rnd();
        if (uni && r % 7 == 0) { const char *u = tr[(r >> 4) % 8]; n += (size_t)snprintf(out + n, cap - n, "%s", u); }
        else if (r % 11 == 0) out[n++] = (char)('0' + (r >> 3) % 10);
        else if (r % 13 == 0) out[n++] = '_';
        else if (r % 5 == 0) out[n++] = (char)('A' + (r >> 3) % 26);
        else out[n++] = (char)('a' + (r >> 3) % 26);
    }
    if (n == 0) out[n++] = 'x';
    out[n] = 0;
    if (out[n - 1] == '.' || out[n - 1] == ' ') out[n - 1] = 'z';
}

static size_t gen_size(void)
{
    uint32_t r = rnd() % 100;
    if (r < 10) return 0;
    if (r < 45) return 1 + rnd() % 700;
    if (r < 80) return 1 + rnd() % 20000;
    if (r < 97) return 20000 + rnd() % 150000;
    return 150000 + rnd() % 600000;
}

/* ---------- fuzz ---------- */

static node_t *pick_node(int dirs_only)
{
    static node_t *all[200000];
    int cnt = 0;
    collect_nodes(root_node, all, &cnt, dirs_only);
    return cnt ? all[rnd() % (uint32_t)cnt] : 0;
}

static int g_verbose;
static int too_long(const char *dir, const char *name) { return strlen(dir) + strlen(name) + 2 > 118; }

#define FAIL(...) do { printf("FUZZ FAIL op#%d: ", opno); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static int fuzz_one(int opno)
{
    uint32_t pick = rnd() % 100;
    char path[2048], name[300], p2[2048];
    if (pick < 22) {                                  /* create file */
        node_t *d = pick_node(1);
        gen_name(name, sizeof(name));
        if (find_kid(d, name)) return 0;
        node_path(d, p2, sizeof(p2));
        if (too_long(p2, name)) return 0;
        snprintf(path, sizeof(path), "%s%s%s", p2, p2[0] ? "/" : "", name);
        size_t sz = gen_size();
        uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
        uint32_t seed = rnd();
        fill_pattern(buf, sz, seed);
        if (g_verbose) printf("op#%d create %s (%zu)\n", opno, path, sz);
        int rc = write_file(path, buf, sz, VFS_WRONLY | VFS_CREAT | VFS_TRUNC, 0);
        if (rc && g_fs.rw) { free(buf); FAIL("create %s size %zu failed", path, sz); }
        if (rc == 0) {
            if (sz == 0) { /* file creation with no write happens in open */ }
            node_t *n = node_new(name, 0, d);
            n->data = buf; n->size = sz;
        } else free(buf);
    } else if (pick < 30) {                           /* mkdir */
        node_t *d = pick_node(1);
        gen_name(name, sizeof(name));
        if (find_kid(d, name)) return 0;
        node_path(d, p2, sizeof(p2));
        if (too_long(p2, name)) return 0;
        snprintf(path, sizeof(path), "%s%s%s", p2, p2[0] ? "/" : "", name);
        if (g_verbose) printf("op#%d mkdir %s\n", opno, path);
        if (g_ops->mkdir(g_ctx, path, 0755) != 0) FAIL("mkdir %s failed", path);
        node_new(name, 1, d);
    } else if (pick < 48) {                           /* write at offset */
        node_t *n = pick_node(0);
        if (!n || n->is_dir) return 0;
        node_path(n, path, sizeof(path));
        size_t off = n->size ? rnd() % (n->size + 1) : 0;
        if (rnd() % 8 == 0) off += rnd() % 5000;
        size_t len = 1 + rnd() % 30000;
        uint8_t *buf = (uint8_t *)malloc(len);
        fill_pattern(buf, len, rnd());
        if (g_verbose) printf("op#%d write %s off=%zu len=%zu (size %zu)\n", opno, path, off, len, n->size);
        /* vfs lseek clamps to the current size, so extend with zeros first */
        if (off > n->size) {
            size_t gap = off - n->size;
            uint8_t *z = (uint8_t *)calloc(1, gap);
            if (write_file(path, z, gap, VFS_WRONLY | VFS_APPEND, 0)) { free(z); free(buf); FAIL("gap-extend %s failed", path); }
            free(z);
            n->data = (uint8_t *)realloc(n->data, off ? off : 1);
            memset(n->data + n->size, 0, gap);
            n->size = off;
        }
        if (write_file(path, buf, len, VFS_WRONLY, (uint32_t)off)) { free(buf); FAIL("write %s failed", path); }
        if (off + len > n->size) { n->data = (uint8_t *)realloc(n->data, off + len); n->size = off + len; }
        memcpy(n->data + off, buf, len);
        free(buf);
    } else if (pick < 56) {                           /* replace (truncate) */
        node_t *n = pick_node(0);
        if (!n || n->is_dir) return 0;
        node_path(n, path, sizeof(path));
        size_t sz = gen_size();
        uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
        fill_pattern(buf, sz, rnd());
        if (g_verbose) printf("op#%d replace %s (%zu -> %zu)\n", opno, path, n->size, sz);
        if (write_file(path, buf, sz, VFS_WRONLY | VFS_TRUNC, 0)) { free(buf); FAIL("replace %s failed", path); }
        free(n->data); n->data = buf; n->size = sz;
    } else if (pick < 68) {                           /* delete */
        node_t *n = pick_node(0);
        if (!n || !n->parent) return 0;
        node_path(n, path, sizeof(path));
        int expect_ok = !n->is_dir || n->nkids == 0;
        if (g_verbose) printf("op#%d remove %s%s\n", opno, path, expect_ok ? "" : " (expect failure)");
        int rc = g_ops->unlink(g_ctx, path);
        if (expect_ok && rc != 0) FAIL("unlink %s failed", path);
        if (!expect_ok && rc == 0) FAIL("unlink of non-empty dir %s succeeded", path);
        if (rc == 0) { node_unlink(n); free(n->data); free(n); }
    } else if (pick < 80) {                           /* rename */
        node_t *n = pick_node(0);
        if (!n || !n->parent) return 0;
        node_t *d = pick_node(1);
        if (n->is_dir && is_under(d, n)) return 0;
        int caseonly = rnd() % 6 == 0 && d == n->parent;
        if (caseonly) {
            snprintf(name, sizeof(name), "%s", n->name);
            for (char *c = name; *c; c++) if (isalpha((unsigned char)*c)) *c = (char)(rnd() % 2 ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
        } else gen_name(name, sizeof(name));
        node_t *clash = find_kid(d, name);
        if (clash && !(clash == n && caseonly)) return 0;
        node_path(n, path, sizeof(path));
        node_path(d, p2, sizeof(p2));
        if (too_long(p2, name)) return 0;
        char np[2048];
        snprintf(np, sizeof(np), "%s%s%s", p2, p2[0] ? "/" : "", name);
        if (g_verbose) printf("op#%d rename %s -> %s\n", opno, path, np);
        if (g_ops->rename(g_ctx, path, np) != 0) FAIL("rename %s -> %s failed", path, np);
        node_unlink(n);
        snprintf(n->name, sizeof(n->name), "%s", name);
        n->parent = d;
        if (d->nkids == d->capkids) { d->capkids = d->capkids ? d->capkids * 2 : 8; d->kids = (node_t **)realloc(d->kids, d->capkids * sizeof(node_t *)); }
        d->kids[d->nkids++] = n;
    } else if (pick < 90) {                           /* case-variant lookup */
        node_t *n = pick_node(0);
        if (!n || !n->parent) return 0;
        node_path(n, path, sizeof(path));
        for (char *c = path; *c; c++) if (isalpha((unsigned char)*c) && (rnd() & 1)) *c = (char)toupper((unsigned char)*c);
        vfs_entry_t e;
        if (g_ops->stat(g_ctx, path, &e) != 0) FAIL("case-variant stat %s failed", path);
        if (e.is_dir != n->is_dir) FAIL("case-variant stat %s wrong type", path);
    } else {                                          /* spot read */
        node_t *n = pick_node(0);
        if (!n || n->is_dir) return 0;
        node_path(n, path, sizeof(path));
        uint8_t *buf = 0;
        ssize_t r = read_file(path, &buf);
        if (r < 0 || (size_t)r != n->size || (n->size && memcmp(buf, n->data, n->size))) { free(buf); FAIL("spot read %s mismatch", path); }
        free(buf);
    }
    return 0;
}

static int fuzz(uint32_t seed, int nops)
{
    rng_state = seed;
    root_node = node_new("", 1, 0);
    int verify_every = 25;
    for (int i = 1; i <= nops; i++) {
        if (fuzz_one(i)) return 1;
        if (i % verify_every == 0) {
            if (verify_node(root_node)) { printf("FUZZ FAIL: model mismatch after op#%d\n", i); return 1; }
        }
    }
    if (verify_node(root_node)) { printf("FUZZ FAIL: final model mismatch\n"); return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s IMAGE fuzz SEED NOPS | script CMD...\n", argv[0]); return 2; }
    setvbuf(stdout, 0, _IONBF, 0);
    if (open_image(argv[1])) return 2;
    if (getenv("NTFS_VERBOSE")) g_verbose = 1;
    if (getenv("NTFS_TRACE")) g_trace = 1;
    if (do_mount()) return 2;
    int rc = 0;
    if (strcmp(argv[2], "fuzz") == 0 && argc >= 5) {
        rc = fuzz((uint32_t)atoi(argv[3]), atoi(argv[4]));
        printf("%s\n", rc ? "FUZZ: FAILED" : "FUZZ: ok");
    } else if (strcmp(argv[2], "resize") == 0) {
        ntfs_umount(&g_fs);
        char err[100] = "";
        rc = ntfs_resize(&g_bd, argc >= 4 ? strtoull(argv[3], 0, 10) : 0, err, sizeof(err));
        printf("resize: %s %s\n", rc ? "FAILED" : "ok", err);
        close(g_bd.fd);
        return rc != 0;
    }
    ntfs_umount(&g_fs);
    close(g_bd.fd);
    return rc;
}
