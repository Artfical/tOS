#include "ntfs.h"
#include "string.h"
#include "stdlib.h"
#include "vfs.h"

#define NTFS_ATTR_STANDARD_INFORMATION 0x10
#define NTFS_ATTR_FILE_NAME            0x30
#define NTFS_ATTR_DATA                 0x80
#define NTFS_ATTR_INDEX_ROOT           0x90
#define NTFS_ATTR_END                  0xFFFFFFFFu

#define NTFS_FILE_RECORD_IN_USE   0x0001
#define NTFS_FILE_RECORD_IS_DIR   0x0002

#define NTFS_FA_DIRECTORY 0x10
#define NTFS_FA_ARCHIVE   0x20

#define NTFS_REC_MFT       0
#define NTFS_REC_MFTMIRR   1
#define NTFS_REC_BITMAP    6
#define NTFS_REC_ROOT      5
#define NTFS_FIRST_USER_RECORD 16

#define NTFS_INDEX_ENTRY_LAST 0x02

#pragma pack(push, 1)

typedef struct {
    uint8_t  jump[3];
    char     oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  zero1[3];
    uint16_t unused1;
    uint8_t  media_descriptor;
    uint16_t zero2;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t unused2;
    uint32_t sig2;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    int8_t   clusters_per_mft_record;
    uint8_t  pad1[3];
    int8_t   clusters_per_index_record;
    uint8_t  pad2[3];
    uint64_t volume_serial;
    uint32_t checksum;
    uint8_t  boot_code[426];
    uint16_t end_signature;
} ntfs_boot_sector_t;

typedef struct {
    char     magic[4];
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t sequence_number;
    uint16_t link_count;
    uint16_t attrs_offset;
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
    uint16_t padding;
    uint32_t record_number;
} ntfs_record_hdr_t;

typedef struct {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attribute_id;
} ntfs_attr_hdr_t;

typedef struct {
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed_flag;
    uint8_t  padding;
} ntfs_attr_res_t;

typedef struct {
    uint64_t starting_vcn;
    uint64_t last_vcn;
    uint16_t runlist_offset;
    uint16_t compression_unit;
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t file_size;
    uint64_t initialized_size;
} ntfs_attr_nonres_t;

typedef struct {
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint32_t file_attributes;
    uint32_t reserved1;
    uint64_t reserved2;
} ntfs_std_info_t;

typedef struct {
    uint64_t parent_ref;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t file_attributes;
    uint32_t reparse;
    uint8_t  name_length;
    uint8_t  namespace_id;
    /* name follows, UTF-16LE, name_length wide chars */
} ntfs_file_name_t;

typedef struct {
    uint32_t attr_type;
    uint32_t collation_rule;
    uint32_t index_entry_size;
    uint8_t  clusters_per_index;
    uint8_t  pad[3];
    uint32_t entries_offset;
    uint32_t index_length;
    uint32_t allocated_size;
    uint8_t  flags;
    uint8_t  pad2[3];
} ntfs_index_root_t;

typedef struct {
    uint64_t mft_ref;
    uint16_t entry_length;
    uint16_t key_length;
    uint16_t flags;
    uint16_t padding;
    /* ntfs_file_name_t key follows if not last entry */
} ntfs_index_entry_t;

#pragma pack(pop)

static int ntfs_raw_read_cluster(ntfs_t *fs, uint64_t lcn, void *buf)
{
    return blockdev_read_bytes(fs->bd, lcn * fs->cluster_size, fs->cluster_size, buf);
}

static int ntfs_raw_write_cluster(ntfs_t *fs, uint64_t lcn, const void *buf)
{
    return blockdev_write_bytes(fs->bd, lcn * fs->cluster_size, fs->cluster_size, buf);
}

/* ---- runlist: sequence of [1 header byte=0x44][4-byte length LE][4-byte signed LCN delta LE], terminated by 0x00 ---- */

static uint8_t *ntfs_runlist_append_run(uint8_t *p, int64_t lcn_delta, uint32_t length)
{
    p[0] = 0x44;
    memcpy(p + 1, &length, 4);
    int32_t delta = (int32_t)lcn_delta;
    memcpy(p + 5, &delta, 4);
    return p + 9;
}

static int ntfs_runlist_lookup(const uint8_t *runlist, uint64_t vcn, uint64_t *out_lcn, uint64_t *out_run_remaining)
{
    const uint8_t *p = runlist;
    int64_t cur_lcn = 0;
    uint64_t cur_vcn = 0;
    while (*p != 0) {
        uint32_t len; int32_t delta;
        memcpy(&len, p + 1, 4);
        memcpy(&delta, p + 5, 4);
        cur_lcn += delta;
        if (vcn >= cur_vcn && vcn < cur_vcn + len) {
            *out_lcn = (uint64_t)(cur_lcn + (int64_t)(vcn - cur_vcn));
            *out_run_remaining = (cur_vcn + len) - vcn;
            return 0;
        }
        cur_vcn += len;
        p += 9;
    }
    return -1;
}

static uint64_t ntfs_runlist_total_vcn(const uint8_t *runlist)
{
    const uint8_t *p = runlist;
    uint64_t total = 0;
    while (*p != 0) {
        uint32_t len;
        memcpy(&len, p + 1, 4);
        total += len;
        p += 9;
    }
    return total;
}

static int64_t ntfs_runlist_last_lcn(const uint8_t *runlist)
{
    const uint8_t *p = runlist;
    int64_t cur_lcn = 0;
    while (*p != 0) {
        int32_t delta;
        memcpy(&delta, p + 5, 4);
        cur_lcn += delta;
        p += 9;
    }
    return cur_lcn;
}

static uint32_t ntfs_runlist_byte_len(const uint8_t *runlist)
{
    const uint8_t *p = runlist;
    while (*p != 0) p += 9;
    return (uint32_t)(p - runlist) + 1;
}

/* ---- attribute access within a record buffer ---- */

static ntfs_attr_hdr_t *ntfs_attr_find(uint8_t *record, uint32_t type)
{
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    uint8_t *p = record + hdr->attrs_offset;
    for (;;) {
        uint32_t t;
        memcpy(&t, p, 4);
        if (t == NTFS_ATTR_END) return 0;
        ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
        if (attr->type == type) return attr;
        p += attr->length;
    }
}

static uint8_t *ntfs_attr_end(uint8_t *record)
{
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    uint8_t *p = record + hdr->attrs_offset;
    for (;;) {
        uint32_t t;
        memcpy(&t, p, 4);
        if (t == NTFS_ATTR_END) return p;
        ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
        p += attr->length;
    }
}

