/* From-scratch ZIP reader/writer. Entries are written "stored"
 * (method 0, no compression) since tOS only carries a DEFLATE
 * *decoder* (originally built for reading PNG files — see
 * inflate_raw_buffer() in png.c), not an encoder; writing an encoder
 * just to shrink already-small files wasn't worth it. Extraction
 * understands both stored and deflated entries, so it can open real
 * ZIP files downloaded from elsewhere, not just ones tOS made. */

#include "zipfmt.h"
#include "fsbridge.h"
#include "png.h"
#include "string.h"
#include "memory.h"

#define COPY_CHUNK 4096
#define ZIP_NAME_MAX 256

#define SIG_LOCAL    0x04034b50u
#define SIG_CENTRAL  0x02014b50u
#define SIG_EOCD     0x06054b50u

static void seterr(char *err, int err_len, const char *msg)
{
    if (err && err_len > 0) { strncpy(err, msg, err_len - 1); err[err_len - 1] = 0; }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!crc_ready) crc_init();
    for (uint32_t i = 0; i < len; i++) crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
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

/* Same tar-slip-style issue as kernel/fs/tarfmt.c's identical helper:
 * a zip entry's name comes straight from the archive with no
 * sanitization, so an absolute path or a ".." component could escape
 * dest_dir during extraction and overwrite/delete arbitrary files. */
static int unsafe_entry_name(const char *name)
{
    if (name[0] == '/') return 1;
    int i = 0;
    while (name[i]) {
        int start = i;
        while (name[i] && name[i] != '/') i++;
        int len = i - start;
        if (len == 2 && name[start] == '.' && name[start + 1] == '.') return 1;
        if (name[i] == '/') i++;
    }
    return 0;
}

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
    int is_dir;
} zip_centry_t;

typedef struct {
    const char *archive;
    uint32_t offset;
    zip_centry_t *entries;
    int count;
    int cap;
} zip_writer_t;

static int writer_emit(zip_writer_t *w, const void *data, uint32_t len)
{
    if (fsbridge_write(w->archive, data, len, w->offset) < 0) return -1;
    w->offset += len;
    return 0;
}

static int writer_add_entry(zip_writer_t *w, const char *name, int is_dir)
{
    if (w->count >= w->cap) {
        int newcap = w->cap ? w->cap * 2 : 32;
        zip_centry_t *ne = (zip_centry_t *)krealloc(w->entries, (size_t)newcap * sizeof(zip_centry_t));
        if (!ne) return -1;
        w->entries = ne;
        w->cap = newcap;
    }
    zip_centry_t *e = &w->entries[w->count++];
    strncpy(e->name, name, ZIP_NAME_MAX - 1);
    e->name[ZIP_NAME_MAX - 1] = 0;
    e->crc = 0;
    e->size = 0;
    e->offset = w->offset;
    e->is_dir = is_dir;
    return 0;
}

