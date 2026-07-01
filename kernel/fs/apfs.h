#ifndef APFS_H
#define APFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define APFS_MAX_FILENAME 255

typedef struct {
    int used;
    uint64_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
} apfs_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t omap_oid;
    uint64_t fs_oid;
    int mounted;
    apfs_fd_t fds[VFS_MAX_FDS];
} apfs_t;

int apfs_probe_and_mount(apfs_t *fs, blockdev_t *bd);
int apfs_umount(apfs_t *fs);
void apfs_mount_vfs(apfs_t *fs, const char *mount_point);
int apfs_format(blockdev_t *bd, const char *label);

#endif
