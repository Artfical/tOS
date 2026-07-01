#include "xfs.h"
#include "memory.h"
#include "string.h"

#define XFS_SB_MAGIC 0x58465342U

#define XFS_DINODE_MAGIC 0x494EU

#define XFS_DINODE_FMT_LOCAL   1
#define XFS_DINODE_FMT_EXTENTS 2
#define XFS_DINODE_FMT_BTREE   3

#define XFS_DIR2_SF_HDR_SIZE   10
#define XFS_DIR3_BLOCK_MAGIC   0x58443342U
#define XFS_DIR2_BLOCK_MAGIC   0x58443242U

#define XFS_INO_MASK(k) ((uint64_t)((1ULL << (k)) - 1))

static uint16_t be16(uint16_t v)
{
    return (uint16_t)(((v & 0xFFU) << 8) | ((v >> 8) & 0xFFU));
}

static uint32_t be32(uint32_t v)
{
    return ((v & 0xFFU) << 24) | (((v >> 8) & 0xFFU) << 16) |
           (((v >> 16) & 0xFFU) << 8) | ((v >> 24) & 0xFFU);
}

static uint64_t be64(uint64_t v)
{
    return ((uint64_t)be32((uint32_t)(v >> 32))) |
           ((uint64_t)be32((uint32_t)(v & 0xFFFFFFFFU)) << 32);
}

typedef struct {
    uint32_t sb_magicnum;
    uint32_t sb_blocksize;
    uint64_t sb_dblocks;
    uint64_t sb_rblocks;
    uint64_t sb_rextents;
    uint8_t  sb_uuid[16];
    uint64_t sb_logstart;
    uint64_t sb_rootino;
    uint64_t sb_rbmino;
    uint64_t sb_rsumino;
    uint32_t sb_rextsize;
    uint32_t sb_agblocks;
    uint32_t sb_agcount;
    uint32_t sb_rbmblocks;
    uint32_t sb_logblocks;
    uint16_t sb_versionnum;
    uint16_t sb_sectsize;
    uint16_t sb_inodesize;
    uint16_t sb_inopblock;
    char     sb_fname[12];
    uint8_t  sb_blocklog;
    uint8_t  sb_sectlog;
    uint8_t  sb_inodelog;
    uint8_t  sb_inopblog;
    uint8_t  sb_agblklog;
    uint8_t  sb_rextslog;
    uint8_t  sb_inprogress;
    uint8_t  sb_imax_pct;
    uint64_t sb_icount;
    uint64_t sb_ifree;
    uint64_t sb_fdblocks;
    uint64_t sb_frextents;
    uint64_t sb_uquotino;
    uint64_t sb_gquotino;
    uint16_t sb_qflags;
    uint8_t  sb_flags;
    uint8_t  sb_shared_vn;
    uint32_t sb_inoalignmt;
    uint32_t sb_unit;
    uint32_t sb_width;
} __attribute__((packed)) xfs_sb_t;

typedef struct {
    uint16_t di_magic;
    uint16_t di_mode;
    uint8_t  di_version;
    uint8_t  di_format;
    uint16_t di_nlinkv1;
    uint32_t di_uid;
    uint32_t di_gid;
    uint32_t di_nlink;
    uint16_t di_projid_lo;
    uint16_t di_projid_hi;
    uint8_t  di_pad[8];
    uint16_t di_flushiter;
    uint64_t di_atime;
    uint64_t di_mtime;
    uint64_t di_ctime;
    uint64_t di_size;
    uint64_t di_nblocks;
    uint32_t di_extsize;
    uint32_t di_nextents;
    uint16_t di_anextents;
    uint8_t  di_forkoff;
    uint8_t  di_aformat;
    uint32_t di_dmevmask;
    uint16_t di_dmstate;
    uint16_t di_flags;
    uint32_t di_gen;
    uint32_t di_next_unlinked;
} __attribute__((packed)) xfs_dinode_t;

typedef struct {
    uint32_t magic;
    uint32_t pad;
    uint64_t reserved1[2];
    uint64_t lsn;
    uint8_t  uuid[16];
    uint64_t owner;
    uint64_t blkno;
    uint8_t  entries[0];
} __attribute__((packed)) xfs_dir3_blk_hdr_t;

