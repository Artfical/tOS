#include "fat32.h"
#include "fat_lfn.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "stdio.h"

#define FAT32_CLUSTER_EOF 0x0FFFFFF8
#define FAT32_CLUSTER_BAD 0x0FFFFFF7

typedef struct {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved_[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_sig;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed)) fat32_boot_sector_t;

typedef struct {
    uint32_t lead_sig;
    uint8_t reserved1[480];
    uint32_t struc_sig;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t reserved2[12];
    uint32_t trail_sig;
} __attribute__((packed)) fat32_fsinfo_t;

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
} __attribute__((packed)) fat32_dirent_t;

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME    0x08
#define FAT32_ATTR_LFN       0x0F   /* long file name fragment */
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

static int fat32_read_sector(fat32_t *fs, uint32_t sector, void *buf)
{
    return blockdev_read(fs->bd, sector, 1, buf);
}

static int fat32_write_sector(fat32_t *fs, uint32_t sector, const void *buf)
{
    return blockdev_write(fs->bd, sector, 1, buf);
}

static uint32_t fat32_get_fat_entry(fat32_t *fs, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *fat_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!fat_buf) return 0;

    if (fat32_read_sector(fs, fat_sector, fat_buf) != 0) {
        free(fat_buf);
        return 0;
    }

    uint32_t entry = (*(uint32_t *)(&fat_buf[sector_offset])) & 0x0FFFFFFF;
    free(fat_buf);
    return entry;
}

/* The driver does not track the free-cluster count, so the first time it
 * allocates or frees a cluster it marks the FSInfo count as unknown
 * (0xFFFFFFFF) instead of leaving a stale number for other systems. */
static void fat32_fsinfo_invalidate(fat32_t *fs)
{
    if (fs->fsinfo_invalid || fs->fsinfo_sector == 0) return;
    fs->fsinfo_invalid = 1;
    uint8_t *b = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!b) return;
    if (blockdev_read(fs->bd, fs->fsinfo_sector, 1, b) == 0) {
        fat32_fsinfo_t *fi = (fat32_fsinfo_t *)b;
        if (fi->lead_sig == 0x41615252 && fi->struc_sig == 0x61417272) {
            fi->free_count = 0xFFFFFFFF;
            fi->next_free = 0xFFFFFFFF;
            blockdev_write(fs->bd, fs->fsinfo_sector, 1, b);
        }
    }
    free(b);
}

static int fat32_set_fat_entry(fat32_t *fs, uint32_t cluster, uint32_t value)
{
    if (cluster >= 2) fat32_fsinfo_invalidate(fs);
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *fat_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!fat_buf) return -1;

    if (fat32_read_sector(fs, fat_sector, fat_buf) != 0) {
        free(fat_buf);
        return -1;
    }

    uint32_t old = *(uint32_t *)(&fat_buf[sector_offset]);
    *(uint32_t *)(&fat_buf[sector_offset]) = (old & 0xF0000000) | (value & 0x0FFFFFFF);
    int ret = fat32_write_sector(fs, fat_sector, fat_buf);

    for (uint32_t i = 1; i < fs->num_fats; i++) {
        fat32_write_sector(fs, fat_sector + i * fs->fat_sectors, fat_buf);
    }

    free(fat_buf);
    return ret;
}

static uint32_t fat32_cluster_to_sector(fat32_t *fs, uint32_t cluster)
{
    return fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t fat32_alloc_cluster(fat32_t *fs)
{
    uint32_t data_sectors = fs->total_sectors - fs->data_start_sector;
    uint32_t total_clusters = data_sectors / fs->sectors_per_cluster;

    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat32_get_fat_entry(fs, c) == 0) return c;
    }
    return 0;
}

static void fat32_free_chain(fat32_t *fs, uint32_t cluster)
{
    fs->dc_first = 0;   /* a cached directory cursor may point into this chain */
    while (cluster != 0 && cluster < FAT32_CLUSTER_BAD) {
        uint32_t next = fat32_get_fat_entry(fs, cluster);
        fat32_set_fat_entry(fs, cluster, 0);
        cluster = next;
    }
}

static int fat32_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
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