static uint8_t *ntfs_attr_res_value(ntfs_attr_hdr_t *attr)
{
    ntfs_attr_res_t *res = (ntfs_attr_res_t *)((uint8_t *)attr + sizeof(ntfs_attr_hdr_t));
    return (uint8_t *)attr + res->value_offset;
}

static ntfs_attr_nonres_t *ntfs_attr_nonres_hdr(ntfs_attr_hdr_t *attr)
{
    return (ntfs_attr_nonres_t *)((uint8_t *)attr + sizeof(ntfs_attr_hdr_t));
}

static uint8_t *ntfs_attr_runlist(ntfs_attr_hdr_t *attr)
{
    ntfs_attr_nonres_t *nr = ntfs_attr_nonres_hdr(attr);
    return (uint8_t *)attr + nr->runlist_offset;
}

/* ---- raw record read/write, bootstrapped through $MFT's own non-resident $DATA runlist ---- */

static int ntfs_read_record(ntfs_t *fs, uint32_t record_number, uint8_t *out_buf)
{
    if (record_number == 0) {
        return ntfs_raw_read_cluster(fs, fs->mft_lcn, out_buf);
    }
    uint8_t *rec0 = (uint8_t *)malloc(fs->cluster_size);
    if (!rec0) return -1;
    if (ntfs_raw_read_cluster(fs, fs->mft_lcn, rec0) != 0) { free(rec0); return -1; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec0, NTFS_ATTR_DATA);
    if (!data || !data->non_resident) { free(rec0); return -1; }
    uint8_t *runlist = ntfs_attr_runlist(data);
    uint64_t lcn, remain;
    if (ntfs_runlist_lookup(runlist, record_number, &lcn, &remain) != 0) { free(rec0); return -1; }
    free(rec0);
    return ntfs_raw_read_cluster(fs, lcn, out_buf);
}

static int ntfs_write_record(ntfs_t *fs, uint32_t record_number, const uint8_t *in_buf)
{
    if (record_number == 0) {
        return ntfs_raw_write_cluster(fs, fs->mft_lcn, in_buf);
    }
    uint8_t *rec0 = (uint8_t *)malloc(fs->cluster_size);
    if (!rec0) return -1;
    if (ntfs_raw_read_cluster(fs, fs->mft_lcn, rec0) != 0) { free(rec0); return -1; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec0, NTFS_ATTR_DATA);
    if (!data || !data->non_resident) { free(rec0); return -1; }
    uint8_t *runlist = ntfs_attr_runlist(data);
    uint64_t lcn, remain;
    if (ntfs_runlist_lookup(runlist, record_number, &lcn, &remain) != 0) { free(rec0); return -1; }
    free(rec0);
    return ntfs_raw_write_cluster(fs, lcn, in_buf);
}

/* ---- cluster bitmap, resident inside record 6's $DATA attribute ---- */

static void ntfs_bitmap_set_range(ntfs_t *fs, uint64_t lcn, uint32_t count, int value)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return;
    if (ntfs_read_record(fs, NTFS_REC_BITMAP, rec) != 0) { free(rec); return; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
    if (data && !data->non_resident) {
        uint8_t *val = ntfs_attr_res_value(data);
        for (uint32_t i = 0; i < count; i++) {
            uint64_t bitno = lcn + i;
            if (value) val[bitno / 8] |= (uint8_t)(1u << (bitno % 8));
            else       val[bitno / 8] &= (uint8_t)~(1u << (bitno % 8));
        }
        ntfs_write_record(fs, NTFS_REC_BITMAP, rec);
    }
    free(rec);
}

/* best-effort contiguous free-cluster allocator; returns actual run length found (<= want), 0 on failure */
static uint32_t ntfs_alloc_clusters(ntfs_t *fs, uint32_t want, uint64_t *out_lcn)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return 0;
    if (ntfs_read_record(fs, NTFS_REC_BITMAP, rec) != 0) { free(rec); return 0; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
    if (!data || data->non_resident) { free(rec); return 0; }
    uint8_t *val = ntfs_attr_res_value(data);

    uint64_t run_start = 0;
    uint32_t run_len = 0;
    for (uint64_t lcn = 0; lcn < fs->total_clusters; lcn++) {
        int used = (val[lcn / 8] >> (lcn % 8)) & 1;
        if (!used) {
            if (run_len == 0) run_start = lcn;
            run_len++;
            if (run_len >= want) break;
        } else {
            run_len = 0;
        }
    }
    free(rec);
    if (run_len == 0) return 0;
    if (run_len > want) run_len = want;
    *out_lcn = run_start;
    ntfs_bitmap_set_range(fs, run_start, run_len, 1);
    return run_len;
}

static void ntfs_free_clusters(ntfs_t *fs, uint64_t lcn, uint32_t count)
{
    ntfs_bitmap_set_range(fs, lcn, count, 0);
}

/* ---- name encoding: ASCII <-> fake UTF-16LE (zero-extended) ---- */

static void ntfs_name_encode(const char *name, uint8_t *out, uint8_t *out_len)
{
    int i = 0;
    while (name[i] && i < NTFS_MAX_FILENAME) {
        out[i * 2] = (uint8_t)name[i];
        out[i * 2 + 1] = 0;
        i++;
    }
    *out_len = (uint8_t)i;
}

/* len comes straight from the on-disk $FILE_NAME attribute's
 * name_length byte (attacker-controlled if the volume/image is
 * hostile -- NTFS allows up to 255 UTF-16 code units), but every
 * caller's destination is a fixed VFS_NAME_LEN (128) char array.
 * Clamp to out_cap so a crafted directory entry can't overflow it. */
static void ntfs_name_decode(const uint8_t *utf16_name, uint8_t len, char *out, size_t out_cap)
{
    if (out_cap == 0) return;
    if (len > out_cap - 1) len = (uint8_t)(out_cap - 1);
    int i;
    for (i = 0; i < len; i++) out[i] = (char)utf16_name[i * 2];
    out[i] = 0;
}

static int ntfs_name_eq(const uint8_t *utf16_name, uint8_t len, const char *ascii)
{
    int alen = (int)strlen(ascii);
    if (alen != (int)len) return 0;
    for (int i = 0; i < alen; i++) {
        if (utf16_name[i * 2] != (uint8_t)ascii[i] || utf16_name[i * 2 + 1] != 0) return 0;
    }
    return 1;
}

/* ---- generic attribute-area growth within a record (fixed attribute order means the
   attribute being grown/shrunk is always the last one before the 0xFFFFFFFF terminator) ---- */

