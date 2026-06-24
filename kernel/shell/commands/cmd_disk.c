#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "stdio.h"
#include "vfs.h"
#include "blockdev.h"
#include "tfsk.h"
#include "fat16.h"
#include "fat32.h"
#include "exfat.h"
#include "ext2.h"
#include "ext3.h"

static const char *bd_type_name(blockdev_type_t t)
{
    switch (t) {
    case BLOCKDEV_ATA:     return "ATA";
    case BLOCKDEV_AHCI:    return "AHCI";
    case BLOCKDEV_NVME:    return "NVMe";
    case BLOCKDEV_USB_MSD: return "USB-MSD";
    default:               return "?";
    }
}

static uint32_t bd_size_mb(blockdev_t *bd)
{
    uint64_t bytes = bd->total_sectors * bd->sector_size;
    return (uint32_t)(bytes / (1024 * 1024));
}

static void print_disk_row(blockdev_t *bd)
{
    char line[160];
    snprintf(line, sizeof(line), "%-8s %-8s %6u MB  sect=%u  %s%s%s\n",
             bd->name, bd_type_name(bd->type), bd_size_mb(bd), bd->sector_size,
             bd->mounted ? "mounted@" : "unmounted",
             bd->mounted ? bd->mount_point : "",
             bd->mounted ? bd->fs_type : "");
    terminal_writestring(line);
}

static void disk_list(void)
{
    int n = blockdev_count();
    if (n == 0) {
        terminal_writestring("No block devices found.\n");
        return;
    }
    terminal_writestring("NAME     TYPE     SIZE          INFO\n");
    for (int i = 0; i < n; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (bd) print_disk_row(bd);
    }
}

static void disk_info(const char *name)
{
    blockdev_t *bd = blockdev_find(name);
    if (!bd) {
        terminal_writestring("disk: no such device: ");
        terminal_writestring(name);
        terminal_putchar('\n');
        return;
    }
    char line[160];
    terminal_writestring("Device:       "); terminal_writestring(bd->name); terminal_putchar('\n');
    terminal_writestring("Type:         "); terminal_writestring(bd_type_name(bd->type)); terminal_putchar('\n');
    snprintf(line, sizeof(line), "Sector size:  %u bytes\n", bd->sector_size);
    terminal_writestring(line);
    snprintf(line, sizeof(line), "Sectors:      %u\n", (uint32_t)bd->total_sectors);
    terminal_writestring(line);
    snprintf(line, sizeof(line), "Capacity:     %u MB\n", bd_size_mb(bd));
    terminal_writestring(line);
    if (bd->mounted) {
        terminal_writestring("Mounted at:   "); terminal_writestring(bd->mount_point); terminal_putchar('\n');
        terminal_writestring("Filesystem:   "); terminal_writestring(bd->fs_type); terminal_putchar('\n');
    } else {
        terminal_writestring("Status:       unmounted\n");
    }
}

static void disk_properties(void)
{
    int n = blockdev_count();
    uint64_t total_bytes = 0, used_bytes = 0;
    for (int i = 0; i < n; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (!bd) continue;
        total_bytes += bd->total_sectors * bd->sector_size;
    }
    char line[160];
    snprintf(line, sizeof(line), "Disks:        %d\n", n);
    terminal_writestring(line);
    snprintf(line, sizeof(line), "Total size:   %u MB\n", (uint32_t)(total_bytes / (1024 * 1024)));
    terminal_writestring(line);
    (void)used_bytes;
    disk_list();
}

