/* XFS driver: reads and writes v5 (CRC) volumes made by mkfs.xfs, and reads v4.
 *
 * Writing works directly on the metadata, one operation at a time, and leaves
 * the log alone (it is checked to be clean when the volume is mounted):
 * free-space and inode B-trees that fit in their root block, extent-format
 * data forks, short-form and single-block directories. Volumes using
 * anything beyond that (real-time device, quotas, reverse mapping,
 * shared reflink extents, multi-level AG trees, a dirty log) are mounted
 * read-only, as are directories that have grown past one block. */

#include "xfs.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "crc32c.h"

#define XFS_SB_MAGIC     0x58465342u   /* "XFSB" */
#define XFS_DINODE_MAGIC 0x494E        /* "IN" */
#define XFS_AGF_MAGIC    0x58414746u
#define XFS_AGI_MAGIC    0x58414749u
#define XFS_AGFL_MAGIC   0x5841464Cu
#define XFS_ABTB_CRC_MAGIC 0x41423342u /* "AB3B" bnobt */
#define XFS_ABTC_CRC_MAGIC 0x41423343u /* "AB3C" cntbt */
#define XFS_IBT_CRC_MAGIC  0x49414233u /* "IAB3" */
#define XFS_FIBT_CRC_MAGIC 0x46494233u /* "FIB3" */

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

#define XFS_RO_COMPAT_FINOBT   0x01u
#define XFS_RO_COMPAT_RMAPBT   0x02u
#define XFS_RO_COMPAT_REFLINK  0x04u
#define XFS_RO_COMPAT_INOBTCNT 0x08u

#define S_IFMT_  0170000
#define S_IFDIR_ 0040000
#define S_IFREG_ 0100000

#define XFS_NULLAGINO 0xFFFFFFFFu
#define XFS_FT_REG_FILE 1
#define XFS_FT_DIR 2

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

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

/* ---------- addressing and checksums ---------- */

static uint64_t fsb_to_byte(xfs_t *fs, uint64_t fsb)
{
    uint64_t agno = fsb >> fs->agblklog;
    uint64_t agbno = fsb & (((uint64_t)1 << fs->agblklog) - 1);
    return (agno * fs->agblocks + agbno) * fs->blocksize;
}

static uint64_t agbno_to_byte(xfs_t *fs, uint32_t ag, uint32_t agbno)
{
    return ((uint64_t)ag * fs->agblocks + agbno) * fs->blocksize;
}

static uint64_t agbno_to_fsb(xfs_t *fs, uint32_t ag, uint32_t agbno)
{
    return ((uint64_t)ag << fs->agblklog) | agbno;
}

/* CRC32C over the block with the 4-byte checksum field read as zero; stored little-endian */
static void xfs_cksum_update(uint8_t *buf, size_t len, size_t off)
{
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    uint32_t c = crc32c_update(0xFFFFFFFFu, buf, off);
    c = crc32c_update(c, zero4, 4);
    c = crc32c_update(c, buf + off + 4, len - off - 4);
    c = ~c;
    buf[off] = (uint8_t)c;
    buf[off + 1] = (uint8_t)(c >> 8);
    buf[off + 2] = (uint8_t)(c >> 16);
    buf[off + 3] = (uint8_t)(c >> 24);
}

static int xfs_rd(xfs_t *fs, uint64_t off, uint32_t len, void *buf)
{
    return blockdev_read_bytes(fs->bd, off, len, buf);
}

static int xfs_wr(xfs_t *fs, uint64_t off, uint32_t len, const void *buf)
{
    return blockdev_write_bytes(fs->bd, off, len, buf);
}

/* ---------- inodes ---------- */

static int inode_location(xfs_t *fs, uint64_t ino, uint64_t *off)
{
    uint64_t agno = ino >> (fs->agblklog + fs->inopblog);
    uint64_t agbno = (ino >> fs->inopblog) & (((uint64_t)1 << fs->agblklog) - 1);
    uint64_t slot = ino & (((uint64_t)1 << fs->inopblog) - 1);
    if (agno >= fs->agcount || agbno >= fs->agblocks) return -1;
    *off = (agno * fs->agblocks + agbno) * fs->blocksize + slot * fs->inodesize;
    return 0;
}

static void inode_parse(xfs_t *fs, xfs_inode_t *ip)
{
    ip->mode = be16(ip->raw + 2);
    ip->version = ip->raw[4];
    ip->format = ip->raw[5];
    ip->size = be64(ip->raw + 56);
    ip->nextents = be32(ip->raw + 76);
    ip->forkoff = ip->raw[82];
    ip->core_len = ip->version >= 3 ? 176 : 100;
    ip->fork = ip->raw + ip->core_len;
    ip->fork_len = ip->forkoff ? (int)ip->forkoff * 8 : (int)fs->inodesize - ip->core_len;
}

static int read_inode(xfs_t *fs, uint64_t ino, xfs_inode_t *ip)
{
    uint64_t off;
    if (inode_location(fs, ino, &off) != 0) return -1;
    memset(ip, 0, sizeof(*ip));
    ip->raw = (uint8_t *)malloc(fs->inodesize);
    if (!ip->raw) return -1;
    if (xfs_rd(fs, off, fs->inodesize, ip->raw) != 0 || be16(ip->raw) != XFS_DINODE_MAGIC) {
        free(ip->raw);
        ip->raw = 0;
        return -1;
    }
    inode_parse(fs, ip);
    if (ip->fork_len < 0 || ip->core_len + ip->fork_len > (int)fs->inodesize) { free(ip->raw); ip->raw = 0; return -1; }
    return 0;
}

static void inode_free(xfs_inode_t *ip)
{
    free(ip->raw);
    ip->raw = 0;
}

/* writes the (modified) raw inode back, refreshing the v3 checksum */
static int write_inode(xfs_t *fs, uint64_t ino, xfs_inode_t *ip)
{
    uint64_t off;
    if (inode_location(fs, ino, &off) != 0) return -1;
    if (ip->version >= 3) xfs_cksum_update(ip->raw, fs->inodesize, 100);
    return xfs_wr(fs, off, fs->inodesize, ip->raw);
}

/* ---------- allocation group headers ---------- */

#define XFS_AGF_SECTOR 1
#define XFS_AGI_SECTOR 2
#define XFS_AGF_CRC_OFF 216
#define XFS_AGI_CRC_OFF 312
#define XFS_SB_CRC_OFF  224
#define XFS_BTBLOCK_CRC_OFF 52
#define XFS_BTBLOCK_HDR 56

static int ag_hdr_read(xfs_t *fs, uint32_t ag, int sector, uint8_t *buf)
{
    return xfs_rd(fs, agbno_to_byte(fs, ag, 0) + (uint64_t)sector * fs->sectsize, fs->sectsize, buf);
}

static int ag_hdr_write(xfs_t *fs, uint32_t ag, int sector, uint8_t *buf, size_t crc_off)
{
    xfs_cksum_update(buf, fs->sectsize, crc_off);
    return xfs_wr(fs, agbno_to_byte(fs, ag, 0) + (uint64_t)sector * fs->sectsize, fs->sectsize, buf);
}

/* the primary superblock's lazily maintained counters */
static int sb_adjust(xfs_t *fs, int64_t d_icount, int64_t d_ifree, int64_t d_fdblocks)
{
    uint8_t *sb = (uint8_t *)malloc(fs->sectsize);
    if (!sb) return -1;
    if (xfs_rd(fs, 0, fs->sectsize, sb) != 0 || be32(sb) != XFS_SB_MAGIC) { free(sb); return -1; }
    put64(sb + 128, (uint64_t)((int64_t)be64(sb + 128) + d_icount));
    put64(sb + 136, (uint64_t)((int64_t)be64(sb + 136) + d_ifree));
    put64(sb + 144, (uint64_t)((int64_t)be64(sb + 144) + d_fdblocks));
    xfs_cksum_update(sb, fs->sectsize, XFS_SB_CRC_OFF);
    int rc = xfs_wr(fs, 0, fs->sectsize, sb);
    free(sb);
    return rc;
}

/* ---------- free space (the by-block and by-size B-trees, one block each) ---------- */

typedef struct { uint32_t start, len; } xfs_frec_t;

static uint32_t fs_tree_maxrecs(xfs_t *fs) { return (fs->blocksize - XFS_BTBLOCK_HDR) / 8; }

static int ag_free_load(xfs_t *fs, uint32_t ag, xfs_frec_t *recs, uint32_t cap, uint32_t *n)
{
    uint8_t agf[512];
    if (fs->sectsize > sizeof(agf) || ag_hdr_read(fs, ag, XFS_AGF_SECTOR, agf) != 0 || be32(agf) != XFS_AGF_MAGIC) return -1;
    if (be32(agf + 28) != 1) return -1;
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    int rc = -1;
    if (xfs_rd(fs, agbno_to_byte(fs, ag, be32(agf + 16)), fs->blocksize, blk) != 0) goto out;
    if (be32(blk) != XFS_ABTB_CRC_MAGIC || be16(blk + 4) != 0) goto out;
    uint32_t nr = be16(blk + 6);
    if (nr > cap || nr > fs_tree_maxrecs(fs)) goto out;
    for (uint32_t i = 0; i < nr; i++) {
        recs[i].start = be32(blk + XFS_BTBLOCK_HDR + i * 8);
        recs[i].len = be32(blk + XFS_BTBLOCK_HDR + i * 8 + 4);
    }
    *n = nr;
    rc = 0;
out:
    free(blk);
    return rc;
}

static int frec_by_size(const xfs_frec_t *a, const xfs_frec_t *b)
{
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return 0;
}

/* writes one tree block (records already sorted) */
static int fs_tree_write(xfs_t *fs, uint32_t ag, uint32_t root, const xfs_frec_t *recs, uint32_t n)
{
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    uint64_t off = agbno_to_byte(fs, ag, root);
    int rc = -1;
    if (xfs_rd(fs, off, fs->blocksize, blk) != 0) goto out;
    put16(blk + 6, (uint16_t)n);
    memset(blk + XFS_BTBLOCK_HDR, 0, fs->blocksize - XFS_BTBLOCK_HDR);
    for (uint32_t i = 0; i < n; i++) {
        put32(blk + XFS_BTBLOCK_HDR + i * 8, recs[i].start);
        put32(blk + XFS_BTBLOCK_HDR + i * 8 + 4, recs[i].len);
    }
    xfs_cksum_update(blk, fs->blocksize, XFS_BTBLOCK_CRC_OFF);
    rc = xfs_wr(fs, off, fs->blocksize, blk);
out:
    free(blk);
    return rc;
}

/* Stores a new free-extent list (sorted by start) in both trees and the AGF counters. */
static int ag_free_store(xfs_t *fs, uint32_t ag, xfs_frec_t *recs, uint32_t n)
{
    if (n > fs_tree_maxrecs(fs)) return -1;
    uint8_t agf[512];
    if (ag_hdr_read(fs, ag, XFS_AGF_SECTOR, agf) != 0 || be32(agf) != XFS_AGF_MAGIC) return -1;
    if (fs_tree_write(fs, ag, be32(agf + 16), recs, n) != 0) return -1;
    xfs_frec_t *bysz = (xfs_frec_t *)malloc((n ? n : 1) * sizeof(xfs_frec_t));
    if (!bysz) return -1;
    uint32_t total = 0, longest = 0;
    for (uint32_t i = 0; i < n; i++) {
        xfs_frec_t t = recs[i];
        uint32_t j = i;
        while (j > 0 && frec_by_size(&bysz[j - 1], &t) > 0) { bysz[j] = bysz[j - 1]; j--; }
        bysz[j] = t;
        total += recs[i].len;
        if (recs[i].len > longest) longest = recs[i].len;
    }
    int rc = fs_tree_write(fs, ag, be32(agf + 20), bysz, n);
    free(bysz);
    if (rc != 0) return -1;
    put32(agf + 52, total);
    put32(agf + 56, longest);
    return ag_hdr_write(fs, ag, XFS_AGF_SECTOR, agf, XFS_AGF_CRC_OFF);
}

