#include "btrfs.h"
#include "memory.h"
#include "string.h"

#define BTRFS_MAGIC         0x4D5F53526648425FULL
#define BTRFS_SUPER_OFFSET  65536ULL
#define BTRFS_SUPER_OFFSET2 67108864ULL

#define BTRFS_FS_TREE_OBJECTID  5ULL
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL
#define BTRFS_ROOT_DIR_OBJECTID 6ULL

#define BTRFS_INODE_ITEM_KEY  0x01
#define BTRFS_INODE_REF_KEY   0x0C
#define BTRFS_DIR_ITEM_KEY    0x54
#define BTRFS_DIR_INDEX_KEY   0x60
#define BTRFS_EXTENT_DATA_KEY 0x6C
#define BTRFS_CHUNK_ITEM_KEY  0xE4
#define BTRFS_ROOT_ITEM_KEY   0x84

#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR      2
#define BTRFS_FT_SYMLINK  7

typedef struct {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint64_t magic;
    uint64_t generation;
    uint64_t root;
    uint64_t chunk_root;
    uint64_t log_root;
    uint64_t log_root_transid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t leafsize;
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    uint8_t  root_level;
    uint8_t  chunk_root_level;
    uint8_t  log_root_level;
    uint8_t  dev_item[98];
    char     label[256];
    uint64_t cache_generation;
    uint64_t uuid_tree_generation;
    uint8_t  metadata_uuid[16];
    uint8_t  reserved[224];
    uint8_t  sys_chunk_array[2048];
    uint8_t  super_roots[672];
    uint8_t  unused[565];
} __attribute__((packed)) btrfs_super_t;

typedef struct {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
} __attribute__((packed)) btrfs_key_t;

typedef struct {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  chunk_tree_uuid[16];
    uint64_t generation;
    uint64_t owner;
    uint32_t nritems;
    uint8_t  level;
} __attribute__((packed)) btrfs_node_hdr_t;

typedef struct {
    btrfs_key_t key;
    uint64_t    blockptr;
    uint64_t    generation;
} __attribute__((packed)) btrfs_key_ptr_t;

typedef struct {
    btrfs_key_t key;
    uint32_t    data_offset;
    uint32_t    data_size;
} __attribute__((packed)) btrfs_item_t;

typedef struct {
    uint64_t generation;
    uint64_t transid;
    uint64_t size;
    uint64_t nbytes;
    uint64_t block_group;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t rdev;
    uint64_t flags;
    uint64_t sequence;
    uint64_t reserved[4];
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint64_t otime_sec;
    uint32_t otime_nsec;
} __attribute__((packed)) btrfs_inode_item_t;

typedef struct {
    btrfs_key_t location;
    uint64_t    transid;
    uint16_t    data_len;
    uint16_t    name_len;
    uint8_t     type;
} __attribute__((packed)) btrfs_dir_item_t;

typedef struct {
    uint64_t generation;
    uint64_t ram_bytes;
    uint8_t  compression;
    uint8_t  encryption;
    uint16_t other_encoding;
    uint8_t  type;
} __attribute__((packed)) btrfs_extent_data_t;

typedef struct {
    uint64_t disk_bytenr;
    uint64_t disk_num_bytes;
    uint64_t offset;
    uint64_t num_bytes;
} __attribute__((packed)) btrfs_extent_reg_t;

typedef struct {
    uint64_t length;
    uint64_t owner;
    uint64_t stripe_len;
    uint64_t type;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint16_t num_stripes;
    uint16_t sub_stripes;
} __attribute__((packed)) btrfs_chunk_t;

typedef struct {
    uint64_t devid;
    uint64_t offset;
    uint8_t  dev_uuid[16];
} __attribute__((packed)) btrfs_stripe_t;

#define BTRFS_NODE_HDR_SZ  101
#define BTRFS_KEY_PTR_SZ   33
#define BTRFS_ITEM_SZ      25
#define BTRFS_KEY_SZ       17

static int btrfs_logical_to_phys(btrfs_t *fs, uint64_t logical, uint64_t *phys)
{
    for (int i = 0; i < fs->num_chunks; i++) {
        btrfs_chunk_map_t *c = &fs->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) {
            *phys = c->physical + (logical - c->logical);
            return 0;
        }
    }
    *phys = logical;
    return 0;
}

static int btrfs_read_node(btrfs_t *fs, uint64_t logical, uint8_t *buf, uint32_t nodesize)
{
    uint64_t phys;
    if (btrfs_logical_to_phys(fs, logical, &phys) != 0) return -1;
    return blockdev_read_bytes(fs->bd, phys, nodesize, buf);
}

static void btrfs_parse_sys_chunk_array(btrfs_t *fs, const uint8_t *arr, uint32_t size)
{
    uint32_t pos = 0;
    while (pos + BTRFS_KEY_SZ + sizeof(btrfs_chunk_t) + sizeof(btrfs_stripe_t) <= size) {
        const btrfs_key_t *key = (const btrfs_key_t *)(arr + pos);
        pos += BTRFS_KEY_SZ;
        const btrfs_chunk_t *chunk = (const btrfs_chunk_t *)(arr + pos);
        uint16_t num_stripes = chunk->num_stripes;
        if (num_stripes < 1) num_stripes = 1;
        const btrfs_stripe_t *stripe = (const btrfs_stripe_t *)(arr + pos + sizeof(btrfs_chunk_t));

        if (fs->num_chunks < BTRFS_MAX_CHUNKS) {
            fs->chunks[fs->num_chunks].logical  = key->offset;
            fs->chunks[fs->num_chunks].length   = chunk->length;
            fs->chunks[fs->num_chunks].physical = stripe->offset;
            fs->num_chunks++;
        }
        pos += (uint32_t)sizeof(btrfs_chunk_t) + (uint32_t)num_stripes * (uint32_t)sizeof(btrfs_stripe_t);
    }
}

static int btrfs_walk_chunk_tree(btrfs_t *fs, uint8_t *node_buf)
{
    if (btrfs_read_node(fs, fs->chunk_root_logical, node_buf, fs->nodesize) != 0) return -1;
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
    if (hdr->level > 0) return 0;

    uint32_t nritems = hdr->nritems;
    uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
    uint8_t *data_end = node_buf + fs->nodesize;

    for (uint32_t i = 0; i < nritems; i++) {
        btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
        if (item->key.type != BTRFS_CHUNK_ITEM_KEY) continue;

        uint8_t *data = data_end - item->data_offset - item->data_size;
        if (data < node_buf || data + sizeof(btrfs_chunk_t) > data_end) continue;

        btrfs_chunk_t *chunk = (btrfs_chunk_t *)data;
        uint16_t num_stripes = chunk->num_stripes;
        if (num_stripes < 1) num_stripes = 1;
        uint8_t *stripe_data = data + sizeof(btrfs_chunk_t);
        if (stripe_data + sizeof(btrfs_stripe_t) > data_end) continue;

        btrfs_stripe_t *stripe = (btrfs_stripe_t *)stripe_data;
        if (fs->num_chunks < BTRFS_MAX_CHUNKS) {
            int dup = 0;
            for (int j = 0; j < fs->num_chunks; j++) {
                if (fs->chunks[j].logical == item->key.offset) { dup = 1; break; }
            }
            if (!dup) {
                fs->chunks[fs->num_chunks].logical  = item->key.offset;
                fs->chunks[fs->num_chunks].length   = chunk->length;
                fs->chunks[fs->num_chunks].physical = stripe->offset;
                fs->num_chunks++;
            }
        }
    }
    return 0;
}

