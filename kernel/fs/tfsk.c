#include "tfsk.h"
#include "string.h"
#include "memory.h"
#include "serial.h"
#include "terminal.h"
#include "scheduler.h"

static uint32_t tfsk_time(void)
{
    return task_get_ticks();
}

static uint32_t tfsk_checksum(const void *data, uint32_t len)
{
    uint32_t sum = 0xDEADBEEF;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        sum = ((sum << 5) | (sum >> 27)) + p[i];
    return sum;
}

static int tfsk_block_read(tfsk_t *fs, uint32_t blk, void *buf)
{
    if (blk >= fs->sb.total_blocks) return -1;
    return ata_read_sectors(fs->dev, (uint64_t)blk * 8, 8, buf);
}

static int tfsk_block_write(tfsk_t *fs, uint32_t blk, void *buf)
{
    if (blk >= fs->sb.total_blocks) return -1;
    return ata_write_sectors(fs->dev, (uint64_t)blk * 8, 8, buf);
}

static int tfsk_block_zero(tfsk_t *fs, uint32_t blk)
{
    uint8_t zero[TFSK_BLOCK_SIZE];
    memset(zero, 0, TFSK_BLOCK_SIZE);
    return tfsk_block_write(fs, blk, zero);
}

static int sb_sync(tfsk_t *fs)
{
    fs->sb.checksum = tfsk_checksum(&fs->sb, TFSK_SB_CKSUM_SIZE);
    return tfsk_block_write(fs, TFSK_SB_BLK, &fs->sb);
}

int tfsk_mount(tfsk_t *fs, ata_device_t *dev)
{
    memset(fs, 0, sizeof(tfsk_t));
    fs->dev = dev;

    if (ata_read_sectors(fs->dev, (uint64_t)TFSK_SB_BLK * 8, 8, &fs->sb) < 0)
        return -1;

    if (fs->sb.magic != TFSK_MAGIC)
        return -1;

    if (tfsk_checksum(&fs->sb, TFSK_SB_CKSUM_SIZE) != fs->sb.checksum)
        return -1;

    fs->sb.mount_count++;
    fs->sb.state = TFSK_STATE_DIRTY;
    fs->dirty = 1;
    sb_sync(fs);
    fs->mounted = 1;
    return 0;
}

int tfsk_umount(tfsk_t *fs)
{
    if (!fs->mounted) return -1;
    if (fs->dirty) {
        fs->sb.state = TFSK_STATE_CLEAN;
        sb_sync(fs);
    }
    fs->dirty = 0;
    fs->mounted = 0;
    return 0;
}