static int add_recursive(zip_writer_t *w, const char *fs_path, const char *arc_name)
{
    if (fsbridge_is_dir(fs_path)) {
        char dirname[ZIP_NAME_MAX];
        int k = 0;
        while (arc_name[k] && k < ZIP_NAME_MAX - 2) { dirname[k] = arc_name[k]; k++; }
        if (k == 0 || dirname[k - 1] != '/') dirname[k++] = '/';
        dirname[k] = 0;

        if (writer_add_entry(w, dirname, 1) != 0) return -1;
        zip_centry_t *e = &w->entries[w->count - 1];
        uint16_t nlen = (uint16_t)strlen(dirname);
        uint8_t lh[30];
        memset(lh, 0, sizeof(lh));
        put32(lh + 0, SIG_LOCAL);
        put16(lh + 4, 20);
        put16(lh + 26, nlen);
        if (writer_emit(w, lh, sizeof(lh)) != 0) return -1;
        if (writer_emit(w, dirname, nlen) != 0) return -1;
        e->crc = 0;
        e->size = 0;

        /* malloc, not a stack array — see the identical comment in
         * tarfmt.c's add_recursive(): a 128-entry vfs_entry_t array
         * (~18KB) stacked at every recursion level overflowed the
         * 32KB kernel task stack on even a shallow directory tree. */
        vfs_entry_t *entries = (vfs_entry_t *)malloc(128 * sizeof(vfs_entry_t));
        if (!entries) return -1;
        int n = fsbridge_list(fs_path, entries, 128);
        for (int i = 0; i < n; i++) {
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
            char child_fs[512], child_arc[ZIP_NAME_MAX];
            join(child_fs, sizeof(child_fs), fs_path, entries[i].name);
            join(child_arc, sizeof(child_arc), dirname, entries[i].name);
            if (add_recursive(w, child_fs, child_arc) != 0) { free(entries); return -1; }
        }
        free(entries);
        return 0;
    }

    uint32_t size = fsbridge_size(fs_path);
    if (writer_add_entry(w, arc_name, 0) != 0) return -1;
    zip_centry_t *e = &w->entries[w->count - 1];

    uint8_t *chunk = (uint8_t *)malloc(COPY_CHUNK);
    if (!chunk) return -1;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t off = 0;
    while (off < size) {
        uint32_t n = size - off;
        if (n > COPY_CHUNK) n = COPY_CHUNK;
        if (fsbridge_read(fs_path, chunk, n, off) < 0) { free(chunk); return -1; }
        crc = crc_update(crc, chunk, n);
        off += n;
    }
    crc ^= 0xFFFFFFFFu;

    uint16_t nlen = (uint16_t)strlen(arc_name);
    uint8_t lh[30];
    memset(lh, 0, sizeof(lh));
    put32(lh + 0, SIG_LOCAL);
    put16(lh + 4, 20);
    put32(lh + 14, crc);
    put32(lh + 18, size);
    put32(lh + 22, size);
    put16(lh + 26, nlen);
    if (writer_emit(w, lh, sizeof(lh)) != 0) { free(chunk); return -1; }
    if (writer_emit(w, arc_name, nlen) != 0) { free(chunk); return -1; }

    off = 0;
    while (off < size) {
        uint32_t n = size - off;
        if (n > COPY_CHUNK) n = COPY_CHUNK;
        if (fsbridge_read(fs_path, chunk, n, off) < 0) { free(chunk); return -1; }
        if (writer_emit(w, chunk, n) != 0) { free(chunk); return -1; }
        off += n;
    }
    free(chunk);

    e->crc = crc;
    e->size = size;
    return 0;
}

