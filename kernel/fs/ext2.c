#include "ext2.h"
#include "memory.h"
#include "string.h"

#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_GOOD_OLD_REV 0
#define EXT2_DYNAMIC_REV  1
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x2

#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFDIR 0x4000

#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

#define EXT2_ROOT_INO 2
#define EXT2_FIRST_NON_RESERVED_INO 11

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
    uint8_t  s_padding[820];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

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
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed)) ext2_dirent_hdr_t;

static int ext2_read_block(ext2_t *fs, uint32_t block, void *buf)
{
    if (block == 0) return -1;
    return blockdev_read_bytes(fs->bd, (uint64_t)block * fs->block_size, fs->block_size, buf);
}

static int ext2_write_block(ext2_t *fs, uint32_t block, const void *buf)
{
    if (block == 0) return -1;
    return blockdev_write_bytes(fs->bd, (uint64_t)block * fs->block_size, fs->block_size, buf);
}

static uint32_t ext2_zalloc_block_raw(ext2_t *fs, uint32_t block)
{
    uint8_t *zbuf = (uint8_t *)malloc(fs->block_size);
    if (!zbuf) return 0;
    memset(zbuf, 0, fs->block_size);
    ext2_write_block(fs, block, zbuf);
    free(zbuf);
    return block;
}

static int ext2_read_group_desc(ext2_t *fs, uint32_t group, ext2_group_desc_t *out)
{
    uint64_t off = (uint64_t)fs->gdt_block * fs->block_size + (uint64_t)group * sizeof(ext2_group_desc_t);
    return blockdev_read_bytes(fs->bd, off, sizeof(ext2_group_desc_t), out);
}

static int ext2_write_group_desc(ext2_t *fs, uint32_t group, const ext2_group_desc_t *in)
{
    uint64_t off = (uint64_t)fs->gdt_block * fs->block_size + (uint64_t)group * sizeof(ext2_group_desc_t);
    return blockdev_write_bytes(fs->bd, off, sizeof(ext2_group_desc_t), in);
}

static uint64_t ext2_inode_offset(ext2_t *fs, uint32_t ino, ext2_group_desc_t *gd_out, uint32_t *group_out)
{
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_group_desc(fs, group, &gd) != 0) return 0;
    if (gd_out) *gd_out = gd;
    if (group_out) *group_out = group;
    return (uint64_t)gd.bg_inode_table * fs->block_size + (uint64_t)index * fs->inode_size;
}

static int ext2_read_inode_raw(ext2_t *fs, uint32_t ino, uint8_t *raw128)
{
    uint64_t off = ext2_inode_offset(fs, ino, 0, 0);
    if (off == 0) return -1;
    return blockdev_read_bytes(fs->bd, off, 128, raw128);
}

static int ext2_write_inode_raw(ext2_t *fs, uint32_t ino, const uint8_t *raw128)
{
    uint64_t off = ext2_inode_offset(fs, ino, 0, 0);
    if (off == 0) return -1;
    return blockdev_write_bytes(fs->bd, off, 128, raw128);
}

static int ext2_bitmap_test(ext2_t *fs, uint32_t bitmap_block, uint32_t bit)
{
    uint32_t byte_off = bit / 8;
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    uint8_t byte;
    blockdev_read_bytes(fs->bd, (uint64_t)bitmap_block * fs->block_size + byte_off, 1, &byte);
    return (byte & mask) ? 1 : 0;
}

static void ext2_bitmap_set(ext2_t *fs, uint32_t bitmap_block, uint32_t bit, int used)
{
    uint32_t byte_off = bit / 8;
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    uint8_t byte;
    uint64_t off = (uint64_t)bitmap_block * fs->block_size + byte_off;
    blockdev_read_bytes(fs->bd, off, 1, &byte);
    if (used) byte |= mask;
    else byte &= (uint8_t)~mask;
    blockdev_write_bytes(fs->bd, off, 1, &byte);
}

static uint32_t ext2_alloc_block(ext2_t *fs)
{
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_group_desc(fs, g, &gd) != 0) continue;
        if (gd.bg_free_blocks_count == 0) continue;

        uint32_t base = fs->first_data_block + g * fs->blocks_per_group;
        uint32_t blocks_in_group = fs->blocks_count - base;
        if (blocks_in_group > fs->blocks_per_group) blocks_in_group = fs->blocks_per_group;

        for (uint32_t bit = 0; bit < blocks_in_group; bit++) {
            if (!ext2_bitmap_test(fs, gd.bg_block_bitmap, bit)) {
                ext2_bitmap_set(fs, gd.bg_block_bitmap, bit, 1);
                gd.bg_free_blocks_count--;
                ext2_write_group_desc(fs, g, &gd);
                uint32_t block = base + bit;
                ext2_zalloc_block_raw(fs, block);
                return block;
            }
        }
    }
    return 0;
}

