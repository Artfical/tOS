/* btrfs driver: single-device volumes (single/DUP/RAID1 chunks), reading
 * uncompressed inline and regular extents, and writing them back.
 *
 * Writes change the trees in place instead of copying them: leaves are
 * edited and split, new tree blocks and data extents come from a free-space
 * map built from the extent tree, and the extent tree, the checksum tree, the
 * block-group items, the root items and the superblock are kept consistent
 * (`btrfs check` and the Linux kernel accept the result). The free-space
 * tree is not maintained: its "valid" bit is cleared in the superblock, which
 * makes Linux rebuild it on the next mount. Not crash-safe in the way the
 * copy-on-write kernel implementation is. */

#include "btrfs.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "crc32c.h"

#define BTRFS_MAGIC         0x4D5F53665248425FULL  /* "_BHRfS_M" */
#define BTRFS_SUPER_OFFSET  65536ULL

#define BTRFS_ROOT_TREE_OBJECTID     1ULL
#define BTRFS_EXTENT_TREE_OBJECTID   2ULL
#define BTRFS_CHUNK_TREE_OBJECTID    3ULL
#define BTRFS_DEV_TREE_OBJECTID      4ULL
#define BTRFS_FS_TREE_OBJECTID       5ULL
#define BTRFS_CSUM_TREE_OBJECTID     7ULL
#define BTRFS_QUOTA_TREE_OBJECTID    8ULL
#define BTRFS_FIRST_FREE_OBJECTID    256ULL
#define BTRFS_EXTENT_CSUM_OBJECTID   0xFFFFFFFFFFFFFFF6ULL   /* -10 */
#define BTRFS_FIRST_CHUNK_OBJECTID   256ULL

#define BTRFS_INODE_ITEM_KEY   0x01
#define BTRFS_INODE_REF_KEY    0x0C
#define BTRFS_XATTR_ITEM_KEY   0x18
#define BTRFS_DIR_ITEM_KEY     0x54
#define BTRFS_DIR_INDEX_KEY    0x60
#define BTRFS_EXTENT_DATA_KEY  0x6C
#define BTRFS_EXTENT_CSUM_KEY  0x80
#define BTRFS_ROOT_ITEM_KEY    0x84
#define BTRFS_EXTENT_ITEM_KEY  0xA8
#define BTRFS_METADATA_ITEM_KEY 0xA9
#define BTRFS_TREE_BLOCK_REF_KEY 0xB0
#define BTRFS_EXTENT_DATA_REF_KEY 0xB2
#define BTRFS_BLOCK_GROUP_ITEM_KEY 0xC0
#define BTRFS_CHUNK_ITEM_KEY   0xE4

#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2

#define BTRFS_NODE_HDR_SZ 101
#define BTRFS_KEY_PTR_SZ  33
#define BTRFS_ITEM_SZ     25

#define BTRFS_INCOMPAT_MIXED_BACKREF  0x1ULL
#define BTRFS_INCOMPAT_DEFAULT_SUBVOL 0x2ULL
#define BTRFS_INCOMPAT_COMPRESS_LZO   0x8ULL
#define BTRFS_INCOMPAT_COMPRESS_ZSTD  0x10ULL
#define BTRFS_INCOMPAT_BIG_METADATA   0x20ULL
#define BTRFS_INCOMPAT_EXTENDED_IREF  0x40ULL
#define BTRFS_INCOMPAT_RAID56         0x80ULL
#define BTRFS_INCOMPAT_SKINNY_METADATA 0x100ULL
#define BTRFS_INCOMPAT_NO_HOLES       0x200ULL
#define BTRFS_INCOMPAT_METADATA_UUID  0x400ULL
#define BTRFS_INCOMPAT_ZONED          0x1000ULL
#define BTRFS_INCOMPAT_EXTENT_TREE_V2 0x2000ULL

#define BTRFS_BG_DATA    0x01ULL
#define BTRFS_BG_SYSTEM  0x02ULL
#define BTRFS_BG_METADATA 0x04ULL
#define BTRFS_BG_RAID0   0x08ULL
#define BTRFS_BG_RAID1   0x10ULL
#define BTRFS_BG_DUP     0x20ULL
#define BTRFS_BG_RAID10  0x40ULL
#define BTRFS_BG_RAID5   0x80ULL
#define BTRFS_BG_RAID6   0x100ULL

#define BT_MAX_LEVEL 8
#define BT_MAX_INLINE_EXTENT 2048u

static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void wr16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr64(void *p, uint64_t v) { memcpy(p, &v, 8); }

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

static void key_write(uint8_t *p, const bt_key_t *k)
{
    wr64(p, k->objectid);
    p[8] = k->type;
    wr64(p + 9, k->offset);
}

static int key_cmp(const bt_key_t *a, const bt_key_t *b)
{
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

/* ---------- logical address mapping ---------- */

static int bt_chunk_index(btrfs_t *fs, uint64_t logical)
{
    for (int i = 0; i < fs->num_chunks; i++) {
        btrfs_chunk_map_t *c = &fs->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) return i;
    }
    return -1;
}

static int bt_phys(btrfs_t *fs, uint64_t logical, uint64_t *phys, uint64_t *avail)
{
    int i = bt_chunk_index(fs, logical);
    if (i < 0) return -1;
    btrfs_chunk_map_t *c = &fs->chunks[i];
    *phys = c->physical + (logical - c->logical);
    *avail = c->logical + c->length - logical;
    return 0;
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

/* writes go to every copy of the data (DUP / RAID1 keep two) */
static int bt_write_logical(btrfs_t *fs, uint64_t logical, uint32_t len, const void *buf)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int i = bt_chunk_index(fs, logical);
        if (i < 0) return -1;
        btrfs_chunk_map_t *c = &fs->chunks[i];
        uint64_t avail = c->logical + c->length - logical;
        uint32_t n = (uint64_t)len < avail ? len : (uint32_t)avail;
        if (blockdev_write_bytes(fs->bd, c->physical + (logical - c->logical), n, p) != 0) return -1;
        if (c->nstripes >= 2 && blockdev_write_bytes(fs->bd, c->physical2 + (logical - c->logical), n, p) != 0) return -1;
        p += n;
        logical += n;
        len -= n;
    }
    return 0;
}

static void chunk_add(btrfs_t *fs, uint64_t logical, uint64_t length, uint64_t physical, uint64_t physical2, uint64_t type, int nstripes)
{
    for (int i = 0; i < fs->num_chunks; i++)
        if (fs->chunks[i].logical == logical) return;
    if (fs->num_chunks >= BTRFS_MAX_CHUNKS) { fs->rw = -1; return; }
    btrfs_chunk_map_t *c = &fs->chunks[fs->num_chunks++];
    c->logical = logical;
    c->length = length;
    c->physical = physical;
    c->physical2 = physical2;
    c->type = type;
    c->nstripes = nstripes;
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
    uint64_t p2 = nstripes >= 2 ? rd64(c + 48 + 32 + 8) : 0;
    chunk_add(fs, logical, length, rd64(c + 48 + 8), p2, type, nstripes > 2 ? 3 : nstripes);
    return 48 + (int)nstripes * 32;
}

/* ---------- tree search ---------- */

typedef struct {
    uint64_t node[BT_MAX_LEVEL + 2];   /* logical address of the node at each level */
    int idx[BT_MAX_LEVEL + 2];         /* chosen child index at each level above the leaf */
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

static int find_subvol_root(btrfs_t *fs, uint64_t id, uint64_t *root);

static void *bt_zalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p) memset(p, 0, n ? n : 1);
    return p;
}

/* ---------- tree blocks: checksums, headers, writing ---------- */

static uint64_t *bt_root_ptr(btrfs_t *fs, uint64_t tree)
{
    switch (tree) {
    case BTRFS_ROOT_TREE_OBJECTID: return &fs->root_logical;
    case BTRFS_EXTENT_TREE_OBJECTID: return &fs->extent_root;
    case BTRFS_CHUNK_TREE_OBJECTID: return &fs->chunk_root_logical;
    case BTRFS_DEV_TREE_OBJECTID: return &fs->dev_root;
    case BTRFS_FS_TREE_OBJECTID: return &fs->fs_tree_logical;
    case BTRFS_CSUM_TREE_OBJECTID: return &fs->csum_root;
    default: return 0;
    }
}

static void bt_blk_csum(btrfs_t *fs, uint8_t *buf)
{
    uint32_t c = ~crc32c_update(0xFFFFFFFFu, buf + 32, fs->nodesize - 32);
    memset(buf, 0, 32);
    wr32(buf, c);
}

static int bt_write_block(btrfs_t *fs, uint64_t bytenr, uint8_t *buf)
{
    bt_blk_csum(fs, buf);
    return bt_write_logical(fs, bytenr, fs->nodesize, buf);
}

static void bt_blk_init(btrfs_t *fs, uint8_t *buf, uint64_t bytenr, uint64_t owner, int level, uint64_t gen)
{
    memset(buf, 0, fs->nodesize);
    memcpy(buf + 32, fs->hdr_fsid, 16);
    wr64(buf + 48, bytenr);
    wr64(buf + 56, 0x0100000000000001ULL);      /* WRITTEN, backref revision 1 */
    memcpy(buf + 64, fs->hdr_chunk_uuid, 16);
    wr64(buf + 80, gen);
    wr64(buf + 88, owner);
    wr32(buf + 96, 0);
    buf[100] = (uint8_t)level;
}

/* ---------- free-space map and allocation ---------- */

static uint64_t align_up64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static int used_lower_bound(btrfs_t *fs, uint64_t start)
{
    int lo = 0, hi = fs->used_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (fs->used[mid].start < start) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int used_insert(btrfs_t *fs, uint64_t start, uint64_t len)
{
    if (fs->used_n == fs->used_cap) {
        int nc = fs->used_cap ? fs->used_cap * 2 : 1024;
        btrfs_range_t *nu = (btrfs_range_t *)malloc((size_t)nc * sizeof(btrfs_range_t));
        if (!nu) return -1;
        if (fs->used_n) memcpy(nu, fs->used, (size_t)fs->used_n * sizeof(btrfs_range_t));
        free(fs->used);
        fs->used = nu;
        fs->used_cap = nc;
    }
    int i = used_lower_bound(fs, start);
    memmove(&fs->used[i + 1], &fs->used[i], (size_t)(fs->used_n - i) * sizeof(btrfs_range_t));
    fs->used[i].start = start;
    fs->used[i].len = len;
    fs->used_n++;
    return 0;
}

static void used_remove(btrfs_t *fs, uint64_t start)
{
    int i = used_lower_bound(fs, start);
    if (i >= fs->used_n || fs->used[i].start != start) return;
    memmove(&fs->used[i], &fs->used[i + 1], (size_t)(fs->used_n - i - 1) * sizeof(btrfs_range_t));
    fs->used_n--;
}

static btrfs_bg_t *bg_find(btrfs_t *fs, uint64_t addr)
{
    for (int i = 0; i < fs->nbgs; i++)
        if (addr >= fs->bgs[i].start && addr < fs->bgs[i].start + fs->bgs[i].len) return &fs->bgs[i];
    return 0;
}

static btrfs_bg_t *bg_add(btrfs_t *fs, uint64_t start, uint64_t len, uint64_t used, uint64_t flags)
{
    if (fs->nbgs == fs->bgs_cap) {
        int nc = fs->bgs_cap ? fs->bgs_cap * 2 : 16;
        btrfs_bg_t *nb = (btrfs_bg_t *)malloc((size_t)nc * sizeof(btrfs_bg_t));
        if (!nb) return 0;
        if (fs->nbgs) memcpy(nb, fs->bgs, (size_t)fs->nbgs * sizeof(btrfs_bg_t));
        free(fs->bgs);
        fs->bgs = nb;
        fs->bgs_cap = nc;
    }
    btrfs_bg_t *b = &fs->bgs[fs->nbgs++];
    b->start = start;
    b->len = len;
    b->used = used;
    b->flags = flags;
    b->dirty = 0;
    return b;
}

/* Reads the extent tree once: every extent and tree block becomes a "used"
 * range, every block-group item a block group. */
static int bt_load_alloc(btrfs_t *fs)
{
    if (fs->alloc_loaded) return 0;
    bt_key_t first = { 0, 0, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->extent_root, &first, &p) != 0) { path_free(&p); return -1; }
    int rc;
    while ((rc = bt_valid(fs, &p)) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.type == BTRFS_EXTENT_ITEM_KEY) {
            if (used_insert(fs, k.objectid, k.offset) != 0) { path_free(&p); return -1; }
        } else if (k.type == BTRFS_METADATA_ITEM_KEY) {
            if (used_insert(fs, k.objectid, fs->nodesize) != 0) { path_free(&p); return -1; }
        } else if (k.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
            uint32_t sz;
            const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
            if (!d || sz < 24) { path_free(&p); return -1; }
            btrfs_bg_t *b = bg_add(fs, k.objectid, k.offset, rd64(d), rd64(d + 16));
            if (!b) { path_free(&p); return -1; }
        }
        p.slot++;
    }
    path_free(&p);
    if (rc < 0) return -1;
    fs->alloc_loaded = 1;
    return 0;
}

/* a block whose copy on the device would overlap a superblock or the
 * reserved first megabyte must not be handed out */
static int bt_excluded(btrfs_t *fs, uint64_t logical, uint64_t size)
{
    int i = bt_chunk_index(fs, logical);
    if (i < 0) return 1;
    btrfs_chunk_map_t *c = &fs->chunks[i];
    for (int s = 0; s < c->nstripes && s < 2; s++) {
        uint64_t ph = (s == 0 ? c->physical : c->physical2) + (logical - c->logical);
        if (ph < (1ULL << 20)) return 1;
        if (ph < (64ULL << 20) + 65536 && ph + size > (64ULL << 20)) return 1;
        if (ph < (256ULL << 30) + 65536 && ph + size > (256ULL << 30)) return 1;
    }
    return 0;
}

static int bt_pend_push(btrfs_t *fs, int kind, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    if (fs->pend_n == fs->pend_cap) {
        int nc = fs->pend_cap ? fs->pend_cap * 2 : 64;
        btrfs_pending_t *np = (btrfs_pending_t *)malloc((size_t)nc * sizeof(btrfs_pending_t));
        if (!np) return -1;
        if (fs->pend_n) memcpy(np, fs->pend, (size_t)fs->pend_n * sizeof(btrfs_pending_t));
        free(fs->pend);
        fs->pend = np;
        fs->pend_cap = nc;
    }
    btrfs_pending_t *e = &fs->pend[fs->pend_n++];
    e->kind = kind; e->a = a; e->b = b; e->c = c; e->d = d;
    return 0;
}

