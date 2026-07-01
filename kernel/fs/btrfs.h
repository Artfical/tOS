#ifndef BTRFS_H
#define BTRFS_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define BTRFS_MAX_FILENAME 255

typedef struct {
    int used;
    uint64_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
} btrfs_fd_t;

#define BTRFS_MAX_CHUNKS 64

typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;
} btrfs_chunk_map_t;

typedef struct {
    blockdev_t *bd;
    uint64_t generation;
    uint64_t root_logical;
    uint64_t chunk_root_logical;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t leafsize;
    char label[256];
    btrfs_chunk_map_t chunks[BTRFS_MAX_CHUNKS];
    int num_chunks;
    uint64_t next_objectid;
    uint64_t next_data_logical;
    uint64_t fs_tree_logical;
    btrfs_fd_t fds[VFS_MAX_FDS];
} btrfs_t;

int btrfs_probe_and_mount(btrfs_t *fs, blockdev_t *bd);
int btrfs_umount(btrfs_t *fs);
void btrfs_mount_vfs(btrfs_t *fs, const char *mount_point);
int btrfs_format(blockdev_t *bd, const char *label);

#endif
