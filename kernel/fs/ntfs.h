#ifndef NTFS_H
#define NTFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define NTFS_MAX_FILENAME   255

typedef struct {
    int used;
    uint32_t record;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int append;
    int dirty;
} ntfs_fd_t;

typedef struct {
    uint64_t vcn;
    uint64_t len;
    int64_t  lcn;   /* -1 = sparse hole */
} ntfs_run_t;

typedef struct {
    blockdev_t *bd;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint64_t total_sectors;
    uint64_t total_clusters;

    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    uint32_t mft_record_size;
    uint32_t mirr_records;
    uint32_t index_block_size;
    int ntfs_major;

    int rw;             /* mounted read-write (volume was clean) */
    int dirty_marked;   /* we set VOLUME_IS_DIRTY and must clear it on umount */

    uint16_t *upcase;
    uint32_t upcase_len;

    ntfs_run_t *mft_runs;
    int mft_nruns;
    uint64_t mft_data_size;

    ntfs_run_t *bm_runs;
    int bm_nruns;
    uint64_t bm_data_size;

    uint64_t alloc_hint;
    uint32_t mft_hint;

    ntfs_fd_t fds[VFS_MAX_FDS];
} ntfs_t;

int ntfs_probe_and_mount(ntfs_t *fs, blockdev_t *bd);
int ntfs_umount(ntfs_t *fs);
void ntfs_mount_vfs(ntfs_t *fs, const char *mount_point);
int ntfs_format(blockdev_t *bd, const char *label);
int ntfs_detect(blockdev_t *bd);  /* boot-sector check only; touches nothing */

/* Grows (or, when the clusters being dropped are all free, shrinks) the
 * filesystem to new_sectors 512-byte-equivalent device sectors. */
int ntfs_resize(blockdev_t *bd, uint64_t new_sectors, char *err, int err_len);

#endif
