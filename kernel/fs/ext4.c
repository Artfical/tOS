#include "ext4.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "crc32c.h"

#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_GOOD_OLD_REV 0
#define EXT4_DYNAMIC_REV  1
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x2
#define EXT4_FEATURE_INCOMPAT_EXTENTS  0x40

#define EXT4_S_IFREG 0x8000
#define EXT4_S_IFDIR 0x4000

#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2

#define EXT4_ROOT_INO 2
#define EXT4_FIRST_NON_RESERVED_INO 11

#define EXT4_EXTENTS_FL 0x80000
#define EXT4_INDEX_FL 0x1000
#define EXT4_HUGE_FILE_FL 0x40000

#define EXT4_BG_INODE_UNINIT 0x1
#define EXT4_BG_BLOCK_UNINIT 0x2
#define EXT4_BG_INODE_ZEROED 0x4

#define EXT4_DIRENT_TAIL_FT 0xDE

#define EXT4_JOURNAL_MAGIC 0x4A344653u
#define EXT4_JBLOCK_DESCRIPTOR 1u
#define EXT4_JBLOCK_COMMIT 2u
#define EXT4_MAX_RECOVER_TXN 32

#define EXT4_EXTENT_MAGIC 0xF30A
#define EXT4_ROOT_MAX_ENTRIES 4

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint32_t s_journal_first_block;
    uint32_t s_journal_blocks;
    uint8_t  s_padding[812];
} __attribute__((packed)) ext4_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext4_group_desc_t;

/* full 64-byte group descriptor; a 32-byte one only fills the first half */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) ext4_gd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  osd2[12];
} __attribute__((packed)) ext4_inode_t;

typedef struct {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed)) ext4_dirent_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t maxlen;
    uint32_t s_committed_seq;
} __attribute__((packed)) ext4_journal_super_t;

typedef struct {
    uint32_t magic;
    uint32_t block_type;
    uint32_t sequence;
    uint32_t num_blocks;
} __attribute__((packed)) ext4_journal_hdr_t;

/* extent tree on-disk structures (stored inline in inode.i_block, or in dedicated leaf blocks) */
typedef struct {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

/* ---------- raw (uncached) block I/O, used for journal and checkpointing ---------- */

static int ext4_raw_read_block(ext4_t *fs, uint32_t block, void *buf)
{
    if (block == 0) return -1;
    return blockdev_read_bytes(fs->bd, (uint64_t)block * fs->block_size, fs->block_size, buf);
}

static int ext4_raw_write_block(ext4_t *fs, uint32_t block, const void *buf)
{
    if (block == 0) return -1;
    return blockdev_write_bytes(fs->bd, (uint64_t)block * fs->block_size, fs->block_size, buf);
}

/* ---------- transaction / cache layer ---------- */

static int ext4_txn_find(ext4_t *fs, uint32_t block)
{
    for (int i = 0; i < fs->txn_count; i++) {
        if (fs->txn[i].block == block) return i;
    }
    return -1;
}

static void ext4_txn_begin(ext4_t *fs)
{
    fs->in_txn = fs->use_journal ? 1 : 0;
    fs->txn_count = 0;
}

static void ext4_journal_commit_batch(ext4_t *fs)
{
    if (fs->txn_count == 0) return;

    uint32_t usable = fs->journal_blocks - 1; /* blocks 1..journal_blocks-1 */
    uint32_t need = 2 + (uint32_t)fs->txn_count; /* descriptor + data + commit */
    if (need > usable) need = usable; /* should never happen given EXT4_MAX_TXN_BLOCKS sizing */

    uint32_t cursor = fs->journal_cursor;
    if (cursor < 1 || cursor + need > fs->journal_blocks) cursor = 1;

    uint8_t *desc = (uint8_t *)malloc(fs->block_size);
    memset(desc, 0, fs->block_size);
    ext4_journal_hdr_t *dh = (ext4_journal_hdr_t *)desc;
    dh->magic = EXT4_JOURNAL_MAGIC;
    dh->block_type = EXT4_JBLOCK_DESCRIPTOR;
    dh->sequence = fs->journal_sequence;
    dh->num_blocks = (uint32_t)fs->txn_count;
    uint32_t *targets = (uint32_t *)(desc + sizeof(ext4_journal_hdr_t));
    for (int i = 0; i < fs->txn_count; i++) targets[i] = fs->txn[i].block;
    ext4_raw_write_block(fs, fs->journal_first_block + cursor, desc);
    free(desc);

    for (int i = 0; i < fs->txn_count; i++) {
        ext4_raw_write_block(fs, fs->journal_first_block + cursor + 1 + (uint32_t)i, fs->txn[i].data);
    }

    uint8_t *commit = (uint8_t *)malloc(fs->block_size);
    memset(commit, 0, fs->block_size);
    ext4_journal_hdr_t *ch = (ext4_journal_hdr_t *)commit;
    ch->magic = EXT4_JOURNAL_MAGIC;
    ch->block_type = EXT4_JBLOCK_COMMIT;
    ch->sequence = fs->journal_sequence;
    ext4_raw_write_block(fs, fs->journal_first_block + cursor + 1 + (uint32_t)fs->txn_count, commit);
    free(commit);

    /* checkpoint: apply to real locations */
    for (int i = 0; i < fs->txn_count; i++) {
        ext4_raw_write_block(fs, fs->txn[i].block, fs->txn[i].data);
        free(fs->txn[i].data);
        fs->txn[i].data = 0;
    }

    fs->journal_sequence++;
    fs->journal_cursor = cursor + need;
    if (fs->journal_cursor >= fs->journal_blocks) fs->journal_cursor = 1;

    /* persist new baseline so recovery never replays a checkpointed txn from an old wrap */
    uint8_t *jsb_buf = (uint8_t *)malloc(fs->block_size);
    if (ext4_raw_read_block(fs, fs->journal_first_block, jsb_buf) == 0) {
        ext4_journal_super_t *jsb = (ext4_journal_super_t *)jsb_buf;
        jsb->s_committed_seq = fs->journal_sequence;
        ext4_raw_write_block(fs, fs->journal_first_block, jsb_buf);
    }
    free(jsb_buf);

    fs->txn_count = 0;
}

static void ext4_txn_flush(ext4_t *fs)
{
    ext4_journal_commit_batch(fs);
}

static void ext4_txn_commit(ext4_t *fs)
{
    ext4_txn_flush(fs);
    fs->in_txn = 0;
}

static int ext4_cached_read_block(ext4_t *fs, uint32_t block, void *buf)
{
    if (block == 0) return -1;
    int idx = fs->in_txn ? ext4_txn_find(fs, block) : -1;
    if (idx >= 0) { memcpy(buf, fs->txn[idx].data, fs->block_size); return 0; }
    return ext4_raw_read_block(fs, block, buf);
}

static int ext4_cached_write_block(ext4_t *fs, uint32_t block, const void *buf)
{
    if (block == 0) return -1;
    if (!fs->in_txn) return ext4_raw_write_block(fs, block, buf);

    int idx = ext4_txn_find(fs, block);
    if (idx >= 0) { memcpy(fs->txn[idx].data, buf, fs->block_size); return 0; }

    if (fs->txn_count >= EXT4_MAX_TXN_BLOCKS) {
        ext4_txn_flush(fs); /* auto-flush, keep in_txn==1 */
    }
    idx = fs->txn_count++;
    fs->txn[idx].block = block;
    fs->txn[idx].data = (uint8_t *)malloc(fs->block_size);
    memcpy(fs->txn[idx].data, buf, fs->block_size);
    return 0;
}

static int ext4_bytes_read_cached(ext4_t *fs, uint64_t off, uint32_t len, void *out)
{
    uint8_t *blk = (uint8_t *)malloc(fs->block_size);
    if (!blk) return -1;
    uint32_t done = 0;
    while (done < len) {
        uint64_t cur = off + done;
        uint32_t block = (uint32_t)(cur / fs->block_size);
        uint32_t in_blk = (uint32_t)(cur % fs->block_size);
        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > len - done) chunk = len - done;
        if (ext4_cached_read_block(fs, block, blk) != 0) { free(blk); return -1; }
        memcpy((uint8_t *)out + done, blk + in_blk, chunk);
        done += chunk;
    }
    free(blk);
    return 0;
}

static int ext4_bytes_write_cached(ext4_t *fs, uint64_t off, uint32_t len, const void *in)
{
    uint8_t *blk = (uint8_t *)malloc(fs->block_size);
    if (!blk) return -1;
    uint32_t done = 0;
    while (done < len) {
        uint64_t cur = off + done;
        uint32_t block = (uint32_t)(cur / fs->block_size);
        uint32_t in_blk = (uint32_t)(cur % fs->block_size);
        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > len - done) chunk = len - done;
        if (chunk < fs->block_size) {
            if (ext4_cached_read_block(fs, block, blk) != 0) { free(blk); return -1; }
        }
        memcpy(blk + in_blk, (const uint8_t *)in + done, chunk);
        if (ext4_cached_write_block(fs, block, blk) != 0) { free(blk); return -1; }
        done += chunk;
    }
    free(blk);
    return 0;
}

static uint32_t ext4_zalloc_block_raw(ext4_t *fs, uint32_t block)
{
    uint8_t *zbuf = (uint8_t *)malloc(fs->block_size);
    if (!zbuf) return 0;
    memset(zbuf, 0, fs->block_size);
    ext4_cached_write_block(fs, block, zbuf);
    free(zbuf);
    return block;
}

/* ---------- little-endian field access and checksums ---------- */

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint16_t ext4_crc16(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint32_t ext4_units(ext4_t *fs, const ext4_inode_t *inode)
{
    return (inode->i_flags & EXT4_HUGE_FILE_FL) ? 1u : fs->block_size / 512;
}

static uint16_t ext4_gd_csum(ext4_t *fs, uint32_t group, const uint8_t *desc)
{
    uint8_t le[4];
    wr32(le, group);
    if (fs->csum_md) {
        static const uint8_t zero2[2] = { 0, 0 };
        uint32_t c = crc32c_update(fs->csum_seed, le, 4);
        c = crc32c_update(c, desc, 0x1E);
        c = crc32c_update(c, zero2, 2);
        if (fs->desc_size > 0x20) c = crc32c_update(c, desc + 0x20, fs->desc_size - 0x20);
        return (uint16_t)(c & 0xFFFF);
    }
    uint16_t c = ext4_crc16(0xFFFF, fs->uuid, 16);
    c = ext4_crc16(c, le, 4);
    c = ext4_crc16(c, desc, 0x1E);
    if (fs->desc_size > 0x20) c = ext4_crc16(c, desc + 0x20, fs->desc_size - 0x20);
    return c;
}

static int ext4_read_group_desc(ext4_t *fs, uint32_t group, ext4_gd_t *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t off = (uint64_t)fs->gdt_block * fs->block_size + (uint64_t)group * fs->desc_size;
    return ext4_bytes_read_cached(fs, off, fs->desc_size, out);
}

static int ext4_write_group_desc(ext4_t *fs, uint32_t group, ext4_gd_t *in)
{
    if (fs->csum_md || fs->csum_gdt) {
        in->bg_checksum = 0;
        in->bg_checksum = ext4_gd_csum(fs, group, (const uint8_t *)in);
    }
    uint64_t off = (uint64_t)fs->gdt_block * fs->block_size + (uint64_t)group * fs->desc_size;
    return ext4_bytes_write_cached(fs, off, fs->desc_size, in);
}

static void ext4_gd_bitmap_csum(ext4_t *fs, ext4_gd_t *gd, int is_inode, const uint8_t *bitmap)
{
    if (!fs->csum_md) return;
    uint32_t len = (is_inode ? fs->inodes_per_group : fs->blocks_per_group) / 8;
    uint32_t c = crc32c_update(fs->csum_seed, bitmap, len);
    if (is_inode) {
        gd->bg_inode_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64) gd->bg_inode_bitmap_csum_hi = (uint16_t)(c >> 16);
    } else {
        gd->bg_block_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64) gd->bg_block_bitmap_csum_hi = (uint16_t)(c >> 16);
    }
}

static uint32_t ext4_iseed(ext4_t *fs, uint32_t ino, const ext4_inode_t *inode)
{
    uint8_t le[4];
    wr32(le, ino);
    uint32_t c = crc32c_update(fs->csum_seed, le, 4);
    wr32(le, inode->i_generation);
    return crc32c_update(c, le, 4);
}