static uint8_t *ntfs_record_append_attr(uint8_t *record, uint32_t total_len)
{
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    uint8_t *old_term = ntfs_attr_end(record);
    uint8_t *new_term = old_term + total_len;
    if ((uint32_t)((new_term + 4) - record) > NTFS_RECORD_SIZE) return 0;
    uint32_t term = NTFS_ATTR_END;
    memcpy(new_term, &term, 4);
    hdr->bytes_in_use = (uint32_t)((new_term + 4) - record);
    return old_term;
}

static uint8_t *ntfs_attr_extend(uint8_t *record, ntfs_attr_hdr_t *attr, uint32_t add_bytes)
{
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    uint8_t *old_end = (uint8_t *)attr + attr->length;
    uint8_t *new_term = old_end + add_bytes;
    if ((uint32_t)((new_term + 4) - record) > NTFS_RECORD_SIZE) return 0;
    attr->length += add_bytes;
    uint32_t term = NTFS_ATTR_END;
    memcpy(new_term, &term, 4);
    hdr->bytes_in_use = (uint32_t)((new_term + 4) - record);
    return old_end;
}

static void ntfs_attr_shrink(uint8_t *record, ntfs_attr_hdr_t *attr, uint32_t sub_bytes)
{
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    uint8_t *new_end = (uint8_t *)attr + attr->length - sub_bytes;
    attr->length -= sub_bytes;
    uint32_t term = NTFS_ATTR_END;
    memcpy(new_end, &term, 4);
    hdr->bytes_in_use = (uint32_t)((new_end + 4) - record);
}

/* ---- record construction ---- */

static void ntfs_record_init_header(uint8_t *record, uint32_t record_number, int is_dir)
{
    memset(record, 0, NTFS_RECORD_SIZE);
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)record;
    memcpy(hdr->magic, "FILE", 4);
    hdr->usa_offset = (uint16_t)sizeof(ntfs_record_hdr_t);
    hdr->usa_count = 0;
    hdr->lsn = 0;
    hdr->sequence_number = 1;
    hdr->link_count = 1;
    hdr->attrs_offset = (uint16_t)sizeof(ntfs_record_hdr_t);
    hdr->flags = NTFS_FILE_RECORD_IN_USE | (is_dir ? NTFS_FILE_RECORD_IS_DIR : 0);
    hdr->bytes_allocated = NTFS_RECORD_SIZE;
    hdr->base_record_ref = 0;
    hdr->next_attr_id = 1;
    hdr->record_number = record_number;

    uint8_t *p = record + hdr->attrs_offset;
    uint32_t term = NTFS_ATTR_END;
    memcpy(p, &term, 4);
    hdr->bytes_in_use = (uint32_t)((p + 4) - record);
}

static ntfs_attr_hdr_t *ntfs_record_add_std_info(uint8_t *record, uint32_t file_attributes)
{
    uint32_t val_len = (uint32_t)sizeof(ntfs_std_info_t);
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t)) + val_len;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_STANDARD_INFORMATION;
    attr->length = total;
    attr->non_resident = 0;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_res_t *res = (ntfs_attr_res_t *)(p + sizeof(ntfs_attr_hdr_t));
    res->value_length = val_len;
    res->value_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t));
    res->indexed_flag = 0;
    res->padding = 0;
    ntfs_std_info_t *info = (ntfs_std_info_t *)(p + res->value_offset);
    memset(info, 0, sizeof(*info));
    info->file_attributes = file_attributes;
    return attr;
}

static ntfs_attr_hdr_t *ntfs_record_add_file_name(uint8_t *record, uint64_t parent_ref, const char *name, uint32_t file_attributes)
{
    uint8_t enc[NTFS_MAX_FILENAME * 2];
    uint8_t name_len = 0;
    ntfs_name_encode(name, enc, &name_len);
    uint32_t val_len = (uint32_t)sizeof(ntfs_file_name_t) + name_len * 2u;
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t)) + val_len;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_FILE_NAME;
    attr->length = total;
    attr->non_resident = 0;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_res_t *res = (ntfs_attr_res_t *)(p + sizeof(ntfs_attr_hdr_t));
    res->value_length = val_len;
    res->value_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t));
    res->indexed_flag = 1;
    res->padding = 0;
    ntfs_file_name_t *fn = (ntfs_file_name_t *)(p + res->value_offset);
    memset(fn, 0, sizeof(*fn));
    fn->parent_ref = parent_ref;
    fn->file_attributes = file_attributes;
    fn->name_length = name_len;
    fn->namespace_id = 0;
    memcpy((uint8_t *)fn + sizeof(ntfs_file_name_t), enc, (size_t)name_len * 2);
    return attr;
}

static ntfs_attr_hdr_t *ntfs_record_add_empty_data(uint8_t *record)
{
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_nonres_t)) + 1u;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_DATA;
    attr->length = total;
    attr->non_resident = 1;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_nonres_t *nr = (ntfs_attr_nonres_t *)(p + sizeof(ntfs_attr_hdr_t));
    memset(nr, 0, sizeof(*nr));
    nr->runlist_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_nonres_t));
    uint8_t *rl = p + nr->runlist_offset;
    rl[0] = 0;
    return attr;
}

static ntfs_attr_hdr_t *ntfs_record_add_data_with_run(uint8_t *record, int64_t lcn, uint32_t cluster_count, uint64_t size_bytes)
{
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_nonres_t)) + 9u + 1u;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_DATA;
    attr->length = total;
    attr->non_resident = 1;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_nonres_t *nr = (ntfs_attr_nonres_t *)(p + sizeof(ntfs_attr_hdr_t));
    memset(nr, 0, sizeof(*nr));
    nr->runlist_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_nonres_t));
    nr->allocated_size = size_bytes;
    nr->file_size = size_bytes;
    nr->initialized_size = size_bytes;
    uint8_t *rl = p + nr->runlist_offset;
    uint8_t *end = ntfs_runlist_append_run(rl, lcn, cluster_count);
    *end = 0;
    return attr;
}

static ntfs_attr_hdr_t *ntfs_record_add_resident_bitmap(uint8_t *record, uint32_t size_bytes)
{
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t)) + size_bytes;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_DATA;
    attr->length = total;
    attr->non_resident = 0;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_res_t *res = (ntfs_attr_res_t *)(p + sizeof(ntfs_attr_hdr_t));
    res->value_length = size_bytes;
    res->value_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t));
    res->indexed_flag = 0;
    res->padding = 0;
    memset(p + res->value_offset, 0, size_bytes);
    return attr;
}