typedef struct {
    uint64_t ino;
    uint8_t  namelen;
    uint8_t  ftype;
    char     name[0];
} __attribute__((packed)) xfs_dir2_data_entry_t;

static uint64_t xfs_ino_to_byte(xfs_t *fs, uint64_t ino)
{
    uint64_t block = ino / fs->inopblock;
    uint64_t off   = ino % fs->inopblock;
    return block * (uint64_t)fs->blocksize + off * (uint64_t)fs->inodesize;
}

static int xfs_read_inode(xfs_t *fs, uint64_t ino, xfs_dinode_t *out)
{
    uint64_t byte_off = xfs_ino_to_byte(fs, ino);
    return blockdev_read_bytes(fs->bd, byte_off, sizeof(xfs_dinode_t), out);
}

static uint64_t xfs_extent_startblock(const uint8_t ext[16])
{
    uint64_t hi = ((uint64_t)ext[0] << 56) | ((uint64_t)ext[1] << 48) |
                  ((uint64_t)ext[2] << 40) | ((uint64_t)ext[3] << 32) |
                  ((uint64_t)ext[4] << 24) | ((uint64_t)ext[5] << 16) |
                  ((uint64_t)ext[6] << 8)  | (uint64_t)ext[7];
    uint64_t lo = ((uint64_t)ext[8] << 56) | ((uint64_t)ext[9] << 48) |
                  ((uint64_t)ext[10] << 40) | ((uint64_t)ext[11] << 32) |
                  ((uint64_t)ext[12] << 24) | ((uint64_t)ext[13] << 16) |
                  ((uint64_t)ext[14] << 8)  | (uint64_t)ext[15];
    /* bits 126..73 = startoff (54 bits), bits 72..21 = startblock (52 bits) */
    uint64_t startblock = (lo >> 21) & ((1ULL << 52) - 1);
    uint64_t hi_bits = (hi >> 9) & 0x1FULL;
    startblock |= (hi_bits << 47);
    return startblock;
}

static uint64_t xfs_extent_startoff(const uint8_t ext[16])
{
    uint64_t hi = ((uint64_t)ext[0] << 56) | ((uint64_t)ext[1] << 48) |
                  ((uint64_t)ext[2] << 40) | ((uint64_t)ext[3] << 32) |
                  ((uint64_t)ext[4] << 24) | ((uint64_t)ext[5] << 16) |
                  ((uint64_t)ext[6] << 8)  | (uint64_t)ext[7];
    uint64_t lo = ((uint64_t)ext[8] << 56) | ((uint64_t)ext[9] << 48) |
                  ((uint64_t)ext[10] << 40) | ((uint64_t)ext[11] << 32) |
                  ((uint64_t)ext[12] << 24) | ((uint64_t)ext[13] << 16) |
                  ((uint64_t)ext[14] << 8)  | (uint64_t)ext[15];
    /* bits 126..73: startoff occupies bits 126-73 across hi and lo */
    uint64_t raw = ((hi & 0x7FFFFFFFFFFFFFFFULL) >> 9);
    raw = (raw << 9) | (lo >> 55);
    raw >>= 1;
    return raw & ((1ULL << 54) - 1);
}

static uint32_t xfs_extent_blockcount(const uint8_t ext[16])
{
    uint64_t lo = ((uint64_t)ext[8] << 56) | ((uint64_t)ext[9] << 48) |
                  ((uint64_t)ext[10] << 40) | ((uint64_t)ext[11] << 32) |
                  ((uint64_t)ext[12] << 24) | ((uint64_t)ext[13] << 16) |
                  ((uint64_t)ext[14] << 8)  | (uint64_t)ext[15];
    return (uint32_t)(lo & ((1ULL << 21) - 1));
}

static int xfs_read_extent_data(xfs_t *fs, uint64_t block, uint32_t count, uint32_t file_off,
                                  void *buf, uint32_t size, uint32_t pos)
{
    uint64_t extent_byte_start = block * (uint64_t)fs->blocksize;
    uint64_t extent_byte_size  = (uint64_t)count * fs->blocksize;
    uint64_t file_byte_start   = (uint64_t)file_off * fs->blocksize;
    uint64_t file_byte_end     = file_byte_start + extent_byte_size;

    uint64_t copy_start = (uint64_t)pos > file_byte_start ? (uint64_t)pos : file_byte_start;
    uint64_t copy_end   = ((uint64_t)pos + size) < file_byte_end ? ((uint64_t)pos + size) : file_byte_end;
    if (copy_start >= copy_end) return 0;

    uint64_t disk_off = extent_byte_start + (copy_start - file_byte_start);
    uint32_t len      = (uint32_t)(copy_end - copy_start);
    uint32_t buf_off  = (uint32_t)(copy_start - pos);

    return blockdev_read_bytes(fs->bd, disk_off, len, (uint8_t *)buf + buf_off);
}