/* First-fit allocation inside a block group of the wanted kind. */
static int bt_alloc_space(btrfs_t *fs, uint64_t want, uint64_t size, uint64_t align, uint64_t *out)
{
    if (bt_load_alloc(fs) != 0) return -1;
    for (int bi = 0; bi < fs->nbgs; bi++) {
        btrfs_bg_t *b = &fs->bgs[bi];
        if (!(b->flags & want)) continue;
        if (b->used + size > b->len) continue;
        uint64_t end = b->start + b->len;
        uint64_t cur = b->start;
        int i = used_lower_bound(fs, b->start);
        for (;;) {
            uint64_t cand = align_up64(cur, align);
            for (int guard = 0; guard < 64 && cand + size <= end && bt_excluded(fs, cand, size); guard++) cand += align;
            uint64_t lim = (i < fs->used_n && fs->used[i].start < end) ? fs->used[i].start : end;
            if (cand + size <= lim && !bt_excluded(fs, cand, size)) {
                if (used_insert(fs, cand, size) != 0) return -1;
                b->used += size;
                b->dirty = 1;
                fs->bytes_used += size;
                *out = cand;
                return 0;
            }
            if (i >= fs->used_n || fs->used[i].start >= end) break;
            uint64_t e = fs->used[i].start + fs->used[i].len;
            if (e > cur) cur = e;
            i++;
        }
    }
    return -1;
}

static void bt_free_space(btrfs_t *fs, uint64_t start, uint64_t size)
{
    used_remove(fs, start);
    btrfs_bg_t *b = bg_find(fs, start);
    if (b) {
        b->used = b->used >= size ? b->used - size : 0;
        b->dirty = 1;
    }
    fs->bytes_used = fs->bytes_used >= size ? fs->bytes_used - size : 0;
}

#define BT_PEND_DATA_INSERT 0     /* a = bytenr, b = length, c = inode, d = file offset of the extent */
#define BT_PEND_META_INSERT 1     /* a = bytenr, b = level, c = owner tree */
#define BT_PEND_META_DELETE 2     /* a = bytenr */

static int bt_alloc_tree_block(btrfs_t *fs, uint64_t tree, int level, uint64_t *out)
{
    uint64_t want = tree == BTRFS_CHUNK_TREE_OBJECTID ? BTRFS_BG_SYSTEM : BTRFS_BG_METADATA;
    if (bt_alloc_space(fs, want, fs->nodesize, fs->nodesize, out) != 0) return -1;
    if (bt_pend_push(fs, BT_PEND_META_INSERT, *out, (uint64_t)level, tree, 0) != 0) return -1;
    return 0;
}

static void bt_free_tree_block(btrfs_t *fs, uint64_t bytenr)
{
    bt_free_space(fs, bytenr, fs->nodesize);
    bt_pend_push(fs, BT_PEND_META_DELETE, bytenr, 0, 0, 0);
}

/* ---------- editing tree blocks ---------- */

typedef struct {
    uint8_t key[17];
    uint32_t size;
    const uint8_t *data;
} bt_item_t;

static int leaf_parse(btrfs_t *fs, const uint8_t *buf, bt_item_t *items, uint32_t cap, uint32_t *n_out)
{
    uint32_t n = rd32(buf + 96);
    if (n > cap) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *it = buf + BTRFS_NODE_HDR_SZ + (size_t)i * BTRFS_ITEM_SZ;
        uint32_t off = rd32(it + 17), sz = rd32(it + 21);
        if ((uint64_t)BTRFS_NODE_HDR_SZ + off + sz > fs->nodesize) return -1;
        memcpy(items[i].key, it, 17);
        items[i].size = sz;
        items[i].data = buf + BTRFS_NODE_HDR_SZ + off;
    }
    *n_out = n;
    return 0;
}

static uint32_t leaf_bytes(const bt_item_t *items, uint32_t from, uint32_t to)
{
    uint32_t t = 0;
    for (uint32_t i = from; i < to; i++) t += BTRFS_ITEM_SZ + items[i].size;
    return t;
}

/* lays items out packed in key order, as the kernel's tree checker demands;
 * the header of `buf` stays as it is (apart from the item count) */
static int leaf_build(btrfs_t *fs, uint8_t *buf, const bt_item_t *items, uint32_t n)
{
    uint32_t area = fs->nodesize - BTRFS_NODE_HDR_SZ;
    if (leaf_bytes(items, 0, n) > area) return -1;
    memset(buf + BTRFS_NODE_HDR_SZ, 0, area);
    uint32_t end = area;
    for (uint32_t i = 0; i < n; i++) {
        end -= items[i].size;
        if (items[i].size) memcpy(buf + BTRFS_NODE_HDR_SZ + end, items[i].data, items[i].size);
        uint8_t *it = buf + BTRFS_NODE_HDR_SZ + (size_t)i * BTRFS_ITEM_SZ;
        memcpy(it, items[i].key, 17);
        wr32(it + 17, end);
        wr32(it + 21, items[i].size);
    }
    wr32(buf + 96, n);
    return 0;
}

static void bt_mark_root(btrfs_t *fs, uint64_t tree)
{
    fs->root_dirty |= 1u << (unsigned)tree;
}

/* The first key of a block changed: update the pointers above it. */
static int bt_fix_low_keys(btrfs_t *fs, const bt_path_t *p, int level, const uint8_t *key17)
{
    uint8_t *nb = (uint8_t *)malloc(fs->nodesize);
    if (!nb) return -1;
    for (int l = level; l <= p->level; l++) {
        if (bt_read(fs, p->node[l], fs->nodesize, nb) != 0) { free(nb); return -1; }
        memcpy(nb + BTRFS_NODE_HDR_SZ + (size_t)p->idx[l] * BTRFS_KEY_PTR_SZ, key17, 17);
        if (bt_write_block(fs, p->node[l], nb) != 0) { free(nb); return -1; }
        if (p->idx[l] != 0) break;
    }
    free(nb);
    return 0;
}

/* Inserts a (key, child) pointer at `pos` of the node at `level`; splits the
 * node and grows the tree when it is full. */
static int bt_insert_ptr(btrfs_t *fs, uint64_t tree, const bt_path_t *p, int level, int pos,
                         const uint8_t *key17, uint64_t child, uint64_t cgen)
{
    uint8_t *nb = (uint8_t *)malloc(fs->nodesize);
    if (!nb) return -1;
    if (level > p->level) {
        /* the old root was split: a new root above it points at both halves */
        uint64_t left = p->node[p->level];
        if (bt_read(fs, left, fs->nodesize, nb) != 0) { free(nb); return -1; }
        uint8_t lkey[17];
        if (rd32(nb + 96) == 0) { free(nb); return -1; }
        memcpy(lkey, nb + BTRFS_NODE_HDR_SZ, 17);
        uint64_t lgen = rd64(nb + 80);
        uint64_t nr;
        if (bt_alloc_tree_block(fs, tree, level, &nr) != 0) { free(nb); return -1; }
        bt_blk_init(fs, nb, nr, tree, level, fs->cur_gen);
        uint8_t *e0 = nb + BTRFS_NODE_HDR_SZ;
        memcpy(e0, lkey, 17); wr64(e0 + 17, left); wr64(e0 + 25, lgen);
        uint8_t *e1 = e0 + BTRFS_KEY_PTR_SZ;
        memcpy(e1, key17, 17); wr64(e1 + 17, child); wr64(e1 + 25, cgen);
        wr32(nb + 96, 2);
        int rc = bt_write_block(fs, nr, nb);
        free(nb);
        if (rc != 0) return -1;
        *bt_root_ptr(fs, tree) = nr;
        bt_mark_root(fs, tree);
        return 0;
    }
    if (bt_read(fs, p->node[level], fs->nodesize, nb) != 0) { free(nb); return -1; }
    uint32_t n = rd32(nb + 96);
    uint32_t maxp = (fs->nodesize - BTRFS_NODE_HDR_SZ) / BTRFS_KEY_PTR_SZ;
    if (n > maxp || pos < 0 || (uint32_t)pos > n) { free(nb); return -1; }
    uint8_t *base = nb + BTRFS_NODE_HDR_SZ;
    if (n < maxp) {
        memmove(base + (size_t)(pos + 1) * BTRFS_KEY_PTR_SZ, base + (size_t)pos * BTRFS_KEY_PTR_SZ, (size_t)(n - pos) * BTRFS_KEY_PTR_SZ);
        uint8_t *e = base + (size_t)pos * BTRFS_KEY_PTR_SZ;
        memcpy(e, key17, 17); wr64(e + 17, child); wr64(e + 25, cgen);
        wr32(nb + 96, n + 1);
        int rc = bt_write_block(fs, p->node[level], nb);
        free(nb);
        return rc;
    }
    /* full: spread n+1 pointers over this node and a new right sibling */
    uint8_t *all = (uint8_t *)malloc((size_t)(n + 1) * BTRFS_KEY_PTR_SZ);
    uint8_t *rb = (uint8_t *)malloc(fs->nodesize);
    if (!all || !rb) { free(all); free(rb); free(nb); return -1; }
    memcpy(all, base, (size_t)pos * BTRFS_KEY_PTR_SZ);
    uint8_t *e = all + (size_t)pos * BTRFS_KEY_PTR_SZ;
    memcpy(e, key17, 17); wr64(e + 17, child); wr64(e + 25, cgen);
    memcpy(all + (size_t)(pos + 1) * BTRFS_KEY_PTR_SZ, base + (size_t)pos * BTRFS_KEY_PTR_SZ, (size_t)(n - pos) * BTRFS_KEY_PTR_SZ);
    uint32_t k = (n + 1) / 2;
    uint64_t rn;
    if (bt_alloc_tree_block(fs, tree, level, &rn) != 0) { free(all); free(rb); free(nb); return -1; }
    bt_blk_init(fs, rb, rn, tree, level, fs->cur_gen);
    memcpy(rb + BTRFS_NODE_HDR_SZ, all + (size_t)k * BTRFS_KEY_PTR_SZ, (size_t)(n + 1 - k) * BTRFS_KEY_PTR_SZ);
    wr32(rb + 96, n + 1 - k);
    memset(base, 0, (size_t)maxp * BTRFS_KEY_PTR_SZ);
    memcpy(base, all, (size_t)k * BTRFS_KEY_PTR_SZ);
    wr32(nb + 96, k);
    uint8_t rkey[17];
    memcpy(rkey, all + (size_t)k * BTRFS_KEY_PTR_SZ, 17);
    int rc = bt_write_block(fs, p->node[level], nb);
    if (rc == 0) rc = bt_write_block(fs, rn, rb);
    free(all); free(rb); free(nb);
    if (rc != 0) return -1;
    int ppos = level + 1 <= p->level ? p->idx[level + 1] + 1 : 1;
    return bt_insert_ptr(fs, tree, p, level + 1, ppos, rkey, rn, fs->cur_gen);
}

/* Inserts an item into the leaf `p` points at (slot = insertion point). */
static int bt_leaf_insert(btrfs_t *fs, uint64_t tree, bt_path_t *p, const bt_key_t *key, const void *data, uint32_t size)
{
    uint32_t cap = fs->nodesize / BTRFS_ITEM_SZ + 4;
    bt_item_t *its = (bt_item_t *)malloc(cap * sizeof(bt_item_t));
    uint8_t *old = (uint8_t *)malloc(fs->nodesize);
    uint8_t *rbuf = 0;
    int rc = -1;
    if (!its || !old) goto out;
    memcpy(old, p->leaf, fs->nodesize);
    uint32_t n;
    if (leaf_parse(fs, old, its, cap - 2, &n) != 0) goto out;
    uint32_t slot = (uint32_t)p->slot;
    if (slot > n) goto out;
    memmove(&its[slot + 1], &its[slot], (size_t)(n - slot) * sizeof(bt_item_t));
    key_write(its[slot].key, key);
    its[slot].size = size;
    its[slot].data = (const uint8_t *)data;
    n++;
    uint32_t area = fs->nodesize - BTRFS_NODE_HDR_SZ;
    uint64_t leaf_addr = p->node[0];

    if (leaf_bytes(its, 0, n) <= area) {
        if (leaf_build(fs, p->leaf, its, n) != 0) goto out;
        if (bt_write_block(fs, leaf_addr, p->leaf) != 0) goto out;
        if (slot == 0 && p->level > 0 && bt_fix_low_keys(fs, p, 1, its[0].key) != 0) goto out;
        rc = 0;
        goto out;
    }

    /* the leaf is full: move the upper part (or only the new item when appending) to a new leaf */
    uint32_t k;
    if (slot == n - 1) {
        k = n - 1;
    } else {
        uint32_t total = leaf_bytes(its, 0, n), acc = 0;
        for (k = 0; k < n - 1; k++) {
            acc += BTRFS_ITEM_SZ + its[k].size;
            if (acc >= total / 2) { k++; break; }
        }
        if (k == 0) k = 1;
        if (k >= n) k = n - 1;
    }
    if (leaf_bytes(its, 0, k) > area || leaf_bytes(its, k, n) > area) goto out;
    uint64_t rb_addr;
    if (bt_alloc_tree_block(fs, tree, 0, &rb_addr) != 0) goto out;
    rbuf = (uint8_t *)malloc(fs->nodesize);
    if (!rbuf) goto out;
    bt_blk_init(fs, rbuf, rb_addr, tree, 0, fs->cur_gen);
    if (leaf_build(fs, rbuf, its + k, n - k) != 0) goto out;
    if (leaf_build(fs, p->leaf, its, k) != 0) goto out;
    if (bt_write_block(fs, rb_addr, rbuf) != 0) goto out;
    if (bt_write_block(fs, leaf_addr, p->leaf) != 0) goto out;
    if (slot == 0 && p->level > 0 && bt_fix_low_keys(fs, p, 1, its[0].key) != 0) goto out;
    int ppos = p->level >= 1 ? p->idx[1] + 1 : 1;
    if (bt_insert_ptr(fs, tree, p, 1, ppos, its[k].key, rb_addr, fs->cur_gen) != 0) goto out;
    rc = 0;
out:
    free(its); free(old); free(rbuf);
    return rc;
}

static int bt_insert_item(btrfs_t *fs, uint64_t tree, const bt_key_t *key, const void *data, uint32_t size)
{
    uint64_t *rootp = bt_root_ptr(fs, tree);
    if (!rootp || !fs->in_txn) return -1;
    bt_path_t p;
    if (bt_search(fs, *rootp, key, &p) != 0) { path_free(&p); return -1; }
    if (p.slot < p.nritems) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (key_cmp(&k, key) == 0) { path_free(&p); return -2; }
    }
    int rc = bt_leaf_insert(fs, tree, &p, key, data, size);
    path_free(&p);
    return rc;
}