static void ext2_free_block(ext2_t *fs, uint32_t block)
{
    if (block == 0) return;
    uint32_t group = (block - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_group_desc(fs, group, &gd) != 0) return;
    ext2_bitmap_set(fs, gd.bg_block_bitmap, bit, 0);
    gd.bg_free_blocks_count++;
    ext2_write_group_desc(fs, group, &gd);
}

static uint32_t ext2_alloc_inode(ext2_t *fs, int is_dir)
{
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_group_desc(fs, g, &gd) != 0) continue;
        if (gd.bg_free_inodes_count == 0) continue;

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            uint32_t ino = g * fs->inodes_per_group + bit + 1;
            if (ino < EXT2_FIRST_NON_RESERVED_INO) continue;
            if (!ext2_bitmap_test(fs, gd.bg_inode_bitmap, bit)) {
                ext2_bitmap_set(fs, gd.bg_inode_bitmap, bit, 1);
                gd.bg_free_inodes_count--;
                if (is_dir) gd.bg_used_dirs_count++;
                ext2_write_group_desc(fs, g, &gd);
                return ino;
            }
        }
    }
    return 0;
}

static void ext2_free_inode(ext2_t *fs, uint32_t ino, int is_dir)
{
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_group_desc(fs, group, &gd) != 0) return;
    ext2_bitmap_set(fs, gd.bg_inode_bitmap, bit, 0);
    gd.bg_free_inodes_count++;
    if (is_dir && gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
    ext2_write_group_desc(fs, group, &gd);
}

static uint32_t ext2_ptr_at(ext2_t *fs, uint32_t index_block, uint32_t slot, int alloc, ext2_inode_t *inode)
{
    uint32_t ppb = fs->block_size / 4;
    if (slot >= ppb) return 0;

    uint32_t *buf = (uint32_t *)malloc(fs->block_size);
    if (!buf) return 0;
    if (ext2_read_block(fs, index_block, buf) != 0) { free(buf); return 0; }

    uint32_t val = buf[slot];
    if (val == 0 && alloc) {
        val = ext2_alloc_block(fs);
        if (val != 0) {
            buf[slot] = val;
            ext2_write_block(fs, index_block, buf);
            if (inode) inode->i_blocks += fs->block_size / 512;
        }
    }
    free(buf);
    return val;
}

static uint32_t ext2_bmap(ext2_t *fs, ext2_inode_t *inode, uint32_t index, int alloc)
{
    uint32_t ppb = fs->block_size / 4;

    if (index < 12) {
        if (inode->i_block[index] == 0 && alloc) {
            uint32_t nb = ext2_alloc_block(fs);
            if (nb == 0) return 0;
            inode->i_block[index] = nb;
            inode->i_blocks += fs->block_size / 512;
        }
        return inode->i_block[index];
    }
    index -= 12;

    if (index < ppb) {
        if (inode->i_block[12] == 0) {
            if (!alloc) return 0;
            uint32_t nb = ext2_alloc_block(fs);
            if (nb == 0) return 0;
            inode->i_block[12] = nb;
            inode->i_blocks += fs->block_size / 512;
        }
        return ext2_ptr_at(fs, inode->i_block[12], index, alloc, inode);
    }
    index -= ppb;

    if (index < ppb * ppb) {
        if (inode->i_block[13] == 0) {
            if (!alloc) return 0;
            uint32_t nb = ext2_alloc_block(fs);
            if (nb == 0) return 0;
            inode->i_block[13] = nb;
            inode->i_blocks += fs->block_size / 512;
        }
        uint32_t outer = index / ppb;
        uint32_t inner = index % ppb;
        uint32_t mid_block = ext2_ptr_at(fs, inode->i_block[13], outer, alloc, inode);
        if (mid_block == 0) return 0;
        return ext2_ptr_at(fs, mid_block, inner, alloc, inode);
    }
    index -= ppb * ppb;

    if (inode->i_block[14] == 0) {
        if (!alloc) return 0;
        uint32_t nb = ext2_alloc_block(fs);
        if (nb == 0) return 0;
        inode->i_block[14] = nb;
        inode->i_blocks += fs->block_size / 512;
    }
    uint32_t outer = index / (ppb * ppb);
    uint32_t rem = index % (ppb * ppb);
    uint32_t mid = rem / ppb;
    uint32_t inner = rem % ppb;
    uint32_t l1 = ext2_ptr_at(fs, inode->i_block[14], outer, alloc, inode);
    if (l1 == 0) return 0;
    uint32_t l2 = ext2_ptr_at(fs, l1, mid, alloc, inode);
    if (l2 == 0) return 0;
    return ext2_ptr_at(fs, l2, inner, alloc, inode);
}

