#ifndef EXT3_H
#define EXT3_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define EXT3_MAX_FILENAME 255
#define EXT3_MAX_TXN_BLOCKS 64

typedef struct {
    int used;
    uint32_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
    uint8_t inode_raw[128];
} ext3_fd_t;

typedef struct {
    uint32_t block;
    uint8_t *data;
} ext3_txn_entry_t;

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

    uint32_t journal_first_block;
    uint32_t journal_blocks;
    uint32_t journal_sequence;
    uint32_t journal_cursor;

    int in_txn;
    int txn_count;
    ext3_txn_entry_t txn[EXT3_MAX_TXN_BLOCKS];

    ext3_fd_t fds[VFS_MAX_FDS];
} ext3_t;

int ext3_probe_and_mount(ext3_t *fs, blockdev_t *bd);
int ext3_umount(ext3_t *fs);
void ext3_mount_vfs(ext3_t *fs, const char *mount_point);
int ext3_format(blockdev_t *bd, const char *label);

#endif
