#ifndef FSBRIDGE_H
#define FSBRIDGE_H

#include <stdint.h>
#include "vfs.h"

int fsbridge_exists(const char *path);
int fsbridge_is_dir(const char *path);
uint32_t fsbridge_size(const char *path);
int fsbridge_read(const char *path, void *buf, uint32_t size, uint32_t offset);
int fsbridge_write(const char *path, const void *buf, uint32_t size, uint32_t offset);
int fsbridge_create(const char *path);
int fsbridge_delete(const char *path);
int fsbridge_mkdir(const char *path);
int fsbridge_rename(const char *old, const char *new_path);
int fsbridge_list(const char *path, vfs_entry_t *entries, int max);

#endif