/* Takes up to maxlen (at least minlen) blocks out of the AG's free space; the
 * start is a multiple of `align`. Prefers an extent that holds all of maxlen. */
static int ag_alloc(xfs_t *fs, uint32_t ag, uint32_t minlen, uint32_t maxlen, uint32_t align, uint32_t *out_bno, uint32_t *out_len)
{
    uint32_t cap = fs_tree_maxrecs(fs), n;
    xfs_frec_t *recs = (xfs_frec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_frec_t));
    if (!recs) return -1;
    if (ag_free_load(fs, ag, recs, cap, &n) != 0) { free(recs); return -1; }
    if (align == 0) align = 1;
    int best = -1;
    uint32_t best_as = 0, best_take = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rs = recs[i].start, re = rs + recs[i].len;
        uint32_t as = (rs + align - 1) / align * align;
        if (as >= re || re - as < minlen) continue;
        uint32_t avail = re - as, take = avail < maxlen ? avail : maxlen;
        if (best < 0 || take > best_take) { best = (int)i; best_as = as; best_take = take; }
        if (take == maxlen) break;
    }
    if (best < 0) { free(recs); return 1; }
    uint32_t rs = recs[best].start, re = rs + recs[best].len;
    uint32_t left = best_as - rs, right = re - (best_as + best_take);
    /* rebuild the list around the hole we cut */
    uint32_t pos = (uint32_t)best;
    xfs_frec_t tail_rec = { best_as + best_take, right };
    if (left) { recs[pos].start = rs; recs[pos].len = left; pos++; }
    else { memmove(&recs[pos], &recs[pos + 1], (size_t)(n - pos - 1) * sizeof(xfs_frec_t)); n--; }
    if (right) {
        memmove(&recs[pos + 1], &recs[pos], (size_t)(n - pos) * sizeof(xfs_frec_t));
        recs[pos] = tail_rec;
        n++;
    }
    int rc = ag_free_store(fs, ag, recs, n);
    free(recs);
    if (rc != 0) return -1;
    *out_bno = best_as;
    *out_len = best_take;
    return 0;
}

static int ag_free(xfs_t *fs, uint32_t ag, uint32_t bno, uint32_t len)
{
    if (len == 0) return 0;
    uint32_t cap = fs_tree_maxrecs(fs), n;
    xfs_frec_t *recs = (xfs_frec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_frec_t));
    if (!recs) return -1;
    if (ag_free_load(fs, ag, recs, cap, &n) != 0) { free(recs); return -1; }
    uint32_t i = 0;
    while (i < n && recs[i].start < bno) i++;
    if ((i < n && bno + len > recs[i].start) || (i > 0 && recs[i - 1].start + recs[i - 1].len > bno)) { free(recs); return -1; }   /* already free: corrupt */
    int merge_prev = i > 0 && recs[i - 1].start + recs[i - 1].len == bno;
    int merge_next = i < n && bno + len == recs[i].start;
    if (merge_prev && merge_next) {
        recs[i - 1].len += len + recs[i].len;
        memmove(&recs[i], &recs[i + 1], (size_t)(n - i - 1) * sizeof(xfs_frec_t));
        n--;
    } else if (merge_prev) {
        recs[i - 1].len += len;
    } else if (merge_next) {
        recs[i].start = bno;
        recs[i].len += len;
    } else {
        memmove(&recs[i + 1], &recs[i], (size_t)(n - i) * sizeof(xfs_frec_t));
        recs[i].start = bno;
        recs[i].len = len;
        n++;
    }
    int rc = ag_free_store(fs, ag, recs, n);
    free(recs);
    return rc;
}

/* blocks for a file: from the preferred AG, then the others */
static int xfs_alloc_blocks(xfs_t *fs, uint32_t pref_ag, uint32_t minlen, uint32_t maxlen, uint32_t align,
                            uint64_t *fsb, uint32_t *len)
{
    for (uint32_t k = 0; k < fs->agcount; k++) {
        uint32_t ag = (pref_ag + k) % fs->agcount, bno, got;
        int rc = ag_alloc(fs, ag, minlen, maxlen, align, &bno, &got);
        if (rc < 0) return -1;
        if (rc == 0) {
            *fsb = agbno_to_fsb(fs, ag, bno);
            *len = got;
            if (sb_adjust(fs, 0, 0, -(int64_t)got) != 0) return -1;
            return 0;
        }
    }
    return 1;
}

static int xfs_free_blocks(xfs_t *fs, uint64_t fsb, uint32_t len)
{
    uint32_t ag = (uint32_t)(fsb >> fs->agblklog);
    uint32_t agbno = (uint32_t)(fsb & (((uint64_t)1 << fs->agblklog) - 1));
    if (ag >= fs->agcount || agbno + len > fs->agblocks) return -1;
    if (ag_free(fs, ag, agbno, len) != 0) return -1;
    return sb_adjust(fs, 0, 0, (int64_t)len);
}

/* ---------- inode allocation (inode B-tree and free-inode B-tree, one block each) ---------- */

typedef struct {
    uint32_t startino;
    uint16_t holemask;
    uint8_t count;
    uint8_t freecount;
    uint64_t free;
} xfs_irec_t;

#define XFS_INODES_PER_CHUNK 64

static uint32_t ibt_maxrecs(xfs_t *fs) { return (fs->blocksize - XFS_BTBLOCK_HDR) / 16; }

static void irec_decode(xfs_t *fs, const uint8_t *p, xfs_irec_t *r)
{
    r->startino = be32(p);
    if (fs->spinodes) {
        r->holemask = be16(p + 4);
        r->count = p[6];
        r->freecount = p[7];
    } else {
        r->holemask = 0;
        r->count = XFS_INODES_PER_CHUNK;
        r->freecount = (uint8_t)be32(p + 4);
    }
    r->free = be64(p + 8);
}

static void irec_encode(xfs_t *fs, uint8_t *p, const xfs_irec_t *r)
{
    put32(p, r->startino);
    if (fs->spinodes) {
        put16(p + 4, r->holemask);
        p[6] = r->count;
        p[7] = r->freecount;
    } else {
        put32(p + 4, r->freecount);
    }
    put64(p + 8, r->free);
}

/* which = 0: inobt, 1: finobt */
static int ibt_root(xfs_t *fs, uint32_t ag, int which, uint32_t *root)
{
    uint8_t agi[512];
    if (ag_hdr_read(fs, ag, XFS_AGI_SECTOR, agi) != 0 || be32(agi) != XFS_AGI_MAGIC) return -1;
    if (which == 0) { *root = be32(agi + 20); return be32(agi + 24) == 1 ? 0 : -1; }
    *root = be32(agi + 328);
    return be32(agi + 332) == 1 ? 0 : -1;
}

static int ibt_load(xfs_t *fs, uint32_t ag, int which, xfs_irec_t *recs, uint32_t cap, uint32_t *n)
{
    uint32_t root;
    if (ibt_root(fs, ag, which, &root) != 0) return -1;
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    int rc = -1;
    uint32_t magic = which == 0 ? XFS_IBT_CRC_MAGIC : XFS_FIBT_CRC_MAGIC;
    if (xfs_rd(fs, agbno_to_byte(fs, ag, root), fs->blocksize, blk) != 0) goto out;
    if (be32(blk) != magic || be16(blk + 4) != 0) goto out;
    uint32_t nr = be16(blk + 6);
    if (nr > cap || nr > ibt_maxrecs(fs)) goto out;
    for (uint32_t i = 0; i < nr; i++) irec_decode(fs, blk + XFS_BTBLOCK_HDR + i * 16, &recs[i]);
    *n = nr;
    rc = 0;
out:
    free(blk);
    return rc;
}

static int ibt_store(xfs_t *fs, uint32_t ag, int which, const xfs_irec_t *recs, uint32_t n)
{
    if (n > ibt_maxrecs(fs)) return -1;
    uint32_t root;
    if (ibt_root(fs, ag, which, &root) != 0) return -1;
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    uint64_t off = agbno_to_byte(fs, ag, root);
    int rc = -1;
    if (xfs_rd(fs, off, fs->blocksize, blk) != 0) goto out;
    put16(blk + 6, (uint16_t)n);
    memset(blk + XFS_BTBLOCK_HDR, 0, fs->blocksize - XFS_BTBLOCK_HDR);
    for (uint32_t i = 0; i < n; i++) irec_encode(fs, blk + XFS_BTBLOCK_HDR + i * 16, &recs[i]);
    xfs_cksum_update(blk, fs->blocksize, XFS_BTBLOCK_CRC_OFF);
    rc = xfs_wr(fs, off, fs->blocksize, blk);
out:
    free(blk);
    return rc;
}

/* sets the record for startino in the finobt (inserting, updating or removing it) */
static int finobt_sync(xfs_t *fs, uint32_t ag, const xfs_irec_t *r)
{
    if (!fs->finobt) return 0;
    uint32_t cap = ibt_maxrecs(fs), n;
    xfs_irec_t *recs = (xfs_irec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_irec_t));
    if (!recs) return -1;
    if (ibt_load(fs, ag, 1, recs, cap, &n) != 0) { free(recs); return -1; }
    uint32_t i = 0;
    while (i < n && recs[i].startino < r->startino) i++;
    int have = i < n && recs[i].startino == r->startino;
    if (r->freecount == 0) {
        if (have) { memmove(&recs[i], &recs[i + 1], (size_t)(n - i - 1) * sizeof(xfs_irec_t)); n--; }
    } else if (have) {
        recs[i] = *r;
    } else {
        memmove(&recs[i + 1], &recs[i], (size_t)(n - i) * sizeof(xfs_irec_t));
        recs[i] = *r;
        n++;
    }
    int rc = ibt_store(fs, ag, 1, recs, n);
    free(recs);
    return rc;
}

static int agi_adjust(xfs_t *fs, uint32_t ag, int32_t d_count, int32_t d_free, int set_newino, uint32_t newino)
{
    uint8_t agi[512];
    if (ag_hdr_read(fs, ag, XFS_AGI_SECTOR, agi) != 0 || be32(agi) != XFS_AGI_MAGIC) return -1;
    put32(agi + 16, be32(agi + 16) + (uint32_t)d_count);
    put32(agi + 28, be32(agi + 28) + (uint32_t)d_free);
    if (set_newino) put32(agi + 32, newino);
    return ag_hdr_write(fs, ag, XFS_AGI_SECTOR, agi, XFS_AGI_CRC_OFF);
}

/* An unused inode slot as mkfs writes it. */
static void inode_init_free(xfs_t *fs, uint8_t *raw, uint64_t ino, uint32_t gen)
{
    memset(raw, 0, fs->inodesize);
    put16(raw, XFS_DINODE_MAGIC);
    raw[4] = 3;
    raw[5] = XFS_DINODE_FMT_EXTENTS;
    raw[83] = XFS_DINODE_FMT_EXTENTS;       /* attribute fork format */
    put32(raw + 92, gen);
    put32(raw + 96, XFS_NULLAGINO);
    put64(raw + 152, ino);
    memcpy(raw + 160, fs->meta_uuid, 16);
}