/* Removes the pointer at p->idx[level] of the node at `level`; an emptied
 * node is released and removed from its parent in turn. */
static int bt_delete_ptr(btrfs_t *fs, uint64_t tree, const bt_path_t *p, int level)
{
    uint8_t *nb = (uint8_t *)malloc(fs->nodesize);
    if (!nb) return -1;
    if (bt_read(fs, p->node[level], fs->nodesize, nb) != 0) { free(nb); return -1; }
    uint32_t n = rd32(nb + 96);
    int idx = p->idx[level];
    if (idx < 0 || (uint32_t)idx >= n) { free(nb); return -1; }
    uint8_t *base = nb + BTRFS_NODE_HDR_SZ;
    memmove(base + (size_t)idx * BTRFS_KEY_PTR_SZ, base + (size_t)(idx + 1) * BTRFS_KEY_PTR_SZ, (size_t)(n - idx - 1) * BTRFS_KEY_PTR_SZ);
    memset(base + (size_t)(n - 1) * BTRFS_KEY_PTR_SZ, 0, BTRFS_KEY_PTR_SZ);
    wr32(nb + 96, n - 1);
    int rc;
    if (n - 1 > 0) {
        rc = bt_write_block(fs, p->node[level], nb);
        if (rc == 0 && idx == 0 && level + 1 <= p->level) rc = bt_fix_low_keys(fs, p, level + 1, base);
        free(nb);
        return rc;
    }
    if (level == p->level) {
        /* the root lost its last pointer: the tree is an empty leaf again */
        nb[100] = 0;
        rc = bt_write_block(fs, p->node[level], nb);
        bt_mark_root(fs, tree);
        free(nb);
        return rc;
    }
    free(nb);
    bt_free_tree_block(fs, p->node[level]);
    return bt_delete_ptr(fs, tree, p, level + 1);
}

/* Removes the item at p->slot of p's leaf. */
static int bt_delete_slot(btrfs_t *fs, uint64_t tree, bt_path_t *p)
{
    if (!fs->in_txn) return -1;
    uint32_t cap = fs->nodesize / BTRFS_ITEM_SZ + 4;
    bt_item_t *its = (bt_item_t *)malloc(cap * sizeof(bt_item_t));
    uint8_t *old = (uint8_t *)malloc(fs->nodesize);
    int rc = -1;
    if (!its || !old) goto out;
    memcpy(old, p->leaf, fs->nodesize);
    uint32_t n;
    if (leaf_parse(fs, old, its, cap - 2, &n) != 0) goto out;
    uint32_t slot = (uint32_t)p->slot;
    if (slot >= n) goto out;
    memmove(&its[slot], &its[slot + 1], (size_t)(n - slot - 1) * sizeof(bt_item_t));
    n--;
    if (n > 0 || p->level == 0) {
        if (leaf_build(fs, p->leaf, its, n) != 0) goto out;
        if (bt_write_block(fs, p->node[0], p->leaf) != 0) goto out;
        if (n > 0 && slot == 0 && p->level > 0 && bt_fix_low_keys(fs, p, 1, its[0].key) != 0) goto out;
        rc = 0;
        goto out;
    }
    bt_free_tree_block(fs, p->node[0]);
    rc = bt_delete_ptr(fs, tree, p, 1);
out:
    free(its); free(old);
    return rc;
}

/* Replaces the item at p->slot by one with new contents (any size). */
static int bt_replace_slot(btrfs_t *fs, uint64_t tree, bt_path_t *p, const void *data, uint32_t size)
{
    bt_key_t key;
    leaf_key(p, p->slot, &key);
    uint32_t sz;
    const uint8_t *cur = leaf_data(fs, p, p->slot, &sz);
    if (cur && sz == size) {                      /* same size: patch in place */
        memcpy((uint8_t *)cur, data, size);
        return bt_write_block(fs, p->node[0], p->leaf);
    }
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
    if (!copy) return -1;
    memcpy(copy, data, size);
    int rc = bt_delete_slot(fs, tree, p);
    if (rc == 0) rc = bt_insert_item(fs, tree, &key, copy, size);
    free(copy);
    return rc;
}

/* Finds the item with exactly this key; fills the path (slot on the item). */
static int bt_find_exact(btrfs_t *fs, uint64_t tree, const bt_key_t *key, bt_path_t *p)
{
    uint64_t *rootp = bt_root_ptr(fs, tree);
    if (!rootp) return -1;
    if (bt_search(fs, *rootp, key, p) != 0) { path_free(p); return -1; }
    if (p->slot < p->nritems) {
        bt_key_t k;
        leaf_key(p, p->slot, &k);
        if (key_cmp(&k, key) == 0) return 0;
    }
    path_free(p);
    return 1;
}

/* Moves to the previous item (possibly in the previous leaf). 0 = moved, 1 = none. */
static int bt_prev_item(btrfs_t *fs, bt_path_t *p)
{
    if (p->slot > 0) { p->slot--; return 0; }
    uint8_t *nb = (uint8_t *)malloc(fs->nodesize);
    if (!nb) return -1;
    for (int lvl = 1; lvl <= p->level; lvl++) {
        if (p->idx[lvl] == 0) continue;
        uint8_t level;
        uint32_t n;
        if (bt_read(fs, p->node[lvl], fs->nodesize, nb) != 0 || node_check(fs, nb, &level, &n) != 0) { free(nb); return -1; }
        p->idx[lvl]--;
        uint64_t cur = rd64(nb + BTRFS_NODE_HDR_SZ + (size_t)p->idx[lvl] * BTRFS_KEY_PTR_SZ + 17);
        for (int l = lvl - 1; l >= 0; l--) {
            uint8_t *dst = l == 0 ? p->leaf : nb;
            if (bt_read(fs, cur, fs->nodesize, dst) != 0 || node_check(fs, dst, &level, &n) != 0 || n == 0) { free(nb); return -1; }
            p->node[l] = cur;
            if (l == 0) {
                p->nritems = (int)n;
                p->slot = (int)n - 1;
                free(nb);
                return 0;
            }
            p->idx[l] = (int)n - 1;
            cur = rd64(dst + BTRFS_NODE_HDR_SZ + (size_t)(n - 1) * BTRFS_KEY_PTR_SZ + 17);
        }
    }
    free(nb);
    return 1;
}

/* ---------- extent tree: references ---------- */

#define BT_EXTENT_FLAG_DATA        1ULL
#define BT_EXTENT_FLAG_TREE_BLOCK  2ULL

static uint32_t inline_ref_size(uint8_t type)
{
    switch (type) {
    case BTRFS_TREE_BLOCK_REF_KEY: return 9;
    case 0xB6: return 9;          /* SHARED_BLOCK_REF */
    case BTRFS_EXTENT_DATA_REF_KEY: return 29;
    case 0xB8: return 13;         /* SHARED_DATA_REF */
    default: return 0;
    }
}

static int bt_csum_delete_range(btrfs_t *fs, uint64_t start, uint64_t end);

/* Locates the EXTENT_ITEM of the data extent starting at `bytenr`. */
static int bt_find_data_extent(btrfs_t *fs, uint64_t bytenr, bt_path_t *p, bt_key_t *key)
{
    bt_key_t k0 = { bytenr, BTRFS_EXTENT_ITEM_KEY, 0 };
    if (bt_search(fs, fs->extent_root, &k0, p) != 0) { path_free(p); return -1; }
    if (bt_valid(fs, p) != 0) { path_free(p); return 1; }
    leaf_key(p, p->slot, key);
    if (key->objectid != bytenr || key->type != BTRFS_EXTENT_ITEM_KEY) { path_free(p); return 1; }
    return 0;
}

/* Changes the reference count of (root, ino, refoff) on a data extent by
 * +1 or -1. Dropping the last reference frees the extent. Only references
 * stored inline in the extent item are handled; anything else is refused. */
static int bt_data_ref_adjust(btrfs_t *fs, uint64_t bytenr, uint64_t root, uint64_t ino, uint64_t refoff, int delta)
{
    bt_path_t p;
    bt_key_t key;
    if (bt_find_data_extent(fs, bytenr, &p, &key) != 0) return -1;
    uint32_t sz;
    const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
    if (!d || sz < 24 || !(rd64(d + 16) & BT_EXTENT_FLAG_DATA)) { path_free(&p); return -1; }
    uint8_t *buf = (uint8_t *)malloc(sz + 32);
    if (!buf) { path_free(&p); return -1; }
    memcpy(buf, d, sz);
    uint64_t refs = rd64(buf);
    uint32_t pos = 24, found = 0;
    while (pos < sz) {
        uint32_t rs = inline_ref_size(buf[pos]);
        if (rs == 0 || pos + rs > sz) { free(buf); path_free(&p); return -1; }
        if (buf[pos] == BTRFS_EXTENT_DATA_REF_KEY && rd64(buf + pos + 1) == root && rd64(buf + pos + 9) == ino &&
            rd64(buf + pos + 17) == refoff) { found = pos; break; }
        pos += rs;
    }
    if (!found) { free(buf); path_free(&p); return -1; }
    uint32_t count = rd32(buf + found + 25);
    int rc;
    uint32_t newsz = sz;
    if (delta > 0) {
        wr32(buf + found + 25, count + 1);
        refs++;
    } else if (count > 1) {
        wr32(buf + found + 25, count - 1);
        refs--;
    } else {
        memmove(buf + found, buf + found + 29, sz - found - 29);
        newsz = sz - 29;
        refs--;
    }
    wr64(buf, refs);
    uint64_t len = key.offset;
    if (refs == 0) {
        rc = bt_delete_slot(fs, BTRFS_EXTENT_TREE_OBJECTID, &p);
        path_free(&p);
        free(buf);
        if (rc != 0) return -1;
        bt_free_space(fs, bytenr, len);
        return bt_csum_delete_range(fs, bytenr, bytenr + len);
    }
    rc = bt_replace_slot(fs, BTRFS_EXTENT_TREE_OBJECTID, &p, buf, newsz);
    path_free(&p);
    free(buf);
    return rc;
}

/* ---------- checksum tree ---------- */

static uint32_t bt_csum_sector(btrfs_t *fs, const uint8_t *data)
{
    return ~crc32c_update(0xFFFFFFFFu, data, fs->sectorsize);
}

#define BT_CSUM_ITEM_SECTORS 1024u

static int bt_csum_insert(btrfs_t *fs, uint64_t bytenr, const uint8_t *data, uint32_t len)
{
    uint32_t ss = fs->sectorsize, nsec = len / ss, done = 0;
    uint32_t *buf = (uint32_t *)malloc(BT_CSUM_ITEM_SECTORS * 4);
    if (!buf) return -1;
    while (done < nsec) {
        uint32_t cnt = nsec - done < BT_CSUM_ITEM_SECTORS ? nsec - done : BT_CSUM_ITEM_SECTORS;
        for (uint32_t i = 0; i < cnt; i++) buf[i] = bt_csum_sector(fs, data + (size_t)(done + i) * ss);
        bt_key_t k = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr + (uint64_t)done * ss };
        if (bt_insert_item(fs, BTRFS_CSUM_TREE_OBJECTID, &k, buf, cnt * 4) != 0) { free(buf); return -1; }
        done += cnt;
    }
    free(buf);
    return 0;
}

/* positions p on the checksum item that covers `start` or the first one after it */
static int bt_csum_first_overlap(btrfs_t *fs, uint64_t start, bt_path_t *p)
{
    bt_key_t k0 = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, start };
    if (bt_search(fs, fs->csum_root, &k0, p) != 0) { path_free(p); return -1; }
    bt_path_t q = *p;                 /* look at the item before the search position */
    q.leaf = (uint8_t *)malloc(fs->nodesize);
    if (!q.leaf) { path_free(p); return -1; }
    memcpy(q.leaf, p->leaf, fs->nodesize);
    int rc = bt_prev_item(fs, &q);
    if (rc == 0) {
        bt_key_t k;
        leaf_key(&q, q.slot, &k);
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &q, q.slot, &sz);
        if (d && k.objectid == BTRFS_EXTENT_CSUM_OBJECTID && k.type == BTRFS_EXTENT_CSUM_KEY &&
            k.offset + (uint64_t)(sz / 4) * fs->sectorsize > start) {
            path_free(p);
            *p = q;
            return 0;
        }
    }
    path_free(&q);
    if (rc < 0) { path_free(p); return -1; }
    if (bt_valid(fs, p) != 0) { path_free(p); return 1; }
    bt_key_t k;
    leaf_key(p, p->slot, &k);
    if (k.objectid != BTRFS_EXTENT_CSUM_OBJECTID || k.type != BTRFS_EXTENT_CSUM_KEY) { path_free(p); return 1; }
    return 0;
}

static int bt_csum_delete_range(btrfs_t *fs, uint64_t start, uint64_t end)
{
    uint32_t ss = fs->sectorsize;
    for (int guard = 0; guard < 1000000; guard++) {
        bt_path_t p;
        int rc = bt_csum_first_overlap(fs, start, &p);
        if (rc < 0) return -1;
        if (rc > 0) return 0;
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (!d) { path_free(&p); return -1; }
        uint64_t is = k.offset, ie = is + (uint64_t)(sz / 4) * ss;
        if (is >= end) { path_free(&p); return 0; }
        uint8_t *copy = (uint8_t *)malloc(sz ? sz : 1);
        if (!copy) { path_free(&p); return -1; }
        memcpy(copy, d, sz);
        int r;
        if (is >= start && ie <= end) {
            r = bt_delete_slot(fs, BTRFS_CSUM_TREE_OBJECTID, &p);
        } else if (is < start && ie > end) {
            uint32_t head = (uint32_t)((start - is) / ss) * 4;
            uint32_t tail_off = (uint32_t)((end - is) / ss) * 4;
            r = bt_replace_slot(fs, BTRFS_CSUM_TREE_OBJECTID, &p, copy, head);
            if (r == 0) {
                bt_key_t tk = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end };
                r = bt_insert_item(fs, BTRFS_CSUM_TREE_OBJECTID, &tk, copy + tail_off, sz - tail_off);
            }
        } else if (is < start) {
            uint32_t head = (uint32_t)((start - is) / ss) * 4;
            r = bt_replace_slot(fs, BTRFS_CSUM_TREE_OBJECTID, &p, copy, head);
        } else {
            uint32_t tail_off = (uint32_t)((end - is) / ss) * 4;
            r = bt_delete_slot(fs, BTRFS_CSUM_TREE_OBJECTID, &p);
            if (r == 0) {
                bt_key_t tk = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end };
                r = bt_insert_item(fs, BTRFS_CSUM_TREE_OBJECTID, &tk, copy + tail_off, sz - tail_off);
            }
        }
        path_free(&p);
        free(copy);
        if (r != 0) return -1;
    }
    return -1;
}