static int btrfs_find_root(btrfs_t *fs, uint8_t *node_buf, uint64_t objectid, uint64_t *root_logical)
{
    if (btrfs_read_node(fs, fs->root_logical, node_buf, fs->nodesize) != 0) return -1;
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;

    uint32_t nritems = hdr->nritems;

    if (hdr->level > 0) {
        uint8_t *ptrs_base = node_buf + BTRFS_NODE_HDR_SZ;
        for (uint32_t i = 0; i < nritems; i++) {
            btrfs_key_ptr_t *kp = (btrfs_key_ptr_t *)(ptrs_base + i * BTRFS_KEY_PTR_SZ);
            if (kp->key.objectid == objectid && kp->key.type == BTRFS_ROOT_ITEM_KEY) {
                uint8_t *child = (uint8_t *)malloc(fs->nodesize);
                if (!child) return -1;
                if (btrfs_read_node(fs, kp->blockptr, child, fs->nodesize) == 0) {
                    btrfs_node_hdr_t *chdr = (btrfs_node_hdr_t *)child;
                    if (chdr->level == 0) {
                        uint8_t *citems_base = child + BTRFS_NODE_HDR_SZ;
                        uint8_t *cdata_end = child + fs->nodesize;
                        for (uint32_t j = 0; j < chdr->nritems; j++) {
                            btrfs_item_t *ci = (btrfs_item_t *)(citems_base + j * BTRFS_ITEM_SZ);
                            if (ci->key.objectid == objectid && ci->key.type == BTRFS_ROOT_ITEM_KEY) {
                                uint8_t *cdata = cdata_end - ci->data_offset - ci->data_size;
                                if (cdata >= child && ci->data_size >= 8) {
                                    memcpy(root_logical, cdata, 8);
                                    free(child);
                                    return 0;
                                }
                            }
                        }
                    }
                }
                free(child);
                return -1;
            }
        }
        return -1;
    }

    uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
    uint8_t *data_end = node_buf + fs->nodesize;

    for (uint32_t i = 0; i < nritems; i++) {
        btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
        if (item->key.objectid == objectid && item->key.type == BTRFS_ROOT_ITEM_KEY) {
            uint8_t *data = data_end - item->data_offset - item->data_size;
            if (data >= node_buf && item->data_size >= 8) {
                memcpy(root_logical, data, 8);
                return 0;
            }
        }
    }
    return -1;
}

static int btrfs_leaf_find_key(btrfs_t *fs, uint8_t *leaf, uint64_t objectid, uint8_t type, uint64_t offset_min,
                                btrfs_item_t **item_out, uint8_t **data_out)
{
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)leaf;
    if (hdr->level != 0) return -1;

    uint32_t nritems = hdr->nritems;
    uint8_t *items_base = leaf + BTRFS_NODE_HDR_SZ;
    uint8_t *data_end = leaf + fs->nodesize;

    for (uint32_t i = 0; i < nritems; i++) {
        btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
        if (item->key.objectid == objectid && item->key.type == type &&
            item->key.offset >= offset_min) {
            uint8_t *data = data_end - item->data_offset - item->data_size;
            if (data < leaf) continue;
            *item_out = item;
            *data_out = data;
            return 0;
        }
    }
    return -1;
}

static int btrfs_read_tree_leaf(btrfs_t *fs, uint64_t tree_root, uint8_t *node_buf,
                                 uint64_t objectid, uint8_t type)
{
    if (btrfs_read_node(fs, tree_root, node_buf, fs->nodesize) != 0) return -1;
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;

    int max_levels = 10;
    while (hdr->level > 0 && max_levels-- > 0) {
        uint32_t nritems = hdr->nritems;
        uint8_t *ptrs_base = node_buf + BTRFS_NODE_HDR_SZ;
        uint64_t best_block = 0;
        int found = 0;

        for (uint32_t i = 0; i < nritems; i++) {
            btrfs_key_ptr_t *kp = (btrfs_key_ptr_t *)(ptrs_base + i * BTRFS_KEY_PTR_SZ);
            if (kp->key.objectid < objectid ||
                (kp->key.objectid == objectid && kp->key.type <= type)) {
                best_block = kp->blockptr;
                found = 1;
            } else {
                break;
            }
        }
        if (!found && nritems > 0) {
            btrfs_key_ptr_t *kp = (btrfs_key_ptr_t *)(ptrs_base);
            best_block = kp->blockptr;
            found = 1;
        }
        if (!found) return -1;

        if (btrfs_read_node(fs, best_block, node_buf, fs->nodesize) != 0) return -1;
        hdr = (btrfs_node_hdr_t *)node_buf;
    }
    return 0;
}


static int btrfs_walk_path(btrfs_t *fs, uint64_t fs_tree, uint8_t *node_buf,
                            const char *path, uint64_t *out_ino, int *out_is_dir)
{
    uint64_t cur_ino = BTRFS_ROOT_DIR_OBJECTID;
    int is_dir = 1;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[BTRFS_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < BTRFS_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        if (i == 0) continue;

        if (!is_dir) return -1;

        if (btrfs_read_tree_leaf(fs, fs_tree, node_buf, cur_ino, BTRFS_DIR_ITEM_KEY) != 0)
            return -1;

        btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
        uint32_t nritems = hdr->nritems;
        uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
        uint8_t *data_end = node_buf + fs->nodesize;

        int found = 0;
        for (uint32_t j = 0; j < nritems && !found; j++) {
            btrfs_item_t *item = (btrfs_item_t *)(items_base + j * BTRFS_ITEM_SZ);
            if (item->key.objectid != cur_ino) continue;
            if (item->key.type != BTRFS_DIR_ITEM_KEY && item->key.type != BTRFS_DIR_INDEX_KEY) continue;

            uint8_t *data = data_end - item->data_offset - item->data_size;
            if (data < node_buf) continue;

            uint8_t *end = data + item->data_size;
            uint8_t *dp = data;
            while (dp + sizeof(btrfs_dir_item_t) <= end) {
                btrfs_dir_item_t *di = (btrfs_dir_item_t *)dp;
                uint8_t *name_ptr = dp + sizeof(btrfs_dir_item_t);
                if (name_ptr + di->name_len > end) break;
                if (di->name_len == (uint16_t)i &&
                    memcmp(name_ptr, comp, (size_t)i) == 0) {
                    cur_ino = di->location.objectid;
                    is_dir = (di->type == BTRFS_FT_DIR);
                    found = 1;
                    break;
                }
                dp += sizeof(btrfs_dir_item_t) + di->name_len + di->data_len;
            }
        }
        if (!found) return -1;
    }
    *out_ino = cur_ino;
    if (out_is_dir) *out_is_dir = is_dir;
    return 0;
}

