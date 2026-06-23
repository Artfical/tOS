#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "serial.h"

typedef struct {
    char path[VFS_NAME_LEN];
    int path_len;
    vfs_ops_t *ops;
    void *private_data;
} mount_t;

typedef struct {
    int used;
    int fd;
    int mount_idx;
    char path[VFS_NAME_LEN];
    uint32_t offset;
    int flags;
} fd_entry_t;

static mount_t mounts[VFS_MAX_MOUNTS];
static fd_entry_t fd_table[VFS_MAX_FDS];
static int mount_count = 0;
static int next_fd = 3;

static char cwd[VFS_NAME_LEN] = "/";

int vfs_init(void)
{
    memset(mounts, 0, sizeof(mounts));
    memset(fd_table, 0, sizeof(fd_table));
    mount_count = 0;
    next_fd = 3;
    serial_write("vfs: initialized\n");
    return 0;
}

int vfs_mount(const char *path, vfs_ops_t *ops, void *private_data)
{
    if (mount_count >= VFS_MAX_MOUNTS) return -1;
    mount_t *m = &mounts[mount_count];
    int i = 0;
    while (path[i] && i < VFS_NAME_LEN - 1) { m->path[i] = path[i]; i++; }
    m->path[i] = 0;
    m->path_len = i;
    m->ops = ops;
    m->private_data = private_data;
    mount_count++;
    return 0;
}

static mount_t *find_mount(const char *path)
{
    mount_t *best = 0;
    int best_len = -1;
    for (int i = 0; i < mount_count; i++) {
        int match = 1;
        for (int j = 0; j < mounts[i].path_len; j++) {
            if (mounts[i].path[j] != path[j]) { match = 0; break; }
        }
        if (match && mounts[i].path_len > best_len) {
            int plen = mounts[i].path_len;
            int ends_with_slash = plen > 0 && mounts[i].path[plen - 1] == '/';
            if (ends_with_slash || path[plen] == '/' || path[plen] == 0) {
                best = &mounts[i];
                best_len = plen;
            }
        }
    }
    return best;
}

static const char *strip_mount(const char *path, mount_t *m)
{
    if (m->path_len == 0) return path;
    const char *sub = path + m->path_len;
    if (*sub == '/') sub++;
    return sub;
}

static void abspath_into(const char *path, char *out)
{
    const char *r = vfs_abspath(path);
    int i = 0;
    while (r[i] && i < VFS_NAME_LEN - 1) { out[i] = r[i]; i++; }
    out[i] = 0;
}

int vfs_open(const char *path, int flags)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);

    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->open) return -1;

    const char *sub = strip_mount(abs, m);
    int fd = m->ops->open(sub, flags);
    if (fd < 0) return -1;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].fd = fd;
            fd_table[i].mount_idx = (int)(m - mounts);
            int j = 0;
            while (abs[j] && j < VFS_NAME_LEN - 1) { fd_table[i].path[j] = abs[j]; j++; }
            fd_table[i].path[j] = 0;
            fd_table[i].offset = 0;
            fd_table[i].flags = flags;
            return i;
        }
    }
    return -1;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    int ret = 0;
    if (m->ops && m->ops->close)
        ret = m->ops->close(fd_table[fd].fd);
    fd_table[fd].used = 0;
    return ret;
}

int vfs_read(int fd, void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->read) return -1;
    return m->ops->read(fd_table[fd].fd, buf, size);
}

int vfs_write(int fd, const void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->write) return -1;
    return m->ops->write(fd_table[fd].fd, buf, size);
}

int vfs_lseek(int fd, uint32_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->lseek) return -1;
    return m->ops->lseek(fd_table[fd].fd, offset, whence);
}

int vfs_readdir(const char *path, vfs_entry_t *entries, int max)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    return m->ops->readdir(strip_mount(abs, m), entries, max);
}

int vfs_mkdir(const char *path, uint32_t mode)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->mkdir) return -1;
    return m->ops->mkdir(strip_mount(abs, m), mode);
}

int vfs_unlink(const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->unlink) return -1;
    return m->ops->unlink(strip_mount(abs, m));
}

int vfs_stat(const char *path, vfs_entry_t *entry)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->stat) return -1;
    return m->ops->stat(strip_mount(abs, m), entry);
}

int vfs_rename(const char *old, const char *new_path)
{
    char abs_old[VFS_NAME_LEN], abs_new[VFS_NAME_LEN];
    abspath_into(old, abs_old);
    abspath_into(new_path, abs_new);
    mount_t *m = find_mount(abs_old);
    if (!m || !m->ops || !m->ops->rename) return -1;
    mount_t *m2 = find_mount(abs_new);
    if (m != m2) return -1;
    return m->ops->rename(strip_mount(abs_old, m), strip_mount(abs_new, m2));
}

int vfs_symlink(const char *target, const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = find_mount(abs);
    if (!m || !m->ops || !m->ops->symlink) return -1;
    return m->ops->symlink(target, strip_mount(abs, m));
}

int vfs_exists(const char *path)
{
    vfs_entry_t e;
    return vfs_stat(path, &e) == 0;
}

char *vfs_abspath(const char *path)
{
    static char buf[VFS_NAME_LEN];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < VFS_NAME_LEN - 1) { buf[i] = path[i]; i++; }
        buf[i] = 0;
    } else {
        int i = 0;
        while (cwd[i] && i < VFS_NAME_LEN - 1) { buf[i] = cwd[i]; i++; }
        if (buf[i-1] != '/') { buf[i] = '/'; i++; }
        int j = 0;
        while (path[j] && i < VFS_NAME_LEN - 1) { buf[i] = path[j]; i++; j++; }
        buf[i] = 0;
    }
    return buf;
}