int xfs_probe_and_mount(xfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(xfs_t));
    fs->bd = bd;

    xfs_sb_t *sb = (xfs_sb_t *)malloc(sizeof(xfs_sb_t));
    if (!sb) return -1;

    if (blockdev_read_bytes(bd, 0, sizeof(xfs_sb_t), sb) != 0) { free(sb); return -1; }

    if (be32(sb->sb_magicnum) != XFS_SB_MAGIC) { free(sb); return -1; }

    fs->blocksize  = be32(sb->sb_blocksize);
    fs->dblocks    = be64(sb->sb_dblocks);
    fs->rootino    = be64(sb->sb_rootino);
    fs->agblocks   = be32(sb->sb_agblocks);
    fs->agcount    = be32(sb->sb_agcount);
    fs->inodesize  = be16(sb->sb_inodesize);
    fs->inopblock  = be16(sb->sb_inopblock);
    fs->agblklog   = sb->sb_agblklog;
    fs->inopblog   = sb->sb_inopblog;
    memcpy(fs->fname, sb->sb_fname, 12);
    fs->fname[12] = 0;

    if (fs->blocksize == 0) fs->blocksize = 4096;
    if (fs->inodesize == 0) fs->inodesize = 256;
    if (fs->inopblock == 0) fs->inopblock = fs->blocksize / fs->inodesize;

    free(sb);
    return 0;
}

int xfs_umount(xfs_t *fs)
{
    (void)fs;
    return 0;
}

