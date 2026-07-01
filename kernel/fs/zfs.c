#include "zfs.h"
#include "memory.h"
#include "string.h"

#define ZFS_UBERBLOCK_MAGIC  0x00bab10cULL

#define ZFS_LABEL_SIZE       (256 * 1024)
#define ZFS_LABEL_UBERBLOCK_OFFSET 16384
#define ZFS_UBERBLOCK_COUNT  128
#define ZFS_UBERBLOCK_SIZE   1024

#define ZFS_LABEL0_OFFSET    0ULL
#define ZFS_LABEL1_OFFSET    (256ULL * 1024)

typedef struct {
    uint64_t ub_magic;
    uint64_t ub_version;
    uint64_t ub_txg;
    uint64_t ub_guid_sum;
    uint64_t ub_timestamp;
    uint8_t  ub_rootbp[128];
} __attribute__((packed)) zfs_uberblock_t;

typedef struct {
    uint64_t dva[2][2];
    uint64_t flags_lvl_type;
    uint64_t phys_birth;
    uint64_t birth;
    uint64_t fill;
    uint64_t cksum[4];
} __attribute__((packed)) zfs_blkptr_t;

static int zfs_read_uberblock(blockdev_t *bd, uint64_t label_offset, uint32_t index,
                               zfs_uberblock_t *out)
{
    uint64_t off = label_offset + ZFS_LABEL_UBERBLOCK_OFFSET + (uint64_t)index * ZFS_UBERBLOCK_SIZE;
    return blockdev_read_bytes(bd, off, sizeof(zfs_uberblock_t), out);
}

static int zfs_find_active_uberblock(blockdev_t *bd, zfs_uberblock_t *best)
{
    int found = 0;
    best->ub_txg = 0;

    uint64_t labels[2] = { ZFS_LABEL0_OFFSET, ZFS_LABEL1_OFFSET };

    zfs_uberblock_t ub;
    for (int li = 0; li < 2; li++) {
        for (uint32_t i = 0; i < ZFS_UBERBLOCK_COUNT; i++) {
            if (zfs_read_uberblock(bd, labels[li], i, &ub) != 0) continue;
            if (ub.ub_magic != ZFS_UBERBLOCK_MAGIC) continue;
            if (ub.ub_txg > best->ub_txg) {
                *best = ub;
                found = 1;
            }
        }
    }
    return found ? 0 : -1;
}

int zfs_probe_and_mount(zfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(zfs_t));
    fs->bd = bd;

    zfs_uberblock_t best;
    if (zfs_find_active_uberblock(bd, &best) != 0) return -1;

    fs->ub_txg       = best.ub_txg;
    fs->ub_version   = best.ub_version;
    fs->ub_timestamp = best.ub_timestamp;
    fs->mounted      = 1;
    return 0;
}

int zfs_umount(zfs_t *fs)
{
    fs->mounted = 0;
    return 0;
}

static int zfs_vfs_open(void *ctx, const char *path, int flags)
{
    (void)ctx; (void)path; (void)flags;
    return -1;
}

static int zfs_vfs_close(void *ctx, int fd)
{
    zfs_t *fs = (zfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int zfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int zfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int zfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    zfs_t *fs = (zfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;
    return (int)fs->fds[fd].pos;
}

static int zfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    (void)ctx; (void)path; (void)entries; (void)max;
    return 0;
}

static int zfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx; (void)path; (void)mode;
    return -1;
}

static int zfs_vfs_unlink(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

static int zfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    (void)ctx; (void)path; (void)entry;
    return -1;
}

static int zfs_vfs_rename(void *ctx, const char *old, const char *new)
{
    (void)ctx; (void)old; (void)new;
    return -1;
}

static int zfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void zfs_mount_vfs(zfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ops = {
        .open    = zfs_vfs_open,
        .close   = zfs_vfs_close,
        .read    = zfs_vfs_read,
        .write   = zfs_vfs_write,
        .lseek   = zfs_vfs_lseek,
        .readdir = zfs_vfs_readdir,
        .mkdir   = zfs_vfs_mkdir,
        .unlink  = zfs_vfs_unlink,
        .stat    = zfs_vfs_stat,
        .rename  = zfs_vfs_rename,
        .symlink = zfs_vfs_symlink,
    };
    vfs_mount(mount_point, &ops, fs);
}

int zfs_format(blockdev_t *bd, const char *label)
{
    (void)label;

    uint8_t *blank = (uint8_t *)malloc(8192);
    if (!blank) return -1;
    memset(blank, 0, 8192);

    /* Clear label 0 preamble */
    blockdev_write_bytes(bd, ZFS_LABEL0_OFFSET, 8192, blank);
    free(blank);

    /* Write a valid uberblock at label 0, slot 0 */
    zfs_uberblock_t ub;
    memset(&ub, 0, sizeof(ub));
    ub.ub_magic     = ZFS_UBERBLOCK_MAGIC;
    ub.ub_version   = 5000ULL;
    ub.ub_txg       = 1ULL;
    ub.ub_guid_sum  = 0ULL;
    ub.ub_timestamp = 0ULL;

    uint64_t off = ZFS_LABEL0_OFFSET + ZFS_LABEL_UBERBLOCK_OFFSET;
    return blockdev_write_bytes(bd, off, sizeof(ub), &ub);
}