static int btrfs_get_inode_size(btrfs_t *fs, uint64_t fs_tree, uint8_t *node_buf,
                                 uint64_t ino, uint64_t *out_size, uint32_t *out_mode)
{
    if (btrfs_read_tree_leaf(fs, fs_tree, node_buf, ino, BTRFS_INODE_ITEM_KEY) != 0) return -1;

    btrfs_item_t *item;
    uint8_t *data;
    if (btrfs_leaf_find_key(fs, node_buf, ino, BTRFS_INODE_ITEM_KEY, 0, &item, &data) != 0) return -1;
    if (item->data_size < sizeof(btrfs_inode_item_t)) return -1;

    btrfs_inode_item_t *ii = (btrfs_inode_item_t *)data;
    if (out_size) *out_size = ii->size;
    if (out_mode) *out_mode = ii->mode;
    return 0;
}

int btrfs_probe_and_mount(btrfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(btrfs_t));
    fs->bd = bd;

    btrfs_super_t *sb = (btrfs_super_t *)malloc(sizeof(btrfs_super_t));
    if (!sb) return -1;

    if (blockdev_read_bytes(bd, BTRFS_SUPER_OFFSET, sizeof(btrfs_super_t), sb) != 0) {
        free(sb);
        return -1;
    }

    if (sb->magic != BTRFS_MAGIC) {
        if (blockdev_read_bytes(bd, BTRFS_SUPER_OFFSET2, sizeof(btrfs_super_t), sb) != 0 ||
            sb->magic != BTRFS_MAGIC) {
            free(sb);
            return -1;
        }
    }

    fs->generation       = sb->generation;
    fs->root_logical     = sb->root;
    fs->chunk_root_logical = sb->chunk_root;
    fs->total_bytes      = sb->total_bytes;
    fs->bytes_used       = sb->bytes_used;
    fs->sectorsize       = sb->sectorsize ? sb->sectorsize : 4096;
    fs->nodesize         = sb->nodesize   ? sb->nodesize   : 16384;
    fs->leafsize         = sb->leafsize   ? sb->leafsize   : 16384;

    int label_len = 0;
    while (label_len < 255 && sb->label[label_len]) label_len++;
    memcpy(fs->label, sb->label, (size_t)label_len);
    fs->label[label_len] = 0;

    /* sys_chunk_array_size is an independent on-disk uint32_t field,
     * not derived from the actual sys_chunk_array[2048] member size
     * -- a crafted superblock (a mounted, potentially hostile btrfs
     * image) can set it arbitrarily large, and the parser below only
     * bounds its walk against whatever `size` it's given. Clamp to
     * the real array size so it can never read past sb's own
     * malloc(sizeof(btrfs_super_t)) heap allocation. */
    uint32_t chunk_arr_size = sb->sys_chunk_array_size;
    if (chunk_arr_size > sizeof(sb->sys_chunk_array)) chunk_arr_size = sizeof(sb->sys_chunk_array);
    btrfs_parse_sys_chunk_array(fs, sb->sys_chunk_array, chunk_arr_size);
    free(sb);

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    btrfs_walk_chunk_tree(fs, node_buf);

    fs->next_objectid = 7;
    if (fs->num_chunks > 0)
        fs->next_data_logical = fs->chunks[0].logical + 3 * fs->nodesize;
    else
        fs->next_data_logical = 4 * 1024 * 1024ULL;
    fs->fs_tree_logical = 0;

    /* Scan FS tree to find real next_objectid and next_data_logical */
    {
        uint64_t fst;
        if (btrfs_find_root(fs, node_buf, BTRFS_FS_TREE_OBJECTID, &fst) == 0) {
            fs->fs_tree_logical = fst;
            if (btrfs_read_node(fs, fst, node_buf, fs->nodesize) == 0) {
                btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
                uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
                uint8_t *data_end = node_buf + fs->nodesize;
                for (uint32_t i = 0; i < hdr->nritems; i++) {
                    btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
                    if (item->key.type == BTRFS_INODE_ITEM_KEY &&
                        item->key.objectid >= fs->next_objectid)
                        fs->next_objectid = item->key.objectid + 1;
                    if (item->key.type == BTRFS_EXTENT_DATA_KEY) {
                        uint8_t *d = data_end - item->data_offset - item->data_size;
                        if (d >= node_buf &&
                            item->data_size >= (uint32_t)(sizeof(btrfs_extent_data_t) + sizeof(btrfs_extent_reg_t))) {
                            btrfs_extent_data_t *ed = (btrfs_extent_data_t *)d;
                            if (ed->type == 1) {
                                btrfs_extent_reg_t *reg = (btrfs_extent_reg_t *)(d + sizeof(btrfs_extent_data_t));
                                uint64_t end = reg->disk_bytenr + reg->disk_num_bytes;
                                if (end > fs->next_data_logical)
                                    fs->next_data_logical = (end + 511) & ~(uint64_t)511;
                            }
                        }
                    }
                }
            }
        }
    }

    free(node_buf);
    return 0;
}

int btrfs_umount(btrfs_t *fs)
{
    (void)fs;
    return 0;
}

static int btrfs_get_fs_tree(btrfs_t *fs, uint8_t *node_buf, uint64_t *fs_tree_out)
{
    return btrfs_find_root(fs, node_buf, BTRFS_FS_TREE_OBJECTID, fs_tree_out);
}

/* Forward declarations for write helpers defined later */
static uint32_t btrfs_leaf_data_total(uint8_t *leaf);
static int btrfs_leaf_find_exact(uint8_t *leaf, uint64_t objectid, uint8_t type, uint64_t offset);
static int btrfs_leaf_insert(btrfs_t *fs, uint8_t *leaf, btrfs_key_t *key, const void *data, uint32_t data_size);
static void btrfs_leaf_remove_at(btrfs_t *fs, uint8_t *leaf, uint32_t pos);
static int btrfs_load_fs_leaf(btrfs_t *fs, uint8_t *leaf);
static int btrfs_write_fs_leaf(btrfs_t *fs, uint8_t *leaf);
static void btrfs_split_path(const char *path, char *parent, char *name);
static int btrfs_add_dir_entry(btrfs_t *fs, uint8_t *leaf, uint64_t dir_ino,
                                uint64_t child_ino, const char *name, uint8_t ftype, uint8_t mode_hi);

