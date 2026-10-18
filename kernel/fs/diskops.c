#include "diskops.h"
#include "stdio.h"
#include "string.h"
#include "blockdev.h"
#include "vfs.h"
#include "tfsk.h"
#include "fat16.h"
#include "fat32.h"
#include "exfat.h"
#include "ext2.h"
#include "ext3.h"
#include "ext4.h"
#include "ntfs.h"
#include "btrfs.h"
#include "xfs.h"
#include "zfs.h"
#include "apfs.h"
#include "klog.h"
#include "auth.h"
#include "debugmon.h"

const char *diskops_fstypes[] = { "tfsk", "fat16", "fat32", "exfat", "ext2", "ext3", "ext4", "ntfs", "btrfs", "xfs", "zfs", "apfs" };
const int diskops_fstypes_count = 12;

/* Every mount/umount/format failure funnels through here, so the
 * kernel log (`log`/`dmesg`) sees every error a caller sees in `err`,
 * without having to remember to log at each of the many per-fstype
 * return points above. */
static void seterr(char *err, int err_len, const char *msg)
{
    if (err && err_len > 0) { strncpy(err, msg, err_len - 1); err[err_len - 1] = 0; }
    klog_write("diskops: error: ");
    klog_write(msg);
    klog_write("\n");
}

static void log3(const char *a, const char *b, const char *c)
{
    klog_write(a);
    if (b) klog_write(b);
    if (c) klog_write(c);
    klog_write("\n");
}

/* the disk tOS runs from (its tFS is at /mnt, and /system, /home, /etc live on it) can never
 * be unmounted, ejected or erased: everything would fall away underneath the running system */
static int is_startup_mount(const char *mp)
{
    return !strcmp(mp, "/mnt") || !strcmp(mp, "/mnt/") || !strcmp(mp, "/");
}

static int is_startup_dev(blockdev_t *bd)
{
    return bd && bd->mounted && is_startup_mount(bd->mount_point);
}

