#include "apfs.h"
#include "memory.h"
#include "string.h"

#define APFS_NX_MAGIC    0x4253584EU  /* 'NXSB' little-endian */
#define APFS_VOL_MAGIC   0x42535041U  /* 'APSB' little-endian */

#define APFS_OBJ_HDR_SIZE 32

#define APFS_OBJECT_TYPE_NX_SUPERBLOCK  0x0001U
#define APFS_OBJECT_TYPE_BTREE          0x0002U
#define APFS_OBJECT_TYPE_BTREE_NODE     0x0003U
#define APFS_OBJECT_TYPE_SPACEMAN       0x0005U
#define APFS_OBJECT_TYPE_OMAP           0x000BU
#define APFS_OBJECT_TYPE_FS             0x000DU
#define APFS_OBJECT_TYPE_INODE          0x0009U

#define APFS_INODE_DIR_TS_LATEST  1

#define APFS_DREC_TYPE_DIR   4
#define APFS_DREC_TYPE_REG   8
#define APFS_DREC_TYPE_LINK  10

#define APFS_KEY_TYPE_INODE   0x3
#define APFS_KEY_TYPE_DREC    0x9
#define APFS_KEY_TYPE_DSTREAM 0xA

typedef struct {
    uint64_t o_cksum;
    uint64_t o_oid;
    uint64_t o_xid;
    uint16_t o_type;
    uint16_t o_flags;
    uint16_t o_subtype;
    uint16_t o_pad;
} __attribute__((packed)) apfs_obj_hdr_t;

typedef struct {
    apfs_obj_hdr_t hdr;
    uint32_t nx_magic;
    uint32_t nx_block_size;
    uint64_t nx_block_count;
    uint64_t nx_features;
    uint64_t nx_readonly_compatible_features;
    uint64_t nx_incompatible_features;
    uint8_t  nx_uuid[16];
    uint64_t nx_next_oid;
    uint64_t nx_next_xid;
    uint32_t nx_xp_desc_blocks;
    uint32_t nx_xp_data_blocks;
    uint64_t nx_xp_desc_base;
    uint64_t nx_xp_data_base;
    uint32_t nx_xp_desc_next;
    uint32_t nx_xp_data_next;
    uint32_t nx_xp_desc_index;
    uint32_t nx_xp_desc_len;
    uint32_t nx_xp_data_index;
    uint32_t nx_xp_data_len;
    uint64_t nx_spaceman_oid;
    uint64_t nx_omap_oid;
    uint64_t nx_reaper_oid;
    uint32_t nx_test_type;
    uint32_t nx_max_file_systems;
    uint64_t nx_fs_oid[100];
} __attribute__((packed)) apfs_nx_sb_t;

typedef struct {
    apfs_obj_hdr_t hdr;
    uint32_t apfs_magic;
    uint32_t apfs_fs_index;
    uint64_t apfs_features;
    uint64_t apfs_readonly_compatible_features;
    uint64_t apfs_incompatible_features;
    uint64_t apfs_unmount_time;
    uint64_t apfs_fs_reserve_block_count;
    uint64_t apfs_quota_block_count;
    uint64_t apfs_fs_alloc_count;
    uint64_t apfs_meta_crypto[5];
    uint32_t apfs_root_tree_type;
    uint32_t apfs_extentref_tree_type;
    uint32_t apfs_snap_meta_tree_type;
    uint32_t pad0;
    uint64_t apfs_omap_oid;
    uint64_t apfs_root_tree_oid;
    uint64_t apfs_extentref_tree_oid;
    uint64_t apfs_snap_meta_tree_oid;
    uint64_t apfs_revert_to_xid;
    uint64_t apfs_revert_to_sblock_oid;
    uint64_t apfs_next_obj_id;
    uint64_t apfs_num_files;
    uint64_t apfs_num_directories;
    uint64_t apfs_num_symlinks;
    uint64_t apfs_num_other_fsobjects;
    uint64_t apfs_num_snapshots;
    uint64_t apfs_total_blocks_alloced;
    uint64_t apfs_total_blocks_freed;
    uint8_t  apfs_vol_uuid[16];
    uint64_t apfs_last_mod_time;
    uint64_t apfs_fs_flags;
    uint8_t  apfs_formatted_by[32];
    uint8_t  apfs_modified_by[8][32];
    char     apfs_volname[256];
} __attribute__((packed)) apfs_vol_sb_t;