static int btrfs_vfs_open(void *ctx, const char *path, int flags)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t ino;
    int is_dir = 0;
    int found = (btrfs_walk_path(fs, fs_tree, node_buf, path, &ino, &is_dir) == 0);

    if (!found) {
        if (!(flags & VFS_CREAT)) { free(node_buf); return -1; }

        char parent[256], name[BTRFS_MAX_FILENAME + 1];
        btrfs_split_path(path, parent, name);
        if (!name[0]) { free(node_buf); return -1; }

        uint64_t dir_ino;
        int dir_is_dir = 0;
        if (btrfs_walk_path(fs, fs_tree, node_buf, parent, &dir_ino, &dir_is_dir) != 0 || !dir_is_dir) {
            free(node_buf); return -1;
        }

        ino = fs->next_objectid++;

        if (btrfs_load_fs_leaf(fs, node_buf) != 0) { free(node_buf); return -1; }
        if (btrfs_add_dir_entry(fs, node_buf, dir_ino, ino, name, BTRFS_FT_REG_FILE, 0) != 0) {
            free(node_buf); return -1;
        }
        btrfs_write_fs_leaf(fs, node_buf);
        is_dir = 0;
    } else if (is_dir) {
        free(node_buf); return -1;
    } else if (flags & VFS_TRUNC) {
        /* Truncate: remove existing extent data */
        if (btrfs_load_fs_leaf(fs, node_buf) == 0) {
            int epos;
            while ((epos = btrfs_leaf_find_exact(node_buf, ino, BTRFS_EXTENT_DATA_KEY, 0)) >= 0)
                btrfs_leaf_remove_at(fs, node_buf, (uint32_t)epos);
            /* Clear size in inode */
            int ipos = btrfs_leaf_find_exact(node_buf, ino, BTRFS_INODE_ITEM_KEY, 0);
            if (ipos >= 0) {
                btrfs_item_t *it = (btrfs_item_t *)(node_buf + BTRFS_NODE_HDR_SZ + (uint32_t)ipos * BTRFS_ITEM_SZ);
                uint8_t *d = node_buf + fs->nodesize - it->data_offset - it->data_size;
                if (it->data_size >= sizeof(btrfs_inode_item_t))
                    ((btrfs_inode_item_t *)d)->size = 0;
            }
            btrfs_write_fs_leaf(fs, node_buf);
        }
    }

    uint64_t size = 0;
    uint32_t mode = 0;
    btrfs_get_inode_size(fs, fs_tree, node_buf, ino, &size, &mode);
    free(node_buf);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used   = 1;
            fs->fds[i].ino    = ino;
            fs->fds[i].pos    = 0;
            fs->fds[i].size   = (uint32_t)size;
            fs->fds[i].is_dir = 0;
            fs->fds[i].dirty  = 0;
            return i;
        }
    }
    return -1;
}

static int btrfs_vfs_close(void *ctx, int fd)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int btrfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    if (fs->fds[fd].is_dir) return -1;

    uint32_t pos = fs->fds[fd].pos;
    uint32_t file_size = fs->fds[fd].size;
    if (pos >= file_size) return 0;
    if (pos + size > file_size) size = file_size - pos;
    if (size == 0) return 0;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t ino = fs->fds[fd].ino;
    if (btrfs_read_tree_leaf(fs, fs_tree, node_buf, ino, BTRFS_EXTENT_DATA_KEY) != 0) {
        free(node_buf);
        return -1;
    }

    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
    uint32_t nritems = hdr->nritems;
    uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
    uint8_t *data_end = node_buf + fs->nodesize;

    uint32_t done = 0;
    memset(buf, 0, size);

    for (uint32_t i = 0; i < nritems && done < size; i++) {
        btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
        if (item->key.objectid != ino) continue;
        if (item->key.type != BTRFS_EXTENT_DATA_KEY) continue;

        uint64_t extent_file_off = item->key.offset;
        uint8_t *data = data_end - item->data_offset - item->data_size;
        if (data < node_buf || item->data_size < sizeof(btrfs_extent_data_t)) continue;

        btrfs_extent_data_t *ed = (btrfs_extent_data_t *)data;
        uint8_t *payload = data + sizeof(btrfs_extent_data_t);

        if (ed->type == 0) {
            uint32_t inline_size = item->data_size - (uint32_t)sizeof(btrfs_extent_data_t);
            for (uint32_t b = 0; b < inline_size && done < size; b++) {
                uint64_t file_pos = extent_file_off + b;
                if (file_pos < pos) continue;
                if (file_pos >= pos + size) break;
                ((uint8_t *)buf)[file_pos - pos] = payload[b];
                done++;
            }
        } else if (ed->type == 1) {
            if (item->data_size < sizeof(btrfs_extent_data_t) + sizeof(btrfs_extent_reg_t)) continue;
            btrfs_extent_reg_t *reg = (btrfs_extent_reg_t *)payload;
            if (reg->disk_bytenr == 0) continue;

            uint64_t num_bytes = reg->num_bytes;
            uint64_t disk_off  = reg->disk_bytenr + reg->offset;

            uint64_t start_file = extent_file_off;
            uint64_t end_file   = extent_file_off + num_bytes;

            uint64_t copy_start = (uint64_t)pos > start_file ? (uint64_t)pos : start_file;
            uint64_t copy_end   = ((uint64_t)pos + size) < end_file ? ((uint64_t)pos + size) : end_file;
            if (copy_start >= copy_end) continue;

            uint64_t phys;
            btrfs_logical_to_phys(fs, disk_off + (copy_start - start_file), &phys);
            uint32_t read_len = (uint32_t)(copy_end - copy_start);
            blockdev_read_bytes(fs->bd, phys, read_len, (uint8_t *)buf + (copy_start - pos));
            done += read_len;
        }
    }

    free(node_buf);
    fs->fds[fd].pos += done;
    return (int)done;
}

/* ---- leaf write helpers ---- */

static uint32_t btrfs_leaf_data_total(uint8_t *leaf)
{
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)leaf;
    uint32_t total = 0;
    uint8_t *ib = leaf + BTRFS_NODE_HDR_SZ;
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        btrfs_item_t *it = (btrfs_item_t *)(ib + i * BTRFS_ITEM_SZ);
        total += it->data_size;
    }
    return total;
}

static int btrfs_leaf_find_exact(uint8_t *leaf, uint64_t objectid, uint8_t type, uint64_t offset)
{
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)leaf;
    uint8_t *ib = leaf + BTRFS_NODE_HDR_SZ;
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        btrfs_item_t *it = (btrfs_item_t *)(ib + i * BTRFS_ITEM_SZ);
        if (it->key.objectid == objectid && it->key.type == type && it->key.offset == offset)
            return (int)i;
    }
    return -1;
}

