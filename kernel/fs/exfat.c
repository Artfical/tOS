#include "exfat.h"
#include "memory.h"
#include "string.h"

#define EXFAT_CLUSTER_EOF 0xFFFFFFFF
#define EXFAT_CLUSTER_BAD 0xFFFFFFF7

#define EXFAT_ENTRY_BITMAP 0x81
#define EXFAT_ENTRY_UPCASE 0x82
#define EXFAT_ENTRY_LABEL  0x83
#define EXFAT_ENTRY_FILE   0x85
#define EXFAT_ENTRY_STREAM 0xC0
#define EXFAT_ENTRY_NAME   0xC1

#define EXFAT_ATTR_READ_ONLY 0x0001
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

typedef struct {
    uint8_t jmp_boot[3];
    char fs_name[8];
    uint8_t must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_directory;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t bytes_per_sector_shift;
    uint8_t sectors_per_cluster_shift;
    uint8_t number_of_fats;
    uint8_t drive_select;
    uint8_t percent_in_use;
    uint8_t reserved[7];
    uint8_t boot_code[390];
    uint16_t boot_signature;
} __attribute__((packed)) exfat_boot_sector_t;

typedef struct {
    int used;
    uint32_t dir_cluster;
    uint32_t primary_slot;
    int secondary_count;
    uint16_t file_attributes;
    uint32_t first_cluster;
    uint32_t data_length;
} exfat_dirent_info_t;

static int exfat_read_sector(exfat_t *fs, uint32_t sector, void *buf)
{
    return blockdev_read(fs->bd, sector, 1, buf);
}

static int exfat_write_sector(exfat_t *fs, uint32_t sector, const void *buf)
{
    return blockdev_write(fs->bd, sector, 1, buf);
}

static uint8_t exfat_log2_u32(uint32_t v)
{
    uint8_t shift = 0;
    while ((v >>= 1) != 0) shift++;
    return shift;
}

static uint32_t exfat_get_fat_entry(exfat_t *fs, uint32_t cluster)
{
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset + (byte_offset / fs->bytes_per_sector);
    uint32_t sector_offset = byte_offset % fs->bytes_per_sector;

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return 0;
    if (exfat_read_sector(fs, fat_sector, buf) != 0) { free(buf); return 0; }
    uint32_t entry = *(uint32_t *)(&buf[sector_offset]);
    free(buf);
    return entry;
}

static int exfat_set_fat_entry(exfat_t *fs, uint32_t cluster, uint32_t value)
{
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset + (byte_offset / fs->bytes_per_sector);
    uint32_t sector_offset = byte_offset % fs->bytes_per_sector;

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;
    if (exfat_read_sector(fs, fat_sector, buf) != 0) { free(buf); return -1; }
    *(uint32_t *)(&buf[sector_offset]) = value;
    int ret = exfat_write_sector(fs, fat_sector, buf);
    free(buf);
    return ret;
}

static uint32_t exfat_cluster_to_sector(exfat_t *fs, uint32_t cluster)
{
    return fs->cluster_heap_offset + (cluster - 2) * fs->sectors_per_cluster;
}

static int exfat_chain_nth_sector(exfat_t *fs, uint32_t first_cluster, uint32_t n, uint32_t *out_sector)
{
    uint32_t cluster = first_cluster;
    uint32_t clusters_to_skip = n / fs->sectors_per_cluster;
    uint32_t sector_in_cluster = n % fs->sectors_per_cluster;

    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        uint32_t e = exfat_get_fat_entry(fs, cluster);
        if (e == 0 || e >= EXFAT_CLUSTER_BAD) return -1;
        cluster = e;
    }
    if (cluster == 0 || cluster >= EXFAT_CLUSTER_BAD) return -1;

    *out_sector = exfat_cluster_to_sector(fs, cluster) + sector_in_cluster;
    return 0;
}

static void exfat_bitmap_update(exfat_t *fs, uint32_t cluster, int used)
{
    uint32_t bit_index = cluster - 2;
    uint32_t byte_offset = bit_index / 8;
    uint8_t bit_mask = (uint8_t)(1u << (bit_index % 8));
    uint32_t sector_index = byte_offset / fs->bytes_per_sector;
    uint32_t byte_in_sector = byte_offset % fs->bytes_per_sector;

    uint32_t sec;
    if (exfat_chain_nth_sector(fs, fs->bitmap_cluster, sector_index, &sec) != 0) return;

    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return;
    if (exfat_read_sector(fs, sec, buf) != 0) { free(buf); return; }
    if (used) buf[byte_in_sector] |= bit_mask;
    else buf[byte_in_sector] &= (uint8_t)~bit_mask;
    exfat_write_sector(fs, sec, buf);
    free(buf);
}

static uint32_t exfat_alloc_cluster(exfat_t *fs)
{
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        if (exfat_get_fat_entry(fs, c) == 0) {
            exfat_bitmap_update(fs, c, 1);
            return c;
        }
    }
    return 0;
}