/* inode checksum over the full on-disk inode, its checksum fields read as zero */
static uint32_t ext4_inode_csum(ext4_t *fs, uint32_t ino, const uint8_t *raw, int *has_hi)
{
    static const uint8_t zero2[2] = { 0, 0 };
    uint8_t le[4];
    wr32(le, ino);
    uint32_t c = crc32c_update(fs->csum_seed, le, 4);
    c = crc32c_update(c, raw + 0x64, 4);               /* i_generation */
    c = crc32c_update(c, raw, 0x7C);
    c = crc32c_update(c, zero2, 2);                    /* i_checksum_lo */
    c = crc32c_update(c, raw + 0x7E, 128 - 0x7E);
    int hi = fs->inode_size > 128 && rd16(raw + 0x80) >= 4;
    if (fs->inode_size > 128) {
        c = crc32c_update(c, raw + 128, 0x82 - 128);   /* i_extra_isize, high word of nothing else */
        if (hi) {
            c = crc32c_update(c, zero2, 2);            /* i_checksum_hi */
            c = crc32c_update(c, raw + 0x84, fs->inode_size - 0x84);
        } else {
            c = crc32c_update(c, raw + 0x82, fs->inode_size - 0x82);
        }
    }
    *has_hi = hi;
    return c;
}

static void ext4_inode_seal(ext4_t *fs, uint32_t ino, uint8_t *raw)
{
    int hi;
    uint32_t c = ext4_inode_csum(fs, ino, raw, &hi);
    wr16(raw + 0x7C, (uint16_t)c);
    if (hi) wr16(raw + 0x82, (uint16_t)(c >> 16));
}

static uint64_t ext4_inode_offset(ext4_t *fs, uint32_t ino)
{
    if (ino == 0 || ino > fs->inodes_count) return 0;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    ext4_gd_t gd;
    if (ext4_read_group_desc(fs, group, &gd) != 0) return 0;
    if (gd.bg_inode_table == 0) return 0;
    return (uint64_t)gd.bg_inode_table * fs->block_size + (uint64_t)index * fs->inode_size;
}

static int ext4_read_inode_raw(ext4_t *fs, uint32_t ino, uint8_t *raw128)
{
    uint64_t off = ext4_inode_offset(fs, ino);
    if (off == 0) return -1;
    return ext4_bytes_read_cached(fs, off, 128, raw128);
}

/* Writes the first 128 bytes of the inode; the extra fields (extra_isize,
 * xattrs in the inode body, ...) already on disk are kept and, with
 * metadata_csum, the checksum over the whole inode is refreshed. */
static int ext4_write_inode_raw(ext4_t *fs, uint32_t ino, const uint8_t *raw128)
{
    uint64_t off = ext4_inode_offset(fs, ino);
    if (off == 0) return -1;
    if (!fs->csum_md || fs->inode_size <= 128) {
        if (fs->csum_md) {
            uint8_t tmp[128];
            memcpy(tmp, raw128, 128);
            ext4_inode_seal(fs, ino, tmp);
            return ext4_bytes_write_cached(fs, off, 128, tmp);
        }
        return ext4_bytes_write_cached(fs, off, 128, raw128);
    }
    uint8_t *full = (uint8_t *)malloc(fs->inode_size);
    if (!full) return -1;
    if (ext4_bytes_read_cached(fs, off, fs->inode_size, full) != 0) { free(full); return -1; }
    memcpy(full, raw128, 128);
    ext4_inode_seal(fs, ino, full);
    int rc = ext4_bytes_write_cached(fs, off, fs->inode_size, full);
    free(full);
    return rc;
}

/* A brand new inode: the whole on-disk slot is rewritten (an uninitialised
 * inode table holds garbage past the first 128 bytes). */
static int ext4_write_inode_new(ext4_t *fs, uint32_t ino, ext4_inode_t *inode)
{
    uint64_t off = ext4_inode_offset(fs, ino);
    if (off == 0) return -1;
    inode->i_generation = (fs->gen_counter += 0x9E3779B1u) ^ ino;
    uint8_t *full = (uint8_t *)malloc(fs->inode_size);
    if (!full) return -1;
    memset(full, 0, fs->inode_size);
    memcpy(full, inode, 128);
    if (fs->inode_size > 128) {
        uint32_t extra = fs->inode_size - 128;
        wr16(full + 0x80, (uint16_t)(extra < 32 ? extra : 32));
    }
    if (fs->csum_md) ext4_inode_seal(fs, ino, full);
    int rc = ext4_bytes_write_cached(fs, off, fs->inode_size, full);
    free(full);
    return rc;
}

/* The superblock keeps filesystem-wide free counts next to the per-group
 * ones; keep both in step so fsck does not report them wrong. */
static void ext4_sb_adjust(ext4_t *fs, int dblocks, int dinodes)
{
    /* straight to the device: with 4KB blocks the superblock sits in block
     * 0, which the transaction layer treats as invalid */
    if (!fs->csum_md) {
        uint32_t v[2];
        if (blockdev_read_bytes(fs->bd, 1024 + 12, 8, v) != 0) return;
        v[0] = (uint32_t)((int32_t)v[0] + dblocks);
        v[1] = (uint32_t)((int32_t)v[1] + dinodes);
        blockdev_write_bytes(fs->bd, 1024 + 12, 8, v);
        return;
    }
    uint8_t *sb = (uint8_t *)malloc(1024);
    if (!sb) return;
    if (blockdev_read_bytes(fs->bd, 1024, 1024, sb) == 0) {
        wr32(sb + 12, (uint32_t)((int32_t)rd32(sb + 12) + dblocks));
        wr32(sb + 16, (uint32_t)((int32_t)rd32(sb + 16) + dinodes));
        wr32(sb + 0x3FC, crc32c_update(0xFFFFFFFFu, sb, 0x3FC));
        blockdev_write_bytes(fs->bd, 1024, 1024, sb);
    }
    free(sb);
}

/* ---------- bitmaps ---------- */

static int ext4_group_has_super(ext4_t *fs, uint32_t g)
{
    if (!(fs->feat_ro & 0x1)) return 1;     /* no sparse_super: every group */
    if (g <= 1) return 1;
    static const uint32_t bases[3] = { 3, 5, 7 };
    for (int i = 0; i < 3; i++) {
        uint64_t n = bases[i];
        while (n < g) n *= bases[i];
        if (n == g) return 1;
    }
    return 0;
}

static void ext4_bit_set(uint8_t *bm, uint32_t bit) { bm[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }

/* bits [from, nbits) set, as mke2fs pads the tail of a bitmap block */
static void ext4_bitmap_pad(uint8_t *bm, uint32_t from, uint32_t nbits)
{
    for (uint32_t b = from; b < nbits; b++) {
        if ((b & 7) == 0 && b + 8 <= nbits) { bm[b >> 3] = 0xFF; b += 7; continue; }
        ext4_bit_set(bm, b);
    }
}

/* The in-memory bitmap of a BLOCK_UNINIT group: its own super/GDT copies
 * and its own metadata. */
static void ext4_init_block_bitmap(ext4_t *fs, uint32_t g, const ext4_gd_t *gd, uint8_t *bm)
{
    memset(bm, 0, fs->block_size);
    uint32_t base = fs->first_data_block + g * fs->blocks_per_group;
    uint32_t in_group = fs->blocks_count - base;
    if (in_group > fs->blocks_per_group) in_group = fs->blocks_per_group;
    if (ext4_group_has_super(fs, g)) {
        uint32_t over = 1 + fs->gdt_blocks + fs->reserved_gdt;
        for (uint32_t b = 0; b < over && b < in_group; b++) ext4_bit_set(bm, b);
    }
    uint32_t itb = (fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
    uint32_t own[3] = { gd->bg_block_bitmap, gd->bg_inode_bitmap, gd->bg_inode_table };
    for (int i = 0; i < 3; i++) {
        uint32_t n = i == 2 ? itb : 1;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t b = own[i] + k;
            if (b >= base && b - base < in_group) ext4_bit_set(bm, b - base);
        }
    }
    ext4_bitmap_pad(bm, in_group, fs->block_size * 8);
}

static int ext4_group_flag(ext4_t *fs, const ext4_gd_t *gd, uint16_t flag)
{
    return fs->lazy && (gd->bg_flags & flag);
}

static int ext4_load_block_bitmap(ext4_t *fs, uint32_t g, const ext4_gd_t *gd, uint8_t *bm)
{
    if (ext4_group_flag(fs, gd, EXT4_BG_BLOCK_UNINIT)) { ext4_init_block_bitmap(fs, g, gd, bm); return 0; }
    if (gd->bg_block_bitmap == 0) return -1;
    return ext4_cached_read_block(fs, gd->bg_block_bitmap, bm);
}

static int ext4_load_inode_bitmap(ext4_t *fs, const ext4_gd_t *gd, uint8_t *bm)
{
    if (ext4_group_flag(fs, gd, EXT4_BG_INODE_UNINIT)) {
        memset(bm, 0, fs->block_size);
        ext4_bitmap_pad(bm, fs->inodes_per_group, fs->block_size * 8);
        return 0;
    }
    if (gd->bg_inode_bitmap == 0) return -1;
    return ext4_cached_read_block(fs, gd->bg_inode_bitmap, bm);
}

/* index of the first clear bit in [from, nbits), or nbits */
static uint32_t ext4_bm_next_free(const uint8_t *bm, uint32_t from, uint32_t nbits)
{
    uint32_t bit = from;
    while (bit < nbits) {
        if ((bit & 7) == 0 && bm[bit >> 3] == 0xFF) { bit += 8; continue; }
        if (!(bm[bit >> 3] & (1u << (bit & 7)))) break;
        bit++;
    }
    return bit < nbits ? bit : nbits;
}

/* Allocates one block, preferring `goal` and the groups after it. */
static uint32_t ext4_alloc_block_goal(ext4_t *fs, uint32_t goal)
{
    uint32_t sg = 0, sbit = 0;
    if (goal >= fs->first_data_block && goal < fs->blocks_count) {
        sg = (goal - fs->first_data_block) / fs->blocks_per_group;
        sbit = (goal - fs->first_data_block) % fs->blocks_per_group;
    }
    uint8_t *bm = (uint8_t *)malloc(fs->block_size);
    if (!bm) return 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t n = 0; n < fs->num_groups; n++) {
            uint32_t g = (sg + n) % fs->num_groups;
            ext4_gd_t gd;
            if (ext4_read_group_desc(fs, g, &gd) != 0) continue;
            if (gd.bg_free_blocks_count == 0) continue;
            int uninit = ext4_group_flag(fs, &gd, EXT4_BG_BLOCK_UNINIT);
            if (pass == 0 && uninit) continue;

            uint32_t base = fs->first_data_block + g * fs->blocks_per_group;
            uint32_t in_group = fs->blocks_count - base;
            if (in_group > fs->blocks_per_group) in_group = fs->blocks_per_group;
            if (ext4_load_block_bitmap(fs, g, &gd, bm) != 0) continue;

            uint32_t bit = ext4_bm_next_free(bm, n == 0 ? sbit : 0, in_group);
            if (bit >= in_group && n == 0 && sbit > 0) bit = ext4_bm_next_free(bm, 0, in_group);
            if (bit >= in_group) continue;

            ext4_bit_set(bm, bit);
            ext4_cached_write_block(fs, gd.bg_block_bitmap, bm);
            gd.bg_free_blocks_count--;
            if (fs->lazy) gd.bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
            ext4_gd_bitmap_csum(fs, &gd, 0, bm);
            ext4_write_group_desc(fs, g, &gd);
            ext4_sb_adjust(fs, -1, 0);
            free(bm);
            uint32_t block = base + bit;
            ext4_zalloc_block_raw(fs, block);
            return block;
        }
    }
    free(bm);
    return 0;
}

static uint32_t ext4_alloc_block(ext4_t *fs)
{
    return ext4_alloc_block_goal(fs, 0);
}

/* Frees `count` blocks starting at `start`, one bitmap pass per group. */
static void ext4_free_range(ext4_t *fs, uint32_t start, uint32_t count)
{
    if (start < fs->first_data_block || start >= fs->blocks_count) return;
    if (count > fs->blocks_count - start) count = fs->blocks_count - start;
    uint8_t *bm = (uint8_t *)malloc(fs->block_size);
    if (!bm) return;
    while (count > 0) {
        uint32_t group = (start - fs->first_data_block) / fs->blocks_per_group;
        uint32_t bit0 = (start - fs->first_data_block) % fs->blocks_per_group;
        uint32_t n = fs->blocks_per_group - bit0;
        if (n > count) n = count;
        ext4_gd_t gd;
        if (ext4_read_group_desc(fs, group, &gd) != 0) break;
        if (ext4_load_block_bitmap(fs, group, &gd, bm) != 0) break;
        uint32_t cleared = 0;
        for (uint32_t b = bit0; b < bit0 + n; b++) {
            if (bm[b >> 3] & (1u << (b & 7))) { bm[b >> 3] &= (uint8_t)~(1u << (b & 7)); cleared++; }
        }
        if (cleared) {
            /* an uninitialised group has nothing allocated; nothing to write back */
            if (!ext4_group_flag(fs, &gd, EXT4_BG_BLOCK_UNINIT)) {
                ext4_cached_write_block(fs, gd.bg_block_bitmap, bm);
                gd.bg_free_blocks_count = (uint16_t)(gd.bg_free_blocks_count + cleared);
                ext4_gd_bitmap_csum(fs, &gd, 0, bm);
                ext4_write_group_desc(fs, group, &gd);
                ext4_sb_adjust(fs, (int)cleared, 0);
            }
        }
        start += n;
        count -= n;
    }
    free(bm);
}

