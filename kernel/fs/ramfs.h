#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include "vfs.h"

#define RAMFS_NAME_LEN    64
#define RAMFS_MAX_INODES  512
#define RAMFS_BLOCK_SZ    512

#define S_IFMT    0170000
#define S_IFREG   0100000
#define S_IFDIR   0040000
#define S_IFLNK   0120000

typedef struct ramfs_inode {
    uint32_t  ino;
    char      name[RAMFS_NAME_LEN];
    uint32_t  mode;
    uint32_t  size;
    uint8_t  *data;
    uint32_t  blocks;
    uint32_t  atime;
    uint32_t  mtime;
    uint32_t  uid;
    uint32_t  gid;
} ramfs_inode_t;

typedef struct ramfs_dir_entry {
    uint32_t ino;
    char name[RAMFS_NAME_LEN];
} ramfs_dir_entry_t;

void ramfs_init(void);
void ramfs_import_initrd(uint32_t addr, uint32_t size);
void ramfs_mount_vfs(void);

typedef vfs_entry_t ramfs_entry_t;

const char *ramfs_getcwd(void);
int ramfs_chdir(const char *path);
int ramfs_list(const char *path, vfs_entry_t *entries, int max);
int ramfs_create(const char *path);
int ramfs_delete(const char *path);
int ramfs_exists(const char *path);
int ramfs_is_dir(const char *path);
uint32_t ramfs_size(const char *path);
int ramfs_read(const char *path, void *buf, uint32_t size, uint32_t offset);
int ramfs_write(const char *path, const void *buf, uint32_t size, uint32_t offset);
int ramfs_rename(const char *old, const char *new_path);
int ramfs_mkdir(const char *path);

int  ramfs_open(const char *path, int flags);
int  ramfs_close(int fd);

void ramfs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes, uint32_t *used_size, uint32_t *total_size);
int ramfs_chmod(const char *path, uint32_t mode);

#endif
