#include "ramfs.h"
#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "serial.h"
#include "terminal.h"

static ramfs_inode_t inodes[RAMFS_MAX_INODES];
static uint32_t next_ino = 1;

typedef struct {
    uint32_t ino;
    uint32_t offset;
    int flags;
    int used;
} ramfs_fd_t;

static ramfs_fd_t fds[VFS_MAX_FDS];
static int ramfs_next_fd = 3;

static uint32_t alloc_ino(void)
{
    while (next_ino < RAMFS_MAX_INODES && inodes[next_ino].ino) next_ino++;
    if (next_ino >= RAMFS_MAX_INODES) return 0;
    uint32_t ino = next_ino++;
    return ino;
}

static ramfs_inode_t *iget(uint32_t ino)
{
    if (ino >= RAMFS_MAX_INODES || !inodes[ino].ino) return 0;
    return &inodes[ino];
}

static int name_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}


static int find_child(uint32_t dir_ino, const char *name)
{
    ramfs_inode_t *d = iget(dir_ino);
    if (!d || !(d->mode & S_IFDIR)) return 0;
    ramfs_dir_entry_t *de = (ramfs_dir_entry_t *)d->data;
    int n = d->size / sizeof(ramfs_dir_entry_t);
    for (int i = 0; i < n; i++)
        if (name_eq(de[i].name, name)) return de[i].ino;
    return 0;
}

static int add_child(uint32_t dir_ino, uint32_t child_ino, const char *name)
{
    ramfs_inode_t *d = iget(dir_ino);
    if (!d) return -1;
    int n = d->size / sizeof(ramfs_dir_entry_t);
    ramfs_dir_entry_t *de = (ramfs_dir_entry_t *)d->data;
    for (int i = 0; i < n; i++)
        if (name_eq(de[i].name, name)) return -1;
    ramfs_dir_entry_t *new_de = malloc((n + 1) * sizeof(ramfs_dir_entry_t));
    if (!new_de) return -1;
    for (int i = 0; i < n; i++) new_de[i] = de[i];
    int k = 0;
    while (name[k] && k < RAMFS_NAME_LEN - 1) { new_de[n].name[k] = name[k]; k++; }
    new_de[n].name[k] = 0;
    new_de[n].ino = child_ino;
    if (d->data) free(d->data);
    d->data = (uint8_t *)new_de;
    d->size = (n + 1) * sizeof(ramfs_dir_entry_t);
    return 0;
}

static void del_child(uint32_t dir_ino, const char *name)
{
    ramfs_inode_t *d = iget(dir_ino);
    if (!d) return;
    int n = d->size / sizeof(ramfs_dir_entry_t);
    ramfs_dir_entry_t *de = (ramfs_dir_entry_t *)d->data;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (name_eq(de[i].name, name)) found = 1;
        if (found && i + 1 < n) de[i] = de[i + 1];
    }
    if (found) {
        d->size -= sizeof(ramfs_dir_entry_t);
        if (d->size == 0) {
            free(d->data);
            d->data = 0;
        }
    }
}

static int follow_link(uint32_t ino, char *out, int out_sz)
{
    ramfs_inode_t *n = iget(ino);
    if (!n || !(n->mode & S_IFLNK) || !n->data) return -1;
    int i = 0;
    while ((uint32_t)i < n->size - 1 && i < out_sz - 1) { out[i] = n->data[i]; i++; }
    out[i] = 0;
    return 0;
}

