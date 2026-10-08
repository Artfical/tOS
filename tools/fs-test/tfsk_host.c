/* Hosted test for the tFS driver: formats a file as a tFS disk and runs VFS-level operations. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "vfs.h"
#include "ata.h"

ata_device_t ata_devices[ATA_MAX_DEVICES];
int ata_device_count;
static int g_fd;
static int g_trace_flags;

int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, void *buf) { (void)dev; return pread(g_fd, buf, count * 512, lba * 512) == (ssize_t)(count * 512) ? 0 : -1; }
int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, const void *buf) { (void)dev; return pwrite(g_fd, buf, count * 512, lba * 512) == (ssize_t)(count * 512) ? 0 : -1; }
int ata_flush(ata_device_t *dev) { (void)dev; return 0; }
void *kcalloc(size_t n, size_t m) { return calloc(n, m); }
void serial_write(const char *s) { fputs(s, stderr); }
uint32_t task_get_ticks(void) { return 1; }

static vfs_ops_t *g_ops; static void *g_ctx;
int vfs_mount(const char *p, vfs_ops_t *o, void *d) { (void)p; g_ops = o; g_ctx = d; return 0; }

#include "tfsk.h"
int main(int argc, char **argv)
{
    (void)g_trace_flags;
    g_fd = open(argv[1], O_RDWR);
    struct stat st; fstat(g_fd, &st);
    ata_device_count = 1; ata_devices[0].present = 1; ata_devices[0].sectors_28 = (int)(st.st_size / 512);
    strcpy(ata_devices[0].model, "FILE");
    if (tfsk_format(&ata_devices[0], (uint64_t)st.st_size / 4096, "test") < 0) { printf("format failed\n"); return 1; }
    static tfsk_t fs;
    if (tfsk_probe_and_mount(&fs) != 0) { printf("mount failed\n"); return 1; }
    tfsk_mount_vfs(&fs, "/mnt");
    printf("mkdir system -> %d\n", tfsk_mkdir(&fs, TFSK_ROOT_INODE, "system"));
    uint32_t ino = 0;
    printf("walk /system -> %d (ino %u)\n", tfsk_walk(&fs, "/system", &ino), ino);
    printf("walk system -> %d\n", tfsk_walk(&fs, "system", &ino));
    {
        uint32_t sys_ino = 0;
        int w = tfsk_walk(&fs, "/system", &sys_ino);
        printf("walk again -> %d ino %u\n", w, sys_ino);
        printf("mkdir programs -> %d\n", tfsk_mkdir(&fs, sys_ino, "programs"));
        uint32_t p2 = 0;
        printf("walk /system/programs -> %d\n", tfsk_walk(&fs, "/system/programs", &p2));
        int f2 = tfsk_vfs_open(&fs, "/system/programs/hello.t", 2);
        printf("open file in programs -> %d\n", f2);
        if (f2 >= 0) { tfsk_vfs_write(&fs, f2, "abc", 3); tfsk_vfs_close(&fs, f2); }
    }
    printf("vfs mkdir etc -> %d\n", g_ops->mkdir(g_ctx, "etc", 0755));
    int fd = g_ops->open(g_ctx, "etc/passwd", VFS_WRONLY | VFS_CREAT | VFS_TRUNC);
    printf("open etc/passwd -> %d\n", fd);
    if (fd >= 0) { printf("write -> %d\n", g_ops->write(g_ctx, fd, "hello\n", 6)); g_ops->close(g_ctx, fd); }
    vfs_entry_t e[16];
    int n = g_ops->readdir(g_ctx, "", e, 16);
    for (int i = 0; i < n; i++) printf("  [%s] dir=%d mode=%o uid=%u\n", e[i].name, e[i].is_dir, e[i].mode, e[i].uid);
    return 0;
}
