/* btrfs driver: reads real btrfs volumes (single device; single/DUP/RAID1
 * chunks; uncompressed inline and regular extents). Always read-only:
 * writing needs copy-on-write trees, extent/csum/free-space bookkeeping and
 * checksums that this driver does not implement. */

#include "btrfs.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "crc32c.h"

#define BTRFS_MAGIC         0x4D5F53665248425FULL  /* "_BHRfS_M" */
#define BTRFS_SUPER_OFFSET  65536ULL

#define BTRFS_FS_TREE_OBJECTID       5ULL
#define BTRFS_FIRST_FREE_OBJECTID    256ULL

#define BTRFS_INODE_ITEM_KEY  0x01
#define BTRFS_DIR_ITEM_KEY    0x54
#define BTRFS_DIR_INDEX_KEY   0x60
#define BTRFS_EXTENT_DATA_KEY 0x6C
#define BTRFS_ROOT_ITEM_KEY   0x84
#define BTRFS_CHUNK_ITEM_KEY  0xE4

#define BTRFS_FT_DIR 2

#define BTRFS_NODE_HDR_SZ 101
#define BTRFS_KEY_PTR_SZ  33
#define BTRFS_ITEM_SZ     25

#define BTRFS_INCOMPAT_RAID56         0x80ULL
#define BTRFS_INCOMPAT_ZONED          0x1000ULL
#define BTRFS_INCOMPAT_EXTENT_TREE_V2 0x2000ULL

#define BTRFS_BG_RAID0   0x08ULL
#define BTRFS_BG_RAID10  0x40ULL
#define BTRFS_BG_RAID5   0x80ULL
#define BTRFS_BG_RAID6   0x100ULL

static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }

typedef struct {
    uint64_t objectid;
    uint8_t type;
    uint64_t offset;
} bt_key_t;

static void key_read(const uint8_t *p, bt_key_t *k)
{
    k->objectid = rd64(p);
    k->type = p[8];
    k->offset = rd64(p + 9);
}

static int key_cmp(const bt_key_t *a, const bt_key_t *b)
{
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

/* ---------- logical address mapping ---------- */

static int bt_phys(btrfs_t *fs, uint64_t logical, uint64_t *phys, uint64_t *avail)
{
    for (int i = 0; i < fs->num_chunks; i++) {
        btrfs_chunk_map_t *c = &fs->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) {
            *phys = c->physical + (logical - c->logical);
            *avail = c->logical + c->length - logical;
            return 0;
        }
    }
    return -1;
}

static int bt_read(btrfs_t *fs, uint64_t logical, uint32_t len, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        uint64_t phys, avail;
        if (bt_phys(fs, logical, &phys, &avail) != 0) return -1;
        uint32_t n = (uint64_t)len < avail ? len : (uint32_t)avail;
        if (blockdev_read_bytes(fs->bd, phys, n, p) != 0) return -1;
        p += n;
        logical += n;
        len -= n;
    }
    return 0;
}

static void chunk_add(btrfs_t *fs, uint64_t logical, uint64_t length, uint64_t physical)
{
    for (int i = 0; i < fs->num_chunks; i++)
        if (fs->chunks[i].logical == logical) return;
    if (fs->num_chunks >= BTRFS_MAX_CHUNKS) return;
    fs->chunks[fs->num_chunks].logical = logical;
    fs->chunks[fs->num_chunks].length = length;
    fs->chunks[fs->num_chunks].physical = physical;
    fs->num_chunks++;
}

/* Parses a CHUNK_ITEM payload; returns its size in bytes or -1 when the
 * striping is one this driver cannot map. */