static void ext4_free_block(ext4_t *fs, uint32_t block)
{
    if (block == 0) return;
    ext4_free_range(fs, block, 1);
}

static uint32_t ext4_alloc_inode(ext4_t *fs, int is_dir)
{
    uint8_t *bm = (uint8_t *)malloc(fs->block_size);
    if (!bm) return 0;
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        ext4_gd_t gd;
        if (ext4_read_group_desc(fs, g, &gd) != 0) continue;
        if (gd.bg_free_inodes_count == 0) continue;
        if (ext4_load_inode_bitmap(fs, &gd, bm) != 0) continue;

        uint32_t bit = ext4_bm_next_free(bm, 0, fs->inodes_per_group);
        while (bit < fs->inodes_per_group && g * fs->inodes_per_group + bit + 1 < fs->first_ino)
            bit = ext4_bm_next_free(bm, bit + 1, fs->inodes_per_group);
        if (bit >= fs->inodes_per_group) continue;

        ext4_bit_set(bm, bit);
        ext4_cached_write_block(fs, gd.bg_inode_bitmap, bm);
        gd.bg_free_inodes_count--;
        if (is_dir) gd.bg_used_dirs_count++;
        if (fs->lazy) {
            gd.bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
            uint32_t unused = gd.bg_itable_unused_lo;
            if (fs->inodes_per_group - unused < bit + 1) gd.bg_itable_unused_lo = (uint16_t)(fs->inodes_per_group - (bit + 1));
        }
        ext4_gd_bitmap_csum(fs, &gd, 1, bm);
        ext4_write_group_desc(fs, g, &gd);
        ext4_sb_adjust(fs, 0, -1);
        free(bm);
        return g * fs->inodes_per_group + bit + 1;
    }
    free(bm);
    return 0;
}