static int fat32_dir_nth_sector(fat32_t *fs, uint32_t first_cluster, uint32_t n, uint32_t *out_sector)
{
    uint32_t cluster = first_cluster;
    uint32_t clusters_to_skip = n / fs->sectors_per_cluster;
    uint32_t sector_in_cluster = n % fs->sectors_per_cluster;
    uint32_t start_idx = 0;

    /* resume from where the previous lookup in this directory stopped:
     * scanning a directory slot by slot used to re-walk the whole chain
     * each time (quadratic in the directory size) */
    if (fs->dc_first == first_cluster && fs->dc_idx <= clusters_to_skip && fs->dc_cluster != 0) {
        cluster = fs->dc_cluster;
        start_idx = fs->dc_idx;
    }
    for (uint32_t i = start_idx; i < clusters_to_skip; i++) {
        uint32_t e = fat32_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT32_CLUSTER_BAD) return -1;
        cluster = e;
    }
    if (cluster == 0 || cluster >= FAT32_CLUSTER_BAD) return -1;
    fs->dc_first = first_cluster;
    fs->dc_idx = clusters_to_skip;
    fs->dc_cluster = cluster;

    *out_sector = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;
    return 0;
}

#define FAT32_NAME_MAX 768   /* UTF-8 bytes for a 255-character name */

typedef struct {
    fat32_dirent_t de;
    uint32_t sec, off;      /* where the short entry lives */
    uint32_t slot;          /* its slot (32-byte entry) index in the directory */
    uint32_t first_slot;    /* first slot of its set (long-name fragments + short entry) */
} fat32_loc_t;

/* Directory slots: a directory is a chain of clusters of 32-byte entries. */
static int fat32_slot_loc(fat32_t *fs, uint32_t dir, uint32_t slot, uint32_t *sec, uint32_t *off)
{
    uint32_t per_sector = fs->bytes_per_sector / 32;
    if (fat32_dir_nth_sector(fs, dir, slot / per_sector, sec) != 0) return -1;
    *off = (slot % per_sector) * 32;
    return 0;
}

static int fat32_slot_read(fat32_t *fs, uint32_t dir, uint32_t slot, uint8_t *e32)
{
    uint32_t sec, off;
    if (fat32_slot_loc(fs, dir, slot, &sec, &off) != 0) return -1;
    uint8_t *b = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!b) return -1;
    int rc = fat32_read_sector(fs, sec, b);
    if (rc == 0) memcpy(e32, b + off, 32);
    free(b);
    return rc;
}

static int fat32_slot_write(fat32_t *fs, uint32_t dir, uint32_t slot, const uint8_t *e32)
{
    uint32_t sec, off;
    if (fat32_slot_loc(fs, dir, slot, &sec, &off) != 0) return -1;
    uint8_t *b = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!b) return -1;
    int rc = fat32_read_sector(fs, sec, b);
    if (rc == 0) {
        memcpy(b + off, e32, 32);
        rc = fat32_write_sector(fs, sec, b);
    }
    free(b);
    return rc;
}

/* Appends one zeroed cluster to a directory's chain. */
static int fat32_dir_extend(fat32_t *fs, uint32_t dir)
{
    uint32_t cur = dir;
    uint32_t e;
    while ((e = fat32_get_fat_entry(fs, cur)) != 0 && e < FAT32_CLUSTER_BAD) cur = e;
    uint32_t nc = fat32_alloc_cluster(fs);
    if (nc == 0) return -1;
    fat32_set_fat_entry(fs, cur, nc);
    fat32_set_fat_entry(fs, nc, FAT32_CLUSTER_EOF);
    uint8_t *z = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!z) return -1;
    memset(z, 0, fs->bytes_per_sector);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
        fat32_write_sector(fs, fat32_cluster_to_sector(fs, nc) + s, z);
    free(z);
    return 0;
}

static int fat32_valid_name(const uint16_t *n, int len)
{
    if (len <= 0 || len > FATL_MAX) return 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = n[i];
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    if (n[len - 1] == '.' || n[len - 1] == ' ') return 0;
    if (len == 1 && n[0] == '.') return 0;
    if (len == 2 && n[0] == '.' && n[1] == '.') return 0;
    return 1;
}

/* Looks a name up (long name or 8.3 alias, case-insensitively). */
static int fat32_dir_find_ex(fat32_t *fs, uint32_t dir, const uint16_t *want, int wlen, fat32_loc_t *out)
{
    fatl_state_t lfn;
    fatl_reset(&lfn);
    uint32_t run_first = 0;
    for (uint32_t slot = 0;; slot++) {
        uint8_t e[32];
        if (fat32_slot_read(fs, dir, slot, e) != 0) return -1;
        if (e[0] == 0x00) return -1;
        if (e[0] == 0xE5) { fatl_reset(&lfn); continue; }
        if (e[11] == FAT32_ATTR_LFN) {
            if (e[0] & 0x40) run_first = slot;
            fatl_feed(&lfn, e);
            continue;
        }
        if (e[11] & FAT32_ATTR_VOLUME) { fatl_reset(&lfn); continue; }

        uint16_t sn[16];
        int sl = fatl_short_to_utf16(e, sn);
        uint16_t ln[FATL_MAX + 1];
        int ll = fatl_complete(&lfn, e, ln, FATL_MAX);
        uint32_t first = ll > 0 ? run_first : slot;
        fatl_reset(&lfn);

        if ((ll > 0 && fatl_equal(want, wlen, ln, ll)) || fatl_equal(want, wlen, sn, sl)) {
            memcpy(&out->de, e, 32);
            out->slot = slot;
            out->first_slot = first;
            if (fat32_slot_loc(fs, dir, slot, &out->sec, &out->off) != 0) return -1;
            return 0;
        }
    }
}