typedef struct {
    apfs_obj_hdr_t hdr;
    uint32_t btn_flags;
    uint16_t btn_level;
    uint16_t btn_key_count;
    uint32_t btn_table_space_off;
    uint32_t btn_table_space_len;
    uint32_t btn_free_space_off;
    uint32_t btn_free_space_len;
    uint32_t btn_key_free_list_off;
    uint32_t btn_key_free_list_len;
    uint32_t btn_val_free_list_off;
    uint32_t btn_val_free_list_len;
} __attribute__((packed)) apfs_btnode_t;

typedef struct {
    uint16_t key_off;
    uint16_t val_off;
} __attribute__((packed)) apfs_kvoff_t;

typedef struct {
    uint64_t oid;
    uint64_t xid;
} __attribute__((packed)) apfs_omap_key_t;

typedef struct {
    uint32_t flags;
    uint32_t size;
    uint64_t paddr;
} __attribute__((packed)) apfs_omap_val_t;

typedef struct {
    uint64_t obj_id_and_type;
} __attribute__((packed)) apfs_key_hdr_t;

typedef struct {
    uint64_t parent_id;
    uint64_t private_id;
    uint64_t create_time;
    uint64_t mod_time;
    uint64_t change_time;
    uint64_t access_time;
    uint64_t internal_flags;
    uint32_t nchildren_or_nlink;
    uint32_t default_protection_class;
    uint32_t write_generation_counter;
    uint32_t bsd_flags;
    uint32_t owner;
    uint32_t group;
    uint16_t mode;
    uint16_t pad1;
    uint64_t pad2;
} __attribute__((packed)) apfs_inode_val_t;

typedef struct {
    uint32_t name_len_and_hash;
} __attribute__((packed)) apfs_drec_key_ext_t;

typedef struct {
    uint64_t file_id;
    uint64_t date_added;
    uint16_t flags;
} __attribute__((packed)) apfs_drec_val_t;

#define APFS_BTN_FLAG_ROOT   1
#define APFS_BTN_FLAG_LEAF   2
#define APFS_BTN_FLAG_FIXED  4

static int apfs_read_block(apfs_t *fs, uint64_t block, void *buf)
{
    return blockdev_read_bytes(fs->bd, block * fs->block_size, fs->block_size, buf);
}

