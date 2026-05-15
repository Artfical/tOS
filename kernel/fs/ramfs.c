#include "ramfs.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "fs.h"

static ramfs_node_t *root = NULL;
static ramfs_node_t *cwd = NULL;
static char cwd_path[256] = "/";

static ramfs_node_t *node_alloc(const char *name, int is_dir)
{
    ramfs_node_t *n = (ramfs_node_t *)malloc(sizeof(ramfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(ramfs_node_t));
    strncpy(n->name, name, RAMFS_NAME_LEN - 1);
    n->is_dir = is_dir;
    return n;
}

static ramfs_node_t *resolve(const char *path)
{
    if (!path || !*path) return NULL;
    if (strcmp(path, "/") == 0) return root;

    ramfs_node_t *start = root;
    const char *p = path;

    if (*p == RAMFS_SEP) {
        start = root;
        p++;
    } else {
        start = cwd;
    }

    char seg[RAMFS_NAME_LEN];
    while (*p) {
        int i = 0;
        while (*p && *p != RAMFS_SEP && i < RAMFS_NAME_LEN - 1)
            seg[i++] = *p++;
        seg[i] = '\0';

        if (strcmp(seg, ".") == 0) {
            if (*p) p++;
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (start->parent) start = start->parent;
            if (*p) p++;
            continue;
        }

        if (!start->is_dir || !start->children) return NULL;

        ramfs_node_t *child = start->children;
        while (child) {
            if (strcmp(child->name, seg) == 0) break;
            child = child->next;
        }
        if (!child) return NULL;
        start = child;
        if (*p) p++;
    }
    return start;
}

static ramfs_node_t *resolve_parent(const char *path, char *name_buf)
{
    if (!path || !*path) return NULL;

    char work[256];
    strncpy(work, path, 255);

    int len = strlen(work);
    while (len > 0 && work[len - 1] == RAMFS_SEP) {
        work[--len] = '\0';
    }
    if (len == 0) return NULL;

    char *sep = NULL;
    for (int i = len - 1; i >= 0; i--) {
        if (work[i] == RAMFS_SEP) {
            sep = &work[i];
            break;
        }
    }

    if (!sep) {
        strncpy(name_buf, work, RAMFS_NAME_LEN - 1);
        return cwd;
    }

    *sep = '\0';
    strncpy(name_buf, sep + 1, RAMFS_NAME_LEN - 1);

    if (*work == '\0') return root;
    return resolve(work);
}

static void rebuild_cwd_path(void)
{
    if (!cwd) { strcpy(cwd_path, "/"); return; }

    char stack[64][RAMFS_NAME_LEN];
    int sp = 0;
    ramfs_node_t *n = cwd;
    while (n && n != root) {
        strncpy(stack[sp++], n->name, RAMFS_NAME_LEN - 1);
        n = n->parent;
    }

    int pos = 0;
    cwd_path[pos++] = RAMFS_SEP;
    for (int i = sp - 1; i >= 0; i--) {
        int j = 0;
        while (stack[i][j] && pos < 255)
            cwd_path[pos++] = stack[i][j++];
        if (i > 0) cwd_path[pos++] = RAMFS_SEP;
    }
    cwd_path[pos] = '\0';
}

void ramfs_init(void)
{
    root = node_alloc("", 1);
    root->parent = root;
    cwd = root;
    strcpy(cwd_path, "/");
}

void ramfs_import_initrd(void)
{
    fs_file_t files[FS_MAX_FILES];
    int count = fs_list(files, FS_MAX_FILES);
    for (int i = 0; i < count; i++) {
        char path[256];
        path[0] = RAMFS_SEP;
        strncpy(path + 1, files[i].name, 254);
        path[255] = '\0';

        if (ramfs_create(path) != 0) {
            continue;
        }

        void *buf = malloc(files[i].size);
        if (!buf) continue;
        fs_read(&files[i], buf, files[i].size, 0);
        ramfs_write(path, buf, files[i].size, 0);
        free(buf);
    }

    terminal_writestring("Ramfs: initrd imported\n");
}

int ramfs_mkdir(const char *path)
{
    char name[RAMFS_NAME_LEN];
    ramfs_node_t *parent = resolve_parent(path, name);
    if (!parent || !parent->is_dir) return -1;
    if (strlen(name) == 0) return -1;

    ramfs_node_t *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return -1;
        child = child->next;
    }

    ramfs_node_t *n = node_alloc(name, 1);
    if (!n) return -1;
    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return 0;
}