/* ---------- transactions ---------- */

static int bt_process_pending(btrfs_t *fs)
{
    int guard = 0;
    while (fs->pend_n > 0) {
        if (++guard > 1000000) return -1;
        btrfs_pending_t e = fs->pend[0];
        memmove(&fs->pend[0], &fs->pend[1], (size_t)(fs->pend_n - 1) * sizeof(btrfs_pending_t));
        fs->pend_n--;
        if (e.kind == BT_PEND_DATA_INSERT) {
            uint8_t d[53];
            memset(d, 0, sizeof(d));
            wr64(d, 1);
            wr64(d + 8, fs->cur_gen);
            wr64(d + 16, BT_EXTENT_FLAG_DATA);
            d[24] = BTRFS_EXTENT_DATA_REF_KEY;
            wr64(d + 25, BTRFS_FS_TREE_OBJECTID);
            wr64(d + 33, e.c);
            wr64(d + 41, e.d);
            wr32(d + 49, 1);
            bt_key_t k = { e.a, BTRFS_EXTENT_ITEM_KEY, e.b };
            if (bt_insert_item(fs, BTRFS_EXTENT_TREE_OBJECTID, &k, d, sizeof(d)) != 0) return -1;
        } else if (e.kind == BT_PEND_META_INSERT) {
            uint8_t d[33];
            memset(d, 0, sizeof(d));
            wr64(d, 1);
            wr64(d + 8, fs->cur_gen);
            wr64(d + 16, BT_EXTENT_FLAG_TREE_BLOCK);
            d[24] = BTRFS_TREE_BLOCK_REF_KEY;
            wr64(d + 25, e.c);
            bt_key_t k = { e.a, BTRFS_METADATA_ITEM_KEY, e.b };
            if (bt_insert_item(fs, BTRFS_EXTENT_TREE_OBJECTID, &k, d, sizeof(d)) != 0) return -1;
        } else if (e.kind == BT_PEND_META_DELETE) {
            bt_key_t k0 = { e.a, BTRFS_METADATA_ITEM_KEY, 0 };
            bt_path_t p;
            if (bt_search(fs, fs->extent_root, &k0, &p) != 0) { path_free(&p); return -1; }
            bt_key_t k;
            int ok = bt_valid(fs, &p) == 0;
            if (ok) { leaf_key(&p, p.slot, &k); ok = k.objectid == e.a && k.type == BTRFS_METADATA_ITEM_KEY; }
            int rc = ok ? bt_delete_slot(fs, BTRFS_EXTENT_TREE_OBJECTID, &p) : -1;
            path_free(&p);
            if (rc != 0) return -1;
        }
    }
    return 0;
}

static int bt_write_super(btrfs_t *fs)
{
    uint8_t *sb = fs->sb;
    wr64(sb + 0x48, fs->cur_gen);
    wr64(sb + 0x50, fs->root_logical);
    wr64(sb + 0x58, fs->chunk_root_logical);
    wr64(sb + 0x78, fs->bytes_used);
    /* the root tree's root block is checked against the superblock's generation */
    {
        uint8_t *rb = (uint8_t *)malloc(fs->nodesize);
        if (!rb) return -1;
        if (bt_read(fs, fs->root_logical, fs->nodesize, rb) != 0) { free(rb); return -1; }
        wr64(rb + 80, fs->cur_gen);
        int wrc = bt_write_block(fs, fs->root_logical, rb);
        free(rb);
        if (wrc != 0) return -1;
    }
    uint8_t hdr[BTRFS_NODE_HDR_SZ];
    if (bt_read(fs, fs->root_logical, sizeof(hdr), hdr) != 0) return -1;
    sb[0xC6] = hdr[100];
    if (bt_read(fs, fs->chunk_root_logical, sizeof(hdr), hdr) != 0) return -1;
    sb[0xC7] = hdr[100];
    wr64(sb + 0xA4, rd64(hdr + 80));
    uint64_t cro = rd64(sb + 0xB4);
    wr64(sb + 0xB4, fs->has_fst ? (cro & ~2ULL) : (cro & ~3ULL));   /* no (valid) free-space tree: Linux builds its own */
    memset(sb, 0, 4);
    wr32(sb, ~crc32c_update(0xFFFFFFFFu, sb + 32, 4096 - 32));
    memset(sb + 4, 0, 28);
    static const uint64_t offs[3] = { BTRFS_SUPER_OFFSET, 64ULL << 20, 256ULL << 30 };
    for (int i = 0; i < 3; i++) {
        if (offs[i] + 4096 > fs->dev_size) break;
        /* superblock copies carry their own bytenr */
        uint8_t copy[4096];
        memcpy(copy, sb, 4096);
        wr64(copy + 0x30, offs[i]);
        wr32(copy, ~crc32c_update(0xFFFFFFFFu, copy + 32, 4096 - 32));
        memset(copy + 4, 0, 28);
        if (blockdev_write_bytes(fs->bd, offs[i], 4096, copy) != 0) return -1;
    }
    return 0;
}

static int bt_update_root_items(btrfs_t *fs)
{
    static const uint64_t trees[] = { BTRFS_EXTENT_TREE_OBJECTID, BTRFS_DEV_TREE_OBJECTID, BTRFS_FS_TREE_OBJECTID, BTRFS_CSUM_TREE_OBJECTID };
    uint8_t hdr[BTRFS_NODE_HDR_SZ];
    for (unsigned i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
        uint64_t t = trees[i];
        if (!(fs->root_dirty & (1u << (unsigned)t))) continue;
        uint64_t rb = *bt_root_ptr(fs, t);
        if (bt_read(fs, rb, sizeof(hdr), hdr) != 0) return -1;
        bt_key_t k = { t, BTRFS_ROOT_ITEM_KEY, 0 };
        bt_path_t p;
        int rc = bt_find_exact(fs, BTRFS_ROOT_TREE_OBJECTID, &k, &p);
        if (rc != 0) return -1;
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (!d || sz < 247) { path_free(&p); return -1; }
        uint8_t *w = (uint8_t *)d;
        wr64(w + 160, rd64(hdr + 80));
        wr64(w + 176, rb);
        w[238] = hdr[100];
        wr64(w + 239, rd64(hdr + 80));
        rc = bt_write_block(fs, p.node[0], p.leaf);
        path_free(&p);
        if (rc != 0) return -1;
    }
    fs->root_dirty = 0;
    return 0;
}

static int bt_update_bg_items(btrfs_t *fs)
{
    for (int i = 0; i < fs->nbgs; i++) {
        btrfs_bg_t *b = &fs->bgs[i];
        if (!b->dirty) continue;
        bt_key_t k = { b->start, BTRFS_BLOCK_GROUP_ITEM_KEY, b->len };
        bt_path_t p;
        if (bt_find_exact(fs, BTRFS_EXTENT_TREE_OBJECTID, &k, &p) != 0) return -1;
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (!d || sz < 24) { path_free(&p); return -1; }
        wr64((uint8_t *)d, b->used);
        int rc = bt_write_block(fs, p.node[0], p.leaf);
        path_free(&p);
        if (rc != 0) return -1;
        b->dirty = 0;
    }
    return 0;
}

/* ---------- allocating new chunks ---------- */

#define BTRFS_DEV_ITEM_KEY 0xD8
#define BTRFS_DEV_EXTENT_KEY 0xCC
#define BTRFS_DEV_ITEMS_OBJECTID 1ULL

/* first device hole of `size` bytes at or above `from`, outside what the dev tree already hands out */
static int bt_dev_hole(btrfs_t *fs, uint64_t from, uint64_t size, uint64_t *out)
{
    uint64_t dev_total = rd64(fs->sb + 0xD1);
    if (dev_total > fs->dev_size) dev_total = fs->dev_size;
    uint64_t cur = from;
    bt_key_t first = { 1, BTRFS_DEV_EXTENT_KEY, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->dev_root, &first, &p) != 0) { path_free(&p); return -1; }
    int rc;
    while ((rc = bt_valid(fs, &p)) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid != 1 || k.type != BTRFS_DEV_EXTENT_KEY) break;
        uint32_t sz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
        if (!d || sz < 48) { path_free(&p); return -1; }
        uint64_t o = k.offset, l = rd64(d + 24);
        if (o >= cur + size) { path_free(&p); *out = cur; return 0; }
        if (o + l > cur) cur = align_up64(o + l, 1ULL << 20);
        p.slot++;
    }
    path_free(&p);
    if (rc < 0) return -1;
    if (cur + size <= dev_total) { *out = cur; return 0; }
    return 1;
}

/* Creates a new block group of the given type (DATA or METADATA): a chunk
 * item, its device extents, the block-group item and the device usage. */
static int bt_chunk_alloc(btrfs_t *fs, uint64_t type)
{
    if (bt_load_alloc(fs) != 0) return -1;
    uint64_t profile = 0;
    for (int i = 0; i < fs->nbgs; i++)
        if (fs->bgs[i].flags & type) { profile = fs->bgs[i].flags & (BTRFS_BG_DUP | BTRFS_BG_RAID1); break; }
    int nstripes = profile ? 2 : 1;

    uint64_t sz = type == BTRFS_BG_DATA ? (1ULL << 30) : (256ULL << 20);
    uint64_t cap = fs->total_bytes / 10;
    if (cap < (8ULL << 20)) cap = 8ULL << 20;
    if (sz > cap) sz = cap;
    sz &= ~((1ULL << 20) - 1);
    uint64_t phys[2] = { 0, 0 };
    for (;;) {
        int rc = bt_dev_hole(fs, 1ULL << 20, sz, &phys[0]);
        if (rc < 0) return -1;
        if (rc == 0 && nstripes == 2) {
            /* the second copy must not overlap the first: look after it */
            rc = bt_dev_hole(fs, phys[0] + sz, sz, &phys[1]);
            if (rc < 0) return -1;
            if (rc > 0) rc = bt_dev_hole(fs, 1ULL << 20, sz, &phys[1]) == 0 && (phys[1] + sz <= phys[0] || phys[1] >= phys[0] + sz) ? 0 : 1;
        }
        if (rc == 0) break;
        sz /= 2;
        sz &= ~((1ULL << 20) - 1);
        if (sz < (4ULL << 20)) return -1;                  /* the device is full */
    }

    uint64_t logical = 0;
    for (int i = 0; i < fs->num_chunks; i++) {
        uint64_t e = fs->chunks[i].logical + fs->chunks[i].length;
        if (e > logical) logical = e;
    }
    logical = align_up64(logical, 1ULL << 20);

    uint64_t flags = type | profile;
    uint8_t dev_uuid[16];
    memcpy(dev_uuid, fs->sb + 0xC9 + 0x42, 16);          /* btrfs_dev_item.uuid */

    uint8_t ci[48 + 64];
    memset(ci, 0, sizeof(ci));
    wr64(ci, sz);
    wr64(ci + 8, BTRFS_EXTENT_TREE_OBJECTID);
    wr64(ci + 16, 65536);
    wr64(ci + 24, flags);
    wr32(ci + 32, 65536);
    wr32(ci + 36, 65536);
    wr32(ci + 40, fs->sectorsize);
    wr16(ci + 44, (uint16_t)nstripes);
    wr16(ci + 46, 1);
    for (int s = 0; s < nstripes; s++) {
        uint8_t *st = ci + 48 + s * 32;
        wr64(st, 1);
        wr64(st + 8, phys[s]);
        memcpy(st + 16, dev_uuid, 16);
    }
    bt_key_t ck = { BTRFS_FIRST_CHUNK_OBJECTID, BTRFS_CHUNK_ITEM_KEY, logical };
    if (bt_insert_item(fs, BTRFS_CHUNK_TREE_OBJECTID, &ck, ci, 48u + 32u * (uint32_t)nstripes) != 0) return -1;

    for (int s = 0; s < nstripes; s++) {
        uint8_t de[48];
        memset(de, 0, sizeof(de));
        wr64(de, BTRFS_CHUNK_TREE_OBJECTID);
        wr64(de + 8, BTRFS_FIRST_CHUNK_OBJECTID);
        wr64(de + 16, logical);
        wr64(de + 24, sz);
        memcpy(de + 32, fs->hdr_chunk_uuid, 16);
        bt_key_t dk = { 1, BTRFS_DEV_EXTENT_KEY, phys[s] };
        if (bt_insert_item(fs, BTRFS_DEV_TREE_OBJECTID, &dk, de, sizeof(de)) != 0) return -1;
    }

    uint8_t bg[24];
    wr64(bg, 0);
    wr64(bg + 8, BTRFS_FIRST_CHUNK_OBJECTID);
    wr64(bg + 16, flags);
    bt_key_t bk = { logical, BTRFS_BLOCK_GROUP_ITEM_KEY, sz };
    if (bt_insert_item(fs, BTRFS_EXTENT_TREE_OBJECTID, &bk, bg, sizeof(bg)) != 0) return -1;

    /* device usage: the DEV_ITEM in the chunk tree and its copy in the superblock */
    {
        bt_key_t dik = { BTRFS_DEV_ITEMS_OBJECTID, BTRFS_DEV_ITEM_KEY, 1 };
        bt_path_t p;
        if (bt_find_exact(fs, BTRFS_CHUNK_TREE_OBJECTID, &dik, &p) != 0) return -1;
        uint32_t dsz;
        const uint8_t *d = leaf_data(fs, &p, p.slot, &dsz);
        if (!d || dsz < 98) { path_free(&p); return -1; }
        uint64_t used = rd64(d + 16) + (uint64_t)nstripes * sz;
        wr64((uint8_t *)d + 16, used);
        int rc = bt_write_block(fs, p.node[0], p.leaf);
        path_free(&p);
        if (rc != 0) return -1;
        wr64(fs->sb + 0xC9 + 16, used);
    }

    chunk_add(fs, logical, sz, phys[0], phys[1], flags, nstripes);
    btrfs_bg_t *nbg = bg_add(fs, logical, sz, 0, flags);
    if (!nbg) return -1;
    nbg->dirty = 0;
    return 0;
}