/* Resolve a virtual OID to a physical block using the omap b-tree */
static int apfs_omap_lookup(apfs_t *fs, uint8_t *omap_root_buf,
                              uint64_t omap_root_block, uint64_t oid, uint64_t *pblock)
{
    uint8_t *node_buf = omap_root_buf;
    uint64_t cur_block = omap_root_block;

    int max_levels = 8;
    while (max_levels-- > 0) {
        if (apfs_read_block(fs, cur_block, node_buf) != 0) return -1;

        apfs_btnode_t *btn = (apfs_btnode_t *)node_buf;
        uint16_t key_count = btn->btn_key_count;
        uint32_t flags = btn->btn_flags;
        int is_leaf = (flags & APFS_BTN_FLAG_LEAF) != 0;

        /* TOC: array of kvoff_t at node_buf + sizeof(apfs_btnode_t) */
        uint8_t *toc_base = node_buf + sizeof(apfs_btnode_t);
        /* Key area starts after TOC */
        uint8_t *key_base = toc_base + key_count * sizeof(apfs_kvoff_t);
        /* Val area counts from end of block */
        uint8_t *val_end  = node_buf + fs->block_size;
        /* For non-root nodes with fixed vals there may be a footer */

        uint64_t best_oid = 0;
        uint64_t best_child = 0;
        int best_found = 0;

        for (uint16_t i = 0; i < key_count; i++) {
            apfs_kvoff_t *kv = (apfs_kvoff_t *)(toc_base + i * sizeof(apfs_kvoff_t));
            uint8_t *key_ptr = key_base + kv->key_off;
            if (key_ptr + sizeof(apfs_omap_key_t) > val_end) continue;
            apfs_omap_key_t *ok = (apfs_omap_key_t *)key_ptr;

            if (ok->oid > oid) break;

            if (is_leaf) {
                uint8_t *val_ptr = val_end - (uint32_t)kv->val_off - sizeof(apfs_omap_val_t);
                if (val_ptr < node_buf) continue;
                apfs_omap_val_t *ov = (apfs_omap_val_t *)val_ptr;
                if (ok->oid == oid) {
                    *pblock = ov->paddr;
                    return 0;
                }
            } else {
                uint8_t *val_ptr = val_end - (uint32_t)kv->val_off - sizeof(uint64_t);
                if (val_ptr < node_buf) continue;
                uint64_t child_block;
                memcpy(&child_block, val_ptr, sizeof(uint64_t));
                if (ok->oid <= oid) {
                    best_oid   = ok->oid;
                    best_child = child_block;
                    best_found = 1;
                }
            }
        }

        if (is_leaf) return -1;
        if (!best_found) return -1;
        (void)best_oid;
        cur_block = best_child;
    }
    return -1;
}

/* Look up an entry in the fs object map (b-tree rooted at a given block) */
/* key = (type<<60)|objectid, returns pointer to value within node_buf */
static uint8_t *apfs_fs_btree_lookup(apfs_t *fs, uint8_t *node_buf,
                                      uint64_t root_block, uint64_t key64,
                                      uint32_t *val_size_out)
{
    uint64_t cur_block = root_block;
    int max_levels = 8;

    while (max_levels-- > 0) {
        if (apfs_read_block(fs, cur_block, node_buf) != 0) return 0;
        apfs_btnode_t *btn = (apfs_btnode_t *)node_buf;
        uint16_t key_count = btn->btn_key_count;
        uint32_t btn_flags = btn->btn_flags;
        int is_leaf = (btn_flags & APFS_BTN_FLAG_LEAF) != 0;

        uint8_t *toc_base  = node_buf + sizeof(apfs_btnode_t);
        uint8_t *key_base  = toc_base + key_count * sizeof(apfs_kvoff_t);
        uint8_t *val_end   = node_buf + fs->block_size;

        /* For fixed-size val leaf nodes, skip the last 40 bytes (btree info) */
        /* For variable val nodes the values sit just before val_end */
        if (btn_flags & APFS_BTN_FLAG_ROOT) val_end -= 40;

        uint64_t best_key = 0;
        uint8_t *best_val = 0;
        uint32_t best_vsz = 0;
        uint64_t best_child = 0;
        int best_found = 0;

        for (uint16_t i = 0; i < key_count; i++) {
            apfs_kvoff_t *kv = (apfs_kvoff_t *)(toc_base + i * sizeof(apfs_kvoff_t));
            uint8_t *key_ptr = key_base + kv->key_off;
            if (key_ptr + sizeof(uint64_t) > val_end) continue;

            uint64_t kval;
            memcpy(&kval, key_ptr, sizeof(uint64_t));

            if (kval > key64) break;

            if (is_leaf) {
                if (kval == key64) {
                    uint8_t *vp;
                    uint32_t vsz;
                    if (btn_flags & APFS_BTN_FLAG_FIXED) {
                        vsz = sizeof(uint64_t);
                        vp  = val_end - (uint32_t)kv->val_off - vsz;
                    } else {
                        /* Variable: val_off is offset from val_end backward, but
                           we don't know the size. Use remaining space heuristic. */
                        if (i + 1 < key_count) {
                            apfs_kvoff_t *next_kv = (apfs_kvoff_t *)(toc_base + (i+1)*sizeof(apfs_kvoff_t));
                            vsz = (uint32_t)next_kv->val_off - (uint32_t)kv->val_off;
                        } else {
                            vsz = 256;
                        }
                        vp = val_end - (uint32_t)kv->val_off - vsz;
                    }
                    if (vp < node_buf) continue;
                    best_key   = kval;
                    best_val   = vp;
                    best_vsz   = vsz;
                    best_found = 1;
                    break;
                }
            } else {
                /* Internal: value is a child OID (8 bytes) */
                uint8_t *vp = val_end - (uint32_t)kv->val_off - sizeof(uint64_t);
                if (vp < node_buf) continue;
                uint64_t child_oid;
                memcpy(&child_oid, vp, sizeof(uint64_t));
                if (kval <= key64) {
                    best_child = child_oid;
                    best_found = 1;
                }
            }
        }

        if (is_leaf) {
            if (!best_found) return 0;
            (void)best_key;
            if (val_size_out) *val_size_out = best_vsz;
            return best_val;
        }
        if (!best_found) return 0;
        cur_block = best_child;
    }
    return 0;
}

