/* XFS driver: reads real XFS volumes (v4 and v5 metadata; short-form,
 * block, leaf and node directories; extent and B-tree data forks).
 * Always read-only. */

#include "xfs.h"
#include "memory.h"
#include "string.h"
#include "klog.h"

#define XFS_SB_MAGIC     0x58465342u   /* "XFSB" */
#define XFS_DINODE_MAGIC 0x494E        /* "IN" */

#define XFS_DIR2_BLOCK_MAGIC   0x58443242u  /* "XD2B" */
#define XFS_DIR2_DATA_MAGIC    0x58443244u  /* "XD2D" */
#define XFS_DIR3_BLOCK_MAGIC   0x58444233u  /* "XDB3" */
#define XFS_DIR3_DATA_MAGIC    0x58444433u  /* "XDD3" */

#define XFS_DINODE_FMT_LOCAL   1
#define XFS_DINODE_FMT_EXTENTS 2
#define XFS_DINODE_FMT_BTREE   3

#define XFS_INCOMPAT_FTYPE     0x01u
#define XFS_INCOMPAT_SPINODES  0x02u
#define XFS_INCOMPAT_META_UUID 0x04u
#define XFS_INCOMPAT_BIGTIME   0x08u
#define XFS_INCOMPAT_OK        (XFS_INCOMPAT_FTYPE | XFS_INCOMPAT_SPINODES | XFS_INCOMPAT_META_UUID | XFS_INCOMPAT_BIGTIME)

#define S_IFMT_  0170000
#define S_IFDIR_ 0040000

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

typedef struct {
    uint64_t startoff;     /* in filesystem blocks */
    uint64_t startblock;   /* filesystem block number (AG-encoded) */
    uint64_t blockcount;
    int unwritten;
} xfs_ext_t;

typedef struct {
    uint8_t *raw;          /* inodesize bytes */
    uint16_t mode;
    uint8_t version, format;
    uint64_t size;
    uint32_t nextents;
    uint8_t forkoff;
    int core_len;
    const uint8_t *fork;
    int fork_len;
} xfs_inode_t;

static uint64_t fsb_to_byte(xfs_t *fs, uint64_t fsb)
{
    uint64_t agno = fsb >> fs->agblklog;
    uint64_t agbno = fsb & (((uint64_t)1 << fs->agblklog) - 1);
    return (agno * fs->agblocks + agbno) * fs->blocksize;
}

static int read_inode(xfs_t *fs, uint64_t ino, xfs_inode_t *ip)
{
    uint64_t agno = ino >> (fs->agblklog + fs->inopblog);
    uint64_t agbno = (ino >> fs->inopblog) & (((uint64_t)1 << fs->agblklog) - 1);
    uint64_t slot = ino & (((uint64_t)1 << fs->inopblog) - 1);
    if (agno >= fs->agcount || agbno >= fs->agblocks) return -1;
    uint64_t off = (agno * fs->agblocks + agbno) * fs->blocksize + slot * fs->inodesize;
    memset(ip, 0, sizeof(*ip));
    ip->raw = (uint8_t *)malloc(fs->inodesize);
    if (!ip->raw) return -1;
    if (blockdev_read_bytes(fs->bd, off, fs->inodesize, ip->raw) != 0 || be16(ip->raw) != XFS_DINODE_MAGIC) {
        free(ip->raw);
        ip->raw = 0;
        return -1;
    }
    ip->mode = be16(ip->raw + 2);
    ip->version = ip->raw[4];
    ip->format = ip->raw[5];
    ip->size = be64(ip->raw + 56);
    ip->nextents = be32(ip->raw + 76);
    ip->forkoff = ip->raw[82];
    ip->core_len = ip->version >= 3 ? 176 : 100;
    ip->fork = ip->raw + ip->core_len;
    ip->fork_len = ip->forkoff ? (int)ip->forkoff * 8 : (int)fs->inodesize - ip->core_len;
    if (ip->fork_len < 0 || ip->core_len + ip->fork_len > (int)fs->inodesize) { free(ip->raw); ip->raw = 0; return -1; }
    return 0;
}

static void inode_free(xfs_inode_t *ip)
{
    free(ip->raw);
    ip->raw = 0;
}

/* ---------- data fork extents ---------- */