static int fat32_dir_find(fat32_t *fs, uint32_t dir, const char *name, fat32_loc_t *out)
{
    uint16_t n16[FATL_MAX + 1];
    int nl = fatl_utf8_to_utf16(name, (int)strlen(name), n16, FATL_MAX);
    if (nl <= 0) return -1;
    return fat32_dir_find_ex(fs, dir, n16, nl, out);
}

static int fat32_short_exists(fat32_t *fs, uint32_t dir, const uint8_t *n11)
{
    for (uint32_t slot = 0;; slot++) {
        uint8_t e[32];
        if (fat32_slot_read(fs, dir, slot, e) != 0) return 0;
        if (e[0] == 0x00) return 0;
        if (e[0] == 0xE5 || e[11] == FAT32_ATTR_LFN) continue;
        if (memcmp(e, n11, 11) == 0) return 1;
    }
}

/* Finds (or makes room for) `count` consecutive free slots. */
static int fat32_dir_alloc_run(fat32_t *fs, uint32_t dir, int count, uint32_t *first)
{
    uint32_t run_start = 0;
    int run = 0;
    for (uint32_t slot = 0;; slot++) {
        uint8_t e[32];
        int rc = fat32_slot_read(fs, dir, slot, e);
        if (rc != 0 || e[0] == 0x00) {
            uint32_t start = run ? run_start : slot;
            for (int k = 0; k < count; k++) {
                uint32_t sec, off;
                while (fat32_slot_loc(fs, dir, start + (uint32_t)k, &sec, &off) != 0)
                    if (fat32_dir_extend(fs, dir) != 0) return -1;
            }
            *first = start;
            return 0;
        }
        if (e[0] == 0xE5) {
            if (!run) run_start = slot;
            if (++run >= count) { *first = run_start; return 0; }
        } else {
            run = 0;
        }
    }
}

/* Creates a directory entry set for `name` (long-name fragments plus a
 * short entry with a generated alias when the name does not fit 8.3).
 * Returns -2 if the name already exists. */
static int fat32_dir_add_named(fat32_t *fs, uint32_t dir, const char *name, const fat32_dirent_t *proto, fat32_loc_t *out)
{
    uint16_t n16[FATL_MAX + 1];
    int nlen = fatl_utf8_to_utf16(name, (int)strlen(name), n16, FATL_MAX);
    if (!fat32_valid_name(n16, nlen)) return -1;
    fat32_loc_t ex;
    if (fat32_dir_find_ex(fs, dir, n16, nlen, &ex) == 0) return -2;

    uint8_t set[22][32];
    int cnt = 0;
    uint8_t s11[11], nt = 0;
    fat32_dirent_t se = *proto;
    if (fatl_fits_83(n16, nlen, s11, &nt)) {
        se.reserved = nt;
    } else {
        int tail = 1;
        for (;; tail++) {
            fatl_make_alias(n16, nlen, tail, s11);
            if (!fat32_short_exists(fs, dir, s11)) break;
            if (tail > 999999) return -1;
        }
        se.reserved = 0;
        cnt = fatl_build_entries(n16, nlen, fatl_checksum(s11), set);
    }
    memcpy(se.name, s11, 8);
    memcpy(se.ext, s11 + 8, 3);
    memcpy(set[cnt], &se, 32);
    cnt++;

    uint32_t first;
    if (fat32_dir_alloc_run(fs, dir, cnt, &first) != 0) return -1;
    for (int k = 0; k < cnt; k++)
        if (fat32_slot_write(fs, dir, first + (uint32_t)k, set[k]) != 0) return -1;
    if (out) {
        out->de = se;
        out->slot = first + (uint32_t)cnt - 1;
        out->first_slot = first;
        if (fat32_slot_loc(fs, dir, out->slot, &out->sec, &out->off) != 0) return -1;
    }
    return 0;
}

/* Marks every slot of an entry set deleted. */
static int fat32_dir_delete_set(fat32_t *fs, uint32_t dir, uint32_t first, uint32_t last)
{
    for (uint32_t s = first; s <= last; s++) {
        uint8_t e[32];
        if (fat32_slot_read(fs, dir, s, e) != 0) return -1;
        e[0] = 0xE5;
        if (fat32_slot_write(fs, dir, s, e) != 0) return -1;
    }
    return 0;
}

