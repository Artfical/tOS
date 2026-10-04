#ifndef XFS_H
#define XFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define XFS_MAX_FILENAME 255

typedef struct {
    int used;
    uint64_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
} xfs_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t blocksize;
    uint64_t dblocks;
    uint64_t rootino;
    uint32_t agblocks;
    uint32_t agcount;
    uint16_t inodesize;
    uint16_t inopblock;
    uint8_t  agblklog;
    uint8_t  inopblog;
    uint8_t  dirblklog;
    int      v5;            /* CRC-enabled (version 5) metadata */
    int      ftype;         /* directory entries carry a file type byte */
    char     fname[13];
    xfs_fd_t fds[VFS_MAX_FDS];
} xfs_t;

/* Real XFS volumes are mounted read-only: writing needs the log, the
 * allocation-group B-trees and CRC maintenance, none of which this driver
 * implements. */
int xfs_probe_and_mount(xfs_t *fs, blockdev_t *bd);
int xfs_umount(xfs_t *fs);
void xfs_mount_vfs(xfs_t *fs, const char *mount_point);
int xfs_format(blockdev_t *bd, const char *label);

#endif