typedef struct {
    xfs_ext_t *list;
    uint32_t n, cap;
} ext_list_t;

static int ext_add(ext_list_t *l, const uint8_t *rec)
{
    uint64_t hi = be64(rec), lo = be64(rec + 8);
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 16;
        xfs_ext_t *nl = (xfs_ext_t *)malloc(nc * sizeof(xfs_ext_t));
        if (!nl) return -1;
        if (l->n) memcpy(nl, l->list, l->n * sizeof(xfs_ext_t));
        free(l->list);
        l->list = nl;
        l->cap = nc;
    }
    xfs_ext_t *e = &l->list[l->n++];
    e->unwritten = (int)(hi >> 63);
    e->startoff = (hi >> 9) & (((uint64_t)1 << 54) - 1);
    e->startblock = ((hi & 0x1FF) << 43) | (lo >> 21);
    e->blockcount = lo & 0x1FFFFF;
    return 0;
}

/* Walks a bmap btree block (long-format pointers) collecting leaf records. */
static int bmbt_walk(xfs_t *fs, uint64_t fsb, int depth, ext_list_t *l)
{
    if (depth > 6) return -1;
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    int rc = -1;
    if (blockdev_read_bytes(fs->bd, fsb_to_byte(fs, fsb), fs->blocksize, blk) != 0) goto out;
    uint32_t magic = be32(blk);
    if (magic != 0x424D4150u && magic != 0x424D4133u) goto out;   /* "BMAP" / "BMA3" */
    uint16_t level = be16(blk + 4), nrecs = be16(blk + 6);
    int hdr = (magic == 0x424D4133u) ? 72 : 24;
    if (level == 0) {
        if ((uint64_t)hdr + (uint64_t)nrecs * 16 > fs->blocksize) goto out;
        for (uint16_t i = 0; i < nrecs; i++)
            if (ext_add(l, blk + hdr + (size_t)i * 16) != 0) goto out;
    } else {
        uint32_t maxrecs = (fs->blocksize - (uint32_t)hdr) / 16;
        if (nrecs > maxrecs) goto out;
        for (uint16_t i = 0; i < nrecs; i++) {
            uint64_t child = be64(blk + hdr + (size_t)maxrecs * 8 + (size_t)i * 8);
            if (bmbt_walk(fs, child, depth + 1, l) != 0) goto out;
        }
    }
    rc = 0;
out:
    free(blk);
    return rc;
}

static int get_extents(xfs_t *fs, const xfs_inode_t *ip, ext_list_t *l)
{
    memset(l, 0, sizeof(*l));
    if (ip->format == XFS_DINODE_FMT_EXTENTS) {
        if ((int64_t)ip->nextents * 16 > ip->fork_len) return -1;
        for (uint32_t i = 0; i < ip->nextents; i++)
            if (ext_add(l, ip->fork + (size_t)i * 16) != 0) { free(l->list); return -1; }
        return 0;
    }
    if (ip->format == XFS_DINODE_FMT_BTREE) {
        uint16_t level = be16(ip->fork), nrecs = be16(ip->fork + 2);
        uint32_t maxrecs = (uint32_t)(ip->fork_len - 4) / 16;
        if (level == 0 || nrecs > maxrecs) return -1;
        for (uint16_t i = 0; i < nrecs; i++) {
            uint64_t child = be64(ip->fork + 4 + (size_t)maxrecs * 8 + (size_t)i * 8);
            if (bmbt_walk(fs, child, 1, l) != 0) { free(l->list); l->list = 0; return -1; }
        }
        return 0;
    }
    return -1;
}

/* ---------- file contents ---------- */

static int read_data(xfs_t *fs, const xfs_inode_t *ip, uint64_t pos, uint8_t *buf, uint32_t len)
{
    if (pos >= ip->size) return 0;
    if ((uint64_t)len > ip->size - pos) len = (uint32_t)(ip->size - pos);
    if (ip->format == XFS_DINODE_FMT_LOCAL) {
        if (pos + len > (uint64_t)ip->fork_len) return -1;
        memcpy(buf, ip->fork + pos, len);
        return (int)len;
    }
    ext_list_t l;
    if (get_extents(fs, ip, &l) != 0) return -1;
    memset(buf, 0, len);
    for (uint32_t i = 0; i < l.n; i++) {
        xfs_ext_t *e = &l.list[i];
        uint64_t estart = e->startoff * fs->blocksize, eend = (e->startoff + e->blockcount) * fs->blocksize;
        if (eend <= pos || estart >= pos + len) continue;
        if (e->unwritten) continue;
        uint64_t from = pos > estart ? pos : estart;
        uint64_t to = pos + len < eend ? pos + len : eend;
        if (blockdev_read_bytes(fs->bd, fsb_to_byte(fs, e->startblock) + (from - estart), (uint32_t)(to - from), buf + (from - pos)) != 0) {
            free(l.list);
            return -1;
        }
    }
    free(l.list);
    return (int)len;
}

