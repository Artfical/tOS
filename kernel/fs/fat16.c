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
    int is_root;
    uint32_t cluster;
} fat16_dirref_t;

static int fat16_read_sector(fat16_t *fs, uint32_t sector, void *buf)
{
    return blockdev_read(fs->bd, sector, 1, buf);
}

static int fat16_write_sector(fat16_t *fs, uint32_t sector, const void *buf)
{
    return blockdev_write(fs->bd, sector, 1, buf);
}

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

static uint32_t fat16_cluster_to_sector(fat16_t *fs, uint32_t cluster)
{
    if (cluster == 0) return fs->root_dir_sector;
    return fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t fat16_alloc_cluster(fat16_t *fs)
{
    uint32_t data_sectors = fs->total_sectors - fs->data_start_sector;
    uint32_t total_clusters = data_sectors / fs->sectors_per_cluster;

    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat16_get_fat_entry(fs, c) == 0) return c;
    }
    return 0;
}

static void fat16_free_chain(fat16_t *fs, uint32_t cluster)
{
    while (cluster != 0 && cluster < FAT16_CLUSTER_BAD) {
        uint16_t next = fat16_get_fat_entry(fs, cluster);
        fat16_set_fat_entry(fs, cluster, 0);
        cluster = next;
    }
}

static void fat16_name_to_83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[j++] = c;
        i++;
    }
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        int k = 0;
        while (name[i] && k < 3) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[8 + k] = c;
            k++;
            i++;
        }
    }
}

static void fat16_83_to_name(const fat16_dirent_t *de, char *out)
{
    int j = 0;
    for (int i = 0; i < 8 && de->name[i] != ' '; i++) out[j++] = de->name[i];
    int has_ext = 0;
    for (int i = 0; i < 3; i++) if (de->ext[i] != ' ') has_ext = 1;
    if (has_ext) {
        out[j++] = '.';
        for (int i = 0; i < 3 && de->ext[i] != ' '; i++) out[j++] = de->ext[i];
    }
    out[j] = 0;
}

static int fat16_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
{
    int len = (int)strlen(path);
    int last_sep = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_sep = i; break; }
    }
    if (last_sep < 0) {
        if (parent_sz < 1) return -1;
        parent[0] = 0;
        size_t k = 0;
        while (path[k] && k < name_sz - 1) { name[k] = path[k]; k++; }
        name[k] = 0;
        return 0;
    }
    if ((size_t)last_sep >= parent_sz) return -1;
    int i;
    for (i = 0; i < last_sep; i++) parent[i] = path[i];
    parent[i] = 0;
    size_t k = 0;
    for (int j = last_sep + 1; path[j] && k < name_sz - 1; j++) name[k++] = path[j];
    name[k] = 0;
    return 0;
}

static int fat16_dir_nth_sector(fat16_t *fs, int is_root, uint32_t first_cluster, uint32_t n, uint32_t *out_sector)
{
    if (is_root) {
        if (n >= fs->root_dir_sectors) return -1;
        *out_sector = fs->root_dir_sector + n;
        return 0;
    }

    uint32_t cluster = first_cluster;
    uint32_t clusters_to_skip = n / fs->sectors_per_cluster;
    uint32_t sector_in_cluster = n % fs->sectors_per_cluster;

    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        uint16_t e = fat16_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT16_CLUSTER_BAD) return -1;
        cluster = e;
    }
    if (cluster == 0 || cluster >= FAT16_CLUSTER_BAD) return -1;

    *out_sector = fat16_cluster_to_sector(fs, cluster) + sector_in_cluster;
    return 0;
}

static int fat16_dir_find(fat16_t *fs, int is_root, uint32_t dir_cluster, const char *name,
                           fat16_dirent_t *out, uint32_t *out_sector, uint32_t *out_offset)
{
    char want[11];
    fat16_name_to_83(name, want);

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;

    int entries_per_sector = fs->bytes_per_sector / sizeof(fat16_dirent_t);

    for (uint32_t n = 0; ; n++) {
        uint32_t sec;
        if (fat16_dir_nth_sector(fs, is_root, dir_cluster, n, &sec) != 0) break;
        if (fat16_read_sector(fs, sec, buf) != 0) break;

        for (int i = 0; i < entries_per_sector; i++) {
            fat16_dirent_t *de = (fat16_dirent_t *)(buf + i * sizeof(fat16_dirent_t));
            if (de->name[0] == 0x00) { free(buf); return -1; }
            if ((uint8_t)de->name[0] == 0xE5) continue;
            if (de->attr == FAT16_ATTR_VOLUME) continue;

            char cmp[11];
            memcpy(cmp, de->name, 8);
            memcpy(cmp + 8, de->ext, 3);
            if (memcmp(cmp, want, 11) == 0) {
                if (out) *out = *de;
                if (out_sector) *out_sector = sec;
                if (out_offset) *out_offset = i * sizeof(fat16_dirent_t);
                free(buf);
                return 0;
            }
        }
    }

    free(buf);
    return -1;
}