int tfsk_format(ata_device_t *dev, uint64_t blocks, const char *volume)
{
    uint8_t zero[TFSK_BLOCK_SIZE];
    memset(zero, 0, TFSK_BLOCK_SIZE);

    for (uint64_t i = 0; i < blocks; i++) {
        if (ata_write_sectors(dev, i * 8, 8, zero) < 0)
            return -1;
    }

    uint32_t bmp_blocks = (blocks + TFSK_BLOCK_SIZE * 8 - 1) / (TFSK_BLOCK_SIZE * 8);
    uint32_t inode_blocks = (TFSK_MAX_INODES + TFSK_INODES_PER_BLK - 1) / TFSK_INODES_PER_BLK;
    uint32_t inode_table_blk = TFSK_BLOCK_BMP_BLK + bmp_blocks;
    uint32_t data_start = inode_table_blk + inode_blocks;

    tfsk_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = TFSK_MAGIC;
    sb.version = 1;
    sb.total_blocks = blocks;
    sb.free_blocks = blocks - data_start - 1;
    sb.total_inodes = TFSK_MAX_INODES;
    sb.free_inodes = TFSK_MAX_INODES - 3;
    sb.inode_bmp_blk = TFSK_INODE_BMP_BLK;
    sb.block_bmp_blk = TFSK_BLOCK_BMP_BLK;
    sb.inode_table_blk = inode_table_blk;
    sb.root_inode = TFSK_ROOT_INODE;
    sb.state = TFSK_STATE_CLEAN;
    sb.mount_count = 0;
    sb.mount_time = 0;

    if (volume) {
        size_t vlen = strlen(volume);
        if (vlen >= TFSK_MAX_VOLUME) vlen = TFSK_MAX_VOLUME - 1;
        memcpy(sb.volume, volume, vlen);
    }

    sb.checksum = tfsk_checksum(&sb, TFSK_SB_CKSUM_SIZE);
    if (ata_write_sectors(dev, TFSK_SB_BLK * 8, 8, &sb) < 0)
        return -1;

    uint8_t *imap = kcalloc(1, TFSK_BLOCK_SIZE);
    if (!imap) return -1;
    for (uint32_t i = 0; i < 3; i++)
        imap[i / 8] |= (1 << (i % 8));
    if (ata_write_sectors(dev, TFSK_INODE_BMP_BLK * 8, 8, imap) < 0) {
        free(imap); return -1;
    }
    free(imap);

    uint8_t *bmap = kcalloc(bmp_blocks * TFSK_BLOCK_SIZE, 1);
    if (!bmap) return -1;
    for (uint32_t i = 0; i < data_start; i++)
        bmap[i / 8] |= (1 << (i % 8));
    for (uint32_t i = 0; i < bmp_blocks; i++) {
        if (ata_write_sectors(dev, (TFSK_BLOCK_BMP_BLK + i) * 8, 8, bmap + i * TFSK_BLOCK_SIZE) < 0) {
            free(bmap); return -1;
        }
    }
    free(bmap);

    tfsk_inode_t root;
    memset(&root, 0, sizeof(root));
    root.mode = (TFSK_FT_DIR << 12) | (4 << 6) | 4;
    root.uid = 0;
    root.gid = 0;
    root.link_count = 2;
    root.size = 0;
    root.blocks = 0;
    root.ctime = root.mtime = root.atime = 0;

    uint32_t root_data_blk = data_start;

    uint32_t inode_blk = inode_table_blk + (TFSK_ROOT_INODE - 1) / TFSK_INODES_PER_BLK;
    uint32_t inode_off = ((TFSK_ROOT_INODE - 1) % TFSK_INODES_PER_BLK) * TFSK_INODE_SIZE;

    uint8_t ibuf[TFSK_BLOCK_SIZE];
    memset(ibuf, 0, TFSK_BLOCK_SIZE);
    memcpy(ibuf + inode_off, &root, sizeof(root));
    if (ata_write_sectors(dev, inode_blk * 8, 8, ibuf) < 0)
        return -1;

    tfsk_dentry_t dot;
    memset(&dot, 0, sizeof(dot));
    dot.inode = TFSK_ROOT_INODE;
    dot.entry_len = sizeof(tfsk_dentry_t);
    dot.name_len = 1;
    dot.file_type = TFSK_FT_DIR;
    dot.name[0] = '.';

    tfsk_dentry_t dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.inode = TFSK_ROOT_INODE;
    dotdot.entry_len = sizeof(tfsk_dentry_t);
    dotdot.name_len = 2;
    dotdot.file_type = TFSK_FT_DIR;
    dotdot.name[0] = '.'; dotdot.name[1] = '.';

    uint8_t dir_block[TFSK_BLOCK_SIZE];
    memset(dir_block, 0, TFSK_BLOCK_SIZE);
    memcpy(dir_block, &dot, sizeof(tfsk_dentry_t));
    memcpy(dir_block + sizeof(tfsk_dentry_t), &dotdot, sizeof(tfsk_dentry_t));

    if (ata_write_sectors(dev, root_data_blk * 8, 8, dir_block) < 0)
        return -1;

    root.size = sizeof(tfsk_dentry_t) * 2;
    root.blocks = 1;
    root.u.ptr.direct[0] = root_data_blk;

    memset(ibuf, 0, TFSK_BLOCK_SIZE);
    memcpy(ibuf + inode_off, &root, sizeof(root));
    if (ata_write_sectors(dev, inode_blk * 8, 8, ibuf) < 0)
        return -1;

    sb.free_blocks = blocks - data_start - 1;
    sb.free_inodes = TFSK_MAX_INODES - 3;
    sb.checksum = tfsk_checksum(&sb, TFSK_SB_CKSUM_SIZE);
    if (ata_write_sectors(dev, TFSK_SB_BLK * 8, 8, &sb) < 0)
        return -1;

    return 0;
}

static int tfsk_inode_read(tfsk_t *fs, uint32_t ino, tfsk_inode_t *inode)
{
    if (ino < 1 || ino > fs->sb.total_inodes) return -1;
    uint32_t blk = fs->sb.inode_table_blk + (ino - 1) / TFSK_INODES_PER_BLK;
    uint32_t off = ((ino - 1) % TFSK_INODES_PER_BLK) * TFSK_INODE_SIZE;
    uint8_t buf[TFSK_BLOCK_SIZE];
    if (tfsk_block_read(fs, blk, buf) < 0) return -1;
    memcpy(inode, buf + off, TFSK_INODE_SIZE);
    return 0;
}

