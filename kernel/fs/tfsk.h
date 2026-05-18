#ifndef TFSK_H
#define TFSK_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "ata.h"

#define TFSK_BLOCK_SIZE     4096
#define TFSK_INODE_SIZE     256
#define TFSK_INODES_PER_BLK (TFSK_BLOCK_SIZE / TFSK_INODE_SIZE)
#define TFSK_DIRECT_BLOCKS  12
#define TFSK_INLINE_MAX     60
#define TFSK_MAX_INODES     65536
#define TFSK_ROOT_INODE     2
#define TFSK_MAX_NAME       255
#define TFSK_MAX_VOLUME     64
#define TFSK_UUID_LEN       16
#define TFSK_SB_BLK         1
#define TFSK_INODE_BMP_BLK  2
#define TFSK_BLOCK_BMP_BLK  3
#define TFSK_SB_CKSUM_SIZE  144
#define TFSK_MAGIC          0x54465301

#define TFSK_FT_UNKNOWN  0
#define TFSK_FT_FILE     1
#define TFSK_FT_DIR      2
#define TFSK_FT_SYMLINK  3

#define TFSK_STATE_CLEAN 0
#define TFSK_STATE_DIRTY 1
#define TFSK_STATE_ERROR 2

typedef struct __attribute__((packed)) tfsk_superblock {
    uint32_t magic;
    uint32_t version;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t inode_bmp_blk;
    uint32_t block_bmp_blk;
    uint32_t inode_table_blk;
    uint32_t root_inode;
    uint32_t state;
    uint32_t mount_count;
    uint32_t mount_time;
    uint32_t last_check;
    uint8_t  uuid[TFSK_UUID_LEN];
    char     volume[TFSK_MAX_VOLUME];
    uint32_t checksum;
    uint8_t  padding[3948];
} tfsk_superblock_t;

typedef struct __attribute__((packed)) tfsk_inode {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t link_count;
    uint32_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint16_t flags;
    uint16_t pad;
    uint32_t blocks;
    union {
        struct { uint32_t direct[TFSK_DIRECT_BLOCKS]; } ptr;
        uint8_t inline_data[TFSK_INLINE_MAX];
    } u;
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t cksum;
    uint8_t  padding[152];
} tfsk_inode_t;

typedef struct __attribute__((packed)) tfsk_dentry {
    uint32_t inode;
    uint16_t entry_len;
    uint16_t name_len;
    uint8_t  file_type;
    char     name[TFSK_MAX_NAME];
} tfsk_dentry_t;

typedef struct {
    int used;
    uint32_t ino;
    uint32_t offset;
    int flags;
} tfsk_file_t;

typedef struct {
    tfsk_superblock_t sb;
    ata_device_t *dev;
    int dirty;
    int mounted;
    tfsk_file_t fds[VFS_MAX_FDS];
} tfsk_t;

int  tfsk_mount(tfsk_t *fs, ata_device_t *dev);
int  tfsk_umount(tfsk_t *fs);
int  tfsk_format(ata_device_t *dev, uint64_t blocks, const char *volume);
int  tfsk_probe_and_mount(tfsk_t *fs);
void tfsk_mount_vfs(tfsk_t *fs, const char *mount_point);

int tfsk_vfs_open(tfsk_t *fs, const char *path, int flags);
int tfsk_vfs_close(tfsk_t *fs, int fd);
int tfsk_vfs_read(tfsk_t *fs, int fd, void *buf, uint32_t size);
int tfsk_vfs_write(tfsk_t *fs, int fd, const void *buf, uint32_t size);
int tfsk_vfs_lseek(tfsk_t *fs, int fd, uint32_t offset, int whence);
int tfsk_vfs_readdir(tfsk_t *fs, const char *path, vfs_entry_t *entries, int max);
int tfsk_vfs_mkdir(tfsk_t *fs, const char *path, uint32_t mode);
int tfsk_vfs_unlink(tfsk_t *fs, const char *path);
int tfsk_vfs_stat(tfsk_t *fs, const char *path, vfs_entry_t *entry);
int tfsk_walk(tfsk_t *fs, const char *path, uint32_t *ino);
int tfsk_mkdir(tfsk_t *fs, uint32_t parent_ino, const char *name);

#endif