static int xfs_walk_path(xfs_t *fs, const char *path, uint64_t *out_ino, int *out_is_dir)
{
    uint64_t cur_ino = fs->rootino;
    int is_dir = 1;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[XFS_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < XFS_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        if (i == 0) continue;

        if (!is_dir) return -1;

        xfs_dinode_t dinode;
        if (xfs_read_inode(fs, cur_ino, &dinode) != 0) return -1;
        if (be16(dinode.di_magic) != XFS_DINODE_MAGIC) return -1;

        uint8_t fmt = dinode.di_format;
        uint64_t inode_byte = xfs_ino_to_byte(fs, cur_ino);
        uint32_t inode_size = fs->inodesize;
        uint8_t *inode_buf = (uint8_t *)malloc(inode_size);
        if (!inode_buf) return -1;
        if (blockdev_read_bytes(fs->bd, inode_byte, inode_size, inode_buf) != 0) {
            free(inode_buf);
            return -1;
        }
        uint8_t *fork = inode_buf + 100;

        int found = 0;

        if (fmt == XFS_DINODE_FMT_LOCAL) {
            /* Shortform directory */
            uint8_t count = fork[0];
            uint8_t i8count = fork[1];
            /* parent ino: 8 bytes */
            uint8_t *ep = fork + 2 + 8;
            for (uint8_t ei = 0; ei < count && !found; ei++) {
                uint8_t namelen = ep[0];
                /* offset: 2 bytes */
                uint8_t *name_ptr = ep + 3;
                if (namelen == (uint8_t)i && memcmp(name_ptr, comp, (size_t)i) == 0) {
                    /* ftype: 1 byte after name */
                    uint8_t ftype = name_ptr[namelen];
                    uint8_t *ino_bytes = name_ptr + namelen + 1;
                    uint64_t child_ino;
                    if (i8count > 0) {
                        child_ino = ((uint64_t)ino_bytes[0] << 56) | ((uint64_t)ino_bytes[1] << 48) |
                                    ((uint64_t)ino_bytes[2] << 40) | ((uint64_t)ino_bytes[3] << 32) |
                                    ((uint64_t)ino_bytes[4] << 24) | ((uint64_t)ino_bytes[5] << 16) |
                                    ((uint64_t)ino_bytes[6] << 8)  | (uint64_t)ino_bytes[7];
                    } else {
                        child_ino = ((uint64_t)ino_bytes[0] << 24) | ((uint64_t)ino_bytes[1] << 16) |
                                    ((uint64_t)ino_bytes[2] << 8)  | (uint64_t)ino_bytes[3];
                    }
                    cur_ino = child_ino;
                    is_dir = (ftype == 2);
                    found = 1;
                }
                uint32_t entry_size = 3 + namelen + 1 + (i8count > 0 ? 8 : 4);
                ep += entry_size;
            }
        } else if (fmt == XFS_DINODE_FMT_EXTENTS) {
            uint32_t nextents = be32(dinode.di_nextents);
            uint8_t *ext_arr = fork;

            uint8_t *blk_buf = (uint8_t *)malloc(fs->blocksize);
            if (!blk_buf) { free(inode_buf); return -1; }

            for (uint32_t ei = 0; ei < nextents && !found; ei++) {
                uint8_t *ext = ext_arr + ei * 16;
                uint64_t startblock = xfs_extent_startblock(ext);
                uint32_t blockcount = xfs_extent_blockcount(ext);

                for (uint32_t bi = 0; bi < blockcount && !found; bi++) {
                    uint64_t disk_byte = (startblock + bi) * fs->blocksize;
                    if (blockdev_read_bytes(fs->bd, disk_byte, fs->blocksize, blk_buf) != 0) continue;

                    uint32_t blk_magic = be32(*(uint32_t *)blk_buf);
                    uint8_t *entries;
                    uint32_t entries_size;

                    if (blk_magic == XFS_DIR3_BLOCK_MAGIC) {
                        entries = blk_buf + sizeof(xfs_dir3_blk_hdr_t);
                        entries_size = fs->blocksize - (uint32_t)sizeof(xfs_dir3_blk_hdr_t);
                    } else {
                        entries = blk_buf + 16;
                        entries_size = fs->blocksize - 16;
                    }

                    uint8_t *ep2 = entries;
                    uint8_t *ep_end = entries + entries_size;
                    while (ep2 + 11 <= ep_end && !found) {
                        uint64_t entry_ino_be;
                        memcpy(&entry_ino_be, ep2, 8);
                        uint64_t entry_ino = be64(entry_ino_be);
                        uint8_t namelen = ep2[8];
                        uint8_t ftype   = ep2[9];
                        uint8_t *name_ptr = ep2 + 10;
                        if (name_ptr + namelen > ep_end) break;
                        if (namelen == (uint8_t)i && memcmp(name_ptr, comp, (size_t)i) == 0) {
                            cur_ino = entry_ino;
                            is_dir = (ftype == 2);
                            found = 1;
                        }
                        uint32_t rec = 10 + namelen + 1;
                        rec = (rec + 7) & ~7U;
                        if (rec < 12) rec = 12;
                        ep2 += rec;
                    }
                }
            }
            free(blk_buf);
        }

        free(inode_buf);
        if (!found) return -1;
    }

    *out_ino = cur_ino;
    if (out_is_dir) *out_is_dir = is_dir;
    return 0;
}