static int tfsk_inode_write(tfsk_t *fs, uint32_t ino, tfsk_inode_t *inode)
{
    if (ino < 1 || ino > fs->sb.total_inodes) return -1;
    uint32_t blk = fs->sb.inode_table_blk + (ino - 1) / TFSK_INODES_PER_BLK;
    uint32_t off = ((ino - 1) % TFSK_INODES_PER_BLK) * TFSK_INODE_SIZE;
    uint8_t buf[TFSK_BLOCK_SIZE];
    if (tfsk_block_read(fs, blk, buf) < 0) return -1;
    memcpy(buf + off, inode, TFSK_INODE_SIZE);
    return tfsk_block_write(fs, blk, buf);
}

static int tfsk_block_alloc(tfsk_t *fs)
{
    uint32_t bmp_blocks = (fs->sb.total_blocks + TFSK_BLOCK_SIZE * 8 - 1) / (TFSK_BLOCK_SIZE * 8);
    uint8_t *bmap = malloc(bmp_blocks * TFSK_BLOCK_SIZE);
    if (!bmap) return -1;

    for (uint32_t i = 0; i < bmp_blocks; i++)
        tfsk_block_read(fs, fs->sb.block_bmp_blk + i, bmap + i * TFSK_BLOCK_SIZE);

    for (uint32_t blk = 0; blk < fs->sb.total_blocks; blk++) {
        if (!(bmap[blk / 8] & (1 << (blk % 8)))) {
            bmap[blk / 8] |= (1 << (blk % 8));
            for (uint32_t i = 0; i < bmp_blocks; i++)
                tfsk_block_write(fs, fs->sb.block_bmp_blk + i, bmap + i * TFSK_BLOCK_SIZE);
            tfsk_block_zero(fs, blk);
            fs->sb.free_blocks--;
            sb_sync(fs);
            free(bmap);
            return blk;
        }
    }
    free(bmap);
    return -1;
}

static int tfsk_block_free(tfsk_t *fs, uint32_t blk)
{
    uint32_t bmp_blocks = (fs->sb.total_blocks + TFSK_BLOCK_SIZE * 8 - 1) / (TFSK_BLOCK_SIZE * 8);
    uint32_t bmp_idx = blk / (TFSK_BLOCK_SIZE * 8);
    uint32_t bmp_off = blk % (TFSK_BLOCK_SIZE * 8);
    if (bmp_idx >= bmp_blocks) return -1;
    uint8_t buf[TFSK_BLOCK_SIZE];
    memset(buf, 0, TFSK_BLOCK_SIZE);
    tfsk_block_read(fs, fs->sb.block_bmp_blk + bmp_idx, buf);
    buf[bmp_off / 8] &= ~(1 << (bmp_off % 8));
    tfsk_block_write(fs, fs->sb.block_bmp_blk + bmp_idx, buf);
    fs->sb.free_blocks++;
    sb_sync(fs);
    return 0;
}

static int tfsk_inode_alloc(tfsk_t *fs)
{
    uint8_t imap[TFSK_BLOCK_SIZE];
    tfsk_block_read(fs, fs->sb.inode_bmp_blk, imap);

    for (uint32_t ino = 1; ino <= fs->sb.total_inodes; ino++) {
        if (!(imap[(ino - 1) / 8] & (1 << ((ino - 1) % 8)))) {
            imap[(ino - 1) / 8] |= (1 << ((ino - 1) % 8));
            tfsk_block_write(fs, fs->sb.inode_bmp_blk, imap);

            tfsk_inode_t inode;
            memset(&inode, 0, sizeof(inode));
            inode.atime = inode.mtime = inode.ctime = tfsk_time();
            tfsk_inode_write(fs, ino, &inode);

            fs->sb.free_inodes--;
            sb_sync(fs);
            return ino;
        }
    }
    return -1;
}

static int tfsk_inode_free(tfsk_t *fs, uint32_t ino)
{
    if (ino < 3) return -1;
    uint8_t imap[TFSK_BLOCK_SIZE];
    tfsk_block_read(fs, fs->sb.inode_bmp_blk, imap);
    imap[(ino - 1) / 8] &= ~(1 << ((ino - 1) % 8));
    tfsk_block_write(fs, fs->sb.inode_bmp_blk, imap);
    fs->sb.free_inodes++;
    sb_sync(fs);
    return 0;
}

