#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

#define EXFAT_MAX_FILENAME 255

typedef struct {
    int used;
    uint32_t dir_cluster;
    uint32_t primary_slot;
    int secondary_count;
    uint32_t start_cluster;
    uint32_t size;
    uint32_t pos;
    int dirty;
} exfat_fd_t;

typedef struct {
    blockdev_t *bd;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t cluster_size;
    uint32_t bitmap_cluster;
    uint32_t bitmap_size_bytes;
    exfat_fd_t fds[VFS_MAX_FDS];
} exfat_t;

int exfat_probe_and_mount(exfat_t *fs, blockdev_t *bd);
int exfat_umount(exfat_t *fs);
void exfat_mount_vfs(exfat_t *fs, const char *mount_point);
int exfat_format(blockdev_t *bd, const char *label);

#endif