static int chunk_parse(btrfs_t *fs, uint64_t logical, const uint8_t *c, uint32_t avail)
{
    if (avail < 48 + 32) return -1;
    uint64_t length = rd64(c);
    uint64_t type = rd64(c + 24);
    uint16_t nstripes = rd16(c + 44);
    if (nstripes < 1 || avail < 48u + (uint32_t)nstripes * 32) return -1;
    if (type & (BTRFS_BG_RAID0 | BTRFS_BG_RAID10 | BTRFS_BG_RAID5 | BTRFS_BG_RAID6)) return -1;
    chunk_add(fs, logical, length, rd64(c + 48 + 8));
    return 48 + (int)nstripes * 32;
}

/* ---------- tree search ---------- */

typedef struct {
    uint64_t node[10];       /* logical address of the node at each level */
    int idx[10];             /* chosen child index at each level above the leaf */
    int level;               /* level of the root */
    uint8_t *leaf;           /* nodesize bytes: the current leaf */
    int slot;
    int nritems;
} bt_path_t;

static void path_free(bt_path_t *p)
{
    free(p->leaf);
    p->leaf = 0;
}

static void leaf_key(const bt_path_t *p, int slot, bt_key_t *k)
{
    key_read(p->leaf + BTRFS_NODE_HDR_SZ + (size_t)slot * BTRFS_ITEM_SZ, k);
}

static const uint8_t *leaf_data(btrfs_t *fs, const bt_path_t *p, int slot, uint32_t *size)
{
    const uint8_t *it = p->leaf + BTRFS_NODE_HDR_SZ + (size_t)slot * BTRFS_ITEM_SZ;
    uint32_t off = rd32(it + 17), sz = rd32(it + 21);
    if ((uint64_t)BTRFS_NODE_HDR_SZ + off + sz > fs->nodesize) { *size = 0; return 0; }
    *size = sz;
    return p->leaf + BTRFS_NODE_HDR_SZ + off;
}

static int node_check(btrfs_t *fs, const uint8_t *buf, uint8_t *level, uint32_t *n)
{
    *level = buf[100];
    *n = rd32(buf + 96);
    uint64_t each = *level ? BTRFS_KEY_PTR_SZ : BTRFS_ITEM_SZ;
    return (BTRFS_NODE_HDR_SZ + (uint64_t)*n * each > fs->nodesize || *level > 8) ? -1 : 0;
}

/* Finds the leaf and slot of the first item whose key is >= `key`; the slot
 * may equal nritems when every item of this leaf is smaller. */
static int bt_search(btrfs_t *fs, uint64_t root, const bt_key_t *key, bt_path_t *p)
{
    memset(p, 0, sizeof(*p));
    p->leaf = (uint8_t *)malloc(fs->nodesize);
    if (!p->leaf) return -1;
    uint64_t cur = root;
    for (int depth = 0; depth < 10; depth++) {
        if (bt_read(fs, cur, fs->nodesize, p->leaf) != 0) return -1;
        uint8_t level;
        uint32_t n;
        if (node_check(fs, p->leaf, &level, &n) != 0) return -1;
        if (depth == 0) p->level = level;
        p->node[level] = cur;
        if (level == 0) {
            p->nritems = (int)n;
            int lo = 0, hi = (int)n;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                bt_key_t k;
                leaf_key(p, mid, &k);
                if (key_cmp(&k, key) < 0) lo = mid + 1; else hi = mid;
            }
            p->slot = lo;
            return 0;
        }
        int sel = 0;
        for (int i = 0; i < (int)n; i++) {
            bt_key_t k;
            key_read(p->leaf + BTRFS_NODE_HDR_SZ + (size_t)i * BTRFS_KEY_PTR_SZ, &k);
            if (key_cmp(&k, key) <= 0) sel = i; else break;
        }
        p->idx[level] = sel;
        cur = rd64(p->leaf + BTRFS_NODE_HDR_SZ + (size_t)sel * BTRFS_KEY_PTR_SZ + 17);
    }
    return -1;
}

/* Moves to the first item of the next leaf. Returns 0, 1 at the end of the
 * tree, or -1 on error. */