static void ext4_free_inode(ext4_t *fs, uint32_t ino, int is_dir)
{
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    ext4_gd_t gd;
    if (ext4_read_group_desc(fs, group, &gd) != 0) return;
    uint8_t *bm = (uint8_t *)malloc(fs->block_size);
    if (!bm) return;
    if (ext4_load_inode_bitmap(fs, &gd, bm) == 0) {
        bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        ext4_cached_write_block(fs, gd.bg_inode_bitmap, bm);
        ext4_gd_bitmap_csum(fs, &gd, 1, bm);
    }
    free(bm);
    gd.bg_free_inodes_count++;
    if (is_dir && gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
    ext4_write_group_desc(fs, group, &gd);
    ext4_sb_adjust(fs, 0, 1);
}

/* ---------- extent tree ---------- */

#define EXT4_MAX_DEPTH 5

static ext4_extent_header_t *ext4_root_hdr(ext4_inode_t *inode)
{
    return (ext4_extent_header_t *)inode->i_block;
}

static void ext4_ext_init_inode(ext4_inode_t *inode)
{
    memset(inode->i_block, 0, sizeof(inode->i_block));
    ext4_extent_header_t *hdr = ext4_root_hdr(inode);
    hdr->magic = EXT4_EXTENT_MAGIC;
    hdr->entries = 0;
    hdr->max = EXT4_ROOT_MAX_ENTRIES;
    hdr->depth = 0;
    hdr->generation = 0;
}

static uint16_t ext4_leaf_max_entries(ext4_t *fs)
{
    return (uint16_t)((fs->block_size - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t));
}

static uint32_t ext4_ee_len(const ext4_extent_t *e)
{
    return e->ee_len > 32768 ? (uint32_t)e->ee_len - 32768 : e->ee_len;
}

/* extent blocks end with a crc32c of the node, seeded from the inode */
static void ext4_ext_seal(ext4_t *fs, uint32_t iseed, uint8_t *buf)
{
    if (!fs->csum_md) return;
    uint32_t off = 12 + 12 * (uint32_t)rd16(buf + 4);
    if (off + 4 > fs->block_size) return;
    wr32(buf + off, crc32c_update(iseed, buf, off));
}

static int ext4_ext_write_node(ext4_t *fs, uint32_t iseed, uint32_t blk, uint8_t *buf)
{
    ext4_ext_seal(fs, iseed, buf);
    return ext4_cached_write_block(fs, blk, buf);
}

typedef struct {
    ext4_t *fs;
    ext4_inode_t *inode;
    uint8_t *buf[EXT4_MAX_DEPTH + 1];
    uint32_t blk[EXT4_MAX_DEPTH + 1];
    int sel[EXT4_MAX_DEPTH + 1];
    int depth;
} ext4_path_t;

static uint8_t *ext4_pnode(ext4_path_t *p, int l)
{
    return l == 0 ? (uint8_t *)p->inode->i_block : p->buf[l];
}

static void ext4_path_free(ext4_path_t *p)
{
    for (int l = 1; l <= EXT4_MAX_DEPTH; l++) { free(p->buf[l]); p->buf[l] = 0; }
}

/* Walks from the root to the leaf that covers (or would take) lblk. */
static int ext4_path_build(ext4_t *fs, ext4_inode_t *inode, uint32_t lblk, ext4_path_t *p)
{
    memset(p, 0, sizeof(*p));
    p->fs = fs;
    p->inode = inode;
    uint8_t *node = (uint8_t *)inode->i_block;
    for (int l = 0;; l++) {
        ext4_extent_header_t *h = (ext4_extent_header_t *)node;
        if (h->magic != EXT4_EXTENT_MAGIC) goto bad;
        uint32_t cap = l == 0 ? EXT4_ROOT_MAX_ENTRIES : ext4_leaf_max_entries(fs);
        if (h->entries > cap || h->entries > h->max) goto bad;
        if (l == 0) {
            if (h->depth > EXT4_MAX_DEPTH) goto bad;
            p->depth = h->depth;
        } else if (h->depth != p->depth - l) {
            goto bad;
        }
        if (l == p->depth) { p->sel[l] = -1; return 0; }
        if (h->entries == 0) goto bad;
        ext4_extent_idx_t *ix = (ext4_extent_idx_t *)(node + sizeof(ext4_extent_header_t));
        int sel = -1;
        for (uint16_t i = 0; i < h->entries; i++) {
            if (ix[i].ei_block <= lblk) sel = i; else break;
        }
        if (sel < 0) sel = 0;
        p->sel[l] = sel;
        uint32_t child = ix[sel].ei_leaf_lo;
        if (child < fs->first_data_block || child >= fs->blocks_count) goto bad;
        p->buf[l + 1] = (uint8_t *)malloc(fs->block_size);
        if (!p->buf[l + 1]) goto bad;
        if (ext4_cached_read_block(fs, child, p->buf[l + 1]) != 0) goto bad;
        p->blk[l + 1] = child;
        node = p->buf[l + 1];
    }
bad:
    ext4_path_free(p);
    return -1;
}

/* Index of the extent holding lblk in a leaf, or -1; *pos is the insertion point. */
static int ext4_leaf_find(uint8_t *leaf, uint32_t lblk, int *pos)
{
    ext4_extent_header_t *h = (ext4_extent_header_t *)leaf;
    ext4_extent_t *e = (ext4_extent_t *)(leaf + sizeof(ext4_extent_header_t));
    int p = 0, hit = -1;
    while (p < h->entries && e[p].ee_block <= lblk) p++;
    if (p > 0 && lblk - e[p - 1].ee_block < ext4_ee_len(&e[p - 1])) hit = p - 1;
    if (pos) *pos = p;
    return hit;
}

/* leaf block written with a new first key: raise the index keys above it */
static void ext4_ext_fix_keys(ext4_t *fs, uint32_t iseed, ext4_path_t *p, uint32_t lblk)
{
    for (int l = p->depth - 1; l >= 0; l--) {
        ext4_extent_idx_t *ix = (ext4_extent_idx_t *)(ext4_pnode(p, l) + sizeof(ext4_extent_header_t));
        int sel = p->sel[l];
        if (ix[sel].ei_block <= lblk) break;
        ix[sel].ei_block = lblk;
        if (l > 0) ext4_ext_write_node(fs, iseed, p->blk[l], p->buf[l]);
    }
}

/* root is full and so is everything below it: push its entries into a new
 * block and make the root a one-entry index above it */
static int ext4_ext_grow(ext4_t *fs, uint32_t iseed, ext4_inode_t *inode)
{
    ext4_extent_header_t *rh = ext4_root_hdr(inode);
    if (rh->depth + 1 > EXT4_MAX_DEPTH) return -1;
    uint32_t b = ext4_alloc_block_goal(fs, 0);
    if (b == 0) return -1;
    uint8_t *nb = (uint8_t *)malloc(fs->block_size);
    if (!nb) { ext4_free_block(fs, b); return -1; }
    memset(nb, 0, fs->block_size);
    ext4_extent_header_t *nh = (ext4_extent_header_t *)nb;
    nh->magic = EXT4_EXTENT_MAGIC;
    nh->depth = rh->depth;
    nh->max = ext4_leaf_max_entries(fs);
    nh->entries = rh->entries;
    memcpy(nb + 12, (uint8_t *)inode->i_block + 12, (size_t)rh->entries * 12);
    ext4_ext_write_node(fs, iseed, b, nb);
    free(nb);

    uint32_t key = rh->depth == 0 ? ((ext4_extent_t *)((uint8_t *)inode->i_block + 12))[0].ee_block
                                  : ((ext4_extent_idx_t *)((uint8_t *)inode->i_block + 12))[0].ei_block;
    uint16_t nd = (uint16_t)(rh->depth + 1);
    ext4_ext_init_inode(inode);
    rh = ext4_root_hdr(inode);
    rh->depth = nd;
    rh->entries = 1;
    ext4_extent_idx_t *ix = (ext4_extent_idx_t *)((uint8_t *)inode->i_block + 12);
    ix[0].ei_block = key;
    ix[0].ei_leaf_lo = b;
    inode->i_blocks += ext4_units(fs, inode);
    return 0;
}

/* Maps logical block lblk to the (just allocated) physical block pblk. */
static int ext4_ext_insert(ext4_t *fs, uint32_t ino, ext4_inode_t *inode, uint32_t lblk, uint32_t pblk)
{
    uint32_t iseed = ext4_iseed(fs, ino, inode);
    for (int iter = 0; iter < 64; iter++) {
        ext4_path_t p;
        if (ext4_path_build(fs, inode, lblk, &p) != 0) return -1;
        int d = p.depth;
        uint8_t *leaf = ext4_pnode(&p, d);
        ext4_extent_header_t *lh = (ext4_extent_header_t *)leaf;
        ext4_extent_t *e = (ext4_extent_t *)(leaf + sizeof(ext4_extent_header_t));
        int pos;
        if (ext4_leaf_find(leaf, lblk, &pos) >= 0) { ext4_path_free(&p); return -1; }

        int done = 0;
        if (pos > 0 && e[pos - 1].ee_len < 32768 && e[pos - 1].ee_block + e[pos - 1].ee_len == lblk &&
            e[pos - 1].ee_start_hi == 0 && e[pos - 1].ee_start_lo + e[pos - 1].ee_len == pblk) {
            e[pos - 1].ee_len++;
            done = 1;
        } else if (pos < lh->entries && e[pos].ee_len < 32768 && e[pos].ee_block == lblk + 1 &&
                   e[pos].ee_start_hi == 0 && e[pos].ee_start_lo == pblk + 1) {
            e[pos].ee_block = lblk;
            e[pos].ee_start_lo = pblk;
            e[pos].ee_len++;
            done = 1;
        } else if (lh->entries < lh->max) {
            memmove(&e[pos + 1], &e[pos], (size_t)(lh->entries - pos) * sizeof(ext4_extent_t));
            e[pos].ee_block = lblk;
            e[pos].ee_len = 1;
            e[pos].ee_start_hi = 0;
            e[pos].ee_start_lo = pblk;
            lh->entries++;
            done = 1;
        }
        if (done) {
            if (d > 0) ext4_ext_write_node(fs, iseed, p.blk[d], leaf);
            if (pos == 0) ext4_ext_fix_keys(fs, iseed, &p, lblk);
            ext4_path_free(&p);
            return 0;
        }

        /* the leaf is full: split the lowest full node that has a parent with room */
        int j = -1;
        for (int l = d - 1; l >= 0; l--) {
            ext4_extent_header_t *h = (ext4_extent_header_t *)ext4_pnode(&p, l);
            if (h->entries < h->max) { j = l; break; }
        }
        if (j < 0) {
            ext4_path_free(&p);
            if (ext4_ext_grow(fs, iseed, inode) != 0) return -1;
            continue;
        }
        int c = j + 1;
        uint8_t *x = ext4_pnode(&p, c);
        ext4_extent_header_t *xh = (ext4_extent_header_t *)x;
        uint32_t n = xh->entries;
        uint32_t nblk = ext4_alloc_block_goal(fs, p.blk[c]);
        uint8_t *nbuf = nblk ? (uint8_t *)malloc(fs->block_size) : 0;
        if (!nbuf) { if (nblk) ext4_free_block(fs, nblk); ext4_path_free(&p); return -1; }
        memset(nbuf, 0, fs->block_size);
        ext4_extent_header_t *nh = (ext4_extent_header_t *)nbuf;
        nh->magic = EXT4_EXTENT_MAGIC;
        nh->depth = xh->depth;
        nh->max = ext4_leaf_max_entries(fs);
        uint32_t key;
        if (c == d && pos == (int)n) {
            key = lblk;                                  /* appending: start a fresh leaf */
        } else {
            uint32_t half = n / 2;
            memcpy(nbuf + 12, x + 12 + half * 12, (size_t)(n - half) * 12);
            nh->entries = (uint16_t)(n - half);
            xh->entries = (uint16_t)half;
            key = rd32(nbuf + 12);                       /* ee_block / ei_block share offset 0 */
            ext4_ext_write_node(fs, iseed, p.blk[c], x);
        }
        ext4_ext_write_node(fs, iseed, nblk, nbuf);
        free(nbuf);

        uint8_t *pn = ext4_pnode(&p, j);
        ext4_extent_header_t *ph = (ext4_extent_header_t *)pn;
        ext4_extent_idx_t *pi = (ext4_extent_idx_t *)(pn + 12);
        int ps = p.sel[j] + 1;
        memmove(&pi[ps + 1], &pi[ps], (size_t)(ph->entries - ps) * sizeof(ext4_extent_idx_t));
        pi[ps].ei_block = key;
        pi[ps].ei_leaf_lo = nblk;
        pi[ps].ei_leaf_hi = 0;
        pi[ps].ei_unused = 0;
        ph->entries++;
        if (j > 0) ext4_ext_write_node(fs, iseed, p.blk[j], pn);
        inode->i_blocks += ext4_units(fs, inode);
        ext4_path_free(&p);
    }
    return -1;
}

/* An unwritten (fallocate) extent that is being written: its blocks may hold
 * stale data, so zero them and mark the extent written. */
static int ext4_ext_convert_unwritten(ext4_t *fs, uint32_t ino, ext4_inode_t *inode, uint32_t lblk)
{
    ext4_path_t p;
    if (ext4_path_build(fs, inode, lblk, &p) != 0) return -1;
    uint8_t *leaf = ext4_pnode(&p, p.depth);
    int hit = ext4_leaf_find(leaf, lblk, 0);
    int rc = -1;
    if (hit >= 0) {
        ext4_extent_t *e = &((ext4_extent_t *)(leaf + sizeof(ext4_extent_header_t)))[hit];
        if (e->ee_len > 32768) {
            uint32_t len = ext4_ee_len(e);
            uint8_t *z = (uint8_t *)malloc(fs->block_size);
            if (z) {
                memset(z, 0, fs->block_size);
                for (uint32_t i = 0; i < len; i++) ext4_cached_write_block(fs, e->ee_start_lo + i, z);
                free(z);
                e->ee_len = (uint16_t)len;
                if (p.depth > 0) ext4_ext_write_node(fs, ext4_iseed(fs, ino, inode), p.blk[p.depth], leaf);
                rc = 0;
            }
        }
    }
    ext4_path_free(&p);
    return rc;
}

/* classic ext2/3 block map (files that predate the extents feature): read only */
static uint32_t ext4_indirect_lookup(ext4_t *fs, ext4_inode_t *inode, uint32_t index)
{
    uint32_t per = fs->block_size / 4;
    if (index < 12) return inode->i_block[index];
    index -= 12;
    uint32_t blk, levels;
    if (index < per) { blk = inode->i_block[12]; levels = 1; }
    else if ((index -= per) < per * per) { blk = inode->i_block[13]; levels = 2; }
    else { index -= per * per; blk = inode->i_block[14]; levels = 3; }
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return 0;
    while (levels > 0 && blk != 0) {
        uint32_t span = 1;
        for (uint32_t i = 1; i < levels; i++) span *= per;
        if (blk >= fs->blocks_count || ext4_cached_read_block(fs, blk, buf) != 0) { blk = 0; break; }
        uint32_t slot = index / span;
        index %= span;
        if (slot >= per) { blk = 0; break; }
        blk = rd32(buf + slot * 4);
        levels--;
    }
    free(buf);
    return blk;
}

static uint32_t ext4_bmap(ext4_t *fs, uint32_t ino, ext4_inode_t *inode, uint32_t index, int alloc)
{
    if (!(inode->i_flags & EXT4_EXTENTS_FL) && ext4_root_hdr(inode)->magic != EXT4_EXTENT_MAGIC) {
        if (alloc && inode->i_blocks == 0) {
            ext4_ext_init_inode(inode);
            inode->i_flags |= EXT4_EXTENTS_FL;
        } else {
            return alloc ? 0 : ext4_indirect_lookup(fs, inode, index);
        }
    }
    if (ext4_root_hdr(inode)->magic != EXT4_EXTENT_MAGIC) {
        if (!alloc) return 0;
        ext4_ext_init_inode(inode);
    }

    ext4_path_t p;
    if (ext4_path_build(fs, inode, index, &p) != 0) return 0;
    uint8_t *leaf = ext4_pnode(&p, p.depth);
    int pos;
    int hit = ext4_leaf_find(leaf, index, &pos);
    ext4_extent_t *e = (ext4_extent_t *)(leaf + sizeof(ext4_extent_header_t));
    uint32_t goal = 0;
    int unwritten = 0;
    uint32_t result = 0;
    if (hit >= 0) {
        if (e[hit].ee_len > 32768) unwritten = 1;
        else result = e[hit].ee_start_lo + (index - e[hit].ee_block);
    } else if (pos > 0) {
        goal = e[pos - 1].ee_start_lo + ext4_ee_len(&e[pos - 1]);
    }
    ext4_path_free(&p);

    if (!alloc) return result;
    if (result) return result;
    if (unwritten) {
        if (ext4_ext_convert_unwritten(fs, ino, inode, index) != 0) return 0;
        return ext4_bmap(fs, ino, inode, index, 0);
    }

    uint32_t nb = ext4_alloc_block_goal(fs, goal);
    if (nb == 0) return 0;
    if (ext4_ext_insert(fs, ino, inode, index, nb) != 0) { ext4_free_block(fs, nb); return 0; }
    inode->i_blocks += ext4_units(fs, inode);
    return nb;
}

static void ext4_free_extent_node(ext4_t *fs, const uint8_t *node, int depth_left)
{
    const ext4_extent_header_t *h = (const ext4_extent_header_t *)node;
    if (h->magic != EXT4_EXTENT_MAGIC || h->entries > h->max || depth_left < 0) return;
    if (h->depth == 0) {
        const ext4_extent_t *e = (const ext4_extent_t *)(node + sizeof(*h));
        for (uint16_t i = 0; i < h->entries; i++) ext4_free_range(fs, e[i].ee_start_lo, ext4_ee_len(&e[i]));
        return;
    }
    const ext4_extent_idx_t *ix = (const ext4_extent_idx_t *)(node + sizeof(*h));
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return;
    for (uint16_t i = 0; i < h->entries; i++) {
        uint32_t child = ix[i].ei_leaf_lo;
        if (child < fs->first_data_block || child >= fs->blocks_count) continue;
        if (ext4_cached_read_block(fs, child, buf) == 0) ext4_free_extent_node(fs, buf, depth_left - 1);
        ext4_free_range(fs, child, 1);
    }
    free(buf);
}

static void ext4_free_indirect(ext4_t *fs, uint32_t blk, int levels)
{
    if (blk == 0 || blk >= fs->blocks_count) return;
    if (levels > 0) {
        uint8_t *buf = (uint8_t *)malloc(fs->block_size);
        if (buf && ext4_cached_read_block(fs, blk, buf) == 0) {
            for (uint32_t i = 0; i < fs->block_size / 4; i++) ext4_free_indirect(fs, rd32(buf + i * 4), levels - 1);
        }
        free(buf);
    }
    ext4_free_range(fs, blk, 1);
}

/* extended-attribute block: shared, so drop one reference */
static void ext4_free_xattr_block(ext4_t *fs, ext4_inode_t *inode)
{
    uint32_t blk = inode->i_file_acl;
    inode->i_file_acl = 0;
    if (blk == 0 || blk >= fs->blocks_count) return;
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return;
    if (ext4_cached_read_block(fs, blk, buf) == 0 && rd32(buf) == 0xEA020000u) {
        uint32_t refs = rd32(buf + 4);
        if (refs > 1) {
            wr32(buf + 4, refs - 1);
            if (fs->csum_md) {
                uint8_t le[8] = { 0 };
                wr32(le, blk);
                wr32(buf + 16, 0);
                uint32_t c = crc32c_update(fs->csum_seed, le, 8);
                wr32(buf + 16, crc32c_update(c, buf, fs->block_size));
            }
            ext4_cached_write_block(fs, blk, buf);
        } else {
            ext4_free_range(fs, blk, 1);
        }
    }
    free(buf);
}

/* Releases every block of the file and resets it to an empty extent tree. */
static void ext4_free_inode_blocks(ext4_t *fs, ext4_inode_t *inode)
{
    int fast_symlink = (inode->i_mode & 0xF000) == 0xA000 && inode->i_blocks == 0;
    if (!fast_symlink) {
        if ((inode->i_flags & EXT4_EXTENTS_FL) || ext4_root_hdr(inode)->magic == EXT4_EXTENT_MAGIC) {
            ext4_free_extent_node(fs, (const uint8_t *)inode->i_block, EXT4_MAX_DEPTH);
        } else {
            for (int i = 0; i < 12; i++) ext4_free_range(fs, inode->i_block[i], inode->i_block[i] ? 1 : 0);
            ext4_free_indirect(fs, inode->i_block[12], 1);
            ext4_free_indirect(fs, inode->i_block[13], 2);
            ext4_free_indirect(fs, inode->i_block[14], 3);
        }
    }
    if (inode->i_file_acl) ext4_free_xattr_block(fs, inode);
    ext4_ext_init_inode(inode);
    inode->i_flags |= EXT4_EXTENTS_FL;
    inode->i_blocks = 0;
}

static int ext4_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
{
    int len = (int)strlen(path);
    int last_sep = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_sep = i; break; }
    }
    if (last_sep < 0) {
        if (parent_sz < 1) return -1;
        parent[0] = 0;
        size_t k = 0;
        while (path[k] && k < name_sz - 1) { name[k] = path[k]; k++; }
        name[k] = 0;
        return 0;
    }
    if ((size_t)last_sep >= parent_sz) return -1;
    int i;
    for (i = 0; i < last_sep; i++) parent[i] = path[i];
    parent[i] = 0;
    size_t k = 0;
    for (int j = last_sep + 1; path[j] && k < name_sz - 1; j++) name[k++] = path[j];
    name[k] = 0;
    return 0;
}

static int ext4_read_inode(ext4_t *fs, uint32_t ino, ext4_inode_t *out)
{
    uint8_t raw[128];
    if (ext4_read_inode_raw(fs, ino, raw) != 0) return -1;
    memcpy(out, raw, sizeof(ext4_inode_t));
    return 0;
}

static int ext4_write_inode(ext4_t *fs, uint32_t ino, const ext4_inode_t *in)
{
    return ext4_write_inode_raw(fs, ino, (const uint8_t *)in);
}

static int ext4_rec_len_min(int name_len)
{
    int need = 8 + name_len;
    return (need + 3) & ~3;
}

typedef struct {
    uint32_t ino;
    uint32_t dir_ino;
    uint32_t block_index;
    uint32_t offset_in_block;
    int file_type;
} ext4_dirent_loc_t;

/* a directory entry that fits inside the block (corrupt rec_len must not loop or overrun) */
static int ext4_dirent_ok(const uint8_t *buf, uint32_t pos, uint32_t bs)
{
    if (pos + 8 > bs) return 0;
    uint16_t rl = rd16(buf + pos + 4);
    uint8_t nl = buf[pos + 6];
    return rl >= 8 && (rl & 3) == 0 && pos + rl <= bs && 8u + nl <= rl;
}

