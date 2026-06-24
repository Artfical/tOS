#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define EXT2_MAX_FILENAME 255

typedef struct {
    int used;
    uint32_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
    uint8_t inode_raw[128];
} ext2_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t num_groups;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t gdt_block;
    uint32_t gdt_blocks;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    ext2_fd_t fds[VFS_MAX_FDS];
} ext2_t;

int ext2_probe_and_mount(ext2_t *fs, blockdev_t *bd);
int ext2_umount(ext2_t *fs);
void ext2_mount_vfs(ext2_t *fs, const char *mount_point);
int ext2_format(blockdev_t *bd, const char *label);

#endif