static int resolve_block_ptr(tfsk_t *fs, tfsk_inode_t *inode, uint32_t block_idx, uint32_t *phys_blk, int alloc)
{
    if (block_idx < TFSK_DIRECT_BLOCKS) {
        if (inode->u.ptr.direct[block_idx] == 0) {
            if (!alloc) { *phys_blk = 0; return 0; }
            int blk = tfsk_block_alloc(fs);
            if (blk < 0) return -1;
            inode->u.ptr.direct[block_idx] = blk;
        }
        *phys_blk = inode->u.ptr.direct[block_idx];
        return 0;
    }

    block_idx -= TFSK_DIRECT_BLOCKS;
    uint32_t per_indirect = TFSK_BLOCK_SIZE / 4;

    if (block_idx < per_indirect) {
        if (inode->indirect == 0) {
            if (!alloc) { *phys_blk = 0; return 0; }
            int blk = tfsk_block_alloc(fs);
            if (blk < 0) return -1;
            inode->indirect = blk;
            tfsk_block_zero(fs, blk);
        }
        uint32_t table[TFSK_BLOCK_SIZE / 4];
        tfsk_block_read(fs, inode->indirect, table);
        if (table[block_idx] == 0) {
            if (!alloc) { *phys_blk = 0; return 0; }
            int blk = tfsk_block_alloc(fs);
            if (blk < 0) return -1;
            table[block_idx] = blk;
            tfsk_block_write(fs, inode->indirect, table);
        }
        *phys_blk = table[block_idx];
        return 0;
    }

    block_idx -= per_indirect;
    if (inode->double_indirect == 0) {
        if (!alloc) { *phys_blk = 0; return 0; }
        int blk = tfsk_block_alloc(fs);
        if (blk < 0) return -1;
        inode->double_indirect = blk;
        tfsk_block_zero(fs, blk);
    }

    uint32_t l1_idx = block_idx / per_indirect;
    uint32_t l2_idx = block_idx % per_indirect;

    uint32_t table[TFSK_BLOCK_SIZE / 4];
    tfsk_block_read(fs, inode->double_indirect, table);
    if (table[l1_idx] == 0) {
        if (!alloc) { *phys_blk = 0; return 0; }
        int blk = tfsk_block_alloc(fs);
        if (blk < 0) return -1;
        table[l1_idx] = blk;
        tfsk_block_write(fs, inode->double_indirect, table);
        tfsk_block_zero(fs, blk);
    }

    uint32_t l2_table[TFSK_BLOCK_SIZE / 4];
    tfsk_block_read(fs, table[l1_idx], l2_table);
    if (l2_table[l2_idx] == 0) {
        if (!alloc) { *phys_blk = 0; return 0; }
        int blk = tfsk_block_alloc(fs);
        if (blk < 0) return -1;
        l2_table[l2_idx] = blk;
        tfsk_block_write(fs, table[l1_idx], l2_table);
    }
    *phys_blk = l2_table[l2_idx];
    return 0;
}

static int tfsk_read_data(tfsk_t *fs, tfsk_inode_t *inode, void *buf, uint32_t size, uint32_t offset)
{
    if (offset >= inode->size) return 0;
    if (offset + size > inode->size) size = inode->size - offset;

    if (inode->size <= TFSK_INLINE_MAX) {
        uint32_t copy = size;
        if (offset + copy > TFSK_INLINE_MAX) copy = TFSK_INLINE_MAX - offset;
        memcpy(buf, inode->u.inline_data + offset, copy);
        return copy;
    }

    uint32_t done = 0;
    while (done < size) {
        uint32_t blk_idx = (offset + done) / TFSK_BLOCK_SIZE;
        uint32_t blk_off = (offset + done) % TFSK_BLOCK_SIZE;
        uint32_t phys;

        if (resolve_block_ptr(fs, inode, blk_idx, &phys, 0) < 0) break;
        if (phys == 0) break;

        char block[TFSK_BLOCK_SIZE];
        tfsk_block_read(fs, phys, block);

        uint32_t chunk = TFSK_BLOCK_SIZE - blk_off;
        if (chunk > size - done) chunk = size - done;
        memcpy((char *)buf + done, block + blk_off, chunk);
        done += chunk;
    }
    return done;
}

static int tfsk_write_data(tfsk_t *fs, tfsk_inode_t *inode, const void *buf, uint32_t size, uint32_t offset)
{
    if (inode->size <= TFSK_INLINE_MAX && offset + size <= TFSK_INLINE_MAX) {
        memcpy(inode->u.inline_data + offset, buf, size);
        if (offset + size > inode->size) inode->size = offset + size;
        return size;
    }

    if (inode->size <= TFSK_INLINE_MAX && offset + size > TFSK_INLINE_MAX) {
        uint8_t tmp[TFSK_INLINE_MAX];
        memcpy(tmp, inode->u.inline_data, inode->size);

        int blk = tfsk_block_alloc(fs);
        if (blk < 0) return -1;
        inode->u.ptr.direct[0] = blk;
        tfsk_block_write(fs, blk, tmp);
        memset(inode->u.inline_data, 0, TFSK_INLINE_MAX);
    }

    uint32_t done = 0;
    while (done < size) {
        uint32_t blk_idx = (offset + done) / TFSK_BLOCK_SIZE;
        uint32_t blk_off = (offset + done) % TFSK_BLOCK_SIZE;
        uint32_t phys;

        if (resolve_block_ptr(fs, inode, blk_idx, &phys, 1) < 0) break;

        char block[TFSK_BLOCK_SIZE];
        tfsk_block_read(fs, phys, block);

        uint32_t chunk = TFSK_BLOCK_SIZE - blk_off;
        if (chunk > size - done) chunk = size - done;
        memcpy(block + blk_off, (const char *)buf + done, chunk);
        tfsk_block_write(fs, phys, block);
        done += chunk;
    }

    if (offset + size > inode->size) inode->size = offset + size;
    uint32_t new_blocks = (inode->size + TFSK_BLOCK_SIZE - 1) / TFSK_BLOCK_SIZE;
    if (new_blocks > inode->blocks) inode->blocks = new_blocks;
    return done;
}