static int fat32_dir_lookup_component(fat32_t *fs, uint32_t dir_cluster, const char *comp, uint32_t *out_cluster)
{
    fat32_loc_t loc;
    if (fat32_dir_find(fs, dir_cluster, comp, &loc) != 0) return -1;
    if (!(loc.de.attr & FAT32_ATTR_DIRECTORY)) return -1;

    uint32_t cl = loc.de.cluster_low | ((uint32_t)loc.de.cluster_high << 16);
    *out_cluster = (cl == 0) ? fs->root_cluster : cl;
    return 0;
}

static int fat32_walk(fat32_t *fs, const char *path, uint32_t *out_cluster)
{
    uint32_t cur = fs->root_cluster;

    while (*path == '/') path++;
    if (!*path) { *out_cluster = cur; return 0; }

    char comp[FAT32_NAME_MAX];
    while (*path) {
        int i = 0;
        while (*path && *path != '/' && i < FAT32_NAME_MAX - 1) comp[i++] = *path++;
        comp[i] = 0;
        while (*path == '/') path++;

        if (comp[0] == '.' && comp[1] == 0) continue;

        uint32_t next;
        if (fat32_dir_lookup_component(fs, cur, comp, &next) != 0) return -1;
        cur = next;
    }
    *out_cluster = cur;
    return 0;
}

static int fat32_read_data(fat32_t *fs, uint32_t start_cluster, uint32_t file_size, uint32_t offset,
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
        uint32_t e = fat32_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT32_CLUSTER_BAD) return 0;
        cluster = e;
    }

    uint8_t *sec_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!sec_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        if (cluster == 0 || cluster >= FAT32_CLUSTER_BAD) break;

        uint32_t sector_in_cluster = cluster_off / fs->bytes_per_sector;
        uint32_t byte_in_sector = cluster_off % fs->bytes_per_sector;
        uint32_t sector = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;

        if (fat32_read_sector(fs, sector, sec_buf) != 0) break;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;
        memcpy((uint8_t *)buf + done, sec_buf + byte_in_sector, chunk);

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size) {
            cluster_off = 0;
            uint32_t e = fat32_get_fat_entry(fs, cluster);
            cluster = (e == 0 || e >= FAT32_CLUSTER_BAD) ? 0 : e;
        }
    }

    free(sec_buf);
    return (int)done;
}

static int fat32_write_data(fat32_t *fs, uint32_t *start_cluster, uint32_t offset, const void *buf, uint32_t size)
{
    if (*start_cluster == 0) {
        uint32_t c = fat32_alloc_cluster(fs);
        if (c == 0) return -1;
        fat32_set_fat_entry(fs, c, FAT32_CLUSTER_EOF);
        *start_cluster = c;
    }

    uint32_t cluster_index = offset / fs->cluster_size;
    uint32_t cluster_off = offset % fs->cluster_size;

    uint32_t cluster = *start_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t e = fat32_get_fat_entry(fs, cluster);
        if (e == 0 || e >= FAT32_CLUSTER_BAD) {
            uint32_t nc = fat32_alloc_cluster(fs);
            if (nc == 0) return -1;
            fat32_set_fat_entry(fs, cluster, nc);
            fat32_set_fat_entry(fs, nc, FAT32_CLUSTER_EOF);
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
        uint32_t sector = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;

        if (chunk < fs->bytes_per_sector) {
            if (fat32_read_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }
        }
        memcpy(sec_buf + byte_in_sector, (const uint8_t *)buf + done, chunk);
        if (fat32_write_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size && done < size) {
            cluster_off = 0;
            uint32_t e = fat32_get_fat_entry(fs, cluster);
            if (e == 0 || e >= FAT32_CLUSTER_BAD) {
                uint32_t nc = fat32_alloc_cluster(fs);
                if (nc == 0) break;
                fat32_set_fat_entry(fs, cluster, nc);
                fat32_set_fat_entry(fs, nc, FAT32_CLUSTER_EOF);
                cluster = nc;
            } else {
                cluster = e;
            }
        }
    }

    free(sec_buf);
    return (int)done;
}