static ntfs_attr_hdr_t *ntfs_record_add_empty_index_root(uint8_t *record)
{
    uint32_t entries_bytes = (uint32_t)sizeof(ntfs_index_entry_t);
    uint32_t val_len = (uint32_t)sizeof(ntfs_index_root_t) + entries_bytes;
    uint32_t total = (uint32_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t)) + val_len;
    uint8_t *p = ntfs_record_append_attr(record, total);
    if (!p) return 0;
    ntfs_attr_hdr_t *attr = (ntfs_attr_hdr_t *)p;
    attr->type = NTFS_ATTR_INDEX_ROOT;
    attr->length = total;
    attr->non_resident = 0;
    attr->name_length = 0;
    attr->name_offset = (uint16_t)sizeof(ntfs_attr_hdr_t);
    attr->flags = 0;
    attr->attribute_id = 0;
    ntfs_attr_res_t *res = (ntfs_attr_res_t *)(p + sizeof(ntfs_attr_hdr_t));
    res->value_length = val_len;
    res->value_offset = (uint16_t)(sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t));
    res->indexed_flag = 0;
    res->padding = 0;
    ntfs_index_root_t *ir = (ntfs_index_root_t *)(p + res->value_offset);
    memset(ir, 0, sizeof(*ir));
    ir->attr_type = NTFS_ATTR_FILE_NAME;
    ir->entries_offset = (uint32_t)sizeof(ntfs_index_root_t);
    ir->index_length = entries_bytes;
    ir->allocated_size = entries_bytes;
    ntfs_index_entry_t *last = (ntfs_index_entry_t *)((uint8_t *)ir + ir->entries_offset);
    memset(last, 0, sizeof(*last));
    last->entry_length = (uint16_t)sizeof(ntfs_index_entry_t);
    last->key_length = 0;
    last->flags = NTFS_INDEX_ENTRY_LAST;
    return attr;
}

static int ntfs_record_create(ntfs_t *fs, uint32_t record_number, uint32_t parent_record,
                               const char *name, int is_dir)
{
    uint8_t *rec = (uint8_t *)malloc(NTFS_RECORD_SIZE);
    if (!rec) return -1;
    uint32_t fattr = is_dir ? NTFS_FA_DIRECTORY : NTFS_FA_ARCHIVE;
    ntfs_record_init_header(rec, record_number, is_dir);
    int ok = 1;
    if (!ntfs_record_add_std_info(rec, fattr)) ok = 0;
    if (ok && !ntfs_record_add_file_name(rec, (uint64_t)parent_record, name, fattr)) ok = 0;
    if (ok) {
        if (is_dir) { if (!ntfs_record_add_empty_index_root(rec)) ok = 0; }
        else        { if (!ntfs_record_add_empty_data(rec)) ok = 0; }
    }
    if (!ok) { free(rec); return -1; }
    int rc = ntfs_write_record(fs, record_number, rec);
    free(rec);
    return rc;
}

/* ---- append one run to a non-resident $DATA attribute's runlist (attr must be the record's last attribute) ---- */

static int ntfs_data_append_run(uint8_t *record, ntfs_attr_hdr_t *data, int64_t lcn_delta, uint32_t length)
{
    uint8_t *runlist = ntfs_attr_runlist(data);
    uint32_t old_len = ntfs_runlist_byte_len(runlist);
    uint8_t *room = ntfs_attr_extend(record, data, 9);
    if (!room) return -1;
    uint8_t *term_byte = runlist + old_len - 1;
    memmove(term_byte + 9, term_byte, 1);
    ntfs_runlist_append_run(term_byte, lcn_delta, length);
    return 0;
}

/* ---- record (MFT entry) allocation: reuse a free slot, else grow $MFT by one cluster ---- */

static int ntfs_alloc_record(ntfs_t *fs, uint32_t *out_record)
{
    uint8_t *rec0 = (uint8_t *)malloc(fs->cluster_size);
    if (!rec0) return -1;
    if (ntfs_raw_read_cluster(fs, fs->mft_lcn, rec0) != 0) { free(rec0); return -1; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec0, NTFS_ATTR_DATA);
    if (!data || !data->non_resident) { free(rec0); return -1; }
    uint8_t *runlist = ntfs_attr_runlist(data);
    uint64_t total_records = ntfs_runlist_total_vcn(runlist);

    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) { free(rec0); return -1; }
    for (uint32_t rn = NTFS_FIRST_USER_RECORD; rn < total_records; rn++) {
        if (ntfs_read_record(fs, rn, rec) != 0) continue;
        ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)rec;
        if (memcmp(hdr->magic, "FILE", 4) != 0 || !(hdr->flags & NTFS_FILE_RECORD_IN_USE)) {
            free(rec);
            free(rec0);
            *out_record = rn;
            return 0;
        }
    }
    free(rec);

    uint64_t new_lcn;
    if (ntfs_alloc_clusters(fs, 1, &new_lcn) != 1) { free(rec0); return -1; }
    int64_t last_lcn = ntfs_runlist_last_lcn(runlist);
    int64_t delta = (int64_t)new_lcn - last_lcn;
    if (ntfs_data_append_run(rec0, data, delta, 1) != 0) {
        ntfs_free_clusters(fs, new_lcn, 1);
        free(rec0);
        return -1;
    }
    ntfs_attr_nonres_t *nr = ntfs_attr_nonres_hdr(data);
    nr->allocated_size += fs->cluster_size;
    if (ntfs_write_record(fs, 0, rec0) != 0) { free(rec0); return -1; }
    *out_record = (uint32_t)total_records;
    free(rec0);
    return 0;
}

/* ---- non-resident $DATA read/write, with allocate-on-write growth ---- */

static int ntfs_data_read(ntfs_t *fs, uint32_t record_number, uint32_t offset, void *buf, uint32_t size)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, record_number, rec) != 0) { free(rec); return -1; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
    if (!data || !data->non_resident) { free(rec); return -1; }
    ntfs_attr_nonres_t *nr = ntfs_attr_nonres_hdr(data);
    uint64_t file_size = nr->file_size;

    if (offset >= file_size) { free(rec); return 0; }
    if ((uint64_t)offset + size > file_size) size = (uint32_t)(file_size - offset);
    if (size == 0) { free(rec); return 0; }

    uint8_t *runlist = ntfs_attr_runlist(data);
    uint8_t *cbuf = (uint8_t *)malloc(fs->cluster_size);
    if (!cbuf) { free(rec); return -1; }

    uint32_t done = 0;
    while (done < size) {
        uint32_t cur_off = offset + done;
        uint64_t vcn = cur_off / fs->cluster_size;
        uint32_t in_cluster = cur_off % fs->cluster_size;
        uint64_t lcn, remain;
        if (ntfs_runlist_lookup(runlist, vcn, &lcn, &remain) != 0) break;
        if (ntfs_raw_read_cluster(fs, lcn, cbuf) != 0) break;
        uint32_t chunk = fs->cluster_size - in_cluster;
        if (chunk > size - done) chunk = size - done;
        memcpy((uint8_t *)buf + done, cbuf + in_cluster, chunk);
        done += chunk;
    }
    free(cbuf);
    free(rec);
    return (int)done;
}