int diskops_mount(const char *name, const char *mount_point, const char *fstype, char *err, int err_len)
{
    if (!auth_is_root()) { seterr(err, err_len, "permission denied (needs root; try sudo)"); return -1; }
    log3("diskops: mounting ", name, NULL);

    blockdev_t *bd = blockdev_find(name);
    if (!bd) { seterr(err, err_len, "no such device"); return -1; }
    if (bd->mounted) { seterr(err, err_len, "device already mounted"); return -1; }
    blockdev_cache_invalidate(bd); /* tfsk writes bypass blockdev, so drop anything cached before mounting */

    if (strcmp(fstype, "tfsk") == 0) {
        if (bd->type != BLOCKDEV_ATA) { seterr(err, err_len, "tfsk is currently only supported on ATA devices"); return -1; }
        static tfsk_t fs_instances[VFS_MAX_MOUNTS];
        static int fs_next = 0;
        if (fs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        tfsk_t *fs = &fs_instances[fs_next];
        memset(fs, 0, sizeof(*fs));
        if (tfsk_mount(fs, (ata_device_t *)bd->driver_data) != 0) { seterr(err, err_len, "tfsk probe failed (not formatted?)"); return -1; }
        fs_next++;
        tfsk_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat16") == 0) {
        static fat16_t fat16_instances[VFS_MAX_MOUNTS];
        fat16_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!fat16_instances[i].bd) { fs = &fat16_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (fat16_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "fat16 probe failed (not formatted?)"); return -1; }
        fat16_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat32") == 0) {
        static fat32_t fat32_instances[VFS_MAX_MOUNTS];
        fat32_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!fat32_instances[i].bd) { fs = &fat32_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (fat32_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "fat32 probe failed (not formatted?)"); return -1; }
        fat32_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "exfat") == 0) {
        static exfat_t exfat_instances[VFS_MAX_MOUNTS];
        exfat_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!exfat_instances[i].bd) { fs = &exfat_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (exfat_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "exfat probe failed (not formatted?)"); return -1; }
        exfat_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext3") == 0 || strcmp(fstype, "ext4") == 0) {
        /* The ext4 driver serves every ext2/3/4 volume made by Linux's mke2fs;
         * it declines volumes in tOS's own ext2/ext3 layout, which the older
         * drivers keep (their journal is not jbd2). */
        static ext4_t ext4_instances[VFS_MAX_MOUNTS];
        ext4_t *fs4 = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!ext4_instances[i].bd) { fs4 = &ext4_instances[i]; break; }
        if (!fs4) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs4, 0, sizeof(*fs4));
        if (ext4_probe_and_mount(fs4, bd) == 0) {
            ext4_mount_vfs(fs4, mount_point);
            bd->fs_ctx = fs4;
            fstype = "ext4";
        } else if (strcmp(fstype, "ext2") == 0) {
            memset(fs4, 0, sizeof(*fs4));
            static ext2_t ext2_instances[VFS_MAX_MOUNTS];
            ext2_t *fs = 0;
            for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!ext2_instances[i].bd) { fs = &ext2_instances[i]; break; }
            if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
            memset(fs, 0, sizeof(*fs));
            if (ext2_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "ext2 probe failed (not formatted?)"); return -1; }
            ext2_mount_vfs(fs, mount_point);
            bd->fs_ctx = fs;
        } else if (strcmp(fstype, "ext3") == 0) {
            memset(fs4, 0, sizeof(*fs4));
            static ext3_t ext3_instances[VFS_MAX_MOUNTS];
            ext3_t *fs = 0;
            for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!ext3_instances[i].bd) { fs = &ext3_instances[i]; break; }
            if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
            memset(fs, 0, sizeof(*fs));
            if (ext3_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "ext3 probe failed (not formatted?)"); return -1; }
            ext3_mount_vfs(fs, mount_point);
            bd->fs_ctx = fs;
        } else {
            memset(fs4, 0, sizeof(*fs4));
            seterr(err, err_len, "ext4 probe failed (not formatted?)");
            return -1;
        }
    } else if (strcmp(fstype, "ntfs") == 0) {
        static ntfs_t ntfs_instances[VFS_MAX_MOUNTS];
        ntfs_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!ntfs_instances[i].bd) { fs = &ntfs_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        if (ntfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "ntfs probe failed (not an NTFS volume, or unsupported layout)"); return -1; }
        ntfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "btrfs") == 0) {
        static btrfs_t btrfs_instances[VFS_MAX_MOUNTS];
        btrfs_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!btrfs_instances[i].bd) { fs = &btrfs_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (btrfs_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "btrfs probe failed (not formatted?)"); return -1; }
        btrfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "xfs") == 0) {
        static xfs_t xfs_instances[VFS_MAX_MOUNTS];
        xfs_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!xfs_instances[i].bd) { fs = &xfs_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (xfs_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "xfs probe failed (not formatted?)"); return -1; }
        xfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "zfs") == 0) {
        static zfs_t zfs_instances[VFS_MAX_MOUNTS];
        zfs_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!zfs_instances[i].bd) { fs = &zfs_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (zfs_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "zfs probe failed (not formatted?)"); return -1; }
        zfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "apfs") == 0) {
        static apfs_t apfs_instances[VFS_MAX_MOUNTS];
        apfs_t *fs = 0;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) if (!apfs_instances[i].bd) { fs = &apfs_instances[i]; break; }
        if (!fs) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        memset(fs, 0, sizeof(*fs));
        if (apfs_probe_and_mount(fs, bd) != 0) { memset(fs, 0, sizeof(*fs)); seterr(err, err_len, "apfs probe failed (not formatted?)"); return -1; }
        apfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else {
        seterr(err, err_len, "unsupported filesystem type");
        return -1;
    }

    bd->mounted = 1;
    int i = 0;
    while (mount_point[i] && i < BLOCKDEV_MOUNT_LEN - 1) { bd->mount_point[i] = mount_point[i]; i++; }
    bd->mount_point[i] = 0;
    i = 0;
    while (fstype[i] && i < BLOCKDEV_FSTYPE_LEN - 1) { bd->fs_type[i] = fstype[i]; i++; }
    bd->fs_type[i] = 0;
    log3("diskops: mounted ", name, " OK");
    return 0;
}