/* a new chunk of 64 inodes in this AG */
static int ialloc_chunk(xfs_t *fs, uint32_t ag)
{
    uint8_t agi[512];
    if (ag_hdr_read(fs, ag, XFS_AGI_SECTOR, agi) != 0 || be32(agi) != XFS_AGI_MAGIC) return -1;
    uint64_t icount = 0, dblocks_bytes = fs->dblocks * fs->blocksize;
    {
        uint8_t sb[512];
        if (xfs_rd(fs, 0, fs->sectsize < sizeof(sb) ? fs->sectsize : sizeof(sb), sb) != 0) return -1;
        icount = be64(sb + 128);
    }
    if (fs->imax_pct && (icount + XFS_INODES_PER_CHUNK) * fs->inodesize > dblocks_bytes / 100 * fs->imax_pct) return 1;
    uint32_t blocks = XFS_INODES_PER_CHUNK * fs->inodesize / fs->blocksize;
    uint32_t cap = ibt_maxrecs(fs), n, bno, got;
    xfs_irec_t *recs = (xfs_irec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_irec_t));
    if (!recs) return -1;
    if (ibt_load(fs, ag, 0, recs, cap, &n) != 0) { free(recs); return -1; }
    if (n + 1 > cap) { free(recs); return 1; }
    int rc = ag_alloc(fs, ag, blocks, blocks, fs->inoalign ? fs->inoalign : 1, &bno, &got);
    if (rc != 0) { free(recs); return rc < 0 ? -1 : 1; }
    uint32_t startino = (ag << (fs->agblklog + fs->inopblog)) | (bno << fs->inopblog);
    uint8_t *raw = (uint8_t *)malloc(fs->inodesize);
    if (!raw) { free(recs); return -1; }
    for (uint32_t i = 0; i < XFS_INODES_PER_CHUNK; i++) {
        inode_init_free(fs, raw, (uint64_t)startino + i, 0);
        xfs_cksum_update(raw, fs->inodesize, 100);
        if (xfs_wr(fs, agbno_to_byte(fs, ag, bno) + (uint64_t)i * fs->inodesize, fs->inodesize, raw) != 0) { free(raw); free(recs); return -1; }
    }
    free(raw);
    xfs_irec_t nr = { startino, 0, XFS_INODES_PER_CHUNK, XFS_INODES_PER_CHUNK, ~0ULL };
    uint32_t pos = 0;
    while (pos < n && recs[pos].startino < startino) pos++;
    memmove(&recs[pos + 1], &recs[pos], (size_t)(n - pos) * sizeof(xfs_irec_t));
    recs[pos] = nr;
    n++;
    rc = ibt_store(fs, ag, 0, recs, n);
    free(recs);
    if (rc != 0) return -1;
    if (finobt_sync(fs, ag, &nr) != 0) return -1;
    if (agi_adjust(fs, ag, XFS_INODES_PER_CHUNK, XFS_INODES_PER_CHUNK, 1, startino) != 0) return -1;
    if (sb_adjust(fs, XFS_INODES_PER_CHUNK, XFS_INODES_PER_CHUNK, -(int64_t)got) != 0) return -1;
    return 0;
}

/* takes a free inode from an existing chunk of the AG; 1 = none there */
static int ialloc_take(xfs_t *fs, uint32_t ag, uint64_t *ino)
{
    uint8_t agi[512];
    if (ag_hdr_read(fs, ag, XFS_AGI_SECTOR, agi) != 0 || be32(agi) != XFS_AGI_MAGIC) return -1;
    if (be32(agi + 28) == 0) return 1;
    uint32_t cap = ibt_maxrecs(fs), n;
    xfs_irec_t *recs = (xfs_irec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_irec_t));
    if (!recs) return -1;
    if (ibt_load(fs, ag, 0, recs, cap, &n) != 0) { free(recs); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        if (recs[i].freecount == 0) continue;
        for (int b = 0; b < XFS_INODES_PER_CHUNK; b++) {
            if (!((recs[i].free >> b) & 1)) continue;
            if (recs[i].holemask & (1u << (b / 4))) continue;       /* a hole in a sparse chunk */
            recs[i].free &= ~(1ULL << b);
            recs[i].freecount--;
            xfs_irec_t r = recs[i];
            int rc = ibt_store(fs, ag, 0, recs, n);
            free(recs);
            if (rc != 0) return -1;
            if (finobt_sync(fs, ag, &r) != 0) return -1;
            if (agi_adjust(fs, ag, 0, -1, 0, 0) != 0) return -1;
            if (sb_adjust(fs, 0, -1, 0) != 0) return -1;
            *ino = (uint64_t)r.startino + (uint32_t)b;
            return 0;
        }
    }
    free(recs);
    return 1;
}

static int xfs_ialloc(xfs_t *fs, uint32_t pref_ag, uint64_t *ino)
{
    for (uint32_t k = 0; k < fs->agcount; k++) {
        int rc = ialloc_take(fs, (pref_ag + k) % fs->agcount, ino);
        if (rc <= 0) return rc;
    }
    for (uint32_t k = 0; k < fs->agcount; k++) {
        uint32_t ag = (pref_ag + k) % fs->agcount;
        int rc = ialloc_chunk(fs, ag);
        if (rc < 0) return -1;
        if (rc > 0) continue;
        rc = ialloc_take(fs, ag, ino);
        if (rc <= 0) return rc;
    }
    return 1;
}

static int xfs_ifree(xfs_t *fs, uint64_t ino)
{
    uint32_t ag = (uint32_t)(ino >> (fs->agblklog + fs->inopblog));
    uint32_t agino = (uint32_t)(ino & (((uint64_t)1 << (fs->agblklog + fs->inopblog)) - 1));
    if (ag >= fs->agcount) return -1;
    uint32_t cap = ibt_maxrecs(fs), n;
    xfs_irec_t *recs = (xfs_irec_t *)malloc((size_t)(cap + 2) * sizeof(xfs_irec_t));
    if (!recs) return -1;
    if (ibt_load(fs, ag, 0, recs, cap, &n) != 0) { free(recs); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = ((uint64_t)recs[i].startino);
        if (agino < s || agino >= s + XFS_INODES_PER_CHUNK) continue;
        int b = (int)(agino - s);
        if ((recs[i].free >> b) & 1) { free(recs); return -1; }       /* already free */
        recs[i].free |= 1ULL << b;
        recs[i].freecount++;
        xfs_irec_t r = recs[i];
        int rc = ibt_store(fs, ag, 0, recs, n);
        free(recs);
        if (rc != 0) return -1;
        if (finobt_sync(fs, ag, &r) != 0) return -1;
        if (agi_adjust(fs, ag, 0, 1, 0, 0) != 0) return -1;
        return sb_adjust(fs, 0, 1, 0);
    }
    free(recs);
    return -1;
}

/* ---------- data fork extents ---------- */

typedef struct {
    xfs_ext_t *list;
    uint32_t n, cap;
} ext_list_t;

static int ext_reserve(ext_list_t *l, uint32_t need)
{
    if (need <= l->cap) return 0;
    uint32_t nc = l->cap ? l->cap * 2 : 16;
    while (nc < need) nc *= 2;
    xfs_ext_t *nl = (xfs_ext_t *)malloc(nc * sizeof(xfs_ext_t));
    if (!nl) return -1;
    if (l->n) memcpy(nl, l->list, l->n * sizeof(xfs_ext_t));
    free(l->list);
    l->list = nl;
    l->cap = nc;
    return 0;
}

static int ext_add(ext_list_t *l, const uint8_t *rec)
{
    uint64_t hi = be64(rec), lo = be64(rec + 8);
    if (ext_reserve(l, l->n + 1) != 0) return -1;
    xfs_ext_t *e = &l->list[l->n++];
    e->unwritten = (int)(hi >> 63);
    e->startoff = (hi >> 9) & (((uint64_t)1 << 54) - 1);
    e->startblock = ((hi & 0x1FF) << 43) | (lo >> 21);
    e->blockcount = lo & 0x1FFFFF;
    return 0;
}

static void ext_pack(const xfs_ext_t *e, uint8_t *rec)
{
    uint64_t hi = ((uint64_t)(e->unwritten ? 1 : 0) << 63) | ((e->startoff & (((uint64_t)1 << 54) - 1)) << 9) | (e->startblock >> 43);
    uint64_t lo = ((e->startblock & (((uint64_t)1 << 43) - 1)) << 21) | (e->blockcount & 0x1FFFFF);
    put64(rec, hi);
    put64(rec + 8, lo);
}

/* Walks a bmap btree block (long-format pointers) collecting leaf records;
 * the blocks of the tree themselves go to `blocks` when it is given. */
static int bmbt_walk(xfs_t *fs, uint64_t fsb, int depth, ext_list_t *l, ext_list_t *blocks)
{
    if (depth > 6) return -1;
    uint8_t *blk = (uint8_t *)malloc(fs->blocksize);
    if (!blk) return -1;
    int rc = -1;
    if (xfs_rd(fs, fsb_to_byte(fs, fsb), fs->blocksize, blk) != 0) goto out;
    uint32_t magic = be32(blk);
    if (magic != 0x424D4150u && magic != 0x424D4133u) goto out;   /* "BMAP" / "BMA3" */
    uint16_t level = be16(blk + 4), nrecs = be16(blk + 6);
    int hdr = (magic == 0x424D4133u) ? 72 : 24;
    if (blocks) {
        if (ext_reserve(blocks, blocks->n + 1) != 0) goto out;
        blocks->list[blocks->n].startblock = fsb;
        blocks->list[blocks->n].blockcount = 1;
        blocks->n++;
    }
    if (level == 0) {
        if ((uint64_t)hdr + (uint64_t)nrecs * 16 > fs->blocksize) goto out;
        for (uint16_t i = 0; i < nrecs; i++)
            if (ext_add(l, blk + hdr + (size_t)i * 16) != 0) goto out;
    } else {
        uint32_t maxrecs = (fs->blocksize - (uint32_t)hdr) / 16;
        if (nrecs > maxrecs) goto out;
        for (uint16_t i = 0; i < nrecs; i++) {
            uint64_t child = be64(blk + hdr + (size_t)maxrecs * 8 + (size_t)i * 8);
            if (bmbt_walk(fs, child, depth + 1, l, blocks) != 0) goto out;
        }
    }
    rc = 0;
out:
    free(blk);
    return rc;
}

static int get_extents_ex(xfs_t *fs, const xfs_inode_t *ip, ext_list_t *l, ext_list_t *blocks)
{
    memset(l, 0, sizeof(*l));
    if (blocks) memset(blocks, 0, sizeof(*blocks));
    if (ip->format == XFS_DINODE_FMT_EXTENTS) {
        if ((int64_t)ip->nextents * 16 > ip->fork_len) return -1;
        for (uint32_t i = 0; i < ip->nextents; i++)
            if (ext_add(l, ip->fork + (size_t)i * 16) != 0) { free(l->list); l->list = 0; return -1; }
        return 0;
    }
    if (ip->format == XFS_DINODE_FMT_BTREE) {
        uint16_t level = be16(ip->fork), nrecs = be16(ip->fork + 2);
        uint32_t maxrecs = (uint32_t)(ip->fork_len - 4) / 16;
        if (level == 0 || nrecs > maxrecs) return -1;
        for (uint16_t i = 0; i < nrecs; i++) {
            uint64_t child = be64(ip->fork + 4 + (size_t)maxrecs * 8 + (size_t)i * 8);
            if (bmbt_walk(fs, child, 1, l, blocks) != 0) {
                free(l->list); l->list = 0;
                if (blocks) { free(blocks->list); blocks->list = 0; }
                return -1;
            }
        }
        return 0;
    }
    return -1;
}

static int get_extents(xfs_t *fs, const xfs_inode_t *ip, ext_list_t *l)
{
    return get_extents_ex(fs, ip, l, 0);
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
        if (xfs_rd(fs, fsb_to_byte(fs, e->startblock) + (from - estart), (uint32_t)(to - from), buf + (from - pos)) != 0) {
            free(l.list);
            return -1;
        }
    }
    free(l.list);
    return (int)len;
}

/* ---------- data fork editing ---------- */

/* Stores the extent list in an extent-format fork; fails when it does not fit. */
static int fork_set_extents(xfs_inode_t *ip, const ext_list_t *l)
{
    if (ip->format != XFS_DINODE_FMT_EXTENTS || (int64_t)l->n * 16 > ip->fork_len) return -1;
    uint8_t *fork = ip->raw + ip->core_len;
    memset(fork, 0, (size_t)ip->fork_len);
    for (uint32_t i = 0; i < l->n; i++) ext_pack(&l->list[i], fork + (size_t)i * 16);
    put32(ip->raw + 76, l->n);
    ip->nextents = l->n;
    return 0;
}