/* ---------- directories ---------- */

typedef int (*dir_cb)(void *ctx, uint64_t ino, const char *name, int name_len);

static int dir_iter(xfs_t *fs, const xfs_inode_t *dir, dir_cb cb, void *ctx)
{
    if (dir->format == XFS_DINODE_FMT_LOCAL) {
        const uint8_t *f = dir->fork;
        int count = f[0], i8 = f[1];
        int isz = i8 ? 8 : 4;
        int p = 2 + isz;
        for (int i = 0; i < count; i++) {
            if (p + 3 > dir->fork_len) return -1;
            int nl = f[p];
            int q = p + 3;                         /* namelen, 2-byte offset */
            if (q + nl + (fs->ftype ? 1 : 0) + isz > dir->fork_len) return -1;
            const uint8_t *name = f + q;
            q += nl + (fs->ftype ? 1 : 0);
            uint64_t ino = isz == 8 ? be64(f + q) : be32(f + q);
            q += isz;
            int rc = cb(ctx, ino, (const char *)name, nl);
            if (rc) return rc;
            p = q;
        }
        return 0;
    }
    ext_list_t l;
    if (get_extents(fs, dir, &l) != 0) return -1;
    uint32_t dirblk = fs->blocksize << fs->dirblklog;
    uint64_t leaf_off = (((uint64_t)1 << 35) / fs->blocksize);        /* XFS_DIR2_LEAF_OFFSET (32GB) */
    uint8_t *blk = (uint8_t *)malloc(dirblk);
    if (!blk) { free(l.list); return -1; }
    int rc = 0;
    for (uint32_t i = 0; i < l.n && rc == 0; i++) {
        xfs_ext_t *e = &l.list[i];
        if (e->startoff >= leaf_off || e->unwritten) continue;
        for (uint64_t b = 0; b < e->blockcount && rc == 0; b += (1u << fs->dirblklog)) {
            if (((e->startoff + b) & ((1u << fs->dirblklog) - 1)) != 0) continue;
            if (b + (1u << fs->dirblklog) > e->blockcount) break;
            if (blockdev_read_bytes(fs->bd, fsb_to_byte(fs, e->startblock + b), dirblk, blk) != 0) { rc = -1; break; }
            uint32_t magic = be32(blk);
            int single = (magic == XFS_DIR2_BLOCK_MAGIC || magic == XFS_DIR3_BLOCK_MAGIC);
            if (!single && magic != XFS_DIR2_DATA_MAGIC && magic != XFS_DIR3_DATA_MAGIC) continue;
            uint32_t o = (magic == XFS_DIR3_BLOCK_MAGIC || magic == XFS_DIR3_DATA_MAGIC) ? 64 : 16;
            uint32_t end = dirblk;
            if (single) {
                uint32_t cnt = be32(blk + dirblk - 8);
                if ((uint64_t)cnt * 8 + 8 > dirblk) { rc = -1; break; }
                end = dirblk - 8 - cnt * 8;
            }
            while (o + 8 <= end) {
                if (be16(blk + o) == 0xFFFF) {                 /* unused space */
                    uint16_t len = be16(blk + o + 2);
                    if (len < 8) { rc = -1; break; }
                    o += len;
                    continue;
                }
                uint64_t ino = be64(blk + o);
                uint8_t nl = blk[o + 8];
                uint32_t esz = (8u + 1 + nl + (fs->ftype ? 1u : 0u) + 2 + 7) & ~7u;
                if (o + esz > end) { rc = -1; break; }
                int r2 = cb(ctx, ino, (const char *)blk + o + 9, nl);
                if (r2) { rc = r2; break; }
                o += esz;
            }
        }
    }
    free(blk);
    free(l.list);
    return rc;
}