/* Insert item sorted by (objectid,type,offset). Data goes after all existing data. */
static int btrfs_leaf_insert(btrfs_t *fs, uint8_t *leaf, btrfs_key_t *key,
                              const void *data, uint32_t data_size)
{
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)leaf;
    uint32_t nr = hdr->nritems;
    uint32_t total_data = btrfs_leaf_data_total(leaf);
    uint32_t free_space = fs->nodesize - BTRFS_NODE_HDR_SZ - (nr + 1) * BTRFS_ITEM_SZ - total_data - data_size;
    if ((int32_t)free_space < 0) return -1;

    /* Find sorted insertion position */
    uint8_t *ib = leaf + BTRFS_NODE_HDR_SZ;
    uint32_t pos = nr;
    for (uint32_t i = 0; i < nr; i++) {
        btrfs_item_t *it = (btrfs_item_t *)(ib + i * BTRFS_ITEM_SZ);
        if (it->key.objectid > key->objectid ||
            (it->key.objectid == key->objectid && it->key.type > key->type) ||
            (it->key.objectid == key->objectid && it->key.type == key->type && it->key.offset >= key->offset)) {
            pos = i;
            break;
        }
    }

    if (pos < nr)
        memmove(ib + (pos + 1) * BTRFS_ITEM_SZ, ib + pos * BTRFS_ITEM_SZ, (nr - pos) * BTRFS_ITEM_SZ);

    btrfs_item_t *new_item = (btrfs_item_t *)(ib + pos * BTRFS_ITEM_SZ);
    new_item->key = *key;
    new_item->data_offset = total_data;
    new_item->data_size = data_size;

    uint8_t *dst = leaf + fs->nodesize - total_data - data_size;
    memcpy(dst, data, data_size);
    hdr->nritems = nr + 1;
    return 0;
}

/* Remove item at position pos, compact data. */
static void btrfs_leaf_remove_at(btrfs_t *fs, uint8_t *leaf, uint32_t pos)
{
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)leaf;
    if (pos >= hdr->nritems) return;
    uint8_t *ib = leaf + BTRFS_NODE_HDR_SZ;
    btrfs_item_t *item = (btrfs_item_t *)(ib + pos * BTRFS_ITEM_SZ);
    uint32_t rem_off  = item->data_offset;
    uint32_t rem_size = item->data_size;
    uint8_t *data_end = leaf + fs->nodesize;

    /* Shift up data that's packed "below" the removed item */
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        if (i == pos) continue;
        btrfs_item_t *it = (btrfs_item_t *)(ib + i * BTRFS_ITEM_SZ);
        if (it->data_offset > rem_off) {
            uint8_t *src = data_end - it->data_offset - it->data_size;
            memmove(src + rem_size, src, it->data_size);
            it->data_offset -= rem_size;
        }
    }
    memset(data_end - rem_off - rem_size, 0, rem_size);

    if (pos + 1 < hdr->nritems)
        memmove(ib + pos * BTRFS_ITEM_SZ, ib + (pos + 1) * BTRFS_ITEM_SZ,
                (hdr->nritems - pos - 1) * BTRFS_ITEM_SZ);
    memset(ib + (hdr->nritems - 1) * BTRFS_ITEM_SZ, 0, BTRFS_ITEM_SZ);
    hdr->nritems--;
}

static int btrfs_load_fs_leaf(btrfs_t *fs, uint8_t *leaf)
{
    if (fs->fs_tree_logical == 0) {
        if (btrfs_find_root(fs, leaf, BTRFS_FS_TREE_OBJECTID, &fs->fs_tree_logical) != 0)
            return -1;
    }
    return btrfs_read_node(fs, fs->fs_tree_logical, leaf, fs->nodesize);
}

static int btrfs_write_fs_leaf(btrfs_t *fs, uint8_t *leaf)
{
    uint64_t phys;
    btrfs_logical_to_phys(fs, fs->fs_tree_logical, &phys);
    return blockdev_write_bytes(fs->bd, phys, fs->nodesize, leaf);
}

static void btrfs_split_path(const char *path, char *parent, char *name)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p;
    if (last == path) {
        parent[0] = '/'; parent[1] = 0;
        int i = 0;
        const char *n = (last[0] == '/' && last[1]) ? last + 1 : last;
        while (*n && i < BTRFS_MAX_FILENAME) name[i++] = *n++;
        name[i] = 0;
    } else {
        int plen = (int)(last - path);
        memcpy(parent, path, (size_t)plen);
        parent[plen] = 0;
        int i = 0;
        const char *n = last + 1;
        while (*n && i < BTRFS_MAX_FILENAME) name[i++] = *n++;
        name[i] = 0;
    }
}

/* Add INODE_ITEM + DIR_ITEM + DIR_INDEX for a new entry under dir_ino */
static int btrfs_add_dir_entry(btrfs_t *fs, uint8_t *leaf, uint64_t dir_ino,
                                uint64_t child_ino, const char *name, uint8_t ftype, uint8_t mode_hi)
{
    int namelen = 0;
    while (name[namelen]) namelen++;

    /* DIR_ITEM */
    uint32_t di_total = (uint32_t)sizeof(btrfs_dir_item_t) + (uint32_t)namelen;
    uint8_t *di_buf = (uint8_t *)malloc(di_total);
    if (!di_buf) return -1;
    memset(di_buf, 0, di_total);
    btrfs_dir_item_t *di = (btrfs_dir_item_t *)di_buf;
    di->location.objectid = child_ino;
    di->location.type = BTRFS_INODE_ITEM_KEY;
    di->location.offset = 0;
    di->name_len = (uint16_t)namelen;
    di->type = ftype;
    memcpy(di_buf + sizeof(btrfs_dir_item_t), name, (size_t)namelen);

    btrfs_key_t k;
    k.objectid = dir_ino;
    k.type = BTRFS_DIR_ITEM_KEY;
    k.offset = 0;
    if (btrfs_leaf_insert(fs, leaf, &k, di_buf, di_total) != 0) { free(di_buf); return -1; }

    k.type = BTRFS_DIR_INDEX_KEY;
    k.offset = child_ino;
    if (btrfs_leaf_insert(fs, leaf, &k, di_buf, di_total) != 0) { free(di_buf); return -1; }
    free(di_buf);

    /* INODE_ITEM */
    btrfs_inode_item_t ii;
    memset(&ii, 0, sizeof(ii));
    ii.nlink = 1;
    if (ftype == BTRFS_FT_DIR) {
        ii.mode = (uint32_t)0040000 | (mode_hi ? mode_hi : 0755);
        ii.nlink = 2;
    } else {
        ii.mode = (uint32_t)0100000 | (mode_hi ? mode_hi : 0644);
    }

    k.objectid = child_ino;
    k.type = BTRFS_INODE_ITEM_KEY;
    k.offset = 0;
    return btrfs_leaf_insert(fs, leaf, &k, &ii, sizeof(ii));
}