int apfs_probe_and_mount(apfs_t *fs, blockdev_t *bd)
{
    memset(fs, 0, sizeof(apfs_t));
    fs->bd         = bd;
    fs->block_size = 4096;

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return -1;

    if (blockdev_read_bytes(bd, 0, 4096, buf) != 0) { free(buf); return -1; }

    apfs_nx_sb_t *nx = (apfs_nx_sb_t *)buf;
    if (nx->nx_magic != APFS_NX_MAGIC) { free(buf); return -1; }

    fs->block_size  = nx->nx_block_size ? nx->nx_block_size : 4096;
    fs->block_count = nx->nx_block_count;
    fs->omap_oid    = nx->nx_omap_oid;
    fs->fs_oid      = nx->nx_fs_oid[0];
    fs->mounted     = 1;

    free(buf);
    return 0;
}

int apfs_umount(apfs_t *fs)
{
    fs->mounted = 0;
    return 0;
}

/* Resolve an omap OID to a physical block. omap_tree_block = physical block of omap btree root. */
static int apfs_resolve_oid(apfs_t *fs, uint64_t omap_tree_block, uint64_t oid, uint64_t *pblock)
{
    uint8_t *node_buf = (uint8_t *)malloc(fs->block_size);
    if (!node_buf) return -1;
    int ret = apfs_omap_lookup(fs, node_buf, omap_tree_block, oid, pblock);
    free(node_buf);
    return ret;
}

/* Get the physical block of the container omap b-tree root */
static int apfs_get_omap_tree_block(apfs_t *fs, uint64_t *out)
{
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    /* omap_oid is a physical OID — read it as a block */
    if (apfs_read_block(fs, fs->omap_oid, buf) != 0) { free(buf); return -1; }

    /* Object at omap_oid is an omap object; its root btree OID is at offset 48 */
    uint64_t root_oid;
    memcpy(&root_oid, buf + APFS_OBJ_HDR_SIZE + 16, sizeof(uint64_t));
    free(buf);

    /* The omap root btree OID is physical for container omap */
    *out = root_oid;
    return 0;
}

/* Get physical block of volume superblock given its virtual OID */
static int apfs_get_vol_sb_block(apfs_t *fs, uint64_t vol_oid, uint64_t *out_block)
{
    uint64_t omap_tree;
    if (apfs_get_omap_tree_block(fs, &omap_tree) != 0) return -1;
    return apfs_resolve_oid(fs, omap_tree, vol_oid, out_block);
}