/* data and metadata room for one operation, growing the volume by new chunks when needed */
static int bt_space_ok(btrfs_t *fs, uint64_t data)
{
    if (bt_load_alloc(fs) != 0) return 0;
    for (int t = 0; t < 4; t++) {
        uint64_t dfree = 0, mfree = 0;
        for (int i = 0; i < fs->nbgs; i++) {
            uint64_t f = fs->bgs[i].len - fs->bgs[i].used;
            if (fs->bgs[i].flags & BTRFS_BG_DATA) dfree += f;
            if (fs->bgs[i].flags & BTRFS_BG_METADATA) mfree += f;
        }
        int need_m = mfree < (uint64_t)fs->nodesize * 16;
        int need_d = dfree < data;
        if (!need_m && !need_d) return 1;
        if (bt_chunk_alloc(fs, need_m ? BTRFS_BG_METADATA : BTRFS_BG_DATA) != 0) return 0;
    }
    return 0;
}

#define BTRFS_FREE_SPACE_TREE_OBJECTID 10ULL

/* frees a tree block and everything below it */
static int bt_free_subtree(btrfs_t *fs, uint64_t addr, int depth)
{
    uint8_t *buf = (uint8_t *)malloc(fs->nodesize);
    if (!buf || depth > BT_MAX_LEVEL) { free(buf); return -1; }
    if (bt_read(fs, addr, fs->nodesize, buf) != 0) { free(buf); return -1; }
    uint8_t level;
    uint32_t n;
    if (node_check(fs, buf, &level, &n) != 0) { free(buf); return -1; }
    if (level > 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint64_t child = rd64(buf + BTRFS_NODE_HDR_SZ + (size_t)i * BTRFS_KEY_PTR_SZ + 17);
            if (bt_free_subtree(fs, child, depth + 1) != 0) { free(buf); return -1; }
        }
    }
    free(buf);
    bt_free_tree_block(fs, addr);
    return 0;
}

/* The free-space tree is not kept up to date by this driver, and a stale one
 * would fail `btrfs check`: remove it the way the kernel's "clear free space
 * tree" does (blocks freed, root item deleted, feature bits cleared). */
static int bt_drop_free_space_tree(btrfs_t *fs)
{
    uint64_t root;
    if (bt_load_alloc(fs) != 0) return -1;
    if (find_subvol_root(fs, BTRFS_FREE_SPACE_TREE_OBJECTID, &root) == 0) {
        if (bt_free_subtree(fs, root, 0) != 0) return -1;
        bt_key_t k = { BTRFS_FREE_SPACE_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0 };
        bt_path_t p;
        if (bt_find_exact(fs, BTRFS_ROOT_TREE_OBJECTID, &k, &p) != 0) return -1;
        int rc = bt_delete_slot(fs, BTRFS_ROOT_TREE_OBJECTID, &p);
        path_free(&p);
        if (rc != 0) return -1;
    }
    fs->has_fst = 0;
    return 0;
}

static int bt_txn_begin(btrfs_t *fs)
{
    fs->cur_gen = fs->generation + 1;
    fs->in_txn = 1;
    if (fs->has_fst && bt_drop_free_space_tree(fs) != 0) {
        klog_write("btrfs: cannot drop the free-space tree\n");
        return -1;
    }
    if (!bt_space_ok(fs, 0)) return -1;
    return 0;
}

static int bt_txn_commit(btrfs_t *fs)
{
    int rc = bt_process_pending(fs);
    if (rc == 0) rc = bt_update_root_items(fs);
    if (rc == 0) rc = bt_update_bg_items(fs);
    if (rc == 0 && fs->root_dirty) rc = bt_update_root_items(fs);
    fs->in_txn = 0;
    if (rc != 0) { klog_write("btrfs: transaction failed\n"); return -1; }
    if (bt_write_super(fs) != 0) return -1;
    fs->generation = fs->cur_gen;
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

static uint32_t name_hash(const char *name, int nlen)
{
    return crc32c_update(0xFFFFFFFEu, name, (size_t)nlen);    /* btrfs_name_hash = crc32c(~1, name) */
}

static int dir_lookup(btrfs_t *fs, uint64_t dir, const char *name, bt_dirent_t *out)
{
    int nlen = (int)strlen(name);
    if (nlen == 0 || nlen > BTRFS_MAX_FILENAME) return -1;
    uint32_t hash = name_hash(name, nlen);
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

/* a file extent item, decoded */
typedef struct {
    uint64_t off;          /* file offset (the item's key) */
    uint8_t type;          /* 0 inline, 1 regular, 2 preallocated */
    uint8_t comp, enc;
    uint64_t ram;
    uint64_t disk, dnum;   /* the disk extent (0 = hole) */
    uint64_t eoff;         /* where in the disk extent this item starts */
    uint64_t num;          /* bytes of file covered */
} bt_fext_t;

static int fext_parse(uint64_t key_off, const uint8_t *d, uint32_t sz, bt_fext_t *e)
{
    if (sz < 21) return -1;
    memset(e, 0, sizeof(*e));
    e->off = key_off;
    e->ram = rd64(d + 8);
    e->comp = d[16];
    e->enc = d[17];
    e->type = d[20];
    if (e->type == 0) {
        e->num = sz - 21;
        return 0;
    }
    if ((e->type != 1 && e->type != 2) || sz < 53) return -1;
    e->disk = rd64(d + 21);
    e->dnum = rd64(d + 29);
    e->eoff = rd64(d + 37);
    e->num = rd64(d + 45);
    return 0;
}

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
        bt_fext_t e;
        if (!d || fext_parse(k.offset, d, sz, &e) != 0) { rc = -1; break; }
        if (e.comp != 0 || e.enc != 0) { rc = -1; break; }
        uint64_t fo = e.off;
        uint64_t ext_len = e.num;

        if (fo + ext_len > pos) {
            uint64_t from = pos > fo ? pos : fo;
            uint64_t to = pos + len < fo + ext_len ? pos + len : fo + ext_len;
            if (to > from) {
                uint32_t n = (uint32_t)(to - from);
                uint8_t *dst = buf + (from - pos);
                if (e.type == 0) {
                    memcpy(dst, d + 21 + (from - fo), n);
                } else if (e.type == 1) {
                    if (e.disk != 0 && bt_read(fs, e.disk + e.eoff + (from - fo), n, dst) != 0) { rc = -1; break; }
                }   /* prealloc and holes stay zero */
            }
        }
        p.slot++;
    }
    path_free(&p);
    return rc;
}

/* ---------- inodes and directories (writing) ---------- */

static int inode_load(btrfs_t *fs, uint64_t ino, uint8_t *item)
{
    bt_key_t key = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    bt_path_t p;
    if (bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, &key, &p) != 0) return -1;
    uint32_t sz;
    const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
    int rc = -1;
    if (d && sz >= 160) { memcpy(item, d, 160); rc = 0; }
    path_free(&p);
    return rc;
}

static int inode_store(btrfs_t *fs, uint64_t ino, const uint8_t *item)
{
    bt_key_t key = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    bt_path_t p;
    if (bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, &key, &p) != 0) return -1;
    uint32_t sz;
    const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
    int rc = -1;
    if (d && sz >= 160) {
        memcpy((uint8_t *)d, item, 160);
        rc = bt_write_block(fs, p.node[0], p.leaf);
    }
    path_free(&p);
    return rc;
}

static void inode_init(btrfs_t *fs, uint8_t *item, uint32_t mode)
{
    memset(item, 0, 160);
    wr64(item, fs->cur_gen);            /* generation */
    wr64(item + 8, fs->cur_gen);        /* transid */
    wr32(item + 40, 1);                 /* nlink */
    wr32(item + 52, mode);
}

/* highest inode number in use in the default subvolume, plus one */
static int next_inode_number(btrfs_t *fs, uint64_t *out)
{
    bt_key_t key = { ~0ULL, 0xFF, ~0ULL };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    p.slot = p.nritems;
    int rc = bt_prev_item(fs, &p);
    uint64_t last = BTRFS_FIRST_FREE_OBJECTID;
    if (rc == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid > last) last = k.objectid;
    }
    path_free(&p);
    if (rc < 0) return -1;
    *out = last + 1;
    return 0;
}

/* next free DIR_INDEX number of a directory */
static int next_dir_index(btrfs_t *fs, uint64_t dir, uint64_t *out)
{
    bt_key_t key = { dir, BTRFS_DIR_INDEX_KEY, ~0ULL };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return -1; }
    int rc = bt_prev_item(fs, &p);
    uint64_t idx = 2;
    if (rc == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid == dir && k.type == BTRFS_DIR_INDEX_KEY && k.offset + 1 > idx) idx = k.offset + 1;
    }
    path_free(&p);
    if (rc < 0) return -1;
    *out = idx;
    return 0;
}

static uint32_t dir_entry_build(btrfs_t *fs, uint8_t *out, uint64_t child, uint8_t type, const char *name, int nlen)
{
    bt_key_t loc = { child, BTRFS_INODE_ITEM_KEY, 0 };
    key_write(out, &loc);
    wr64(out + 17, fs->cur_gen);
    wr16(out + 25, 0);
    wr16(out + 27, (uint16_t)nlen);
    out[29] = type;
    memcpy(out + 30, name, (size_t)nlen);
    return 30u + (uint32_t)nlen;
}

/* Adds `data` to the item at `key`, creating it or appending to what is there. */
static int item_append(btrfs_t *fs, const bt_key_t *key, const uint8_t *data, uint32_t size)
{
    bt_path_t p;
    int rc = bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, key, &p);
    if (rc < 0) return -1;
    if (rc > 0) return bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, key, data, size);
    uint32_t sz;
    const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
    if (!d) { path_free(&p); return -1; }
    uint8_t *nb = (uint8_t *)malloc(sz + size);
    if (!nb) { path_free(&p); return -1; }
    memcpy(nb, d, sz);
    memcpy(nb + sz, data, size);
    rc = bt_replace_slot(fs, BTRFS_FS_TREE_OBJECTID, &p, nb, sz + size);
    path_free(&p);
    free(nb);
    return rc;
}

static int dir_size_adjust(btrfs_t *fs, uint64_t dir, int64_t delta)
{
    uint8_t it[160];
    if (inode_load(fs, dir, it) != 0) return -1;
    int64_t sz = (int64_t)rd64(it + 16) + delta;
    if (sz < 0) sz = 0;
    wr64(it + 16, (uint64_t)sz);
    return inode_store(fs, dir, it);
}

static int dir_add_entry(btrfs_t *fs, uint64_t dir, const char *name, uint64_t child, uint8_t type, uint64_t index)
{
    int nlen = (int)strlen(name);
    uint8_t ent[30 + BTRFS_MAX_FILENAME];
    uint32_t esz = dir_entry_build(fs, ent, child, type, name, nlen);
    bt_key_t hk = { dir, BTRFS_DIR_ITEM_KEY, name_hash(name, nlen) };
    if (item_append(fs, &hk, ent, esz) != 0) return -1;
    bt_key_t ik = { dir, BTRFS_DIR_INDEX_KEY, index };
    if (bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, &ik, ent, esz) != 0) return -1;
    return dir_size_adjust(fs, dir, 2 * nlen);
}

static int inode_ref_add(btrfs_t *fs, uint64_t child, uint64_t parent, const char *name, uint64_t index)
{
    int nlen = (int)strlen(name);
    uint8_t ref[10 + BTRFS_MAX_FILENAME];
    wr64(ref, index);
    wr16(ref + 8, (uint16_t)nlen);
    memcpy(ref + 10, name, (size_t)nlen);
    bt_key_t k = { child, BTRFS_INODE_REF_KEY, parent };
    return item_append(fs, &k, ref, 10u + (uint32_t)nlen);
}

/* Removes the entry called `name` from the item at `key` (a DIR_ITEM or
 * INODE_REF); deletes the item when it was the only entry. *index receives the
 * entry's index (INODE_REF) when asked. */
static int item_remove_entry(btrfs_t *fs, const bt_key_t *key, const char *name, int is_ref, uint64_t *index)
{
    int nlen = (int)strlen(name);
    bt_path_t p;
    int rc = bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, key, &p);
    if (rc != 0) return -1;
    uint32_t sz;
    const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
    if (!d) { path_free(&p); return -1; }
    uint32_t off = 0, found_off = 0, found_len = 0;
    int found = 0;
    while (off < sz) {
        uint32_t len, en;
        const uint8_t *e = d + off;
        if (is_ref) {
            if (off + 10 > sz) break;
            en = rd16(e + 8);
            len = 10 + en;
            if (off + len > sz) break;
            if ((int)en == nlen && memcmp(e + 10, name, (size_t)nlen) == 0) {
                found = 1; found_off = off; found_len = len;
                if (index) *index = rd64(e);
                break;
            }
        } else {
            bt_dirent_t de;
            if (dir_item_parse(e, sz - off, &de, &len) != 0) break;
            if (de.name_len == nlen && memcmp(de.name, name, (size_t)nlen) == 0) { found = 1; found_off = off; found_len = len; break; }
        }
        off += len;
    }
    if (!found) { path_free(&p); return -1; }
    if (found_len == sz) {
        rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &p);
    } else {
        uint8_t *nb = (uint8_t *)malloc(sz - found_len);
        if (!nb) { path_free(&p); return -1; }
        memcpy(nb, d, found_off);
        memcpy(nb + found_off, d + found_off + found_len, sz - found_off - found_len);
        rc = bt_replace_slot(fs, BTRFS_FS_TREE_OBJECTID, &p, nb, sz - found_len);
        free(nb);
    }
    path_free(&p);
    return rc;
}

/* creates an inode (file or directory) called `name` in `parent` */
static int create_node(btrfs_t *fs, uint64_t parent, const char *name, uint32_t mode, uint8_t ftype, uint64_t *out_ino)
{
    uint64_t ino, index;
    if (next_inode_number(fs, &ino) != 0 || next_dir_index(fs, parent, &index) != 0) return -1;
    uint8_t it[160];
    inode_init(fs, it, mode);
    bt_key_t k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    if (bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, &k, it, 160) != 0) return -1;
    if (inode_ref_add(fs, ino, parent, name, index) != 0) return -1;
    if (dir_add_entry(fs, parent, name, ino, ftype, index) != 0) return -1;
    *out_ino = ino;
    return 0;
}

/* ---------- freeing and writing file data ---------- */

#define BT_PUNCH_BATCH 64

/* Removes the mapping of file bytes [a, b) (sector aligned; b may be ~0).
 * Disk extents lose a reference and are freed with the last one. *removed
 * receives the bytes taken away from the inode's nbytes. */