static int resolve_path(const char *path, int follow_final)
{
    if (!path || !path[0]) return 1;
    uint32_t cur = 1;
    int i = 0;
    while (path[i] == '/') i++;
    while (path[i]) {
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int len = i - start;
        char comp[RAMFS_NAME_LEN];
        for (int j = 0; j < len && j < RAMFS_NAME_LEN - 1; j++) comp[j] = path[start + j];
        comp[len] = 0;

        if (name_eq(comp, ".")) { while (path[i] == '/') i++; continue; }
        if (name_eq(comp, "..")) {
            ramfs_inode_t *d = iget(cur);
            if (d) {
                uint32_t parent = find_child(cur, "..");
                if (parent) cur = parent;
            }
            while (path[i] == '/') i++;
            continue;
        }

        uint32_t next = find_child(cur, comp);
        if (!next) return 0;

        ramfs_inode_t *n = iget(next);
        if (n && (n->mode & S_IFLNK) && (follow_final || path[i] != 0)) {
            char target[512];
            if (follow_link(next, target, sizeof(target)) == 0) {
                char full[VFS_NAME_LEN];
                if (target[0] == '/') {
                    int k = 0;
                    while (target[k] && k < VFS_NAME_LEN - 1) { full[k] = target[k]; k++; }
                    full[k] = 0;
                } else {
                    ramfs_inode_t *d = iget(cur);
                    uint32_t p_ino = (d && find_child(cur, "..")) ? find_child(cur, "..") : 1;
                    ramfs_inode_t *p = iget(p_ino);
                    if (p) {
                        int k = 0; while (p->name[k] && k < VFS_NAME_LEN - 1) { full[k] = p->name[k]; k++; }
                        if (k > 0 && full[k-1] != '/') { full[k] = '/'; k++; }
                        int t = 0; while (target[t] && k < VFS_NAME_LEN - 1) { full[k] = target[t]; k++; t++; }
                        full[k] = 0;
                    }
                }
                return resolve_path(full, follow_final);
            }
        }

        cur = next;
        while (path[i] == '/') i++;
    }
    return cur;
}

void ramfs_init(void)
{
    memset(inodes, 0, sizeof(inodes));
    memset(fds, 0, sizeof(fds));
    next_ino = 1;
    ramfs_next_fd = 3;

    uint32_t root = alloc_ino();
    ramfs_inode_t *r = iget(root);
    r->ino = root;
    r->mode = S_IFDIR | 0755;
    r->name[0] = '/'; r->name[1] = 0;
    r->uid = 0; r->gid = 0;
    add_child(root, root, ".");
    serial_write("ramfs: init\n");
}

void ramfs_mount_vfs(void)
{
    static vfs_ops_t ops;
    extern int ramfs_vfs_open(const char *, int);
    extern int ramfs_vfs_close(int);
    extern int ramfs_vfs_read(int, void *, uint32_t);
    extern int ramfs_vfs_write(int, const void *, uint32_t);
    extern int ramfs_vfs_lseek(int, uint32_t, int);
    extern int ramfs_vfs_readdir(const char *, vfs_entry_t *, int);
    extern int ramfs_vfs_unlink(const char *);
    extern int ramfs_vfs_stat(const char *, vfs_entry_t *);
    extern int ramfs_vfs_symlink(const char *, const char *);
    extern int ramfs_mkdir_mode(const char *, uint32_t);
    extern int ramfs_rename(const char *, const char *);
    ops.open    = ramfs_vfs_open;
    ops.close   = ramfs_vfs_close;
    ops.read    = ramfs_vfs_read;
    ops.write   = ramfs_vfs_write;
    ops.lseek   = ramfs_vfs_lseek;
    ops.readdir = ramfs_vfs_readdir;
    ops.mkdir   = ramfs_mkdir_mode;
    ops.unlink  = ramfs_vfs_unlink;
    ops.stat    = ramfs_vfs_stat;
    ops.rename  = ramfs_rename;
    ops.symlink = ramfs_vfs_symlink;
    vfs_mount("/", &ops, 0);
}

static uint32_t make_inode(const char *name, uint32_t mode, uint32_t parent_ino)
{
    uint32_t ino = alloc_ino();
    if (!ino) return 0;
    ramfs_inode_t *n = iget(ino);
    n->ino = ino;
    n->mode = mode;
    n->uid = 0; n->gid = 0;
    int k = 0;
    while (name[k] && k < RAMFS_NAME_LEN - 1) { n->name[k] = name[k]; k++; }
    n->name[k] = 0;
    if (parent_ino && add_child(parent_ino, ino, name) < 0) {
        n->ino = 0;
        return 0;
    }
    if (mode & S_IFDIR) {
        add_child(ino, ino, ".");
        add_child(ino, parent_ino ? parent_ino : ino, "..");
    }
    return ino;
}

