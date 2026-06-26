#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "stdio.h"
#include "vfs.h"
#include "blockdev.h"
#include "diskops.h"

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
    char err[80];
    if (diskops_mount(name, mount_point, fstype, err, sizeof(err)) != 0) {
        terminal_writestring("disk: ");
        terminal_writestring(err);
        terminal_putchar('\n');
        return -1;
    }
    terminal_writestring("disk: mounted ");
    terminal_writestring(name);
    terminal_writestring(" at ");
    terminal_writestring(mount_point);
    terminal_putchar('\n');
    return 0;
}

static int do_umount(const char *mount_point)
{
    char err[80];
    if (diskops_umount(mount_point, err, sizeof(err)) != 0) {
        terminal_writestring("disk: ");
        terminal_writestring(err);
        terminal_putchar('\n');
        return -1;
    }
    terminal_writestring("disk: unmounted ");
    terminal_writestring(mount_point);
    terminal_putchar('\n');
    return 0;
}

static int do_format(const char *name, const char *fstype)
{
    char err[80];
    if (diskops_format(name, fstype, err, sizeof(err)) != 0) {
        terminal_writestring("disk: ");
        terminal_writestring(err);
        terminal_putchar('\n');
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