/* Get the fs btree root block from the volume superblock */
static int apfs_get_fstree_block(apfs_t *fs, uint64_t vol_block,
                                  uint64_t *omap_tree_out, uint64_t *fstree_block_out)
{
    uint8_t *buf = (uint8_t *)malloc(fs->block_size);
    if (!buf) return -1;

    if (apfs_read_block(fs, vol_block, buf) != 0) { free(buf); return -1; }

    apfs_vol_sb_t *vsb = (apfs_vol_sb_t *)buf;
    if (vsb->apfs_magic != APFS_VOL_MAGIC) { free(buf); return -1; }

    uint64_t vol_omap_oid = vsb->apfs_omap_oid;
    uint64_t root_tree_oid = vsb->apfs_root_tree_oid;
    free(buf);

    /* Resolve volume omap */
    uint8_t *omap_buf = (uint8_t *)malloc(fs->block_size);
    if (!omap_buf) return -1;
    if (apfs_read_block(fs, vol_omap_oid, omap_buf) != 0) { free(omap_buf); return -1; }
    uint64_t vol_omap_tree;
    memcpy(&vol_omap_tree, omap_buf + APFS_OBJ_HDR_SIZE + 16, sizeof(uint64_t));
    free(omap_buf);

    *omap_tree_out = vol_omap_tree;

    uint64_t fstree_block;
    if (apfs_resolve_oid(fs, vol_omap_tree, root_tree_oid, &fstree_block) != 0) return -1;
    *fstree_block_out = fstree_block;
    return 0;
}

static int apfs_walk_path(apfs_t *fs, uint64_t fstree_block, uint64_t omap_tree,
                            const char *path, uint64_t *out_ino, int *out_is_dir)
{
    (void)omap_tree;
    uint64_t cur_ino = 2;
    int is_dir = 1;
    const char *p = path;

    uint8_t *node_buf = (uint8_t *)malloc(fs->block_size);
    if (!node_buf) return -1;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[APFS_MAX_FILENAME + 1];
        int i = 0;
        while (*p && *p != '/' && i < APFS_MAX_FILENAME) comp[i++] = *p++;
        comp[i] = 0;
        if (i == 0) continue;
        if (!is_dir) { free(node_buf); return -1; }

        /* Build drec key: type=9 (DREC), object_id = cur_ino */
        uint64_t key64 = ((uint64_t)APFS_KEY_TYPE_DREC << 60) | (cur_ino & 0x0FFFFFFFFFFFFFFFULL);

        uint32_t vsz = 0;
        uint8_t *val = apfs_fs_btree_lookup(fs, node_buf, fstree_block, key64, &vsz);
        if (!val) { free(node_buf); return -1; }

        /* val points into node_buf; we need to scan directory entries */
        /* The key for drec includes the name; we need to iterate nearby keys */
        /* Simplified: re-read the leaf and scan all drec entries for cur_ino */

        if (apfs_read_block(fs, fstree_block, node_buf) != 0) { free(node_buf); return -1; }
        apfs_btnode_t *btn = (apfs_btnode_t *)node_buf;
        uint16_t key_count = btn->btn_key_count;
        uint8_t *toc_base  = node_buf + sizeof(apfs_btnode_t);
        uint8_t *key_base  = toc_base + key_count * sizeof(apfs_kvoff_t);
        uint8_t *val_end   = node_buf + fs->block_size;

        int found = 0;
        for (uint16_t ki = 0; ki < key_count && !found; ki++) {
            apfs_kvoff_t *kv = (apfs_kvoff_t *)(toc_base + ki * sizeof(apfs_kvoff_t));
            uint8_t *key_ptr = key_base + kv->key_off;
            if (key_ptr + 8 > val_end) continue;

            uint64_t raw_key;
            memcpy(&raw_key, key_ptr, 8);
            uint64_t koid = raw_key & 0x0FFFFFFFFFFFFFFFULL;
            uint32_t ktype = (uint32_t)(raw_key >> 60) & 0xFU;

            if (koid != cur_ino) continue;
            if (ktype != APFS_KEY_TYPE_DREC) continue;

            /* name follows the 8-byte key header: 4 bytes (name_len_and_hash) then name */
            uint8_t *name_ext = key_ptr + 8;
            if (name_ext + 4 > val_end) continue;
            uint32_t name_lh;
            memcpy(&name_lh, name_ext, 4);
            uint16_t name_len = (uint16_t)(name_lh & 0x3FFU);
            uint8_t *name_ptr2 = name_ext + 4;
            if (name_ptr2 + name_len > val_end) continue;

            if (name_len != (uint16_t)i) continue;
            if (memcmp(name_ptr2, comp, (size_t)i) != 0) continue;

            /* Found matching drec */
            apfs_drec_val_t *dv = (apfs_drec_val_t *)(val_end - kv->val_off - sizeof(apfs_drec_val_t));
            if ((uint8_t *)dv < node_buf) continue;
            cur_ino = dv->file_id;
            uint16_t dtype = (dv->flags >> 4) & 0xFU;
            is_dir = (dtype == APFS_DREC_TYPE_DIR);
            found = 1;
        }

        if (!found) { free(node_buf); return -1; }
    }

    free(node_buf);
    *out_ino = cur_ino;
    if (out_is_dir) *out_is_dir = is_dir;
    return 0;
}

