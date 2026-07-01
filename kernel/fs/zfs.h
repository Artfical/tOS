#ifndef ZFS_H
#define ZFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define ZFS_MAX_FILENAME 255

typedef struct {
    int used;
    uint64_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
} zfs_fd_t;

typedef struct {
    blockdev_t *bd;
    uint64_t ub_txg;
    uint64_t ub_version;
    uint64_t ub_timestamp;
    int mounted;
    zfs_fd_t fds[VFS_MAX_FDS];
} zfs_t;

int zfs_probe_and_mount(zfs_t *fs, blockdev_t *bd);
int zfs_umount(zfs_t *fs);
void zfs_mount_vfs(zfs_t *fs, const char *mount_point);
int zfs_format(blockdev_t *bd, const char *label);

#endif