static int fat16_dir_add_entry(fat16_t *fs, int is_root, uint32_t dir_cluster, const fat16_dirent_t *entry,
                                uint32_t *out_sector, uint32_t *out_offset)
{
    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;

    int entries_per_sector = fs->bytes_per_sector / sizeof(fat16_dirent_t);
    uint32_t n = 0;

    while (1) {
        uint32_t sec;
        int rc = fat16_dir_nth_sector(fs, is_root, dir_cluster, n, &sec);
        if (rc != 0) {
            if (is_root) { free(buf); return -1; }

            uint32_t cur = dir_cluster;
            uint16_t e;
            while ((e = fat16_get_fat_entry(fs, cur)) != 0 && e < FAT16_CLUSTER_BAD) cur = e;

            uint32_t new_cluster = fat16_alloc_cluster(fs);
            if (new_cluster == 0) { free(buf); return -1; }
            fat16_set_fat_entry(fs, cur, new_cluster);
            fat16_set_fat_entry(fs, new_cluster, FAT16_CLUSTER_EOF);

            memset(buf, 0, fs->bytes_per_sector);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
                fat16_write_sector(fs, fat16_cluster_to_sector(fs, new_cluster) + s, buf);

            continue;
        }

        if (fat16_read_sector(fs, sec, buf) != 0) { free(buf); return -1; }

        for (int i = 0; i < entries_per_sector; i++) {
            fat16_dirent_t *de = (fat16_dirent_t *)(buf + i * sizeof(fat16_dirent_t));
            if (de->name[0] == 0x00 || (uint8_t)de->name[0] == 0xE5) {
                memcpy(de, entry, sizeof(fat16_dirent_t));
                if (fat16_write_sector(fs, sec, buf) != 0) { free(buf); return -1; }
                if (out_sector) *out_sector = sec;
                if (out_offset) *out_offset = i * sizeof(fat16_dirent_t);
                free(buf);
                return 0;
            }
        }
        n++;
    }
}

static int fat16_dir_lookup_component(fat16_t *fs, fat16_dirref_t *dir, const char *comp, fat16_dirref_t *out)
{
    fat16_dirent_t de;
    if (fat16_dir_find(fs, dir->is_root, dir->cluster, comp, &de, NULL, NULL) != 0) return -1;
    if (!(de.attr & FAT16_ATTR_DIRECTORY)) return -1;

    uint32_t cl = de.cluster_low | ((uint32_t)de.cluster_high << 16);
    if (cl == 0) {
        out->is_root = 1;
        out->cluster = 0;
    } else {
        out->is_root = 0;
        out->cluster = cl;
    }
    return 0;
}

static int fat16_walk(fat16_t *fs, const char *path, fat16_dirref_t *out)
{
    fat16_dirref_t cur = { 1, 0 };

    while (*path == '/') path++;
    if (!*path) { *out = cur; return 0; }

    char comp[13];
    while (*path) {
        int i = 0;
        while (*path && *path != '/' && i < 12) comp[i++] = *path++;
        comp[i] = 0;
        while (*path == '/') path++;

        if (comp[0] == '.' && comp[1] == 0) continue;

        fat16_dirref_t next;
        if (fat16_dir_lookup_component(fs, &cur, comp, &next) != 0) return -1;
        cur = next;
    }
    *out = cur;
    return 0;
}