typedef struct {
    const char *want;
    int want_len;
    uint64_t ino;
    int found;
} lookup_ctx_t;

static int lookup_cb(void *ctx, uint64_t ino, const char *name, int nl)
{
    lookup_ctx_t *c = (lookup_ctx_t *)ctx;
    if (nl == c->want_len && memcmp(name, c->want, (size_t)nl) == 0) {
        c->ino = ino;
        c->found = 1;
        return 1;
    }
    return 0;
}

static int walk(xfs_t *fs, const char *path, uint64_t *out_ino)
{
    uint64_t cur = fs->rootino;
    const char *s = path;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *c = s;
        while (*s && *s != '/') s++;
        int n = (int)(s - c);
        if (n == 1 && c[0] == '.') continue;
        if (n > XFS_MAX_FILENAME) return -1;
        xfs_inode_t d;
        if (read_inode(fs, cur, &d) != 0) return -1;
        if ((d.mode & S_IFMT_) != S_IFDIR_) { inode_free(&d); return -1; }
        lookup_ctx_t lc = { c, n, 0, 0 };
        int rc = dir_iter(fs, &d, lookup_cb, &lc);
        inode_free(&d);
        if (!lc.found) return rc < 0 ? -1 : -1;
        cur = lc.ino;
    }
    *out_ino = cur;
    return 0;
}

typedef struct {
    xfs_t *fs;
    vfs_entry_t *entries;
    int max, count;
} list_ctx_t;

static int list_cb(void *ctx, uint64_t ino, const char *name, int nl)
{
    list_ctx_t *c = (list_ctx_t *)ctx;
    if (c->count >= c->max) return 1;
    if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.')) return 0;
    vfs_entry_t *e = &c->entries[c->count];
    memset(e, 0, sizeof(*e));
    int n = nl < VFS_NAME_LEN - 1 ? nl : VFS_NAME_LEN - 1;
    memcpy(e->name, name, (size_t)n);
    e->name[n] = 0;
    e->inode = (uint32_t)ino;
    xfs_inode_t ip;
    if (read_inode(c->fs, ino, &ip) == 0) {
        e->size = ip.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)ip.size;
        e->is_dir = (ip.mode & S_IFMT_) == S_IFDIR_;
        e->mode = ip.mode;
        inode_free(&ip);
    }
    c->count++;
    return 0;
}

/* ---------- VFS ---------- */

static int xfs_vfs_open(void *ctx, const char *path, int flags)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (flags & (VFS_WRONLY | VFS_RDWR | VFS_CREAT | VFS_TRUNC | VFS_APPEND)) return -1;
    uint64_t ino;
    if (walk(fs, path, &ino) != 0) return -1;
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].pos = 0;
            fs->fds[i].size = ip.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)ip.size;
            fs->fds[i].is_dir = (ip.mode & S_IFMT_) == S_IFDIR_;
            inode_free(&ip);
            return i;
        }
    }
    inode_free(&ip);
    return -1;
}

static int xfs_vfs_close(void *ctx, int fd)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int xfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    xfs_fd_t *f = &fs->fds[fd];
    if (f->pos >= f->size) return 0;
    uint32_t n = size;
    if (n > f->size - f->pos) n = f->size - f->pos;
    if (n == 0) return 0;
    xfs_inode_t ip;
    if (read_inode(fs, f->ino, &ip) != 0) return -1;
    int r = read_data(fs, &ip, f->pos, (uint8_t *)buf, n);
    inode_free(&ip);
    if (r < 0) return -1;
    f->pos += (uint32_t)r;
    return r;
}

static int xfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int xfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    xfs_fd_t *f = &fs->fds[fd];
    uint64_t np;
    if (whence == VFS_SEEK_SET) np = offset;
    else if (whence == VFS_SEEK_CUR) np = (uint64_t)f->pos + offset;
    else if (whence == VFS_SEEK_END) np = (uint64_t)f->size + offset;
    else return -1;
    if (np > f->size) np = f->size;
    f->pos = (uint32_t)np;
    return (int)f->pos;
}