static int bt_next_leaf(btrfs_t *fs, bt_path_t *p)
{
    uint8_t *nb = (uint8_t *)malloc(fs->nodesize);
    if (!nb) return -1;
    for (int lvl = 1; lvl <= p->level; lvl++) {
        uint8_t level;
        uint32_t n;
        if (bt_read(fs, p->node[lvl], fs->nodesize, nb) != 0 || node_check(fs, nb, &level, &n) != 0) { free(nb); return -1; }
        if (p->idx[lvl] + 1 >= (int)n) continue;
        p->idx[lvl]++;
        uint64_t cur = rd64(nb + BTRFS_NODE_HDR_SZ + (size_t)p->idx[lvl] * BTRFS_KEY_PTR_SZ + 17);
        for (int l = lvl - 1; l >= 0; l--) {
            uint8_t *dst = l == 0 ? p->leaf : nb;
            if (bt_read(fs, cur, fs->nodesize, dst) != 0 || node_check(fs, dst, &level, &n) != 0) { free(nb); return -1; }
            p->node[l] = cur;
            if (l == 0) {
                p->nritems = (int)n;
                p->slot = 0;
                free(nb);
                return 0;
            }
            p->idx[l] = 0;
            cur = rd64(dst + BTRFS_NODE_HDR_SZ + 17);
        }
    }
    free(nb);
    return 1;
}

/* Makes sure the path points at a real item (moving to later leaves when the
 * slot ran off the end). 0 = item available, 1 = none left, -1 = error. */
static int bt_valid(btrfs_t *fs, bt_path_t *p)
{
    while (p->slot >= p->nritems) {
        int rc = bt_next_leaf(fs, p);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ---------- reading the filesystem trees ---------- */

static int read_chunk_tree(btrfs_t *fs)
{
    bt_key_t first = { 0, 0, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->chunk_root_logical, &first, &p) != 0) { path_free(&p); return -1; }
    int rc = 0;
    while ((rc = bt_valid(fs, &p)) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.type == BTRFS_CHUNK_ITEM_KEY) {
            uint32_t sz;
            const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
            if (!d || chunk_parse(fs, k.offset, d, sz) < 0) { path_free(&p); return -1; }
        }
        p.slot++;
    }
    path_free(&p);
    return rc < 0 ? -1 : 0;
}