static int fat32_probe(fat32_t *fs, blockdev_t *bd)
{
    fs->bd = bd;

    fat32_boot_sector_t *bs = (fat32_boot_sector_t *)malloc(512);
    if (!bs) return -1;

    if (fat32_read_sector(fs, 0, bs) != 0) {
        free(bs);
        return -1;
    }

    uint8_t *raw = (uint8_t *)bs;
    if (bs->boot_sig != 0x29 || raw[510] != 0x55 || raw[511] != 0xAA ||
        memcmp(bs->fs_type, "FAT32", 5) != 0) {
        free(bs);
        return -1;
    }

    fs->bytes_per_sector = bs->bytes_per_sector;
    if (fs->bytes_per_sector == 0) fs->bytes_per_sector = 512;

    fs->sectors_per_cluster = bs->sectors_per_cluster;
    if (fs->sectors_per_cluster == 0) fs->sectors_per_cluster = 1;

    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->fat_sectors = bs->fat_size_32;
    fs->total_sectors = bs->total_sectors_32 ? bs->total_sectors_32 : bs->total_sectors_16;
    fs->root_cluster = bs->root_cluster;
    fs->fsinfo_sector = (bs->fs_info != 0 && bs->fs_info != 0xFFFF) ? bs->fs_info : 0;
    fs->fsinfo_invalid = 0;

    fs->data_start_sector = fs->reserved_sectors + (fs->num_fats * fs->fat_sectors);
    fs->cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    free(bs);
    return 0;
}

int fat32_probe_and_mount(fat32_t *fs, blockdev_t *bd)
{
    if (fat32_probe(fs, bd) != 0) return -1;
    return 0;
}

int fat32_umount(fat32_t *fs)
{
    (void)fs;
    return 0;
}