static int dir_find(tfsk_t *fs, tfsk_inode_t *dir, const char *name, tfsk_dentry_t *out, uint32_t *offset)
{
    uint32_t off = 0;
    tfsk_dentry_t dent;
    size_t nlen = strlen(name);

    while (off < dir->size) {
        if (tfsk_read_data(fs, dir, &dent, sizeof(tfsk_dentry_t), off) < (int)sizeof(tfsk_dentry_t))
            break;

        if (dent.inode && dent.name_len == nlen &&
            memcmp(dent.name, name, dent.name_len) == 0) {
            if (out) memcpy(out, &dent, sizeof(tfsk_dentry_t));
            if (offset) *offset = off;
            return 0;
        }
        off += dent.entry_len;
    }
    return -1;
}

static int tfsk_lookup(tfsk_t *fs, uint32_t dir_ino, const char *name, uint32_t *ino)
{
    tfsk_inode_t dir;
    if (tfsk_inode_read(fs, dir_ino, &dir) < 0) return -1;
    if (!(dir.mode & (TFSK_FT_DIR << 12))) return -1;

    tfsk_dentry_t dent;
    if (dir_find(fs, &dir, name, &dent, NULL) < 0) return -1;
    if (ino) *ino = dent.inode;
    return 0;
}

static int tfsk_readdir(tfsk_t *fs, uint32_t dir_ino, uint32_t *offset, tfsk_dentry_t *dent)
{
    tfsk_inode_t dir;
    if (tfsk_inode_read(fs, dir_ino, &dir) < 0) return -1;

    uint32_t off = offset ? *offset : 0;
    while (off < dir.size) {
        int n = tfsk_read_data(fs, &dir, dent, sizeof(tfsk_dentry_t), off);
        if (n < (int)sizeof(tfsk_dentry_t)) break;

        if (dent->inode != 0) {
            if (offset) *offset = off + dent->entry_len;
            return 0;
        }
        off += dent->entry_len;
    }
    return -1;
}

static int tfsk_link(tfsk_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, int filetype)
{
    tfsk_inode_t dir;
    if (tfsk_inode_read(fs, dir_ino, &dir) < 0) return -1;

    if (dir_find(fs, &dir, name, NULL, NULL) == 0) return -1;

    uint16_t nlen = strlen(name);
    uint16_t elen = sizeof(tfsk_dentry_t);

    tfsk_dentry_t dent;
    memset(&dent, 0, sizeof(dent));
    dent.inode = ino;
    dent.entry_len = elen;
    dent.name_len = nlen;
    dent.file_type = filetype;
    memcpy(dent.name, name, nlen);

    int n = tfsk_write_data(fs, &dir, &dent, sizeof(tfsk_dentry_t), dir.size);
    if (n < 0) return -1;

    dir.atime = dir.mtime = tfsk_time();
    tfsk_inode_write(fs, dir_ino, &dir);

    if (ino) {
        tfsk_inode_t target;
        tfsk_inode_read(fs, ino, &target);
        target.link_count++;
        tfsk_inode_write(fs, ino, &target);
    }
    return 0;
}