/* metadata_csum directory leaves end in a fake 12-byte entry holding the checksum */
static int ext4_dir_has_tail(ext4_t *fs, const uint8_t *buf)
{
    const uint8_t *t = buf + fs->block_size - 12;
    return rd32(t) == 0 && rd16(t + 4) == 12 && t[6] == 0 && t[7] == EXT4_DIRENT_TAIL_FT;
}

static void ext4_dir_put_tail(ext4_t *fs, uint8_t *buf)
{
    uint8_t *t = buf + fs->block_size - 12;
    memset(t, 0, 12);
    wr16(t + 4, 12);
    t[7] = EXT4_DIRENT_TAIL_FT;
}

/* usable length of a leaf: everything before the checksum tail */
static uint32_t ext4_dir_limit(ext4_t *fs, const uint8_t *buf)
{
    return (fs->csum_md && ext4_dir_has_tail(fs, buf)) ? fs->block_size - 12 : fs->block_size;
}

/* make room for the tail in a leaf that lacks one (slack in the last entry) */
static int ext4_dir_ensure_tail(ext4_t *fs, uint8_t *buf)
{
    if (!fs->csum_md || ext4_dir_has_tail(fs, buf)) return 0;
    uint32_t pos = 0;
    while (pos < fs->block_size) {
        if (!ext4_dirent_ok(buf, pos, fs->block_size)) return -1;
        uint16_t rl = rd16(buf + pos + 4);
        if (pos + rl == fs->block_size) {
            uint16_t used = rd32(buf + pos) ? (uint16_t)ext4_rec_len_min(buf[pos + 6]) : 8;
            if (rl - used < 12) return -1;
            wr16(buf + pos + 4, (uint16_t)(rl - 12));
            ext4_dir_put_tail(fs, buf);
            return 0;
        }
        pos += rl;
    }
    return -1;
}

static int ext4_dir_write_block(ext4_t *fs, uint32_t iseed, uint32_t blk, uint8_t *buf)
{
    if (fs->csum_md && ext4_dir_has_tail(fs, buf))
        wr32(buf + fs->block_size - 4, crc32c_update(iseed, buf, fs->block_size - 12));
    return ext4_cached_write_block(fs, blk, buf);
}

/* A leaf holding no entries (everything is one free slot). */
static void ext4_dir_empty_leaf(ext4_t *fs, uint8_t *buf)
{
    memset(buf, 0, fs->block_size);
    wr16(buf + 4, (uint16_t)(fs->csum_md ? fs->block_size - 12 : fs->block_size));
    if (fs->csum_md) ext4_dir_put_tail(fs, buf);
}

/* Directories with a hash-tree index: adding an entry would have to keep the
 * index in step, so the index is dropped instead (the entries stay in their
 * leaf blocks; "fsck -D" rebuilds an index later). The root block keeps "."
 * and "..", every interior index node becomes an empty leaf. */
static int ext4_dir_unindex(ext4_t *fs, uint32_t dir_ino, ext4_inode_t *dir)
{
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;
    uint32_t iseed = ext4_iseed(fs, dir_ino, dir);
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) { free(buf); return -1; }
        if (bi == 0) {
            if (!ext4_dirent_ok(buf, 0, fs->block_size) || rd16(buf + 4) != 12 || !ext4_dirent_ok(buf, 12, fs->block_size)) {
                free(buf);
                return -1;
            }
            uint32_t lim = fs->csum_md ? fs->block_size - 12 : fs->block_size;
            memset(buf + 24, 0, fs->block_size - 24);
            wr16(buf + 12 + 4, (uint16_t)(lim - 12));
            if (fs->csum_md) ext4_dir_put_tail(fs, buf);
            ext4_dir_write_block(fs, iseed, blk, buf);
        } else if (rd32(buf) == 0 && rd16(buf + 4) == fs->block_size) {
            ext4_dir_empty_leaf(fs, buf);
            ext4_dir_write_block(fs, iseed, blk, buf);
        }
    }
    free(buf);
    dir->i_flags &= ~EXT4_INDEX_FL;
    return ext4_write_inode_raw(fs, dir_ino, (const uint8_t *)dir);
}

static int ext4_dir_find(ext4_t *fs, uint32_t dir_ino, const char *name, ext4_dirent_loc_t *out)
{
    ext4_inode_t dir;
    if (ext4_read_inode(fs, dir_ino, &dir) != 0) return -1;
    if ((dir.i_mode & 0xF000) != EXT4_S_IFDIR) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            if (!ext4_dirent_ok(buf, pos, fs->block_size)) break;
            ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)(buf + pos);
            if (hdr->ino != 0 && hdr->name_len == name_len &&
                memcmp(buf + pos + 8, name, (size_t)name_len) == 0) {
                if (out) {
                    out->ino = hdr->ino;
                    out->dir_ino = dir_ino;
                    out->block_index = bi;
                    out->offset_in_block = pos;
                    out->file_type = hdr->file_type;
                }
                free(buf);
                return 0;
            }
            pos += hdr->rec_len;
        }
    }
    free(buf);
    return -1;
}

static int ext4_dir_add_entry(ext4_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, int file_type)
{
    ext4_inode_t dir;
    if (ext4_read_inode(fs, dir_ino, &dir) != 0) return -1;
    if (dir.i_flags & EXT4_INDEX_FL) {
        if (ext4_dir_unindex(fs, dir_ino, &dir) != 0) return -1;
    }
    uint32_t iseed = ext4_iseed(fs, dir_ino, &dir);

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    if (name_len == 0 || name_len > EXT4_MAX_FILENAME) { free(buf); return -1; }
    uint16_t need = (uint16_t)ext4_rec_len_min(name_len);

    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) continue;
        if (ext4_dir_ensure_tail(fs, buf) != 0) continue;
        uint32_t limit = ext4_dir_limit(fs, buf);

        uint32_t pos = 0;
        while (pos < limit) {
            if (!ext4_dirent_ok(buf, pos, limit)) break;
            ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)(buf + pos);

            uint16_t used = hdr->ino != 0 ? (uint16_t)ext4_rec_len_min(hdr->name_len) : 0;
            uint16_t avail = (uint16_t)(hdr->rec_len - used);

            if (avail >= need) {
                if (hdr->ino == 0) {
                    hdr->ino = ino;
                    hdr->name_len = (uint8_t)name_len;
                    hdr->file_type = (uint8_t)file_type;
                    memcpy(buf + pos + 8, name, (size_t)name_len);
                } else {
                    uint16_t old_rec_len = hdr->rec_len;
                    hdr->rec_len = used;
                    uint32_t new_pos = pos + used;
                    ext4_dirent_hdr_t *nh = (ext4_dirent_hdr_t *)(buf + new_pos);
                    nh->ino = ino;
                    nh->rec_len = (uint16_t)(old_rec_len - used);
                    nh->name_len = (uint8_t)name_len;
                    nh->file_type = (uint8_t)file_type;
                    memcpy(buf + new_pos + 8, name, (size_t)name_len);
                }
                ext4_dir_write_block(fs, iseed, blk, buf);
                free(buf);
                return 0;
            }
            pos += hdr->rec_len;
        }
    }

    uint32_t new_blk = ext4_bmap(fs, dir_ino, &dir, nblocks, 1);
    if (new_blk == 0) { free(buf); return -1; }

    ext4_dir_empty_leaf(fs, buf);
    ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)buf;
    hdr->ino = ino;
    hdr->name_len = (uint8_t)name_len;
    hdr->file_type = (uint8_t)file_type;
    memcpy(buf + 8, name, (size_t)name_len);
    ext4_dir_write_block(fs, iseed, new_blk, buf);
    free(buf);

    dir.i_size += fs->block_size;
    ext4_write_inode(fs, dir_ino, &dir);
    return 0;
}

static int ext4_dir_remove_entry(ext4_t *fs, uint32_t dir_ino, const char *name)
{
    ext4_inode_t dir;
    if (ext4_read_inode(fs, dir_ino, &dir) != 0) return -1;
    uint32_t iseed = ext4_iseed(fs, dir_ino, &dir);

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            if (!ext4_dirent_ok(buf, pos, fs->block_size)) break;
            ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)(buf + pos);
            if (hdr->ino != 0 && hdr->name_len == name_len &&
                memcmp(buf + pos + 8, name, (size_t)name_len) == 0) {
                hdr->ino = 0;
                ext4_dir_write_block(fs, iseed, blk, buf);
                free(buf);
                return 0;
            }
            pos += hdr->rec_len;
        }
    }
    free(buf);
    return -1;
}

static int ext4_dir_is_empty(ext4_t *fs, uint32_t dir_ino)
{
    ext4_inode_t dir;
    if (ext4_read_inode(fs, dir_ino, &dir) != 0) return 0;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return 0;

    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
    int empty = 1;

    for (uint32_t bi = 0; bi < nblocks && empty; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            if (!ext4_dirent_ok(buf, pos, fs->block_size)) break;
            ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)(buf + pos);
            if (hdr->ino != 0) {
                int is_dot = (hdr->name_len == 1 && buf[pos + 8] == '.');
                int is_dotdot = (hdr->name_len == 2 && buf[pos + 8] == '.' && buf[pos + 9] == '.');
                if (!is_dot && !is_dotdot) { empty = 0; break; }
            }
            pos += hdr->rec_len;
        }
    }
    free(buf);
    return empty;
}

static int ext4_dir_lookup_component(ext4_t *fs, uint32_t dir_ino, const char *comp, uint32_t *out_ino, int *out_is_dir)
{
    ext4_dirent_loc_t loc;
    if (ext4_dir_find(fs, dir_ino, comp, &loc) != 0) return -1;
    *out_ino = loc.ino;
    if (out_is_dir) *out_is_dir = (loc.file_type == EXT4_FT_DIR);
    return 0;
}

static int ext4_walk(ext4_t *fs, const char *path, uint32_t *out_ino)
{
    uint32_t cur = EXT4_ROOT_INO;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[EXT4_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < EXT4_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        while (*p == '/') p++;

        uint32_t next;
        if (ext4_dir_lookup_component(fs, cur, comp, &next, 0) != 0) return -1;
        cur = next;
    }
    *out_ino = cur;
    return 0;
}

static int ext4_read_data(ext4_t *fs, uint32_t ino, ext4_inode_t *inode, uint32_t offset, void *buf, uint32_t size)
{
    if (offset >= inode->i_size) return 0;
    if (offset + size > inode->i_size) size = inode->i_size - offset;
    if (size == 0) return 0;

    uint8_t *blk_buf = (uint8_t *)malloc(fs->block_size);
    if (!blk_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        uint32_t cur_off = offset + done;
        uint32_t bi = cur_off / fs->block_size;
        uint32_t in_blk = cur_off % fs->block_size;

        uint32_t blk = ext4_bmap(fs, ino, inode, bi, 0);
        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > size - done) chunk = size - done;

        if (blk == 0) {
            memset((uint8_t *)buf + done, 0, chunk);
        } else {
            if (ext4_cached_read_block(fs, blk, blk_buf) != 0) break;
            memcpy((uint8_t *)buf + done, blk_buf + in_blk, chunk);
        }
        done += chunk;
    }

    free(blk_buf);
    return (int)done;
}

static int ext4_write_data(ext4_t *fs, uint32_t ino, ext4_inode_t *inode, uint32_t offset, const void *buf, uint32_t size)
{
    uint8_t *blk_buf = (uint8_t *)malloc(fs->block_size);
    if (!blk_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        uint32_t cur_off = offset + done;
        uint32_t bi = cur_off / fs->block_size;
        uint32_t in_blk = cur_off % fs->block_size;

        uint32_t blk = ext4_bmap(fs, ino, inode, bi, 1);
        if (blk == 0) break;

        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > size - done) chunk = size - done;

        if (chunk < fs->block_size) {
            if (ext4_cached_read_block(fs, blk, blk_buf) != 0) break;
        }
        memcpy(blk_buf + in_blk, (const uint8_t *)buf + done, chunk);
        if (ext4_cached_write_block(fs, blk, blk_buf) != 0) break;

        done += chunk;
    }

    free(blk_buf);
    if (done > 0 && offset + done > inode->i_size) inode->i_size = offset + done;
    return (int)done;
}

/* ---------- journal recovery ---------- */

typedef struct {
    uint32_t sequence;
    uint32_t num_blocks;
    uint32_t targets[EXT4_MAX_TXN_BLOCKS];
    uint8_t *data[EXT4_MAX_TXN_BLOCKS];
} ext4_recover_txn_t;

