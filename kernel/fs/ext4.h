#ifndef EXT4_H
#define EXT4_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define EXT4_MAX_FILENAME 255
#define EXT4_MAX_TXN_BLOCKS 64

typedef struct {
    int used;
    uint32_t ino;
    uint32_t pos;
    uint32_t size;
    int is_dir;
    int dirty;
    uint8_t inode_raw[128];
} ext4_fd_t;

typedef struct {
    uint32_t block;
    uint8_t *data;
} ext4_txn_entry_t;

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
    ext4_txn_entry_t txn[EXT4_MAX_TXN_BLOCKS];

    ext4_fd_t fds[VFS_MAX_FDS];
} ext4_t;

int ext4_probe_and_mount(ext4_t *fs, blockdev_t *bd);
int ext4_umount(ext4_t *fs);
void ext4_mount_vfs(ext4_t *fs, const char *mount_point);
int ext4_format(blockdev_t *bd, const char *label);

#endif