int ramfs_vfs_open(const char *path, int flags)
{
    if (!path || !path[0]) return -1;
    uint32_t ino = resolve_path(path, 1);
    if (!ino) {
        if (!(flags & 2)) return -1;
        char parent_path[VFS_NAME_LEN];
        char name_buf[RAMFS_NAME_LEN];
        int i = 0;
        while (path[i]) i++;
        int last_sep = -1;
        for (int j = i - 1; j >= 0; j--) { if (path[j] == '/') { last_sep = j; break; } }
        if (last_sep < 0) { return -1; }
        for (int j = 0; j < last_sep && j < VFS_NAME_LEN - 1; j++) parent_path[j] = path[j];
        parent_path[last_sep < 1 ? 1 : last_sep] = 0;
        int k = 0;
        for (int j = last_sep + 1; path[j] && k < RAMFS_NAME_LEN - 1; j++) name_buf[k++] = path[j];
        name_buf[k] = 0;
        uint32_t p_ino = resolve_path(parent_path, 1);
        if (!p_ino) return -1;
        ino = make_inode(name_buf, S_IFREG | 0644, p_ino);
        if (!ino) return -1;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].ino = ino;
            fds[i].offset = 0;
            fds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

int ramfs_vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    fds[fd].used = 0;
    return 0;
}

int ramfs_vfs_read(int fd, void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    ramfs_inode_t *n = iget(fds[fd].ino);
    if (!n || (n->mode & S_IFDIR)) return -1;
    uint32_t avail = n->size > fds[fd].offset ? n->size - fds[fd].offset : 0;
    if (size > avail) size = avail;
    if (size == 0) return 0;
    memcpy(buf, n->data + fds[fd].offset, size);
    fds[fd].offset += size;
    return size;
}

int ramfs_vfs_write(int fd, const void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    ramfs_inode_t *n = iget(fds[fd].ino);
    if (!n || (n->mode & S_IFDIR)) return -1;
    uint32_t new_size = fds[fd].offset + size;
    if (new_size > n->size) {
        uint8_t *new_data = malloc(new_size);
        if (!new_data) return -1;
        if (n->data) { memcpy(new_data, n->data, n->size); free(n->data); }
        n->data = new_data;
        n->size = new_size;
    }
    memcpy(n->data + fds[fd].offset, buf, size);
    fds[fd].offset += size;
    return size;
}

int ramfs_vfs_lseek(int fd, uint32_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    ramfs_inode_t *n = iget(fds[fd].ino);
    if (!n) return -1;
    if (whence == 0) fds[fd].offset = offset;
    else if (whence == 1) fds[fd].offset += offset;
    else if (whence == 2) fds[fd].offset = n->size + offset;
    if (fds[fd].offset > n->size) fds[fd].offset = n->size;
    return fds[fd].offset;
}

int ramfs_vfs_readdir(const char *path, vfs_entry_t *entries, int max)
{
    uint32_t ino = resolve_path(path, 1);
    if (!ino) return -1;
    ramfs_inode_t *d = iget(ino);
    if (!d || !(d->mode & S_IFDIR)) return -1;

    ramfs_dir_entry_t *de = (ramfs_dir_entry_t *)d->data;
    int n = d->size / sizeof(ramfs_dir_entry_t);
    if (n > max) n = max;

    for (int i = 0; i < n; i++) {
        ramfs_inode_t *c = iget(de[i].ino);
        if (!c) continue;
        int j = 0;
        while (c->name[j] && j < VFS_NAME_LEN - 1) { entries[i].name[j] = c->name[j]; j++; }
        entries[i].name[j] = 0;
        entries[i].size = c->size;
        entries[i].is_dir = (c->mode & S_IFDIR) ? 1 : 0;
        entries[i].inode = c->ino;
        entries[i].mode = c->mode;
    }
    return n;
}

int ramfs_mkdir_mode(const char *path, uint32_t mode)
{
    char parent_path[VFS_NAME_LEN];
    char name_buf[RAMFS_NAME_LEN];
    int i = 0;
    while (path[i]) i++;
    int last_sep = -1;
    for (int j = i - 1; j >= 0; j--) { if (path[j] == '/') { last_sep = j; break; } }
    if (last_sep < 0) return -1;
    for (int j = 0; j < last_sep && j < VFS_NAME_LEN - 1; j++) parent_path[j] = path[j];
    parent_path[last_sep < 1 ? 1 : last_sep] = 0;
    int k = 0;
    for (int j = last_sep + 1; path[j] && k < RAMFS_NAME_LEN - 1; j++) name_buf[k++] = path[j];
    name_buf[k] = 0;
    uint32_t p_ino = resolve_path(parent_path, 1);
    if (!p_ino) return -1;
    if (find_child(p_ino, name_buf)) return -1;
    uint32_t ino = make_inode(name_buf, S_IFDIR | (mode & 0777), p_ino);
    return ino ? 0 : -1;
}