static void exfat_free_chain(exfat_t *fs, uint32_t cluster)
{
    while (cluster != 0 && cluster < EXFAT_CLUSTER_BAD) {
        uint32_t next = exfat_get_fat_entry(fs, cluster);
        exfat_set_fat_entry(fs, cluster, 0);
        exfat_bitmap_update(fs, cluster, 0);
        cluster = next;
    }
}

static char exfat_upcase_ascii(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int exfat_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (exfat_upcase_ascii(*a) != exfat_upcase_ascii(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int exfat_ascii_to_utf16(const char *name, uint16_t *out, int max_chars)
{
    int n = 0;
    while (name[n] && n < max_chars) {
        uint8_t c = (uint8_t)name[n];
        out[n] = (c < 128) ? c : '?';
        n++;
    }
    return n;
}

static uint16_t exfat_name_hash(const uint16_t *name_utf16, int len)
{
    uint16_t hash = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = name_utf16[i];
        uint16_t up = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
        uint8_t lo = (uint8_t)(up & 0xFF);
        uint8_t hi = (uint8_t)((up >> 8) & 0xFF);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + lo);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + hi);
    }
    return hash;
}

static uint16_t exfat_set_checksum(const uint8_t *entry_set, int total_bytes)
{
    uint16_t sum = 0;
    for (int i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + entry_set[i]);
    }
    return sum;
}

static int exfat_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
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

static int exfat_slot_to_sector(exfat_t *fs, uint32_t dir_cluster, uint32_t slot, uint32_t *out_sector, uint32_t *out_offset)
{
    uint32_t slots_per_sector = fs->bytes_per_sector / 32;
    uint32_t sector_index = slot / slots_per_sector;
    uint32_t slot_in_sector = slot % slots_per_sector;
    uint32_t sec;
    if (exfat_chain_nth_sector(fs, dir_cluster, sector_index, &sec) != 0) return -1;
    *out_sector = sec;
    *out_offset = slot_in_sector * 32;
    return 0;
}

static int exfat_read_slot(exfat_t *fs, uint32_t dir_cluster, uint32_t slot, uint8_t *out32)
{
    uint32_t sec, off;
    if (exfat_slot_to_sector(fs, dir_cluster, slot, &sec, &off) != 0) return -1;
    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;
    if (exfat_read_sector(fs, sec, buf) != 0) { free(buf); return -1; }
    memcpy(out32, buf + off, 32);
    free(buf);
    return 0;
}

static int exfat_write_slot(exfat_t *fs, uint32_t dir_cluster, uint32_t slot, const uint8_t *in32)
{
    uint32_t sec, off;
    if (exfat_slot_to_sector(fs, dir_cluster, slot, &sec, &off) != 0) return -1;
    uint8_t *buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!buf) return -1;
    if (exfat_read_sector(fs, sec, buf) != 0) { free(buf); return -1; }
    memcpy(buf + off, in32, 32);
    int ret = exfat_write_sector(fs, sec, buf);
    free(buf);
    return ret;
}

static uint32_t exfat_dir_extend(exfat_t *fs, uint32_t dir_cluster)
{
    uint32_t cur = dir_cluster;
    uint32_t e;
    while ((e = exfat_get_fat_entry(fs, cur)) != 0 && e < EXFAT_CLUSTER_BAD) cur = e;

    uint32_t new_cluster = exfat_alloc_cluster(fs);
    if (new_cluster == 0) return 0;
    exfat_set_fat_entry(fs, cur, new_cluster);
    exfat_set_fat_entry(fs, new_cluster, EXFAT_CLUSTER_EOF);

    uint8_t *zbuf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (zbuf) {
        memset(zbuf, 0, fs->bytes_per_sector);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
            exfat_write_sector(fs, exfat_cluster_to_sector(fs, new_cluster) + s, zbuf);
        free(zbuf);
    }
    return new_cluster;
}

static int exfat_dir_alloc_slots(exfat_t *fs, uint32_t dir_cluster, int count, uint32_t *out_slot)
{
    uint32_t slot = 0;
    uint8_t entry[32];
    while (1) {
        if (exfat_read_slot(fs, dir_cluster, slot, entry) != 0) {
            if (exfat_dir_extend(fs, dir_cluster) == 0) return -1;
            continue;
        }
        if (entry[0] == 0x00) break;
        slot++;
    }

    for (int i = 1; i < count; i++) {
        while (exfat_read_slot(fs, dir_cluster, slot + i, entry) != 0) {
            if (exfat_dir_extend(fs, dir_cluster) == 0) return -1;
        }
    }

    *out_slot = slot;
    return 0;
}