static int btrfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    if (size == 0) return 0;

    uint8_t *leaf = (uint8_t *)malloc(fs->nodesize);
    if (!leaf) return -1;
    if (btrfs_load_fs_leaf(fs, leaf) != 0) { free(leaf); return -1; }

    uint32_t pos = fs->fds[fd].pos;
    uint64_t ino = fs->fds[fd].ino;
    uint32_t new_end = pos + size;

    /* Max inline size: keep some headroom */
    uint32_t max_inline = (fs->nodesize / 4 < 4096) ? fs->nodesize / 4 : 4096;

    if (new_end <= max_inline) {
        int epos = btrfs_leaf_find_exact(leaf, ino, BTRFS_EXTENT_DATA_KEY, 0);
        uint32_t existing_inline = 0;
        uint8_t *existing_data = NULL;

        if (epos >= 0) {
            btrfs_item_t *it = (btrfs_item_t *)(leaf + BTRFS_NODE_HDR_SZ + (uint32_t)epos * BTRFS_ITEM_SZ);
            uint8_t *d = leaf + fs->nodesize - it->data_offset - it->data_size;
            btrfs_extent_data_t *ed = (btrfs_extent_data_t *)d;
            if (ed->type == 0)
                existing_inline = it->data_size - (uint32_t)sizeof(btrfs_extent_data_t);
            existing_data = d + sizeof(btrfs_extent_data_t);
        }

        uint32_t new_inline = (new_end > existing_inline) ? new_end : existing_inline;
        uint32_t new_total = (uint32_t)sizeof(btrfs_extent_data_t) + new_inline;
        uint8_t *ed_buf = (uint8_t *)malloc(new_total);
        if (!ed_buf) { free(leaf); return -1; }
        memset(ed_buf, 0, new_total);

        if (epos >= 0 && existing_data)
            memcpy(ed_buf + sizeof(btrfs_extent_data_t), existing_data, existing_inline);
        memcpy(ed_buf + sizeof(btrfs_extent_data_t) + pos, buf, size);

        btrfs_extent_data_t *ned = (btrfs_extent_data_t *)ed_buf;
        ned->ram_bytes = new_inline;
        ned->type = 0;

        if (epos >= 0)
            btrfs_leaf_remove_at(fs, leaf, (uint32_t)epos);

        btrfs_key_t k = { .objectid = ino, .type = BTRFS_EXTENT_DATA_KEY, .offset = 0 };
        int r = btrfs_leaf_insert(fs, leaf, &k, ed_buf, new_total);
        free(ed_buf);
        if (r != 0) { free(leaf); return -1; }
    } else {
        /* Regular extent: write data to disk and store a regular EXTENT_DATA */
        uint64_t data_logical = fs->next_data_logical;
        uint64_t data_phys;
        btrfs_logical_to_phys(fs, data_logical, &data_phys);
        blockdev_write_bytes(fs->bd, data_phys, size, buf);
        fs->next_data_logical = (data_logical + size + 511) & ~(uint64_t)511;

        int epos = btrfs_leaf_find_exact(leaf, ino, BTRFS_EXTENT_DATA_KEY, (uint64_t)pos);
        if (epos >= 0)
            btrfs_leaf_remove_at(fs, leaf, (uint32_t)epos);

        uint32_t reg_total = (uint32_t)(sizeof(btrfs_extent_data_t) + sizeof(btrfs_extent_reg_t));
        uint8_t *ed_buf = (uint8_t *)malloc(reg_total);
        if (!ed_buf) { free(leaf); return -1; }
        memset(ed_buf, 0, reg_total);
        btrfs_extent_data_t *ned = (btrfs_extent_data_t *)ed_buf;
        ned->ram_bytes = size;
        ned->type = 1;
        btrfs_extent_reg_t *reg = (btrfs_extent_reg_t *)(ed_buf + sizeof(btrfs_extent_data_t));
        reg->disk_bytenr = data_logical;
        reg->disk_num_bytes = size;
        reg->offset = 0;
        reg->num_bytes = size;

        btrfs_key_t k = { .objectid = ino, .type = BTRFS_EXTENT_DATA_KEY, .offset = (uint64_t)pos };
        int r = btrfs_leaf_insert(fs, leaf, &k, ed_buf, reg_total);
        free(ed_buf);
        if (r != 0) { free(leaf); return -1; }
    }

    /* Update INODE_ITEM size */
    int ipos = btrfs_leaf_find_exact(leaf, ino, BTRFS_INODE_ITEM_KEY, 0);
    if (ipos >= 0) {
        btrfs_item_t *it = (btrfs_item_t *)(leaf + BTRFS_NODE_HDR_SZ + (uint32_t)ipos * BTRFS_ITEM_SZ);
        uint8_t *d = leaf + fs->nodesize - it->data_offset - it->data_size;
        if (it->data_size >= sizeof(btrfs_inode_item_t)) {
            btrfs_inode_item_t *ii = (btrfs_inode_item_t *)d;
            if (new_end > (uint32_t)ii->size)
                ii->size = new_end;
        }
    }

    btrfs_write_fs_leaf(fs, leaf);
    free(leaf);

    fs->fds[fd].pos += size;
    if (fs->fds[fd].pos > fs->fds[fd].size)
        fs->fds[fd].size = fs->fds[fd].pos;
    return (int)size;
}

static int btrfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    btrfs_t *fs = (btrfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;
    return (int)fs->fds[fd].pos;
}

static int btrfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t dir_ino;
    int is_dir;
    if (btrfs_walk_path(fs, fs_tree, node_buf, path, &dir_ino, &is_dir) != 0) {
        free(node_buf);
        return -1;
    }
    if (!is_dir) { free(node_buf); return -1; }

    if (btrfs_read_tree_leaf(fs, fs_tree, node_buf, dir_ino, BTRFS_DIR_INDEX_KEY) != 0) {
        free(node_buf);
        return 0;
    }

    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
    uint32_t nritems = hdr->nritems;
    uint8_t *items_base = node_buf + BTRFS_NODE_HDR_SZ;
    uint8_t *data_end = node_buf + fs->nodesize;

    int count = 0;
    for (uint32_t i = 0; i < nritems && count < max; i++) {
        btrfs_item_t *item = (btrfs_item_t *)(items_base + i * BTRFS_ITEM_SZ);
        if (item->key.objectid != dir_ino) continue;
        if (item->key.type != BTRFS_DIR_INDEX_KEY) continue;

        uint8_t *data = data_end - item->data_offset - item->data_size;
        if (data < node_buf || item->data_size < sizeof(btrfs_dir_item_t)) continue;

        btrfs_dir_item_t *di = (btrfs_dir_item_t *)data;
        uint8_t *name_ptr = data + sizeof(btrfs_dir_item_t);
        if (name_ptr + di->name_len > data_end) continue;

        int nl = di->name_len;
        if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
        if (nl == 1 && name_ptr[0] == '.') continue;
        if (nl == 2 && name_ptr[0] == '.' && name_ptr[1] == '.') continue;

        memcpy(entries[count].name, name_ptr, (size_t)nl);
        entries[count].name[nl] = 0;
        entries[count].is_dir   = (di->type == BTRFS_FT_DIR);
        entries[count].inode    = (uint32_t)di->location.objectid;
        entries[count].size     = 0;
        entries[count].mode     = 0;

        uint64_t child_size = 0;
        uint32_t child_mode = 0;
        uint8_t *tmp_buf = (uint8_t *)malloc(fs->nodesize);
        if (tmp_buf) {
            btrfs_get_inode_size(fs, fs_tree, tmp_buf, di->location.objectid, &child_size, &child_mode);
            free(tmp_buf);
        }
        entries[count].size = (uint32_t)child_size;
        entries[count].mode = child_mode;
        count++;
    }

    free(node_buf);
    return count;
}

