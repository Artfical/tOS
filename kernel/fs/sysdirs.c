#include "sysdirs.h"
#include "vfs.h"
#include "fsbridge.h"
#include "string.h"
#include "memory.h"
#include "klog.h"
#include "usermode.h"
#include "version.h"

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

/* The built-in applications. Each gets a small .t program in /system/apps that asks the
 * kernel (SYS_OPEN_APP) to open it, so the system folder lists everything that can be started
 * and typing the name in a shell runs that program. */
static const char *const app_names[] = {
    "terminal", "files", "notepad", "calculator", "clock", "paint", "viewer", "taskmgr", "mediaplayer",
    "netmon", "snake", "2048", "pdfviewer", "notes", "diskutil", "about", "doom", "wm", 0
};

/* mov eax,250 ; mov ebx,name ; int 0x80 ; mov eax,1 ; xor ebx,ebx ; int 0x80 ; "name",0 ; memory size */
static int write_launcher(const char *app)
{
    char path[VFS_NAME_LEN];
    strcpy(path, "/system/apps/");
    strcat(path, app);
    strcat(path, ".t");
    uint8_t img[96];
    uint32_t nl = (uint32_t)strlen(app) + 1;
    uint32_t addr = USER_CODE_BASE + 21;
    uint8_t code[21] = { 0xB8, 250, 0, 0, 0,  0xBB, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
                         0xCD, 0x80,  0xB8, 1, 0, 0, 0,  0x31, 0xDB,  0xCD, 0x80 };
    memcpy(img, code, 21);
    memcpy(img + 21, app, nl);
    uint32_t content = 21 + nl;
    img[content] = (uint8_t)content;
    img[content + 1] = (uint8_t)(content >> 8);
    img[content + 2] = 0;
    img[content + 3] = 0;
    if (fsbridge_create(path) != 0) return -1;
    if (fsbridge_write(path, img, content + 4, 0) < 0) return -1;
    vfs_chown(path, 0, 0);
    vfs_chmod(path, 0755);
    return 0;
}

static void install_system_layout(void)
{
    int fresh_apps = !vfs_exists("/system/apps");
    ensure_dir("/system/apps", 0755);
    ensure_dir("/system/kernel", 0755);
    ensure_dir("/system/config", 0755);
    if (fresh_apps) {
        for (int i = 0; app_names[i]; i++) write_launcher(app_names[i]);
    }
    if (!vfs_exists("/system/kernel/version")) {
        const char *v = TOS_BOOT_STRING "\n";
        if (fsbridge_create("/system/kernel/version") == 0) {
            fsbridge_write("/system/kernel/version", v, (uint32_t)strlen(v), 0);
            vfs_chown("/system/kernel/version", 0, 0);
            vfs_chmod("/system/kernel/version", 0644);
        }
    }
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
    install_system_layout();
    ensure_dir("/tmp", 01777);
    vfs_chown("/programs", 0, 0);
    if (seed) seed_programs();
    klog_write("sysdirs: /system /etc /home /root ready\n");
}