static int exfat_dir_find(exfat_t *fs, uint32_t dir_cluster, const char *name, exfat_dirent_info_t *out)
{
    uint32_t slot = 0;
    uint8_t entry[32];

    while (1) {
        if (exfat_read_slot(fs, dir_cluster, slot, entry) != 0) return -1;
        if (entry[0] == 0x00) return -1;

        uint8_t raw_type = entry[0] & 0x7F;
        if (raw_type != (EXFAT_ENTRY_FILE & 0x7F)) { slot++; continue; }

        int in_use = (entry[0] & 0x80) != 0;
        int secondary_count = entry[1];
        uint32_t primary_slot = slot;

        if (!in_use) { slot += (uint32_t)(1 + secondary_count); continue; }

        uint8_t stream_entry[32];
        if (exfat_read_slot(fs, dir_cluster, slot + 1, stream_entry) != 0) return -1;
        uint8_t name_len = stream_entry[3];
        uint32_t first_cluster = *(uint32_t *)(&stream_entry[20]);
        uint64_t raw_data_length = *(uint64_t *)(&stream_entry[24]);

        char namebuf[EXFAT_MAX_FILENAME + 1];
        int nb = 0;
        int name_entries = secondary_count - 1;
        for (int ne = 0; ne < name_entries && nb < EXFAT_MAX_FILENAME; ne++) {
            uint8_t name_entry[32];
            if (exfat_read_slot(fs, dir_cluster, slot + 2 + (uint32_t)ne, name_entry) != 0) return -1;
            for (int k = 0; k < 15 && nb < name_len && nb < EXFAT_MAX_FILENAME; k++) {
                uint16_t c = (uint16_t)(name_entry[2 + k * 2] | (name_entry[2 + k * 2 + 1] << 8));
                namebuf[nb++] = (c < 128) ? (char)c : '?';
            }
        }
        namebuf[nb] = 0;

        if (exfat_name_eq(namebuf, name)) {
            if (out) {
                out->dir_cluster = dir_cluster;
                out->primary_slot = primary_slot;
                out->secondary_count = secondary_count;
                out->file_attributes = (uint16_t)(entry[4] | (entry[5] << 8));
                out->first_cluster = first_cluster;
                out->data_length = (uint32_t)raw_data_length;
            }
            return 0;
        }

        slot += (uint32_t)(1 + secondary_count);
    }
}

static int exfat_dir_add_entry(exfat_t *fs, uint32_t dir_cluster, const char *name,
                                uint16_t attributes, uint32_t first_cluster, uint32_t data_length,
                                uint32_t *out_slot, int *out_secondary_count)
{
    uint16_t name_utf16[EXFAT_MAX_FILENAME];
    int n = exfat_ascii_to_utf16(name, name_utf16, EXFAT_MAX_FILENAME);
    int name_entries = (n + 14) / 15;
    if (name_entries < 1) name_entries = 1;
    int secondary_count = 1 + name_entries;
    int total_slots = 1 + secondary_count;

    uint32_t slot;
    if (exfat_dir_alloc_slots(fs, dir_cluster, total_slots, &slot) != 0) return -1;

    uint16_t hash = exfat_name_hash(name_utf16, n);

    uint8_t *set = (uint8_t *)malloc((size_t)(32 * total_slots));
    if (!set) return -1;
    memset(set, 0, (size_t)(32 * total_slots));

    set[0] = EXFAT_ENTRY_FILE;
    set[1] = (uint8_t)secondary_count;
    set[4] = (uint8_t)(attributes & 0xFF);
    set[5] = (uint8_t)((attributes >> 8) & 0xFF);

    uint8_t *stream = set + 32;
    stream[0] = EXFAT_ENTRY_STREAM;
    stream[1] = 0x01;
    stream[3] = (uint8_t)n;
    stream[4] = (uint8_t)(hash & 0xFF);
    stream[5] = (uint8_t)((hash >> 8) & 0xFF);
    *(uint64_t *)(stream + 8) = (uint64_t)data_length;
    *(uint32_t *)(stream + 20) = first_cluster;
    *(uint64_t *)(stream + 24) = (uint64_t)data_length;

    for (int ne = 0; ne < name_entries; ne++) {
        uint8_t *ent = set + 32 * (2 + ne);
        ent[0] = EXFAT_ENTRY_NAME;
        for (int k = 0; k < 15; k++) {
            int idx = ne * 15 + k;
            uint16_t c = (idx < n) ? name_utf16[idx] : 0;
            ent[2 + k * 2] = (uint8_t)(c & 0xFF);
            ent[2 + k * 2 + 1] = (uint8_t)((c >> 8) & 0xFF);
        }
    }

    uint16_t checksum = exfat_set_checksum(set, 32 * total_slots);
    set[2] = (uint8_t)(checksum & 0xFF);
    set[3] = (uint8_t)((checksum >> 8) & 0xFF);

    int ret = 0;
    for (int i = 0; i < total_slots; i++) {
        if (exfat_write_slot(fs, dir_cluster, slot + (uint32_t)i, set + 32 * i) != 0) ret = -1;
    }
    free(set);
    if (ret != 0) return -1;

    if (out_slot) *out_slot = slot;
    if (out_secondary_count) *out_secondary_count = secondary_count;
    return 0;
}

