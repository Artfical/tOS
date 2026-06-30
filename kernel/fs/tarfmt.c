/* From-scratch ustar (POSIX tar) reader/writer. No compression — tar
 * itself never compressed anything either; that's what piping through
 * gzip was always for. Field layout and the checksum algorithm follow
 * the ustar header format directly (same header tOS already had to
 * parse once, for the boot initrd — see ramfs_import_initrd() in
 * ramfs.c, which this mirrors for the header fields that matter). */

#include "tarfmt.h"
#include "fsbridge.h"
#include "string.h"
#include "memory.h"

#define TAR_BLOCK 512
#define TAR_NAME_MAX 100
#define COPY_CHUNK 4096

static void seterr(char *err, int err_len, const char *msg)
{
    if (err && err_len > 0) { strncpy(err, msg, err_len - 1); err[err_len - 1] = 0; }
}

static void put_octal(char *dst, int width, unsigned int value)
{
    for (int i = width - 2; i >= 0; i--) {
        dst[i] = (char)('0' + (value & 7));
        value >>= 3;
    }
    dst[width - 1] = 0;
}

static unsigned int get_octal(const char *s, int len)
{
    unsigned int v = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) | (unsigned int)(s[i] - '0');
    }
    return v;
}

static void write_header(uint8_t *blk, const char *name, unsigned int size, char typeflag)
{
    memset(blk, 0, TAR_BLOCK);
    strncpy((char *)blk, name, TAR_NAME_MAX - 1);
    put_octal((char *)blk + 100, 8, 0644);
    put_octal((char *)blk + 108, 8, 0);
    put_octal((char *)blk + 116, 8, 0);
    put_octal((char *)blk + 124, 12, size);
    put_octal((char *)blk + 136, 12, 0);
    memset(blk + 148, ' ', 8);
    blk[156] = typeflag;
    memcpy(blk + 257, "ustar\0" "00", 8);

    unsigned int sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) sum += blk[i];
    char chk[7];
    put_octal(chk, 7, sum);
    memcpy(blk + 148, chk, 6);
    blk[154] = 0;
    blk[155] = ' ';
}

static int join(char *out, int cap, const char *a, const char *b)
{
    int k = 0;
    while (a[k] && k < cap - 1) { out[k] = a[k]; k++; }
    if (k > 0 && out[k - 1] != '/' && k < cap - 1) out[k++] = '/';
    int j = 0;
    while (b[j] && k < cap - 1) out[k++] = b[j++];
    out[k] = 0;
    return k;
}

typedef struct {
    int (*emit)(void *ctx, const void *data, unsigned int len);
    void *ctx;
} sink_t;

static int file_sink(void *ctx, const void *data, unsigned int len)
{
    struct { const char *path; uint32_t off; } *s = ctx;
    if (fsbridge_write(s->path, data, len, s->off) < 0) return -1;
    s->off += len;
    return 0;
}