const char *diskops_detect(const char *name)
{
    blockdev_t *bd = blockdev_find(name);
    if (!bd || bd->mounted) return NULL;
    blockdev_cache_invalidate(bd);

    static fat32_t det_f32;
    static fat16_t det_f16;
    static exfat_t det_ef;
    static ext4_t  det_e4;
    static ext3_t  det_e3;
    static ext2_t  det_e2;
    static btrfs_t det_btrfs;
    static xfs_t   det_xfs;
    static zfs_t   det_zfs;
    static apfs_t  det_apfs;

    /* this device only, and without writing: tfsk_probe_and_mount() tries every ATA disk and
     * mounting writes the superblock, which on the running startup disk is no probe at all */
    if (bd->type == BLOCKDEV_ATA && tfsk_probe((ata_device_t *)bd->driver_data) == 0) return "tfsk";

    memset(&det_f32, 0, sizeof(det_f32));
    if (fat32_probe_and_mount(&det_f32, bd) == 0) return "fat32";

    memset(&det_f16, 0, sizeof(det_f16));
    if (fat16_probe_and_mount(&det_f16, bd) == 0) return "fat16";

    memset(&det_ef, 0, sizeof(det_ef));
    if (exfat_probe_and_mount(&det_ef, bd) == 0) return "exfat";

    memset(&det_e4, 0, sizeof(det_e4));
    if (ext4_probe_only(&det_e4, bd) == 0) return ext4_flavor(&det_e4);

    memset(&det_e3, 0, sizeof(det_e3));
    if (ext3_probe_and_mount(&det_e3, bd) == 0) return "ext3";

    memset(&det_e2, 0, sizeof(det_e2));
    if (ext2_probe_and_mount(&det_e2, bd) == 0) return "ext2";

    if (ntfs_detect(bd) == 0) return "ntfs";

    memset(&det_btrfs, 0, sizeof(det_btrfs));
    if (btrfs_probe_and_mount(&det_btrfs, bd) == 0) return "btrfs";

    memset(&det_xfs, 0, sizeof(det_xfs));
    if (xfs_probe_and_mount(&det_xfs, bd) == 0) return "xfs";

    memset(&det_zfs, 0, sizeof(det_zfs));
    if (zfs_probe_and_mount(&det_zfs, bd) == 0) return "zfs";

    memset(&det_apfs, 0, sizeof(det_apfs));
    if (apfs_probe_and_mount(&det_apfs, bd) == 0) return "apfs";

    return NULL;
}

int diskops_umount(const char *mount_point, char *err, int err_len)
{
    if (!auth_is_root()) { seterr(err, err_len, "permission denied (needs root; try sudo)"); return -1; }
    log3("diskops: unmounting ", mount_point, NULL);
    if (is_startup_mount(mount_point)) { seterr(err, err_len, "the startup disk can't be unmounted"); return -1; }

    if (vfs_unmount(mount_point) != 0) { seterr(err, err_len, "umount failed (not mounted, or busy)"); return -1; }

    int n = blockdev_count();
    for (int i = 0; i < n; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (bd && bd->mounted && strcmp(bd->mount_point, mount_point) == 0) {
            if (strcmp(bd->fs_type, "ntfs") == 0 && bd->fs_ctx) {
                ntfs_t *nfs = (ntfs_t *)bd->fs_ctx;
                ntfs_umount(nfs);
                memset(nfs, 0, sizeof(*nfs));
            }
            if (strcmp(bd->fs_type, "exfat") == 0 && bd->fs_ctx) exfat_umount((exfat_t *)bd->fs_ctx);
            if (strcmp(bd->fs_type, "btrfs") == 0 && bd->fs_ctx) btrfs_umount((btrfs_t *)bd->fs_ctx);
            if (bd->fs_ctx) {
                /* free the instance slot (a slot is in use while its bd is set) */
                const char *ft = bd->fs_type;
#define CLR(name, type) if (strcmp(ft, name) == 0) memset(bd->fs_ctx, 0, sizeof(type))
                CLR("fat16", fat16_t); else CLR("fat32", fat32_t); else CLR("exfat", exfat_t);
                else CLR("ext2", ext2_t); else CLR("ext3", ext3_t); else CLR("ext4", ext4_t);
                else CLR("btrfs", btrfs_t); else CLR("xfs", xfs_t); else CLR("zfs", zfs_t);
                else CLR("apfs", apfs_t);
#undef CLR
            }
            blockdev_flush(bd);
            bd->mounted = 0;
            bd->mount_point[0] = 0;
            bd->fs_type[0] = 0;
            bd->fs_ctx = 0;
            break;
        }
    }
    return 0;
}