static int apfs_get_inode(apfs_t *fs, uint64_t fstree_block, uint64_t ino,
                           apfs_inode_val_t *out_ival, uint64_t *out_size)
{
    uint64_t key64 = ((uint64_t)APFS_KEY_TYPE_INODE << 60) | (ino & 0x0FFFFFFFFFFFFFFFULL);
    uint8_t *node_buf = (uint8_t *)malloc(fs->block_size);
    if (!node_buf) return -1;

    uint32_t vsz = 0;
    uint8_t *val = apfs_fs_btree_lookup(fs, node_buf, fstree_block, key64, &vsz);
    if (!val || vsz < sizeof(apfs_inode_val_t)) { free(node_buf); return -1; }

    memcpy(out_ival, val, sizeof(apfs_inode_val_t));

    /* Try to get data size from dstream key */
    if (out_size) {
        *out_size = 0;
        uint64_t ds_key = ((uint64_t)APFS_KEY_TYPE_DSTREAM << 60) | (ino & 0x0FFFFFFFFFFFFFFFULL);
        uint32_t dsz = 0;
        uint8_t *ds_val = apfs_fs_btree_lookup(fs, node_buf, fstree_block, ds_key, &dsz);
        if (ds_val && dsz >= 8) {
            uint64_t sz;
            memcpy(&sz, ds_val, 8);
            *out_size = sz;
        }
    }

    free(node_buf);
    return 0;
}

static int apfs_vfs_open(void *ctx, const char *path, int flags)
{
    apfs_t *fs = (apfs_t *)ctx;
    (void)flags;
    if (!fs->mounted) return -1;

    uint64_t vol_block;
    if (apfs_get_vol_sb_block(fs, fs->fs_oid, &vol_block) != 0) return -1;

    uint64_t omap_tree, fstree_block;
    if (apfs_get_fstree_block(fs, vol_block, &omap_tree, &fstree_block) != 0) return -1;

    uint64_t ino;
    int is_dir = 0;
    if (apfs_walk_path(fs, fstree_block, omap_tree, path, &ino, &is_dir) != 0) return -1;
    if (is_dir) return -1;

    apfs_inode_val_t ival;
    uint64_t file_size = 0;
    apfs_get_inode(fs, fstree_block, ino, &ival, &file_size);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used   = 1;
            fs->fds[i].ino    = ino;
            fs->fds[i].pos    = 0;
            fs->fds[i].size   = (uint32_t)file_size;
            fs->fds[i].is_dir = 0;
            fs->fds[i].dirty  = 0;
            return i;
        }
    }
    return -1;
}

