#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define FAT32_MAX_FILENAME 13

typedef struct {
    int used;
    uint32_t dirent_sector;
    uint32_t dirent_offset;
    uint32_t start_cluster;
    uint32_t size;
    uint32_t pos;
    int dirty;
} fat32_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t data_start_sector;
    uint32_t root_cluster;
    uint32_t cluster_size;
    fat32_fd_t fds[VFS_MAX_FDS];
} fat32_t;

int fat32_probe_and_mount(fat32_t *fs, blockdev_t *bd);
int fat32_umount(fat32_t *fs);
void fat32_mount_vfs(fat32_t *fs, const char *mount_point);
int fat32_format(blockdev_t *bd, const char *label);

#endif