static int bt_punch(btrfs_t *fs, uint64_t ino, uint64_t a, uint64_t b, uint64_t *removed)
{
    bt_fext_t *list = (bt_fext_t *)malloc(sizeof(bt_fext_t) * BT_PUNCH_BATCH);
    if (!list) return -1;
    int rc = 0;
    for (int round = 0; round < 1000000 && rc == 0; round++) {
        int n = 0;
        bt_key_t key = { ino, BTRFS_EXTENT_DATA_KEY, a };
        bt_path_t p;
        if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); rc = -1; break; }
        /* the extent that starts before a may reach into the range */
        bt_path_t q = p;
        q.leaf = (uint8_t *)malloc(fs->nodesize);
        if (q.leaf) {
            memcpy(q.leaf, p.leaf, fs->nodesize);
            if (bt_prev_item(fs, &q) == 0) {
                bt_key_t k;
                leaf_key(&q, q.slot, &k);
                uint32_t sz;
                const uint8_t *d = leaf_data(fs, &q, q.slot, &sz);
                bt_fext_t e;
                if (d && k.objectid == ino && k.type == BTRFS_EXTENT_DATA_KEY && k.offset < a &&
                    fext_parse(k.offset, d, sz, &e) == 0 && e.off + e.num > a)
                    list[n++] = e;
            }
            path_free(&q);
        }
        while (n < BT_PUNCH_BATCH && bt_valid(fs, &p) == 0) {
            bt_key_t k;
            leaf_key(&p, p.slot, &k);
            if (k.objectid != ino || k.type != BTRFS_EXTENT_DATA_KEY || k.offset >= b) break;
            uint32_t sz;
            const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
            bt_fext_t e;
            if (!d || fext_parse(k.offset, d, sz, &e) != 0) { rc = -1; break; }
            list[n++] = e;
            p.slot++;
        }
        path_free(&p);
        if (rc != 0 || n == 0) break;

        for (int i = 0; i < n && rc == 0; i++) {
            bt_fext_t *e = &list[i];
            uint64_t s = e->off, end = s + e->num;
            bt_key_t ek = { ino, BTRFS_EXTENT_DATA_KEY, s };
            bt_path_t ep;
            if (bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, &ek, &ep) != 0) { rc = -1; break; }
            uint32_t sz;
            const uint8_t *d = leaf_data(fs, &ep, ep.slot, &sz);
            if (!d) { path_free(&ep); rc = -1; break; }
            int whole = (a <= s && end <= b);
            if (e->type == 0) {                                  /* inline: always rewritten as a whole */
                rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &ep);
                path_free(&ep);
                if (rc == 0 && removed) *removed += e->num;
                continue;
            }
            if ((e->comp || e->enc) && !whole) { path_free(&ep); rc = -1; break; }
            uint64_t ref_off = s - e->eoff;
            int is_hole = (e->disk == 0);
            if (whole) {
                rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &ep);
                path_free(&ep);
                if (rc == 0 && !is_hole) {
                    rc = bt_data_ref_adjust(fs, e->disk, BTRFS_FS_TREE_OBJECTID, ino, ref_off, -1);
                    if (removed) *removed += e->num;
                }
            } else if (s < a && end <= b) {                      /* keep the head */
                wr64((uint8_t *)d + 45, a - s);
                rc = bt_write_block(fs, ep.node[0], ep.leaf);
                path_free(&ep);
                if (rc == 0 && !is_hole && removed) *removed += end - a;
            } else if (a <= s) {                                 /* keep the tail: new key */
                uint8_t nd[53];
                memcpy(nd, d, sizeof(nd) < sz ? sizeof(nd) : sz);
                uint64_t cut = b - s;
                wr64(nd + 37, e->eoff + cut);
                wr64(nd + 45, end - b);
                rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &ep);
                path_free(&ep);
                if (rc == 0) {
                    bt_key_t nk = { ino, BTRFS_EXTENT_DATA_KEY, b };
                    rc = bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, &nk, nd, 53);
                }
                if (rc == 0 && !is_hole && removed) *removed += cut;
            } else {                                             /* a hole in the middle: split in two */
                uint8_t nd[53];
                memcpy(nd, d, sizeof(nd) < sz ? sizeof(nd) : sz);
                wr64((uint8_t *)d + 45, a - s);
                rc = bt_write_block(fs, ep.node[0], ep.leaf);
                path_free(&ep);
                wr64(nd + 37, e->eoff + (b - s));
                wr64(nd + 45, end - b);
                if (rc == 0) {
                    bt_key_t nk = { ino, BTRFS_EXTENT_DATA_KEY, b };
                    rc = bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, &nk, nd, 53);
                }
                if (rc == 0 && !is_hole) {
                    rc = bt_data_ref_adjust(fs, e->disk, BTRFS_FS_TREE_OBJECTID, ino, ref_off, +1);
                    if (removed) *removed += b - a;
                }
            }
        }
    }
    free(list);
    return rc;
}

#define BT_MAX_EXTENT (8u * 1024u * 1024u)

/* Writes buf[0..len) at byte `pos` of a file: the touched sectors are
 * rewritten as new extent(s); what they replaced is released. */
static int bt_write_range(btrfs_t *fs, uint64_t ino, uint64_t pos, const uint8_t *buf, uint32_t len)
{
    if (len == 0) return 0;
    uint8_t it[160];
    if (inode_load(fs, ino, it) != 0) return -1;
    uint64_t old_size = rd64(it + 16), nbytes = rd64(it + 24);
    uint32_t ss = fs->sectorsize;

    /* the stale part of the last sector must read as zeros once the file grows past it */
    if (pos > old_size && (old_size % ss) && pos >= align_up64(old_size, ss)) {
        uint32_t n = (uint32_t)(align_up64(old_size, ss) - old_size);
        uint8_t *z = (uint8_t *)bt_zalloc(n);
        if (!z) return -1;
        int rc = bt_write_range(fs, ino, old_size, z, n);
        free(z);
        if (rc != 0) return -1;
        if (inode_load(fs, ino, it) != 0) return -1;
        old_size = rd64(it + 16);
        nbytes = rd64(it + 24);
    }

    uint64_t end = pos + len;
    uint64_t a = pos & ~(uint64_t)(ss - 1), b = align_up64(end, ss);

    /* an inline extent cannot coexist with others: rewrite the whole (small) file */
    int has_inline = 0;
    if (old_size > 0) {
        bt_key_t k0 = { ino, BTRFS_EXTENT_DATA_KEY, 0 };
        bt_path_t p;
        if (bt_search(fs, fs->fs_tree_logical, &k0, &p) != 0) { path_free(&p); return -1; }
        if (bt_valid(fs, &p) == 0) {
            bt_key_t k;
            leaf_key(&p, p.slot, &k);
            uint32_t sz;
            const uint8_t *d = leaf_data(fs, &p, p.slot, &sz);
            if (d && k.objectid == ino && k.type == BTRFS_EXTENT_DATA_KEY && k.offset == 0 && sz >= 21 && d[20] == 0) has_inline = 1;
        }
        path_free(&p);
    }
    if (has_inline) {
        a = 0;
        if (b < align_up64(old_size, ss)) b = align_up64(old_size, ss);
    }

    uint64_t total = b - a;
    if (total > 0xFFFFFFFFULL || !bt_space_ok(fs, total + ss)) return -1;
    uint8_t *nb = (uint8_t *)bt_zalloc((size_t)total);
    if (!nb) return -1;
    int rc = 0;
    /* keep what the partial first and last sectors already hold */
    if (a < pos && a < old_size) {
        uint64_t to = pos < old_size ? pos : old_size;
        rc = read_file(fs, ino, old_size, a, nb, (uint32_t)(to - a));
    }
    if (rc == 0 && end < b && end < old_size) {
        uint64_t to = b < old_size ? b : old_size;
        rc = read_file(fs, ino, old_size, end, nb + (end - a), (uint32_t)(to - end));
    }
    if (rc != 0) { free(nb); return -1; }
    memcpy(nb + (pos - a), buf, len);

    uint64_t removed = 0;
    rc = bt_punch(fs, ino, a, b, &removed);
    for (uint64_t pa = a; rc == 0 && pa < b;) {
        uint64_t pl = b - pa;
        if (pl > BT_MAX_EXTENT) pl = BT_MAX_EXTENT;
        uint64_t bytenr;
        if (bt_alloc_space(fs, BTRFS_BG_DATA, pl, ss, &bytenr) != 0 &&
            (bt_chunk_alloc(fs, BTRFS_BG_DATA) != 0 || bt_alloc_space(fs, BTRFS_BG_DATA, pl, ss, &bytenr) != 0)) { rc = -1; break; }
        const uint8_t *src = nb + (pa - a);
        if (bt_write_logical(fs, bytenr, (uint32_t)pl, src) != 0) { rc = -1; break; }
        uint8_t fe[53];
        memset(fe, 0, sizeof(fe));
        wr64(fe, fs->cur_gen);
        wr64(fe + 8, pl);
        fe[20] = 1;
        wr64(fe + 21, bytenr);
        wr64(fe + 29, pl);
        wr64(fe + 37, 0);
        wr64(fe + 45, pl);
        bt_key_t fk = { ino, BTRFS_EXTENT_DATA_KEY, pa };
        if (bt_insert_item(fs, BTRFS_FS_TREE_OBJECTID, &fk, fe, sizeof(fe)) != 0) { rc = -1; break; }
        if (bt_pend_push(fs, BT_PEND_DATA_INSERT, bytenr, pl, ino, pa) != 0) { rc = -1; break; }
        if (bt_csum_insert(fs, bytenr, src, (uint32_t)pl) != 0) { rc = -1; break; }
        pa += pl;
    }
    free(nb);

    /* inode: size and nbytes follow the extents */
    if (inode_load(fs, ino, it) != 0) return -1;
    nbytes = rd64(it + 24);
    nbytes = nbytes >= removed ? nbytes - removed : 0;
    if (rc == 0) nbytes += total;
    wr64(it + 24, nbytes);
    uint64_t ns = old_size > end ? old_size : end;
    if (rc == 0) wr64(it + 16, ns);
    wr64(it + 8, fs->cur_gen);
    if (inode_store(fs, ino, it) != 0) return -1;
    return rc;
}

/* drops every extent of a file (open with O_TRUNC, unlink) */
static int bt_file_free_all(btrfs_t *fs, uint64_t ino)
{
    uint64_t removed = 0;
    int rc = bt_punch(fs, ino, 0, ~0ULL, &removed);
    uint8_t it[160];
    if (inode_load(fs, ino, it) != 0) return -1;
    wr64(it + 16, 0);
    wr64(it + 24, 0);
    if (inode_store(fs, ino, it) != 0) return -1;
    return rc;
}

/* deletes every item of an inode that is gone (links dropped to zero) */
static int bt_inode_delete(btrfs_t *fs, uint64_t ino)
{
    if (bt_file_free_all(fs, ino) != 0) return -1;
    for (int guard = 0; guard < 100000; guard++) {
        bt_key_t k0 = { ino, 0, 0 };
        bt_path_t p;
        if (bt_search(fs, fs->fs_tree_logical, &k0, &p) != 0) { path_free(&p); return -1; }
        int more = 0;
        if (bt_valid(fs, &p) == 0) {
            bt_key_t k;
            leaf_key(&p, p.slot, &k);
            more = (k.objectid == ino);
        }
        int rc = 0;
        if (more) rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &p);
        path_free(&p);
        if (rc != 0) return -1;
        if (!more) return 0;
    }
    return -1;
}

/* is the directory empty? (no DIR_INDEX items) */
static int dir_is_empty(btrfs_t *fs, uint64_t dir)
{
    bt_key_t key = { dir, BTRFS_DIR_INDEX_KEY, 0 };
    bt_path_t p;
    if (bt_search(fs, fs->fs_tree_logical, &key, &p) != 0) { path_free(&p); return 0; }
    int empty = 1;
    if (bt_valid(fs, &p) == 0) {
        bt_key_t k;
        leaf_key(&p, p.slot, &k);
        if (k.objectid == dir && k.type == BTRFS_DIR_INDEX_KEY) empty = 0;
    }
    path_free(&p);
    return empty;
}

/* Takes the entry `name` out of `parent`: DIR_ITEM, DIR_INDEX, INODE_REF.
 * The inode itself stays; *child / *ftype describe it. */
static int unlink_entry(btrfs_t *fs, uint64_t parent, const char *name, uint64_t *child, int *ftype)
{
    bt_dirent_t e;
    if (dir_lookup(fs, parent, name, &e) != 0 || e.location.type != BTRFS_INODE_ITEM_KEY) return -1;
    int nlen = (int)strlen(name);
    uint64_t index = 0;
    bt_key_t rk = { e.location.objectid, BTRFS_INODE_REF_KEY, parent };
    if (item_remove_entry(fs, &rk, name, 1, &index) != 0) return -1;
    bt_key_t hk = { parent, BTRFS_DIR_ITEM_KEY, name_hash(name, nlen) };
    if (item_remove_entry(fs, &hk, name, 0, 0) != 0) return -1;
    bt_key_t ik = { parent, BTRFS_DIR_INDEX_KEY, index };
    bt_path_t p;
    if (bt_find_exact(fs, BTRFS_FS_TREE_OBJECTID, &ik, &p) != 0) return -1;
    int rc = bt_delete_slot(fs, BTRFS_FS_TREE_OBJECTID, &p);
    path_free(&p);
    if (rc != 0) return -1;
    if (dir_size_adjust(fs, parent, -2 * nlen) != 0) return -1;
    *child = e.location.objectid;
    *ftype = e.type;
    return 0;
}

/* removes a name and, with the last link, the inode */
static int remove_node(btrfs_t *fs, uint64_t parent, const char *name)
{
    uint64_t child;
    int ftype;
    bt_dirent_t e;
    if (dir_lookup(fs, parent, name, &e) != 0 || e.location.type != BTRFS_INODE_ITEM_KEY) return -1;
    if (e.type == BTRFS_FT_DIR && !dir_is_empty(fs, e.location.objectid)) return -1;
    if (unlink_entry(fs, parent, name, &child, &ftype) != 0) return -1;
    uint8_t it[160];
    if (inode_load(fs, child, it) != 0) return -1;
    uint32_t nlink = rd32(it + 40);
    if (nlink > 1 && ftype != BTRFS_FT_DIR) {
        wr32(it + 40, nlink - 1);
        return inode_store(fs, child, it);
    }
    return bt_inode_delete(fs, child);
}

/* ---------- VFS ---------- */

static int bt_fd_flush(btrfs_t *fs, btrfs_fd_t *f)
{
    if (!f->wlen) return 0;
    if (bt_txn_begin(fs) != 0) return -1;
    int rc = bt_write_range(fs, f->ino, f->wstart, f->wbuf, f->wlen);
    int rc2 = bt_txn_commit(fs);
    f->wlen = 0;
    return (rc != 0 || rc2 != 0) ? -1 : 0;
}

static void bt_flush_all(btrfs_t *fs)
{
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (fs->fds[i].used && fs->fds[i].wlen) bt_fd_flush(fs, &fs->fds[i]);
}

