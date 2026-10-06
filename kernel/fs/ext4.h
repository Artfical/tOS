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

    /* feature state of a volume made by someone else's mke2fs */
    uint32_t desc_size;      /* group descriptor size: 32, or 64 with the 64bit feature */
    uint32_t feat_incompat;
    uint32_t feat_ro;
    uint32_t reserved_gdt;   /* resize_inode: reserved GDT blocks after the descriptors */
    uint32_t first_ino;
    uint32_t csum_seed;      /* crc32c(~0, uuid) or the s_checksum_seed override */
    uint8_t  uuid[16];
    int csum_md;             /* metadata_csum: crc32c on every metadata structure */
    int csum_gdt;            /* uninit_bg / gdt_csum: crc16 group descriptor checksums */
    int lazy;                /* group flags (BLOCK_UNINIT/INODE_UNINIT) are meaningful */
    uint32_t gen_counter;    /* i_generation for new inodes */

    int ro;            /* mounted read-only: the volume uses features this driver cannot maintain */
    int use_journal;   /* tOS's own mini journal: only on volumes ext4_format() made */
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
