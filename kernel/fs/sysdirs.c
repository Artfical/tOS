#include "sysdirs.h"
#include "vfs.h"
#include "fsbridge.h"
#include "string.h"
#include "memory.h"
#include "klog.h"

static void ensure_dir(const char *path, uint32_t mode)
{
    if (!vfs_exists(path)) vfs_mkdir(path, mode);
    vfs_chown(path, 0, 0);
    vfs_chmod(path, mode);
}

/* programs shipped in the initrd are copied into /system/programs once */
static void seed_programs(void)
{
    int n;
    vfs_entry_t *list = fsbridge_list_all("/programs", &n);
    if (!list) return;
    for (int i = 0; i < n; i++) {
        if (list[i].is_dir || list[i].name[0] == '.') continue;
        char src[VFS_NAME_LEN], dst[VFS_NAME_LEN];
        strcpy(src, "/programs/");
        strncpy(src + strlen(src), list[i].name, sizeof(src) - strlen(src) - 1);
        strcpy(dst, "/system/programs/");
        strncpy(dst + strlen(dst), list[i].name, sizeof(dst) - strlen(dst) - 1);
        if (vfs_exists(dst)) continue;
        uint32_t sz = list[i].size;
        char *buf = (char *)malloc(sz ? sz : 1);
        if (!buf) continue;
        int got = sz ? fsbridge_read(src, buf, sz, 0) : 0;
        if (got >= 0 && fsbridge_create(dst) == 0) {
            if (got > 0) fsbridge_write(dst, buf, (uint32_t)got, 0);
            vfs_chown(dst, 0, 0);
            vfs_chmod(dst, 0755);
        }
        free(buf);
    }
    free(list);
}

void sysdirs_setup(int have_disk)
{
    /* programs are copied in once: when the system folder is first made, or on every live boot */
    int seed = !have_disk || !vfs_exists("/mnt/system/programs");
    if (have_disk) {
        ensure_dir("/mnt/system", 0755);
        ensure_dir("/mnt/system/programs", 0755);
        ensure_dir("/mnt/home", 0755);
        ensure_dir("/mnt/etc", 0755);
        ensure_dir("/mnt/root", 0700);
        vfs_bind("/system", "/mnt/system");
        vfs_bind("/home", "/mnt/home");
        vfs_bind("/etc", "/mnt/etc");
        vfs_bind("/root", "/mnt/root");
    } else {
        ensure_dir("/system", 0755);
        ensure_dir("/system/programs", 0755);
        ensure_dir("/home", 0755);
        ensure_dir("/etc", 0755);
        ensure_dir("/root", 0700);
    }
    ensure_dir("/tmp", 01777);
    vfs_chown("/programs", 0, 0);
    if (seed) seed_programs();
    klog_write("sysdirs: /system /etc /home /root ready\n");
}