static int tfsk_unlink(tfsk_t *fs, uint32_t dir_ino, const char *name)
{
    tfsk_inode_t dir;
    if (tfsk_inode_read(fs, dir_ino, &dir) < 0) return -1;

    tfsk_dentry_t dent;
    uint32_t off;
    if (dir_find(fs, &dir, name, &dent, &off) < 0) return -1;

    uint32_t ino = dent.inode;
    tfsk_inode_t target;
    tfsk_inode_read(fs, ino, &target);

    if (target.mode & (TFSK_FT_DIR << 12)) {
        if (target.link_count > 2) return -1;
        uint32_t d_off = 0;
        tfsk_dentry_t de;
        while (tfsk_readdir(fs, ino, &d_off, &de) == 0) {
            if (de.inode && !(de.name_len == 1 && de.name[0] == '.') &&
                !(de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.')) {
                return -1;
            }
        }
    }

    dent.inode = 0;
    tfsk_write_data(fs, &dir, &dent, sizeof(tfsk_dentry_t), off);

    dir.atime = dir.mtime = tfsk_time();
    tfsk_inode_write(fs, dir_ino, &dir);

    target.link_count--;
    tfsk_inode_write(fs, ino, &target);

    if (target.link_count == 0) {
        for (uint32_t i = 0; i < target.blocks; i++) {
            uint32_t phys;
            if (resolve_block_ptr(fs, &target, i, &phys, 0) == 0 && phys != 0)
                tfsk_block_free(fs, phys);
        }
        if (target.indirect) tfsk_block_free(fs, target.indirect);
        if (target.double_indirect) tfsk_block_free(fs, target.double_indirect);
        tfsk_inode_free(fs, ino);
    }
    return 0;
}

int tfsk_mkdir(tfsk_t *fs, uint32_t parent_ino, const char *name)
{
    int ino = tfsk_inode_alloc(fs);
    if (ino < 0) return -1;

    tfsk_inode_t inode;
    tfsk_inode_read(fs, ino, &inode);
    inode.mode = (TFSK_FT_DIR << 12) | (4 << 6) | 4;
    inode.link_count = 2;
    inode.ctime = inode.mtime = inode.atime = tfsk_time();
    tfsk_inode_write(fs, ino, &inode);

    tfsk_dentry_t dot;
    memset(&dot, 0, sizeof(dot));
    dot.inode = ino;
    dot.entry_len = sizeof(tfsk_dentry_t);
    dot.name_len = 1;
    dot.file_type = TFSK_FT_DIR;
    dot.name[0] = '.';
    tfsk_write_data(fs, &inode, &dot, sizeof(tfsk_dentry_t), 0);

    tfsk_dentry_t dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.inode = parent_ino;
    dotdot.entry_len = sizeof(tfsk_dentry_t);
    dotdot.name_len = 2;
    dotdot.file_type = TFSK_FT_DIR;
    dotdot.name[0] = '.'; dotdot.name[1] = '.';
    tfsk_write_data(fs, &inode, &dotdot, sizeof(tfsk_dentry_t), sizeof(tfsk_dentry_t));

    inode.size = sizeof(tfsk_dentry_t) * 2;
    tfsk_inode_write(fs, ino, &inode);

    tfsk_link(fs, parent_ino, name, ino, TFSK_FT_DIR);

    tfsk_inode_t parent;
    tfsk_inode_read(fs, parent_ino, &parent);
    parent.link_count++;
    tfsk_inode_write(fs, parent_ino, &parent);

    return 0;
}

static int tfsk_creat(tfsk_t *fs, uint32_t dir_ino, const char *name)
{
    int ino = tfsk_inode_alloc(fs);
    if (ino < 0) return -1;

    tfsk_inode_t inode;
    tfsk_inode_read(fs, ino, &inode);
    inode.mode = (TFSK_FT_FILE << 12) | (6 << 6) | 6;
    inode.link_count = 1;
    inode.ctime = inode.mtime = inode.atime = tfsk_time();
    tfsk_inode_write(fs, ino, &inode);
    tfsk_link(fs, dir_ino, name, ino, TFSK_FT_FILE);
    return ino;
}

int tfsk_walk(tfsk_t *fs, const char *path, uint32_t *ino)
{
    if (!path || !path[0]) return -1;
    uint32_t cur = TFSK_ROOT_INODE;

    while (*path == '/') path++;
    if (!*path) { *ino = cur; return 0; }

    char comp[TFSK_MAX_NAME];
    while (*path) {
        int i = 0;
        while (*path && *path != '/' && i < TFSK_MAX_NAME - 1)
            comp[i++] = *path++;
        comp[i] = 0;
        while (*path == '/') path++;

        if (comp[0] == '.' && comp[1] == 0) continue;

        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            uint32_t parent;
            if (tfsk_lookup(fs, cur, "..", &parent) == 0)
                cur = parent;
            continue;
        }

        if (tfsk_lookup(fs, cur, comp, &cur) < 0)
            return -1;
    }
    *ino = cur;
    return 0;
}