static int fat16_read_data(fat16_t *fs, uint32_t start_cluster, uint32_t file_size, uint32_t offset,
                            void *buf, uint32_t size)
{
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;
    if (size == 0) return 0;
    if (start_cluster == 0) return 0;

    uint32_t cluster_index = offset / fs->cluster_size;
    uint32_t cluster_off = offset % fs->cluster_size;

    uint32_t cluster = start_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint16_t e = fat16_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT16_CLUSTER_BAD) return 0;
        cluster = e;
    }

    uint8_t *sec_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!sec_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        if (cluster == 0 || cluster >= FAT16_CLUSTER_BAD) break;

        uint32_t sector_in_cluster = cluster_off / fs->bytes_per_sector;
        uint32_t byte_in_sector = cluster_off % fs->bytes_per_sector;
        uint32_t sector = fat16_cluster_to_sector(fs, cluster) + sector_in_cluster;

        if (fat16_read_sector(fs, sector, sec_buf) != 0) break;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;
        memcpy((uint8_t *)buf + done, sec_buf + byte_in_sector, chunk);

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size) {
            cluster_off = 0;
            uint16_t e = fat16_get_fat_entry(fs, cluster);
            cluster = (e == 0 || e >= FAT16_CLUSTER_BAD) ? 0 : e;
        }
    }

    free(sec_buf);
    return (int)done;
}

static int fat16_write_data(fat16_t *fs, uint32_t *start_cluster, uint32_t offset, const void *buf, uint32_t size)
{
    if (*start_cluster == 0) {
        uint32_t c = fat16_alloc_cluster(fs);
        if (c == 0) return -1;
        fat16_set_fat_entry(fs, c, FAT16_CLUSTER_EOF);
        *start_cluster = c;
    }

    uint32_t cluster_index = offset / fs->cluster_size;
    uint32_t cluster_off = offset % fs->cluster_size;

    uint32_t cluster = *start_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint16_t e = fat16_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT16_CLUSTER_BAD) {
            uint32_t nc = fat16_alloc_cluster(fs);
            if (nc == 0) return -1;
            fat16_set_fat_entry(fs, cluster, nc);
            fat16_set_fat_entry(fs, nc, FAT16_CLUSTER_EOF);
            cluster = nc;
        } else {
            cluster = e;
        }
    }

    uint8_t *sec_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!sec_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        uint32_t sector_in_cluster = cluster_off / fs->bytes_per_sector;
        uint32_t byte_in_sector = cluster_off % fs->bytes_per_sector;
        uint32_t sector = fat16_cluster_to_sector(fs, cluster) + sector_in_cluster;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;

        if (chunk < fs->bytes_per_sector) {
            if (fat16_read_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }
        }
        memcpy(sec_buf + byte_in_sector, (const uint8_t *)buf + done, chunk);
        if (fat16_write_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size && done < size) {
            cluster_off = 0;
            uint16_t e = fat16_get_fat_entry(fs, cluster);
            if (e == 0 || e >= FAT16_CLUSTER_BAD) {
                uint32_t nc = fat16_alloc_cluster(fs);
                if (nc == 0) break;
                fat16_set_fat_entry(fs, cluster, nc);
                fat16_set_fat_entry(fs, nc, FAT16_CLUSTER_EOF);
                cluster = nc;
            } else {
                cluster = e;
            }
        }
    }

    free(sec_buf);
    return (int)done;
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
    fat16_t *fs = (fat16_t *)ctx;

    char parent_path[256];
    char name[13];
    if (fat16_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    fat16_dirref_t parent;
    if (fat16_walk(fs, parent_path, &parent) != 0) return -1;

    fat16_dirent_t de;
    uint32_t sec = 0, off = 0;
    int found = (fat16_dir_find(fs, parent.is_root, parent.cluster, name, &de, &sec, &off) == 0);

    if (!found) {
        if (!(flags & VFS_CREAT)) return -1;

        fat16_dirent_t newde;
        memset(&newde, 0, sizeof(newde));
        char want[11];
        fat16_name_to_83(name, want);
        memcpy(newde.name, want, 8);
        memcpy(newde.ext, want + 8, 3);
        newde.attr = FAT16_ATTR_ARCHIVE;

        if (fat16_dir_add_entry(fs, parent.is_root, parent.cluster, &newde, &sec, &off) != 0) return -1;
        de = newde;
    } else if (flags & VFS_TRUNC) {
        uint32_t cl = de.cluster_low | ((uint32_t)de.cluster_high << 16);
        fat16_free_chain(fs, cl);
        de.cluster_low = 0;
        de.cluster_high = 0;
        de.file_size = 0;

        uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
        if (buf) {
            if (fat16_read_sector(fs, sec, buf) == 0) {
                memcpy(buf + off, &de, sizeof(de));
                fat16_write_sector(fs, sec, buf);
            }
            free(buf);
        }
    }

    if (de.attr & FAT16_ATTR_DIRECTORY) return -1;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].dirent_sector = sec;
            fs->fds[i].dirent_offset = off;
            fs->fds[i].start_cluster = de.cluster_low | ((uint32_t)de.cluster_high << 16);
            fs->fds[i].size = de.file_size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? de.file_size : 0;
            fs->fds[i].dirty = 0;
            return i;
        }
    }
    return -1;
}

