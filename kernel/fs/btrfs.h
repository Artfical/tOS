#ifndef BTRFS_H
#define BTRFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define BTRFS_MAX_FILENAME 255

#define BTRFS_WBUF_SIZE (128u * 1024u)

typedef struct {
    int used;
    uint64_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    uint8_t *wbuf;          /* sequential writes are gathered here and written as one extent */
    uint32_t wstart, wlen;
} btrfs_fd_t;

#define BTRFS_MAX_CHUNKS 512

typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;      /* first stripe */
    uint64_t physical2;     /* second stripe (DUP / RAID1), when nstripes == 2 */
    uint64_t type;          /* block group type and profile bits */
    int nstripes;
} btrfs_chunk_map_t;

typedef struct { uint64_t start, len; } btrfs_range_t;

typedef struct {
    uint64_t start, len, flags, used;
    int dirty;
} btrfs_bg_t;

typedef struct { int kind; uint64_t a, b, c, d; } btrfs_pending_t;

typedef struct {
    blockdev_t *bd;
    uint64_t generation;
    uint64_t root_logical;          /* tree of tree roots */
    uint64_t chunk_root_logical;
    uint64_t fs_tree_logical;       /* default subvolume (id 5) */
    uint64_t extent_root;
    uint64_t dev_root;
    uint64_t csum_root;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint32_t sectorsize;
    uint32_t nodesize;
    char label[256];
    btrfs_chunk_map_t chunks[BTRFS_MAX_CHUNKS];
    int num_chunks;

    /* writing: the volume is changed in place, one transaction per VFS call */
    int rw;
    uint8_t sb[4096];               /* the superblock as read, patched and rewritten at commit */
    uint8_t hdr_fsid[16];           /* header fields copied into every new tree block */
    uint8_t hdr_chunk_uuid[16];
    uint64_t cur_gen;
    int in_txn;
    uint32_t root_dirty;            /* bit per tree id whose root block or level changed */
    btrfs_range_t *used;            /* allocated extents and tree blocks, sorted by start */
    int used_n, used_cap;
    btrfs_bg_t *bgs;
    int nbgs, bgs_cap;
    int alloc_loaded;
    int has_fst;                    /* the free-space tree is still there (dropped at the first write) */
    btrfs_pending_t *pend;          /* extent-tree items still to be written */
    int pend_n, pend_cap;
    uint64_t dev_size;

    btrfs_fd_t fds[VFS_MAX_FDS];
} btrfs_t;

int btrfs_probe_and_mount(btrfs_t *fs, blockdev_t *bd);
int btrfs_umount(btrfs_t *fs);
void btrfs_mount_vfs(btrfs_t *fs, const char *mount_point);
int btrfs_format(blockdev_t *bd, const char *label);

#endif