static int ntfs_data_write(ntfs_t *fs, uint32_t record_number, uint32_t offset, const void *buf, uint32_t size)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, record_number, rec) != 0) { free(rec); return -1; }
    ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
    if (!data || !data->non_resident) { free(rec); return -1; }
    ntfs_attr_nonres_t *nr = ntfs_attr_nonres_hdr(data);

    uint64_t needed_end = (uint64_t)offset + size;
    uint8_t *cbuf = (uint8_t *)malloc(fs->cluster_size);
    if (!cbuf) { free(rec); return -1; }

    while (nr->allocated_size < needed_end) {
        uint64_t new_lcn;
        if (ntfs_alloc_clusters(fs, 1, &new_lcn) != 1) break;
        uint8_t *runlist = ntfs_attr_runlist(data);
        int64_t last_lcn = ntfs_runlist_last_lcn(runlist);
        int64_t delta = (int64_t)new_lcn - last_lcn;
        if (ntfs_data_append_run(rec, data, delta, 1) != 0) {
            ntfs_free_clusters(fs, new_lcn, 1);
            break;
        }
        nr = ntfs_attr_nonres_hdr(data);
        nr->allocated_size += fs->cluster_size;
    }

    uint8_t *runlist = ntfs_attr_runlist(data);
    uint32_t done = 0;
    while (done < size && (uint64_t)offset + done < nr->allocated_size) {
        uint32_t cur_off = offset + done;
        uint64_t vcn = cur_off / fs->cluster_size;
        uint32_t in_cluster = cur_off % fs->cluster_size;
        uint64_t lcn, remain;
        if (ntfs_runlist_lookup(runlist, vcn, &lcn, &remain) != 0) break;
        uint32_t chunk = fs->cluster_size - in_cluster;
        if (chunk > size - done) chunk = size - done;
        if (chunk < fs->cluster_size) {
            if (ntfs_raw_read_cluster(fs, lcn, cbuf) != 0) break;
            memcpy(cbuf + in_cluster, (const uint8_t *)buf + done, chunk);
            if (ntfs_raw_write_cluster(fs, lcn, cbuf) != 0) break;
        } else {
            if (ntfs_raw_write_cluster(fs, lcn, (const uint8_t *)buf + done) != 0) break;
        }
        done += chunk;
    }

    uint64_t new_end = (uint64_t)offset + done;
    if (new_end > nr->file_size) nr->file_size = new_end;
    if (new_end > nr->initialized_size) nr->initialized_size = new_end;
    ntfs_write_record(fs, record_number, rec);
    free(cbuf);
    free(rec);
    return (int)done;
}

/* ---- directory ($INDEX_ROOT) entry management ---- */

static ntfs_index_root_t *ntfs_dir_index_root(uint8_t *record, ntfs_attr_hdr_t **out_attr)
{
    ntfs_attr_hdr_t *attr = ntfs_attr_find(record, NTFS_ATTR_INDEX_ROOT);
    if (!attr) return 0;
    if (out_attr) *out_attr = attr;
    return (ntfs_index_root_t *)ntfs_attr_res_value(attr);
}

static int ntfs_dir_find_entry(uint8_t *record, const char *name, ntfs_index_entry_t **out_entry, ntfs_file_name_t **out_key)
{
    ntfs_index_root_t *ir = ntfs_dir_index_root(record, 0);
    if (!ir) return -1;
    uint8_t *base = (uint8_t *)ir + ir->entries_offset;
    uint32_t pos = 0;
    while (pos < ir->index_length) {
        ntfs_index_entry_t *e = (ntfs_index_entry_t *)(base + pos);
        if (e->flags & NTFS_INDEX_ENTRY_LAST) break;
        ntfs_file_name_t *fn = (ntfs_file_name_t *)((uint8_t *)e + sizeof(ntfs_index_entry_t));
        if (ntfs_name_eq((uint8_t *)fn + sizeof(ntfs_file_name_t), fn->name_length, name)) {
            if (out_entry) *out_entry = e;
            if (out_key) *out_key = fn;
            return 0;
        }
        pos += e->entry_length;
    }
    return -1;
}

static int ntfs_dir_is_empty(uint8_t *record)
{
    ntfs_index_root_t *ir = ntfs_dir_index_root(record, 0);
    if (!ir) return 1;
    return ir->index_length <= sizeof(ntfs_index_entry_t);
}

static int ntfs_dir_add_entry(ntfs_t *fs, uint32_t dir_record, const char *name, uint64_t mft_ref, uint32_t file_attributes)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, dir_record, rec) != 0) { free(rec); return -1; }

    ntfs_attr_hdr_t *ir_attr;
    ntfs_index_root_t *ir = ntfs_dir_index_root(rec, &ir_attr);
    if (!ir) { free(rec); return -1; }

    if (ntfs_dir_find_entry(rec, name, 0, 0) == 0) { free(rec); return -1; }

    uint8_t enc[NTFS_MAX_FILENAME * 2];
    uint8_t name_len = 0;
    ntfs_name_encode(name, enc, &name_len);
    uint32_t key_len = (uint32_t)sizeof(ntfs_file_name_t) + name_len * 2u;
    uint32_t entry_len = (uint32_t)sizeof(ntfs_index_entry_t) + key_len;

    uint8_t *base = (uint8_t *)ir + ir->entries_offset;
    uint8_t *last_entry = base + (ir->index_length - sizeof(ntfs_index_entry_t));

    uint8_t *room = ntfs_attr_extend(rec, ir_attr, entry_len);
    if (!room) { free(rec); return -1; }

    memmove(last_entry + entry_len, last_entry, sizeof(ntfs_index_entry_t));

    ntfs_index_entry_t *ne = (ntfs_index_entry_t *)last_entry;
    ne->mft_ref = mft_ref;
    ne->entry_length = (uint16_t)entry_len;
    ne->key_length = (uint16_t)key_len;
    ne->flags = 0;
    ne->padding = 0;
    ntfs_file_name_t *fn = (ntfs_file_name_t *)((uint8_t *)ne + sizeof(ntfs_index_entry_t));
    memset(fn, 0, sizeof(*fn));
    fn->parent_ref = (uint64_t)dir_record;
    fn->file_attributes = file_attributes;
    fn->name_length = name_len;
    fn->namespace_id = 0;
    memcpy((uint8_t *)fn + sizeof(ntfs_file_name_t), enc, (size_t)name_len * 2);

    ir->index_length += entry_len;
    ir->allocated_size += entry_len;

    int rc = ntfs_write_record(fs, dir_record, rec);
    free(rec);
    return rc;
}