static int xfs_vfs_open(void *ctx, const char *path, int flags)
{
    xfs_t *fs = (xfs_t *)ctx;
    (void)flags;

    uint64_t ino;
    int is_dir = 0;
    if (xfs_walk_path(fs, path, &ino, &is_dir) != 0) return -1;
    if (is_dir) return -1;

    xfs_dinode_t dinode;
    if (xfs_read_inode(fs, ino, &dinode) != 0) return -1;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used   = 1;
            fs->fds[i].ino    = ino;
            fs->fds[i].pos    = 0;
            fs->fds[i].size   = (uint32_t)be64(dinode.di_size);
            fs->fds[i].is_dir = 0;
            fs->fds[i].dirty  = 0;
            return i;
        }
    }
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
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    uint32_t pos = fs->fds[fd].pos;
    uint32_t file_size = fs->fds[fd].size;
    if (pos >= file_size) return 0;
    if (pos + size > file_size) size = file_size - pos;
    if (size == 0) return 0;

    uint64_t ino = fs->fds[fd].ino;
    uint32_t inode_size = fs->inodesize;
    uint8_t *inode_buf = (uint8_t *)malloc(inode_size);
    if (!inode_buf) return -1;

    uint64_t inode_byte = xfs_ino_to_byte(fs, ino);
    if (blockdev_read_bytes(fs->bd, inode_byte, inode_size, inode_buf) != 0) {
        free(inode_buf);
        return -1;
    }

    xfs_dinode_t *dinode = (xfs_dinode_t *)inode_buf;
    uint8_t fmt = dinode->di_format;
    uint8_t *fork = inode_buf + 100;

    memset(buf, 0, size);
    uint32_t done = 0;

    if (fmt == XFS_DINODE_FMT_LOCAL) {
        uint64_t isize = be64(dinode->di_size);
        uint32_t inline_size = (uint32_t)isize;
        uint32_t copy_start = pos;
        uint32_t copy_end   = pos + size;
        if (copy_end > inline_size) copy_end = inline_size;
        if (copy_start < copy_end) {
            memcpy(buf, fork + copy_start, copy_end - copy_start);
            done = copy_end - copy_start;
        }
    } else if (fmt == XFS_DINODE_FMT_EXTENTS) {
        uint32_t nextents = be32(dinode->di_nextents);
        uint8_t *ext_arr = fork;

        for (uint32_t ei = 0; ei < nextents; ei++) {
            uint8_t *ext = ext_arr + ei * 16;
            uint64_t startblock = xfs_extent_startblock(ext);
            uint64_t startoff   = xfs_extent_startoff(ext);
            uint32_t blockcount = xfs_extent_blockcount(ext);

            xfs_read_extent_data(fs, startblock, blockcount,
                                  (uint32_t)startoff, buf, size, pos);
        }
        done = size;
    }

    free(inode_buf);
    fs->fds[fd].pos += done;
    return (int)done;
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
    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;
    return (int)fs->fds[fd].pos;
}