static int xfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    xfs_t *fs = (xfs_t *)ctx;
    uint64_t ino;
    if (walk(fs, path, &ino) != 0) return -1;
    xfs_inode_t d;
    if (read_inode(fs, ino, &d) != 0) return -1;
    if ((d.mode & S_IFMT_) != S_IFDIR_) { inode_free(&d); return -1; }
    list_ctx_t lc = { fs, entries, max, 0 };
    int rc = dir_iter(fs, &d, list_cb, &lc);
    inode_free(&d);
    if (rc < 0) return -1;
    return lc.count;
}

static int xfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx; (void)path; (void)mode;
    return -1;
}

static int xfs_vfs_unlink(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

static int xfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    xfs_t *fs = (xfs_t *)ctx;
    uint64_t ino;
    if (walk(fs, path, &ino) != 0) return -1;
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    memset(entry, 0, sizeof(*entry));
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    int s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    int nl = len - s;
    if (nl >= VFS_NAME_LEN) nl = VFS_NAME_LEN - 1;
    memcpy(entry->name, path + s, (size_t)nl);
    entry->name[nl] = 0;
    entry->size = ip.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)ip.size;
    entry->is_dir = (ip.mode & S_IFMT_) == S_IFDIR_;
    entry->inode = (uint32_t)ino;
    entry->mode = ip.mode;
    inode_free(&ip);
    return 0;
}

static int xfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    (void)ctx; (void)old; (void)new_path;
    return -1;
}

static int xfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void xfs_mount_vfs(xfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ops = {
        .open    = xfs_vfs_open,
        .close   = xfs_vfs_close,
        .read    = xfs_vfs_read,
        .write   = xfs_vfs_write,
        .lseek   = xfs_vfs_lseek,
        .readdir = xfs_vfs_readdir,
        .mkdir   = xfs_vfs_mkdir,
        .unlink  = xfs_vfs_unlink,
        .stat    = xfs_vfs_stat,
        .rename  = xfs_vfs_rename,
        .symlink = xfs_vfs_symlink,
    };
    klog_write("xfs: mounted read-only\n");
    vfs_mount(mount_point, &ops, fs);
}

/* ---------- mount ---------- */

int xfs_probe_and_mount(xfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    uint8_t *sb = (uint8_t *)malloc(512);
    if (!sb) return -1;
    if (blockdev_read_bytes(bd, 0, 512, sb) != 0 || be32(sb) != XFS_SB_MAGIC) { free(sb); return -1; }

    fs->blocksize = be32(sb + 4);
    fs->dblocks = be64(sb + 8);
    fs->rootino = be64(sb + 56);
    fs->agblocks = be32(sb + 84);
    fs->agcount = be32(sb + 88);
    uint16_t versionnum = be16(sb + 100);
    fs->inodesize = be16(sb + 104);
    fs->inopblock = be16(sb + 106);
    memcpy(fs->fname, sb + 108, 12);
    fs->fname[12] = 0;
    fs->agblklog = sb[124];
    fs->inopblog = sb[123];
    fs->dirblklog = sb[192];
    fs->v5 = (versionnum & 0xF) == 5;
    uint32_t features2 = be32(sb + 200);
    uint32_t incompat = fs->v5 ? be32(sb + 216) : 0;
    uint32_t rextents = (uint32_t)be64(sb + 24);
    if (fs->v5) {
        fs->ftype = (incompat & XFS_INCOMPAT_FTYPE) != 0;
    } else {
        fs->ftype = (features2 & 0x200) != 0;
    }
    free(sb);

    if (fs->blocksize < 512 || fs->blocksize > 65536 || (fs->blocksize & (fs->blocksize - 1)) ||
        fs->inodesize < 256 || fs->inodesize > 2048 || fs->agblocks == 0 || fs->agcount == 0 ||
        fs->agblklog > 31 || fs->inopblog > 6 || fs->dirblklog > 4 || rextents != 0 ||
        (fs->v5 && (incompat & ~XFS_INCOMPAT_OK))) return -1;

    xfs_inode_t root;
    if (read_inode(fs, fs->rootino, &root) != 0) return -1;
    int ok = (root.mode & S_IFMT_) == S_IFDIR_;
    inode_free(&root);
    return ok ? 0 : -1;
}

int xfs_umount(xfs_t *fs)
{
    (void)fs;
    return 0;
}

int xfs_format(blockdev_t *bd, const char *label)
{
    (void)bd; (void)label;
    return -1;
}