/* inserts an extent keeping the list sorted by file offset, merging neighbours */
static int ext_insert(ext_list_t *l, const xfs_ext_t *e)
{
    if (ext_reserve(l, l->n + 1) != 0) return -1;
    uint32_t i = 0;
    while (i < l->n && l->list[i].startoff < e->startoff) i++;
    memmove(&l->list[i + 1], &l->list[i], (l->n - i) * sizeof(xfs_ext_t));
    l->list[i] = *e;
    l->n++;
    if (i + 1 < l->n) {
        xfs_ext_t *a = &l->list[i], *b = &l->list[i + 1];
        if (a->unwritten == b->unwritten && a->startoff + a->blockcount == b->startoff &&
            a->startblock + a->blockcount == b->startblock && a->blockcount + b->blockcount <= 0x1FFFFF) {
            a->blockcount += b->blockcount;
            memmove(b, b + 1, (l->n - i - 2) * sizeof(xfs_ext_t));
            l->n--;
        }
    }
    if (i > 0) {
        xfs_ext_t *a = &l->list[i - 1], *b = &l->list[i];
        if (a->unwritten == b->unwritten && a->startoff + a->blockcount == b->startoff &&
            a->startblock + a->blockcount == b->startblock && a->blockcount + b->blockcount <= 0x1FFFFF) {
            a->blockcount += b->blockcount;
            memmove(b, b + 1, (l->n - i - 1) * sizeof(xfs_ext_t));
            l->n--;
        }
    }
    return 0;
}

static uint32_t inode_pref_ag(xfs_t *fs, uint64_t ino)
{
    return (uint32_t)(ino >> (fs->agblklog + fs->inopblog));
}

static int zero_blocks(xfs_t *fs, uint64_t fsb, uint32_t count)
{
    uint8_t *z = (uint8_t *)malloc(fs->blocksize);
    if (!z) return -1;
    memset(z, 0, fs->blocksize);
    int rc = 0;
    for (uint32_t i = 0; i < count && rc == 0; i++) rc = xfs_wr(fs, fsb_to_byte(fs, fsb + i), fs->blocksize, z);
    free(z);
    return rc;
}

#define XFS_MAX_HOLES 64

/* Writes buf[0..len) at `pos` of a regular file in extent format: holes in the
 * range get new blocks, existing blocks are overwritten in place. */
static int xfs_file_write(xfs_t *fs, uint64_t ino, uint64_t pos, const uint8_t *buf, uint32_t len)
{
    if (len == 0) return 0;
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    if ((ip.mode & S_IFMT_) != S_IFREG_ || ip.format != XFS_DINODE_FMT_EXTENTS) { inode_free(&ip); return -1; }
    ext_list_t l;
    if (get_extents(fs, &ip, &l) != 0) { inode_free(&ip); return -1; }
    int rc = -1;
    uint32_t bs = fs->blocksize;
    uint64_t end = pos + len;
    uint64_t fb0 = pos / bs, fb1 = (end - 1) / bs;

    /* holes in the written range */
    struct { uint64_t off, cnt; } holes[XFS_MAX_HOLES];
    int nholes = 0;
    uint64_t cur = fb0;
    for (uint32_t i = 0; i < l.n && cur <= fb1; i++) {
        xfs_ext_t *e = &l.list[i];
        if (e->startoff + e->blockcount <= cur) continue;
        if (e->startoff > fb1) break;
        if (e->startoff > cur) {
            if (nholes >= XFS_MAX_HOLES) goto out;
            holes[nholes].off = cur;
            holes[nholes++].cnt = e->startoff - cur;
        }
        cur = e->startoff + e->blockcount;
    }
    if (cur <= fb1) {
        if (nholes >= XFS_MAX_HOLES) goto out;
        holes[nholes].off = cur;
        holes[nholes++].cnt = fb1 - cur + 1;
    }
    if (((int64_t)l.n + nholes) * 16 > ip.fork_len) goto out;        /* too fragmented for the inode */

    uint64_t nblocks = be64(ip.raw + 64);
    uint32_t pref = inode_pref_ag(fs, ino);
    for (int h = 0; h < nholes; h++) {
        uint64_t off = holes[h].off, cnt = holes[h].cnt;
        while (cnt > 0) {
            uint32_t want = cnt > 0x1FFFFF ? 0x1FFFFF : (uint32_t)cnt, got;
            uint64_t fsb;
            int arc = xfs_alloc_blocks(fs, pref, 1, want, 1, &fsb, &got);
            if (arc != 0) goto out_partial;
            /* only the partially written first/last block of a new extent must be cleared */
            for (uint32_t k = 0; k < got; k++) {
                uint64_t fb = off + k;
                int full = pos <= fb * bs && (fb + 1) * bs <= end;
                if (!full && zero_blocks(fs, fsb + k, 1) != 0) goto out_partial;
            }
            xfs_ext_t ne = { off, fsb, got, 0 };
            if (ext_insert(&l, &ne) != 0) goto out_partial;
            nblocks += got;
            off += got;
            cnt -= got;
        }
    }

    /* unwritten (preallocated) extents under the range become written, with zeros where we do not write */
    for (uint32_t i = 0; i < l.n; i++) {
        xfs_ext_t *e = &l.list[i];
        if (!e->unwritten || e->startoff + e->blockcount <= fb0 || e->startoff > fb1) continue;
        if (e->blockcount > 4096 || zero_blocks(fs, e->startblock, (uint32_t)e->blockcount) != 0) goto out_partial;
        e->unwritten = 0;
    }

    rc = 0;
out_partial:
    /* record what was allocated even if a later step failed */
    if (fork_set_extents(&ip, &l) != 0) rc = -1;
    put64(ip.raw + 64, nblocks);
    if (rc == 0 && end > ip.size) { ip.size = end; put64(ip.raw + 56, end); }
    if (write_inode(fs, ino, &ip) != 0) rc = -1;
    if (rc != 0) goto out;

    /* the data itself */
    {
        uint8_t *tmp = (uint8_t *)malloc(bs);
        if (!tmp) { rc = -1; goto out; }
        uint32_t done = 0;
        uint32_t li = 0;
        while (done < len) {
            uint64_t p = pos + done, fb = p / bs;
            uint32_t in_blk = (uint32_t)(p % bs), n = bs - in_blk;
            if (n > len - done) n = len - done;
            while (li > 0 && l.list[li].startoff > fb) li--;
            while (li < l.n && l.list[li].startoff + l.list[li].blockcount <= fb) li++;
            if (li >= l.n || l.list[li].startoff > fb) { rc = -1; break; }
            uint64_t byte = fsb_to_byte(fs, l.list[li].startblock + (fb - l.list[li].startoff));
            if (n == bs) {
                rc = xfs_wr(fs, byte, bs, buf + done);
            } else {
                rc = xfs_rd(fs, byte, bs, tmp);
                if (rc == 0) { memcpy(tmp + in_blk, buf + done, n); rc = xfs_wr(fs, byte, bs, tmp); }
            }
            if (rc != 0) break;
            done += n;
        }
        free(tmp);
    }
out:
    free(l.list);
    inode_free(&ip);
    return rc;
}

/* Frees every block of a file (data and bmap-btree blocks) and empties its fork. */
static int xfs_file_free_blocks(xfs_t *fs, uint64_t ino, xfs_inode_t *ip)
{
    (void)ino;
    if (ip->format == XFS_DINODE_FMT_LOCAL) return 0;
    ext_list_t l, tb;
    if (get_extents_ex(fs, ip, &l, &tb) != 0) return -1;
    int rc = 0;
    for (uint32_t i = 0; i < l.n && rc == 0; i++) {
        xfs_ext_t *e = &l.list[i];
        for (uint64_t done = 0; done < e->blockcount && rc == 0;) {
            uint64_t fsb = e->startblock + done;
            uint32_t agbno = (uint32_t)(fsb & (((uint64_t)1 << fs->agblklog) - 1));
            uint64_t chunk = e->blockcount - done;
            if (chunk > fs->agblocks - agbno) chunk = fs->agblocks - agbno;   /* never spans two AGs, but be safe */
            rc = xfs_free_blocks(fs, fsb, (uint32_t)chunk);
            done += chunk;
        }
    }
    for (uint32_t i = 0; i < tb.n && rc == 0; i++) rc = xfs_free_blocks(fs, tb.list[i].startblock, 1);
    free(l.list);
    free(tb.list);
    if (rc != 0) return -1;
    /* an empty extent-format fork */
    memset(ip->raw + ip->core_len, 0, (size_t)ip->fork_len);
    ip->raw[5] = XFS_DINODE_FMT_EXTENTS;
    ip->format = XFS_DINODE_FMT_EXTENTS;
    put32(ip->raw + 76, 0);
    ip->nextents = 0;
    put64(ip->raw + 64, 0);
    return 0;
}

/* ---------- extended attribute fork (only needs releasing when an inode goes away) ---------- */

static int xfs_attr_free(xfs_t *fs, xfs_inode_t *ip)
{
    uint16_t anext = be16(ip->raw + 80);
    uint8_t aformat = ip->raw[83];
    if (ip->forkoff == 0 || anext == 0 || aformat == XFS_DINODE_FMT_LOCAL) return 0;
    if (aformat != XFS_DINODE_FMT_EXTENTS) return -1;
    const uint8_t *afork = ip->raw + ip->core_len + (size_t)ip->forkoff * 8;
    int alen = (int)fs->inodesize - ip->core_len - (int)ip->forkoff * 8;
    if ((int)anext * 16 > alen) return -1;
    for (uint16_t i = 0; i < anext; i++) {
        ext_list_t tmp = { 0, 0, 0 };
        if (ext_add(&tmp, afork + (size_t)i * 16) != 0) return -1;
        int rc = xfs_free_blocks(fs, tmp.list[0].startblock, (uint32_t)tmp.list[0].blockcount);
        free(tmp.list);
        if (rc != 0) return -1;
    }
    return 0;
}

/* ---------- directories ---------- */

typedef struct {
    uint64_t ino;
    uint8_t ftype;
    uint8_t nlen;
    char name[XFS_MAX_FILENAME + 1];
} xfs_dent_t;

typedef struct {
    xfs_dent_t *e;
    int n, cap;
    uint64_t parent;
    int is_block;
} xfs_dir_t;

static uint32_t xfs_hashname(const uint8_t *name, int len)
{
    uint32_t hash = 0;
#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
    for (; len >= 4; name += 4, len -= 4)
        hash = ((uint32_t)name[0] << 21) ^ ((uint32_t)name[1] << 14) ^ ((uint32_t)name[2] << 7) ^ ((uint32_t)name[3]) ^ ROL32(hash, 7 * 4);
    switch (len) {
    case 3: return ((uint32_t)name[0] << 14) ^ ((uint32_t)name[1] << 7) ^ ((uint32_t)name[2]) ^ ROL32(hash, 7 * 3);
    case 2: return ((uint32_t)name[0] << 7) ^ ((uint32_t)name[1]) ^ ROL32(hash, 7 * 2);
    case 1: return ((uint32_t)name[0]) ^ ROL32(hash, 7 * 1);
    default: return hash;
    }
#undef ROL32
}

static uint32_t dirent_size(xfs_t *fs, int nlen)
{
    return (8u + 1 + (uint32_t)nlen + (fs->ftype ? 1u : 0u) + 2 + 7) & ~7u;
}

static void dir_free(xfs_dir_t *d)
{
    free(d->e);
    d->e = 0;
    d->n = d->cap = 0;
}

static int dir_push(xfs_dir_t *d, uint64_t ino, uint8_t ftype, const uint8_t *name, int nlen)
{
    if (nlen <= 0 || nlen > XFS_MAX_FILENAME) return -1;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 16;
        xfs_dent_t *ne = (xfs_dent_t *)malloc((size_t)nc * sizeof(xfs_dent_t));
        if (!ne) return -1;
        if (d->n) memcpy(ne, d->e, (size_t)d->n * sizeof(xfs_dent_t));
        free(d->e);
        d->e = ne;
        d->cap = nc;
    }
    xfs_dent_t *x = &d->e[d->n++];
    x->ino = ino;
    x->ftype = ftype;
    x->nlen = (uint8_t)nlen;
    memcpy(x->name, name, (size_t)nlen);
    x->name[nlen] = 0;
    return 0;
}

