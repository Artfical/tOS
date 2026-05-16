#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_FDS      128
#define VFS_MAX_MOUNTS   8
#define VFS_NAME_LEN     128

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#define VFS_RDONLY  0
#define VFS_WRONLY  1
#define VFS_RDWR    2
#define VFS_CREAT   0x100
#define VFS_TRUNC   0x200
#define VFS_APPEND  0x400

typedef struct vfs_entry {
    char name[VFS_NAME_LEN];
    uint32_t size;
    int is_dir;
    uint32_t inode;
    uint32_t mode;
} vfs_entry_t;

typedef struct vfs_ops {
    int   (*open)(const char *path, int flags);
    int   (*close)(int fd);
    int   (*read)(int fd, void *buf, uint32_t size);
    int   (*write)(int fd, const void *buf, uint32_t size);
    int   (*lseek)(int fd, uint32_t offset, int whence);
    int   (*readdir)(const char *path, vfs_entry_t *entries, int max);
    int   (*mkdir)(const char *path, uint32_t mode);
    int   (*unlink)(const char *path);
    int   (*stat)(const char *path, vfs_entry_t *entry);
    int   (*rename)(const char *old, const char *new);
    int   (*symlink)(const char *target, const char *path);
} vfs_ops_t;

int  vfs_init(void);
int  vfs_mount(const char *path, vfs_ops_t *ops, void *private_data);
int  vfs_open(const char *path, int flags);
int  vfs_close(int fd);
int  vfs_read(int fd, void *buf, uint32_t size);
int  vfs_write(int fd, const void *buf, uint32_t size);
int  vfs_lseek(int fd, uint32_t offset, int whence);
int  vfs_readdir(const char *path, vfs_entry_t *entries, int max);
int  vfs_mkdir(const char *path, uint32_t mode);
int  vfs_unlink(const char *path);
int  vfs_stat(const char *path, vfs_entry_t *entry);
int  vfs_rename(const char *old, const char *new);
int  vfs_symlink(const char *target, const char *path);
int  vfs_exists(const char *path);
char *vfs_abspath(const char *path);

#endif