static void ext4_journal_recover(ext4_t *fs)
{
    uint8_t *jsb_buf = (uint8_t *)malloc(fs->block_size);
    if (!jsb_buf) return;
    if (ext4_raw_read_block(fs, fs->journal_first_block, jsb_buf) != 0) { free(jsb_buf); return; }
    ext4_journal_super_t jsb;
    memcpy(&jsb, jsb_buf, sizeof(jsb));
    free(jsb_buf);

    if (jsb.magic != EXT4_JOURNAL_MAGIC) {
        fs->journal_sequence = 1;
        fs->journal_cursor = 1;
        return;
    }

    uint32_t baseline = jsb.s_committed_seq;
    fs->journal_sequence = baseline;
    fs->journal_cursor = 1;

    ext4_recover_txn_t *txns = (ext4_recover_txn_t *)malloc(sizeof(ext4_recover_txn_t) * EXT4_MAX_RECOVER_TXN);
    int txn_n = 0;
    uint32_t max_seq_seen = baseline;
    int any_replayed = 0;

    uint8_t *blk = (uint8_t *)malloc(fs->block_size);
    uint8_t *cblk = (uint8_t *)malloc(fs->block_size);

    uint32_t cur = 1;
    while (cur < fs->journal_blocks && txn_n < EXT4_MAX_RECOVER_TXN) {
        if (ext4_raw_read_block(fs, fs->journal_first_block + cur, blk) != 0) break;
        ext4_journal_hdr_t *dh = (ext4_journal_hdr_t *)blk;
        if (dh->magic != EXT4_JOURNAL_MAGIC || dh->block_type != EXT4_JBLOCK_DESCRIPTOR) break;
        if (dh->num_blocks > EXT4_MAX_TXN_BLOCKS) break;

        uint32_t num_blocks = dh->num_blocks;
        uint32_t targets[EXT4_MAX_TXN_BLOCKS];
        memcpy(targets, blk + sizeof(ext4_journal_hdr_t), num_blocks * sizeof(uint32_t));

        if (cur + 1 + num_blocks >= fs->journal_blocks) break;

        if (ext4_raw_read_block(fs, fs->journal_first_block + cur + 1 + num_blocks, cblk) != 0) break;
        ext4_journal_hdr_t *ch = (ext4_journal_hdr_t *)cblk;
        if (ch->magic != EXT4_JOURNAL_MAGIC || ch->block_type != EXT4_JBLOCK_COMMIT || ch->sequence != dh->sequence) break;

        if (dh->sequence >= baseline) {
            ext4_recover_txn_t *t = &txns[txn_n];
            t->sequence = dh->sequence;
            t->num_blocks = num_blocks;
            for (uint32_t i = 0; i < num_blocks; i++) {
                t->targets[i] = targets[i];
                t->data[i] = (uint8_t *)malloc(fs->block_size);
                ext4_raw_read_block(fs, fs->journal_first_block + cur + 1 + i, t->data[i]);
            }
            txn_n++;
            if (dh->sequence > max_seq_seen) max_seq_seen = dh->sequence;
            any_replayed = 1;
        }

        cur += 2 + num_blocks;
    }

    /* insertion sort by sequence ascending */
    for (int i = 1; i < txn_n; i++) {
        ext4_recover_txn_t tmp = txns[i];
        int j = i - 1;
        while (j >= 0 && txns[j].sequence > tmp.sequence) {
            txns[j + 1] = txns[j];
            j--;
        }
        txns[j + 1] = tmp;
    }

    for (int i = 0; i < txn_n; i++) {
        for (uint32_t b = 0; b < txns[i].num_blocks; b++) {
            ext4_raw_write_block(fs, txns[i].targets[b], txns[i].data[b]);
            free(txns[i].data[b]);
        }
    }

    free(blk);
    free(cblk);
    free(txns);

    if (any_replayed) {
        uint32_t new_baseline = max_seq_seen + 1;
        fs->journal_sequence = new_baseline;
        uint8_t *buf2 = (uint8_t *)malloc(fs->block_size);
        if (ext4_raw_read_block(fs, fs->journal_first_block, buf2) == 0) {
            ext4_journal_super_t *sb2 = (ext4_journal_super_t *)buf2;
            sb2->s_committed_seq = new_baseline;
            ext4_raw_write_block(fs, fs->journal_first_block, buf2);
        }
        free(buf2);
    }
}

/* ---------- mount / probe ---------- */

static int ext4_probe(ext4_t *fs, blockdev_t *bd)
{
    fs->bd = bd;

    ext4_superblock_t *sb = (ext4_superblock_t *)malloc(1024);
    if (!sb) return -1;
    if (blockdev_read_bytes(bd, 1024, 1024, sb) != 0) { free(sb); return -1; }

    if (sb->s_magic != EXT4_SUPER_MAGIC) { free(sb); return -1; }

    if (sb->s_log_block_size > 6) { free(sb); return -1; }
    fs->block_size = 1024u << sb->s_log_block_size;
    fs->blocks_count = sb->s_blocks_count;
    fs->inodes_count = sb->s_inodes_count;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_data_block = sb->s_first_data_block;
    fs->free_blocks_count = sb->s_free_blocks_count;
    fs->free_inodes_count = sb->s_free_inodes_count;
    fs->inode_size = (sb->s_rev_level >= EXT4_DYNAMIC_REV) ? sb->s_inode_size : 128;
    fs->first_ino = (sb->s_rev_level >= EXT4_DYNAMIC_REV && sb->s_first_ino >= EXT4_FIRST_NON_RESERVED_INO) ? sb->s_first_ino : EXT4_FIRST_NON_RESERVED_INO;
    if (fs->block_size < 1024 || fs->block_size > 65536 || fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
        fs->blocks_count <= fs->first_data_block || fs->inode_size < 128 || fs->inode_size > fs->block_size ||
        (fs->inode_size & (fs->inode_size - 1))) {
        free(sb);
        return -1;
    }
    fs->num_groups = (fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->gdt_block = fs->first_data_block + 1;

    const uint8_t *raw = (const uint8_t *)sb;
    fs->feat_incompat = sb->s_feature_incompat;
    fs->feat_ro = sb->s_feature_ro_compat;
    fs->desc_size = (sb->s_feature_incompat & 0x80 /* 64bit */) ? rd16(raw + 0xFE) : 32;
    int bad_desc = (fs->desc_size < 32 || fs->desc_size > 64 || (fs->desc_size & (fs->desc_size - 1)));
    if (bad_desc) fs->desc_size = 32;
    fs->gdt_blocks = (fs->num_groups * fs->desc_size + fs->block_size - 1) / fs->block_size;
    fs->journal_first_block = sb->s_journal_first_block;
    fs->journal_blocks = sb->s_journal_blocks;
    fs->reserved_gdt = (sb->s_feature_compat & 0x10 /* resize_inode */) ? rd16(raw + 0xCE) : 0;
    memcpy(fs->uuid, raw + 0x68, 16);
    fs->csum_md = (sb->s_feature_ro_compat & 0x400) != 0;
    fs->csum_gdt = !fs->csum_md && (sb->s_feature_ro_compat & 0x10) != 0;
    fs->lazy = fs->csum_md || fs->csum_gdt;
    fs->csum_seed = (sb->s_feature_incompat & 0x2000 /* csum_seed */) ? rd32(raw + 0x270)
                                                                       : crc32c_update(0xFFFFFFFFu, raw + 0x68, 16);
    fs->gen_counter = rd32(raw + 0x68) ^ sb->s_wtime;
    uint32_t blocks_hi = (sb->s_feature_incompat & 0x80) ? rd32(raw + 0x150) : 0;
    int bad_csum = fs->csum_md && (raw[0x175] != 1 /* crc32c */ || rd32(raw + 0x3FC) != crc32c_update(0xFFFFFFFFu, raw, 0x3FC));

    /* s_journal_first_block/s_journal_blocks are not fields of the real
     * ext4 superblock (those bytes hold other data), and the log written
     * here is not jbd2. Only volumes made by ext4_format() (exactly its
     * feature set) may use it; any other volume is written in place. */
    fs->use_journal = (sb->s_feature_compat == 0 &&
                       sb->s_feature_incompat == (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS) &&
                       sb->s_feature_ro_compat == 0 &&
                       sb->s_journal_blocks >= 8 && sb->s_journal_first_block != 0 &&
                       (uint64_t)sb->s_journal_first_block + sb->s_journal_blocks <= sb->s_blocks_count);

    /* Writing is allowed for every feature whose on-disk structures this
     * driver keeps up to date (see the checksum, lazy-init and extent code
     * above). Anything else (inline data, bigalloc, quota, meta_bg, ...)
     * and a journal that still has to be replayed make the volume read-only,
     * as does a block count that does not fit in 32 bits. */
    uint32_t compat_ok = 0x1 /* dir_prealloc */ | 0x4 /* has_journal */ | 0x8 /* ext_attr */ | 0x10 /* resize_inode */ | 0x20 /* dir_index */;
    uint32_t incompat_ok = EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS | 0x80 /* 64bit */ |
                           0x200 /* flex_bg */ | 0x2000 /* csum_seed */ | 0x4000 /* largedir */;
    uint32_t ro_ok = 0x1 /* sparse_super */ | 0x2 /* large_file */ | 0x8 /* huge_file */ | 0x10 /* gdt_csum */ |
                     0x20 /* dir_nlink */ | 0x40 /* extra_isize */ | 0x400 /* metadata_csum */;
    fs->ro = ((sb->s_feature_compat & ~compat_ok) != 0) || ((sb->s_feature_incompat & ~incompat_ok) != 0) ||
             ((sb->s_feature_ro_compat & ~ro_ok) != 0) ||
             (sb->s_feature_incompat & 0x4 /* needs_recovery */) != 0 || blocks_hi != 0 || bad_desc || bad_csum;
    if (fs->ro) klog_write("ext4: mounted read-only (volume uses features the driver cannot maintain)\n");

    free(sb);

    fs->in_txn = 0;
    fs->txn_count = 0;
    return 0;
}

int ext4_probe_and_mount(ext4_t *fs, blockdev_t *bd)
{
    if (ext4_probe(fs, bd) != 0) return -1;
    if (fs->use_journal) ext4_journal_recover(fs);
    return 0;
}

int ext4_umount(ext4_t *fs)
{
    (void)fs;
    return 0;
}

/* ---------- VFS callbacks ---------- */

static int ext4_vfs_open(void *ctx, const char *path, int flags)
{
    ext4_t *fs = (ext4_t *)ctx;
    if (fs->ro && (flags & (VFS_WRONLY | VFS_RDWR | VFS_CREAT | VFS_TRUNC | VFS_APPEND))) return -1;
    int do_txn = (flags & (VFS_CREAT | VFS_TRUNC)) != 0;
    if (do_txn) ext4_txn_begin(fs);

    char parent_path[256];
    char name[EXT4_MAX_FILENAME + 1];
    if (ext4_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) {
        if (do_txn) ext4_txn_commit(fs);
        return -1;
    }

    uint32_t parent_ino;
    if (ext4_walk(fs, parent_path, &parent_ino) != 0) {
        if (do_txn) ext4_txn_commit(fs);
        return -1;
    }

    ext4_dirent_loc_t loc;
    int found = (ext4_dir_find(fs, parent_ino, name, &loc) == 0);
    uint32_t ino;

    if (!found) {
        if (!(flags & VFS_CREAT)) { if (do_txn) ext4_txn_commit(fs); return -1; }

        ino = ext4_alloc_inode(fs, 0);
        if (ino == 0) { if (do_txn) ext4_txn_commit(fs); return -1; }

        ext4_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = EXT4_S_IFREG | 0644;
        inode.i_links_count = 1;
        inode.i_flags = EXT4_EXTENTS_FL;
        ext4_ext_init_inode(&inode);
        ext4_write_inode_new(fs, ino, &inode);

        if (ext4_dir_add_entry(fs, parent_ino, name, ino, EXT4_FT_REG_FILE) != 0) {
            ext4_free_inode(fs, ino, 0);
            if (do_txn) ext4_txn_commit(fs);
            return -1;
        }
    } else {
        if (loc.file_type == EXT4_FT_DIR) { if (do_txn) ext4_txn_commit(fs); return -1; }
        ino = loc.ino;
        if (flags & VFS_TRUNC) {
            ext4_inode_t inode;
            if (ext4_read_inode(fs, ino, &inode) == 0) {
                ext4_free_inode_blocks(fs, &inode);
                inode.i_size = 0;
                ext4_write_inode(fs, ino, &inode);
            }
        }
    }

    if (do_txn) ext4_txn_commit(fs);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            uint8_t raw[128];
            if (ext4_read_inode_raw(fs, ino, raw) != 0) return -1;
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].is_dir = 0;
            fs->fds[i].dirty = 0;
            memcpy(fs->fds[i].inode_raw, raw, 128);
            ext4_inode_t *inp = (ext4_inode_t *)fs->fds[i].inode_raw;
            fs->fds[i].size = inp->i_size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? inp->i_size : 0;
            return i;
        }
    }
    return -1;
}