static uint8_t ftype_of_mode(uint16_t mode)
{
    switch (mode & S_IFMT_) {
    case S_IFDIR_: return XFS_FT_DIR;
    case S_IFREG_: return XFS_FT_REG_FILE;
    case 0120000: return 7;
    case 0020000: return 3;
    case 0060000: return 4;
    case 0010000: return 5;
    case 0140000: return 6;
    default: return 0;
    }
}

static int dir_find(const xfs_dir_t *d, const char *name, int nlen)
{
    for (int i = 0; i < d->n; i++)
        if (d->e[i].nlen == nlen && memcmp(d->e[i].name, name, (size_t)nlen) == 0) return i;
    return -1;
}

/* bytes the entries need as a short-form directory */
static uint32_t sf_size(xfs_t *fs, const xfs_dir_t *d, int *i8)
{
    *i8 = d->parent > 0xFFFFFFFFULL;
    for (int i = 0; i < d->n; i++) if (d->e[i].ino > 0xFFFFFFFFULL) *i8 = 1;
    uint32_t isz = *i8 ? 8 : 4, sz = 2 + isz;
    for (int i = 0; i < d->n; i++) sz += 3 + d->e[i].nlen + (fs->ftype ? 1u : 0u) + isz;
    return sz;
}

static void sf_encode(xfs_t *fs, const xfs_dir_t *d, int i8, uint8_t *out)
{
    uint32_t isz = i8 ? 8 : 4;
    out[0] = (uint8_t)d->n;
    out[1] = (uint8_t)(i8 ? d->n : 0);
    if (i8) put64(out + 2, d->parent); else put32(out + 2, (uint32_t)d->parent);
    uint32_t p = 2 + isz;
    uint32_t off = (fs->v5 ? 64u : 16u) + dirent_size(fs, 1) + dirent_size(fs, 2);   /* after the data header, "." and ".." */
    for (int i = 0; i < d->n; i++) {
        const xfs_dent_t *x = &d->e[i];
        out[p] = x->nlen;
        put16(out + p + 1, (uint16_t)off);
        memcpy(out + p + 3, x->name, x->nlen);
        uint32_t q = p + 3 + x->nlen;
        if (fs->ftype) out[q++] = x->ftype;
        if (i8) put64(out + q, x->ino); else put32(out + q, (uint32_t)x->ino);
        p = q + isz;
        off += dirent_size(fs, x->nlen);
    }
}

/* lays the directory out as one block (v5 layout) */
static int dir_block_build(xfs_t *fs, uint64_t dino, uint64_t daddr_bytes, const xfs_dir_t *d, uint8_t *blk)
{
    uint32_t bs = fs->blocksize;
    memset(blk, 0, bs);
    put32(blk, XFS_DIR3_BLOCK_MAGIC);
    put64(blk + 8, daddr_bytes / 512);
    memcpy(blk + 24, fs->meta_uuid, 16);
    put64(blk + 40, dino);
    struct { uint32_t hash, addr; } *leaf = malloc(((size_t)d->n + 2) * 8);
    if (!leaf) return -1;
    uint32_t o = 64;
    int nl = 0;
    for (int i = -2; i < d->n; i++) {
        uint64_t ino;
        uint8_t ftype, nlen;
        const char *name;
        if (i == -2) { ino = dino; ftype = XFS_FT_DIR; nlen = 1; name = "."; }
        else if (i == -1) { ino = d->parent; ftype = XFS_FT_DIR; nlen = 2; name = ".."; }
        else { ino = d->e[i].ino; ftype = d->e[i].ftype; nlen = d->e[i].nlen; name = d->e[i].name; }
        uint32_t esz = dirent_size(fs, nlen);
        if (o + esz > bs) { free(leaf); return 1; }
        put64(blk + o, ino);
        blk[o + 8] = nlen;
        memcpy(blk + o + 9, name, nlen);
        if (fs->ftype) blk[o + 9 + nlen] = ftype;
        put16(blk + o + esz - 2, (uint16_t)o);
        leaf[nl].hash = xfs_hashname((const uint8_t *)name, nlen);
        leaf[nl].addr = o >> 3;
        nl++;
        o += esz;
    }
    uint32_t leaf_start = bs - 8 - (uint32_t)nl * 8;
    if (o > leaf_start) { free(leaf); return 1; }
    for (int i = 1; i < nl; i++) {                       /* sort by hash, then address */
        uint32_t h = leaf[i].hash, a = leaf[i].addr;
        int j = i;
        while (j > 0 && (leaf[j - 1].hash > h || (leaf[j - 1].hash == h && leaf[j - 1].addr > a))) { leaf[j] = leaf[j - 1]; j--; }
        leaf[j].hash = h;
        leaf[j].addr = a;
    }
    for (int i = 0; i < nl; i++) {
        put32(blk + leaf_start + (uint32_t)i * 8, leaf[i].hash);
        put32(blk + leaf_start + (uint32_t)i * 8 + 4, leaf[i].addr);
    }
    free(leaf);
    put32(blk + bs - 8, (uint32_t)nl);
    put32(blk + bs - 4, 0);
    if (leaf_start > o) {                               /* the gap in front of the leaf entries is one free region */
        put16(blk + o, 0xFFFF);
        put16(blk + o + 2, (uint16_t)(leaf_start - o));
        put16(blk + leaf_start - 2, (uint16_t)o);
        put16(blk + 48, (uint16_t)o);
        put16(blk + 50, (uint16_t)(leaf_start - o));
    }
    xfs_cksum_update(blk, bs, 4);
    return 0;
}

#define XFS_DIR2_LEAF_OFFSET ((uint64_t)1 << 35)     /* byte offsets of the leaf and free-index areas of a directory */
#define XFS_DIR2_FREE_OFFSET ((uint64_t)1 << 36)
#define XFS_DIR3_FREE_MAGIC  0x58444633u             /* "XDF3" */
#define XFS_DIR3_LEAFN_MAGIC 0x3dff
#define XFS_DA3_NODE_MAGIC   0x3ebe

/* Loads the entries of a short-form, block, leaf or node directory (without "." and ".."). */
static int dir_load(xfs_t *fs, const xfs_inode_t *dir, xfs_dir_t *d)
{
    memset(d, 0, sizeof(*d));
    if (dir->format == XFS_DINODE_FMT_LOCAL) {
        const uint8_t *f = dir->fork;
        if (dir->fork_len < 6) return -1;
        int count = f[0], isz = f[1] ? 8 : 4;
        d->parent = isz == 8 ? be64(f + 2) : be32(f + 2);
        int p = 2 + isz;
        for (int i = 0; i < count; i++) {
            if (p + 3 > dir->fork_len) { dir_free(d); return -1; }
            int nl = f[p], q = p + 3;
            if (q + nl + (fs->ftype ? 1 : 0) + isz > dir->fork_len) { dir_free(d); return -1; }
            const uint8_t *name = f + q;
            q += nl;
            uint8_t ft = 0;
            if (fs->ftype) ft = f[q++];
            uint64_t ino = isz == 8 ? be64(f + q) : be32(f + q);
            q += isz;
            if (dir_push(d, ino, ft, name, nl) != 0) { dir_free(d); return -1; }
            p = q;
        }
        return 0;
    }
    if (dir->format != XFS_DINODE_FMT_EXTENTS || fs->dirblklog != 0 || !fs->v5) return -2;
    uint32_t bs = fs->blocksize;
    ext_list_t l;
    if (get_extents(fs, dir, &l) != 0) return -2;
    uint8_t *blk = (uint8_t *)malloc(bs);
    if (!blk) { free(l.list); return -1; }
    uint64_t leaf_blk = XFS_DIR2_LEAF_OFFSET / bs;
    int rc = 0, saw = 0;
    for (uint32_t i = 0; i < l.n && rc == 0; i++) {
        xfs_ext_t *e = &l.list[i];
        if (e->startoff >= leaf_blk) continue;
        if (e->unwritten) { rc = -2; break; }
        for (uint64_t k = 0; k < e->blockcount && e->startoff + k < leaf_blk && rc == 0; k++) {
            if (xfs_rd(fs, fsb_to_byte(fs, e->startblock + k), bs, blk) != 0) { rc = -1; break; }
            uint32_t magic = be32(blk);
            uint32_t end = bs;
            if (magic == XFS_DIR3_BLOCK_MAGIC) {
                if (saw) { rc = -2; break; }
                uint32_t cnt = be32(blk + bs - 8);
                if ((uint64_t)cnt * 8 + 8 > bs) { rc = -1; break; }
                end = bs - 8 - cnt * 8;
                d->is_block = 1;
            } else if (magic == XFS_DIR3_DATA_MAGIC) {
                d->is_block = 2;
            } else { rc = -2; break; }
            saw++;
            uint32_t o = 64;
            while (o + 8 <= end) {
                if (be16(blk + o) == 0xFFFF) {
                    uint16_t len = be16(blk + o + 2);
                    if (len < 8) { rc = -1; break; }
                    o += len;
                    continue;
                }
                uint64_t ino = be64(blk + o);
                uint8_t nl = blk[o + 8];
                uint32_t esz = dirent_size(fs, nl);
                if (nl == 0 || o + esz > end) { rc = -1; break; }
                uint8_t ft = fs->ftype ? blk[o + 9 + nl] : 0;
                if (nl == 1 && blk[o + 9] == '.') { /* self */ }
                else if (nl == 2 && blk[o + 9] == '.' && blk[o + 10] == '.') d->parent = ino;
                else if (dir_push(d, ino, ft, blk + o + 9, nl) != 0) { rc = -1; break; }
                o += esz;
            }
        }
    }
    free(blk);
    free(l.list);
    if (rc == 0 && !saw) rc = -2;
    if (rc != 0) dir_free(d);
    return rc;
}

/* Makes the directory's blocks exactly the logical blocks in need[] (sorted, no
 * duplicates): keeps the ones it has, allocates the missing, frees the rest.
 * phys[i] receives the filesystem block of need[i]. Rewrites the inode's extent
 * list (without writing the inode). */