static int exfat_update_entry(exfat_t *fs, uint32_t dir_cluster, uint32_t primary_slot, int secondary_count,
                               uint32_t first_cluster, uint32_t data_length)
{
    int total_slots = 1 + secondary_count;
    uint8_t *set = (uint8_t *)malloc((size_t)(32 * total_slots));
    if (!set) return -1;

    for (int i = 0; i < total_slots; i++) {
        if (exfat_read_slot(fs, dir_cluster, primary_slot + (uint32_t)i, set + 32 * i) != 0) { free(set); return -1; }
    }

    uint8_t *stream = set + 32;
    *(uint64_t *)(stream + 8) = (uint64_t)data_length;
    *(uint32_t *)(stream + 20) = first_cluster;
    *(uint64_t *)(stream + 24) = (uint64_t)data_length;

    uint16_t checksum = exfat_set_checksum(set, 32 * total_slots);
    set[2] = (uint8_t)(checksum & 0xFF);
    set[3] = (uint8_t)((checksum >> 8) & 0xFF);

    int ret = 0;
    for (int i = 0; i < total_slots; i++) {
        if (exfat_write_slot(fs, dir_cluster, primary_slot + (uint32_t)i, set + 32 * i) != 0) ret = -1;
    }
    free(set);
    return ret;
}

static int exfat_mark_deleted(exfat_t *fs, uint32_t dir_cluster, uint32_t primary_slot, int secondary_count)
{
    int total_slots = 1 + secondary_count;
    for (int i = 0; i < total_slots; i++) {
        uint8_t entry[32];
        if (exfat_read_slot(fs, dir_cluster, primary_slot + (uint32_t)i, entry) != 0) return -1;
        entry[0] &= 0x7F;
        if (exfat_write_slot(fs, dir_cluster, primary_slot + (uint32_t)i, entry) != 0) return -1;
    }
    return 0;
}

static int exfat_dir_lookup_component(exfat_t *fs, uint32_t dir_cluster, const char *comp, uint32_t *out_cluster)
{
    exfat_dirent_info_t info;
    if (exfat_dir_find(fs, dir_cluster, comp, &info) != 0) return -1;
    if (!(info.file_attributes & EXFAT_ATTR_DIRECTORY)) return -1;
    *out_cluster = info.first_cluster;
    return 0;
}

static int exfat_walk(exfat_t *fs, const char *path, uint32_t *out_cluster)
{
    char comps[64][EXFAT_MAX_FILENAME + 1];
    int top = 0;

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[EXFAT_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < EXFAT_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        while (*p == '/') p++;

        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (top > 0) top--;
            continue;
        }
        if (top < 64) {
            int k = 0;
            while (comp[k] && k < EXFAT_MAX_FILENAME) { comps[top][k] = comp[k]; k++; }
            comps[top][k] = 0;
            top++;
        }
    }

    uint32_t cur = fs->root_cluster;
    for (int i = 0; i < top; i++) {
        uint32_t next;
        if (exfat_dir_lookup_component(fs, cur, comps[i], &next) != 0) return -1;
        cur = next;
    }
    *out_cluster = cur;
    return 0;
}

static int exfat_read_data(exfat_t *fs, uint32_t start_cluster, uint32_t file_size, uint32_t offset,
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
        uint32_t e = exfat_get_fat_entry(fs, cluster);
        if (e == 0 || e >= EXFAT_CLUSTER_BAD) return 0;
        cluster = e;
    }

    uint8_t *sec_buf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (!sec_buf) return -1;

    uint32_t done = 0;
    while (done < size) {
        if (cluster == 0 || cluster >= EXFAT_CLUSTER_BAD) break;

        uint32_t sector_in_cluster = cluster_off / fs->bytes_per_sector;
        uint32_t byte_in_sector = cluster_off % fs->bytes_per_sector;
        uint32_t sector = exfat_cluster_to_sector(fs, cluster) + sector_in_cluster;

        if (exfat_read_sector(fs, sector, sec_buf) != 0) break;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;
        memcpy((uint8_t *)buf + done, sec_buf + byte_in_sector, chunk);

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size) {
            cluster_off = 0;
            uint32_t e = exfat_get_fat_entry(fs, cluster);
            cluster = (e == 0 || e >= EXFAT_CLUSTER_BAD) ? 0 : e;
        }
    }

    free(sec_buf);
    return (int)done;
}

static int exfat_write_data(exfat_t *fs, uint32_t *start_cluster, uint32_t offset, const void *buf, uint32_t size)
{
    if (*start_cluster == 0) {
        uint32_t c = exfat_alloc_cluster(fs);
        if (c == 0) return -1;
        exfat_set_fat_entry(fs, c, EXFAT_CLUSTER_EOF);
        *start_cluster = c;
    }

    uint32_t cluster_index = offset / fs->cluster_size;
    uint32_t cluster_off = offset % fs->cluster_size;

    uint32_t cluster = *start_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t e = exfat_get_fat_entry(fs, cluster);
        if (e == 0 || e >= EXFAT_CLUSTER_BAD) {
            uint32_t nc = exfat_alloc_cluster(fs);
            if (nc == 0) return -1;
            exfat_set_fat_entry(fs, cluster, nc);
            exfat_set_fat_entry(fs, nc, EXFAT_CLUSTER_EOF);
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
        uint32_t sector = exfat_cluster_to_sector(fs, cluster) + sector_in_cluster;

        uint32_t chunk = fs->bytes_per_sector - byte_in_sector;
        if (chunk > size - done) chunk = size - done;

        if (chunk < fs->bytes_per_sector) {
            if (exfat_read_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }
        }
        memcpy(sec_buf + byte_in_sector, (const uint8_t *)buf + done, chunk);
        if (exfat_write_sector(fs, sector, sec_buf) != 0) { free(sec_buf); return done > 0 ? (int)done : -1; }

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= fs->cluster_size && done < size) {
            cluster_off = 0;
            uint32_t e = exfat_get_fat_entry(fs, cluster);
            if (e == 0 || e >= EXFAT_CLUSTER_BAD) {
                uint32_t nc = exfat_alloc_cluster(fs);
                if (nc == 0) break;
                exfat_set_fat_entry(fs, cluster, nc);
                exfat_set_fat_entry(fs, nc, EXFAT_CLUSTER_EOF);
                cluster = nc;
            } else {
                cluster = e;
            }
        }
    }

    free(sec_buf);
    return (int)done;
}