static int btrfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t dummy_ino;
    int dummy_is_dir;
    if (btrfs_walk_path(fs, fs_tree, node_buf, path, &dummy_ino, &dummy_is_dir) == 0) {
        free(node_buf); return -1; /* already exists */
    }

    char parent[256], name[BTRFS_MAX_FILENAME + 1];
    btrfs_split_path(path, parent, name);
    if (!name[0]) { free(node_buf); return -1; }

    uint64_t dir_ino;
    int dir_is_dir = 0;
    if (btrfs_walk_path(fs, fs_tree, node_buf, parent, &dir_ino, &dir_is_dir) != 0 || !dir_is_dir) {
        free(node_buf); return -1;
    }

    uint64_t new_ino = fs->next_objectid++;
    if (btrfs_load_fs_leaf(fs, node_buf) != 0) { free(node_buf); return -1; }
    if (btrfs_add_dir_entry(fs, node_buf, dir_ino, new_ino, name, BTRFS_FT_DIR, (uint8_t)(mode & 0777)) != 0) {
        free(node_buf); return -1;
    }
    btrfs_write_fs_leaf(fs, node_buf);
    free(node_buf);
    return 0;
}

static int btrfs_vfs_unlink(void *ctx, const char *path)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t ino;
    int is_dir = 0;
    if (btrfs_walk_path(fs, fs_tree, node_buf, path, &ino, &is_dir) != 0) {
        free(node_buf); return -1;
    }

    char parent[256], name[BTRFS_MAX_FILENAME + 1];
    btrfs_split_path(path, parent, name);

    uint64_t dir_ino;
    int dir_is_dir = 0;
    btrfs_walk_path(fs, fs_tree, node_buf, parent, &dir_ino, &dir_is_dir);

    if (btrfs_load_fs_leaf(fs, node_buf) != 0) { free(node_buf); return -1; }

    /* Remove DIR_ITEM, DIR_INDEX, INODE_ITEM, EXTENT_DATA */
    int p;
    while ((p = btrfs_leaf_find_exact(node_buf, dir_ino, BTRFS_DIR_ITEM_KEY, 0)) >= 0)
        btrfs_leaf_remove_at(fs, node_buf, (uint32_t)p);
    while ((p = btrfs_leaf_find_exact(node_buf, dir_ino, BTRFS_DIR_INDEX_KEY, ino)) >= 0)
        btrfs_leaf_remove_at(fs, node_buf, (uint32_t)p);
    while ((p = btrfs_leaf_find_exact(node_buf, ino, BTRFS_INODE_ITEM_KEY, 0)) >= 0)
        btrfs_leaf_remove_at(fs, node_buf, (uint32_t)p);
    /* Remove any extent data */
    btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node_buf;
    for (uint32_t i = 0; i < hdr->nritems; ) {
        btrfs_item_t *it = (btrfs_item_t *)(node_buf + BTRFS_NODE_HDR_SZ + i * BTRFS_ITEM_SZ);
        if (it->key.objectid == ino && it->key.type == BTRFS_EXTENT_DATA_KEY) {
            btrfs_leaf_remove_at(fs, node_buf, i);
        } else {
            i++;
        }
    }

    btrfs_write_fs_leaf(fs, node_buf);
    free(node_buf);
    return 0;
}

static int btrfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t ino;
    int is_dir = 0;
    if (btrfs_walk_path(fs, fs_tree, node_buf, path, &ino, &is_dir) != 0) {
        free(node_buf);
        return -1;
    }

    uint64_t size = 0;
    uint32_t mode = 0;
    btrfs_get_inode_size(fs, fs_tree, node_buf, ino, &size, &mode);
    free(node_buf);

    const char *base = path;
    const char *p = path;
    while (*p) { if (*p == '/') base = p + 1; p++; }
    int nl = 0;
    while (base[nl] && nl < VFS_NAME_LEN - 1) { entry->name[nl] = base[nl]; nl++; }
    entry->name[nl] = 0;
    entry->size   = (uint32_t)size;
    entry->is_dir = is_dir;
    entry->inode  = (uint32_t)ino;
    entry->mode   = mode;
    return 0;
}

static int btrfs_vfs_rename(void *ctx, const char *old, const char *new)
{
    btrfs_t *fs = (btrfs_t *)ctx;

    uint8_t *node_buf = (uint8_t *)malloc(fs->nodesize);
    if (!node_buf) return -1;

    uint64_t fs_tree;
    if (btrfs_get_fs_tree(fs, node_buf, &fs_tree) != 0) { free(node_buf); return -1; }

    uint64_t ino;
    int is_dir = 0;
    if (btrfs_walk_path(fs, fs_tree, node_buf, old, &ino, &is_dir) != 0) {
        free(node_buf); return -1;
    }

    char old_parent[256], old_name[BTRFS_MAX_FILENAME + 1];
    char new_parent[256], new_name[BTRFS_MAX_FILENAME + 1];
    btrfs_split_path(old, old_parent, old_name);
    btrfs_split_path(new, new_parent, new_name);

    uint64_t old_dir_ino, new_dir_ino;
    int tmp_is_dir = 0;
    if (btrfs_walk_path(fs, fs_tree, node_buf, old_parent, &old_dir_ino, &tmp_is_dir) != 0) {
        free(node_buf); return -1;
    }
    if (btrfs_walk_path(fs, fs_tree, node_buf, new_parent, &new_dir_ino, &tmp_is_dir) != 0) {
        free(node_buf); return -1;
    }

    if (btrfs_load_fs_leaf(fs, node_buf) != 0) { free(node_buf); return -1; }

    /* Remove old dir entries */
    int p;
    while ((p = btrfs_leaf_find_exact(node_buf, old_dir_ino, BTRFS_DIR_ITEM_KEY, 0)) >= 0)
        btrfs_leaf_remove_at(fs, node_buf, (uint32_t)p);
    while ((p = btrfs_leaf_find_exact(node_buf, old_dir_ino, BTRFS_DIR_INDEX_KEY, ino)) >= 0)
        btrfs_leaf_remove_at(fs, node_buf, (uint32_t)p);

    /* Add new dir entries */
    uint8_t ftype = is_dir ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE;
    uint32_t di_total = (uint32_t)sizeof(btrfs_dir_item_t) + (uint32_t)strlen(new_name);
    uint8_t *di_buf = (uint8_t *)malloc(di_total);
    if (di_buf) {
        memset(di_buf, 0, di_total);
        btrfs_dir_item_t *di = (btrfs_dir_item_t *)di_buf;
        di->location.objectid = ino;
        di->location.type = BTRFS_INODE_ITEM_KEY;
        di->name_len = (uint16_t)strlen(new_name);
        di->type = ftype;
        memcpy(di_buf + sizeof(btrfs_dir_item_t), new_name, strlen(new_name));

        btrfs_key_t k;
        k.objectid = new_dir_ino;
        k.type = BTRFS_DIR_ITEM_KEY;
        k.offset = 0;
        btrfs_leaf_insert(fs, node_buf, &k, di_buf, di_total);
        k.type = BTRFS_DIR_INDEX_KEY;
        k.offset = ino;
        btrfs_leaf_insert(fs, node_buf, &k, di_buf, di_total);
        free(di_buf);
    }

    btrfs_write_fs_leaf(fs, node_buf);
    free(node_buf);
    return 0;
}