static void ext2_free_indirect(ext2_t *fs, uint32_t block, int depth)
{
    if (block == 0) return;
    if (depth == 0) { ext2_free_block(fs, block); return; }

    uint32_t ppb = fs->block_size / 4;
    uint32_t *buf = (uint32_t *)malloc(fs->block_size);
    if (!buf) return;
    if (ext2_read_block(fs, block, buf) == 0) {
        for (uint32_t i = 0; i < ppb; i++) {
            if (buf[i] != 0) ext2_free_indirect(fs, buf[i], depth - 1);
        }
    }
    free(buf);
    ext2_free_block(fs, block);
}

static void ext2_free_inode_blocks(ext2_t *fs, ext2_inode_t *inode)
{
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i] != 0) ext2_free_block(fs, inode->i_block[i]);
    }
    ext2_free_indirect(fs, inode->i_block[12], 1);
    ext2_free_indirect(fs, inode->i_block[13], 2);
    ext2_free_indirect(fs, inode->i_block[14], 3);
    inode->i_block[12] = inode->i_block[13] = inode->i_block[14] = 0;
    inode->i_blocks = 0;
}

static int ext2_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
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

static int ext2_read_inode(ext2_t *fs, uint32_t ino, ext2_inode_t *out)
{
    uint8_t raw[128];
    if (ext2_read_inode_raw(fs, ino, raw) != 0) return -1;
    memcpy(out, raw, sizeof(ext2_inode_t));
    return 0;
}

static int ext2_write_inode(ext2_t *fs, uint32_t ino, const ext2_inode_t *in)
{
    return ext2_write_inode_raw(fs, ino, (const uint8_t *)in);
}

static int ext2_rec_len_min(int name_len)
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
} ext2_dirent_loc_t;

static int ext2_dir_find(ext2_t *fs, uint32_t dir_ino, const char *name, ext2_dirent_loc_t *out)
{
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext2_bmap(fs, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext2_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)(buf + pos);
            if (hdr->rec_len == 0) break;
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

static int ext2_dir_add_entry(ext2_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, int file_type)
{
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    uint16_t need = (uint16_t)ext2_rec_len_min(name_len);

    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext2_bmap(fs, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext2_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)(buf + pos);
            if (hdr->rec_len == 0) break;

            uint16_t used = hdr->ino != 0 ? (uint16_t)ext2_rec_len_min(hdr->name_len) : 0;
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
                    ext2_dirent_hdr_t *nh = (ext2_dirent_hdr_t *)(buf + new_pos);
                    nh->ino = ino;
                    nh->rec_len = (uint16_t)(old_rec_len - used);
                    nh->name_len = (uint8_t)name_len;
                    nh->file_type = (uint8_t)file_type;
                    memcpy(buf + new_pos + 8, name, (size_t)name_len);
                }
                ext2_write_block(fs, blk, buf);
                free(buf);
                return 0;
            }
            pos += hdr->rec_len;
        }
    }

    uint32_t new_blk = ext2_bmap(fs, &dir, nblocks, 1);
    if (new_blk == 0) { free(buf); return -1; }

    memset(buf, 0, fs->block_size);
    ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)buf;
    hdr->ino = ino;
    hdr->rec_len = (uint16_t)fs->block_size;
    hdr->name_len = (uint8_t)name_len;
    hdr->file_type = (uint8_t)file_type;
    memcpy(buf + 8, name, (size_t)name_len);
    ext2_write_block(fs, new_blk, buf);
    free(buf);

    dir.i_size += fs->block_size;
    ext2_write_inode(fs, dir_ino, &dir);
    return 0;
}

static int ext2_dir_remove_entry(ext2_t *fs, uint32_t dir_ino, const char *name)
{
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int name_len = (int)strlen(name);
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = ext2_bmap(fs, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext2_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)(buf + pos);
            if (hdr->rec_len == 0) break;
            if (hdr->ino != 0 && hdr->name_len == name_len &&
                memcmp(buf + pos + 8, name, (size_t)name_len) == 0) {
                hdr->ino = 0;
                ext2_write_block(fs, blk, buf);
                free(buf);
                return 0;
            }
            pos += hdr->rec_len;
        }
    }
    free(buf);
    return -1;
}

