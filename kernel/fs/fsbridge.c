#include "fsbridge.h"
#include "vfs.h"
#include "ramfs.h"

int fsbridge_exists(const char *path)
{
    if (vfs_path_has_mount(path)) return vfs_exists(path);
    return ramfs_exists(path);
}

int fsbridge_is_dir(const char *path)
{
    if (vfs_path_has_mount(path)) {
        vfs_entry_t e;
        if (vfs_stat(path, &e) != 0) return 0;
        return e.is_dir;
    }
    return ramfs_is_dir(path);
}

uint32_t fsbridge_size(const char *path)
{
    if (vfs_path_has_mount(path)) {
        vfs_entry_t e;
        if (vfs_stat(path, &e) != 0) return 0;
        return e.size;
    }
    return ramfs_size(path);
}

int fsbridge_read(const char *path, void *buf, uint32_t size, uint32_t offset)
{
    if (vfs_path_has_mount(path)) {
        int fd = vfs_open(path, VFS_RDONLY);
        if (fd < 0) return -1;
        vfs_lseek(fd, offset, VFS_SEEK_SET);
        int n = vfs_read(fd, buf, size);
        vfs_close(fd);
        return n;
    }
    return ramfs_read(path, buf, size, offset);
}

int fsbridge_write(const char *path, const void *buf, uint32_t size, uint32_t offset)
{
    if (vfs_path_has_mount(path)) {
        int fd = vfs_open(path, VFS_WRONLY);
        if (fd < 0) return -1;
        vfs_lseek(fd, offset, VFS_SEEK_SET);
        int n = vfs_write(fd, buf, size);
        vfs_close(fd);
        return n;
    }
    return ramfs_write(path, buf, size, offset);
}

int fsbridge_create(const char *path)
{
    if (vfs_path_has_mount(path)) {
        int fd = vfs_open(path, VFS_WRONLY | VFS_CREAT | VFS_TRUNC);
        if (fd < 0) return -1;
        vfs_close(fd);
        return 0;
    }
    return ramfs_create(path);
}

int fsbridge_delete(const char *path)
{
    if (vfs_path_has_mount(path)) return vfs_unlink(path);
    return ramfs_delete(path);
}

int fsbridge_mkdir(const char *path)
{
    if (vfs_path_has_mount(path)) return vfs_mkdir(path, 0);
    return ramfs_mkdir(path);
}

int fsbridge_rename(const char *old, const char *new_path)
{
    int old_vfs = vfs_path_has_mount(old);
    int new_vfs = vfs_path_has_mount(new_path);
    if (old_vfs != new_vfs) return -1;
    if (old_vfs) return vfs_rename(old, new_path);
    return ramfs_rename(old, new_path);
}

int fsbridge_list(const char *path, vfs_entry_t *entries, int max)
{
    if (vfs_path_has_mount(path)) return vfs_readdir(path, entries, max);
    return ramfs_list(path, entries, max);
}