static int dir_remap(xfs_t *fs, uint64_t dino, xfs_inode_t *dir, const uint64_t *need, int nneed, uint64_t *phys)
{
    ext_list_t l;
    if (dir->format == XFS_DINODE_FMT_EXTENTS) {
        if (get_extents(fs, dir, &l) != 0) return -2;
    } else {
        memset(&l, 0, sizeof(l));
    }
    int cap = nneed ? nneed : 1;
    int *have = (int *)malloc((size_t)cap * sizeof(int));
    struct { uint64_t fsb; uint32_t len; } *fresh = malloc((size_t)cap * sizeof(*fresh));
    if (!have || !fresh) { free(have); free(fresh); free(l.list); return -1; }
    memset(have, 0, (size_t)cap * sizeof(int));
    int nfresh = 0, rc = 0;
    uint32_t pref = inode_pref_ag(fs, dino);
    /* which needed blocks already exist */
    for (uint32_t i = 0; i < l.n; i++) {
        if (l.list[i].unwritten) continue;
        for (uint64_t k = 0; k < l.list[i].blockcount; k++) {
            uint64_t lb = l.list[i].startoff + k;
            int lo = 0, hi = nneed - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (need[mid] == lb) { phys[mid] = l.list[i].startblock + k; have[mid] = 1; break; }
                if (need[mid] < lb) lo = mid + 1; else hi = mid - 1;
            }
        }
    }
    /* allocate the missing ones, runs of consecutive logical blocks together */
    for (int i = 0; i < nneed && rc == 0;) {
        if (have[i]) { i++; continue; }
        int run = 1;
        while (i + run < nneed && !have[i + run] && need[i + run] == need[i] + (uint64_t)run) run++;
        uint64_t fsb;
        uint32_t got;
        int arc = xfs_alloc_blocks(fs, pref, 1, (uint32_t)run, 1, &fsb, &got);
        if (arc != 0) { rc = arc < 0 ? -1 : 1; break; }
        fresh[nfresh].fsb = fsb;
        fresh[nfresh++].len = got;
        for (uint32_t k = 0; k < got; k++) { phys[i + (int)k] = fsb + k; have[i + (int)k] = 1; }
        i += (int)got;
    }
    ext_list_t nl;
    memset(&nl, 0, sizeof(nl));
    for (int i = 0; i < nneed && rc == 0; i++) {
        xfs_ext_t e = { need[i], phys[i], 1, 0 };
        if (ext_insert(&nl, &e) != 0) rc = -1;
    }
    if (rc == 0 && (int64_t)nl.n * 16 > dir->fork_len) rc = 1;           /* too many extents for the inode */
    if (rc != 0) {                                                       /* undo: nothing of the old layout was touched */
        for (int i = 0; i < nfresh; i++) xfs_free_blocks(fs, fresh[i].fsb, fresh[i].len);
        free(have); free(fresh); free(nl.list); free(l.list);
        return rc;
    }
    /* free the old blocks that are no longer needed */
    for (uint32_t i = 0; i < l.n && rc == 0; i++) {
        for (uint64_t k = 0; k < l.list[i].blockcount; k++) {
            uint64_t lb = l.list[i].startoff + k, fsb = l.list[i].startblock + k;
            int lo = 0, hi = nneed - 1, at = -1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (need[mid] == lb) { at = mid; break; }
                if (need[mid] < lb) lo = mid + 1; else hi = mid - 1;
            }
            if ((at < 0 || l.list[i].unwritten || phys[at] != fsb) && xfs_free_blocks(fs, fsb, 1) != 0) { rc = -1; break; }
        }
    }
    free(have); free(fresh); free(l.list);
    if (rc != 0) { free(nl.list); return rc; }
    dir->raw[5] = XFS_DINODE_FMT_EXTENTS;
    dir->format = XFS_DINODE_FMT_EXTENTS;
    int frc = fork_set_extents(dir, &nl);
    free(nl.list);
    if (frc != 0) return -1;
    put64(dir->raw + 64, (uint64_t)nneed);
    return 0;
}

typedef struct { uint32_t hash, addr; } leafent_t;

static void dir_hdr3(xfs_t *fs, uint8_t *blk, uint32_t magic, uint64_t daddr_bytes, uint64_t owner)
{
    put32(blk, magic);
    put64(blk + 8, daddr_bytes / 512);
    memcpy(blk + 24, fs->meta_uuid, 16);
    put64(blk + 40, owner);
}

static void da3_hdr(xfs_t *fs, uint8_t *blk, uint16_t magic, uint32_t forw, uint32_t back, uint64_t daddr_bytes, uint64_t owner)
{
    put32(blk, forw);
    put32(blk + 4, back);
    put16(blk + 8, magic);
    put64(blk + 16, daddr_bytes / 512);
    memcpy(blk + 32, fs->meta_uuid, 16);
    put64(blk + 48, owner);
}

/* A directory too big for one block: data blocks, leaf blocks under a node block,
 * and a free-space index, all rebuilt from the entry list. */
static int dir_multi_store(xfs_t *fs, uint64_t dino, xfs_inode_t *dir, const xfs_dir_t *d)
{
    uint32_t bs = fs->blocksize;
    uint32_t total = (uint32_t)d->n + 2;
    uint64_t leaf_dablk = XFS_DIR2_LEAF_OFFSET / bs, free_dablk = XFS_DIR2_FREE_OFFSET / bs;
    int rc = -1;
    uint32_t dcap = 16, nd = 0;
    uint8_t **db = (uint8_t **)malloc(dcap * sizeof(uint8_t *));
    uint32_t *dpos = (uint32_t *)malloc(dcap * sizeof(uint32_t));
    leafent_t *le = (leafent_t *)malloc((size_t)total * sizeof(leafent_t));
    uint64_t *need = 0, *phys = 0;
    uint32_t *lcnt = 0;
    uint8_t *buf = 0;
    if (!db || !dpos || !le) goto out;

    /* data blocks */
    for (int i = -2; i < d->n; i++) {
        uint64_t ino;
        uint8_t ftype, nlen;
        const char *name;
        if (i == -2) { ino = dino; ftype = XFS_FT_DIR; nlen = 1; name = "."; }
        else if (i == -1) { ino = d->parent; ftype = XFS_FT_DIR; nlen = 2; name = ".."; }
        else { ino = d->e[i].ino; ftype = d->e[i].ftype; nlen = d->e[i].nlen; name = d->e[i].name; }
        uint32_t esz = dirent_size(fs, nlen);
        if (nd == 0 || dpos[nd - 1] + esz > bs) {
            if (nd == dcap) {
                dcap *= 2;
                uint8_t **ndb = (uint8_t **)malloc(dcap * sizeof(uint8_t *));
                uint32_t *ndp = (uint32_t *)malloc(dcap * sizeof(uint32_t));
                if (!ndb || !ndp) { free(ndb); free(ndp); goto out; }
                memcpy(ndb, db, nd * sizeof(uint8_t *));
                memcpy(ndp, dpos, nd * sizeof(uint32_t));
                free(db); free(dpos);
                db = ndb; dpos = ndp;
            }
            db[nd] = (uint8_t *)malloc(bs);
            if (!db[nd]) goto out;
            memset(db[nd], 0, bs);
            dpos[nd] = 64;
            nd++;
        }
        uint8_t *b = db[nd - 1];
        uint32_t o = dpos[nd - 1];
        put64(b + o, ino);
        b[o + 8] = nlen;
        memcpy(b + o + 9, name, nlen);
        if (fs->ftype) b[o + 9 + nlen] = ftype;
        put16(b + o + esz - 2, (uint16_t)o);
        uint32_t idx = (uint32_t)(i + 2);
        le[idx].hash = xfs_hashname((const uint8_t *)name, nlen);
        le[idx].addr = (uint32_t)(((uint64_t)(nd - 1) * bs + o) >> 3);
        dpos[nd - 1] = o + esz;
    }
    uint32_t lcap = (bs - 64) / 8;
    for (uint32_t i = 1; i < total; i++) {                                  /* sort by hash, then address */
        leafent_t t = le[i];
        uint32_t j = i;
        while (j > 0 && (le[j - 1].hash > t.hash || (le[j - 1].hash == t.hash && le[j - 1].addr > t.addr))) { le[j] = le[j - 1]; j--; }
        le[j] = t;
    }

    /* leaf blocks hold runs of the hash order; equal hash values must not straddle two leaves */
    lcnt = (uint32_t *)malloc((size_t)lcap * sizeof(uint32_t));
    if (!lcnt) goto out;
    uint32_t nleaf = 0, leaf1 = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t cap = attempt == 0 ? lcap : (total + 1) / 2;
        nleaf = 0;
        for (uint32_t first = 0; first < total;) {
            uint32_t cnt = total - first < cap ? total - first : cap;
            while (first + cnt < total && cnt > 1 && le[first + cnt].hash == le[first + cnt - 1].hash) cnt--;
            if (nleaf >= lcap) { rc = 1; goto out; }
            lcnt[nleaf++] = cnt;
            first += cnt;
        }
        /* a single leaf block is the "leaf" directory form: entries and the data blocks' free list in one block */
        if (nleaf == 1 && 64 + total * 8 + nd * 2 + 4 <= bs) { leaf1 = 1; break; }
        if (nleaf >= 2) break;
    }
    if (nleaf == 1 && !leaf1) { rc = 1; goto out; }
    if (nd > (bs - 64) / 2) { rc = 1; goto out; }

    /* logical blocks: data, then the node and its leaves, then the free index */
    uint32_t nneed = leaf1 ? nd + 1 : nd + 1 + nleaf + 1;
    need = (uint64_t *)malloc(nneed * sizeof(uint64_t));
    phys = (uint64_t *)malloc(nneed * sizeof(uint64_t));
    if (!need || !phys) goto out;
    for (uint32_t i = 0; i < nd; i++) need[i] = i;
    if (leaf1) need[nd] = leaf_dablk;
    else {
        for (uint32_t i = 0; i < 1 + nleaf; i++) need[nd + i] = leaf_dablk + i;
        need[nd + 1 + nleaf] = free_dablk;
    }
    int mrc = dir_remap(fs, dino, dir, need, (int)nneed, phys);
    if (mrc != 0) { rc = mrc; goto out; }

    buf = (uint8_t *)malloc(bs);
    if (!buf) goto out;
    /* data blocks: header, free tail, checksum */
    for (uint32_t i = 0; i < nd; i++) {
        uint8_t *b = db[i];
        dir_hdr3(fs, b, XFS_DIR3_DATA_MAGIC, fsb_to_byte(fs, phys[i]), dino);
        uint32_t o = dpos[i];
        if (o < bs) {
            put16(b + o, 0xFFFF);
            put16(b + o + 2, (uint16_t)(bs - o));
            put16(b + bs - 2, (uint16_t)o);
            put16(b + 48, (uint16_t)o);
            put16(b + 50, (uint16_t)(bs - o));
        }
        xfs_cksum_update(b, bs, 4);
        if (xfs_wr(fs, fsb_to_byte(fs, phys[i]), bs, b) != 0) goto out;
    }
    if (leaf1) {
        memset(buf, 0, bs);
        da3_hdr(fs, buf, 0x3df1, 0, 0, fsb_to_byte(fs, phys[nd]), dino);       /* XFS_DIR3_LEAF1_MAGIC */
        put16(buf + 56, (uint16_t)total);
        put16(buf + 58, 0);
        for (uint32_t k = 0; k < total; k++) {
            put32(buf + 64 + k * 8, le[k].hash);
            put32(buf + 64 + k * 8 + 4, le[k].addr);
        }
        for (uint32_t i = 0; i < nd; i++) put16(buf + bs - 4 - nd * 2 + i * 2, dpos[i] < bs ? (uint16_t)(bs - dpos[i]) : 0);
        put32(buf + bs - 4, nd);
        xfs_cksum_update(buf, bs, 12);
        if (xfs_wr(fs, fsb_to_byte(fs, phys[nd]), bs, buf) != 0) goto out;
    } else {
    /* leaf blocks */
    uint32_t first = 0;
    for (uint32_t li = 0; li < nleaf; li++) {
        uint32_t cnt = lcnt[li];
        memset(buf, 0, bs);
        da3_hdr(fs, buf, XFS_DIR3_LEAFN_MAGIC, li + 1 < nleaf ? (uint32_t)(leaf_dablk + 1 + li + 1) : 0,
                li > 0 ? (uint32_t)(leaf_dablk + 1 + li - 1) : 0, fsb_to_byte(fs, phys[nd + 1 + li]), dino);
        put16(buf + 56, (uint16_t)cnt);
        put16(buf + 58, 0);
        for (uint32_t k = 0; k < cnt; k++) {
            put32(buf + 64 + k * 8, le[first + k].hash);
            put32(buf + 64 + k * 8 + 4, le[first + k].addr);
        }
        xfs_cksum_update(buf, bs, 12);
        if (xfs_wr(fs, fsb_to_byte(fs, phys[nd + 1 + li]), bs, buf) != 0) goto out;
        first += cnt;
    }
    /* the node block above the leaves */
    memset(buf, 0, bs);
    da3_hdr(fs, buf, XFS_DA3_NODE_MAGIC, 0, 0, fsb_to_byte(fs, phys[nd]), dino);
    put16(buf + 56, (uint16_t)nleaf);
    put16(buf + 58, 1);
    first = 0;
    for (uint32_t li = 0; li < nleaf; li++) {
        uint32_t cnt = lcnt[li];
        put32(buf + 64 + li * 8, le[first + cnt - 1].hash);
        put32(buf + 64 + li * 8 + 4, (uint32_t)(leaf_dablk + 1 + li));
        first += cnt;
    }
    xfs_cksum_update(buf, bs, 12);
    if (xfs_wr(fs, fsb_to_byte(fs, phys[nd]), bs, buf) != 0) goto out;
    /* free-space index: the longest free run of every data block */
    memset(buf, 0, bs);
    dir_hdr3(fs, buf, XFS_DIR3_FREE_MAGIC, fsb_to_byte(fs, phys[nd + 1 + nleaf]), dino);
    put32(buf + 48, 0);
    put32(buf + 52, nd);
    put32(buf + 56, nd);
    for (uint32_t i = 0; i < nd; i++) put16(buf + 64 + i * 2, dpos[i] < bs ? (uint16_t)(bs - dpos[i]) : 0);
    xfs_cksum_update(buf, bs, 4);
    if (xfs_wr(fs, fsb_to_byte(fs, phys[nd + 1 + nleaf]), bs, buf) != 0) goto out;

    }
    put64(dir->raw + 56, (uint64_t)nd * bs);
    dir->size = (uint64_t)nd * bs;
    rc = write_inode(fs, dino, dir) == 0 ? 0 : -1;