static int ext4_vfs_close(void *ctx, int fd)
{
    ext4_t *fs = (ext4_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (fs->fds[fd].dirty) {
        ext4_txn_begin(fs);
        ext4_inode_t *inp = (ext4_inode_t *)fs->fds[fd].inode_raw;
        inp->i_size = fs->fds[fd].size;
        ext4_write_inode_raw(fs, fs->fds[fd].ino, fs->fds[fd].inode_raw);
        ext4_txn_commit(fs);
    }

    fs->fds[fd].used = 0;
    return 0;
}

static int ext4_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    ext4_t *fs = (ext4_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    ext4_inode_t *inp = (ext4_inode_t *)fs->fds[fd].inode_raw;
    int n = ext4_read_data(fs, fs->fds[fd].ino, inp, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += (uint32_t)n;
    return n;
}

static int ext4_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    if (((ext4_t *)ctx)->ro) return -1;
    ext4_t *fs = (ext4_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    ext4_txn_begin(fs);
    ext4_inode_t *inp = (ext4_inode_t *)fs->fds[fd].inode_raw;
    int n = ext4_write_data(fs, fs->fds[fd].ino, inp, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += (uint32_t)n;
        fs->fds[fd].size = inp->i_size;
        fs->fds[fd].dirty = 1;
    }
    ext4_txn_commit(fs);
    return n;
}

static int ext4_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    ext4_t *fs = (ext4_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int ext4_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    ext4_t *fs = (ext4_t *)ctx;

    uint32_t dir_ino;
    if (ext4_walk(fs, path, &dir_ino) != 0) return -1;

    ext4_inode_t dir;
    if (ext4_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int count = 0;
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks && count < max; bi++) {
        uint32_t blk = ext4_bmap(fs, dir_ino, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext4_cached_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size && count < max) {
            if (!ext4_dirent_ok(buf, pos, fs->block_size)) break;
            ext4_dirent_hdr_t *hdr = (ext4_dirent_hdr_t *)(buf + pos);

            if (hdr->ino != 0) {
                int is_dot = (hdr->name_len == 1 && buf[pos + 8] == '.');
                int is_dotdot = (hdr->name_len == 2 && buf[pos + 8] == '.' && buf[pos + 9] == '.');
                if (!is_dot && !is_dotdot) {
                    int nl = hdr->name_len;
                    if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
                    memcpy(entries[count].name, buf + pos + 8, (size_t)nl);
                    entries[count].name[nl] = 0;

                    ext4_inode_t child;
                    ext4_read_inode(fs, hdr->ino, &child);
                    entries[count].size = child.i_size;
                    entries[count].is_dir = (hdr->file_type == EXT4_FT_DIR);
                    entries[count].inode = hdr->ino;
                    entries[count].mode = child.i_mode;
                    count++;
                }
            }
            pos += hdr->rec_len;
        }
    }

    free(buf);
    return count;
}

static int ext4_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    if (((ext4_t *)ctx)->ro) return -1;
    (void)mode;
    ext4_t *fs = (ext4_t *)ctx;
    ext4_txn_begin(fs);

    char parent_path[256];
    char name[EXT4_MAX_FILENAME + 1];
    if (ext4_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) { ext4_txn_commit(fs); return -1; }
    if (name[0] == 0) { ext4_txn_commit(fs); return -1; }

    uint32_t parent_ino;
    if (ext4_walk(fs, parent_path, &parent_ino) != 0) { ext4_txn_commit(fs); return -1; }

    ext4_dirent_loc_t existing;
    if (ext4_dir_find(fs, parent_ino, name, &existing) == 0) { ext4_txn_commit(fs); return -1; }

    uint32_t new_ino = ext4_alloc_inode(fs, 1);
    if (new_ino == 0) { ext4_txn_commit(fs); return -1; }

    uint32_t data_blk = ext4_alloc_block(fs);
    if (data_blk == 0) { ext4_free_inode(fs, new_ino, 1); ext4_txn_commit(fs); return -1; }

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) { ext4_free_block(fs, data_blk); ext4_free_inode(fs, new_ino, 1); ext4_txn_commit(fs); return -1; }
    memset(buf, 0, fs->block_size);

    ext4_dirent_hdr_t *dot = (ext4_dirent_hdr_t *)buf;
    dot->ino = new_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    buf[8] = '.';

    ext4_dirent_hdr_t *dotdot = (ext4_dirent_hdr_t *)(buf + 12);
    dotdot->ino = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->block_size - 12 - (fs->csum_md ? 12 : 0));
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    buf[12 + 8] = '.';
    buf[12 + 9] = '.';
    if (fs->csum_md) ext4_dir_put_tail(fs, buf);

    ext4_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT4_S_IFDIR | 0755;
    inode.i_links_count = 2;
    inode.i_size = fs->block_size;
    inode.i_flags = EXT4_EXTENTS_FL;
    ext4_ext_init_inode(&inode);
    {
        ext4_extent_header_t *h = ext4_root_hdr(&inode);
        ext4_extent_t *e = (ext4_extent_t *)((uint8_t *)inode.i_block + sizeof(ext4_extent_header_t));
        e[0].ee_block = 0; e[0].ee_len = 1; e[0].ee_start_hi = 0; e[0].ee_start_lo = data_blk;
        h->entries = 1;
    }
    inode.i_blocks = fs->block_size / 512;
    ext4_write_inode_new(fs, new_ino, &inode);   /* assigns i_generation, which the block checksum needs */

    ext4_dir_write_block(fs, ext4_iseed(fs, new_ino, &inode), data_blk, buf);
    free(buf);

    if (ext4_dir_add_entry(fs, parent_ino, name, new_ino, EXT4_FT_DIR) != 0) {
        ext4_free_block(fs, data_blk);
        ext4_free_inode(fs, new_ino, 1);
        ext4_txn_commit(fs);
        return -1;
    }

    ext4_inode_t parent;
    if (ext4_read_inode(fs, parent_ino, &parent) == 0) {
        parent.i_links_count++;
        ext4_write_inode(fs, parent_ino, &parent);
    }

    ext4_txn_commit(fs);
    return 0;
}

/* Removes directory entry `name` of `parent_ino` and releases the inode when
 * its last link goes. Caller holds the transaction. */
static int ext4_unlink_at(ext4_t *fs, uint32_t parent_ino, const char *name)
{
    ext4_dirent_loc_t loc;
    if (ext4_dir_find(fs, parent_ino, name, &loc) != 0) return -1;

    ext4_inode_t inode;
    if (ext4_read_inode(fs, loc.ino, &inode) != 0) return -1;

    if (loc.file_type == EXT4_FT_DIR) {
        if (!ext4_dir_is_empty(fs, loc.ino)) return -1;
        if (ext4_dir_remove_entry(fs, parent_ino, name) != 0) return -1;

        ext4_free_inode_blocks(fs, &inode);
        memset(&inode, 0, sizeof(inode));
        ext4_write_inode(fs, loc.ino, &inode);
        ext4_free_inode(fs, loc.ino, 1);

        ext4_inode_t parent;
        if (ext4_read_inode(fs, parent_ino, &parent) == 0 && parent.i_links_count > 0) {
            parent.i_links_count--;
            ext4_write_inode(fs, parent_ino, &parent);
        }
        return 0;
    }

    if (ext4_dir_remove_entry(fs, parent_ino, name) != 0) return -1;

    if (inode.i_links_count > 0) inode.i_links_count--;
    if (inode.i_links_count == 0) {
        ext4_free_inode_blocks(fs, &inode);
        memset(&inode, 0, sizeof(inode));
        ext4_write_inode(fs, loc.ino, &inode);
        ext4_free_inode(fs, loc.ino, 0);
    } else {
        ext4_write_inode(fs, loc.ino, &inode);
    }
    return 0;
}

static int ext4_vfs_unlink(void *ctx, const char *path)
{
    if (((ext4_t *)ctx)->ro) return -1;
    ext4_t *fs = (ext4_t *)ctx;
    ext4_txn_begin(fs);

    char parent_path[256];
    char name[EXT4_MAX_FILENAME + 1];
    if (ext4_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) { ext4_txn_commit(fs); return -1; }

    uint32_t parent_ino;
    if (ext4_walk(fs, parent_path, &parent_ino) != 0) { ext4_txn_commit(fs); return -1; }

    int rc = ext4_unlink_at(fs, parent_ino, name);
    ext4_txn_commit(fs);
    return rc;
}