int zip_create(const char *archive, const char **paths, int npaths, char *err, int err_len)
{
    if (fsbridge_exists(archive)) fsbridge_delete(archive);
    if (fsbridge_create(archive) != 0) { seterr(err, err_len, "cannot create archive"); return -1; }

    zip_writer_t w;
    memset(&w, 0, sizeof(w));
    w.archive = archive;

    for (int i = 0; i < npaths; i++) {
        const char *p = paths[i];
        const char *base = p;
        for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
        if (!fsbridge_exists(p)) { seterr(err, err_len, "no such file or directory"); free(w.entries); return -1; }
        if (add_recursive(&w, p, base) != 0) { seterr(err, err_len, "write failed"); free(w.entries); return -1; }
    }

    uint32_t cd_offset = w.offset;
    for (int i = 0; i < w.count; i++) {
        zip_centry_t *e = &w.entries[i];
        uint16_t nlen = (uint16_t)strlen(e->name);
        uint8_t ch[46];
        memset(ch, 0, sizeof(ch));
        put32(ch + 0, SIG_CENTRAL);
        put16(ch + 4, 20);
        put16(ch + 6, 20);
        put32(ch + 16, e->crc);
        put32(ch + 20, e->size);
        put32(ch + 24, e->size);
        put16(ch + 28, nlen);
        put32(ch + 42, e->offset);
        if (writer_emit(&w, ch, sizeof(ch)) != 0) { seterr(err, err_len, "write failed"); free(w.entries); return -1; }
        if (writer_emit(&w, e->name, nlen) != 0) { seterr(err, err_len, "write failed"); free(w.entries); return -1; }
    }
    uint32_t cd_size = w.offset - cd_offset;

    uint8_t eocd[22];
    memset(eocd, 0, sizeof(eocd));
    put32(eocd + 0, SIG_EOCD);
    put16(eocd + 8, (uint16_t)w.count);
    put16(eocd + 10, (uint16_t)w.count);
    put32(eocd + 12, cd_size);
    put32(eocd + 16, cd_offset);
    int rc = writer_emit(&w, eocd, sizeof(eocd));
    free(w.entries);
    if (rc != 0) { seterr(err, err_len, "write failed"); return -1; }
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

int zip_extract(const char *archive, const char *dest_dir, char *err, int err_len)
{
    uint32_t size = fsbridge_size(archive);
    uint32_t pos = 0;
    int count = 0;

    while (pos + 30 <= size) {
        uint8_t lh[30];
        if (fsbridge_read(archive, lh, 30, pos) < 0) break;
        if (get32(lh + 0) != SIG_LOCAL) break;

        uint16_t method = get16(lh + 8);
        uint32_t comp_size = get32(lh + 18);
        uint32_t uncomp_size = get32(lh + 22);
        uint16_t nlen = get16(lh + 26);
        uint16_t elen = get16(lh + 28);

        char name[ZIP_NAME_MAX];
        uint32_t name_off = pos + 30;
        uint32_t to_read = nlen < ZIP_NAME_MAX - 1 ? nlen : ZIP_NAME_MAX - 1;
        if (fsbridge_read(archive, name, to_read, name_off) < 0) break;
        name[to_read] = 0;

        uint32_t data_off = pos + 30 + nlen + elen;
        int nl = (int)strlen(name);
        int is_dir = (nl > 0 && name[nl - 1] == '/');

        if (unsafe_entry_name(name)) {
            /* Skip this entry (still advance pos below) -- see
             * unsafe_entry_name()'s comment. */
        } else {
            char full[600];
            join(full, sizeof(full), dest_dir, name);
            int flen = (int)strlen(full);
            if (is_dir && flen > 0 && full[flen - 1] == '/') full[flen - 1] = 0;

            if (is_dir) {
                ensure_parent_dirs(full);
                if (!fsbridge_exists(full)) fsbridge_mkdir(full);
            } else if (method == 0) {
                ensure_parent_dirs(full);
                if (fsbridge_exists(full)) fsbridge_delete(full);
                fsbridge_create(full);
                uint8_t chunk[COPY_CHUNK];
                uint32_t off = 0;
                while (off < comp_size) {
                    uint32_t n = comp_size - off;
                    if (n > COPY_CHUNK) n = COPY_CHUNK;
                    if (fsbridge_read(archive, chunk, n, data_off + off) < 0) break;
                    fsbridge_write(full, chunk, n, off);
                    off += n;
                }
            } else if (method == 8) {
                uint8_t *src = (uint8_t *)malloc(comp_size ? comp_size : 1);
                uint8_t *dst = (uint8_t *)malloc(uncomp_size ? uncomp_size : 1);
                if (src && dst && fsbridge_read(archive, src, comp_size, data_off) >= 0) {
                    uint32_t out_len = 0;
                    if (inflate_raw_buffer(src, comp_size, dst, uncomp_size, &out_len) == 0) {
                        ensure_parent_dirs(full);
                        if (fsbridge_exists(full)) fsbridge_delete(full);
                        fsbridge_create(full);
                        fsbridge_write(full, dst, out_len, 0);
                    }
                }
                free(src);
                free(dst);
            }
            count++;
        }
        pos = data_off + comp_size;
    }
    if (count == 0) { seterr(err, err_len, "empty, invalid, or unsupported zip archive"); return -1; }
    return count;
}