static int fat16_vfs_close(void *ctx, int fd)
{
    fat16_t *fs = (fat16_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (fs->fds[fd].dirty) {
        uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
        if (buf) {
            if (fat16_read_sector(fs, fs->fds[fd].dirent_sector, buf) == 0) {
                fat16_dirent_t *de = (fat16_dirent_t *)(buf + fs->fds[fd].dirent_offset);
                de->file_size = fs->fds[fd].size;
                de->cluster_low = (uint16_t)(fs->fds[fd].start_cluster & 0xFFFF);
                de->cluster_high = (uint16_t)((fs->fds[fd].start_cluster >> 16) & 0xFFFF);
                fat16_write_sector(fs, fs->fds[fd].dirent_sector, buf);
            }
            free(buf);
        }
    }

    fs->fds[fd].used = 0;
    return 0;
}

static int fat16_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    fat16_t *fs = (fat16_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = fat16_read_data(fs, fs->fds[fd].start_cluster, fs->fds[fd].size, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += n;
    return n;
}

static int fat16_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    fat16_t *fs = (fat16_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = fat16_write_data(fs, &fs->fds[fd].start_cluster, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += n;
        if (fs->fds[fd].pos > fs->fds[fd].size) fs->fds[fd].size = fs->fds[fd].pos;
        fs->fds[fd].dirty = 1;
    }
    return n;
}

static int fat16_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    fat16_t *fs = (fat16_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int fat16_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    fat16_t *fs = (fat16_t *)ctx;

    fat16_dirref_t dir;
    if (fat16_walk(fs, path, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;

    int entries_per_sector = fs->bytes_per_sector / sizeof(fat16_dirent_t);
    int count = 0;

    for (uint32_t n = 0; count < max; n++) {
        uint32_t sec;
        if (fat16_dir_nth_sector(fs, dir.is_root, dir.cluster, n, &sec) != 0) break;
        if (fat16_read_sector(fs, sec, buf) != 0) break;

        int stop = 0;
        for (int i = 0; i < entries_per_sector && count < max; i++) {
            fat16_dirent_t *de = (fat16_dirent_t *)(buf + i * sizeof(fat16_dirent_t));
            if (de->name[0] == 0x00) { stop = 1; break; }
            if ((uint8_t)de->name[0] == 0xE5) continue;
            if (de->attr == FAT16_ATTR_VOLUME) continue;

            char nm[13];
            fat16_83_to_name(de, nm);
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;

            int k = 0;
            while (nm[k] && k < VFS_NAME_LEN - 1) { entries[count].name[k] = nm[k]; k++; }
            entries[count].name[k] = 0;
            entries[count].size = de->file_size;
            entries[count].is_dir = (de->attr & FAT16_ATTR_DIRECTORY) ? 1 : 0;
            entries[count].inode = de->cluster_low | ((uint32_t)de->cluster_high << 16);
            entries[count].mode = de->attr;
            count++;
        }
        if (stop) break;
    }

    free(buf);
    return count;
}

static int fat16_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    fat16_t *fs = (fat16_t *)ctx;

    char parent_path[256];
    char name[13];
    if (fat16_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    fat16_dirref_t parent;
    if (fat16_walk(fs, parent_path, &parent) != 0) return -1;

    fat16_dirent_t existing;
    if (fat16_dir_find(fs, parent.is_root, parent.cluster, name, &existing, NULL, NULL) == 0) return -1;

    uint32_t new_cluster = fat16_alloc_cluster(fs);
    if (new_cluster == 0) return -1;
    fat16_set_fat_entry(fs, new_cluster, FAT16_CLUSTER_EOF);

    uint8_t *buf = (uint8_t *)malloc(fs->cluster_size);
    if (!buf) return -1;
    memset(buf, 0, fs->cluster_size);

    fat16_dirent_t *dot = (fat16_dirent_t *)buf;
    memset(dot->name, ' ', 8);
    memset(dot->ext, ' ', 3);
    dot->name[0] = '.';
    dot->attr = FAT16_ATTR_DIRECTORY;
    dot->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    dot->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    fat16_dirent_t *dotdot = (fat16_dirent_t *)(buf + sizeof(fat16_dirent_t));
    memset(dotdot->name, ' ', 8);
    memset(dotdot->ext, ' ', 3);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT16_ATTR_DIRECTORY;
    uint32_t parent_cluster = parent.is_root ? 0 : parent.cluster;
    dotdot->cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    dotdot->cluster_high = (uint16_t)((parent_cluster >> 16) & 0xFFFF);

    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        fat16_write_sector(fs, fat16_cluster_to_sector(fs, new_cluster) + s, buf + s * fs->bytes_per_sector);
    }
    free(buf);

    fat16_dirent_t newde;
    memset(&newde, 0, sizeof(newde));
    char want[11];
    fat16_name_to_83(name, want);
    memcpy(newde.name, want, 8);
    memcpy(newde.ext, want + 8, 3);
    newde.attr = FAT16_ATTR_DIRECTORY;
    newde.cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    newde.cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    if (fat16_dir_add_entry(fs, parent.is_root, parent.cluster, &newde, NULL, NULL) != 0) return -1;
    return 0;
}

static int fat16_vfs_unlink(void *ctx, const char *path)
{
    fat16_t *fs = (fat16_t *)ctx;

    char parent_path[256];
    char name[13];
    if (fat16_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    fat16_dirref_t parent;
    if (fat16_walk(fs, parent_path, &parent) != 0) return -1;

    fat16_dirent_t de;
    uint32_t sec, off;
    if (fat16_dir_find(fs, parent.is_root, parent.cluster, name, &de, &sec, &off) != 0) return -1;

    if (de.attr & FAT16_ATTR_DIRECTORY) return -1;

    uint32_t cluster = de.cluster_low | ((uint32_t)de.cluster_high << 16);
    fat16_free_chain(fs, cluster);

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;
    if (fat16_read_sector(fs, sec, buf) != 0) { free(buf); return -1; }
    buf[off] = 0xE5;
    int ret = fat16_write_sector(fs, sec, buf);
    free(buf);
    return ret;
}

static int fat16_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    fat16_t *fs = (fat16_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        entry->name[0] = 0;
        entry->size = 0;
        entry->is_dir = 1;
        entry->inode = 0;
        entry->mode = FAT16_ATTR_DIRECTORY;
        return 0;
    }

    char parent_path[256];
    char name[13];
    if (fat16_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    fat16_dirref_t parent;
    if (fat16_walk(fs, parent_path, &parent) != 0) return -1;

    fat16_dirent_t de;
    if (fat16_dir_find(fs, parent.is_root, parent.cluster, name, &de, NULL, NULL) != 0) return -1;

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = de.file_size;
    entry->is_dir = (de.attr & FAT16_ATTR_DIRECTORY) ? 1 : 0;
    entry->inode = de.cluster_low | ((uint32_t)de.cluster_high << 16);
    entry->mode = de.attr;
    return 0;
}

static int fat16_vfs_rename(void *ctx, const char *old, const char *new)
{
    fat16_t *fs = (fat16_t *)ctx;

    char old_parent_path[256], old_name[13];
    if (fat16_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) return -1;
    fat16_dirref_t old_parent;
    if (fat16_walk(fs, old_parent_path, &old_parent) != 0) return -1;

    fat16_dirent_t de;
    uint32_t sec, off;
    if (fat16_dir_find(fs, old_parent.is_root, old_parent.cluster, old_name, &de, &sec, &off) != 0) return -1;

    char new_parent_path[256], new_name[13];
    if (fat16_split_path(new, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) return -1;
    fat16_dirref_t new_parent;
    if (fat16_walk(fs, new_parent_path, &new_parent) != 0) return -1;

    if (new_parent.is_root == old_parent.is_root && new_parent.cluster == old_parent.cluster) {
        char want[11];
        fat16_name_to_83(new_name, want);
        memcpy(de.name, want, 8);
        memcpy(de.ext, want + 8, 3);

        uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
        if (!buf) return -1;
        if (fat16_read_sector(fs, sec, buf) != 0) { free(buf); return -1; }
        memcpy(buf + off, &de, sizeof(de));
        int ret = fat16_write_sector(fs, sec, buf);
        free(buf);
        return ret;
    }

    char want[11];
    fat16_name_to_83(new_name, want);
    fat16_dirent_t newde = de;
    memcpy(newde.name, want, 8);
    memcpy(newde.ext, want + 8, 3);
    if (fat16_dir_add_entry(fs, new_parent.is_root, new_parent.cluster, &newde, NULL, NULL) != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (buf) {
        if (fat16_read_sector(fs, sec, buf) == 0) {
            buf[off] = 0xE5;
            fat16_write_sector(fs, sec, buf);
        }
        free(buf);
    }
    return 0;
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
    uint32_t bytes_per_sector = bd->sector_size ? bd->sector_size : 512;
    uint32_t sectors_per_cluster = 4;
    uint32_t reserved_sectors = 1;
    uint32_t num_fats = 2;
    uint32_t num_dir_entries = 512;

    uint64_t total_sectors64 = bd->total_sectors;
    if (total_sectors64 > 0xFFFFFFFFULL) total_sectors64 = 0xFFFFFFFFULL;
    uint32_t total_sectors = (uint32_t)total_sectors64;

    uint32_t root_dir_sectors = ((num_dir_entries * 32) + bytes_per_sector - 1) / bytes_per_sector;

    uint32_t data_sectors_guess = total_sectors - reserved_sectors - root_dir_sectors;
    uint32_t clusters_guess = data_sectors_guess / sectors_per_cluster;
    uint32_t fat_sectors = ((clusters_guess + 2) * 2 + bytes_per_sector - 1) / bytes_per_sector;
    if (fat_sectors == 0) fat_sectors = 1;

    uint8_t *sector = (uint8_t *)malloc(bytes_per_sector);
    if (!sector) return -1;
    memset(sector, 0, bytes_per_sector);

    fat16_boot_sector_t *bs = (fat16_boot_sector_t *)sector;
    bs->jmp[0] = 0xEB; bs->jmp[1] = 0x3C; bs->jmp[2] = 0x90;
    memcpy(bs->oem, "tOS     ", 8);
    bs->bytes_per_sector = (uint16_t)bytes_per_sector;
    bs->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bs->reserved_sectors = (uint16_t)reserved_sectors;
    bs->num_fats = (uint8_t)num_fats;
    bs->num_dir_entries = (uint16_t)num_dir_entries;
    bs->total_sectors_16 = (uint16_t)((total_sectors <= 0xFFFF) ? total_sectors : 0);
    bs->media = 0xF8;
    bs->fat_size_sectors = (uint16_t)fat_sectors;
    bs->sectors_per_track = 63;
    bs->num_heads = 255;
    bs->hidden_sectors = 0;
    bs->total_sectors_32 = (total_sectors > 0xFFFF) ? total_sectors : 0;
    bs->drive_number = 0x80;
    bs->reserved = 0;
    bs->boot_sig = 0x29;
    bs->volume_id = 0x12345678;
    memset(bs->volume_label, ' ', 11);
    if (label) {
        size_t i = 0;
        while (label[i] && i < 11) { bs->volume_label[i] = label[i]; i++; }
    }
    memcpy(bs->fs_type, "FAT16   ", 8);

    int ret = blockdev_write(bd, 0, 1, sector);
    free(sector);
    if (ret != 0) return -1;

    uint8_t *fat_sector_buf = (uint8_t *)malloc(bytes_per_sector);
    if (!fat_sector_buf) return -1;

    memset(fat_sector_buf, 0, bytes_per_sector);
    fat_sector_buf[0] = 0xF8; fat_sector_buf[1] = 0xFF;
    fat_sector_buf[2] = 0xFF; fat_sector_buf[3] = 0xFF;

    for (uint32_t f = 0; f < num_fats; f++) {
        if (blockdev_write(bd, reserved_sectors + f * fat_sectors, 1, fat_sector_buf) != 0) {
            free(fat_sector_buf);
            return -1;
        }
    }

    memset(fat_sector_buf, 0, bytes_per_sector);
    for (uint32_t f = 0; f < num_fats; f++) {
        for (uint32_t s = 1; s < fat_sectors; s++) {
            if (blockdev_write(bd, reserved_sectors + f * fat_sectors + s, 1, fat_sector_buf) != 0) {
                free(fat_sector_buf);
                return -1;
            }
        }
    }
    free(fat_sector_buf);

    uint32_t root_dir_sector = reserved_sectors + num_fats * fat_sectors;
    uint8_t *zero_sector = (uint8_t *)malloc(bytes_per_sector);
    if (!zero_sector) return -1;
    memset(zero_sector, 0, bytes_per_sector);
    for (uint32_t s = 0; s < root_dir_sectors; s++) {
        if (blockdev_write(bd, root_dir_sector + s, 1, zero_sector) != 0) {
            free(zero_sector);
            return -1;
        }
    }
    free(zero_sector);

    return 0;
}
