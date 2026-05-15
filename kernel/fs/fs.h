#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define FS_MAX_FILES 64
#define FS_NAME_LEN 128

typedef struct {
    char name[FS_NAME_LEN];
    uint32_t size;
    uint32_t offset;
    int exists;
} fs_file_t;

void fs_init(uint32_t addr, uint32_t size);
int fs_list(fs_file_t *files, int max);
int fs_open(const char *name, fs_file_t *file);
int fs_read(fs_file_t *file, void *buf, uint32_t size, uint32_t offset);
int fs_exists(const char *name);

#endif