static int ext2_dir_is_empty(ext2_t *fs, uint32_t dir_ino)
{
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0) return 0;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return 0;

    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
    int empty = 1;

    for (uint32_t bi = 0; bi < nblocks && empty; bi++) {
        uint32_t blk = ext2_bmap(fs, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext2_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size) {
            ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)(buf + pos);
            if (hdr->rec_len == 0) break;
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

static int ext2_dir_lookup_component(ext2_t *fs, uint32_t dir_ino, const char *comp, uint32_t *out_ino, int *out_is_dir)
{
    ext2_dirent_loc_t loc;
    if (ext2_dir_find(fs, dir_ino, comp, &loc) != 0) return -1;
    *out_ino = loc.ino;
    if (out_is_dir) *out_is_dir = (loc.file_type == EXT2_FT_DIR);
    return 0;
}

static int ext2_walk(ext2_t *fs, const char *path, uint32_t *out_ino)
{
    uint32_t cur = EXT2_ROOT_INO;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[EXT2_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < EXT2_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        while (*p == '/') p++;

        uint32_t next;
        if (ext2_dir_lookup_component(fs, cur, comp, &next, 0) != 0) return -1;
        cur = next;
    }
    *out_ino = cur;
    return 0;
}

static int ext2_read_data(ext2_t *fs, ext2_inode_t *inode, uint32_t offset, void *buf, uint32_t size)
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

        uint32_t blk = ext2_bmap(fs, inode, bi, 0);
        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > size - done) chunk = size - done;

        if (blk == 0) {
            memset((uint8_t *)buf + done, 0, chunk);
        } else {
            if (ext2_read_block(fs, blk, blk_buf) != 0) break;
            memcpy((uint8_t *)buf + done, blk_buf + in_blk, chunk);
        }
        done += chunk;
    }

    free(blk_buf);
    return (int)done;
}

static int ext2_write_data(ext2_t *fs, ext2_inode_t *inode, uint32_t offset, const void *buf, uint32_t size)
{
    uint8_t *blk_buf = (uint8_t *)malloc(fs->block_size);
    if (!blk_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        uint32_t cur_off = offset + done;
        uint32_t bi = cur_off / fs->block_size;
        uint32_t in_blk = cur_off % fs->block_size;

        uint32_t blk = ext2_bmap(fs, inode, bi, 1);
        if (blk == 0) break;

        uint32_t chunk = fs->block_size - in_blk;
        if (chunk > size - done) chunk = size - done;

        if (chunk < fs->block_size) {
            if (ext2_read_block(fs, blk, blk_buf) != 0) break;
        }
        memcpy(blk_buf + in_blk, (const uint8_t *)buf + done, chunk);
        if (ext2_write_block(fs, blk, blk_buf) != 0) break;

        done += chunk;
    }

    free(blk_buf);
    if (done > 0 && offset + done > inode->i_size) inode->i_size = offset + done;
    return (int)done;
}