static int ntfs_dir_lookup_component(ntfs_t *fs, uint32_t dir_record, const char *name, uint32_t *out_record)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, dir_record, rec) != 0) { free(rec); return -1; }
    ntfs_index_entry_t *e = 0;
    int rc = ntfs_dir_find_entry(rec, name, &e, 0);
    if (rc == 0 && out_record) *out_record = (uint32_t)e->mft_ref;
    free(rec);
    return rc;
}

static int ntfs_dir_remove_entry(ntfs_t *fs, uint32_t dir_record, const char *name)
{
    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, dir_record, rec) != 0) { free(rec); return -1; }

    ntfs_attr_hdr_t *ir_attr;
    ntfs_index_root_t *ir = ntfs_dir_index_root(rec, &ir_attr);
    if (!ir) { free(rec); return -1; }

    uint8_t *base = (uint8_t *)ir + ir->entries_offset;
    uint32_t pos = 0;
    ntfs_index_entry_t *found = 0;
    while (pos < ir->index_length) {
        ntfs_index_entry_t *e = (ntfs_index_entry_t *)(base + pos);
        if (e->flags & NTFS_INDEX_ENTRY_LAST) break;
        ntfs_file_name_t *fn = (ntfs_file_name_t *)((uint8_t *)e + sizeof(ntfs_index_entry_t));
        if (ntfs_name_eq((uint8_t *)fn + sizeof(ntfs_file_name_t), fn->name_length, name)) {
            found = e;
            break;
        }
        pos += e->entry_length;
    }
    if (!found) { free(rec); return -1; }

    uint32_t entry_len = found->entry_length;
    uint8_t *after = (uint8_t *)found + entry_len;
    uint32_t tail_len = (uint32_t)(base + ir->index_length - after);
    memmove(found, after, tail_len);

    ir->index_length -= entry_len;
    ir->allocated_size -= entry_len;
    ntfs_attr_shrink(rec, ir_attr, entry_len);

    int rc = ntfs_write_record(fs, dir_record, rec);
    free(rec);
    return rc;
}

/* ---- path split/walk ---- */

static int ntfs_split_path(const char *path, char *parent, size_t parent_sz, char *name, size_t name_sz)
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

static int ntfs_walk(ntfs_t *fs, const char *path, uint32_t *out_record)
{
    uint32_t cur = NTFS_REC_ROOT;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[NTFS_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < NTFS_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        while (*p == '/') p++;

        uint32_t next;
        if (ntfs_dir_lookup_component(fs, cur, comp, &next) != 0) return -1;
        cur = next;
    }
    *out_record = cur;
    return 0;
}

/* ---- VFS callbacks ---- */

static int ntfs_vfs_open(void *ctx, const char *path, int flags)
{
    ntfs_t *fs = (ntfs_t *)ctx;

    char parent_path[256];
    char name[NTFS_MAX_FILENAME + 1];
    if (ntfs_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_record;
    if (ntfs_walk(fs, parent_path, &parent_record) != 0) return -1;

    uint32_t record;
    int is_dir = 0;
    uint64_t file_size = 0;
    int found = (ntfs_dir_lookup_component(fs, parent_record, name, &record) == 0);

    if (!found) {
        if (!(flags & VFS_CREAT)) return -1;
        uint32_t new_record;
        if (ntfs_alloc_record(fs, &new_record) != 0) return -1;
        if (ntfs_record_create(fs, new_record, parent_record, name, 0) != 0) return -1;
        if (ntfs_dir_add_entry(fs, parent_record, name, (uint64_t)new_record, NTFS_FA_ARCHIVE) != 0) return -1;
        record = new_record;
    } else {
        uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
        if (!rec) return -1;
        if (ntfs_read_record(fs, record, rec) != 0) { free(rec); return -1; }
        ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)rec;
        is_dir = (hdr->flags & NTFS_FILE_RECORD_IS_DIR) ? 1 : 0;
        if (is_dir) { free(rec); return -1; }

        ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
        if (flags & VFS_TRUNC) {
            if (data && data->non_resident) {
                ntfs_attr_nonres_t *nr = ntfs_attr_nonres_hdr(data);
                nr->file_size = 0;
                nr->initialized_size = 0;
                ntfs_write_record(fs, record, rec);
            }
        } else if (data && data->non_resident) {
            file_size = ntfs_attr_nonres_hdr(data)->file_size;
        }
        free(rec);
    }

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].record = record;
            fs->fds[i].is_dir = is_dir;
            fs->fds[i].size = (uint32_t)file_size;
            fs->fds[i].pos = (flags & VFS_APPEND) ? (uint32_t)file_size : 0;
            return i;
        }
    }
    return -1;
}

static int ntfs_vfs_close(void *ctx, int fd)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int ntfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    int n = ntfs_data_read(fs, fs->fds[fd].record, fs->fds[fd].pos, buf, size);
    if (n > 0) fs->fds[fd].pos += (uint32_t)n;
    return n;
}

static int ntfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    int n = ntfs_data_write(fs, fs->fds[fd].record, fs->fds[fd].pos, buf, size);
    if (n > 0) {
        fs->fds[fd].pos += (uint32_t)n;
        if (fs->fds[fd].pos > fs->fds[fd].size) fs->fds[fd].size = fs->fds[fd].pos;
    }
    return n;
}

static int ntfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;

    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;

    return (int)fs->fds[fd].pos;
}

static int ntfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    uint32_t dir_record;
    if (ntfs_walk(fs, path, &dir_record) != 0) return -1;

    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, dir_record, rec) != 0) { free(rec); return -1; }

    ntfs_index_root_t *ir = ntfs_dir_index_root(rec, 0);
    if (!ir) { free(rec); return -1; }

    uint8_t *child = (uint8_t *)malloc(fs->mft_record_size);
    uint8_t *base = (uint8_t *)ir + ir->entries_offset;
    int count = 0;
    uint32_t pos = 0;
    while (pos < ir->index_length && count < max) {
        ntfs_index_entry_t *e = (ntfs_index_entry_t *)(base + pos);
        if (e->flags & NTFS_INDEX_ENTRY_LAST) break;
        ntfs_file_name_t *fn = (ntfs_file_name_t *)((uint8_t *)e + sizeof(ntfs_index_entry_t));

        ntfs_name_decode((uint8_t *)fn + sizeof(ntfs_file_name_t), fn->name_length, entries[count].name, sizeof(entries[count].name));
        entries[count].is_dir = (fn->file_attributes & NTFS_FA_DIRECTORY) ? 1 : 0;
        entries[count].inode = (uint32_t)e->mft_ref;
        entries[count].mode = fn->file_attributes;
        entries[count].size = 0;
        if (!entries[count].is_dir && child && ntfs_read_record(fs, (uint32_t)e->mft_ref, child) == 0) {
            ntfs_attr_hdr_t *data = ntfs_attr_find(child, NTFS_ATTR_DATA);
            if (data && data->non_resident) entries[count].size = (uint32_t)ntfs_attr_nonres_hdr(data)->file_size;
        }
        count++;
        pos += e->entry_length;
    }
    if (child) free(child);
    free(rec);
    return count;
}

