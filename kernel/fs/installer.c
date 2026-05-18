#include "installer.h"
#include "terminal.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"
#include "memory.h"
#include "stdio.h"
#include "tfsk.h"
#include "ramfs.h"
#include "vfs.h"

static uint64_t sectors_to_mb(uint64_t sectors)
{
    return (sectors * 512) / (1024 * 1024);
}

typedef struct {
    int index;
    ata_device_t *dev;
    uint64_t total_blocks;
} disk_entry_t;

int installer_check_installed(void)
{
    for (int i = 0; i < ata_device_count; i++) {
        ata_device_t *dev = &ata_devices[i];
        uint8_t buf[TFSK_BLOCK_SIZE];
        if (ata_read_sectors(dev, TFSK_SB_BLK * 8, 8, buf) == 0) {
            tfsk_superblock_t *sb = (tfsk_superblock_t *)buf;
            if (sb->magic == TFSK_MAGIC) {
                return 1;
            }
        }
    }
    return 0;
}

int installer_run(void)
{
    terminal_setcolor(0x07);
    terminal_clear();

    int y = 0;
    terminal_setpos(0, y++);
    terminal_writestring("============================================");
    terminal_setpos(0, y++);
    terminal_writestring("         tOS Installation Wizard");
    terminal_setpos(0, y++);
    terminal_writestring("============================================");
    y++;

    if (ata_device_count == 0) {
        terminal_setpos(0, y++);
        terminal_writestring("ERROR: No disk found!");
        terminal_setpos(0, y++);
        terminal_writestring("At least one IDE disk is required for installation.");
        y++;
        terminal_setpos(0, y++);
        terminal_writestring("Installation cancelled. Booting with ramfs only.");
        y++;
        terminal_setpos(0, y++);
        terminal_writestring("Press any key to continue...");
        keyboard_getchar();
        return -1;
    }

    terminal_setpos(0, y++);
    terminal_writestring("Available disks:");
    y++;

    disk_entry_t disks[4];
    int disk_count = 0;

    for (int i = 0; i < ata_device_count; i++) {
        ata_device_t *dev = &ata_devices[i];
        uint64_t total_sectors = dev->lba48 ? dev->sectors_48 : (uint64_t)dev->sectors_28;
        uint64_t total_mb = sectors_to_mb(total_sectors);
        uint64_t tfs_blocks = total_sectors / 8;

        char line[80];
        int pos = 0;
        line[pos++] = '0' + disk_count;
        line[pos++] = ')';
        line[pos++] = ' ';
        for (int j = 0; dev->model[j] && j < 40; j++)
            line[pos++] = dev->model[j];
        pos += sprintf(line + pos, "  -  %d MB", (int)total_mb);
        if (tfs_blocks > 0) {
            pos += sprintf(line + pos, "  (%d tFS blocks)", (int)tfs_blocks);
        }
        line[pos] = 0;

        terminal_setpos(0, y++);
        terminal_writestring(line);

        disks[disk_count].index = i;
        disks[disk_count].dev = dev;
        disks[disk_count].total_blocks = tfs_blocks;
        disk_count++;

        if (disk_count >= 4) break;
    }

    y++;
    terminal_setpos(0, y++);
    terminal_writestring("Install tOS? [y/N]: ");

    char ans = keyboard_getchar();
    terminal_putchar(ans);
    if (ans != 'Y' && ans != 'y') {
        terminal_setpos(0, y++);
        terminal_writestring("Installation cancelled. Booting with ramfs only.");
        y++;
        terminal_setpos(0, y++);
        terminal_writestring("Press any key to continue...");
        keyboard_getchar();
        return -1;
    }

    y++;

    int selected = 0;
    if (disk_count > 1) {
        terminal_setpos(0, y++);
        terminal_writestring("Select disk to install (0-");
        terminal_putchar('0' + disk_count - 1);
        terminal_writestring("): ");

        for (;;) {
            char c = keyboard_getchar();
            if (c >= '0' && c < '0' + disk_count) {
                selected = c - '0';
                terminal_putchar(c);
                break;
            }
        }
        y++;
    }

    terminal_setpos(0, y++);
    terminal_writestring("Formatting disk...");

    ata_device_t *disk = disks[selected].dev;
    uint64_t blocks = disks[selected].total_blocks;

    if (tfsk_format(disk, blocks, "tOS Root") < 0) {
        terminal_setpos(0, y++);
        terminal_writestring("ERROR: Failed to format disk!");
        return -1;
    }

    terminal_setpos(0, y++);
    terminal_writestring("Disk formatted successfully.");

    tfsk_t fs;
    if (tfsk_mount(&fs, disk) < 0) {
        terminal_setpos(0, y++);
        terminal_writestring("ERROR: Failed to mount formatted disk!");
        return -1;
    }

    terminal_setpos(0, y++);
    terminal_writestring("Creating directory structure...");

    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "bin");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "etc");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "home");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "mnt");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "usr");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "tmp");

    terminal_setpos(0, y++);
    terminal_writestring("Copying files...");

    vfs_entry_t entries[64];
    int copied = 0;
    const char *src_dirs[] = { "/programs", "/bin", 0 };

    for (int di = 0; src_dirs[di]; di++) {
        int n = vfs_readdir(src_dirs[di], entries, 64);
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) {
            if (entries[i].is_dir) continue;

            char src_path[VFS_NAME_LEN];
            int sp = 0;
            const char *sd = src_dirs[di];
            while (*sd) src_path[sp++] = *sd++;
            src_path[sp++] = '/';
            int nk = 0;
            while (entries[i].name[nk] && sp < VFS_NAME_LEN - 1)
                src_path[sp++] = entries[i].name[nk++];
            src_path[sp] = 0;

            uint8_t *buf = malloc(entries[i].size);
            if (!buf) continue;

            int fd = vfs_open(src_path, 0);
            if (fd < 0) { free(buf); continue; }

            int rd = vfs_read(fd, buf, entries[i].size);
            vfs_close(fd);

            if (rd > 0) {
                int fd2 = tfsk_vfs_open(&fs, entries[i].name, 2);
                if (fd2 >= 0) {
                    tfsk_vfs_write(&fs, fd2, buf, rd);
                    tfsk_vfs_close(&fs, fd2);
                    copied++;
                }
            }
            free(buf);
        }
    }

    char done_msg[80];
    int dlen = sprintf(done_msg, "%d files copied.", copied);
    done_msg[dlen] = 0;
    terminal_setpos(0, y++);
    terminal_writestring(done_msg);

    tfsk_umount(&fs);

    terminal_setpos(0, y++);
    terminal_writestring("============================================");
    terminal_setpos(0, y++);
    terminal_writestring("  tOS installation complete!");
    terminal_setpos(0, y++);
    terminal_writestring("  Reboot to start using tOS.");
    terminal_setpos(0, y++);
    terminal_writestring("============================================");
    y++;
    terminal_setpos(0, y++);
    terminal_writestring("Press any key to continue...");
    keyboard_getchar();

    return 0;
}
