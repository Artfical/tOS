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

int diskops_mount(const char *name, const char *mount_point, const char *fstype, char *err, int err_len)
{
    log3("diskops: mounting ", name, NULL);

    blockdev_t *bd = blockdev_find(name);
    if (!bd) { seterr(err, err_len, "no such device"); return -1; }
    if (bd->mounted) { seterr(err, err_len, "device already mounted"); return -1; }

    if (strcmp(fstype, "tfsk") == 0) {
        if (bd->type != BLOCKDEV_ATA) { seterr(err, err_len, "tfsk is currently only supported on ATA devices"); return -1; }
        static tfsk_t fs_instances[VFS_MAX_MOUNTS];
        static int fs_next = 0;
        if (fs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        tfsk_t *fs = &fs_instances[fs_next];
        memset(fs, 0, sizeof(*fs));
        fs->dev = (ata_device_t *)bd->driver_data;
        if (tfsk_probe_and_mount(fs) != 0) { seterr(err, err_len, "tfsk probe failed (not formatted?)"); return -1; }
        fs_next++;
        tfsk_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat16") == 0) {
        static fat16_t fat16_instances[VFS_MAX_MOUNTS];
        static int fat16_next = 0;
        if (fat16_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        fat16_t *fs = &fat16_instances[fat16_next];
        memset(fs, 0, sizeof(*fs));
        if (fat16_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "fat16 probe failed (not formatted?)"); return -1; }
        fat16_next++;
        fat16_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat32") == 0) {
        static fat32_t fat32_instances[VFS_MAX_MOUNTS];
        static int fat32_next = 0;
        if (fat32_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        fat32_t *fs = &fat32_instances[fat32_next];
        memset(fs, 0, sizeof(*fs));
        if (fat32_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "fat32 probe failed (not formatted?)"); return -1; }
        fat32_next++;
        fat32_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "exfat") == 0) {
        static exfat_t exfat_instances[VFS_MAX_MOUNTS];
        static int exfat_next = 0;
        if (exfat_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        exfat_t *fs = &exfat_instances[exfat_next];
        memset(fs, 0, sizeof(*fs));
        if (exfat_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "exfat probe failed (not formatted?)"); return -1; }
        exfat_next++;
        exfat_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext2") == 0) {
        static ext2_t ext2_instances[VFS_MAX_MOUNTS];
        static int ext2_next = 0;
        if (ext2_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        ext2_t *fs = &ext2_instances[ext2_next];
        memset(fs, 0, sizeof(*fs));
        if (ext2_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "ext2 probe failed (not formatted?)"); return -1; }
        ext2_next++;
        ext2_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext3") == 0) {
        static ext3_t ext3_instances[VFS_MAX_MOUNTS];
        static int ext3_next = 0;
        if (ext3_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        ext3_t *fs = &ext3_instances[ext3_next];
        memset(fs, 0, sizeof(*fs));
        if (ext3_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "ext3 probe failed (not formatted?)"); return -1; }
        ext3_next++;
        ext3_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext4") == 0) {
        static ext4_t ext4_instances[VFS_MAX_MOUNTS];
        static int ext4_next = 0;
        if (ext4_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        ext4_t *fs = &ext4_instances[ext4_next];
        memset(fs, 0, sizeof(*fs));
        if (ext4_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "ext4 probe failed (not formatted?)"); return -1; }
        ext4_next++;
        ext4_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ntfs") == 0) {
        static ntfs_t ntfs_instances[VFS_MAX_MOUNTS];
        static int ntfs_next = 0;
        if (ntfs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        ntfs_t *fs = &ntfs_instances[ntfs_next];
        memset(fs, 0, sizeof(*fs));
        if (ntfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "ntfs probe failed (not formatted?)"); return -1; }
        ntfs_next++;
        ntfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "btrfs") == 0) {
        static btrfs_t btrfs_instances[VFS_MAX_MOUNTS];
        static int btrfs_next = 0;
        if (btrfs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        btrfs_t *fs = &btrfs_instances[btrfs_next];
        memset(fs, 0, sizeof(*fs));
        if (btrfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "btrfs probe failed (not formatted?)"); return -1; }
        btrfs_next++;
        btrfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "xfs") == 0) {
        static xfs_t xfs_instances[VFS_MAX_MOUNTS];
        static int xfs_next = 0;
        if (xfs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        xfs_t *fs = &xfs_instances[xfs_next];
        memset(fs, 0, sizeof(*fs));
        if (xfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "xfs probe failed (not formatted?)"); return -1; }
        xfs_next++;
        xfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "zfs") == 0) {
        static zfs_t zfs_instances[VFS_MAX_MOUNTS];
        static int zfs_next = 0;
        if (zfs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        zfs_t *fs = &zfs_instances[zfs_next];
        memset(fs, 0, sizeof(*fs));
        if (zfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "zfs probe failed (not formatted?)"); return -1; }
        zfs_next++;
        zfs_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "apfs") == 0) {
        static apfs_t apfs_instances[VFS_MAX_MOUNTS];
        static int apfs_next = 0;
        if (apfs_next >= VFS_MAX_MOUNTS) { seterr(err, err_len, "too many mounted filesystems"); return -1; }
        apfs_t *fs = &apfs_instances[apfs_next];
        memset(fs, 0, sizeof(*fs));
        if (apfs_probe_and_mount(fs, bd) != 0) { seterr(err, err_len, "apfs probe failed (not formatted?)"); return -1; }
        apfs_next++;
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

    static tfsk_t  det_tfs;
    static fat32_t det_f32;
    static fat16_t det_f16;
    static exfat_t det_ef;
    static ext4_t  det_e4;
    static ext3_t  det_e3;
    static ext2_t  det_e2;
    static ntfs_t  det_nt;
    static btrfs_t det_btrfs;
    static xfs_t   det_xfs;
    static zfs_t   det_zfs;
    static apfs_t  det_apfs;

    if (bd->type == BLOCKDEV_ATA) {
        memset(&det_tfs, 0, sizeof(det_tfs));
        det_tfs.dev = (ata_device_t *)bd->driver_data;
        if (tfsk_probe_and_mount(&det_tfs) == 0) return "tfsk";
    }

    memset(&det_f32, 0, sizeof(det_f32));
    if (fat32_probe_and_mount(&det_f32, bd) == 0) return "fat32";

    memset(&det_f16, 0, sizeof(det_f16));
    if (fat16_probe_and_mount(&det_f16, bd) == 0) return "fat16";

    memset(&det_ef, 0, sizeof(det_ef));
    if (exfat_probe_and_mount(&det_ef, bd) == 0) return "exfat";

    memset(&det_e4, 0, sizeof(det_e4));
    if (ext4_probe_and_mount(&det_e4, bd) == 0) return "ext4";

    memset(&det_e3, 0, sizeof(det_e3));
    if (ext3_probe_and_mount(&det_e3, bd) == 0) return "ext3";

    memset(&det_e2, 0, sizeof(det_e2));
    if (ext2_probe_and_mount(&det_e2, bd) == 0) return "ext2";

    memset(&det_nt, 0, sizeof(det_nt));
    if (ntfs_probe_and_mount(&det_nt, bd) == 0) return "ntfs";

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
    log3("diskops: unmounting ", mount_point, NULL);

    if (vfs_unmount(mount_point) != 0) { seterr(err, err_len, "umount failed (not mounted, or busy)"); return -1; }
    int n = blockdev_count();
    for (int i = 0; i < n; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (bd && bd->mounted && strcmp(bd->mount_point, mount_point) == 0) {
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
    log3("diskops: formatting ", name, " as...");
    klog_write(fstype);
    klog_write("\n");

    blockdev_t *bd = blockdev_find(name);
    if (!bd) { seterr(err, err_len, "no such device"); return -1; }
    if (bd->mounted) { seterr(err, err_len, "cannot format a mounted device, umount first"); return -1; }

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
        if (ext2_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ext3") == 0) {
        if (ext3_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ext4") == 0) {
        if (ext4_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "ntfs") == 0) {
        if (ntfs_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "btrfs") == 0) {
        if (btrfs_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
    } else if (strcmp(fstype, "xfs") == 0) {
        if (xfs_format(bd, "tOS") != 0) { seterr(err, err_len, "format failed"); return -1; }
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