static int ext2_probe(ext2_t *fs, blockdev_t *bd)
{
    fs->bd = bd;

    ext2_superblock_t *sb = (ext2_superblock_t *)malloc(1024);
    if (!sb) return -1;
    if (blockdev_read_bytes(bd, 1024, 1024, sb) != 0) { free(sb); return -1; }

    if (sb->s_magic != EXT2_SUPER_MAGIC) { free(sb); return -1; }

    fs->block_size = 1024u << sb->s_log_block_size;
    fs->blocks_count = sb->s_blocks_count;
    fs->inodes_count = sb->s_inodes_count;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_data_block = sb->s_first_data_block;
    fs->free_blocks_count = sb->s_free_blocks_count;
    fs->free_inodes_count = sb->s_free_inodes_count;
    fs->inode_size = (sb->s_rev_level >= EXT2_DYNAMIC_REV) ? sb->s_inode_size : 128;
    fs->num_groups = (fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->gdt_block = fs->first_data_block + 1;
    fs->gdt_blocks = (fs->num_groups * sizeof(ext2_group_desc_t) + fs->block_size - 1) / fs->block_size;

    free(sb);
    return 0;
}

int ext2_probe_and_mount(ext2_t *fs, blockdev_t *bd)
{
    if (ext2_probe(fs, bd) != 0) return -1;
    return 0;
}

int ext2_umount(ext2_t *fs)
{
    (void)fs;
    return 0;
}

static int ext2_vfs_open(void *ctx, const char *path, int flags)
{
    ext2_t *fs = (ext2_t *)ctx;

    char parent_path[256];
    char name[EXT2_MAX_FILENAME + 1];
    if (ext2_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_ino;
    if (ext2_walk(fs, parent_path, &parent_ino) != 0) return -1;

    ext2_dirent_loc_t loc;
    int found = (ext2_dir_find(fs, parent_ino, name, &loc) == 0);
    uint32_t ino;

    if (!found) {
        if (!(flags & VFS_CREAT)) return -1;

        ino = ext2_alloc_inode(fs, 0);
        if (ino == 0) return -1;

        ext2_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = EXT2_S_IFREG | 0644;
        inode.i_links_count = 1;
        ext2_write_inode(fs, ino, &inode);

        if (ext2_dir_add_entry(fs, parent_ino, name, ino, EXT2_FT_REG_FILE) != 0) {
            ext2_free_inode(fs, ino, 0);
            return -1;
        }
    } else {
        if (loc.file_type == EXT2_FT_DIR) return -1;
        ino = loc.ino;
        if (flags & VFS_TRUNC) {
            ext2_inode_t inode;
            if (ext2_read_inode(fs, ino, &inode) == 0) {
                ext2_free_inode_blocks(fs, &inode);
                inode.i_size = 0;
                ext2_write_inode(fs, ino, &inode);
            }
        }
    }

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            uint8_t raw[128];
            if (ext2_read_inode_raw(fs, ino, raw) != 0) return -1;
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].is_dir = 0;
            fs->fds[i].dirty = 0;
            memcpy(fs->fds[i].inode_raw, raw, 128);
            ext2_inode_t *inp = (ext2_inode_t *)fs->fds[i].inode_raw;
            fs->fds[i].size = inp->i_size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? inp->i_size : 0;
            return i;
        }
    }
    return -1;
}