int diskops_format(const char *name, const char *fstype, char *err, int err_len)
{
    if (!auth_is_root()) { seterr(err, err_len, "permission denied (needs root; try sudo)"); return -1; }
    log3("diskops: formatting ", name, " as...");
    klog_write(fstype);
    klog_write("\n");

    blockdev_t *bd = blockdev_find(name);
    if (!bd) { seterr(err, err_len, "no such device"); return -1; }
    if (is_startup_dev(bd)) { seterr(err, err_len, "the startup disk can't be erased"); return -1; }
    if (bd->mounted) { seterr(err, err_len, "cannot format a mounted device, umount first"); return -1; }
    blockdev_cache_invalidate(bd);

    if (strcmp(fstype, "tfsk") == 0) {
        if (bd->type != BLOCKDEV_ATA) { seterr(err, err_len, "tfsk is currently only supported on ATA devices"); return -1; }
        if (tfsk_format((ata_device_t *)bd->driver_data, bd->total_sectors / 8, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "fat16") == 0) {
        if (fat16_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "fat32") == 0) {
        if (fat32_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "exfat") == 0) {
        if (exfat_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ext2") == 0) {
        if (ext4_mkfs(bd, 2, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ext3") == 0) {
        if (ext4_mkfs(bd, 3, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ext4") == 0) {
        if (ext4_mkfs(bd, 4, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ntfs") == 0) {
        if (ntfs_format(bd, "tOS") != 0) { seterr(err, err_len, "formatting NTFS is not supported; create the volume with mkfs.ntfs or Windows"); return -1; }
    } else if (strcmp(fstype, "btrfs") == 0) {
        if (btrfs_format(bd, "tOS") != 0) { seterr(err, err_len, "btrfs format failed (device too small? needs 48 MiB)"); return -1; }
    } else if (strcmp(fstype, "xfs") == 0) {
        if (xfs_format(bd, "tOS") != 0) { seterr(err, err_len, "xfs format failed (device too small? needs 16 MiB)"); return -1; }
    } else if (strcmp(fstype, "zfs") == 0) {
        if (zfs_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "apfs") == 0) {
        if (apfs_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else {
        seterr(err, err_len, "unsupported filesystem type");
        return -1;
    }

    log3("diskops: formatted ", name, " OK");

    /* Read back the freshly-written boot sector / superblock area so
     * the operation log shows real evidence of what landed on disk,
     * not just "it returned 0" — the same kind of hex dump `hexdump`
     * gives for files, but for the raw sector a format just wrote. */
    uint8_t sector0[512];
    if (blockdev_read(bd, 0, 1, sector0) == 0) {
        klog_write_hex("diskops: sector 0 after format:", sector0, 128);
    }

    return 0;
}

int diskops_resize(const char *name, uint32_t size_mb, char *err, int err_len)
{
    if (!auth_is_root()) { seterr(err, err_len, "permission denied (needs root; try sudo)"); return -1; }
    blockdev_t *bd = blockdev_find(name);
    if (!bd) { seterr(err, err_len, "no such device"); return -1; }
    if (bd->mounted) { seterr(err, err_len, "umount the device before resizing it"); return -1; }
    blockdev_cache_invalidate(bd);

    uint64_t dev_bytes = bd->total_sectors * (uint64_t)bd->sector_size;
    uint64_t want_bytes = size_mb ? (uint64_t)size_mb * 1024 * 1024 : dev_bytes;
    if (want_bytes > dev_bytes) { seterr(err, err_len, "requested size is larger than the device"); return -1; }
    uint64_t sectors512 = want_bytes / 512;

    const char *type = diskops_detect(name);
    if (!type) { seterr(err, err_len, "unknown filesystem"); return -1; }
    int rc;
    if (strcmp(type, "ntfs") == 0) rc = ntfs_resize(bd, sectors512, err, err_len);
    else if (!strcmp(type, "fat32") || !strcmp(type, "fat16")) rc = fat32_grow(bd, want_bytes, err, err_len);
    else if (!strcmp(type, "btrfs")) rc = btrfs_grow(bd, want_bytes, err, err_len);
    else if (!strcmp(type, "exfat")) rc = exfat_grow(bd, want_bytes, err, err_len);
    else if (!strcmp(type, "ext2") || !strcmp(type, "ext3") || !strcmp(type, "ext4")) {
        char e2[100] = "";
        rc = ext4_grow(bd, want_bytes, e2, sizeof(e2));
        if (rc) seterr(err, err_len, e2[0] ? e2 : "ext grow failed");
    }
    else { seterr(err, err_len, "resizing is not implemented for this filesystem"); rc = -1; }
    blockdev_cache_invalidate(bd);
    return rc;
}

/* ------------------------------------------------------------------ usage and First Aid */

/* free clusters of a FAT volume, counted from the table itself; remembered for a few seconds
 * because Disk Utility asks on every repaint and a big FAT is many sectors */
static struct { blockdev_t *bd; uint32_t free, total, ms; } fat_cache[4];

static int fat_count(blockdev_t *bd, int is32, uint32_t fat_lba, uint32_t clusters, uint32_t *free_out, uint32_t *bad_out)
{
    static uint8_t buf[8 * 512];       /* eight sectors a read: a USB disk answers each read slowly */
    uint32_t per = is32 ? 128 : 256, free = 0, bad = 0, max = clusters + 1;
    for (uint32_t c = 2, cached = 0xFFFFFFFF; c <= max; c++) {
        uint32_t s = c / per;
        if (s / 8 != cached) { if (blockdev_read(bd, fat_lba + (s / 8) * 8, 8, buf) != 0) return -1; cached = s / 8; }
        uint8_t *sec = buf + (s % 8) * 512;
        uint32_t i = c % per;
        uint32_t v = is32 ? (((uint32_t)sec[i * 4] | ((uint32_t)sec[i * 4 + 1] << 8) | ((uint32_t)sec[i * 4 + 2] << 16) | ((uint32_t)sec[i * 4 + 3] << 24)) & 0x0FFFFFFF)
                          : ((uint32_t)sec[i * 2] | ((uint32_t)sec[i * 2 + 1] << 8));
        if (v == 0) { free++; continue; }
        uint32_t eoc = is32 ? 0x0FFFFFF8 : 0xFFF8, badmark = is32 ? 0x0FFFFFF7 : 0xFFF7;
        /* a link must point at a cluster that exists, or end the chain */
        if (v != badmark && v < eoc && (v < 2 || v > max)) bad++;
    }
    *free_out = free;
    if (bad_out) *bad_out = bad;
    return 0;
}

int diskops_usage(const char *name, uint64_t *total, uint64_t *used)
{
    blockdev_t *bd = blockdev_find(name);
    if (!bd || !bd->mounted || !bd->fs_ctx) return -1;
    const char *ft = bd->fs_type;
    if (!strcmp(ft, "tfsk")) {
        tfsk_t *fs = (tfsk_t *)bd->fs_ctx;
        *total = fs->sb.total_blocks * TFSK_BLOCK_SIZE;
        *used = (fs->sb.total_blocks - fs->sb.free_blocks) * TFSK_BLOCK_SIZE;
        return 0;
    }
    if (!strcmp(ft, "ext4")) {
        ext4_t *fs = (ext4_t *)bd->fs_ctx;
        *total = (uint64_t)fs->blocks_count * fs->block_size;
        *used = (uint64_t)(fs->blocks_count - fs->free_blocks_count) * fs->block_size;
        return 0;
    }
    if (!strcmp(ft, "fat32") || !strcmp(ft, "fat16")) {
        int is32 = !strcmp(ft, "fat32");
        uint32_t spc, data, totsec, res, bps;
        if (is32) { fat32_t *f = bd->fs_ctx; spc = f->sectors_per_cluster; data = f->data_start_sector; totsec = f->total_sectors; res = f->reserved_sectors; bps = f->bytes_per_sector; }
        else      { fat16_t *f = bd->fs_ctx; spc = f->sectors_per_cluster; data = f->data_start_sector; totsec = f->total_sectors; res = f->reserved_sectors; bps = f->bytes_per_sector; }
        if (!spc || bps != 512 || totsec <= data) return -1;
        uint32_t clusters = (totsec - data) / spc;
        uint32_t now = debugmon_uptime_ms(), free = 0xFFFFFFFF;
        /* FAT32 keeps a free count in its FSInfo sector, valid until the driver first changes the
         * table (it marks it unknown then) */
        if (is32) {
            fat32_t *f = bd->fs_ctx;
            static uint8_t fi[512];
            if (f->fsinfo_sector && !f->fsinfo_invalid && blockdev_read(bd, f->fsinfo_sector, 1, fi) == 0 &&
                fi[0] == 0x52 && fi[1] == 0x52 && fi[2] == 0x61 && fi[3] == 0x41) {
                uint32_t v = (uint32_t)fi[488] | ((uint32_t)fi[489] << 8) | ((uint32_t)fi[490] << 16) | ((uint32_t)fi[491] << 24);
                if (v <= clusters) free = v;
            }
        }
        int slot = -1;
        for (int i = 0; i < 4; i++) if (fat_cache[i].bd == bd) slot = i;
        if (free != 0xFFFFFFFF) { }
        else if (slot >= 0 && now - fat_cache[slot].ms < 60000) free = fat_cache[slot].free;
        else {
            if (fat_count(bd, is32, res, clusters, &free, 0) != 0) return -1;
            if (slot < 0) { slot = 0; for (int i = 0; i < 4; i++) if (!fat_cache[i].bd) { slot = i; break; } }
            fat_cache[slot].bd = bd; fat_cache[slot].free = free; fat_cache[slot].ms = now;
        }
        *total = (uint64_t)clusters * spc * 512;
        *used = (uint64_t)(clusters - free) * spc * 512;
        return 0;
    }
    return -1;
}

static void rep(char *r, int len, const char *s)
{
    int l = (int)strlen(r);
    while (*s && l < len - 1) r[l++] = *s++;
    r[l] = 0;
}

static void rep_num(char *r, int len, uint32_t v)
{
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[12]; int k = 0;
    while (n) o[k++] = b[--n];
    o[k] = 0;
    rep(r, len, o);
}

/* First Aid: checks what can be checked without changing anything and writes what it did, line
 * by line, into report. 0 = the volume looks right, 1 = problems found, -1 = it could not be read. */
int diskops_first_aid(const char *name, char *report, int len)
{
    report[0] = 0;
    blockdev_t *bd = blockdev_find(name);
    if (!bd) { rep(report, len, "No such device.\n"); return -1; }
    rep(report, len, "Running First Aid on \xE2\x80\x9C"); rep(report, len, name); rep(report, len, "\xE2\x80\x9D\n");
    static uint8_t sec[512];
    rep(report, len, "Checking that the device can be read.\n");
    uint64_t probes[4] = { 0, bd->total_sectors / 3, bd->total_sectors * 2 / 3, bd->total_sectors - 1 };
    for (int i = 0; i < 4; i++)
        if (blockdev_read(bd, probes[i], 1, sec) != 0) { rep(report, len, "Sector "); rep_num(report, len, (uint32_t)probes[i]); rep(report, len, " could not be read.\nFirst Aid failed: the device has read errors.\n"); return -1; }
    if (!bd->mounted || !bd->fs_ctx) {
        const char *fs = diskops_detect(name);
        rep(report, len, fs ? "The volume is not mounted; only the device was checked.\n" : "No file system was found on the device.\n");
        rep(report, len, "Operation successful.\n");
        return 0;
    }
    const char *ft = bd->fs_type;
    int problems = 0;
    rep(report, len, "Checking the "); rep(report, len, ft); rep(report, len, " file system.\n");
    if (!strcmp(ft, "fat32") || !strcmp(ft, "fat16")) {
        int is32 = !strcmp(ft, "fat32");
        uint32_t spc, data, totsec, res, nf, fsz;
        if (is32) { fat32_t *f = bd->fs_ctx; spc = f->sectors_per_cluster; data = f->data_start_sector; totsec = f->total_sectors; res = f->reserved_sectors; nf = f->num_fats; fsz = f->fat_sectors; }
        else      { fat16_t *f = bd->fs_ctx; spc = f->sectors_per_cluster; data = f->data_start_sector; totsec = f->total_sectors; res = f->reserved_sectors; nf = f->num_fats; fsz = f->fat_sectors; }
        uint32_t clusters = (totsec - data) / spc, free = 0, bad = 0;
        rep(report, len, "Checking the file allocation table ("); rep_num(report, len, clusters); rep(report, len, " clusters).\n");
        if (fat_count(bd, is32, res, clusters, &free, &bad) != 0) { rep(report, len, "The table could not be read.\n"); return -1; }
        if (bad) { rep_num(report, len, bad); rep(report, len, " cluster links point outside the volume.\n"); problems++; }
        if (nf > 1) {
            rep(report, len, "Comparing the two copies of the table.\n");
            static uint8_t s2[512];
            uint32_t diff = 0;
            for (uint32_t i = 0; i < fsz && i < 4096; i++) {
                if (blockdev_read(bd, res + i, 1, sec) != 0 || blockdev_read(bd, res + fsz + i, 1, s2) != 0) break;
                if (memcmp(sec, s2, 512)) diff++;
            }
            if (diff) { rep_num(report, len, diff); rep(report, len, " sectors differ between the copies.\n"); problems++; }
        }
        rep_num(report, len, clusters - free); rep(report, len, " clusters in use, "); rep_num(report, len, free); rep(report, len, " free.\n");
    } else if (!strcmp(ft, "tfsk")) {
        tfsk_t *fs = bd->fs_ctx;
        rep(report, len, "Checking the superblock.\n");
        if (fs->sb.magic != TFSK_MAGIC) { rep(report, len, "The superblock has the wrong signature.\n"); problems++; }
        if (fs->sb.free_blocks > fs->sb.total_blocks || fs->sb.free_inodes > fs->sb.total_inodes) { rep(report, len, "The free counts are larger than the volume.\n"); problems++; }
        if (fs->sb.state == TFSK_STATE_ERROR) { rep(report, len, "The volume is marked as having errors.\n"); problems++; }
        rep_num(report, len, fs->sb.total_inodes - fs->sb.free_inodes); rep(report, len, " files and folders.\n");
    } else if (!strcmp(ft, "ext4")) {
        ext4_t *fs = bd->fs_ctx;
        rep(report, len, "Checking the superblock and block groups.\n");
        if (fs->free_blocks_count > fs->blocks_count || fs->free_inodes_count > fs->inodes_count) { rep(report, len, "The free counts are larger than the volume.\n"); problems++; }
        rep_num(report, len, fs->num_groups); rep(report, len, " block groups.\n");
    } else rep(report, len, "This file system has no checks yet; the device reads correctly.\n");
    if (problems) { rep(report, len, "First Aid found problems on the volume. Back up its files and erase it.\n"); return 1; }
    rep(report, len, "The volume appears to be OK.\nOperation successful.\n");
    return 0;
}
