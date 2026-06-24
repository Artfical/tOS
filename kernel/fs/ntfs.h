#ifndef NTFS_H
#define NTFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define NTFS_MAX_FILENAME   255
#define NTFS_RECORD_SIZE    4096
#define NTFS_CLUSTER_SIZE   4096

typedef struct {
    int used;
    uint32_t record;
    uint32_t pos;
    uint32_t size;
    int is_dir;
} ntfs_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint64_t total_clusters;

    uint64_t mft_lcn;
    uint32_t mft_record_size;

    ntfs_fd_t fds[VFS_MAX_FDS];
} ntfs_t;

int ntfs_probe_and_mount(ntfs_t *fs, blockdev_t *bd);
int ntfs_umount(ntfs_t *fs);
void ntfs_mount_vfs(ntfs_t *fs, const char *mount_point);
int ntfs_format(blockdev_t *bd, const char *label);

#endif