/* The root tree maps subvolume ids to the logical address of their tree. */
static int find_subvol_root(btrfs_t *fs, uint64_t id, uint64_t *root)
{
    bt_key_t key = { id, BTRFS_ROOT_ITEM_KEY, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->root_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int rc = -1;
    if (bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (k.objectid == id && k.type == BTRFS_ROOT_ITEM_KEY && d && sz >= 184) {
            *root = rd64(d + 176);        /* btrfs_root_item.bytenr, after the 160-byte inode item */
            rc = 0;
        }
    }
    path_free(&p);
    return rc;
}

static int get_inode(btrfs_t *fs, uint64_t ino, uint64_t *size, uint32_t *mode)
{
    bt_key_t key = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int rc = -1;
    if (bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (k.objectid == ino && k.type == BTRFS_INODE_ITEM_KEY && d && sz >= 56) {
            *size = rd64(d + 16);
            *mode = rd32(d + 52);
            rc = 0;
        }
    }
    path_free(&p);
    return rc;
}

/* One directory entry as stored in DIR_ITEM / DIR_INDEX items. */
typedef struct {
    bt_key_t location;
    uint8_t type;
    char name[BTRFS_MAX_FILENAME + 1];
    int name_len;
} bt_dirent_t;

static int dir_item_parse(const uint8_t *d, uint32_t avail, bt_dirent_t *out, uint32_t *consumed)
{
    if (avail < 30) return -1;
    key_read(d, &out->location);
    uint16_t data_len = rd16(d + 25), name_len = rd16(d + 27);
    out->type = d[29];
    if (name_len > BTRFS_MAX_FILENAME || 30u + name_len + data_len > avail) return -1;
    memcpy(out->name, d + 30, name_len);
    out->name[name_len] = 0;
    out->name_len = name_len;
    *consumed = 30u + name_len + data_len;
    return 0;
}

static int dir_lookup(btrfs_t *fs, uint64_t dir, const char *name, bt_dirent_t *out)
{
    int nlen = (int)strlen(name);
    if (nlen == 0 || nlen > BTRFS_MAX_FILENAME) return -1;
    uint32_t hash = crc32c_update(0xFFFFFFFEu, name, (size_t)nlen);   /* btrfs_name_hash = crc32c(~1, name) */
    bt_key_t key = { dir, BTRFS_DIR_ITEM_KEY, hash };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int rc = -1;
    if (bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (d && key_cmp(&k, &key) == 0) {
            uint32_t off = 0;
            while (off < sz) {
                bt_dirent_t e;
                uint32_t used;
                if (dir_item_parse(d + off, sz - off, &e, &used) != 0) break;
                if (e.name_len == nlen && memcmp(e.name, name, (size_t)nlen) == 0) { *out = e; rc = 0; break; }
                off += used;
            }
        }
    }
    path_free(&p);
    return rc;
}

static int walk(btrfs_t *fs, const char *path, uint64_t *ino, int *is_dir)
{
    uint64_t cur = BTRFS_FIRST_FREE_OBJECTID;
    int dir = 1;
    const char *s = path;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char comp[BTRFS_MAX_FILENAME + 1];
        int n = 0;
        while (*s && *s != '/') {
            if (n >= BTRFS_MAX_FILENAME) return -1;
            comp[n++] = *s++;
        }
        comp[n] = 0;
        if (n == 1 && comp[0] == '.') continue;
        if (!dir) return -1;
        bt_dirent_t e;
        if (dir_lookup(fs, cur, comp, &e) != 0) return -1;
        if (e.location.type != BTRFS_INODE_ITEM_KEY) return -1;   /* subvolume boundaries are not followed */
        cur = e.location.objectid;
        dir = (e.type == BTRFS_FT_DIR);
    }
    *ino = cur;
    *is_dir = dir;
    return 0;
}

/* ---------- file data ---------- */

/* Reads [pos, pos+len) of a file, honouring holes (missing or zero extents). */
static int read_file(btrfs_t *fs, uint64_t ino, uint64_t size, uint64_t pos, uint8_t *buf, uint32_t len)
{
    if (pos >= size) return 0;
    if ((uint64_t)len > size - pos) len = (uint32_t)(size - pos);
    memset(buf, 0, len);

    /* start from the extent that may begin before pos */
    bt_key_t key = { ino, BTRFS_EXTENT_DATA_KEY, pos };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int rc = 0;
    if (p.slot > 0) {
        bt_key_t pk;
        leaf_key(&p, p.slot - 1, &pk);
        if (pk.objectid == ino && pk.type == BTRFS_EXTENT_DATA_KEY) p.slot--;
    } else if (pos > 0) {
        /* the covering extent may sit at the end of the previous leaf: restart
         * from the file's first extent instead of missing it */
        path_free(&p);
        bt_key_t first = { ino, BTRFS_EXTENT_DATA_KEY, 0 };
        if (bt_search(fs, fs->fs_tree_logical, &first, &p) != 0) { path_free(&p); return -1; }
    }
    while (rc == 0 && bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid != ino || k.type != BTRFS_EXTENT_DATA_KEY || k.offset >= pos + len) break;
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (!d || sz < 21) { rc = -1; break; }
        uint8_t compression = d[16], encryption = d[17], type = d[20];
        if (compression != 0 || encryption != 0) { rc = -1; break; }
        uint64_t fo = k.offset;
        uint64_t ext_len;
        if (type == 0) {
            ext_len = sz - 21;
        } else if (type == 1 || type == 2) {
            if (sz < 53) { rc = -1; break; }
            ext_len = rd64(d + 45);
        } else { rc = -1; break; }

        if (fo + ext_len > pos) {
            uint64_t from = pos > fo ? pos : fo;
            uint64_t to = pos + len < fo + ext_len ? pos + len : fo + ext_len;
            if (to > from) {
                uint32_t n = (uint32_t)(to - from);
                uint8_t *dst = buf + (from - pos);
                if (type == 0) {
                    memcpy(dst, d + 21 + (from - fo), n);
                } else if (type == 1) {
                    uint64_t disk = rd64(d + 21), extoff = rd64(d + 37);
                    if (disk != 0 && bt_read(fs, disk + extoff + (from - fo), n, dst) != 0) { rc = -1; break; }
                }   /* prealloc and holes stay zero */
            }
        }
        p.slot++;
    }
    path_free(&p);
    return rc;
}

