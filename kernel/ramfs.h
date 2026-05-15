#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>

#define RAMFS_NAME_LEN 64
#define RAMFS_SEP '/'

typedef struct ramfs_node {
    char name[RAMFS_NAME_LEN];
    int is_dir;
    uint32_t size;
    uint8_t *data;
    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *next;
} ramfs_node_t;

typedef struct {
    char name[RAMFS_NAME_LEN];
    uint32_t size;
    int is_dir;
} ramfs_entry_t;

void ramfs_init(void);
void ramfs_import_initrd(void);
int ramfs_mkdir(const char *path);
int ramfs_create(const char *path);
int ramfs_delete(const char *path);
int ramfs_read(const char *path, void *buf, uint32_t size, uint32_t offset);
int ramfs_write(const char *path, const void *buf, uint32_t size, uint32_t offset);
int ramfs_list(const char *path, ramfs_entry_t *entries, int max);
int ramfs_exists(const char *path);
int ramfs_is_dir(const char *path);
uint32_t ramfs_size(const char *path);
const char *ramfs_getcwd(void);
int ramfs_chdir(const char *path);
int ramfs_rename(const char *old_path, const char *new_path);

#endif