static int ntfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    ntfs_t *fs = (ntfs_t *)ctx;

    char parent_path[256];
    char name[NTFS_MAX_FILENAME + 1];
    if (ntfs_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_record;
    if (ntfs_walk(fs, parent_path, &parent_record) != 0) return -1;

    uint32_t existing;
    if (ntfs_dir_lookup_component(fs, parent_record, name, &existing) == 0) return -1;

    uint32_t new_record;
    if (ntfs_alloc_record(fs, &new_record) != 0) return -1;
    if (ntfs_record_create(fs, new_record, parent_record, name, 1) != 0) return -1;
    if (ntfs_dir_add_entry(fs, parent_record, name, (uint64_t)new_record, NTFS_FA_DIRECTORY) != 0) return -1;
    return 0;
}

static int ntfs_vfs_unlink(void *ctx, const char *path)
{
    ntfs_t *fs = (ntfs_t *)ctx;

    char parent_path[256];
    char name[NTFS_MAX_FILENAME + 1];
    if (ntfs_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;

    uint32_t parent_record;
    if (ntfs_walk(fs, parent_path, &parent_record) != 0) return -1;

    uint32_t record;
    if (ntfs_dir_lookup_component(fs, parent_record, name, &record) != 0) return -1;

    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, record, rec) != 0) { free(rec); return -1; }
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)rec;
    int is_dir = (hdr->flags & NTFS_FILE_RECORD_IS_DIR) ? 1 : 0;

    if (is_dir && !ntfs_dir_is_empty(rec)) { free(rec); return -1; }

    if (ntfs_dir_remove_entry(fs, parent_record, name) != 0) { free(rec); return -1; }

    if (!is_dir) {
        ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
        if (data && data->non_resident) {
            const uint8_t *p = ntfs_attr_runlist(data);
            int64_t cur_lcn = 0;
            while (*p != 0) {
                uint32_t len; int32_t delta;
                memcpy(&len, p + 1, 4);
                memcpy(&delta, p + 5, 4);
                cur_lcn += delta;
                ntfs_free_clusters(fs, (uint64_t)cur_lcn, len);
                p += 9;
            }
        }
    }
    hdr->flags &= ~NTFS_FILE_RECORD_IN_USE;
    ntfs_write_record(fs, record, rec);
    free(rec);
    return 0;
}

static int ntfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    ntfs_t *fs = (ntfs_t *)ctx;

    const char *p = path;
    while (*p == '/') p++;

    uint32_t record;
    char name[NTFS_MAX_FILENAME + 1];

    if (!*p) {
        record = NTFS_REC_ROOT;
        name[0] = 0;
    } else {
        char parent_path[256];
        if (ntfs_split_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) != 0) return -1;
        uint32_t parent_record;
        if (ntfs_walk(fs, parent_path, &parent_record) != 0) return -1;
        if (ntfs_dir_lookup_component(fs, parent_record, name, &record) != 0) return -1;
    }

    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, record, rec) != 0) { free(rec); return -1; }
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)rec;
    int is_dir = (hdr->flags & NTFS_FILE_RECORD_IS_DIR) ? 1 : 0;

    uint32_t size = 0;
    if (!is_dir) {
        ntfs_attr_hdr_t *data = ntfs_attr_find(rec, NTFS_ATTR_DATA);
        if (data && data->non_resident) size = (uint32_t)ntfs_attr_nonres_hdr(data)->file_size;
    }

    int k = 0;
    while (name[k] && k < VFS_NAME_LEN - 1) { entry->name[k] = name[k]; k++; }
    entry->name[k] = 0;
    entry->size = size;
    entry->is_dir = is_dir;
    entry->inode = record;
    entry->mode = is_dir ? NTFS_FA_DIRECTORY : NTFS_FA_ARCHIVE;
    free(rec);
    return 0;
}

static int ntfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    ntfs_t *fs = (ntfs_t *)ctx;

    char old_parent_path[256], old_name[NTFS_MAX_FILENAME + 1];
    if (ntfs_split_path(old, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) != 0) return -1;
    uint32_t old_parent;
    if (ntfs_walk(fs, old_parent_path, &old_parent) != 0) return -1;

    uint32_t record;
    if (ntfs_dir_lookup_component(fs, old_parent, old_name, &record) != 0) return -1;

    uint8_t *rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!rec) return -1;
    if (ntfs_read_record(fs, record, rec) != 0) { free(rec); return -1; }
    ntfs_record_hdr_t *hdr = (ntfs_record_hdr_t *)rec;
    uint32_t file_attributes = (hdr->flags & NTFS_FILE_RECORD_IS_DIR) ? NTFS_FA_DIRECTORY : NTFS_FA_ARCHIVE;
    free(rec);

    char new_parent_path[256], new_name[NTFS_MAX_FILENAME + 1];
    if (ntfs_split_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name)) != 0) return -1;
    uint32_t new_parent;
    if (ntfs_walk(fs, new_parent_path, &new_parent) != 0) return -1;

    if (ntfs_dir_add_entry(fs, new_parent, new_name, (uint64_t)record, file_attributes) != 0) return -1;
    ntfs_dir_remove_entry(fs, old_parent, old_name);

    rec = (uint8_t *)malloc(fs->mft_record_size);
    if (rec) {
        if (ntfs_read_record(fs, record, rec) == 0) {
            ntfs_attr_hdr_t *fn_attr = ntfs_attr_find(rec, NTFS_ATTR_FILE_NAME);
            if (fn_attr) {
                ntfs_file_name_t *fn = (ntfs_file_name_t *)ntfs_attr_res_value(fn_attr);
                fn->parent_ref = (uint64_t)new_parent;
                ntfs_write_record(fs, record, rec);
            }
        }
        free(rec);
    }
    return 0;
}