static int xfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    xfs_t *fs = (xfs_t *)ctx;

    uint64_t dir_ino;
    int is_dir = 0;
    if (xfs_walk_path(fs, path, &dir_ino, &is_dir) != 0) return -1;
    if (!is_dir) return -1;

    uint32_t inode_size = fs->inodesize;
    uint8_t *inode_buf = (uint8_t *)malloc(inode_size);
    if (!inode_buf) return -1;

    uint64_t inode_byte = xfs_ino_to_byte(fs, dir_ino);
    if (blockdev_read_bytes(fs->bd, inode_byte, inode_size, inode_buf) != 0) {
        free(inode_buf);
        return -1;
    }

    xfs_dinode_t *dinode = (xfs_dinode_t *)inode_buf;
    uint8_t fmt = dinode->di_format;
    uint8_t *fork = inode_buf + 100;
    int count = 0;

    if (fmt == XFS_DINODE_FMT_LOCAL) {
        uint8_t ncount = fork[0];
        uint8_t i8count = fork[1];
        uint8_t *ep = fork + 2 + 8;
        for (uint8_t ei = 0; ei < ncount && count < max; ei++) {
            uint8_t namelen = ep[0];
            uint8_t *name_ptr = ep + 3;
            uint8_t ftype = name_ptr[namelen];
            uint8_t *ino_bytes = name_ptr + namelen + 1;

            if (namelen == 1 && name_ptr[0] == '.') goto next_sf;
            if (namelen == 2 && name_ptr[0] == '.' && name_ptr[1] == '.') goto next_sf;

            {
                uint64_t child_ino;
                if (i8count > 0) {
                    child_ino = ((uint64_t)ino_bytes[0] << 56) | ((uint64_t)ino_bytes[1] << 48) |
                                ((uint64_t)ino_bytes[2] << 40) | ((uint64_t)ino_bytes[3] << 32) |
                                ((uint64_t)ino_bytes[4] << 24) | ((uint64_t)ino_bytes[5] << 16) |
                                ((uint64_t)ino_bytes[6] << 8)  | (uint64_t)ino_bytes[7];
                } else {
                    child_ino = ((uint64_t)ino_bytes[0] << 24) | ((uint64_t)ino_bytes[1] << 16) |
                                ((uint64_t)ino_bytes[2] << 8)  | (uint64_t)ino_bytes[3];
                }

                int nl = namelen;
                if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
                memcpy(entries[count].name, name_ptr, (size_t)nl);
                entries[count].name[nl] = 0;
                entries[count].is_dir  = (ftype == 2);
                entries[count].inode   = (uint32_t)child_ino;
                entries[count].mode    = 0;
                entries[count].size    = 0;

                xfs_dinode_t child_dinode;
                if (xfs_read_inode(fs, child_ino, &child_dinode) == 0) {
                    entries[count].size = (uint32_t)be64(child_dinode.di_size);
                    entries[count].mode = be16(child_dinode.di_mode);
                }
                count++;
            }
next_sf:;
            uint32_t esz = 3 + namelen + 1 + (i8count > 0 ? 8 : 4);
            ep += esz;
        }
    } else if (fmt == XFS_DINODE_FMT_EXTENTS) {
        uint32_t nextents = be32(dinode->di_nextents);
        uint8_t *ext_arr = fork;
        uint8_t *blk_buf = (uint8_t *)malloc(fs->blocksize);
        if (!blk_buf) { free(inode_buf); return -1; }

        for (uint32_t ei = 0; ei < nextents && count < max; ei++) {
            uint8_t *ext = ext_arr + ei * 16;
            uint64_t startblock = xfs_extent_startblock(ext);
            uint32_t blockcount = xfs_extent_blockcount(ext);

            for (uint32_t bi = 0; bi < blockcount && count < max; bi++) {
                uint64_t disk_byte = (startblock + bi) * fs->blocksize;
                if (blockdev_read_bytes(fs->bd, disk_byte, fs->blocksize, blk_buf) != 0) continue;

                uint32_t blk_magic = be32(*(uint32_t *)blk_buf);
                uint8_t *ep2;
                uint8_t *ep_end;

                if (blk_magic == XFS_DIR3_BLOCK_MAGIC) {
                    ep2    = blk_buf + sizeof(xfs_dir3_blk_hdr_t);
                    ep_end = blk_buf + fs->blocksize;
                } else {
                    ep2    = blk_buf + 16;
                    ep_end = blk_buf + fs->blocksize;
                }

                while (ep2 + 11 <= ep_end && count < max) {
                    uint64_t entry_ino_be;
                    memcpy(&entry_ino_be, ep2, 8);
                    uint64_t entry_ino = be64(entry_ino_be);
                    uint8_t namelen = ep2[8];
                    uint8_t ftype   = ep2[9];
                    uint8_t *name_ptr = ep2 + 10;
                    if (name_ptr + namelen > ep_end) break;
                    if (entry_ino == 0) goto next_ext;
                    if (namelen == 1 && name_ptr[0] == '.') goto next_ext;
                    if (namelen == 2 && name_ptr[0] == '.' && name_ptr[1] == '.') goto next_ext;

                    {
                        int nl = namelen;
                        if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
                        memcpy(entries[count].name, name_ptr, (size_t)nl);
                        entries[count].name[nl] = 0;
                        entries[count].is_dir  = (ftype == 2);
                        entries[count].inode   = (uint32_t)entry_ino;
                        entries[count].size    = 0;
                        entries[count].mode    = 0;

                        xfs_dinode_t child_dinode;
                        if (xfs_read_inode(fs, entry_ino, &child_dinode) == 0) {
                            entries[count].size = (uint32_t)be64(child_dinode.di_size);
                            entries[count].mode = be16(child_dinode.di_mode);
                        }
                        count++;
                    }
next_ext:;
                    uint32_t rec = 10 + namelen + 1;
                    rec = (rec + 7) & ~7U;
                    if (rec < 12) rec = 12;
                    ep2 += rec;
                }
            }
        }
        free(blk_buf);
    }

    free(inode_buf);
    return count;
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
    int is_dir = 0;
    if (xfs_walk_path(fs, path, &ino, &is_dir) != 0) return -1;

    xfs_dinode_t dinode;
    if (xfs_read_inode(fs, ino, &dinode) != 0) return -1;

    const char *base = path;
    const char *p = path;
    while (*p) { if (*p == '/') base = p + 1; p++; }
    int nl = 0;
    while (base[nl] && nl < VFS_NAME_LEN - 1) { entry->name[nl] = base[nl]; nl++; }
    entry->name[nl] = 0;
    entry->size   = (uint32_t)be64(dinode.di_size);
    entry->is_dir = is_dir;
    entry->inode  = (uint32_t)ino;
    entry->mode   = be16(dinode.di_mode);
    return 0;
}