out:
    if (db) for (uint32_t i = 0; i < nd; i++) free(db[i]);
    free(db); free(dpos); free(le); free(need); free(phys); free(buf); free(lcnt);
    return rc;
}

/* Writes the directory back. A short-form directory that has outgrown its inode
 * becomes a one-block directory, and a block that is full becomes a multi-block
 * one; a directory that fits in its inode again goes back to short form.
 * Returns 1 when it does not fit anywhere, -2 for unsupported layouts. */
static int dir_store(xfs_t *fs, uint64_t dino, xfs_inode_t *dir, const xfs_dir_t *d)
{
    uint32_t bs = fs->blocksize;
    int i8;
    uint32_t sz = sf_size(fs, d, &i8);
    int fits_sf = d->n <= 255 && (int)sz <= dir->fork_len;
    uint8_t *fork = dir->raw + dir->core_len;

    if (fits_sf) {
        if (dir->format != XFS_DINODE_FMT_LOCAL) {
            /* give the directory blocks back and become short-form */
            uint64_t none[1] = { 0 };
            int mrc = dir_remap(fs, dino, dir, none, 0, none);
            if (mrc != 0) return mrc;
            dir->raw[5] = XFS_DINODE_FMT_LOCAL;
            dir->format = XFS_DINODE_FMT_LOCAL;
            put32(dir->raw + 76, 0);
            dir->nextents = 0;
            put64(dir->raw + 64, 0);
        }
        memset(fork, 0, (size_t)dir->fork_len);
        sf_encode(fs, d, i8, fork);
        put64(dir->raw + 56, sz);
        dir->size = sz;
        return write_inode(fs, dino, dir) == 0 ? 0 : -1;
    }
    if (fs->dirblklog != 0 || !fs->v5) return -2;

    /* one block, when everything fits and the directory is not already in multi-block form */
    if (dir->format == XFS_DINODE_FMT_LOCAL || dir->size == bs) {
        uint8_t *blk = (uint8_t *)malloc(bs);
        if (!blk) return -1;
        int brc = dir_block_build(fs, dino, 0, d, blk);
        if (brc < 0) { free(blk); return -1; }
        if (brc == 0) {
            uint64_t need0 = 0, phys0;
            int mrc = dir_remap(fs, dino, dir, &need0, 1, &phys0);
            if (mrc != 0) { free(blk); return mrc; }
            brc = dir_block_build(fs, dino, fsb_to_byte(fs, phys0), d, blk);
            int wrc = brc == 0 ? xfs_wr(fs, fsb_to_byte(fs, phys0), bs, blk) : -1;
            free(blk);
            if (wrc != 0) return -1;
            put64(dir->raw + 56, bs);
            dir->size = bs;
            return write_inode(fs, dino, dir) == 0 ? 0 : -1;
        }
        free(blk);
    }
    return dir_multi_store(fs, dino, dir, d);
}

/* ---------- walking ---------- */

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
            if (xfs_rd(fs, fsb_to_byte(fs, e->startblock + b), dirblk, blk) != 0) { rc = -1; break; }
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
        dir_iter(fs, &d, lookup_cb, &lc);
        inode_free(&d);
        if (!lc.found) return -1;
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

/* ---------- creating and removing inodes and names ---------- */

static int split_path(const char *path, char *parent, size_t psz, char *name, size_t nsz)
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

static int resolve_parent(xfs_t *fs, const char *path, uint64_t *parent, char *name, size_t nsz)
{
    char pp[512];
    if (split_path(path, pp, sizeof(pp), name, nsz) != 0) return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;
    if (walk(fs, pp, parent) != 0) return -1;
    return 0;
}

static void inode_set_nlink(xfs_inode_t *ip, uint32_t n) { put32(ip->raw + 16, n); }
static uint32_t inode_nlink(const xfs_inode_t *ip) { return be32(ip->raw + 16); }

/* allocates and writes a fresh inode (regular file or empty directory) */
static int new_inode(xfs_t *fs, uint64_t parent, uint16_t mode, uint64_t *out)
{
    uint64_t ino;
    int rc = xfs_ialloc(fs, inode_pref_ag(fs, parent), &ino);
    if (rc != 0) return rc < 0 ? -1 : 1;
    uint64_t off;
    uint8_t *raw = (uint8_t *)malloc(fs->inodesize);
    if (!raw || inode_location(fs, ino, &off) != 0) { free(raw); return -1; }
    uint32_t gen = 1;
    if (xfs_rd(fs, off, fs->inodesize, raw) == 0 && be16(raw) == XFS_DINODE_MAGIC) gen = be32(raw + 92) + 1;
    inode_init_free(fs, raw, ino, gen);
    put16(raw + 2, mode);
    put32(raw + 16, (mode & S_IFMT_) == S_IFDIR_ ? 2 : 1);
    put64(raw + 104, 1);                                 /* change count */
    if ((mode & S_IFMT_) == S_IFDIR_) {
        raw[5] = XFS_DINODE_FMT_LOCAL;
        raw[176] = 0;                                    /* no entries */
        raw[177] = 0;
        put32(raw + 178, (uint32_t)parent);
        put64(raw + 56, 6);
    }
    xfs_cksum_update(raw, fs->inodesize, 100);
    rc = xfs_wr(fs, off, fs->inodesize, raw);
    free(raw);
    if (rc != 0) return -1;
    *out = ino;
    return 0;
}

/* add `name` -> ino to a directory */
static int dir_add(xfs_t *fs, uint64_t dino, const char *name, uint64_t ino, uint16_t mode)
{
    int nlen = (int)strlen(name);
    xfs_inode_t dir;
    if (read_inode(fs, dino, &dir) != 0) return -1;
    xfs_dir_t d;
    int rc = dir_load(fs, &dir, &d);
    if (rc != 0) { inode_free(&dir); return rc == -2 ? -2 : -1; }
    if (dir_find(&d, name, nlen) >= 0) { dir_free(&d); inode_free(&dir); return -1; }
    rc = dir_push(&d, ino, ftype_of_mode(mode), (const uint8_t *)name, nlen);
    if (rc == 0) rc = dir_store(fs, dino, &dir, &d);
    dir_free(&d);
    inode_free(&dir);
    return rc;
}

static int dir_remove_name(xfs_t *fs, uint64_t dino, const char *name, uint64_t *child)
{
    int nlen = (int)strlen(name);
    xfs_inode_t dir;
    if (read_inode(fs, dino, &dir) != 0) return -1;
    xfs_dir_t d;
    int rc = dir_load(fs, &dir, &d);
    if (rc != 0) { inode_free(&dir); return -1; }
    int i = dir_find(&d, name, nlen);
    if (i < 0) { dir_free(&d); inode_free(&dir); return -1; }
    *child = d.e[i].ino;
    memmove(&d.e[i], &d.e[i + 1], (size_t)(d.n - i - 1) * sizeof(xfs_dent_t));
    d.n--;
    rc = dir_store(fs, dino, &dir, &d);
    dir_free(&d);
    inode_free(&dir);
    return rc;
}

/* rewrites the ".." of a directory (it moved) */
static int dir_set_parent(xfs_t *fs, uint64_t dino, uint64_t parent)
{
    xfs_inode_t dir;
    if (read_inode(fs, dino, &dir) != 0) return -1;
    xfs_dir_t d;
    int rc = dir_load(fs, &dir, &d);
    if (rc == 0) {
        d.parent = parent;
        rc = dir_store(fs, dino, &dir, &d);
        dir_free(&d);
    } else rc = -1;
    inode_free(&dir);
    return rc;
}

static int adjust_nlink(xfs_t *fs, uint64_t ino, int delta)
{
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    uint32_t n = inode_nlink(&ip);
    if (delta < 0 && n < (uint32_t)-delta) n = 0; else n = (uint32_t)((int32_t)n + delta);
    inode_set_nlink(&ip, n);
    int rc = write_inode(fs, ino, &ip);
    inode_free(&ip);
    return rc;
}

/* drops an inode and its blocks (its last name is gone) */
static int release_inode(xfs_t *fs, uint64_t ino)
{
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    int rc = xfs_file_free_blocks(fs, ino, &ip);
    if (rc == 0) rc = xfs_attr_free(fs, &ip);
    if (rc == 0) {
        uint32_t gen = be32(ip.raw + 92) + 1;
        uint8_t *raw = (uint8_t *)malloc(fs->inodesize);
        if (!raw) { inode_free(&ip); return -1; }
        inode_init_free(fs, raw, ino, gen);
        xfs_cksum_update(raw, fs->inodesize, 100);
        uint64_t off;
        rc = inode_location(fs, ino, &off) == 0 ? xfs_wr(fs, off, fs->inodesize, raw) : -1;
        free(raw);
        if (rc == 0) rc = xfs_ifree(fs, ino);
    }
    inode_free(&ip);
    return rc;
}

static int dir_is_empty_inode(xfs_t *fs, uint64_t ino)
{
    xfs_inode_t dir;
    if (read_inode(fs, ino, &dir) != 0) return 0;
    xfs_dir_t d;
    int rc = dir_load(fs, &dir, &d);
    inode_free(&dir);
    if (rc != 0) return 0;
    int empty = d.n == 0;
    dir_free(&d);
    return empty;
}

/* ---------- VFS ---------- */

#define XFS_WRITE_FLAGS (VFS_WRONLY | VFS_RDWR | VFS_CREAT | VFS_TRUNC | VFS_APPEND)