static int ntfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void ntfs_mount_vfs(ntfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ntfs_vfs_ops = {
        .open = ntfs_vfs_open,
        .close = ntfs_vfs_close,
        .read = ntfs_vfs_read,
        .write = ntfs_vfs_write,
        .lseek = ntfs_vfs_lseek,
        .readdir = ntfs_vfs_readdir,
        .mkdir = ntfs_vfs_mkdir,
        .unlink = ntfs_vfs_unlink,
        .stat = ntfs_vfs_stat,
        .rename = ntfs_vfs_rename,
        .symlink = ntfs_vfs_symlink,
    };
    vfs_mount(mount_point, &ntfs_vfs_ops, fs);
}

/* ---- format ---- */

int ntfs_format(blockdev_t *bd, const char *label)
{
    (void)label;
    uint32_t bytes_per_sector = bd->sector_size ? bd->sector_size : 512;
    uint32_t sectors_per_cluster = NTFS_CLUSTER_SIZE / bytes_per_sector;
    if (sectors_per_cluster == 0) sectors_per_cluster = 1;

    uint64_t total_sectors = bd->total_sectors;
    uint64_t total_bytes = total_sectors * bytes_per_sector;
    uint64_t total_clusters = total_bytes / NTFS_CLUSTER_SIZE;

    uint32_t mft_initial_clusters = 64;
    if (total_clusters < (uint64_t)(mft_initial_clusters + 16)) return -1;

    uint32_t bitmap_bytes = (uint32_t)((total_clusters + 7) / 8);
    uint32_t bitmap_overhead = (uint32_t)(sizeof(ntfs_record_hdr_t) + sizeof(ntfs_attr_hdr_t) + sizeof(ntfs_attr_res_t) + 16);
    if (bitmap_bytes + bitmap_overhead > NTFS_RECORD_SIZE) return -1;

    uint64_t mft_lcn = 1;

    uint8_t *zero = (uint8_t *)malloc(NTFS_CLUSTER_SIZE);
    if (!zero) return -1;
    memset(zero, 0, NTFS_CLUSTER_SIZE);
    if (blockdev_write_bytes(bd, 0, NTFS_CLUSTER_SIZE, zero) != 0) { free(zero); return -1; }
    for (uint32_t c = 0; c < mft_initial_clusters; c++) {
        if (blockdev_write_bytes(bd, (mft_lcn + c) * NTFS_CLUSTER_SIZE, NTFS_CLUSTER_SIZE, zero) != 0) { free(zero); return -1; }
    }
    free(zero);

    uint8_t *boot = (uint8_t *)malloc(bytes_per_sector);
    if (!boot) return -1;
    memset(boot, 0, bytes_per_sector);
    ntfs_boot_sector_t *bs = (ntfs_boot_sector_t *)boot;
    bs->jump[0] = 0xEB; bs->jump[1] = 0x52; bs->jump[2] = 0x90;
    memcpy(bs->oem_id, "NTFS    ", 8);
    bs->bytes_per_sector = (uint16_t)bytes_per_sector;
    bs->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bs->media_descriptor = 0xF8;
    bs->total_sectors = total_sectors;
    bs->mft_lcn = mft_lcn;
    bs->mftmirr_lcn = 0;
    bs->clusters_per_mft_record = 1;
    bs->clusters_per_index_record = 1;
    bs->volume_serial = 0x1122334455667788ULL;
    bs->end_signature = 0xAA55;
    int rc = blockdev_write_bytes(bd, 0, bytes_per_sector, boot);
    free(boot);
    if (rc != 0) return -1;

    ntfs_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.bd = bd;
    fmt.bytes_per_sector = bytes_per_sector;
    fmt.sectors_per_cluster = sectors_per_cluster;
    fmt.cluster_size = NTFS_CLUSTER_SIZE;
    fmt.total_clusters = total_clusters;
    fmt.mft_lcn = mft_lcn;
    fmt.mft_record_size = NTFS_RECORD_SIZE;

    uint8_t *rec = (uint8_t *)malloc(NTFS_RECORD_SIZE);
    if (!rec) return -1;

    /* record 0: $MFT itself, describing the initial MFT cluster run */
    ntfs_record_init_header(rec, NTFS_REC_MFT, 0);
    if (!ntfs_record_add_data_with_run(rec, (int64_t)mft_lcn, mft_initial_clusters, (uint64_t)mft_initial_clusters * NTFS_RECORD_SIZE)) {
        free(rec); return -1;
    }
    if (ntfs_write_record(&fmt, NTFS_REC_MFT, rec) != 0) { free(rec); return -1; }

    /* record 6: $Bitmap, resident, marking the boot sector cluster and the initial MFT clusters used */
    ntfs_record_init_header(rec, NTFS_REC_BITMAP, 0);
    ntfs_attr_hdr_t *bm_attr = ntfs_record_add_resident_bitmap(rec, bitmap_bytes);
    if (!bm_attr) { free(rec); return -1; }
    uint8_t *bm = ntfs_attr_res_value(bm_attr);
    uint64_t used_clusters = mft_lcn + mft_initial_clusters;
    for (uint64_t i = 0; i < used_clusters; i++) bm[i / 8] |= (uint8_t)(1u << (i % 8));
    if (ntfs_write_record(&fmt, NTFS_REC_BITMAP, rec) != 0) { free(rec); return -1; }

    /* record 5: root directory, empty $INDEX_ROOT */
    ntfs_record_init_header(rec, NTFS_REC_ROOT, 1);
    if (!ntfs_record_add_empty_index_root(rec)) { free(rec); return -1; }
    if (ntfs_write_record(&fmt, NTFS_REC_ROOT, rec) != 0) { free(rec); return -1; }

    free(rec);
    return 0;
}

/* ---- mount/umount ---- */

int ntfs_probe_and_mount(ntfs_t *fs, blockdev_t *bd)
{
    uint8_t *boot = (uint8_t *)malloc(512);
    if (!boot) return -1;
    if (blockdev_read_bytes(bd, 0, 512, boot) != 0) { free(boot); return -1; }
    ntfs_boot_sector_t *bs = (ntfs_boot_sector_t *)boot;
    if (memcmp(bs->oem_id, "NTFS    ", 8) != 0) { free(boot); return -1; }

    memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    fs->bytes_per_sector = bs->bytes_per_sector ? bs->bytes_per_sector : 512;
    fs->sectors_per_cluster = bs->sectors_per_cluster ? bs->sectors_per_cluster : 8;
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->total_clusters = bs->total_sectors / fs->sectors_per_cluster;
    fs->mft_lcn = bs->mft_lcn;
    fs->mft_record_size = NTFS_RECORD_SIZE;
    free(boot);

    if (fs->cluster_size != NTFS_CLUSTER_SIZE) return -1;
    return 0;
}

int ntfs_umount(ntfs_t *fs)
{
    (void)fs;
    return 0;
}
