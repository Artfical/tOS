#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define FAT16_MAX_FILENAME 13

typedef struct {
    int used;
    uint32_t dirent_sector;
    uint32_t dirent_offset;
    uint32_t start_cluster;
    uint32_t size;
    uint32_t pos;
    int dirty;
} fat16_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t num_dir_entries;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t data_start_sector;
    uint32_t root_dir_sector;
    uint32_t root_dir_sectors;
    uint32_t cluster_size;
    fat16_fd_t fds[VFS_MAX_FDS];
} fat16_t;

int fat16_probe_and_mount(fat16_t *fs, blockdev_t *bd);
int fat16_umount(fat16_t *fs);
void fat16_mount_vfs(fat16_t *fs, const char *mount_point);
int fat16_format(blockdev_t *bd, const char *label);

#endif
