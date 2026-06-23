#include "fat16.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "stdio.h"

#define FAT16_CLUSTER_EOF 0xFFF8
#define FAT16_CLUSTER_BAD 0xFFF7

typedef struct {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t num_dir_entries;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_sectors;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t boot_sig;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed)) fat16_boot_sector_t;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat16_dirent_t;

#define FAT16_ATTR_READ_ONLY 0x01
#define FAT16_ATTR_HIDDEN    0x02
#define FAT16_ATTR_SYSTEM    0x04
#define FAT16_ATTR_VOLUME    0x08
#define FAT16_ATTR_DIRECTORY 0x10
#define FAT16_ATTR_ARCHIVE   0x20

typedef struct {
    fat16_t *fs;
    fat16_file_t file;
    uint32_t current_cluster;
    uint32_t cluster_pos;
} fat16_fd_t;

static int fat16_read_sector(fat16_t *fs, uint32_t sector, void *buf)
{
    return blockdev_read(fs->bd, sector, 1, buf);
}

static int fat16_write_sector(fat16_t *fs, uint32_t sector, const void *buf)
{
    return blockdev_write(fs->bd, sector, 1, buf);
}

__attribute__((unused))
static uint16_t fat16_get_fat_entry(fat16_t *fs, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 2;
    uint32_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *fat_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!fat_buf) return 0;

    if (fat16_read_sector(fs, fat_sector, fat_buf) != 0) {
        free(fat_buf);
        return 0;
    }

    uint16_t entry = *(uint16_t *)(&fat_buf[sector_offset]);
    free(fat_buf);
    return entry;
}

__attribute__((unused))
static int fat16_set_fat_entry(fat16_t *fs, uint32_t cluster, uint16_t value)
{
    uint32_t fat_offset = cluster * 2;
    uint32_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *fat_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!fat_buf) return -1;

    if (fat16_read_sector(fs, fat_sector, fat_buf) != 0) {
        free(fat_buf);
        return -1;
    }

    *(uint16_t *)(&fat_buf[sector_offset]) = value;
    int ret = fat16_write_sector(fs, fat_sector, fat_buf);

    for (uint32_t i = 1; i < fs->num_fats; i++) {
        fat16_write_sector(fs, fat_sector + i * fs->fat_sectors, fat_buf);
    }

    free(fat_buf);
    return ret;
}

__attribute__((unused))
static uint32_t fat16_cluster_to_sector(fat16_t *fs, uint32_t cluster)
{
    if (cluster == 0) return fs->root_dir_sector;
    return fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
}

static int fat16_probe(fat16_t *fs, blockdev_t *bd)
{
    fs->bd = bd;

    fat16_boot_sector_t *bs = (fat16_boot_sector_t *)malloc(512);
    if (!bs) return -1;

    if (fat16_read_sector(fs, 0, bs) != 0) {
        free(bs);
        return -1;
    }

    fs->bytes_per_sector = bs->bytes_per_sector;
    if (fs->bytes_per_sector == 0) fs->bytes_per_sector = 512;

    fs->sectors_per_cluster = bs->sectors_per_cluster;
    if (fs->sectors_per_cluster == 0) fs->sectors_per_cluster = 1;

    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->num_dir_entries = bs->num_dir_entries;
    fs->fat_sectors = bs->fat_size_sectors;
    fs->total_sectors = bs->total_sectors_16 ? bs->total_sectors_16 : bs->total_sectors_32;

    if (bs->boot_sig != 0x29) {
        free(bs);
        return -1;
    }

    fs->root_dir_sectors = ((fs->num_dir_entries * 32) + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    fs->root_dir_sector = fs->reserved_sectors + (fs->num_fats * fs->fat_sectors);
    fs->data_start_sector = fs->root_dir_sector + fs->root_dir_sectors;
    fs->cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    free(bs);
    return 0;
}

int fat16_probe_and_mount(fat16_t *fs, blockdev_t *bd)
{
    if (fat16_probe(fs, bd) != 0) return -1;
    return 0;
}

int fat16_umount(fat16_t *fs)
{
    (void)fs;
    return 0;
}

static int fat16_vfs_open(void *ctx, const char *path, int flags)
{
    (void)ctx; (void)path; (void)flags;
    return -1;
}

static int fat16_vfs_close(void *ctx, int fd)
{
    (void)ctx; (void)fd;
    return -1;
}

static int fat16_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int fat16_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int fat16_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    (void)ctx; (void)fd; (void)offset; (void)whence;
    return -1;
}

static int fat16_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    (void)ctx; (void)path; (void)entries; (void)max;
    return -1;
}

static int fat16_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx; (void)path; (void)mode;
    return -1;
}

static int fat16_vfs_unlink(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

static int fat16_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    (void)ctx; (void)path; (void)entry;
    return -1;
}

static int fat16_vfs_rename(void *ctx, const char *old, const char *new)
{
    (void)ctx; (void)old; (void)new;
    return -1;
}

static int fat16_vfs_symlink(void *ctx, const char *target, const char *name)
{
    (void)ctx; (void)target; (void)name;
    return -1;
}

void fat16_mount_vfs(fat16_t *fs, const char *mount_point)
{
    static vfs_ops_t fat16_vfs_ops = {
        .open = fat16_vfs_open,
        .close = fat16_vfs_close,
        .read = fat16_vfs_read,
        .write = fat16_vfs_write,
        .lseek = fat16_vfs_lseek,
        .readdir = fat16_vfs_readdir,
        .mkdir = fat16_vfs_mkdir,
        .unlink = fat16_vfs_unlink,
        .stat = fat16_vfs_stat,
        .rename = fat16_vfs_rename,
        .symlink = fat16_vfs_symlink,
    };
    vfs_mount(mount_point, &fat16_vfs_ops, fs);
}

int fat16_format(blockdev_t *bd, const char *label)
{
    (void)bd; (void)label;
    return -1;
}