int ramfs_vfs_unlink(const char *path)
{
    uint32_t ino = resolve_path(path, 0);
    if (!ino) return -1;
    ramfs_inode_t *n = iget(ino);
    if (!n) return -1;
    if (n->mode & S_IFDIR) return -1;

    char parent_path[VFS_NAME_LEN];
    char name_buf[RAMFS_NAME_LEN];
    int i = 0;
    while (path[i]) i++;
    int last_sep = -1;
    for (int j = i - 1; j >= 0; j--) { if (path[j] == '/') { last_sep = j; break; } }
    if (last_sep < 0) return -1;
    for (int j = 0; j < last_sep && j < VFS_NAME_LEN - 1; j++) parent_path[j] = path[j];
    parent_path[last_sep < 1 ? 1 : last_sep] = 0;
    int k = 0;
    for (int j = last_sep + 1; path[j] && k < RAMFS_NAME_LEN - 1; j++) name_buf[k++] = path[j];
    name_buf[k] = 0;

    uint32_t p_ino = resolve_path(parent_path, 1);
    if (!p_ino) return -1;
    if (n->data) free(n->data);
    n->ino = 0;
    del_child(p_ino, name_buf);
    return 0;
}

int ramfs_vfs_stat(const char *path, vfs_entry_t *entry)
{
    uint32_t ino = resolve_path(path, 1);
    if (!ino) return -1;
    ramfs_inode_t *n = iget(ino);
    if (!n) return -1;
    int j = 0;
    while (n->name[j] && j < VFS_NAME_LEN - 1) { entry->name[j] = n->name[j]; j++; }
    entry->name[j] = 0;
    entry->size = n->size;
    entry->is_dir = (n->mode & S_IFDIR) ? 1 : 0;
    entry->inode = n->ino;
    entry->mode = n->mode;
    return 0;
}

int ramfs_rename(const char *old_path, const char *new_path)
{
    uint32_t old_ino = resolve_path(old_path, 0);
    if (!old_ino) return -1;
    ramfs_inode_t *n = iget(old_ino);
    if (!n) return -1;

    char old_parent[VFS_NAME_LEN], old_name[RAMFS_NAME_LEN];
    char new_parent[VFS_NAME_LEN], new_name[RAMFS_NAME_LEN];
    int i;

    i = 0; while (old_path[i]) i++;
    int os = -1; for (int j = i-1; j >= 0; j--) { if (old_path[j]=='/') { os=j; break; } }
    for (int j = 0; j < (os<1?1:os) && j < VFS_NAME_LEN-1; j++) old_parent[j] = old_path[j];
    old_parent[os<1?1:os] = 0;
    int k = 0; for (int j = os+1; old_path[j] && k < RAMFS_NAME_LEN-1; j++) old_name[k++] = old_path[j];
    old_name[k] = 0;

    i = 0; while (new_path[i]) i++;
    int ns = -1; for (int j = i-1; j >= 0; j--) { if (new_path[j]=='/') { ns=j; break; } }
    for (int j = 0; j < (ns<1?1:ns) && j < VFS_NAME_LEN-1; j++) new_parent[j] = new_path[j];
    new_parent[ns<1?1:ns] = 0;
    k = 0; for (int j = ns+1; new_path[j] && k < RAMFS_NAME_LEN-1; j++) new_name[k++] = new_path[j];
    new_name[k] = 0;

    uint32_t op_ino = resolve_path(old_parent, 1);
    uint32_t np_ino = resolve_path(new_parent, 1);
    if (!op_ino || !np_ino) return -1;

    if (op_ino != np_ino) return -1;
    del_child(op_ino, old_name);
    add_child(op_ino, old_ino, new_name);
    k = 0; while (new_name[k] && k < RAMFS_NAME_LEN - 1) { n->name[k] = new_name[k]; k++; }
    n->name[k] = 0;
    return 0;
}

int ramfs_vfs_symlink(const char *target, const char *path)
{
    char parent_path[VFS_NAME_LEN];
    char name_buf[RAMFS_NAME_LEN];
    int i = 0;
    while (path[i]) i++;
    int last_sep = -1;
    for (int j = i - 1; j >= 0; j--) { if (path[j] == '/') { last_sep = j; break; } }
    if (last_sep < 0) return -1;
    for (int j = 0; j < last_sep && j < VFS_NAME_LEN - 1; j++) parent_path[j] = path[j];
    parent_path[last_sep < 1 ? 1 : last_sep] = 0;
    int k = 0;
    for (int j = last_sep + 1; path[j] && k < RAMFS_NAME_LEN - 1; j++) name_buf[k++] = path[j];
    name_buf[k] = 0;

    uint32_t p_ino = resolve_path(parent_path, 1);
    if (!p_ino) return -1;

    uint32_t ino = make_inode(name_buf, S_IFLNK | 0777, p_ino);
    if (!ino) return -1;

    ramfs_inode_t *n = iget(ino);
    int tlen = 0;
    while (target[tlen]) tlen++;
    n->data = malloc(tlen + 1);
    if (!n->data) return -1;
    for (int j = 0; j < tlen; j++) n->data[j] = target[j];
    n->data[tlen] = 0;
    n->size = tlen + 1;
    return 0;
}