int tfsk_vfs_open(tfsk_t *fs, const char *path, int flags)
{
    if (!fs->mounted) return -1;

    uint32_t ino;
    if (tfsk_walk(fs, path, &ino) < 0) {
        if (!(flags & 2)) return -1;

        char parent_path[TFSK_MAX_NAME];
        char name_buf[TFSK_MAX_NAME];
        int i = 0, last_sep = -1;
        while (path[i]) i++;
        for (int j = i - 1; j >= 0; j--) {
            if (path[j] == '/') { last_sep = j; break; }
        }
        if (last_sep < 0) return -1;
        for (int j = 0; j < last_sep; j++) parent_path[j] = path[j];
        parent_path[last_sep] = 0;
        int k = 0;
        for (int j = last_sep + 1; path[j]; j++) name_buf[k++] = path[j];
        name_buf[k] = 0;

        uint32_t p_ino;
        if (tfsk_walk(fs, parent_path, &p_ino) < 0) return -1;
        ino = tfsk_creat(fs, p_ino, name_buf);
        if ((int)ino < 0) return -1;
    }

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].ino = ino;
            fs->fds[i].offset = 0;
            fs->fds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

int tfsk_vfs_close(tfsk_t *fs, int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    fs->fds[fd].used = 0;
    return 0;
}

int tfsk_vfs_read(tfsk_t *fs, int fd, void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    tfsk_inode_t inode;
    if (tfsk_inode_read(fs, fs->fds[fd].ino, &inode) < 0) return -1;
    if (inode.mode & (TFSK_FT_DIR << 12)) return -1;

    int n = tfsk_read_data(fs, &inode, buf, size, fs->fds[fd].offset);
    if (n > 0) fs->fds[fd].offset += n;
    return n;
}

int tfsk_vfs_write(tfsk_t *fs, int fd, const void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    tfsk_inode_t inode;
    if (tfsk_inode_read(fs, fs->fds[fd].ino, &inode) < 0) return -1;
    if (inode.mode & (TFSK_FT_DIR << 12)) return -1;

    int n = tfsk_write_data(fs, &inode, buf, size, fs->fds[fd].offset);
    if (n > 0) {
        fs->fds[fd].offset += n;
        inode.mtime = inode.atime = tfsk_time();
        tfsk_inode_write(fs, fs->fds[fd].ino, &inode);
    }
    return n;
}

int tfsk_vfs_lseek(tfsk_t *fs, int fd, uint32_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    tfsk_inode_t inode;
    if (tfsk_inode_read(fs, fs->fds[fd].ino, &inode) < 0) return -1;

    if (whence == 0) fs->fds[fd].offset = offset;
    else if (whence == 1) fs->fds[fd].offset += offset;
    else if (whence == 2) fs->fds[fd].offset = inode.size + offset;
    if (fs->fds[fd].offset > inode.size) fs->fds[fd].offset = inode.size;
    return fs->fds[fd].offset;
}

int tfsk_vfs_readdir(tfsk_t *fs, const char *path, vfs_entry_t *entries, int max)
{
    if (!fs->mounted) return -1;

    uint32_t ino;
    if (tfsk_walk(fs, path, &ino) < 0) return -1;

    tfsk_inode_t dir;
    if (tfsk_inode_read(fs, ino, &dir) < 0) return -1;

    uint32_t off = 0;
    int count = 0;
    tfsk_dentry_t dent;

    while (count < max && tfsk_readdir(fs, ino, &off, &dent) == 0) {
        uint32_t child_ino = dent.inode;
        tfsk_inode_t child;
        if (tfsk_inode_read(fs, child_ino, &child) < 0) continue;

        int k = 0;
        while (dent.name[k] && k < VFS_NAME_LEN - 1) {
            entries[count].name[k] = dent.name[k];
            k++;
        }
        entries[count].name[k] = 0;
        entries[count].size = child.size;
        entries[count].is_dir = (child.mode & (TFSK_FT_DIR << 12)) ? 1 : 0;
        entries[count].inode = child_ino;
        entries[count].mode = child.mode;
        count++;
    }
    return count;
}

int tfsk_vfs_mkdir(tfsk_t *fs, const char *path, uint32_t mode)
{
    (void)mode;
    if (!fs->mounted) return -1;

    char parent_path[TFSK_MAX_NAME];
    char name_buf[TFSK_MAX_NAME];
    int i = 0, last_sep = -1;
    while (path[i]) i++;
    for (int j = i - 1; j >= 0; j--) {
        if (path[j] == '/') { last_sep = j; break; }
    }
    if (last_sep < 0) return -1;
    for (int j = 0; j < last_sep; j++) parent_path[j] = path[j];
    parent_path[last_sep] = 0;
    int k = 0;
    for (int j = last_sep + 1; path[j]; j++) name_buf[k++] = path[j];
    name_buf[k] = 0;

    uint32_t p_ino;
    if (tfsk_walk(fs, parent_path, &p_ino) < 0) return -1;
    return tfsk_mkdir(fs, p_ino, name_buf);
}