int ramfs_create(const char *path)
{
    char name[RAMFS_NAME_LEN];
    ramfs_node_t *parent = resolve_parent(path, name);
    if (!parent || !parent->is_dir) return -1;
    if (strlen(name) == 0) return -1;

    ramfs_node_t *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return -1;
        child = child->next;
    }

    ramfs_node_t *n = node_alloc(name, 0);
    if (!n) return -1;
    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return 0;
}

int ramfs_delete(const char *path)
{
    char name[RAMFS_NAME_LEN];
    ramfs_node_t *parent = resolve_parent(path, name);
    if (!parent || !parent->is_dir) return -1;

    ramfs_node_t *prev = NULL;
    ramfs_node_t *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) {
            if (!child->is_dir || !child->children) {
                if (prev)
                    prev->next = child->next;
                else
                    parent->children = child->next;

                ramfs_node_t *scan = child->children;
                while (scan) {
                    ramfs_node_t *next = scan->next;
                    if (!scan->is_dir && scan->data) free(scan->data);
                    free(scan);
                    scan = next;
                }

                if (!child->is_dir && child->data) free(child->data);
                free(child);
                return 0;
            }
            return -1;
        }
        prev = child;
        child = child->next;
    }
    return -1;
}

int ramfs_read(const char *path, void *buf, uint32_t size, uint32_t offset)
{
    ramfs_node_t *n = resolve(path);
    if (!n || n->is_dir) return -1;
    if (offset >= n->size) return 0;
    uint32_t readable = n->size - offset;
    if (size > readable) size = readable;
    memcpy(buf, n->data + offset, size);
    return size;
}

int ramfs_write(const char *path, const void *buf, uint32_t size, uint32_t offset)
{
    ramfs_node_t *n = resolve(path);
    if (!n || n->is_dir) return -1;

    uint32_t needed = offset + size;
    if (needed > n->size) {
        uint8_t *new_data = (uint8_t *)malloc(needed);
        if (!new_data) return -1;
        if (n->data) {
            memcpy(new_data, n->data, n->size);
            free(n->data);
        }
        if (needed > n->size)
            memset(new_data + n->size, 0, needed - n->size);
        n->data = new_data;
        n->size = needed;
    }
    memcpy(n->data + offset, buf, size);
    return size;
}

int ramfs_list(const char *path, ramfs_entry_t *entries, int max)
{
    ramfs_node_t *n = resolve(path);
    if (!n || !n->is_dir) return -1;

    int count = 0;
    ramfs_node_t *child = n->children;
    while (child && count < max) {
        strncpy(entries[count].name, child->name, RAMFS_NAME_LEN - 1);
        entries[count].size = child->size;
        entries[count].is_dir = child->is_dir;
        count++;
        child = child->next;
    }
    return count;
}

int ramfs_exists(const char *path)
{
    ramfs_node_t *n = resolve(path);
    return n != NULL;
}

int ramfs_is_dir(const char *path)
{
    ramfs_node_t *n = resolve(path);
    return n && n->is_dir;
}

uint32_t ramfs_size(const char *path)
{
    ramfs_node_t *n = resolve(path);
    return n ? n->size : 0;
}

const char *ramfs_getcwd(void)
{
    return cwd_path;
}

int ramfs_chdir(const char *path)
{
    ramfs_node_t *n = resolve(path);
    if (!n || !n->is_dir) return -1;
    cwd = n;
    rebuild_cwd_path();
    return 0;
}

int ramfs_rename(const char *old_path, const char *new_path)
{
    char name[RAMFS_NAME_LEN];
    ramfs_node_t *parent = resolve_parent(old_path, name);
    if (!parent || !parent->is_dir) return -1;

    ramfs_node_t *node = NULL;
    ramfs_node_t *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) { node = child; break; }
        child = child->next;
    }
    if (!node) return -1;

    char new_name[RAMFS_NAME_LEN];
    ramfs_node_t *new_parent = resolve_parent(new_path, new_name);
    if (!new_parent || !new_parent->is_dir) return -1;

    ramfs_node_t *dup = new_parent->children;
    while (dup) {
        if (strcmp(dup->name, new_name) == 0) return -1;
        dup = dup->next;
    }

    child = parent->children;
    ramfs_node_t *prev = NULL;
    while (child) {
        if (child == node) {
            if (prev) prev->next = child->next;
            else parent->children = child->next;
            break;
        }
        prev = child;
        child = child->next;
    }

    strncpy(node->name, new_name, RAMFS_NAME_LEN - 1);
    node->parent = new_parent;
    node->next = new_parent->children;
    new_parent->children = node;
    return 0;
}