static int exfat_probe(exfat_t *fs, blockdev_t *bd)
{
    fs->bd = bd;

    exfat_boot_sector_t *bs = (exfat_boot_sector_t *)malloc(512);
    if (!bs) return -1;

    if (exfat_read_sector(fs, 0, bs) != 0) { free(bs); return -1; }

    uint8_t *raw = (uint8_t *)bs;
    if (memcmp(bs->fs_name, "EXFAT   ", 8) != 0 || raw[510] != 0x55 || raw[511] != 0xAA) {
        free(bs);
        return -1;
    }

    fs->bytes_per_sector = 1u << bs->bytes_per_sector_shift;
    fs->sectors_per_cluster = 1u << bs->sectors_per_cluster_shift;
    fs->fat_offset = bs->fat_offset;
    fs->fat_length = bs->fat_length;
    fs->cluster_heap_offset = bs->cluster_heap_offset;
    fs->cluster_count = bs->cluster_count;
    fs->root_cluster = bs->first_cluster_of_root_directory;
    fs->cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    free(bs);

    fs->bitmap_cluster = 0;
    fs->bitmap_size_bytes = 0;

    uint32_t slot = 0;
    uint8_t entry[32];
    while (1) {
        if (exfat_read_slot(fs, fs->root_cluster, slot, entry) != 0) break;
        if (entry[0] == 0x00) break;

        if (entry[0] == EXFAT_ENTRY_BITMAP) {
            fs->bitmap_cluster = *(uint32_t *)(&entry[20]);
            fs->bitmap_size_bytes = (uint32_t)(*(uint64_t *)(&entry[24]));
            break;
        }

        if ((entry[0] & 0x7F) == (EXFAT_ENTRY_FILE & 0x7F)) {
            slot += (uint32_t)(1 + entry[1]);
        } else {
            slot++;
        }
    }

    if (fs->bitmap_cluster == 0) return -1;
    return 0;
}

int exfat_probe_and_mount(exfat_t *fs, blockdev_t *bd)
{
    if (exfat_probe(fs, bd) != 0) return -1;
    return 0;
}

int exfat_umount(exfat_t *fs)
{
    (void)fs;
    return 0;
}

static int exfat_vfs_open(void *ctx, const char *path, int flags)
{
    exfat_t *fs = (exfat_t *)ctx;

    char parent_path[256];
    char name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (exfat_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    exfat_dirent_info_t info;
    int found = (exfat_dir_find(fs, parent_cluster, name, &info) == 0);

    if (!found) {
        if (!(flags & VFS_CREAT)) return -1;

        uint32_t slot; int secondary_count;
        if (exfat_dir_add_entry(fs, parent_cluster, name, EXFAT_ATTR_ARCHIVE, 0, 0, &slot, &secondary_count) != 0) return -1;

        info.dir_cluster = parent_cluster;
        info.primary_slot = slot;
        info.secondary_count = secondary_count;
        info.file_attributes = EXFAT_ATTR_ARCHIVE;
        info.first_cluster = 0;
        info.data_length = 0;
    } else if (flags & VFS_TRUNC) {
        if (info.first_cluster != 0) exfat_free_chain(fs, info.first_cluster);
        info.first_cluster = 0;
        info.data_length = 0;
        exfat_update_entry(fs, info.dir_cluster, info.primary_slot, info.secondary_count, 0, 0);
    }

    if (info.file_attributes & EXFAT_ATTR_DIRECTORY) return -1;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].dir_cluster = info.dir_cluster;
            fs->fds[i].primary_slot = info.primary_slot;
            fs->fds[i].secondary_count = info.secondary_count;
            fs->fds[i].start_cluster = info.first_cluster;
            fs->fds[i].size = info.data_length;
            fs->fds[i].pos = (flags & VFS_APPEND) ? info.data_length : 0;
            fs->fds[i].dirty = 0;
            return i;
        }
    }
    return -1;
}