static int apfs_vfs_close(void *ctx, int fd)
{
    apfs_t *fs = (apfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

static int apfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int apfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    (void)ctx; (void)fd; (void)buf; (void)size;
    return -1;
}

static int apfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    apfs_t *fs = (apfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    if (whence == VFS_SEEK_SET) fs->fds[fd].pos = offset;
    else if (whence == VFS_SEEK_CUR) fs->fds[fd].pos += offset;
    else if (whence == VFS_SEEK_END) fs->fds[fd].pos = fs->fds[fd].size + offset;
    return (int)fs->fds[fd].pos;
}

static int apfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    apfs_t *fs = (apfs_t *)ctx;
    if (!fs->mounted) return -1;

    uint64_t vol_block;
    if (apfs_get_vol_sb_block(fs, fs->fs_oid, &vol_block) != 0) return -1;

    uint64_t omap_tree, fstree_block;
    if (apfs_get_fstree_block(fs, vol_block, &omap_tree, &fstree_block) != 0) return -1;

    uint64_t dir_ino;
    int is_dir = 0;
    if (apfs_walk_path(fs, fstree_block, omap_tree, path, &dir_ino, &is_dir) != 0) return -1;
    if (!is_dir) return -1;

    uint8_t *node_buf = (uint8_t *)malloc(fs->block_size);
    if (!node_buf) return -1;

    if (apfs_read_block(fs, fstree_block, node_buf) != 0) { free(node_buf); return -1; }

    apfs_btnode_t *btn = (apfs_btnode_t *)node_buf;
    uint16_t key_count = btn->btn_key_count;
    uint8_t *toc_base  = node_buf + sizeof(apfs_btnode_t);
    uint8_t *key_base  = toc_base + key_count * sizeof(apfs_kvoff_t);
    uint8_t *val_end   = node_buf + fs->block_size;

    int count = 0;
    for (uint16_t ki = 0; ki < key_count && count < max; ki++) {
        apfs_kvoff_t *kv = (apfs_kvoff_t *)(toc_base + ki * sizeof(apfs_kvoff_t));
        uint8_t *key_ptr = key_base + kv->key_off;
        if (key_ptr + 8 > val_end) continue;

        uint64_t raw_key;
        memcpy(&raw_key, key_ptr, 8);
        uint64_t koid  = raw_key & 0x0FFFFFFFFFFFFFFFULL;
        uint32_t ktype = (uint32_t)(raw_key >> 60) & 0xFU;

        if (koid != dir_ino) continue;
        if (ktype != APFS_KEY_TYPE_DREC) continue;

        uint8_t *name_ext = key_ptr + 8;
        if (name_ext + 4 > val_end) continue;
        uint32_t name_lh;
        memcpy(&name_lh, name_ext, 4);
        uint16_t name_len = (uint16_t)(name_lh & 0x3FFU);
        uint8_t *name_ptr2 = name_ext + 4;
        if (name_ptr2 + name_len > val_end) continue;

        if (name_len == 1 && name_ptr2[0] == '.') continue;
        if (name_len == 2 && name_ptr2[0] == '.' && name_ptr2[1] == '.') continue;

        apfs_drec_val_t *dv = (apfs_drec_val_t *)(val_end - kv->val_off - sizeof(apfs_drec_val_t));
        if ((uint8_t *)dv < node_buf) continue;

        uint16_t dtype = (dv->flags >> 4) & 0xFU;
        int child_is_dir = (dtype == APFS_DREC_TYPE_DIR);

        int nl = name_len;
        if (nl > VFS_NAME_LEN - 1) nl = VFS_NAME_LEN - 1;
        memcpy(entries[count].name, name_ptr2, (size_t)nl);
        entries[count].name[nl] = 0;
        entries[count].is_dir   = child_is_dir;
        entries[count].inode    = (uint32_t)dv->file_id;
        entries[count].size     = 0;
        entries[count].mode     = 0;

        apfs_inode_val_t ival;
        uint64_t child_size = 0;
        if (apfs_get_inode(fs, fstree_block, dv->file_id, &ival, &child_size) == 0) {
            entries[count].size = (uint32_t)child_size;
            entries[count].mode = ival.mode;
        }
        count++;
    }

    free(node_buf);
    return count;
}

static int apfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx; (void)path; (void)mode;
    return -1;
}

