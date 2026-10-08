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
    uint32_t sectsize;
    uint64_t dblocks;
    uint64_t rootino;
    uint32_t agblocks;
    uint32_t agcount;
    uint16_t inodesize;
    uint16_t inopblock;
    uint8_t  agblklog;
    uint8_t  inopblog;
    uint8_t  dirblklog;
    uint8_t  imax_pct;
    uint32_t inoalign;      /* inode chunk alignment in blocks */
    uint64_t logstart;      /* internal log: first block (filesystem block number) */
    uint32_t logblocks;
    int      v5;            /* CRC-enabled (version 5) metadata */
    int      ftype;         /* directory entries carry a file type byte */
    int      spinodes;      /* sparse inode chunks: inode B-tree records carry a hole mask */
    int      finobt;        /* free inode B-tree present */
    int      inobtcount;    /* inode B-tree block counts are kept in the AGI */
    int      rw;            /* writable: everything we would touch is something we keep up to date */
    uint8_t  meta_uuid[16];
    char     fname[13];
    xfs_fd_t fds[VFS_MAX_FDS];
} xfs_t;

/* Version 5 volumes made by mkfs.xfs are written to when their log is clean and they use
 * no features beyond those handled here; anything else is read-only. */
int xfs_probe_and_mount(xfs_t *fs, blockdev_t *bd);
int xfs_umount(xfs_t *fs);
void xfs_mount_vfs(xfs_t *fs, const char *mount_point);
int xfs_format(blockdev_t *bd, const char *label);

#endif