static int exfat_vfs_close(void *ctx, int fd)
{
    exfat_t *fs = (exfat_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (fs->fds[fd].dirty) {
        exfat_update_entry(fs, fs->fds[fd].dir_cluster, fs->fds[fd].primary_slot, fs->fds[fd].secondary_count,
                            fs->fds[fd].start_cluster, fs->fds[fd].size);
    }

    fs->fds[fd].used = 0;
    return 0;
}

static int exfat_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    exfat_t *fs = (exfat_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = exfat_read_data(fs, fs->fds[fd].start_cluster, fs->fds[fd].size, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += n;
    return n;
}

static int exfat_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    exfat_t *fs = (exfat_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    int n = exfat_write_data(fs, &fs->fds[fd].start_cluster, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += n;
        if (fs->fds[fd].pos > fs->fds[fd].size) fs->fds[fd].size = fs->fds[fd].pos;
        fs->fds[fd].dirty = 1;
    }
    return n;
}

static int exfat_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    exfat_t *fs = (exfat_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int exfat_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    exfat_t *fs = (exfat_t *)ctx;

    uint32_t dir_cluster;
    if (exfat_walk(fs, path, &dir_cluster) != 0) return -1;

    int count = 0;
    uint32_t slot = 0;
    uint8_t entry[32];

    while (count < max) {
        if (exfat_read_slot(fs, dir_cluster, slot, entry) != 0) break;
        if (entry[0] == 0x00) break;

        uint8_t raw_type = entry[0] & 0x7F;
        if (raw_type != (EXFAT_ENTRY_FILE & 0x7F)) { slot++; continue; }

        int in_use = (entry[0] & 0x80) != 0;
        int secondary_count = entry[1];

        if (!in_use) { slot += (uint32_t)(1 + secondary_count); continue; }

        uint8_t stream_entry[32];
        if (exfat_read_slot(fs, dir_cluster, slot + 1, stream_entry) != 0) break;
        uint8_t name_len = stream_entry[3];
        uint32_t first_cluster = *(uint32_t *)(&stream_entry[20]);
        uint64_t raw_data_length = *(uint64_t *)(&stream_entry[24]);
        uint16_t attrs = (uint16_t)(entry[4] | (entry[5] << 8));

        char namebuf[EXFAT_MAX_FILENAME + 1];
        int nb = 0;
        int name_entries = secondary_count - 1;
        for (int ne = 0; ne < name_entries && nb < EXFAT_MAX_FILENAME; ne++) {
            uint8_t name_entry[32];
            if (exfat_read_slot(fs, dir_cluster, slot + 2 + (uint32_t)ne, name_entry) != 0) break;
            for (int k = 0; k < 15 && nb < name_len && nb < EXFAT_MAX_FILENAME; k++) {
                uint16_t c = (uint16_t)(name_entry[2 + k * 2] | (name_entry[2 + k * 2 + 1] << 8));
                namebuf[nb++] = (c < 128) ? (char)c : '?';
            }
        }
        namebuf[nb] = 0;

        int k = 0;
        while (namebuf[k] && k < VFS_NAME_LEN - 1) { entries[count].name[k] = namebuf[k]; k++; }
        entries[count].name[k] = 0;
        entries[count].size = (uint32_t)raw_data_length;
        entries[count].is_dir = (attrs & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
        entries[count].inode = first_cluster;
        entries[count].mode = attrs;
        count++;

        slot += (uint32_t)(1 + secondary_count);
    }

    return count;
}

static int exfat_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    exfat_t *fs = (exfat_t *)ctx;

    char parent_path[256];
    char name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (exfat_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    exfat_dirent_info_t existing;
    if (exfat_dir_find(fs, parent_cluster, name, &existing) == 0) return -1;

    uint32_t new_cluster = exfat_alloc_cluster(fs);
    if (new_cluster == 0) return -1;
    exfat_set_fat_entry(fs, new_cluster, EXFAT_CLUSTER_EOF);

    uint8_t *zbuf = (uint8_t *)malloc(fs->bytes_per_sector);
    if (zbuf) {
        memset(zbuf, 0, fs->bytes_per_sector);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
            exfat_write_sector(fs, exfat_cluster_to_sector(fs, new_cluster) + s, zbuf);
        free(zbuf);
    }

    uint32_t slot; int secondary_count;
    if (exfat_dir_add_entry(fs, parent_cluster, name, EXFAT_ATTR_DIRECTORY, new_cluster, 0, &slot, &secondary_count) != 0) {
        exfat_free_chain(fs, new_cluster);
        return -1;
    }
    return 0;
}

static int exfat_vfs_unlink(void *ctx, const char *path)
{
    exfat_t *fs = (exfat_t *)ctx;

    char parent_path[256];
    char name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (exfat_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    exfat_dirent_info_t info;
    if (exfat_dir_find(fs, parent_cluster, name, &info) != 0) return -1;

    if (info.file_attributes & EXFAT_ATTR_DIRECTORY) return -1;

    if (info.first_cluster != 0) exfat_free_chain(fs, info.first_cluster);

    return exfat_mark_deleted(fs, info.dir_cluster, info.primary_slot, info.secondary_count);
}

static int exfat_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    exfat_t *fs = (exfat_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        entry->name[0] = 0;
        entry->size = 0;
        entry->is_dir = 1;
        entry->inode = fs->root_cluster;
        entry->mode = EXFAT_ATTR_DIRECTORY;
        return 0;
    }

    char parent_path[256];
    char name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_cluster;
    if (exfat_walk(fs, parent_path, &parent_cluster) != 0) return -1;

    exfat_dirent_info_t info;
    if (exfat_dir_find(fs, parent_cluster, name, &info) != 0) return -1;

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = info.data_length;
    entry->is_dir = (info.file_attributes & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
    entry->inode = info.first_cluster;
    entry->mode = info.file_attributes;
    return 0;
}

static int exfat_vfs_rename(void *ctx, const char *old, const char *new)
{
    exfat_t *fs = (exfat_t *)ctx;

    char old_parent_path[256], old_name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) return -1;
    uint32_t old_parent;
    if (exfat_walk(fs, old_parent_path, &old_parent) != 0) return -1;

    exfat_dirent_info_t info;
    if (exfat_dir_find(fs, old_parent, old_name, &info) != 0) return -1;

    char new_parent_path[256], new_name[EXFAT_MAX_FILENAME + 1];
    if (exfat_split_path(new, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) return -1;
    uint32_t new_parent;
    if (exfat_walk(fs, new_parent_path, &new_parent) != 0) return -1;

    uint32_t slot; int secondary_count;
    if (exfat_dir_add_entry(fs, new_parent, new_name, info.file_attributes, info.first_cluster, info.data_length,
                             &slot, &secondary_count) != 0) return -1;

    exfat_mark_deleted(fs, old_parent, info.primary_slot, info.secondary_count);
    return 0;
}

static int exfat_vfs_symlink(void *ctx, const char *target, const char *name)
{
    (void)ctx; (void)target; (void)name;
    return -1;
}

void exfat_mount_vfs(exfat_t *fs, const char *mount_point)
{
    static vfs_ops_t exfat_vfs_ops = {
        .open = exfat_vfs_open,
        .close = exfat_vfs_close,
        .read = exfat_vfs_read,
        .write = exfat_vfs_write,
        .lseek = exfat_vfs_lseek,
        .readdir = exfat_vfs_readdir,
        .mkdir = exfat_vfs_mkdir,
        .unlink = exfat_vfs_unlink,
        .stat = exfat_vfs_stat,
        .rename = exfat_vfs_rename,
        .symlink = exfat_vfs_symlink,
    };
    vfs_mount(mount_point, &exfat_vfs_ops, fs);
}

int exfat_format(blockdev_t *bd, const char *label)
{
    uint32_t bytes_per_sector = bd->sector_size ? bd->sector_size : 512;
    uint8_t bytes_per_sector_shift = exfat_log2_u32(bytes_per_sector);
    uint8_t sectors_per_cluster_shift = 6;
    uint32_t sectors_per_cluster = 1u << sectors_per_cluster_shift;

    uint32_t fat_offset = 24;

    uint64_t total_sectors64 = bd->total_sectors;
    if (total_sectors64 > 0xFFFFFFFFULL) total_sectors64 = 0xFFFFFFFFULL;
    uint32_t total_sectors = (uint32_t)total_sectors64;

    uint32_t data_sectors_guess = total_sectors - fat_offset;
    uint32_t clusters_guess = data_sectors_guess / sectors_per_cluster;
    uint32_t fat_length = ((clusters_guess + 2) * 4 + bytes_per_sector - 1) / bytes_per_sector;
    if (fat_length < 1) fat_length = 1;

    uint32_t cluster_heap_offset = fat_offset + fat_length;
    uint32_t heap_sectors = total_sectors - cluster_heap_offset;
    uint32_t cluster_count = heap_sectors / sectors_per_cluster;

    uint32_t bitmap_cluster = 2;
    uint32_t upcase_cluster = 3;
    uint32_t root_cluster = 4;
    uint32_t bitmap_size_bytes = (cluster_count + 7) / 8;

    uint32_t region_bytes = 11 * bytes_per_sector;
    uint8_t *region = (uint8_t *)malloc(region_bytes);
    if (!region) return -1;
    memset(region, 0, region_bytes);

    exfat_boot_sector_t *bs = (exfat_boot_sector_t *)region;
    bs->jmp_boot[0] = 0xEB; bs->jmp_boot[1] = 0x76; bs->jmp_boot[2] = 0x90;
    memcpy(bs->fs_name, "EXFAT   ", 8);
    bs->partition_offset = 0;
    bs->volume_length = total_sectors;
    bs->fat_offset = fat_offset;
    bs->fat_length = fat_length;
    bs->cluster_heap_offset = cluster_heap_offset;
    bs->cluster_count = cluster_count;
    bs->first_cluster_of_root_directory = root_cluster;
    bs->volume_serial_number = 0x12345678;
    bs->fs_revision = 0x0100;
    bs->volume_flags = 0;
    bs->bytes_per_sector_shift = bytes_per_sector_shift;
    bs->sectors_per_cluster_shift = sectors_per_cluster_shift;
    bs->number_of_fats = 1;
    bs->drive_select = 0x80;
    bs->percent_in_use = 0xFF;
    region[510] = 0x55;
    region[511] = 0xAA;

    for (int i = 1; i <= 8; i++) {
        *(uint32_t *)(region + (uint32_t)i * bytes_per_sector + bytes_per_sector - 4) = 0xAA550000;
    }

    uint32_t checksum = 0;
    for (uint32_t i = 0; i < region_bytes; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        checksum = ((checksum << 31) | (checksum >> 1)) + region[i];
    }

    uint8_t *checksum_sector = (uint8_t *)malloc(bytes_per_sector);
    if (!checksum_sector) { free(region); return -1; }
    for (uint32_t i = 0; i < bytes_per_sector; i += 4) {
        *(uint32_t *)(checksum_sector + i) = checksum;
    }

    int ret = 0;
    for (uint32_t s = 0; s < 11 && ret == 0; s++) {
        ret = blockdev_write(bd, s, 1, region + s * bytes_per_sector);
    }
    if (ret == 0) ret = blockdev_write(bd, 11, 1, checksum_sector);
    for (uint32_t s = 0; s < 11 && ret == 0; s++) {
        ret = blockdev_write(bd, 12 + s, 1, region + s * bytes_per_sector);
    }
    if (ret == 0) ret = blockdev_write(bd, 23, 1, checksum_sector);

    free(region);
    free(checksum_sector);
    if (ret != 0) return -1;

    uint8_t *fat_buf = (uint8_t *)malloc(bytes_per_sector);
    if (!fat_buf) return -1;
    memset(fat_buf, 0, bytes_per_sector);
    uint32_t *fat_entries = (uint32_t *)fat_buf;
    fat_entries[0] = 0xFFFFFFF8;
    fat_entries[1] = 0xFFFFFFFF;
    fat_entries[2] = EXFAT_CLUSTER_EOF;
    fat_entries[3] = EXFAT_CLUSTER_EOF;
    fat_entries[4] = EXFAT_CLUSTER_EOF;

    if (blockdev_write(bd, fat_offset, 1, fat_buf) != 0) { free(fat_buf); return -1; }

    memset(fat_buf, 0, bytes_per_sector);
    for (uint32_t s = 1; s < fat_length; s++) {
        if (blockdev_write(bd, fat_offset + s, 1, fat_buf) != 0) { free(fat_buf); return -1; }
    }
    free(fat_buf);

    uint8_t *cluster_buf = (uint8_t *)malloc((size_t)sectors_per_cluster * bytes_per_sector);
    if (!cluster_buf) return -1;

    memset(cluster_buf, 0, (size_t)sectors_per_cluster * bytes_per_sector);
    cluster_buf[0] = 0x07;
    uint32_t bitmap_sector = cluster_heap_offset + (bitmap_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (blockdev_write(bd, bitmap_sector + s, 1, cluster_buf + s * bytes_per_sector) != 0) {
            free(cluster_buf); return -1;
        }
    }

    memset(cluster_buf, 0, (size_t)sectors_per_cluster * bytes_per_sector);
    uint16_t *upcase = (uint16_t *)cluster_buf;
    int ui = 0;
    upcase[ui++] = 0xFFFF; upcase[ui++] = 0x0061;
    for (uint16_t c = 0x0061; c <= 0x007A; c++) upcase[ui++] = (uint16_t)(c - 0x20);
    upcase[ui++] = 0xFFFF; upcase[ui++] = 0xFF85;
    uint32_t upcase_table_bytes = (uint32_t)ui * 2;

    uint32_t upcase_checksum = 0;
    for (uint32_t i = 0; i < upcase_table_bytes; i++) {
        upcase_checksum = ((upcase_checksum << 31) | (upcase_checksum >> 1)) + cluster_buf[i];
    }

    uint32_t upcase_sector = cluster_heap_offset + (upcase_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (blockdev_write(bd, upcase_sector + s, 1, cluster_buf + s * bytes_per_sector) != 0) {
            free(cluster_buf); return -1;
        }
    }

    memset(cluster_buf, 0, (size_t)sectors_per_cluster * bytes_per_sector);
    uint8_t *bitmap_entry = cluster_buf;
    bitmap_entry[0] = EXFAT_ENTRY_BITMAP;
    bitmap_entry[1] = 0;
    *(uint32_t *)(bitmap_entry + 20) = bitmap_cluster;
    *(uint64_t *)(bitmap_entry + 24) = (uint64_t)bitmap_size_bytes;

    uint8_t *upcase_entry = cluster_buf + 32;
    upcase_entry[0] = EXFAT_ENTRY_UPCASE;
    *(uint32_t *)(upcase_entry + 4) = upcase_checksum;
    *(uint32_t *)(upcase_entry + 20) = upcase_cluster;
    *(uint64_t *)(upcase_entry + 24) = (uint64_t)upcase_table_bytes;

    if (label && label[0]) {
        uint8_t *label_entry = cluster_buf + 64;
        label_entry[0] = EXFAT_ENTRY_LABEL;
        uint16_t *lbl16 = (uint16_t *)(label_entry + 2);
        int llen = 0;
        while (label[llen] && llen < 11) {
            uint8_t c = (uint8_t)label[llen];
            lbl16[llen] = (c < 128) ? c : '?';
            llen++;
        }
        label_entry[1] = (uint8_t)llen;
    }

    uint32_t root_sector = cluster_heap_offset + (root_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (blockdev_write(bd, root_sector + s, 1, cluster_buf + s * bytes_per_sector) != 0) {
            free(cluster_buf); return -1;
        }
    }
    free(cluster_buf);

    return 0;
}