static int ext2_vfs_close(void *ctx, int fd)
{
    ext2_t *fs = (ext2_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (fs->fds[fd].dirty) {
        ext2_inode_t *inp = (ext2_inode_t *)fs->fds[fd].inode_raw;
        inp->i_size = fs->fds[fd].size;
        ext2_write_inode_raw(fs, fs->fds[fd].ino, fs->fds[fd].inode_raw);
    }

    fs->fds[fd].used = 0;
    return 0;
}

static int ext2_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    ext2_t *fs = (ext2_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    ext2_inode_t *inp = (ext2_inode_t *)fs->fds[fd].inode_raw;
    int n = ext2_read_data(fs, inp, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += (uint32_t)n;
    return n;
}

static int ext2_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    ext2_t *fs = (ext2_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    ext2_inode_t *inp = (ext2_inode_t *)fs->fds[fd].inode_raw;
    int n = ext2_write_data(fs, inp, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += (uint32_t)n;
        fs->fds[fd].size = inp->i_size;
        fs->fds[fd].dirty = 1;
    }
    return n;
}

static int ext2_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    ext2_t *fs = (ext2_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int ext2_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    ext2_t *fs = (ext2_t *)ctx;

    uint32_t dir_ino;
    if (ext2_walk(fs, path, &dir_ino) != 0) return -1;

    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    int count = 0;
    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks && count < max; bi++) {
        uint32_t blk = ext2_bmap(fs, &dir, bi, 0);
        if (blk == 0) continue;
        if (ext2_read_block(fs, blk, buf) != 0) continue;

        uint32_t pos = 0;
        while (pos < fs->block_size && count < max) {
            /* hdr->rec_len/name_len are on-disk fields, attacker-
             * controlled if the mounted image is hostile -- name_len
             * was only ever clamped against VFS_NAME_LEN, never
             * against how much of `buf` (a single malloc(block_size)
             * allocation) actually remains at this position, letting
             * a crafted directory entry read past the end of that
             * heap block. Bound the whole record against both the
             * block buffer and rec_len before touching name_len. */
            if (pos + 8 > fs->block_size) break;
            ext2_dirent_hdr_t *hdr = (ext2_dirent_hdr_t *)(buf + pos);
            if (hdr->rec_len == 0) break;
            if (hdr->rec_len < 8 || pos + hdr->rec_len > fs->block_size) break;

            if (hdr->ino != 0) {
                int is_dot = (hdr->name_len == 1 && buf[pos + 8] == '.');
                int is_dotdot = (hdr->name_len == 2 && buf[pos + 8] == '.' && buf[pos + 9] == '.');
                if (!is_dot && !is_dotdot) {
                    uint32_t max_name = fs->block_size - pos - 8;
                    int nl = hdr->name_len;
                    if ((uint32_t)nl > max_name) nl = (int)max_name;
                    if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
                    memcpy(entries[count].name, buf + pos + 8, (size_t)nl);
                    entries[count].name[nl] = 0;

                    ext2_inode_t child;
                    ext2_read_inode(fs, hdr->ino, &child);
                    entries[count].size = child.i_size;
                    entries[count].is_dir = (hdr->file_type == EXT2_FT_DIR);
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

static int ext2_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    ext2_t *fs = (ext2_t *)ctx;

    char parent_path[256];
    char name[EXT2_MAX_FILENAME + 1];
    if (ext2_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_ino;
    if (ext2_walk(fs, parent_path, &parent_ino) != 0) return -1;

    ext2_dirent_loc_t existing;
    if (ext2_dir_find(fs, parent_ino, name, &existing) == 0) return -1;

    uint32_t new_ino = ext2_alloc_inode(fs, 1);
    if (new_ino == 0) return -1;

    uint32_t data_blk = ext2_alloc_block(fs);
    if (data_blk == 0) { ext2_free_inode(fs, new_ino, 1); return -1; }

    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) { ext2_free_block(fs, data_blk); ext2_free_inode(fs, new_ino, 1); return -1; }
    memset(buf, 0, fs->block_size);

    ext2_dirent_hdr_t *dot = (ext2_dirent_hdr_t *)buf;
    dot->ino = new_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    buf[8] = '.';

    ext2_dirent_hdr_t *dotdot = (ext2_dirent_hdr_t *)(buf + 12);
    dotdot->ino = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    buf[12 + 8] = '.';
    buf[12 + 9] = '.';

    ext2_write_block(fs, data_blk, buf);
    free(buf);

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFDIR | 0755;
    inode.i_links_count = 2;
    inode.i_size = fs->block_size;
    inode.i_block[0] = data_blk;
    inode.i_blocks = fs->block_size / 512;
    ext2_write_inode(fs, new_ino, &inode);

    if (ext2_dir_add_entry(fs, parent_ino, name, new_ino, EXT2_FT_DIR) != 0) {
        ext2_free_block(fs, data_blk);
        ext2_free_inode(fs, new_ino, 1);
        return -1;
    }

    ext2_inode_t parent;
    if (ext2_read_inode(fs, parent_ino, &parent) == 0) {
        parent.i_links_count++;
        ext2_write_inode(fs, parent_ino, &parent);
    }

    return 0;
}

static int ext2_vfs_unlink(void *ctx, const char *path)
{
    ext2_t *fs = (ext2_t *)ctx;

    char parent_path[256];
    char name[EXT2_MAX_FILENAME + 1];
    if (ext2_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_ino;
    if (ext2_walk(fs, parent_path, &parent_ino) != 0) return -1;

    ext2_dirent_loc_t loc;
    if (ext2_dir_find(fs, parent_ino, name, &loc) != 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, loc.ino, &inode) != 0) return -1;

    if (loc.file_type == EXT2_FT_DIR) {
        if (!ext2_dir_is_empty(fs, loc.ino)) return -1;

        if (ext2_dir_remove_entry(fs, parent_ino, name) != 0) return -1;

        ext2_free_inode_blocks(fs, &inode);
        ext2_free_inode(fs, loc.ino, 1);

        ext2_inode_t parent;
        if (ext2_read_inode(fs, parent_ino, &parent) == 0 && parent.i_links_count > 0) {
            parent.i_links_count--;
            ext2_write_inode(fs, parent_ino, &parent);
        }
        return 0;
    }

    if (ext2_dir_remove_entry(fs, parent_ino, name) != 0) return -1;

    if (inode.i_links_count > 0) inode.i_links_count--;
    if (inode.i_links_count == 0) {
        ext2_free_inode_blocks(fs, &inode);
        ext2_free_inode(fs, loc.ino, 0);
    } else {
        ext2_write_inode(fs, loc.ino, &inode);
    }
    return 0;
}

static int ext2_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    ext2_t *fs = (ext2_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;

    uint32_t ino;
    char name[EXT2_MAX_FILENAME + 1];
    int file_type;

    if (!*p) {
        ino = EXT2_ROOT_INO;
        name[0] = 0;
        file_type = EXT2_FT_DIR;
    } else {
        char parent_path[256];
        if (ext2_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

        uint32_t parent_ino;
        if (ext2_walk(fs, parent_path, &parent_ino) != 0) return -1;

        ext2_dirent_loc_t loc;
        if (ext2_dir_find(fs, parent_ino, name, &loc) != 0) return -1;
        ino = loc.ino;
        file_type = loc.file_type;
    }

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return -1;

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = inode.i_size;
    entry->is_dir = (file_type == EXT2_FT_DIR);
    entry->inode = ino;
    entry->mode = inode.i_mode;
    return 0;
}

static int ext2_vfs_rename(void *ctx, const char *old, const char *new)
{
    ext2_t *fs = (ext2_t *)ctx;

    char old_parent_path[256], old_name[EXT2_MAX_FILENAME + 1];
    if (ext2_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) return -1;
    uint32_t old_parent;
    if (ext2_walk(fs, old_parent_path, &old_parent) != 0) return -1;

    ext2_dirent_loc_t loc;
    if (ext2_dir_find(fs, old_parent, old_name, &loc) != 0) return -1;

    char new_parent_path[256], new_name[EXT2_MAX_FILENAME + 1];
    if (ext2_split_path(new, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) return -1;
    uint32_t new_parent;
    if (ext2_walk(fs, new_parent_path, &new_parent) != 0) return -1;

    if (ext2_dir_add_entry(fs, new_parent, new_name, loc.ino, loc.file_type) != 0) return -1;
    ext2_dir_remove_entry(fs, old_parent, old_name);

    if (loc.file_type == EXT2_FT_DIR && old_parent != new_parent) {
        ext2_inode_t moved;
        if (ext2_read_inode(fs, loc.ino, &moved) == 0) {
            uint32_t blk = ext2_bmap(fs, &moved, 0, 0);
            if (blk != 0) {
                uint8_t *buf = (uint8_t *)malloc(fs->block_size);
                if (buf) {
                    if (ext2_read_block(fs, blk, buf) == 0) {
                        ext2_dirent_hdr_t *dot = (ext2_dirent_hdr_t *)buf;
                        if (dot->rec_len > 0) {
                            ext2_dirent_hdr_t *dotdot = (ext2_dirent_hdr_t *)(buf + dot->rec_len);
                            if (dotdot->name_len == 2) {
                                dotdot->ino = new_parent;
                                ext2_write_block(fs, blk, buf);
                            }
                        }
                    }
                    free(buf);
                }
            }
        }

        ext2_inode_t op, np;
        if (ext2_read_inode(fs, old_parent, &op) == 0 && op.i_links_count > 0) {
            op.i_links_count--;
            ext2_write_inode(fs, old_parent, &op);
        }
        if (ext2_read_inode(fs, new_parent, &np) == 0) {
            np.i_links_count++;
            ext2_write_inode(fs, new_parent, &np);
        }
    }

    return 0;
}

static int ext2_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void ext2_mount_vfs(ext2_t *fs, const char *mount_point)
{
    static vfs_ops_t ext2_vfs_ops = {
        .open = ext2_vfs_open,
        .close = ext2_vfs_close,
        .read = ext2_vfs_read,
        .write = ext2_vfs_write,
        .lseek = ext2_vfs_lseek,
        .readdir = ext2_vfs_readdir,
        .mkdir = ext2_vfs_mkdir,
        .unlink = ext2_vfs_unlink,
        .stat = ext2_vfs_stat,
        .rename = ext2_vfs_rename,
        .symlink = ext2_vfs_symlink,
    };
    vfs_mount(mount_point, &ext2_vfs_ops, fs);
}

int ext2_format(blockdev_t *bd, const char *label)
{
    uint32_t block_size = 1024;
    uint32_t sector_size = bd->sector_size ? bd->sector_size : 512;
    uint64_t total_bytes = bd->total_sectors * sector_size;
    uint32_t total_blocks = (uint32_t)(total_bytes / block_size);

    uint32_t first_data_block = 1;
    uint32_t blocks_per_group = 8192;
    uint32_t num_groups = (total_blocks - first_data_block + blocks_per_group - 1) / blocks_per_group;
    if (num_groups < 1) num_groups = 1;

    uint32_t gdt_blocks = (num_groups * sizeof(ext2_group_desc_t) + block_size - 1) / block_size;

    uint32_t inodes_per_group = (total_blocks / 4) / num_groups;
    inodes_per_group = (inodes_per_group / 8) * 8;
    if (inodes_per_group < 32) inodes_per_group = 32;
    uint32_t inode_table_blocks = inodes_per_group / 8;

    uint32_t inodes_count = inodes_per_group * num_groups;

    /* Per-group layout */
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
        uint32_t blocks_in_group = total_blocks - base;
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

    /* Zero out metadata area of group 0 plus superblock/gdt, and zero bitmaps for all groups */
    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t meta_start = (g == 0) ? group_base[g] : group_base[g];
        uint32_t meta_blocks = (g == 0)
            ? (1 + gdt_blocks + 2 + inode_table_blocks)
            : (2 + inode_table_blocks);
        for (uint32_t b = 0; b < meta_blocks; b++) {
            blockdev_write_bytes(bd, (uint64_t)(meta_start + b) * block_size, block_size, zbuf);
        }
    }

    /* Build block bitmaps: mark metadata blocks + reserved tail used */
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

    /* root directory data block: allocate first free data block of group 0 */
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

    /* Build inode bitmaps: mark reserved inodes 1-10 used in group 0 */
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
    free_inodes_total -= 0;

    /* Zero out inode tables */
    for (uint32_t g = 0; g < num_groups; g++) {
        for (uint32_t b = 0; b < inode_table_blocks; b++) {
            blockdev_write_bytes(bd, (uint64_t)(group_inode_table[g] + b) * block_size, block_size, zbuf);
        }
    }

    /* Write root directory data (., ..) */
    {
        uint8_t *buf = (uint8_t *)malloc(block_size);
        memset(buf, 0, block_size);

        ext2_dirent_hdr_t *dot = (ext2_dirent_hdr_t *)buf;
        dot->ino = EXT2_ROOT_INO;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        buf[8] = '.';

        ext2_dirent_hdr_t *dotdot = (ext2_dirent_hdr_t *)(buf + 12);
        dotdot->ino = EXT2_ROOT_INO;
        dotdot->rec_len = (uint16_t)(block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        buf[12 + 8] = '.';
        buf[12 + 9] = '.';

        blockdev_write_bytes(bd, (uint64_t)root_data_block * block_size, block_size, buf);
        free(buf);
    }

    /* Write root inode (inode 2) */
    {
        uint64_t off = (uint64_t)group_inode_table[0] * block_size + (uint64_t)(EXT2_ROOT_INO - 1) * 128;
        ext2_inode_t root;
        memset(&root, 0, sizeof(root));
        root.i_mode = EXT2_S_IFDIR | 0755;
        root.i_links_count = 2;
        root.i_size = block_size;
        root.i_block[0] = root_data_block;
        root.i_blocks = block_size / 512;
        blockdev_write_bytes(bd, off, sizeof(root), &root);
    }

    free(zbuf);

    /* Write group descriptor table */
    {
        uint8_t *gdt_buf = (uint8_t *)malloc(gdt_blocks * block_size);
        memset(gdt_buf, 0, gdt_blocks * block_size);
        ext2_group_desc_t *gds = (ext2_group_desc_t *)gdt_buf;

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

    /* Write superblock */
    {
        ext2_superblock_t *sb = (ext2_superblock_t *)malloc(1024);
        memset(sb, 0, 1024);

        sb->s_inodes_count = inodes_count;
        sb->s_blocks_count = total_blocks;
        sb->s_r_blocks_count = 0;
        sb->s_free_blocks_count = free_blocks_total;
        sb->s_free_inodes_count = free_inodes_total;
        sb->s_first_data_block = first_data_block;
        sb->s_log_block_size = 0; /* 1024 << 0 = 1024 */
        sb->s_log_frag_size = 0;
        sb->s_blocks_per_group = blocks_per_group;
        sb->s_frags_per_group = blocks_per_group;
        sb->s_inodes_per_group = inodes_per_group;
        sb->s_mtime = 0;
        sb->s_wtime = 0;
        sb->s_mnt_count = 0;
        sb->s_max_mnt_count = 0xFFFF;
        sb->s_magic = EXT2_SUPER_MAGIC;
        sb->s_state = 1;
        sb->s_errors = 1;
        sb->s_minor_rev_level = 0;
        sb->s_lastcheck = 0;
        sb->s_checkinterval = 0;
        sb->s_creator_os = 0;
        sb->s_rev_level = EXT2_DYNAMIC_REV;
        sb->s_def_resuid = 0;
        sb->s_def_resgid = 0;
        sb->s_first_ino = EXT2_FIRST_NON_RESERVED_INO;
        sb->s_inode_size = 128;
        sb->s_block_group_nr = 0;
        sb->s_feature_compat = 0;
        sb->s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
        sb->s_feature_ro_compat = 0;

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
