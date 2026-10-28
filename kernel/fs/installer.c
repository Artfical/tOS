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
#include "auth.h"
#include "fsbridge.h"
#include "klog.h"

static void strncat_safe(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(dst);
    while (*src && n + 1 < cap) dst[n++] = *src++;
    dst[n] = 0;
}

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

    /* the system's own files live under /system (programs inside), owned by root */
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "system");
    {
        uint32_t sys_ino;
        if (tfsk_walk(&fs, "/system", &sys_ino) == 0) tfsk_mkdir(&fs, sys_ino, "programs");
    }
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "etc");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "home");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "root");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "mnt");
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "tmp");

    terminal_setpos(0, y++);
    terminal_writestring("Copying files...");

    /* on the heap: kernel stacks are 32 KB, and a name is 256 bytes */
    vfs_entry_t *entries = malloc(sizeof(vfs_entry_t) * 64);
    if (!entries) return -1;
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
                char dst_path[VFS_NAME_LEN];
                strcpy(dst_path, "/system/programs/");
                strncat_safe(dst_path, entries[i].name, sizeof(dst_path));
                int fd2 = tfsk_vfs_open(&fs, dst_path, 2);
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
    terminal_writestring("  Next: choose a computer name, a root password and your user account.");
    terminal_setpos(0, y++);
    terminal_writestring("============================================");
    y++;
    terminal_setpos(0, y++);
    terminal_writestring("Press any key to continue...");
    keyboard_getchar();

    { free(entries); return 0; }
    free(entries);
}

/* A valid computer name: letters, digits and '-' only. */
static int hostname_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 30) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

static int ask_new_password(const char *what, char *out, int max)
{
    for (;;) {
        char again[64];
        char prompt[64];
        strcpy(prompt, what);
        strcat(prompt, " password: ");
        if (auth_prompt_password(prompt, out, max) < 0) continue;
        if (out[0] == 0) { terminal_writestring("The password cannot be empty.\n"); continue; }
        if (auth_prompt_password("Retype password: ", again, sizeof(again)) < 0) continue;
        if (strcmp(out, again) != 0) { terminal_writestring("The passwords do not match, try again.\n"); continue; }
        memset(again, 0, sizeof(again));
        return 0;
    }
}

/* The account questions of a fresh installation, in the way Linux installers ask them:
 * computer name, root password, then the first user (who may use sudo). */
int installer_setup_accounts(void)
{
    terminal_writestring("\n=== tOS first-time setup ===\n");
    char host[40], pw[64], user[AUTH_NAME_MAX], full[48];

    for (;;) {
        if (auth_read_line("Computer name [tos]: ", host, sizeof(host), 1) < 0) continue;
        if (host[0] == 0) strcpy(host, "tos");
        if (hostname_ok(host)) break;
        terminal_writestring("Use only lowercase letters, digits and '-'.\n");
    }
    auth_set_hostname(host);

    terminal_writestring("\nSet the password of the administrator account (root).\n");
    ask_new_password("Root", pw, sizeof(pw));
    auth_set_root_password(pw);
    memset(pw, 0, sizeof(pw));

    terminal_writestring("\nNow create your own user account.\n");
    for (;;) {
        if (auth_read_line("Username: ", user, sizeof(user), 1) < 0) continue;
        if (user[0] == 0) continue;
        if (auth_lookup(user, 0) == 0) { terminal_writestring("That name is taken.\n"); continue; }
        auth_read_line("Full name (optional): ", full, sizeof(full), 1);
        ask_new_password("User", pw, sizeof(pw));
        uint32_t uid;
        if (auth_add_user(user, pw, full[0] ? full : user, 1, &uid) == 0) break;
        terminal_writestring("Could not create that account (use lowercase letters, digits, '_' or '-', starting with a letter).\n");
    }
    memset(pw, 0, sizeof(pw));
    terminal_writestring("\nAccounts created. This user may use sudo.\n");
    return 0;
}

/* ------------------------------------------------------------------ the graphical installer's steps */
#include "sysdirs.h"

static void say(void (*progress)(int, const char *), int pct, const char *what)
{
    klog_write("installer: ");
    klog_write(what);
    klog_write("\n");
    if (progress) progress(pct, what);
}

/* Erases `disk` with a tFS called "tOS Root", lays out the system folders and copies the
 * programs on it. Reports along the way through progress (percent, what it is doing). */