static int xfs_vfs_open(void *ctx, const char *path, int flags)
{
    xfs_t *fs = (xfs_t *)ctx;
    int wr = (flags & XFS_WRITE_FLAGS) != 0;
    if (wr && !fs->rw) return -1;
    uint64_t ino;
    if (walk(fs, path, &ino) != 0) {
        if (!(flags & VFS_CREAT)) return -1;
        uint64_t parent;
        char name[XFS_MAX_FILENAME + 1];
        if (resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
        uint16_t mode = S_IFREG_ | 0644;
        if (new_inode(fs, parent, mode, &ino) != 0) return -1;
        if (dir_add(fs, parent, name, ino, mode) != 0) { release_inode(fs, ino); return -1; }
    }
    xfs_inode_t ip;
    if (read_inode(fs, ino, &ip) != 0) return -1;
    int is_dir = (ip.mode & S_IFMT_) == S_IFDIR_;
    if (wr && is_dir) { inode_free(&ip); return -1; }
    if ((flags & VFS_TRUNC) && !is_dir && ip.size > 0) {
        int rc = xfs_file_free_blocks(fs, ino, &ip);
        if (rc == 0) { put64(ip.raw + 56, 0); ip.size = 0; rc = write_inode(fs, ino, &ip); }
        if (rc != 0) { inode_free(&ip); return -1; }
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].size = ip.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)ip.size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? fs->fds[i].size : 0;
            fs->fds[i].is_dir = is_dir;
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
    xfs_t *fs = (xfs_t *)ctx;
    if (!fs->rw || fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    if (size == 0) return 0;
    xfs_fd_t *f = &fs->fds[fd];
    if (xfs_file_write(fs, f->ino, f->pos, (const uint8_t *)buf, size) != 0) return -1;
    f->pos += size;
    if (f->pos > f->size) f->size = f->pos;
    return (int)size;
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
    (void)mode;
    xfs_t *fs = (xfs_t *)ctx;
    if (!fs->rw) return -1;
    uint64_t parent, ino;
    char name[XFS_MAX_FILENAME + 1];
    if (resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
    xfs_inode_t pi;
    if (read_inode(fs, parent, &pi) != 0) return -1;
    xfs_dir_t d;
    int rc = dir_load(fs, &pi, &d);
    inode_free(&pi);
    if (rc != 0) return -1;
    int exists = dir_find(&d, name, (int)strlen(name)) >= 0;
    dir_free(&d);
    if (exists) return -1;
    uint16_t m = S_IFDIR_ | 0755;
    if (new_inode(fs, parent, m, &ino) != 0) return -1;
    if (dir_add(fs, parent, name, ino, m) != 0) { release_inode(fs, ino); return -1; }
    return adjust_nlink(fs, parent, +1);
}

/* removes name from parent; the inode goes with its last link */
static int remove_entry(xfs_t *fs, uint64_t parent, const char *name)
{
    int nlen = (int)strlen(name);
    xfs_inode_t pi;
    if (read_inode(fs, parent, &pi) != 0) return -1;
    xfs_dir_t d;
    int rc = dir_load(fs, &pi, &d);
    inode_free(&pi);
    if (rc != 0) return -1;
    int i = dir_find(&d, name, nlen);
    uint64_t child = i >= 0 ? d.e[i].ino : 0;
    dir_free(&d);
    if (i < 0) return -1;
    xfs_inode_t ci;
    if (read_inode(fs, child, &ci) != 0) return -1;
    int is_dir = (ci.mode & S_IFMT_) == S_IFDIR_;
    uint32_t nlink = inode_nlink(&ci);
    inode_free(&ci);
    if (is_dir && !dir_is_empty_inode(fs, child)) return -1;
    if (is_dir || nlink <= 1) {
        /* give the blocks back first: if that cannot be done the file stays as it was */
        xfs_inode_t fi;
        if (read_inode(fs, child, &fi) != 0) return -1;
        int frc = xfs_file_free_blocks(fs, child, &fi);
        if (frc == 0) { put64(fi.raw + 56, 0); fi.size = 0; frc = write_inode(fs, child, &fi); }
        inode_free(&fi);
        if (frc != 0) return -1;
    }
    uint64_t removed;
    if (dir_remove_name(fs, parent, name, &removed) != 0) return -1;
    if (is_dir) {
        if (adjust_nlink(fs, parent, -1) != 0) return -1;
        return release_inode(fs, child);
    }
    if (nlink > 1) return adjust_nlink(fs, child, -1);
    return release_inode(fs, child);
}

static int xfs_vfs_unlink(void *ctx, const char *path)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (!fs->rw) return -1;
    uint64_t parent;
    char name[XFS_MAX_FILENAME + 1];
    if (resolve_parent(fs, path, &parent, name, sizeof(name)) != 0) return -1;
    return remove_entry(fs, parent, name);
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

/* is `anc` the directory `dir` or above it? (walks the ".." chain) */
static int dir_is_under(xfs_t *fs, uint64_t anc, uint64_t dir)
{
    uint64_t cur = dir;
    for (int guard = 0; guard < 4096; guard++) {
        if (cur == anc) return 1;
        if (cur == fs->rootino) return 0;
        xfs_inode_t ip;
        if (read_inode(fs, cur, &ip) != 0) return 1;
        xfs_dir_t d;
        int rc = dir_load(fs, &ip, &d);
        inode_free(&ip);
        if (rc != 0) return 1;
        uint64_t parent = d.parent;
        dir_free(&d);
        cur = parent;
    }
    return 1;
}

static int xfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    xfs_t *fs = (xfs_t *)ctx;
    if (!fs->rw) return -1;
    uint64_t sp, dp;
    char sname[XFS_MAX_FILENAME + 1], dname[XFS_MAX_FILENAME + 1];
    if (resolve_parent(fs, old, &sp, sname, sizeof(sname)) != 0) return -1;
    if (resolve_parent(fs, new_path, &dp, dname, sizeof(dname)) != 0) return -1;
    if (sp == dp && strcmp(sname, dname) == 0) return 0;

    xfs_inode_t pi;
    xfs_dir_t sd, dd;
    if (read_inode(fs, sp, &pi) != 0) return -1;
    int rc = dir_load(fs, &pi, &sd);
    inode_free(&pi);
    if (rc != 0) return -1;
    int si = dir_find(&sd, sname, (int)strlen(sname));
    uint64_t child = si >= 0 ? sd.e[si].ino : 0;
    dir_free(&sd);
    if (si < 0) return -1;
    xfs_inode_t ci;
    if (read_inode(fs, child, &ci) != 0) return -1;
    uint16_t cmode = ci.mode;
    int is_dir = (cmode & S_IFMT_) == S_IFDIR_;
    inode_free(&ci);
    if (is_dir && dir_is_under(fs, child, dp)) return -1;

    if (read_inode(fs, dp, &pi) != 0) return -1;
    rc = dir_load(fs, &pi, &dd);
    inode_free(&pi);
    if (rc != 0) return -1;
    int di = dir_find(&dd, dname, (int)strlen(dname));
    uint64_t victim = di >= 0 ? dd.e[di].ino : 0;
    dir_free(&dd);
    if (di >= 0) {
        if (victim == child) return 0;
        xfs_inode_t vi;
        if (read_inode(fs, victim, &vi) != 0) return -1;
        int vdir = (vi.mode & S_IFMT_) == S_IFDIR_;
        inode_free(&vi);
        if (vdir != is_dir) return -1;
        if (remove_entry(fs, dp, dname) != 0) return -1;
    }
    if (dir_add(fs, dp, dname, child, cmode) != 0) return -1;
    uint64_t removed;
    if (dir_remove_name(fs, sp, sname, &removed) != 0) return -1;
    if (is_dir && sp != dp) {
        if (dir_set_parent(fs, child, dp) != 0) return -1;
        if (adjust_nlink(fs, sp, -1) != 0 || adjust_nlink(fs, dp, +1) != 0) return -1;
    }
    return 0;
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
    klog_write(fs->rw ? "xfs: mounted read-write\n" : "xfs: mounted read-only\n");
    vfs_mount(mount_point, &ops, fs);
}

/* ---------- mount ---------- */

/* The log is clean when its last record is an unmount record: nothing is left to replay. */
static int xfs_log_clean(xfs_t *fs)
{
    uint64_t base = fsb_to_byte(fs, fs->logstart);
    uint64_t nbb = (uint64_t)fs->logblocks * fs->blocksize / 512;
    uint8_t h[512], d[512];
    if (xfs_rd(fs, base, 512, h) != 0 || be32(h) != 0xFEEDBABEu) return 0;
    uint32_t c0 = be32(h + 4);
    uint64_t b = 0, last = 0;
    int have_last = 0;
    uint32_t last_ops = 0;
    for (int guard = 0; guard < 4000000; guard++) {
        if (b >= nbb) break;
        if (xfs_rd(fs, base + b * 512, 512, h) != 0) return 0;
        if (be32(h) != 0xFEEDBABEu || be32(h + 4) != c0) break;
        uint32_t len = be32(h + 12);
        if (len > 32768) return 0;                       /* extended record headers: do not guess */
        last = b;
        have_last = 1;
        last_ops = be32(h + 40);
        b += 1 + (len + 511) / 512;
    }
    if (!have_last || last_ops != 1) return 0;
    if (xfs_rd(fs, base + (last + 1) * 512, 512, d) != 0) return 0;
    return (d[9] & 0x20) != 0;                           /* XLOG_UNMOUNT_TRANS */
}

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
    fs->logstart = be64(sb + 48);
    fs->logblocks = be32(sb + 96);
    fs->agblocks = be32(sb + 84);
    fs->agcount = be32(sb + 88);
    uint16_t versionnum = be16(sb + 100);
    fs->sectsize = be16(sb + 102);
    fs->inodesize = be16(sb + 104);
    fs->inopblock = be16(sb + 106);
    memcpy(fs->fname, sb + 108, 12);
    fs->fname[12] = 0;
    fs->agblklog = sb[124];
    fs->inopblog = sb[123];
    fs->imax_pct = sb[127];
    fs->inoalign = be32(sb + 180);
    fs->dirblklog = sb[192];
    fs->v5 = (versionnum & 0xF) == 5;
    uint32_t features2 = be32(sb + 200);
    uint32_t ro_compat = fs->v5 ? be32(sb + 212) : 0;
    uint32_t incompat = fs->v5 ? be32(sb + 216) : 0;
    uint32_t log_incompat = fs->v5 ? be32(sb + 220) : 0;
    uint32_t rextents = (uint32_t)be64(sb + 24);
    int inprogress = sb[126];
    uint64_t qino[3] = { be64(sb + 160), be64(sb + 168), fs->v5 ? be64(sb + 232) : 0 };
    uint64_t quota = 0;
    for (int q = 0; q < 3; q++) if (qino[q] != 0 && qino[q] != ~0ULL) quota = 1;     /* NULLFSINO means "none" */
    uint16_t qflags = be16(sb + 176);
    if (fs->v5) {
        fs->ftype = (incompat & XFS_INCOMPAT_FTYPE) != 0;
        fs->spinodes = (incompat & XFS_INCOMPAT_SPINODES) != 0;
        fs->finobt = (ro_compat & XFS_RO_COMPAT_FINOBT) != 0;
        if (incompat & XFS_INCOMPAT_META_UUID) memcpy(fs->meta_uuid, sb + 248, 16);
        else memcpy(fs->meta_uuid, sb + 32, 16);
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
    if (!ok) return -1;

    /* writable only for a quiet, simple v5 volume */
    int rw = fs->v5 && fs->sectsize == 512 && !inprogress && !quota && !qflags && log_incompat == 0 &&
             !(ro_compat & XFS_RO_COMPAT_RMAPBT) && fs->logstart != 0 && fs->logblocks != 0 &&
             fs->inodesize >= 256 && fs->inopblock == (fs->blocksize / fs->inodesize);
    if (rw && (ro_compat & ~(XFS_RO_COMPAT_FINOBT | XFS_RO_COMPAT_REFLINK | XFS_RO_COMPAT_INOBTCNT))) rw = 0;
    if (rw && !xfs_log_clean(fs)) rw = 0;
    for (uint32_t ag = 0; rw && ag < fs->agcount; ag++) {
        uint8_t agf[512], agi[512];
        if (ag_hdr_read(fs, ag, XFS_AGF_SECTOR, agf) != 0 || ag_hdr_read(fs, ag, XFS_AGI_SECTOR, agi) != 0 ||
            be32(agf) != XFS_AGF_MAGIC || be32(agi) != XFS_AGI_MAGIC) { rw = 0; break; }
        if (be32(agf + 28) != 1 || be32(agf + 32) != 1 || be32(agi + 24) != 1) rw = 0;
        if (fs->finobt && be32(agi + 332) != 1) rw = 0;
        if ((ro_compat & XFS_RO_COMPAT_REFLINK) && be32(agf + 84) > 1) rw = 0;     /* shared extents exist */
        if (rw && (ro_compat & XFS_RO_COMPAT_REFLINK) && be32(agf + 84) == 1) {
            /* an empty refcount tree is fine */
            uint8_t *rb = (uint8_t *)malloc(fs->blocksize);
            if (!rb || xfs_rd(fs, agbno_to_byte(fs, ag, be32(agf + 88)), fs->blocksize, rb) != 0 || be16(rb + 6) != 0) rw = 0;
            free(rb);
        }
    }
    fs->rw = rw;
    return 0;
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