static int ext4_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    ext4_t *fs = (ext4_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;

    uint32_t ino;
    char name[EXT4_MAX_FILENAME + 1];
    int file_type;

    if (!*p) {
        ino = EXT4_ROOT_INO;
        name[0] = 0;
        file_type = EXT4_FT_DIR;
    } else {
        char parent_path[256];
        if (ext4_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

        uint32_t parent_ino;
        if (ext4_walk(fs, parent_path, &parent_ino) != 0) return -1;

        ext4_dirent_loc_t loc;
        if (ext4_dir_find(fs, parent_ino, name, &loc) != 0) return -1;
        ino = loc.ino;
        file_type = loc.file_type;
    }

    ext4_inode_t inode;
    if (ext4_read_inode(fs, ino, &inode) != 0) return -1;

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = inode.i_size;
    entry->is_dir = (file_type == EXT4_FT_DIR);
    entry->inode = ino;
    entry->mode = inode.i_mode;
    return 0;
}

static int ext4_vfs_rename(void *ctx, const char *old, const char *new)
{
    if (((ext4_t *)ctx)->ro) return -1;
    ext4_t *fs = (ext4_t *)ctx;
    ext4_txn_begin(fs);

    char old_parent_path[256], old_name[EXT4_MAX_FILENAME + 1];
    if (ext4_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) { ext4_txn_commit(fs); return -1; }
    uint32_t old_parent;
    if (ext4_walk(fs, old_parent_path, &old_parent) != 0) { ext4_txn_commit(fs); return -1; }

    ext4_dirent_loc_t loc;
    if (old_name[0] == 0 || ext4_dir_find(fs, old_parent, old_name, &loc) != 0) { ext4_txn_commit(fs); return -1; }

    char new_parent_path[256], new_name[EXT4_MAX_FILENAME + 1];
    if (ext4_split_path(new, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) { ext4_txn_commit(fs); return -1; }
    uint32_t new_parent;
    if (new_name[0] == 0 || ext4_walk(fs, new_parent_path, &new_parent) != 0) { ext4_txn_commit(fs); return -1; }

    /* a directory must not become its own descendant */
    if (loc.file_type == EXT4_FT_DIR) {
        uint32_t cur = new_parent;
        for (int guard = 0; guard < 256 && cur != EXT4_ROOT_INO; guard++) {
            if (cur == loc.ino) { ext4_txn_commit(fs); return -1; }
            ext4_dirent_loc_t up;
            if (ext4_dir_find(fs, cur, "..", &up) != 0) break;
            cur = up.ino;
        }
        if (cur == loc.ino) { ext4_txn_commit(fs); return -1; }
    }

    /* an existing target is replaced (files over files, empty dirs over dirs) */
    ext4_dirent_loc_t dst;
    if (ext4_dir_find(fs, new_parent, new_name, &dst) == 0) {
        if (dst.ino == loc.ino) { ext4_txn_commit(fs); return 0; }
        if ((dst.file_type == EXT4_FT_DIR) != (loc.file_type == EXT4_FT_DIR)) { ext4_txn_commit(fs); return -1; }
        if (ext4_unlink_at(fs, new_parent, new_name) != 0) { ext4_txn_commit(fs); return -1; }
    }

    if (ext4_dir_add_entry(fs, new_parent, new_name, loc.ino, loc.file_type) != 0) { ext4_txn_commit(fs); return -1; }
    ext4_dir_remove_entry(fs, old_parent, old_name);

    if (loc.file_type == EXT4_FT_DIR && old_parent != new_parent) {
        ext4_inode_t moved;
        if (ext4_read_inode(fs, loc.ino, &moved) == 0) {
            uint32_t blk = ext4_bmap(fs, loc.ino, &moved, 0, 0);
            if (blk != 0) {
                uint8_t *buf = (uint8_t *)malloc(fs->block_size);
                if (buf) {
                    if (ext4_cached_read_block(fs, blk, buf) == 0 && ext4_dirent_ok(buf, 0, fs->block_size)) {
                        uint16_t dl = rd16(buf + 4);
                        if (ext4_dirent_ok(buf, dl, fs->block_size) && buf[dl + 6] == 2) {
                            wr32(buf + dl, new_parent);
                            ext4_dir_write_block(fs, ext4_iseed(fs, loc.ino, &moved), blk, buf);
                        }
                    }
                    free(buf);
                }
            }
        }

        ext4_inode_t op, np;
        if (ext4_read_inode(fs, old_parent, &op) == 0 && op.i_links_count > 0) {
            op.i_links_count--;
            ext4_write_inode(fs, old_parent, &op);
        }
        if (ext4_read_inode(fs, new_parent, &np) == 0) {
            np.i_links_count++;
            ext4_write_inode(fs, new_parent, &np);
        }
    }

    ext4_txn_commit(fs);
    return 0;
}

static int ext4_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void ext4_mount_vfs(ext4_t *fs, const char *mount_point)
{
    static vfs_ops_t ext4_vfs_ops = {
        .open = ext4_vfs_open,
        .close = ext4_vfs_close,
        .read = ext4_vfs_read,
        .write = ext4_vfs_write,
        .lseek = ext4_vfs_lseek,
        .readdir = ext4_vfs_readdir,
        .mkdir = ext4_vfs_mkdir,
        .unlink = ext4_vfs_unlink,
        .stat = ext4_vfs_stat,
        .rename = ext4_vfs_rename,
        .symlink = ext4_vfs_symlink,
    };
    vfs_mount(mount_point, &ext4_vfs_ops, fs);
}

/* ---------- format ---------- */

int ext4_format(blockdev_t *bd, const char *label)
{
    uint32_t block_size = 1024;
    uint32_t sector_size = bd->sector_size ? bd->sector_size : 512;
    uint64_t total_bytes = bd->total_sectors * sector_size;
    uint32_t total_blocks = (uint32_t)(total_bytes / block_size);

    /* reserve a contiguous journal region at the very end of the disk */
    uint32_t journal_blocks = total_blocks / 32;
    if (journal_blocks < 130) journal_blocks = 130;
    if (journal_blocks > 1024) journal_blocks = 1024;
    if (journal_blocks > total_blocks / 4) journal_blocks = total_blocks / 4;
    uint32_t journal_first_block = total_blocks - journal_blocks;

    uint32_t usable_blocks = journal_first_block;

    uint32_t first_data_block = 1;
    uint32_t blocks_per_group = 8192;
    uint32_t num_groups = (usable_blocks - first_data_block + blocks_per_group - 1) / blocks_per_group;
    if (num_groups < 1) num_groups = 1;

    uint32_t gdt_blocks = (num_groups * sizeof(ext4_group_desc_t) + block_size - 1) / block_size;

    uint32_t inodes_per_group = (usable_blocks / 4) / num_groups;
    inodes_per_group = (inodes_per_group / 8) * 8;
    if (inodes_per_group < 32) inodes_per_group = 32;
    uint32_t inode_table_blocks = inodes_per_group / 8;

    uint32_t inodes_count = inodes_per_group * num_groups;

    uint32_t *group_base = (uint32_t *)malloc(num_groups * sizeof(uint32_t));
    uint32_t *group_blocks = (uint32_t *)malloc(num_groups * sizeof(uint32_t));
    uint32_t *group_block_bitmap = (uint32_t *)malloc(num_groups * sizeof(uint32_t));
    uint32_t *group_inode_bitmap = (uint32_t *)malloc(num_groups * sizeof(uint32_t));
    uint32_t *group_inode_table = (uint32_t *)malloc(num_groups * sizeof(uint32_t));
    if (!group_base || !group_blocks || !group_block_bitmap || !group_inode_bitmap || !group_inode_table) {
        free(group_base); free(group_blocks); free(group_block_bitmap);
        free(group_inode_bitmap); free(group_inode_table);
        return -1;
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t base = first_data_block + g * blocks_per_group;
        group_base[g] = base;
        uint32_t blocks_in_group = usable_blocks - base;
        if (blocks_in_group > blocks_per_group) blocks_in_group = blocks_per_group;
        group_blocks[g] = blocks_in_group;

        uint32_t cur = base;
        if (g == 0) cur += 1 + gdt_blocks;
        group_block_bitmap[g] = cur++;
        group_inode_bitmap[g] = cur++;
        group_inode_table[g] = cur;
        cur += inode_table_blocks;
    }

    uint32_t free_blocks_total = 0;
    uint32_t free_inodes_total = inodes_count - 10;

    uint8_t *zbuf = (uint8_t *)malloc(block_size);
    if (!zbuf) {
        free(group_base); free(group_blocks); free(group_block_bitmap);
        free(group_inode_bitmap); free(group_inode_table);
        return -1;
    }
    memset(zbuf, 0, block_size);

    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t meta_start = group_base[g];
        uint32_t meta_blocks = (g == 0)
            ? (1 + gdt_blocks + 2 + inode_table_blocks)
            : (2 + inode_table_blocks);
        for (uint32_t b = 0; b < meta_blocks; b++) {
            blockdev_write_bytes(bd, (uint64_t)(meta_start + b) * block_size, block_size, zbuf);
        }
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t blocks_in_group = group_blocks[g];
        uint32_t meta_bit_count = (g == 0)
            ? (1 + gdt_blocks + 2 + inode_table_blocks)
            : (2 + inode_table_blocks);

        uint8_t *bitmap = (uint8_t *)malloc(block_size);
        memset(bitmap, 0, block_size);

        for (uint32_t bit = 0; bit < meta_bit_count; bit++) {
            bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        for (uint32_t bit = blocks_in_group; bit < block_size * 8; bit++) {
            bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }

        uint32_t free_in_group = blocks_in_group - meta_bit_count;
        free_blocks_total += free_in_group;

        blockdev_write_bytes(bd, (uint64_t)group_block_bitmap[g] * block_size, block_size, bitmap);
        free(bitmap);
    }

    uint32_t root_data_block = group_inode_table[0] + inode_table_blocks;
    free_blocks_total -= 1;
    {
        uint8_t *bitmap = (uint8_t *)malloc(block_size);
        blockdev_read_bytes(bd, (uint64_t)group_block_bitmap[0] * block_size, block_size, bitmap);
        uint32_t bit = root_data_block - group_base[0];
        bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        blockdev_write_bytes(bd, (uint64_t)group_block_bitmap[0] * block_size, block_size, bitmap);
        free(bitmap);
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        uint8_t *bitmap = (uint8_t *)malloc(block_size);
        memset(bitmap, 0, block_size);

        if (g == 0) {
            for (uint32_t bit = 0; bit < 10; bit++) {
                bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
            }
        }
        for (uint32_t bit = inodes_per_group; bit < block_size * 8; bit++) {
            bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }

        blockdev_write_bytes(bd, (uint64_t)group_inode_bitmap[g] * block_size, block_size, bitmap);
        free(bitmap);
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        for (uint32_t b = 0; b < inode_table_blocks; b++) {
            blockdev_write_bytes(bd, (uint64_t)(group_inode_table[g] + b) * block_size, block_size, zbuf);
        }
    }

    {
        uint8_t *buf = (uint8_t *)malloc(block_size);
        memset(buf, 0, block_size);

        ext4_dirent_hdr_t *dot = (ext4_dirent_hdr_t *)buf;
        dot->ino = EXT4_ROOT_INO;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = EXT4_FT_DIR;
        buf[8] = '.';

        ext4_dirent_hdr_t *dotdot = (ext4_dirent_hdr_t *)(buf + 12);
        dotdot->ino = EXT4_ROOT_INO;
        dotdot->rec_len = (uint16_t)(block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT4_FT_DIR;
        buf[12 + 8] = '.';
        buf[12 + 9] = '.';

        blockdev_write_bytes(bd, (uint64_t)root_data_block * block_size, block_size, buf);
        free(buf);
    }

    {
        uint64_t off = (uint64_t)group_inode_table[0] * block_size + (uint64_t)(EXT4_ROOT_INO - 1) * 128;
        ext4_inode_t root;
        memset(&root, 0, sizeof(root));
        root.i_mode = EXT4_S_IFDIR | 0755;
        root.i_links_count = 2;
        root.i_size = block_size;
        root.i_flags = EXT4_EXTENTS_FL;
        ext4_ext_init_inode(&root);
        {
            ext4_extent_header_t *h = ext4_root_hdr(&root);
            ext4_extent_t *e = (ext4_extent_t *)((uint8_t *)root.i_block + sizeof(ext4_extent_header_t));
            e[0].ee_block = 0; e[0].ee_len = 1; e[0].ee_start_hi = 0; e[0].ee_start_lo = root_data_block;
            h->entries = 1;
        }
        root.i_blocks = block_size / 512;
        blockdev_write_bytes(bd, off, sizeof(root), &root);
    }

    free(zbuf);

    {
        uint8_t *gdt_buf = (uint8_t *)malloc(gdt_blocks * block_size);
        memset(gdt_buf, 0, gdt_blocks * block_size);
        ext4_group_desc_t *gds = (ext4_group_desc_t *)gdt_buf;

        for (uint32_t g = 0; g < num_groups; g++) {
            uint32_t meta_bit_count = (g == 0)
                ? (1 + gdt_blocks + 2 + inode_table_blocks)
                : (2 + inode_table_blocks);
            uint32_t free_in_group = group_blocks[g] - meta_bit_count;
            if (g == 0) free_in_group -= 1;

            gds[g].bg_block_bitmap = group_block_bitmap[g];
            gds[g].bg_inode_bitmap = group_inode_bitmap[g];
            gds[g].bg_inode_table = group_inode_table[g];
            gds[g].bg_free_blocks_count = (uint16_t)free_in_group;
            gds[g].bg_free_inodes_count = (uint16_t)((g == 0) ? (inodes_per_group - 10) : inodes_per_group);
            gds[g].bg_used_dirs_count = (uint16_t)((g == 0) ? 1 : 0);
        }

        blockdev_write_bytes(bd, (uint64_t)(first_data_block + 1) * block_size, gdt_blocks * block_size, gdt_buf);
        free(gdt_buf);
    }

    /* initialize the journal region: block 0 = journal superblock, rest zeroed */
    {
        uint8_t *jbuf = (uint8_t *)malloc(block_size);
        memset(jbuf, 0, block_size);
        ext4_journal_super_t *jsb = (ext4_journal_super_t *)jbuf;
        jsb->magic = EXT4_JOURNAL_MAGIC;
        jsb->block_size = block_size;
        jsb->maxlen = journal_blocks;
        jsb->s_committed_seq = 1;
        blockdev_write_bytes(bd, (uint64_t)journal_first_block * block_size, block_size, jbuf);

        memset(jbuf, 0, block_size);
        for (uint32_t b = 1; b < journal_blocks; b++) {
            blockdev_write_bytes(bd, (uint64_t)(journal_first_block + b) * block_size, block_size, jbuf);
        }
        free(jbuf);
    }

    {
        ext4_superblock_t *sb = (ext4_superblock_t *)malloc(1024);
        memset(sb, 0, 1024);

        sb->s_inodes_count = inodes_count;
        sb->s_blocks_count = total_blocks;
        sb->s_r_blocks_count = 0;
        sb->s_free_blocks_count = free_blocks_total;
        sb->s_free_inodes_count = free_inodes_total;
        sb->s_first_data_block = first_data_block;
        sb->s_log_block_size = 0;
        sb->s_log_frag_size = 0;
        sb->s_blocks_per_group = blocks_per_group;
        sb->s_frags_per_group = blocks_per_group;
        sb->s_inodes_per_group = inodes_per_group;
        sb->s_mtime = 0;
        sb->s_wtime = 0;
        sb->s_mnt_count = 0;
        sb->s_max_mnt_count = 0xFFFF;
        sb->s_magic = EXT4_SUPER_MAGIC;
        sb->s_state = 1;
        sb->s_errors = 1;
        sb->s_minor_rev_level = 0;
        sb->s_lastcheck = 0;
        sb->s_checkinterval = 0;
        sb->s_creator_os = 0;
        sb->s_rev_level = EXT4_DYNAMIC_REV;
        sb->s_def_resuid = 0;
        sb->s_def_resgid = 0;
        sb->s_first_ino = EXT4_FIRST_NON_RESERVED_INO;
        sb->s_inode_size = 128;
        sb->s_block_group_nr = 0;
        sb->s_feature_compat = 0;
        sb->s_feature_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS;
        sb->s_feature_ro_compat = 0;
        sb->s_journal_first_block = journal_first_block;
        sb->s_journal_blocks = journal_blocks;

        if (label) {
            int i = 0;
            while (label[i] && i < 15) { sb->s_volume_name[i] = label[i]; i++; }
            sb->s_volume_name[i] = 0;
        }

        int ret = blockdev_write_bytes(bd, 1024, 1024, sb);
        free(sb);

        free(group_base); free(group_blocks); free(group_block_bitmap);
        free(group_inode_bitmap); free(group_inode_table);

        if (ret != 0) return -1;
    }

    return 0;
}