static int bt_split_path(const char *path, char *parent, size_t psz, char *name, size_t nsz)
{
    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    int last = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { last = i; break; }
    int ns = last + 1;
    if (len - ns <= 0 || (size_t)(len - ns) >= nsz || (size_t)(last < 0 ? 0 : last) >= psz) return -1;
    memcpy(name, path + ns, (size_t)(len - ns));
    name[len - ns] = 0;
    memcpy(parent, path, (size_t)(last < 0 ? 0 : last));
    parent[last < 0 ? 0 : last] = 0;
    return 0;
}

/* inode of the directory holding `path`, and the final name */
static int bt_resolve_parent(btrfs_t *fs, const char *path, uint64_t *parent, char *name, size_t nsz)
{
    char pp[512];
    if (bt_split_path(path, pp, sizeof(pp), name, nsz) != 0) return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;
    int is_dir;
    if (walk(fs, pp, parent, &is_dir) != 0 || !is_dir) return -1;
    return 0;
}

#define BT_WRITE_FLAGS (VFS_WRONLY | VFS_RDWR | VFS_CREAT | VFS_TRUNC | VFS_APPEND)

static int btrfs_vfs_open(void *ctx, const char *path, int flags)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    int wr = (flags & BT_WRITE_FLAGS) != 0;
    if (wr && !fs->rw) return -1;
    bt_flush_all(fs);
    uint64_t ino;
    int is_dir;
    if (walk(fs, path, &ino, &is_dir) != 0) {
        if (!(flags & VFS_CREAT)) return -1;
        uint64_t parent;
        char name[BTRFS_MAX_FILENAME + 1];
        if (bt_resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
        if (bt_txn_begin(fs) != 0) return -1;
        int rc = create_node(fs, parent, name, 0100644, BTRFS_FT_REG_FILE, &ino);
        int rc2 = bt_txn_commit(fs);
        if (rc != 0 || rc2 != 0) return -1;
        is_dir = 0;
    } else {
        if (is_dir && wr) return -1;
        if ((flags & VFS_TRUNC) && !is_dir) {
            if (bt_txn_begin(fs) != 0) return -1;
            int rc = bt_file_free_all(fs, ino);
            int rc2 = bt_txn_commit(fs);
            if (rc != 0 || rc2 != 0) return -1;
        }
    }
    uint64_t size = 0;
    uint32_t mode = 0;
    if (get_inode(fs, ino, &size, &mode) != 0) return -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? fs->fds[i].size : 0;
            fs->fds[i].is_dir = is_dir;
            fs->fds[i].wlen = 0;
            fs->fds[i].wstart = 0;
            return i;
        }
    }
    return -1;
}

static int btrfs_vfs_close(void *ctx, int fd)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    int rc = bt_fd_flush(fs, &fs->fds[fd]);
    free(fs->fds[fd].wbuf);
    fs->fds[fd].wbuf = 0;
    fs->fds[fd].used = 0;
    return rc;
}

static int btrfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    bt_flush_all(fs);
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
    btrfs_t *fs = (btrfs_t *)ctx;
    if (!fs->rw || fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    btrfs_fd_t *f = &fs->fds[fd];
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < size) {
        uint32_t n = size - done;
        if (f->wlen && f->pos == f->wstart + f->wlen) {
            uint32_t room = BTRFS_WBUF_SIZE - f->wlen;
            if (room == 0) { if (bt_fd_flush(fs, f) != 0) break; continue; }
            if (n > room) n = room;
            memcpy(f->wbuf + f->wlen, src + done, n);
            f->wlen += n;
        } else {
            if (f->wlen && bt_fd_flush(fs, f) != 0) break;
            if (n >= BTRFS_WBUF_SIZE) {
                if (n > (1u << 20)) n = 1u << 20;
                if (bt_txn_begin(fs) != 0) break;
                int rc = bt_write_range(fs, f->ino, f->pos, src + done, n);
                int rc2 = bt_txn_commit(fs);
                if (rc != 0 || rc2 != 0) break;
            } else {
                if (!f->wbuf) f->wbuf = (uint8_t *)malloc(BTRFS_WBUF_SIZE);
                if (!f->wbuf) break;
                f->wstart = f->pos;
                memcpy(f->wbuf, src + done, n);
                f->wlen = n;
            }
        }
        f->pos += n;
        done += n;
        if (f->pos > f->size) f->size = f->pos;
    }
    return done ? (int)done : (size ? -1 : 0);
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
    bt_flush_all(fs);
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
    (void)mode;
    btrfs_t *fs = (btrfs_t *)ctx;
    if (!fs->rw) return -1;
    bt_flush_all(fs);
    uint64_t parent, ino;
    char name[BTRFS_MAX_FILENAME + 1];
    if (bt_resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
    bt_dirent_t e;
    if (dir_lookup(fs, parent, name, &e) == 0) return -1;
    if (bt_txn_begin(fs) != 0) return -1;
    int rc = create_node(fs, parent, name, 040755, BTRFS_FT_DIR, &ino);
    int rc2 = bt_txn_commit(fs);
    return (rc != 0 || rc2 != 0) ? -1 : 0;
}

static int btrfs_vfs_unlink(void *ctx, const char *path)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (!fs->rw) return -1;
    bt_flush_all(fs);
    uint64_t parent;
    char name[BTRFS_MAX_FILENAME + 1];
    if (bt_resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
    if (bt_txn_begin(fs) != 0) return -1;
    int rc = remove_node(fs, parent, name);
    int rc2 = bt_txn_commit(fs);
    return (rc != 0 || rc2 != 0) ? -1 : 0;
}

static int btrfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    bt_flush_all(fs);
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

/* is `anc` the directory `dir` itself or one of its ancestors? */
static int dir_is_under(btrfs_t *fs, uint64_t anc, uint64_t dir)
{
    uint64_t cur = dir;
    for (int guard = 0; guard < 4096; guard++) {
        if (cur == anc) return 1;
        if (cur == BTRFS_FIRST_FREE_OBJECTID) return 0;
        bt_key_t k0 = { cur, BTRFS_INODE_REF_KEY, 0 };
        bt_path_t p;
        if (bt_search(fs, fs->fs_tree_logical, &k0, &p) != 0) { path_free(&p); return 1; }
        uint64_t parent = 0;
        if (bt_valid(fs, &p) == 0) {
            bt_key_t k;
            leaf_key(&p, p.slot, &k);
            if (k.objectid == cur && k.type == BTRFS_INODE_REF_KEY) parent = k.offset;
        }
        path_free(&p);
        if (parent == 0) return 1;
        cur = parent;
    }
    return 1;
}

static int btrfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (!fs->rw) return -1;
    bt_flush_all(fs);
    uint64_t sp, dp;
    char sname[BTRFS_MAX_FILENAME + 1], dname[BTRFS_MAX_FILENAME + 1];
    if (bt_resolve_parent(fs, old, &sp, sname, sizeof(sname)) != 0) return -1;
    if (bt_resolve_parent(fs, new_path, &dp, dname, sizeof(dname)) != 0) return -1;
    bt_dirent_t src, dst;
    if (dir_lookup(fs, sp, sname, &src) != 0 || src.location.type != BTRFS_INODE_ITEM_KEY) return -1;
    if (sp == dp && strcmp(sname, dname) == 0) return 0;
    if (src.type == BTRFS_FT_DIR && dir_is_under(fs, src.location.objectid, dp)) return -1;

    if (bt_txn_begin(fs) != 0) return -1;
    int rc = 0;
    if (dir_lookup(fs, dp, dname, &dst) == 0) {
        if (dst.location.objectid == src.location.objectid || dst.type != src.type) rc = -1;
        else rc = remove_node(fs, dp, dname);
    }
    uint64_t child = 0, index = 0;
    int ftype = 0;
    if (rc == 0) rc = unlink_entry(fs, sp, sname, &child, &ftype);
    if (rc == 0) rc = next_dir_index(fs, dp, &index);
    if (rc == 0) rc = inode_ref_add(fs, child, dp, dname, index);
    if (rc == 0) rc = dir_add_entry(fs, dp, dname, child, (uint8_t)ftype, index);
    int rc2 = bt_txn_commit(fs);
    return (rc != 0 || rc2 != 0) ? -1 : 0;
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
    klog_write(fs->rw ? "btrfs: mounted read-write\n" : "btrfs: mounted read-only\n");
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
    memcpy(fs->sb, sb, 4096);

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
    fs->dev_size = bd->total_sectors * (uint64_t)(bd->sector_size ? bd->sector_size : 512);

    if (fs->nodesize < 4096 || fs->nodesize > 65536 || (fs->nodesize & (fs->nodesize - 1)) ||
        fs->sectorsize < 512 || fs->sectorsize > fs->nodesize || (fs->sectorsize & (fs->sectorsize - 1)) || num_devices != 1 ||
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

    /* writable only when every on-disk structure we would touch is one we keep up to date */
    int ok = fs->rw == 0;
    uint64_t compat_ro = rd64(fs->sb + 0xB4);
    static const uint64_t incompat_ok = BTRFS_INCOMPAT_MIXED_BACKREF | BTRFS_INCOMPAT_DEFAULT_SUBVOL | BTRFS_INCOMPAT_COMPRESS_LZO |
        BTRFS_INCOMPAT_COMPRESS_ZSTD | BTRFS_INCOMPAT_BIG_METADATA | BTRFS_INCOMPAT_EXTENDED_IREF | BTRFS_INCOMPAT_SKINNY_METADATA |
        BTRFS_INCOMPAT_NO_HOLES | BTRFS_INCOMPAT_METADATA_UUID;
    if (rd16(fs->sb + 0xC4) != 0) ok = 0;                                  /* checksum type must be crc32c */
    if (rd64(fs->sb + 0x60) != 0) ok = 0;                                  /* a log tree would need replaying */
    if (compat_ro & ~3ULL) ok = 0;
    if (incompat & ~incompat_ok) ok = 0;
    if (!(incompat & BTRFS_INCOMPAT_SKINNY_METADATA) || !(incompat & BTRFS_INCOMPAT_NO_HOLES)) ok = 0;
    if (rd64(fs->sb + 0x38) & ~1ULL) ok = 0;
    for (int i = 0; i < fs->num_chunks; i++) if (fs->chunks[i].nstripes > 2) ok = 0;
    if (ok && (find_subvol_root(fs, BTRFS_EXTENT_TREE_OBJECTID, &fs->extent_root) != 0 ||
               find_subvol_root(fs, BTRFS_DEV_TREE_OBJECTID, &fs->dev_root) != 0 ||
               find_subvol_root(fs, BTRFS_CSUM_TREE_OBJECTID, &fs->csum_root) != 0)) ok = 0;
    uint64_t quota;
    if (ok && find_subvol_root(fs, BTRFS_QUOTA_TREE_OBJECTID, &quota) == 0) ok = 0;
    if (ok) {
        uint8_t hdr[BTRFS_NODE_HDR_SZ];
        if (bt_read(fs, fs->root_logical, sizeof(hdr), hdr) != 0) ok = 0;
        else {
            memcpy(fs->hdr_fsid, hdr + 32, 16);
            memcpy(fs->hdr_chunk_uuid, hdr + 64, 16);
        }
    }
    fs->has_fst = (compat_ro & 1) != 0;
    fs->rw = ok;
    return 0;
}

int btrfs_umount(btrfs_t *fs)
{
    if (fs->rw) bt_flush_all(fs);
    for (int i = 0; i < VFS_MAX_FDS; i++) { free(fs->fds[i].wbuf); fs->fds[i].wbuf = 0; }
    free(fs->used); fs->used = 0; fs->used_n = fs->used_cap = 0;
    free(fs->bgs); fs->bgs = 0; fs->nbgs = 0;
    free(fs->pend); fs->pend = 0; fs->pend_n = fs->pend_cap = 0;
    fs->alloc_loaded = 0;
    return 0;
}

/* ---------- mkfs ---------- */