int installer_install(ata_device_t *disk, void (*progress)(int pct, const char *what))
{
    uint64_t sectors = disk->lba48 ? disk->sectors_48 : (uint64_t)disk->sectors_28;
    say(progress, 5, "Preparing the disk\xE2\x80\xA6");
    if (tfsk_format(disk, sectors / 8, "tOS Root") < 0) { say(progress, 5, "The disk could not be erased."); return -1; }
    static tfsk_t fs;
    say(progress, 25, "Creating the file system\xE2\x80\xA6");
    if (tfsk_mount(&fs, disk) < 0) { say(progress, 25, "The new file system could not be opened."); return -1; }
    tfsk_mkdir(&fs, TFSK_ROOT_INODE, "system");
    uint32_t sys_ino;
    if (tfsk_walk(&fs, "/system", &sys_ino) == 0) tfsk_mkdir(&fs, sys_ino, "programs");
    static const char *const dirs[] = { "etc", "home", "root", "mnt", "tmp" };
    for (int i = 0; i < 5; i++) tfsk_mkdir(&fs, TFSK_ROOT_INODE, dirs[i]);

    /* the programs, from the installer's own file system */
    static vfs_entry_t entries[64];
    const char *src_dirs[] = { "/programs", "/bin", 0 };
    int total = 0, copied = 0;
    for (int di = 0; src_dirs[di]; di++) { int n = vfs_readdir(src_dirs[di], entries, 64); if (n > 0) total += n; }
    for (int di = 0; src_dirs[di]; di++) {
        int n = vfs_readdir(src_dirs[di], entries, 64);
        for (int i = 0; i < n; i++) {
            if (entries[i].is_dir) continue;
            char src[VFS_NAME_LEN], dst[VFS_NAME_LEN];
            strcpy(src, src_dirs[di]); strncat_safe(src, "/", sizeof(src)); strncat_safe(src, entries[i].name, sizeof(src));
            uint8_t *buf = malloc(entries[i].size ? entries[i].size : 1);
            if (!buf) continue;
            int fd = vfs_open(src, 0);
            int rd = fd >= 0 ? vfs_read(fd, buf, entries[i].size) : -1;
            if (fd >= 0) vfs_close(fd);
            if (rd > 0) {
                strcpy(dst, "/system/programs/"); strncat_safe(dst, entries[i].name, sizeof(dst));
                int fd2 = tfsk_vfs_open(&fs, dst, 2);
                if (fd2 >= 0) { tfsk_vfs_write(&fs, fd2, buf, rd); tfsk_vfs_close(&fs, fd2); copied++; }
            }
            free(buf);
            char what[64] = "Copying ";
            strncat_safe(what, entries[i].name, sizeof(what));
            say(progress, 30 + (total ? 50 * (copied + 1) / total : 50), what);
        }
    }
    tfsk_umount(&fs);
    say(progress, 82, "The system files are in place.");
    return 0;
}

static void write_file(const char *path, const char *text)
{
    int fd = vfs_open(path, 2);
    if (fd < 0) return;
    vfs_write(fd, text, (uint32_t)strlen(text));
    vfs_close(fd);
}

/* After installer_install(): mounts the new disk at /mnt as the system would at boot, and makes
 * the computer name, the administrator account (root gets the same password), the keyboard
 * layout and the account picture. */
int installer_finish(ata_device_t *disk, const char *host, const char *user, const char *full, const char *password,
                     const char *keyboard, int picture, void (*progress)(int pct, const char *what))
{
    static tfsk_t fs;
    say(progress, 86, "Setting up your account\xE2\x80\xA6");
    if (tfsk_mount(&fs, disk) < 0) return -1;
    tfsk_mount_vfs(&fs, "/mnt");
    sysdirs_setup(1);
    auth_init();
    auth_set_hostname(host);
    auth_set_root_password(password);
    uint32_t uid;
    if (auth_add_user(user, password, full[0] ? full : user, 1, &uid) != 0) { say(progress, 86, "The account could not be created."); return -1; }
    auth_save();
    write_file("/etc/keyboard", keyboard);
    auth_user_t u;
    if (auth_lookup(user, &u) == 0) {
        char path[96], n[4] = { (char)('0' + picture / 10), (char)('0' + picture % 10), '\n', 0 };
        strcpy(path, u.home);
        strncat_safe(path, "/.picture", sizeof(path));
        write_file(path, n);
        vfs_chown(path, u.uid, u.gid);
    }
    say(progress, 100, "tOS is installed.");
    return 0;
}