static int fat32_vfs_open(void *ctx, const char *path, int flags)
{
    fat32_t *fs = (fat32_t *)ctx;

    char parent_path[256];
    char name[FAT32_NAME_MAX];
    if (fat32_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (fat32_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    fat32_loc_t loc;
    fat32_dirent_t de;
    uint32_t sec = 0, off = 0;
    int found = (fat32_dir_find(fs, parent_cluster, name, &loc) == 0);

    if (!found) {
        if (!(flags & VFS_CREAT)) return -1;

        fat32_dirent_t newde;
        memset(&newde, 0, sizeof(newde));
        newde.attr = FAT32_ATTR_ARCHIVE;

        if (fat32_dir_add_named(fs, parent_cluster, name, &newde, &loc) != 0) return -1;
        de = loc.de;
        sec = loc.sec;
        off = loc.off;
    } else {
        de = loc.de;
        sec = loc.sec;
        off = loc.off;
        if (flags & VFS_TRUNC) {
            uint32_t cl = de.cluster_low | ((uint32_t)de.cluster_high << 16);
            fat32_free_chain(fs, cl);
            de.cluster_low = 0;
            de.cluster_high = 0;
            de.file_size = 0;

            uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
            if (buf) {
                if (fat32_read_sector(fs, sec, buf) == 0) {
                    memcpy(buf + off, &de, sizeof(de));
                    fat32_write_sector(fs, sec, buf);
                }
                free(buf);
            }
        }
    }

    if (de.attr & FAT32_ATTR_DIRECTORY) return -1;

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

static int fat32_vfs_close(void *ctx, int fd)
{
    fat32_t *fs = (fat32_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (fs->fds[fd].dirty) {
        uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
        if (buf) {
            if (fat32_read_sector(fs, fs->fds[fd].dirent_sector, buf) == 0) {
                fat32_dirent_t *de = (fat32_dirent_t *)(buf + fs->fds[fd].dirent_offset);
                de->file_size = fs->fds[fd].size;
                de->cluster_low = (uint16_t)(fs->fds[fd].start_cluster & 0xFFFF);
                de->cluster_high = (uint16_t)((fs->fds[fd].start_cluster >> 16) & 0xFFFF);
                fat32_write_sector(fs, fs->fds[fd].dirent_sector, buf);
            }
            free(buf);
        }
    }

    fs->fds[fd].used = 0;
    return 0;
}

static int fat32_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    fat32_t *fs = (fat32_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = fat32_read_data(fs, fs->fds[fd].start_cluster, fs->fds[fd].size, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += n;
    return n;
}

static int fat32_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    fat32_t *fs = (fat32_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = fat32_write_data(fs, &fs->fds[fd].start_cluster, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += n;
        if (fs->fds[fd].pos > fs->fds[fd].size) fs->fds[fd].size = fs->fds[fd].pos;
        fs->fds[fd].dirty = 1;
    }
    return n;
}

static int fat32_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    fat32_t *fs = (fat32_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int fat32_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    fat32_t *fs = (fat32_t *)ctx;

    uint32_t dir_cluster;
    if (fat32_walk(fs, path, &dir_cluster) != 0) return -1;

    fatl_state_t lfn;
    fatl_reset(&lfn);
    int count = 0;
    for (uint32_t slot = 0; count < max; slot++) {
        uint8_t e[32];
        if (fat32_slot_read(fs, dir_cluster, slot, e) != 0) break;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { fatl_reset(&lfn); continue; }
        if (e[11] == FAT32_ATTR_LFN) { fatl_feed(&lfn, e); continue; }
        if (e[11] & FAT32_ATTR_VOLUME) { fatl_reset(&lfn); continue; }

        uint16_t nm[FATL_MAX + 1];
        int nl = fatl_complete(&lfn, e, nm, FATL_MAX);
        if (nl <= 0) nl = fatl_short_to_utf16(e, nm);
        fatl_reset(&lfn);
        if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) continue;

        const fat32_dirent_t *de = (const fat32_dirent_t *)e;
        fatl_utf16_to_utf8(nm, nl, entries[count].name, VFS_NAME_LEN);
        entries[count].size = de->file_size;
        entries[count].is_dir = (de->attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
        entries[count].inode = de->cluster_low | ((uint32_t)de->cluster_high << 16);
        entries[count].mode = de->attr;
        count++;
    }
    return count;
}

static int fat32_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    fat32_t *fs = (fat32_t *)ctx;

    char parent_path[256];
    char name[FAT32_NAME_MAX];
    if (fat32_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (fat32_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    fat32_loc_t existing;
    if (fat32_dir_find(fs, parent_cluster, name, &existing) == 0) return -1;

    uint32_t new_cluster = fat32_alloc_cluster(fs);
    if (new_cluster == 0) return -1;
    fat32_set_fat_entry(fs, new_cluster, FAT32_CLUSTER_EOF);

    uint8_t *buf = (uint8_t *)malloc(fs->cluster_size);
    if (!buf) { fat32_free_chain(fs, new_cluster); return -1; }
    memset(buf, 0, fs->cluster_size);

    fat32_dirent_t *dot = (fat32_dirent_t *)buf;
    memset(dot->name, ' ', 8);
    memset(dot->ext, ' ', 3);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    dot->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    fat32_dirent_t *dotdot = (fat32_dirent_t *)(buf + sizeof(fat32_dirent_t));
    memset(dotdot->name, ' ', 8);
    memset(dotdot->ext, ' ', 3);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    uint32_t dotdot_cluster = (parent_cluster == fs->root_cluster) ? 0 : parent_cluster;
    dotdot->cluster_low = (uint16_t)(dotdot_cluster & 0xFFFF);
    dotdot->cluster_high = (uint16_t)((dotdot_cluster >> 16) & 0xFFFF);

    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        fat32_write_sector(fs, fat32_cluster_to_sector(fs, new_cluster) + s, buf + s * fs->bytes_per_sector);
    }
    free(buf);

    fat32_dirent_t newde;
    memset(&newde, 0, sizeof(newde));
    newde.attr = FAT32_ATTR_DIRECTORY;
    newde.cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    newde.cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    if (fat32_dir_add_named(fs, parent_cluster, name, &newde, NULL) != 0) {
        fat32_free_chain(fs, new_cluster);
        return -1;
    }
    return 0;
}

/* A directory can be removed only when it holds nothing but "." and "..". */
static int fat32_dir_is_empty(fat32_t *fs, uint32_t dir_cluster)
{
    for (uint32_t slot = 0;; slot++) {
        uint8_t e[32];
        if (fat32_slot_read(fs, dir_cluster, slot, e) != 0) return 1;
        if (e[0] == 0x00) return 1;
        if (e[0] == 0xE5 || e[11] == FAT32_ATTR_LFN) continue;
        if (e[11] & FAT32_ATTR_VOLUME) continue;
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) continue;
        return 0;
    }
}

static int fat32_vfs_unlink(void *ctx, const char *path)
{
    fat32_t *fs = (fat32_t *)ctx;

    char parent_path[256];
    char name[FAT32_NAME_MAX];
    if (fat32_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (fat32_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    fat32_loc_t loc;
    if (fat32_dir_find(fs, parent_cluster, name, &loc) != 0) return -1;

    uint32_t cluster = loc.de.cluster_low | ((uint32_t)loc.de.cluster_high << 16);
    if ((loc.de.attr & FAT32_ATTR_DIRECTORY) && !fat32_dir_is_empty(fs, cluster)) return -1;
    fat32_free_chain(fs, cluster);
    return fat32_dir_delete_set(fs, parent_cluster, loc.first_slot, loc.slot);
}

static int fat32_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    fat32_t *fs = (fat32_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        entry->name[0] = 0;
        entry->size = 0;
        entry->is_dir = 1;
        entry->inode = fs->root_cluster;
        entry->mode = FAT32_ATTR_DIRECTORY;
        return 0;
    }

    char parent_path[256];
    char name[FAT32_NAME_MAX];
    if (fat32_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (fat32_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    fat32_loc_t loc;
    if (fat32_dir_find(fs, parent_cluster, name, &loc) != 0) return -1;

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = loc.de.file_size;
    entry->is_dir = (loc.de.attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
    entry->inode = loc.de.cluster_low | ((uint32_t)loc.de.cluster_high << 16);
    entry->mode = loc.de.attr;
    return 0;
}

static int fat32_vfs_rename(void *ctx, const char *old, const char *new)
{
    fat32_t *fs = (fat32_t *)ctx;

    char old_parent_path[256], old_name[FAT32_NAME_MAX];
    if (fat32_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) return -1;
    uint32_t old_parent;
    if (fat32_walk(fs, old_parent_path, &old_parent) != 0) return -1;

    fat32_loc_t loc;
    if (fat32_dir_find(fs, old_parent, old_name, &loc) != 0) return -1;

    char new_parent_path[256], new_name[FAT32_NAME_MAX];
    if (fat32_split_path(new, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) return -1;
    uint32_t new_parent;
    if (fat32_walk(fs, new_parent_path, &new_parent) != 0) return -1;

    /* a case-only rename of the same entry is just a delete + add of the set */
    fat32_loc_t clash;
    int clash_found = (fat32_dir_find(fs, new_parent, new_name, &clash) == 0);
    if (clash_found && !(new_parent == old_parent && clash.slot == loc.slot)) return -1;

    fat32_dirent_t proto = loc.de;
    if (clash_found) {
        /* same entry under a differently-cased name: drop the old set first */
        if (fat32_dir_delete_set(fs, old_parent, loc.first_slot, loc.slot) != 0) return -1;
        if (fat32_dir_add_named(fs, new_parent, new_name, &proto, NULL) != 0) return -1;
        return 0;
    }

    fat32_loc_t added;
    if (fat32_dir_add_named(fs, new_parent, new_name, &proto, &added) != 0) return -1;

    /* a moved directory's ".." must point at its new parent (0 for the root) */
    if ((proto.attr & FAT32_ATTR_DIRECTORY) && new_parent != old_parent) {
        uint32_t dcl = proto.cluster_low | ((uint32_t)proto.cluster_high << 16);
        uint8_t *db = (uint8_t *)malloc(fs->bytes_per_sector);
        if (db) {
            uint32_t dsec = fat32_cluster_to_sector(fs, dcl);
            if (fat32_read_sector(fs, dsec, db) == 0) {
                fat32_dirent_t *dd = (fat32_dirent_t *)(db + sizeof(fat32_dirent_t));
                uint32_t nc = (new_parent == fs->root_cluster) ? 0 : new_parent;
                dd->cluster_low = (uint16_t)(nc & 0xFFFF);
                dd->cluster_high = (uint16_t)(nc >> 16);
                fat32_write_sector(fs, dsec, db);
            }
            free(db);
        }
    }

    return fat32_dir_delete_set(fs, old_parent, loc.first_slot, loc.slot);
}

static int fat32_vfs_symlink(void *ctx, const char *target, const char *name)
{
    (void)ctx; (void)target; (void)name;
    return -1;
}

void fat32_mount_vfs(fat32_t *fs, const char *mount_point)
{
    static vfs_ops_t fat32_vfs_ops = {
        .open = fat32_vfs_open,
        .close = fat32_vfs_close,
        .read = fat32_vfs_read,
        .write = fat32_vfs_write,
        .lseek = fat32_vfs_lseek,
        .readdir = fat32_vfs_readdir,
        .mkdir = fat32_vfs_mkdir,
        .unlink = fat32_vfs_unlink,
        .stat = fat32_vfs_stat,
        .rename = fat32_vfs_rename,
        .symlink = fat32_vfs_symlink,
    };
    vfs_mount(mount_point, &fat32_vfs_ops, fs);
}

int fat32_format(blockdev_t *bd, const char *label)
{
    uint32_t bytes_per_sector = bd->sector_size ? bd->sector_size : 512;
    uint32_t sectors_per_cluster = 1;
    uint32_t reserved_sectors = 32;
    uint32_t num_fats = 2;
    uint32_t root_cluster = 2;

    uint64_t total_sectors64 = bd->total_sectors;
    if (total_sectors64 > 0xFFFFFFFFULL) total_sectors64 = 0xFFFFFFFFULL;
    uint32_t total_sectors = (uint32_t)total_sectors64;

    uint32_t data_sectors_guess = total_sectors - reserved_sectors;
    uint32_t clusters_guess = data_sectors_guess / sectors_per_cluster;
    uint32_t fat_sectors = ((clusters_guess + 2) * 4 + bytes_per_sector - 1) / bytes_per_sector;
    if (fat_sectors == 0) fat_sectors = 1;

    uint8_t *sector = (uint8_t *)malloc(bytes_per_sector);
    if (!sector) return -1;
    memset(sector, 0, bytes_per_sector);

    fat32_boot_sector_t *bs = (fat32_boot_sector_t *)sector;
    bs->jmp[0] = 0xEB; bs->jmp[1] = 0x58; bs->jmp[2] = 0x90;
    memcpy(bs->oem, "tOS     ", 8);
    bs->bytes_per_sector = (uint16_t)bytes_per_sector;
    bs->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bs->reserved_sectors = (uint16_t)reserved_sectors;
    bs->num_fats = (uint8_t)num_fats;
    bs->root_entry_count = 0;
    bs->total_sectors_16 = 0;
    bs->media = 0xF8;
    bs->fat_size_16 = 0;
    bs->sectors_per_track = 63;
    bs->num_heads = 255;
    bs->hidden_sectors = 0;
    bs->total_sectors_32 = total_sectors;
    bs->fat_size_32 = fat_sectors;
    bs->ext_flags = 0;
    bs->fs_version = 0;
    bs->root_cluster = root_cluster;
    bs->fs_info = 1;
    bs->backup_boot_sector = 6;
    memset(bs->reserved_, 0, 12);
    bs->drive_number = 0x80;
    bs->reserved1 = 0;
    bs->boot_sig = 0x29;
    bs->volume_id = 0x12345678;
    memset(bs->volume_label, ' ', 11);
    if (label) {
        size_t i = 0;
        while (label[i] && i < 11) { bs->volume_label[i] = label[i]; i++; }
    }
    memcpy(bs->fs_type, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    int ret = blockdev_write(bd, 0, 1, sector);
    if (ret == 0) ret = blockdev_write(bd, 6, 1, sector);
    free(sector);
    if (ret != 0) return -1;

    uint8_t *fsi_buf = (uint8_t *)malloc(bytes_per_sector);
    if (!fsi_buf) return -1;
    memset(fsi_buf, 0, bytes_per_sector);
    fat32_fsinfo_t *fsi = (fat32_fsinfo_t *)fsi_buf;
    fsi->lead_sig = 0x41615252;
    fsi->struc_sig = 0x61417272;
    fsi->free_count = 0xFFFFFFFF;
    fsi->next_free = 0xFFFFFFFF;
    fsi->trail_sig = 0xAA550000;
    ret = blockdev_write(bd, 1, 1, fsi_buf);
    if (ret == 0) ret = blockdev_write(bd, 7, 1, fsi_buf);
    free(fsi_buf);
    if (ret != 0) return -1;

    uint8_t *zero_sector = (uint8_t *)malloc(bytes_per_sector);
    if (!zero_sector) return -1;
    memset(zero_sector, 0, bytes_per_sector);
    for (uint32_t s = 2; s < reserved_sectors; s++) {
        if (s == 6 || s == 7) continue;
        if (blockdev_write(bd, s, 1, zero_sector) != 0) { free(zero_sector); return -1; }
    }

    uint8_t *fat_buf = (uint8_t *)malloc(bytes_per_sector);
    if (!fat_buf) { free(zero_sector); return -1; }
    memset(fat_buf, 0, bytes_per_sector);
    uint32_t *fat_entries = (uint32_t *)fat_buf;
    fat_entries[0] = 0x0FFFFFF8;
    fat_entries[1] = 0x0FFFFFFF;
    fat_entries[2] = 0x0FFFFFFF;

    for (uint32_t f = 0; f < num_fats; f++) {
        if (blockdev_write(bd, reserved_sectors + f * fat_sectors, 1, fat_buf) != 0) {
            free(fat_buf); free(zero_sector);
            return -1;
        }
    }

    memset(fat_buf, 0, bytes_per_sector);
    for (uint32_t f = 0; f < num_fats; f++) {
        for (uint32_t s = 1; s < fat_sectors; s++) {
            if (blockdev_write(bd, reserved_sectors + f * fat_sectors + s, 1, fat_buf) != 0) {
                free(fat_buf); free(zero_sector);
                return -1;
            }
        }
    }
    free(fat_buf);

    uint32_t data_start_sector = reserved_sectors + num_fats * fat_sectors;
    uint32_t root_sector = data_start_sector + (root_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (blockdev_write(bd, root_sector + s, 1, zero_sector) != 0) {
            free(zero_sector);
            return -1;
        }
    }
    free(zero_sector);

    return 0;
}