static int apfs_vfs_unlink(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

static int apfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    apfs_t *fs = (apfs_t *)ctx;
    if (!fs->mounted) return -1;

    uint64_t vol_block;
    if (apfs_get_vol_sb_block(fs, fs->fs_oid, &vol_block) != 0) return -1;

    uint64_t omap_tree, fstree_block;
    if (apfs_get_fstree_block(fs, vol_block, &omap_tree, &fstree_block) != 0) return -1;

    uint64_t ino;
    int is_dir = 0;
    if (apfs_walk_path(fs, fstree_block, omap_tree, path, &ino, &is_dir) != 0) return -1;

    apfs_inode_val_t ival;
    uint64_t file_size = 0;
    apfs_get_inode(fs, fstree_block, ino, &ival, &file_size);

    const char *base = path;
    const char *p = path;
    while (*p) { if (*p == '/') base = p + 1; p++; }
    int nl = 0;
    while (base[nl] && nl < VFS_NAME_LEN - 1) { entry->name[nl] = base[nl]; nl++; }
    entry->name[nl] = 0;
    entry->size   = (uint32_t)file_size;
    entry->is_dir = is_dir;
    entry->inode  = (uint32_t)ino;
    entry->mode   = ival.mode;
    return 0;
}

static int apfs_vfs_rename(void *ctx, const char *old, const char *new)
{
    (void)ctx; (void)old; (void)new;
    return -1;
}

static int apfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

void apfs_mount_vfs(apfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ops = {
        .open    = apfs_vfs_open,
        .close   = apfs_vfs_close,
        .read    = apfs_vfs_read,
        .write   = apfs_vfs_write,
        .lseek   = apfs_vfs_lseek,
        .readdir = apfs_vfs_readdir,
        .mkdir   = apfs_vfs_mkdir,
        .unlink  = apfs_vfs_unlink,
        .stat    = apfs_vfs_stat,
        .rename  = apfs_vfs_rename,
        .symlink = apfs_vfs_symlink,
    };
    vfs_mount(mount_point, &ops, fs);
}

int apfs_format(blockdev_t *bd, const char *label)
{
    uint32_t block_size = 4096;
    uint64_t sector_sz  = bd->sector_size ? bd->sector_size : 512;
    uint64_t total_bytes= bd->total_sectors * sector_sz;
    uint64_t block_count= total_bytes / block_size;

    uint8_t *buf = (uint8_t *)malloc(block_size);
    if (!buf) return -1;
    memset(buf, 0, block_size);

    /* Write container superblock at block 0 */
    apfs_nx_sb_t *nx = (apfs_nx_sb_t *)buf;
    memset(buf, 0, block_size);

    nx->hdr.o_oid     = 1;
    nx->hdr.o_xid     = 1;
    nx->hdr.o_type    = APFS_OBJECT_TYPE_NX_SUPERBLOCK;
    nx->hdr.o_flags   = 0x8000U;
    nx->hdr.o_subtype = 0;
    nx->nx_magic      = APFS_NX_MAGIC;
    nx->nx_block_size = block_size;
    nx->nx_block_count= block_count;
    nx->nx_next_oid   = 1024;
    nx->nx_next_xid   = 2;
    nx->nx_omap_oid   = 2;
    nx->nx_fs_oid[0]  = 3;
    nx->nx_max_file_systems = 1;

    blockdev_write_bytes(bd, 0, block_size, buf);

    /* Write a minimal volume superblock at block 3 */
    memset(buf, 0, block_size);
    apfs_vol_sb_t *vsb = (apfs_vol_sb_t *)buf;

    vsb->hdr.o_oid     = 3;
    vsb->hdr.o_xid     = 1;
    vsb->hdr.o_type    = APFS_OBJECT_TYPE_FS;
    vsb->hdr.o_flags   = 0x8000U;
    vsb->apfs_magic    = APFS_VOL_MAGIC;
    vsb->apfs_next_obj_id = 1024;
    vsb->apfs_omap_oid    = 4;
    vsb->apfs_root_tree_oid = 5;

    if (label) {
        int k = 0;
        while (label[k] && k < 255) { vsb->apfs_volname[k] = label[k]; k++; }
        vsb->apfs_volname[k] = 0;
    } else {
        vsb->apfs_volname[0] = 0;
    }

    blockdev_write_bytes(bd, 3 * block_size, block_size, buf);
    free(buf);
    return 0;
}