/* ---------- VFS ---------- */

static int btrfs_vfs_open(void *ctx, const char *path, int flags)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (flags & (VFS_WRONLY | VFS_RDWR | VFS_CREAT | VFS_TRUNC | VFS_APPEND)) return -1;
    uint64_t ino;
    int is_dir;
    if (walk(fs, path, &ino, &is_dir) != 0) return -1;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (get_inode(fs, ino, &size, &mode) != 0) return -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].pos = 0;
            fs->fds[i].size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
            fs->fds[i].is_dir = is_dir;
            return i;
        }
    }
    return -1;
}

static int btrfs_vfs_close(void *ctx, int fd)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int btrfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    btrfs_fd_t *f = &fs->fds[fd];
    if (f->pos >= f->size) return 0;
    uint32_t n = size;
    if (n > f->size - f->pos) n = f->size - f->pos;
    if (n == 0) return 0;
    if (read_file(fs, f->ino, f->size, f->pos, (uint8_t *)buf, n) != 0) return -1;
    f->pos += n;
    return (int)n;
}

static int btrfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int btrfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    btrfs_fd_t *f = &fs->fds[fd];
    uint64_t np;
    if (whence == VFS_SEEK_SET) np = offset;
    else if (whence == VFS_SEEK_CUR) np = (uint64_t)f->pos + offset;
    else if (whence == VFS_SEEK_END) np = (uint64_t)f->size + offset;
    else return -1;
    if (np > f->size) np = f->size;
    f->pos = (uint32_t)np;
    return (int)f->pos;
}

static int btrfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    uint64_t dir;
    int is_dir;
    if (walk(fs, path, &dir, &is_dir) != 0 || !is_dir) return -1;

    bt_key_t key = { dir, BTRFS_DIR_INDEX_KEY, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int count = 0;
    while (count < max && bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid != dir || k.type != BTRFS_DIR_INDEX_KEY) break;
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        bt_dirent_t e;
        uint32_t used;
        if (d && dir_item_parse(d, sz, &e, &used) == 0) {
            vfs_entry_t *out = &entries[count];
            memset(out, 0, sizeof(*out));
            int nl = e.name_len < VFS_NAME_LEN - 1 ? e.name_len : VFS_NAME_LEN - 1;
            memcpy(out->name, e.name, (size_t)nl);
            out->name[nl] = 0;
            out->is_dir = (e.type == BTRFS_FT_DIR);
            if (e.location.type == BTRFS_INODE_ITEM_KEY) {
                out->inode = (uint32_t)e.location.objectid;
                uint64_t isz = 0;
                uint32_t imode = 0;
                if (get_inode(fs, e.location.objectid, &isz, &imode) == 0) {
                    out->size = isz > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)isz;
                    out->mode = imode;
                }
            } else {
                out->is_dir = 1;       /* a subvolume */
                out->mode = 040755;
            }
            count++;
        }
        p.slot++;
    }
    path_free(&p);
    return count;
}

static int btrfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx; (void)path; (void)mode;
    return -1;
}