static int do_mount(const char *name, const char *mount_point, const char *fstype)
{
    blockdev_t *bd = blockdev_find(name);
    if (!bd) {
        terminal_writestring("disk: no such device\n");
        return -1;
    }
    if (bd->mounted) {
        terminal_writestring("disk: device already mounted\n");
        return -1;
    }

    if (strcmp(fstype, "tfsk") == 0) {
        if (bd->type != BLOCKDEV_ATA) {
            terminal_writestring("disk: tfsk is currently only supported on ATA devices\n");
            return -1;
        }
        static tfsk_t fs_instances[VFS_MAX_MOUNTS];
        static int fs_next = 0;
        if (fs_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        tfsk_t *fs = &fs_instances[fs_next];
        memset(fs, 0, sizeof(*fs));
        fs->dev = (ata_device_t *)bd->driver_data;
        if (tfsk_probe_and_mount(fs) != 0) {
            terminal_writestring("disk: tfsk probe failed (not formatted?)\n");
            return -1;
        }
        fs_next++;
        tfsk_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat16") == 0) {
        static fat16_t fat16_instances[VFS_MAX_MOUNTS];
        static int fat16_next = 0;
        if (fat16_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        fat16_t *fs = &fat16_instances[fat16_next];
        memset(fs, 0, sizeof(*fs));
        if (fat16_probe_and_mount(fs, bd) != 0) {
            terminal_writestring("disk: fat16 probe failed (not formatted?)\n");
            return -1;
        }
        fat16_next++;
        fat16_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "fat32") == 0) {
        static fat32_t fat32_instances[VFS_MAX_MOUNTS];
        static int fat32_next = 0;
        if (fat32_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        fat32_t *fs = &fat32_instances[fat32_next];
        memset(fs, 0, sizeof(*fs));
        if (fat32_probe_and_mount(fs, bd) != 0) {
            terminal_writestring("disk: fat32 probe failed (not formatted?)\n");
            return -1;
        }
        fat32_next++;
        fat32_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "exfat") == 0) {
        static exfat_t exfat_instances[VFS_MAX_MOUNTS];
        static int exfat_next = 0;
        if (exfat_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        exfat_t *fs = &exfat_instances[exfat_next];
        memset(fs, 0, sizeof(*fs));
        if (exfat_probe_and_mount(fs, bd) != 0) {
            terminal_writestring("disk: exfat probe failed (not formatted?)\n");
            return -1;
        }
        exfat_next++;
        exfat_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext2") == 0) {
        static ext2_t ext2_instances[VFS_MAX_MOUNTS];
        static int ext2_next = 0;
        if (ext2_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        ext2_t *fs = &ext2_instances[ext2_next];
        memset(fs, 0, sizeof(*fs));
        if (ext2_probe_and_mount(fs, bd) != 0) {
            terminal_writestring("disk: ext2 probe failed (not formatted?)\n");
            return -1;
        }
        ext2_next++;
        ext2_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else if (strcmp(fstype, "ext3") == 0) {
        static ext3_t ext3_instances[VFS_MAX_MOUNTS];
        static int ext3_next = 0;
        if (ext3_next >= VFS_MAX_MOUNTS) {
            terminal_writestring("disk: too many mounted filesystems\n");
            return -1;
        }
        ext3_t *fs = &ext3_instances[ext3_next];
        memset(fs, 0, sizeof(*fs));
        if (ext3_probe_and_mount(fs, bd) != 0) {
            terminal_writestring("disk: ext3 probe failed (not formatted?)\n");
            return -1;
        }
        ext3_next++;
        ext3_mount_vfs(fs, mount_point);
        bd->fs_ctx = fs;
    } else {
        terminal_writestring("disk: unsupported filesystem type: ");
        terminal_writestring(fstype);
        terminal_putchar('\n');
        terminal_writestring("disk: supported types: tfsk, fat16, fat32, exfat, ext2, ext3 (ext4/NTFS coming soon)\n");
        return -1;
    }

    bd->mounted = 1;
    int i = 0;
    while (mount_point[i] && i < BLOCKDEV_MOUNT_LEN - 1) { bd->mount_point[i] = mount_point[i]; i++; }
    bd->mount_point[i] = 0;
    i = 0;
    while (fstype[i] && i < BLOCKDEV_FSTYPE_LEN - 1) { bd->fs_type[i] = fstype[i]; i++; }
    bd->fs_type[i] = 0;

    terminal_writestring("disk: mounted ");
    terminal_writestring(name);
    terminal_writestring(" at ");
    terminal_writestring(mount_point);
    terminal_putchar('\n');
    return 0;
}

static int do_umount(const char *mount_point)
{
    if (vfs_unmount(mount_point) != 0) {
        terminal_writestring("disk: umount failed (not mounted, or busy)\n");
        return -1;
    }
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
    terminal_writestring("disk: unmounted ");
    terminal_writestring(mount_point);
    terminal_putchar('\n');
    return 0;
}

static int do_format(const char *name, const char *fstype)
{
    blockdev_t *bd = blockdev_find(name);
    if (!bd) {
        terminal_writestring("disk: no such device\n");
        return -1;
    }
    if (bd->mounted) {
        terminal_writestring("disk: cannot format a mounted device, umount first\n");
        return -1;
    }

    if (strcmp(fstype, "tfsk") == 0) {
        if (bd->type != BLOCKDEV_ATA) {
            terminal_writestring("disk: tfsk is currently only supported on ATA devices\n");
            return -1;
        }
        if (tfsk_format((ata_device_t *)bd->driver_data, bd->total_sectors / 8, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else if (strcmp(fstype, "fat16") == 0) {
        if (fat16_format(bd, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else if (strcmp(fstype, "fat32") == 0) {
        if (fat32_format(bd, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else if (strcmp(fstype, "exfat") == 0) {
        if (exfat_format(bd, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else if (strcmp(fstype, "ext2") == 0) {
        if (ext2_format(bd, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else if (strcmp(fstype, "ext3") == 0) {
        if (ext3_format(bd, "tOS") != 0) {
            terminal_writestring("disk: format failed\n");
            return -1;
        }
    } else {
        terminal_writestring("disk: unsupported filesystem type: ");
        terminal_writestring(fstype);
        terminal_putchar('\n');
        terminal_writestring("disk: supported types: tfsk, fat16, fat32, exfat, ext2, ext3 (ext4/NTFS coming soon)\n");
        return -1;
    }

    terminal_writestring("disk: formatted ");
    terminal_writestring(name);
    terminal_writestring(" as ");
    terminal_writestring(fstype);
    terminal_putchar('\n');
    return 0;
}

static void disk_usage(void)
{
    terminal_writestring("Usage: disk <list|info|properties|mount|umount|format> [args]\n");
    terminal_writestring("  disk list\n");
    terminal_writestring("  disk info <device>\n");
    terminal_writestring("  disk properties\n");
    terminal_writestring("  disk mount <device> <mountpoint> <fstype>\n");
    terminal_writestring("  disk umount <mountpoint>\n");
    terminal_writestring("  disk format <device> <fstype>\n");
}

void cmd_disk(int argc, char **args)
{
    if (argc < 2) { disk_usage(); return; }

    const char *sub = args[1];
    if (strcmp(sub, "list") == 0) {
        disk_list();
    } else if (strcmp(sub, "info") == 0) {
        if (argc < 3) { disk_usage(); return; }
        disk_info(args[2]);
    } else if (strcmp(sub, "properties") == 0) {
        disk_properties();
    } else if (strcmp(sub, "mount") == 0) {
        if (argc < 5) { disk_usage(); return; }
        do_mount(args[2], args[3], args[4]);
    } else if (strcmp(sub, "umount") == 0) {
        if (argc < 3) { disk_usage(); return; }
        do_umount(args[2]);
    } else if (strcmp(sub, "format") == 0) {
        if (argc < 4) { disk_usage(); return; }
        do_format(args[2], args[3]);
    } else {
        disk_usage();
    }
}