static uint32_t mk_rand(void)
{
    static uint32_t x;
    uint32_t tsc;
    __asm__ volatile("rdtsc" : "=a"(tsc) : : "edx");
    x ^= tsc + 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

typedef struct {
    uint8_t key[17];
    uint32_t size;
    const uint8_t *data;
} mk_item_t;

static void mk_key(uint8_t *k, uint64_t obj, uint8_t type, uint64_t off)
{
    bt_key_t kk = { obj, type, off };
    key_write(k, &kk);
}

static int mk_key_cmp(const mk_item_t *a, const mk_item_t *b)
{
    bt_key_t ka, kb;
    key_read(a->key, &ka);
    key_read(b->key, &kb);
    return key_cmp(&ka, &kb);
}

/* writes one leaf holding `items` (sorted here) at `bytenr` */
static int mk_leaf(btrfs_t *fs, uint64_t bytenr, uint64_t owner, mk_item_t *items, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {                     /* insertion sort */
        mk_item_t t = items[i];
        uint32_t j = i;
        while (j > 0 && mk_key_cmp(&items[j - 1], &t) > 0) { items[j] = items[j - 1]; j--; }
        items[j] = t;
    }
    uint8_t *buf = (uint8_t *)malloc(fs->nodesize);
    bt_item_t *bi = (bt_item_t *)malloc((n ? n : 1) * sizeof(bt_item_t));
    if (!buf || !bi) { free(buf); free(bi); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        memcpy(bi[i].key, items[i].key, 17);
        bi[i].size = items[i].size;
        bi[i].data = items[i].data;
    }
    bt_blk_init(fs, buf, bytenr, owner, 0, 1);
    int rc = leaf_build(fs, buf, bi, n);
    if (rc == 0) rc = bt_write_block(fs, bytenr, buf);
    free(buf);
    free(bi);
    return rc;
}

static void mk_inode(uint8_t *it, uint32_t mode, uint64_t size)
{
    memset(it, 0, 160);
    wr64(it, 1);                 /* generation */
    wr64(it + 16, size);
    wr32(it + 40, 1);            /* nlink */
    wr32(it + 52, mode);
}

static void mk_root_item(uint8_t *ri, uint64_t bytenr, uint64_t dirid, uint32_t nodesize)
{
    memset(ri, 0, 439);
    mk_inode(ri, 040755, 0);
    wr64(ri + 160, 1);           /* generation */
    wr64(ri + 168, dirid);
    wr64(ri + 176, bytenr);
    wr64(ri + 192, nodesize);    /* bytes_used */
    wr32(ri + 216, 1);           /* refs */
    wr64(ri + 239, 1);           /* generation_v2 */
}

int btrfs_format(blockdev_t *bd, const char *label)
{
    uint32_t ss = 4096, ns = 16384;
    uint64_t dev = bd->total_sectors * (uint64_t)(bd->sector_size ? bd->sector_size : 512);
    dev &= ~(uint64_t)(ss - 1);
    if (dev < (48ULL << 20)) return -1;

    uint64_t meta_len = dev >= (512ULL << 20) ? (32ULL << 20) : dev >= (128ULL << 20) ? (16ULL << 20) : (8ULL << 20);
    const uint64_t sys_len = 4ULL << 20, data_len = 8ULL << 20, MiB = 1ULL << 20;
    uint64_t sys_l = MiB, meta_l = sys_l + sys_len, data_l = meta_l + meta_len;
    uint64_t sys_p0 = MiB, sys_p1 = sys_p0 + sys_len, meta_p0 = sys_p1 + sys_len, meta_p1 = meta_p0 + meta_len, data_p = meta_p1 + meta_len;
    if (data_p + data_len > dev) return -1;

    btrfs_t *fs = (btrfs_t *)malloc(sizeof(btrfs_t));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    fs->nodesize = ns;
    fs->sectorsize = ss;
    chunk_add(fs, sys_l, sys_len, sys_p0, sys_p1, BTRFS_BG_SYSTEM | BTRFS_BG_DUP, 2);
    chunk_add(fs, meta_l, meta_len, meta_p0, meta_p1, BTRFS_BG_METADATA | BTRFS_BG_DUP, 2);
    chunk_add(fs, data_l, data_len, data_p, 0, BTRFS_BG_DATA, 1);

    uint8_t fsid[16], chunk_uuid[16], dev_uuid[16];
    for (int i = 0; i < 16; i += 4) { wr32(fsid + i, mk_rand()); wr32(chunk_uuid + i, mk_rand()); wr32(dev_uuid + i, mk_rand()); }
    memcpy(fs->hdr_fsid, fsid, 16);
    memcpy(fs->hdr_chunk_uuid, chunk_uuid, 16);

    /* tree block addresses */
    uint64_t chunk_blk = sys_l;
    uint64_t root_blk = meta_l, extent_blk = meta_l + ns, dev_blk = meta_l + 2 * ns, fs_blk = meta_l + 3 * ns,
             csum_blk = meta_l + 4 * ns, reloc_blk = meta_l + 5 * ns;

    int rc = -1;
    uint8_t dev_item[98], chunk_sys[48 + 64], chunk_meta[48 + 64], chunk_data[48 + 32];
    uint8_t ri[6][439], ino_dir[160], ino_ref_root[12], ino_ref_default[17], dir_default[37];
    uint8_t mi[7][33], bgi[3][24], de[5][48];
    mk_item_t it[40];
    uint32_t n;

    /* device item and chunk items */
    memset(dev_item, 0, sizeof(dev_item));
    wr64(dev_item, 1);
    wr64(dev_item + 8, dev);
    wr64(dev_item + 16, 2 * sys_len + 2 * meta_len + data_len);
    wr32(dev_item + 24, ss); wr32(dev_item + 28, ss); wr32(dev_item + 32, ss);
    memcpy(dev_item + 66, dev_uuid, 16);
    memcpy(dev_item + 82, fsid, 16);

    struct { uint8_t *c; uint64_t len, type, p0, p1; int ns; } ch[3] = {
        { chunk_sys, sys_len, BTRFS_BG_SYSTEM | BTRFS_BG_DUP, sys_p0, sys_p1, 2 },
        { chunk_meta, meta_len, BTRFS_BG_METADATA | BTRFS_BG_DUP, meta_p0, meta_p1, 2 },
        { chunk_data, data_len, BTRFS_BG_DATA, data_p, 0, 1 },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t *c = ch[i].c;
        memset(c, 0, 48 + 32 * (size_t)ch[i].ns);
        wr64(c, ch[i].len); wr64(c + 8, BTRFS_EXTENT_TREE_OBJECTID); wr64(c + 16, 65536); wr64(c + 24, ch[i].type);
        wr32(c + 32, 65536); wr32(c + 36, 65536); wr32(c + 40, ss);
        wr16(c + 44, (uint16_t)ch[i].ns); wr16(c + 46, 1);
        for (int s = 0; s < ch[i].ns; s++) {
            uint8_t *st = c + 48 + s * 32;
            wr64(st, 1); wr64(st + 8, s == 0 ? ch[i].p0 : ch[i].p1); memcpy(st + 16, dev_uuid, 16);
        }
    }

    /* chunk tree */
    uint8_t k_dev[17], k_c0[17], k_c1[17], k_c2[17];
    mk_key(k_dev, 1, 0xD8, 1);
    mk_key(k_c0, BTRFS_FIRST_CHUNK_OBJECTID, BTRFS_CHUNK_ITEM_KEY, sys_l);
    mk_key(k_c1, BTRFS_FIRST_CHUNK_OBJECTID, BTRFS_CHUNK_ITEM_KEY, meta_l);
    mk_key(k_c2, BTRFS_FIRST_CHUNK_OBJECTID, BTRFS_CHUNK_ITEM_KEY, data_l);
    n = 0;
    memcpy(it[n].key, k_dev, 17); it[n].size = 98; it[n++].data = dev_item;
    memcpy(it[n].key, k_c0, 17); it[n].size = 48 + 64; it[n++].data = chunk_sys;
    memcpy(it[n].key, k_c1, 17); it[n].size = 48 + 64; it[n++].data = chunk_meta;
    memcpy(it[n].key, k_c2, 17); it[n].size = 48 + 32; it[n++].data = chunk_data;
    if (mk_leaf(fs, chunk_blk, BTRFS_CHUNK_TREE_OBJECTID, it, n) != 0) goto out;

    /* root tree */
    mk_root_item(ri[0], extent_blk, 0, ns);
    mk_root_item(ri[1], dev_blk, 0, ns);
    mk_root_item(ri[2], fs_blk, 256, ns);
    mk_root_item(ri[3], csum_blk, 0, ns);
    mk_root_item(ri[4], reloc_blk, 256, ns);
    mk_inode(ino_dir, 040755, 0);
    wr64(ino_ref_root, 0); wr16(ino_ref_root + 8, 2); ino_ref_root[10] = '.'; ino_ref_root[11] = '.';
    wr64(ino_ref_default, 0); wr16(ino_ref_default + 8, 7); memcpy(ino_ref_default + 10, "default", 7);
    memset(dir_default, 0, sizeof(dir_default));
    mk_key(dir_default, BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, ~0ULL);
    wr16(dir_default + 27, 7); dir_default[29] = BTRFS_FT_DIR; memcpy(dir_default + 30, "default", 7);
    n = 0;
    mk_key(it[n].key, BTRFS_EXTENT_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0); it[n].size = 439; it[n++].data = ri[0];
    mk_key(it[n].key, BTRFS_DEV_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0); it[n].size = 439; it[n++].data = ri[1];
    mk_key(it[n].key, BTRFS_FS_TREE_OBJECTID, BTRFS_INODE_REF_KEY, 6); it[n].size = 17; it[n++].data = ino_ref_default;
    mk_key(it[n].key, BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0); it[n].size = 439; it[n++].data = ri[2];
    mk_key(it[n].key, 6, BTRFS_INODE_ITEM_KEY, 0); it[n].size = 160; it[n++].data = ino_dir;
    mk_key(it[n].key, 6, BTRFS_INODE_REF_KEY, 6); it[n].size = 12; it[n++].data = ino_ref_root;
    mk_key(it[n].key, 6, BTRFS_DIR_ITEM_KEY, name_hash("default", 7)); it[n].size = 37; it[n++].data = dir_default;
    mk_key(it[n].key, BTRFS_CSUM_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0); it[n].size = 439; it[n++].data = ri[3];
    mk_key(it[n].key, 0xFFFFFFFFFFFFFFF7ULL, BTRFS_ROOT_ITEM_KEY, 0); it[n].size = 439; it[n++].data = ri[4];
    if (mk_leaf(fs, root_blk, BTRFS_ROOT_TREE_OBJECTID, it, n) != 0) goto out;

    /* extent tree: one skinny item per tree block, one block-group item per chunk */
    static const uint64_t owners[7] = { BTRFS_CHUNK_TREE_OBJECTID, BTRFS_ROOT_TREE_OBJECTID, BTRFS_EXTENT_TREE_OBJECTID,
                                        BTRFS_DEV_TREE_OBJECTID, BTRFS_FS_TREE_OBJECTID, BTRFS_CSUM_TREE_OBJECTID, 0xFFFFFFFFFFFFFFF7ULL };
    uint64_t addrs[7] = { chunk_blk, root_blk, extent_blk, dev_blk, fs_blk, csum_blk, reloc_blk };
    n = 0;
    for (int i = 0; i < 7; i++) {
        memset(mi[i], 0, 33);
        wr64(mi[i], 1); wr64(mi[i] + 8, 1); wr64(mi[i] + 16, BT_EXTENT_FLAG_TREE_BLOCK);
        mi[i][24] = BTRFS_TREE_BLOCK_REF_KEY; wr64(mi[i] + 25, owners[i]);
        mk_key(it[n].key, addrs[i], BTRFS_METADATA_ITEM_KEY, 0); it[n].size = 33; it[n++].data = mi[i];
    }
    uint64_t bg_start[3] = { sys_l, meta_l, data_l }, bg_len[3] = { sys_len, meta_len, data_len };
    uint64_t bg_used[3] = { ns, 6 * (uint64_t)ns, 0 };
    for (int i = 0; i < 3; i++) {
        wr64(bgi[i], bg_used[i]); wr64(bgi[i] + 8, BTRFS_FIRST_CHUNK_OBJECTID); wr64(bgi[i] + 16, ch[i].type);
        mk_key(it[n].key, bg_start[i], BTRFS_BLOCK_GROUP_ITEM_KEY, bg_len[i]); it[n].size = 24; it[n++].data = bgi[i];
    }
    if (mk_leaf(fs, extent_blk, BTRFS_EXTENT_TREE_OBJECTID, it, n) != 0) goto out;

    /* device tree */
    uint64_t dev_p[5] = { sys_p0, sys_p1, meta_p0, meta_p1, data_p };
    uint64_t dev_c[5] = { sys_l, sys_l, meta_l, meta_l, data_l };
    uint64_t dev_n[5] = { sys_len, sys_len, meta_len, meta_len, data_len };
    n = 0;
    for (int i = 0; i < 5; i++) {
        memset(de[i], 0, 48);
        wr64(de[i], BTRFS_CHUNK_TREE_OBJECTID); wr64(de[i] + 8, BTRFS_FIRST_CHUNK_OBJECTID);
        wr64(de[i] + 16, dev_c[i]); wr64(de[i] + 24, dev_n[i]); memcpy(de[i] + 32, chunk_uuid, 16);
        mk_key(it[n].key, 1, BTRFS_DEV_EXTENT_KEY, dev_p[i]); it[n].size = 48; it[n++].data = de[i];
    }
    if (mk_leaf(fs, dev_blk, BTRFS_DEV_TREE_OBJECTID, it, n) != 0) goto out;

    /* default subvolume and the data relocation tree: just their root directory */
    {
        uint8_t ino_fs[160], ref_fs[12];
        mk_inode(ino_fs, 040755, 0);
        wr64(ref_fs, 0); wr16(ref_fs + 8, 2); ref_fs[10] = '.'; ref_fs[11] = '.';
        for (int t = 0; t < 2; t++) {
            n = 0;
            mk_key(it[n].key, 256, BTRFS_INODE_ITEM_KEY, 0); it[n].size = 160; it[n++].data = ino_fs;
            mk_key(it[n].key, 256, BTRFS_INODE_REF_KEY, 256); it[n].size = 12; it[n++].data = ref_fs;
            if (mk_leaf(fs, t == 0 ? fs_blk : reloc_blk, t == 0 ? BTRFS_FS_TREE_OBJECTID : 0xFFFFFFFFFFFFFFF7ULL, it, n) != 0) goto out;
        }
    }
    if (mk_leaf(fs, csum_blk, BTRFS_CSUM_TREE_OBJECTID, it, 0) != 0) goto out;

    /* superblock */
    {
        uint8_t *sb = (uint8_t *)malloc(4096);
        if (!sb) goto out;
        memset(sb, 0, 4096);
        memcpy(sb + 0x20, fsid, 16);
        wr64(sb + 0x30, BTRFS_SUPER_OFFSET);
        wr64(sb + 0x38, 1);                                   /* WRITTEN */
        wr64(sb + 0x40, BTRFS_MAGIC);
        wr64(sb + 0x48, 1);
        wr64(sb + 0x50, root_blk);
        wr64(sb + 0x58, chunk_blk);
        wr64(sb + 0x70, dev);
        wr64(sb + 0x78, 7 * (uint64_t)ns);
        wr64(sb + 0x80, 6);
        wr64(sb + 0x88, 1);
        wr32(sb + 0x90, ss);
        wr32(sb + 0x94, ns);
        wr32(sb + 0x98, ns);
        wr32(sb + 0x9C, ss);
        wr32(sb + 0xA0, 17 + 48 + 64);
        wr64(sb + 0xA4, 1);
        wr64(sb + 0xBC, BTRFS_INCOMPAT_MIXED_BACKREF | BTRFS_INCOMPAT_BIG_METADATA | BTRFS_INCOMPAT_EXTENDED_IREF |
                         BTRFS_INCOMPAT_SKINNY_METADATA | BTRFS_INCOMPAT_NO_HOLES);
        memcpy(sb + 0xC9, dev_item, 98);
        if (label) for (int i = 0; i < 255 && label[i]; i++) sb[0x12B + i] = (uint8_t)label[i];
        mk_key(sb + 0x32B, BTRFS_FIRST_CHUNK_OBJECTID, BTRFS_CHUNK_ITEM_KEY, sys_l);
        memcpy(sb + 0x32B + 17, chunk_sys, 48 + 64);
        static const uint64_t offs[3] = { BTRFS_SUPER_OFFSET, 64ULL << 20, 256ULL << 30 };
        int wrc = 0;
        for (int i = 0; i < 3 && wrc == 0; i++) {
            if (offs[i] + 4096 > dev) break;
            uint8_t copy[4096];
            memcpy(copy, sb, 4096);
            wr64(copy + 0x30, offs[i]);
            uint32_t c = ~crc32c_update(0xFFFFFFFFu, copy + 32, 4096 - 32);
            memset(copy, 0, 32);
            wr32(copy, c);
            wrc = blockdev_write_bytes(bd, offs[i], 4096, copy);
        }
        free(sb);
        if (wrc != 0) goto out;
    }
    rc = 0;
out:
    free(fs);
    return rc;
}