static int btrfs_vfs_unlink(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

static int btrfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    uint64_t ino;
    int is_dir;
    if (walk(fs, path, &ino, &is_dir) != 0) return -1;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (get_inode(fs, ino, &size, &mode) != 0) return -1;
    memset(entry, 0, sizeof(*entry));
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    int s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    int nl = len - s;
    if (nl >= VFS_NAME_LEN) nl = VFS_NAME_LEN - 1;
    memcpy(entry->name, path + s, (size_t)nl);
    entry->name[nl] = 0;
    entry->size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
    entry->is_dir = is_dir;
    entry->inode = (uint32_t)ino;
    entry->mode = mode;
    return 0;
}

static int btrfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    (void)ctx; (void)old; (void)new_path;
    return -1;
}

static int btrfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void btrfs_mount_vfs(btrfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ops = {
        .open    = btrfs_vfs_open,
        .close   = btrfs_vfs_close,
        .read    = btrfs_vfs_read,
        .write   = btrfs_vfs_write,
        .lseek   = btrfs_vfs_lseek,
        .readdir = btrfs_vfs_readdir,
        .mkdir   = btrfs_vfs_mkdir,
        .unlink  = btrfs_vfs_unlink,
        .stat    = btrfs_vfs_stat,
        .rename  = btrfs_vfs_rename,
        .symlink = btrfs_vfs_symlink,
    };
    klog_write("btrfs: mounted read-only\n");
    vfs_mount(mount_point, &ops, fs);
}

/* ---------- mount ---------- */

int btrfs_probe_and_mount(btrfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(btrfs_t));
    fs->bd = bd;

    uint8_t *sb = (uint8_t *)malloc(4096);
    if (!sb) return -1;
    if (blockdev_read_bytes(bd, BTRFS_SUPER_OFFSET, 4096, sb) != 0 || rd64(sb + 0x40) != BTRFS_MAGIC) { free(sb); return -1; }

    fs->generation = rd64(sb + 0x48);
    fs->root_logical = rd64(sb + 0x50);
    fs->chunk_root_logical = rd64(sb + 0x58);
    fs->total_bytes = rd64(sb + 0x70);
    fs->bytes_used = rd64(sb + 0x78);
    uint64_t num_devices = rd64(sb + 0x88);
    fs->sectorsize = rd32(sb + 0x90);
    fs->nodesize = rd32(sb + 0x94);
    uint32_t sys_array_size = rd32(sb + 0xA0);
    uint64_t incompat = rd64(sb + 0xBC);
    memcpy(fs->label, sb + 0x12B, 255);
    fs->label[255] = 0;

    if (fs->nodesize < 4096 || fs->nodesize > 65536 || (fs->nodesize & (fs->nodesize - 1)) ||
        fs->sectorsize < 512 || num_devices != 1 ||
        (incompat & (BTRFS_INCOMPAT_RAID56 | BTRFS_INCOMPAT_ZONED | BTRFS_INCOMPAT_EXTENT_TREE_V2))) {
        free(sb);
        return -1;
    }

    /* bootstrap chunks from the superblock's system chunk array */
    if (sys_array_size > 2048) sys_array_size = 2048;
    const uint8_t *arr = sb + 0x32B;
    uint32_t pos = 0;
    while (pos + 17 + 48 + 32 <= sys_array_size) {
        bt_key_t key;
        key_read(arr + pos, &key);
        pos += 17;
        int used = chunk_parse(fs, key.offset, arr + pos, sys_array_size - pos);
        if (used < 0) { free(sb); return -1; }
        pos += (uint32_t)used;
    }
    free(sb);

    if (read_chunk_tree(fs) != 0) return -1;
    if (find_subvol_root(fs, BTRFS_FS_TREE_OBJECTID, &fs->fs_tree_logical) != 0) return -1;
    return 0;
}

int btrfs_umount(btrfs_t *fs)
{
    (void)fs;
    return 0;
}

int btrfs_format(blockdev_t *bd, const char *label)
{
    (void)bd; (void)label;
    return -1;
}