void ramfs_import_initrd(uint32_t addr, uint32_t size)
{
    uint8_t *p = (uint8_t *)addr;
    uint8_t *end = p + size;
    serial_write("ramfs: importing initrd\n");

    while (p + 512 <= end) {
        char name[100];
        for (int i = 0; i < 100; i++) name[i] = p[i];
        name[99] = 0;
        if (name[0] == 0) break;

        uint32_t fsize = 0;
        for (int i = 0; i < 12; i++) {
            char c = p[124 + i];
            if (c >= '0' && c <= '7') fsize = (fsize << 3) | (c - '0');
        }

        uint32_t ino = resolve_path(name, 0);
        if (!ino) {
            char dir_path[VFS_NAME_LEN];
            int k = 0;
            while (name[k] && k < VFS_NAME_LEN - 1) { dir_path[k] = name[k]; k++; }
            dir_path[k] = 0;
            for (int j = 0; j < k; j++) {
                if (dir_path[j] == '/') {
                    dir_path[j] = 0;
                    if (!resolve_path(dir_path, 0))
                        ramfs_mkdir_mode(dir_path, 0755);
                    dir_path[j] = '/';
                }
            }
            uint32_t f_ino = make_inode(name + k - 1, S_IFREG | 0644, resolve_path(dir_path, 1));
            if (f_ino && fsize > 0) {
                ramfs_inode_t *fn = iget(f_ino);
                fn->data = malloc(fsize);
                if (fn->data) {
                    for (uint32_t j = 0; j < fsize; j++) fn->data[j] = p[512 + j];
                    fn->size = fsize;
                }
            }
        }
        p += 512 * ((fsize + 511) / 512 + 1);
    }
    serial_write("ramfs: initrd imported\n");
}

static char ramfs_cwd[VFS_NAME_LEN] = "/";

const char *ramfs_getcwd(void) { return ramfs_cwd; }

int ramfs_chdir(const char *path)
{
    vfs_entry_t e;
    char *abs = vfs_abspath(path);
    if (vfs_stat(abs, &e) < 0 || !e.is_dir) return -1;
    int i = 0;
    while (abs[i] && i < VFS_NAME_LEN - 1) { ramfs_cwd[i] = abs[i]; i++; }
    ramfs_cwd[i] = 0;
    return 0;
}

int ramfs_list(const char *path, vfs_entry_t *entries, int max)
{
    return vfs_readdir(path, entries, max);
}

int ramfs_create(const char *path)
{
    int fd = vfs_open(path, 2);
    if (fd >= 0) vfs_close(fd);
    return (fd >= 0) ? 0 : -1;
}

int ramfs_delete(const char *path)
{
    return vfs_unlink(path);
}

int ramfs_exists(const char *path)
{
    return vfs_exists(path);
}

int ramfs_is_dir(const char *path)
{
    vfs_entry_t e;
    if (vfs_stat(path, &e) < 0) return 0;
    return e.is_dir;
}

uint32_t ramfs_size(const char *path)
{
    vfs_entry_t e;
    if (vfs_stat(path, &e) < 0) return 0;
    return e.size;
}

int ramfs_read(const char *path, void *buf, uint32_t size, uint32_t offset)
{
    int fd = vfs_open(path, 0);
    if (fd < 0) return -1;
    vfs_lseek(fd, offset, 0);
    int n = vfs_read(fd, buf, size);
    vfs_close(fd);
    return n;
}

int ramfs_write(const char *path, const void *buf, uint32_t size, uint32_t offset)
{
    int fd = vfs_open(path, 2);
    if (fd < 0) return -1;
    vfs_lseek(fd, offset, 0);
    int n = vfs_write(fd, buf, size);
    vfs_close(fd);
    return n;
}

int ramfs_mkdir(const char *path)
{
    return ramfs_mkdir_mode(path, 0755);
}