static int btrfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void btrfs_mount_vfs(btrfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ops = {
        .open    = btrfs_vfs_open,
        .close   = btrfs_vfs_close,
        .read    = btrfs_vfs_read,
        .write   = btrfs_vfs_write,
        .lseek   = btrfs_vfs_lseek,
        .readdir = btrfs_vfs_readdir,
        .mkdir   = btrfs_vfs_mkdir,
        .unlink  = btrfs_vfs_unlink,
        .stat    = btrfs_vfs_stat,
        .rename  = btrfs_vfs_rename,
        .symlink = btrfs_vfs_symlink,
    };
    vfs_mount(mount_point, &ops, fs);
}

int btrfs_format(blockdev_t *bd, const char *label)
{
    uint32_t nodesize   = 16384;
    uint32_t sectorsize = 4096;

    uint8_t *zero = (uint8_t *)malloc(nodesize);
    if (!zero) return -1;
    memset(zero, 0, nodesize);

    uint64_t sector_size = bd->sector_size ? bd->sector_size : 512;
    uint64_t total_bytes = bd->total_sectors * sector_size;
    (void)total_bytes;

    btrfs_super_t *sb = (btrfs_super_t *)malloc(sizeof(btrfs_super_t));
    if (!sb) { free(zero); return -1; }
    memset(sb, 0, sizeof(btrfs_super_t));

    sb->magic           = BTRFS_MAGIC;
    sb->bytenr          = BTRFS_SUPER_OFFSET;
    sb->generation      = 1;
    sb->sectorsize      = sectorsize;
    sb->nodesize        = nodesize;
    sb->leafsize        = nodesize;
    sb->stripesize      = sectorsize;
    sb->total_bytes     = total_bytes;
    sb->bytes_used      = nodesize * 3;
    sb->root_dir_objectid = BTRFS_ROOT_DIR_OBJECTID;
    sb->num_devices     = 1;

    uint64_t chunk_phys    = 1 * 1024 * 1024ULL;
    uint64_t chunk_logical = 1 * 1024 * 1024ULL;
    uint64_t chunk_length  = 256 * 1024 * 1024ULL;

    uint8_t *arr = sb->sys_chunk_array;
    uint32_t arr_pos = 0;

    btrfs_key_t ckey;
    ckey.objectid = BTRFS_CHUNK_TREE_OBJECTID;
    ckey.type     = BTRFS_CHUNK_ITEM_KEY;
    ckey.offset   = chunk_logical;
    memcpy(arr + arr_pos, &ckey, sizeof(btrfs_key_t));
    arr_pos += sizeof(btrfs_key_t);

    btrfs_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.length      = chunk_length;
    chunk.owner       = BTRFS_CHUNK_TREE_OBJECTID;
    chunk.stripe_len  = 65536;
    chunk.num_stripes = 1;
    chunk.sub_stripes = 1;
    chunk.sector_size = sectorsize;
    memcpy(arr + arr_pos, &chunk, sizeof(chunk));
    arr_pos += sizeof(chunk);

    btrfs_stripe_t stripe;
    memset(&stripe, 0, sizeof(stripe));
    stripe.devid  = 1;
    stripe.offset = chunk_phys;
    memcpy(arr + arr_pos, &stripe, sizeof(stripe));
    arr_pos += sizeof(stripe);

    sb->sys_chunk_array_size = arr_pos;

    uint64_t root_tree_phys  = chunk_phys;
    uint64_t chunk_tree_phys = chunk_phys + nodesize;
    uint64_t fs_tree_phys    = chunk_phys + nodesize * 2;

    sb->root       = chunk_logical;
    sb->chunk_root = chunk_logical + nodesize;

    if (label) {
        int k = 0;
        while (label[k] && k < 255) { sb->label[k] = label[k]; k++; }
        sb->label[k] = 0;
    }

    uint8_t *node = (uint8_t *)malloc(nodesize);
    if (!node) { free(sb); free(zero); return -1; }

    /* Write root tree node with a single ROOT_ITEM pointing to the fs tree */
    memset(node, 0, nodesize);
    {
        btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node;
        hdr->bytenr      = chunk_logical;
        hdr->generation  = 1;
        hdr->owner       = BTRFS_FS_TREE_OBJECTID;
        hdr->level       = 0;

        /* One item: ROOT_ITEM for FS_TREE_OBJECTID, data = logical addr of fs tree */
        uint64_t fs_tree_logical_val = chunk_logical + nodesize * 2;
        btrfs_item_t *it = (btrfs_item_t *)(node + BTRFS_NODE_HDR_SZ);
        it->key.objectid = BTRFS_FS_TREE_OBJECTID;
        it->key.type     = BTRFS_ROOT_ITEM_KEY;
        it->key.offset   = 0;
        it->data_offset  = 0;
        it->data_size    = 8;
        /* Data at end of node: 8 bytes = fs_tree logical */
        memcpy(node + nodesize - 8, &fs_tree_logical_val, 8);
        hdr->nritems = 1;
    }
    blockdev_write_bytes(bd, root_tree_phys, nodesize, node);

    /* Write empty chunk tree node */
    memset(node, 0, nodesize);
    {
        btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node;
        hdr->bytenr      = chunk_logical + nodesize;
        hdr->generation  = 1;
        hdr->owner       = BTRFS_CHUNK_TREE_OBJECTID;
        hdr->nritems     = 0;
        hdr->level       = 0;
    }
    blockdev_write_bytes(bd, chunk_tree_phys, nodesize, node);

    /* Write empty fs tree leaf */
    memset(node, 0, nodesize);
    {
        btrfs_node_hdr_t *hdr = (btrfs_node_hdr_t *)node;
        hdr->bytenr      = chunk_logical + nodesize * 2;
        hdr->generation  = 1;
        hdr->owner       = BTRFS_FS_TREE_OBJECTID;
        hdr->nritems     = 0;
        hdr->level       = 0;
    }
    blockdev_write_bytes(bd, fs_tree_phys, nodesize, node);
    free(node);

    int ret = blockdev_write_bytes(bd, BTRFS_SUPER_OFFSET, sizeof(btrfs_super_t), sb);
    free(sb);
    free(zero);
    return ret;
}