int tfsk_vfs_unlink(tfsk_t *fs, const char *path)
{
    if (!fs->mounted) return -1;

    char parent_path[TFSK_MAX_NAME];
    char name_buf[TFSK_MAX_NAME];
    int i = 0, last_sep = -1;
    while (path[i]) i++;
    for (int j = i - 1; j >= 0; j--) {
        if (path[j] == '/') { last_sep = j; break; }
    }
    if (last_sep < 0) return -1;
    for (int j = 0; j < last_sep; j++) parent_path[j] = path[j];
    parent_path[last_sep] = 0;
    int k = 0;
    for (int j = last_sep + 1; path[j]; j++) name_buf[k++] = path[j];
    name_buf[k] = 0;

    uint32_t p_ino;
    if (tfsk_walk(fs, parent_path, &p_ino) < 0) return -1;
    return tfsk_unlink(fs, p_ino, name_buf);
}

int tfsk_vfs_stat(tfsk_t *fs, const char *path, vfs_entry_t *entry)
{
    if (!fs->mounted) return -1;

    uint32_t ino;
    if (tfsk_walk(fs, path, &ino) < 0) return -1;

    tfsk_inode_t inode;
    if (tfsk_inode_read(fs, ino, &inode) < 0) return -1;

    int i = 0;
    const char *p = path;
    while (*p) p++;
    while (p > path && *(p-1) != '/') p--;
    while (*p && i < VFS_NAME_LEN - 1) { entry->name[i++] = *p++; }
    entry->name[i] = 0;

    entry->size = inode.size;
    entry->is_dir = (inode.mode & (TFSK_FT_DIR << 12)) ? 1 : 0;
    entry->inode = ino;
    entry->mode = inode.mode;
    return 0;
}

int tfsk_probe_and_mount(tfsk_t *fs)
{
    for (int i = 0; i < ata_device_count; i++) {
        if (tfsk_mount(fs, &ata_devices[i]) == 0) {
            serial_write("tfsk: mounted from device ");
            serial_write(ata_devices[i].model);
            serial_write("\n");
            return 0;
        }
    }
    return -1;
}

static int tfsk_open_stub(void *ctx, const char *path, int flags)
{
    return tfsk_vfs_open((tfsk_t *)ctx, path, flags);
}

static int tfsk_close_stub(void *ctx, int fd)
{
    return tfsk_vfs_close((tfsk_t *)ctx, fd);
}

static int tfsk_read_stub(void *ctx, int fd, void *buf, uint32_t size)
{
    return tfsk_vfs_read((tfsk_t *)ctx, fd, buf, size);
}

static int tfsk_write_stub(void *ctx, int fd, const void *buf, uint32_t size)
{
    return tfsk_vfs_write((tfsk_t *)ctx, fd, buf, size);
}

static int tfsk_lseek_stub(void *ctx, int fd, uint32_t offset, int whence)
{
    return tfsk_vfs_lseek((tfsk_t *)ctx, fd, offset, whence);
}

static int tfsk_readdir_stub(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    return tfsk_vfs_readdir((tfsk_t *)ctx, path, entries, max);
}

static int tfsk_mkdir_stub(void *ctx, const char *path, uint32_t mode)
{
    return tfsk_vfs_mkdir((tfsk_t *)ctx, path, mode);
}

static int tfsk_unlink_stub(void *ctx, const char *path)
{
    return tfsk_vfs_unlink((tfsk_t *)ctx, path);
}

static int tfsk_stat_stub(void *ctx, const char *path, vfs_entry_t *entry)
{
    return tfsk_vfs_stat((tfsk_t *)ctx, path, entry);
}

void tfsk_mount_vfs(tfsk_t *fs, const char *mount_point)
{
    static vfs_ops_t tfsk_vfs_ops;
    tfsk_vfs_ops.open    = tfsk_open_stub;
    tfsk_vfs_ops.close   = tfsk_close_stub;
    tfsk_vfs_ops.read    = tfsk_read_stub;
    tfsk_vfs_ops.write   = tfsk_write_stub;
    tfsk_vfs_ops.lseek   = tfsk_lseek_stub;
    tfsk_vfs_ops.readdir = tfsk_readdir_stub;
    tfsk_vfs_ops.mkdir   = tfsk_mkdir_stub;
    tfsk_vfs_ops.unlink  = tfsk_unlink_stub;
    tfsk_vfs_ops.stat    = tfsk_stat_stub;
    tfsk_vfs_ops.rename  = 0;
    tfsk_vfs_ops.symlink = 0;
    vfs_mount(mount_point, &tfsk_vfs_ops, fs);
}