static int add_recursive(sink_t *sink, const char *fs_path, const char *arc_name)
{
    if (fsbridge_is_dir(fs_path)) {
        char dirname[TAR_NAME_MAX];
        int k = 0;
        while (arc_name[k] && k < TAR_NAME_MAX - 2) { dirname[k] = arc_name[k]; k++; }
        if (k == 0 || dirname[k - 1] != '/') dirname[k++] = '/';
        dirname[k] = 0;

        uint8_t hdr[TAR_BLOCK];
        write_header(hdr, dirname, 0, '5');
        if (sink->emit(sink->ctx, hdr, TAR_BLOCK) != 0) return -1;

        /* malloc, not a stack array: this function recurses once per
         * directory depth, and a few hundred bytes of stack per level
         * is fine, but a 128-entry vfs_entry_t array (~18KB) stacked
         * on top of itself at every nesting level blew straight
         * through the 32KB kernel task stack on a tree just two
         * levels deep — page-faulted into kernel panic. */
        vfs_entry_t *entries = (vfs_entry_t *)malloc(128 * sizeof(vfs_entry_t));
        if (!entries) return -1;
        int n = fsbridge_list(fs_path, entries, 128);
        for (int i = 0; i < n; i++) {
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
            char child_fs[512], child_arc[TAR_NAME_MAX];
            join(child_fs, sizeof(child_fs), fs_path, entries[i].name);
            join(child_arc, sizeof(child_arc), dirname, entries[i].name);
            if (add_recursive(sink, child_fs, child_arc) != 0) { free(entries); return -1; }
        }
        free(entries);
        return 0;
    }

    uint32_t size = fsbridge_size(fs_path);
    uint8_t hdr[TAR_BLOCK];
    write_header(hdr, arc_name, size, '0');
    if (sink->emit(sink->ctx, hdr, TAR_BLOCK) != 0) return -1;

    uint8_t *chunk = (uint8_t *)malloc(COPY_CHUNK);
    if (!chunk) return -1;
    uint32_t off = 0;
    while (off < size) {
        uint32_t n = size - off;
        if (n > COPY_CHUNK) n = COPY_CHUNK;
        if (fsbridge_read(fs_path, chunk, n, off) < 0) { free(chunk); return -1; }
        if (sink->emit(sink->ctx, chunk, n) != 0) { free(chunk); return -1; }
        off += n;
    }
    free(chunk);
    uint32_t pad = (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK;
    if (pad) {
        uint8_t zero[TAR_BLOCK];
        memset(zero, 0, pad);
        if (sink->emit(sink->ctx, zero, pad) != 0) return -1;
    }
    return 0;
}

int tar_create(const char *archive, const char **paths, int npaths, char *err, int err_len)
{
    if (fsbridge_exists(archive)) fsbridge_delete(archive);
    if (fsbridge_create(archive) != 0) { seterr(err, err_len, "cannot create archive"); return -1; }

    struct { const char *path; uint32_t off; } s = { archive, 0 };
    sink_t sink = { file_sink, &s };

    for (int i = 0; i < npaths; i++) {
        const char *p = paths[i];
        const char *base = p;
        for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
        if (!fsbridge_exists(p)) { seterr(err, err_len, "no such file or directory"); return -1; }
        if (add_recursive(&sink, p, base) != 0) { seterr(err, err_len, "write failed"); return -1; }
    }

    uint8_t zero[TAR_BLOCK * 2];
    memset(zero, 0, sizeof(zero));
    if (file_sink(&s, zero, sizeof(zero)) != 0) { seterr(err, err_len, "write failed"); return -1; }
    return 0;
}

static int ensure_parent_dirs(const char *path)
{
    char dir[512];
    int k = 0;
    int last_slash = -1;
    while (path[k] && k < (int)sizeof(dir) - 1) { dir[k] = path[k]; if (path[k] == '/') last_slash = k; k++; }
    dir[k] = 0;
    if (last_slash < 0) return 0;
    dir[last_slash] = 0;
    if (dir[0] == 0 || fsbridge_exists(dir)) return 0;

    char partial[512];
    int pk = 0;
    for (int i = 0; i <= last_slash; i++) {
        if (path[i] == '/' && pk > 0) {
            partial[pk] = 0;
            if (!fsbridge_exists(partial)) fsbridge_mkdir(partial);
        }
        if (i < last_slash) partial[pk++] = path[i];
    }
    return 0;
}

int tar_extract(const char *archive, const char *dest_dir, char *err, int err_len)
{
    uint32_t size = fsbridge_size(archive);
    uint8_t hdr[TAR_BLOCK];
    uint32_t pos = 0;
    int count = 0;

    while (pos + TAR_BLOCK <= size) {
        if (fsbridge_read(archive, hdr, TAR_BLOCK, pos) < 0) break;
        if (hdr[0] == 0) break;
        pos += TAR_BLOCK;

        char name[TAR_NAME_MAX + 1];
        memcpy(name, hdr, TAR_NAME_MAX);
        name[TAR_NAME_MAX] = 0;
        unsigned int fsize = get_octal((const char *)hdr + 124, 11);
        char typeflag = (char)hdr[156];

        char full[600];
        join(full, sizeof(full), dest_dir, name);
        int flen = (int)strlen(full);
        int is_dir = (typeflag == '5') || (flen > 0 && full[flen - 1] == '/');
        if (is_dir && flen > 0 && full[flen - 1] == '/') full[flen - 1] = 0;

        if (is_dir) {
            if (!fsbridge_exists(full)) fsbridge_mkdir(full);
        } else {
            ensure_parent_dirs(full);
            if (fsbridge_exists(full)) fsbridge_delete(full);
            fsbridge_create(full);
            uint8_t chunk[COPY_CHUNK];
            uint32_t off = 0;
            while (off < fsize) {
                uint32_t n = fsize - off;
                if (n > COPY_CHUNK) n = COPY_CHUNK;
                if (fsbridge_read(archive, chunk, n, pos + off) < 0) break;
                fsbridge_write(full, chunk, n, off);
                off += n;
            }
        }
        count++;

        uint32_t data_blocks = (fsize + TAR_BLOCK - 1) / TAR_BLOCK;
        pos += data_blocks * TAR_BLOCK;
    }
    if (count == 0) { seterr(err, err_len, "empty or invalid tar archive"); return -1; }
    return count;
}

int tar_list(const char *archive, void (*cb)(const char *name, int is_dir, unsigned int size), char *err, int err_len)
{
    uint32_t size = fsbridge_size(archive);
    uint8_t hdr[TAR_BLOCK];
    uint32_t pos = 0;
    int count = 0;

    while (pos + TAR_BLOCK <= size) {
        if (fsbridge_read(archive, hdr, TAR_BLOCK, pos) < 0) break;
        if (hdr[0] == 0) break;
        pos += TAR_BLOCK;

        char name[TAR_NAME_MAX + 1];
        memcpy(name, hdr, TAR_NAME_MAX);
        name[TAR_NAME_MAX] = 0;
        unsigned int fsize = get_octal((const char *)hdr + 124, 11);
        char typeflag = (char)hdr[156];
        int nlen = (int)strlen(name);
        int is_dir = (typeflag == '5') || (nlen > 0 && name[nlen - 1] == '/');

        if (cb) cb(name, is_dir, fsize);
        count++;

        uint32_t data_blocks = (fsize + TAR_BLOCK - 1) / TAR_BLOCK;
        pos += data_blocks * TAR_BLOCK;
    }
    if (count == 0) { seterr(err, err_len, "empty or invalid tar archive"); return -1; }
    return count;
}