static int xfs_vfs_rename(void *ctx, const char *old, const char *new)
{
    (void)ctx; (void)old; (void)new;
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
    vfs_mount(mount_point, &ops, fs);
}

int xfs_format(blockdev_t *bd, const char *label)
{
    uint32_t blocksize  = 4096;
    uint16_t inodesize  = 512;
    uint16_t inopblock  = (uint16_t)(blocksize / inodesize);
    uint64_t sector_sz  = bd->sector_size ? bd->sector_size : 512;
    uint64_t total_bytes= bd->total_sectors * sector_sz;
    uint64_t total_blocks = total_bytes / blocksize;
    uint32_t agblocks   = (uint32_t)(total_blocks / 4);
    if (agblocks < 64) agblocks = 64;
    uint32_t agcount    = 4;

    uint8_t agblklog = 0;
    uint32_t tmp = agblocks - 1;
    while (tmp > 0) { agblklog++; tmp >>= 1; }

    uint8_t inopblog = 0;
    tmp = inopblock;
    while (tmp > 1) { inopblog++; tmp >>= 1; }

    /* Root inode at inode 128 (block 0, slot 0 in second allocation) */
    uint64_t rootino = 128;

    xfs_sb_t *sb = (xfs_sb_t *)malloc(sizeof(xfs_sb_t));
    if (!sb) return -1;
    memset(sb, 0, sizeof(xfs_sb_t));

    sb->sb_magicnum  = be32(XFS_SB_MAGIC);
    sb->sb_blocksize = be32(blocksize);
    sb->sb_dblocks   = be64(total_blocks);
    sb->sb_rblocks   = 0;
    sb->sb_rextents  = 0;
    sb->sb_logstart  = be64(1ULL);
    sb->sb_rootino   = be64(rootino);
    sb->sb_rbmino    = be64(rootino + 1);
    sb->sb_rsumino   = be64(rootino + 2);
    sb->sb_rextsize  = be32(1);
    sb->sb_agblocks  = be32(agblocks);
    sb->sb_agcount   = be32(agcount);
    sb->sb_rbmblocks = 0;
    sb->sb_logblocks = be32(512);
    sb->sb_versionnum= be16(0xB004U);
    sb->sb_sectsize  = be16(512);
    sb->sb_inodesize = be16(inodesize);
    sb->sb_inopblock = be16(inopblock);
    if (label) {
        int k = 0;
        while (label[k] && k < 11) { sb->sb_fname[k] = label[k]; k++; }
        sb->sb_fname[k] = 0;
    }
    sb->sb_blocklog  = 12;
    sb->sb_sectlog   = 9;
    sb->sb_inodelog  = 9;
    sb->sb_inopblog  = inopblog;
    sb->sb_agblklog  = agblklog;
    sb->sb_rextslog  = 0;
    sb->sb_inprogress= 0;
    sb->sb_imax_pct  = 25;
    sb->sb_icount    = be64(1);
    sb->sb_ifree     = 0;
    sb->sb_fdblocks  = be64(total_blocks - 10);

    blockdev_write_bytes(bd, 0, sizeof(xfs_sb_t), sb);
    free(sb);

    /* Write root inode */
    uint8_t *inode_buf = (uint8_t *)malloc(inodesize);
    if (!inode_buf) return -1;
    memset(inode_buf, 0, inodesize);

    xfs_dinode_t *di = (xfs_dinode_t *)inode_buf;
    di->di_magic    = be16(XFS_DINODE_MAGIC);
    di->di_mode     = be16(0x41EDU); /* dir | 0755 */
    di->di_version  = 2;
    di->di_format   = XFS_DINODE_FMT_LOCAL;
    di->di_nlink    = be32(2);
    di->di_size     = be64(10 + 8); /* shortform hdr */

    uint8_t *fork = inode_buf + 100;
    /* shortform dir: count=0, i8count=0, parent=rootino */
    fork[0] = 0;
    fork[1] = 0;
    uint64_t parent_be = be64(rootino);
    memcpy(fork + 2, &parent_be, 8);

    uint64_t root_byte = (rootino / inopblock) * blocksize + (rootino % inopblock) * inodesize;
    blockdev_write_bytes(bd, root_byte, inodesize, inode_buf);
    free(inode_buf);
    return 0;
}
