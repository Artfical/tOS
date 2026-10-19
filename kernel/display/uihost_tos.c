#include "uihost.h"
#include "ramfs.h"
#include "wm.h"
#include "wm_internal.h"
#include "fsbridge.h"
#include "scheduler.h"
#include "memory.h"
#include "string.h"
#include "cmos.h"
#include "debugmon.h"
#include "auth.h"
#include "net.h"
#include "nic.h"
#include "webfetch.h"
#include "imgload.h"

/* uihost.h for tOS: the VFS, the scheduler and the network stack. */

uint32_t uihost_ms(void) { return debugmon_uptime_ms(); }

void uihost_time(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    cmos_time_t t;
    cmos_get_time(&t);
    *year = t.year; *month = t.month; *day = t.day;
    *hour = t.hour; *min = t.minute; *sec = t.second;
}

const char *uihost_home(void)
{
    static char home[64];
    uint32_t uid, gid;
    auth_session(&uid, &gid);
    auth_user_t u;
    if (auth_lookup_uid(uid, &u) == 0 && u.home[0] && fsbridge_is_dir(u.home)) strcpy(home, u.home);
    else strcpy(home, "/");
    return home;
}

int uihost_list(const char *dir, uihost_entry_t *out, int max)
{
    static vfs_entry_t ents[128];
    int n = fsbridge_list(dir, ents, max < 128 ? max : 128);
    if (n < 0) return -1;
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].name, ".") || !strcmp(ents[i].name, "..")) continue;
        strncpy(out[o].name, ents[i].name, sizeof(out[o].name) - 1);
        out[o].name[sizeof(out[o].name) - 1] = 0;
        out[o].size = ents[i].size;
        out[o].is_dir = ents[i].is_dir;
        o++;
    }
    return o;
}

int uihost_read(const char *path, uint8_t **data, uint32_t *len)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    uint32_t n = fsbridge_size(path);
    uint8_t *b = malloc(n + 1);
    if (!b) return -1;
    int got = n ? fsbridge_read(path, b, n, 0) : 0;
    if (got < 0) { free(b); return -1; }
    b[got] = 0;
    *data = b;
    *len = (uint32_t)got;
    return 0;
}

int uihost_write(const char *path, const void *data, uint32_t len)
{
    /* replace, not overwrite in place: a shorter file must not keep the old tail */
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0 && !fsbridge_exists(path)) return -1;
    if (!len) return 0;
    return fsbridge_write(path, data, len, 0) == (int)len ? 0 : -1;
}

int uihost_mkdir(const char *path) { return fsbridge_mkdir(path); }
int uihost_delete(const char *path) { return fsbridge_delete(path); }
int uihost_rename(const char *from, const char *to) { return fsbridge_rename(from, to); }

void uihost_open(const char *app_id, const char *path)
{
    if (wm_gfx_open_native(app_id, path) == 0) return;
    /* the apps that are still the classic character-cell ones */
    if (!strcmp(app_id, "pdf") && path) wm_open_pdfviewer_file(path);
    else if (!strcmp(app_id, "run") && path) {
        char cmd[64] = "run ";
        int o = 4;
        for (const char *q = path; *q && o < (int)sizeof(cmd) - 1; q++) cmd[o++] = *q;
        cmd[o] = 0;
        wm_open_terminal(cmd);
    } else if (!strcmp(app_id, "music")) wm_open_app("mediaplayer");
    else wm_open_app(app_id);
}

static uihost_task_t *t_out;
static int t_n, t_max;

static void task_cb(uint32_t pid, const char *name, uint32_t state)
{
    if (t_n >= t_max) return;
    t_out[t_n].pid = pid;
    strncpy(t_out[t_n].name, name, sizeof(t_out[t_n].name) - 1);
    t_out[t_n].name[sizeof(t_out[t_n].name) - 1] = 0;
    t_out[t_n].state = (int)state;
    t_out[t_n].cpu = task_get_cpu_ticks(pid);
    t_n++;
}

int uihost_tasks(uihost_task_t *out, int max)
{
    t_out = out; t_n = 0; t_max = max;
    task_foreach(task_cb);
    return t_n;
}

int uihost_kill(uint32_t pid)
{
    if (wm_kill_task_window(pid) == 0) return 0;
    return task_kill(pid);
}

void uihost_memory(uint32_t *total_kb, uint32_t *used_kb) { memory_get_usage(total_kb, used_kb); }

void uihost_net(uihost_net_t *n)
{
    memset(n, 0, sizeof(*n));
    n->up = nic_send != 0;
    n->ip = net_ip; n->gateway = net_gateway; n->dns = net_dns; n->netmask = net_netmask;
    memcpy(n->mac, net_mac, 6);
    strncpy(n->driver, nic_driver_name, sizeof(n->driver) - 1);
    n->rx_bytes = nic_rx_bytes; n->tx_bytes = nic_tx_bytes;
    n->rx_packets = nic_rx_packets; n->tx_packets = nic_tx_packets;
}

int uihost_net_start(void) { return nic_send ? 0 : -1; }    /* tOS brings the network up at boot */

void *uihost_fetch(const char *url) { return webfetch_start(url); }

int uihost_fetch_poll(void *h)
{
    webfetch_t *f = h;
    return !f ? -1 : f->state == WEBFETCH_RUNNING ? 0 : f->state == WEBFETCH_DONE ? 1 : -1;
}

const uint8_t *uihost_fetch_result(void *h, uint32_t *len, int *status, const char **ctype, const char **final_url, const char **error)
{
    webfetch_t *f = h;
    if (len) *len = f->body_len;
    if (status) *status = f->status;
    if (ctype) *ctype = f->ctype;
    if (final_url) *final_url = f->final_url[0] ? f->final_url : f->url;
    if (error) *error = f->error;
    return f->body;
}

void uihost_fetch_free(void *h) { webfetch_free(h); }

/* the size a PNG or JPEG says it has, before decoding it; 0 = can't tell (or a JPEG we can't decode) */
static int image_size(const uint8_t *d, uint32_t n, uint32_t *w, uint32_t *h)
{
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        *w = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        *h = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
        return 1;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        uint32_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { i++; continue; }
            uint8_t m = d[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += m == 0xFF ? 1 : 2; continue; }
            uint32_t l = (uint32_t)d[i + 2] << 8 | d[i + 3];
            if (m == 0xC0 || m == 0xC1) {
                *h = (uint32_t)d[i + 5] << 8 | d[i + 6];
                *w = (uint32_t)d[i + 7] << 8 | d[i + 8];
                return 1;
            }
            if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return 0;   /* progressive etc. */
            i += 2 + l;
        }
    }
    return 0;
}

int uihost_image_decode(const uint8_t *data, uint32_t len, int max_w, int max_h, uint8_t **rgba, int *w, int *h)
{
    uint32_t iw, ih;
    if (!image_size(data, len, &iw, &ih) || !iw || !ih || iw > 6000 || ih > 6000 || iw * ih > 1600000) return -1;
    uint8_t *px;
    if (img_decode(data, len, &px, &iw, &ih, 0) != 0) return -1;
    uint32_t tw = iw, th = ih;
    if (max_w > 0 && tw > (uint32_t)max_w) { th = th * (uint32_t)max_w / tw; tw = (uint32_t)max_w; }
    if (max_h > 0 && th > (uint32_t)max_h) { tw = tw * (uint32_t)max_h / th; th = (uint32_t)max_h; }
    if (!tw) tw = 1;
    if (!th) th = 1;
    if (tw != iw || th != ih) {
        uint8_t *sc = img_scale(px, iw, ih, tw, th);
        free(px);
        if (!sc) return -1;
        px = sc;
    }
    *rgba = px;
    *w = (int)tw;
    *h = (int)th;
    return 0;
}

const char *uihost_system_name(void) { return "tOS"; }

const char *uihost_hostname(void) { return auth_hostname(); }

/* /etc/passwd: name:x:uid:gid:gecos:home:shell */
int uihost_users(uihost_user_t *out, int max)
{
    uint8_t *d; uint32_t n;
    if (uihost_read("/etc/passwd", &d, &n) != 0) return 0;
    uint32_t sess_uid, sess_gid;
    auth_session(&sess_uid, &sess_gid);
    int count = 0;
    char *p = (char *)d, *end = p + n;
    while (p < end && count < max) {
        char *line = p;
        while (p < end && *p != '\n') p++;
        if (p < end) *p++ = 0; else *end = 0;
        char *f[7];
        int k = 0;
        f[k++] = line;
        for (char *q = line; *q && k < 7; q++) if (*q == ':') { *q = 0; f[k++] = q + 1; }
        if (k < 5) continue;
        uint32_t uid = 0;
        for (char *q = f[2]; *q >= '0' && *q <= '9'; q++) uid = uid * 10 + (uint32_t)(*q - '0');
        if (uid != 0 && uid < 1000) continue;             /* system accounts */
        strncpy(out[count].name, f[0], sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = 0;
        strncpy(out[count].full, f[4][0] ? f[4] : f[0], sizeof(out[count].full) - 1);
        out[count].full[sizeof(out[count].full) - 1] = 0;
        out[count].uid = uid;
        out[count].admin = uid == 0 || auth_can_sudo(uid);
        out[count].current = uid == sess_uid;
        count++;
    }
    free(d);
    return count;
}

int wm_gfx_take_authorization(void);

int uihost_add_user(const char *name, const char *password, const char *full, int admin)
{
    if (!wm_gfx_take_authorization()) return -1;
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    uint32_t uid;
    int r = auth_add_user(name, password, full, admin, &uid);
    if (r == 0) auth_save();
    auth_restore(&saved);
    return r;
}

int uihost_set_password(const char *name, const char *password)
{
    if (!wm_gfx_take_authorization()) return -1;
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    int r = auth_set_password(name, password);
    if (r == 0) auth_save();
    auth_restore(&saved);
    return r;
}

/* ---- removable disks ---- */
#include "volumes.h"

int uihost_volumes(uihost_volume_t *out, int max)
{
    volume_info_t v[VOLUMES_MAX];
    int n = volumes_list(v, max < VOLUMES_MAX ? max : VOLUMES_MAX);
    for (int i = 0; i < n; i++) {
        strcpy(out[i].name, v[i].name);
        strcpy(out[i].path, v[i].path);
        out[i].bytes = v[i].bytes;
    }
    return n;
}

int uihost_eject(const char *path) { return volumes_eject(path); }

/* ---- Disk Utility ---- */
#include "blockdev.h"
#include "diskops.h"
#include "tfsk.h"

int uihost_disks(uihost_disk_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < blockdev_count() && n < max; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (!bd) continue;
        uihost_disk_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        strncpy(d->dev, bd->name, sizeof(d->dev) - 1);
        d->size = bd->total_sectors * bd->sector_size;
        d->removable = bd->type == BLOCKDEV_USB_MSD;
        const char *conn = bd->type == BLOCKDEV_USB_MSD ? "USB" : bd->type == BLOCKDEV_NVME ? "PCI-Express" : bd->type == BLOCKDEV_AHCI ? "SATA" : "ATA";
        strcpy(d->connection, conn);
        if (bd->type == BLOCKDEV_ATA && bd->driver_data) {
            const char *m = ((ata_device_t *)bd->driver_data)->model;
            int l = 0;
            while (m[l] && l < 39) { d->model[l] = m[l]; l++; }
            while (l > 0 && d->model[l - 1] == ' ') l--;
            d->model[l] = 0;
        }
        if (!d->model[0]) strcpy(d->model, bd->type == BLOCKDEV_USB_MSD ? "USB Flash Disk" : bd->type == BLOCKDEV_NVME ? "NVMe SSD" : "Disk");
        d->mounted = bd->mounted;
        if (bd->mounted) {
            strncpy(d->fs, bd->fs_type, sizeof(d->fs) - 1);
            strncpy(d->mount, bd->mount_point, sizeof(d->mount) - 1);
            d->startup = !strcmp(bd->mount_point, "/mnt");
            const char *vn = volumes_name_of(bd->name);
            if (vn) strncpy(d->name, vn, sizeof(d->name) - 1);
            else if (d->startup && !strcmp(bd->fs_type, "tfsk") && bd->fs_ctx && ((tfsk_t *)bd->fs_ctx)->sb.volume[0])
                strncpy(d->name, ((tfsk_t *)bd->fs_ctx)->sb.volume, sizeof(d->name) - 1);
            else if (d->startup) strcpy(d->name, "tOS");
            else {
                const char *b = bd->mount_point;
                for (const char *p = bd->mount_point; *p; p++) if (*p == '/' && p[1]) b = p + 1;
                strncpy(d->name, b, sizeof(d->name) - 1);
            }
            d->used_known = diskops_usage(bd->name, &d->total, &d->used) == 0;
        } else {
            const char *fs = diskops_detect(bd->name);
            if (fs) { strncpy(d->fs, fs, sizeof(d->fs) - 1); volumes_label_of(bd->name, fs, d->name); }
        }
    }
    return n;
}

int uihost_disk_mount(const char *dev) { return volumes_mount(dev); }
int uihost_disk_unmount(const char *dev) { return volumes_unmount(dev); }

int uihost_disk_erase(const char *dev, const char *fs, const char *name, char *err, int len)
{
    err[0] = 0;
    if (!wm_gfx_take_authorization()) { strncpy(err, "Not authorised.", (size_t)len); return -1; }
    return volumes_erase(dev, fs, name, err, len);
}

int uihost_disk_first_aid(const char *dev, char *report, int len) { return diskops_first_aid(dev, report, len); }

/* ---- disk jobs: erase and First Aid on a task of their own ---- */
#include "scheduler.h"

static volatile int job_running, job_rc;
static int job_op;
static char job_dev[16], job_fs[16], job_name[24], job_report[1200];

static void job_entry(void)
{
    char err[96] = "";
    if (job_op == 1) {
        job_report[0] = 0;
        strcpy(job_report, "Unmounting the disk.\nErasing and creating \xE2\x80\x9C");
        strcat(job_report, job_name);
        strcat(job_report, "\xE2\x80\x9D.\n");
        job_rc = volumes_erase(job_dev, job_fs, job_name, err, sizeof(err)) == 0 ? 0 : -1;
        strcat(job_report, job_rc == 0 ? "Mounting the new volume.\nOperation successful.\n" : (err[0] ? err : "Erasing failed."));
        if (job_rc) strcat(job_report, "\n");
    } else job_rc = diskops_first_aid(job_dev, job_report, sizeof(job_report));
    volumes_release(job_dev);
    job_running = 0;
    task_exit();
}

int uihost_disk_job(int op, const char *dev, const char *fs, const char *name)
{
    if (job_running || volumes_claim(dev) != 0) return -1;
    if (op == 1 && !wm_gfx_take_authorization()) { volumes_release(dev); return -1; }
    job_op = op;
    strncpy(job_dev, dev, sizeof(job_dev) - 1);
    strncpy(job_fs, fs ? fs : "", sizeof(job_fs) - 1);
    strncpy(job_name, name ? name : "", sizeof(job_name) - 1);
    job_report[0] = 0;
    job_running = 1;
    if (task_spawn(job_entry, op == 1 ? "diskutil-erase" : "diskutil-firstaid") < 0) {
        job_running = 0;
        volumes_release(dev);
        return -1;
    }
    return 0;
}

int uihost_disk_job_poll(char *report, int len)
{
    if (job_running) return -2;
    strncpy(report, job_report, (size_t)len - 1);
    report[len - 1] = 0;
    return job_rc;
}

/* ---- tOS Recovery: keyboard, installing, the live session ---- */
#include "keyboard.h"
#include "installer.h"
#include "io.h"

int wm_gfx_session(void);
void wm_gfx_set_session(int s);
static int kbd_layout;

int  uihost_session(void) { return wm_gfx_session(); }
int  uihost_keyboard(void) { return kbd_layout; }
void uihost_keyboard_set(int layout)
{
    kbd_layout = layout == 1 ? 1 : 0;
    keyboard_set_layout(kbd_layout ? KBD_LAYOUT_TRQ : KBD_LAYOUT_US);
}

static volatile int inst_running, inst_rc, inst_pct;
static char inst_status[96], inst_dev[16], inst_host[40], inst_user[32], inst_full[48], inst_pw[64];
static int inst_kbd, inst_pic;

static void inst_progress(int pct, const char *what)
{
    inst_pct = pct;
    strncpy(inst_status, what, sizeof(inst_status) - 1);
}

static void inst_entry(void)
{
    blockdev_t *bd = blockdev_find(inst_dev);
    ata_device_t *disk = bd && bd->type == BLOCKDEV_ATA ? (ata_device_t *)bd->driver_data : 0;
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    inst_rc = -1;
    if (!disk) inst_progress(0, "That disk can't be used for tOS.");
    else if (installer_install(disk, inst_progress) == 0 &&
             installer_finish(disk, inst_host, inst_user, inst_full, inst_pw, inst_kbd ? "trq" : "us", inst_pic, inst_progress) == 0)
        inst_rc = 0;
    auth_restore(&saved);
    memset(inst_pw, 0, sizeof(inst_pw));
    inst_running = 0;
    task_exit();
}

int uihost_install_start(const char *dev, const char *host, const char *user, const char *full,
                         const char *password, int keyboard, int picture)
{
    if (inst_running || wm_gfx_session() != 1) return -1;          /* only from Recovery */
    strncpy(inst_dev, dev, sizeof(inst_dev) - 1);
    strncpy(inst_host, host, sizeof(inst_host) - 1);
    strncpy(inst_user, user, sizeof(inst_user) - 1);
    strncpy(inst_full, full, sizeof(inst_full) - 1);
    strncpy(inst_pw, password, sizeof(inst_pw) - 1);
    inst_kbd = keyboard; inst_pic = picture;
    inst_pct = 0; inst_status[0] = 0; inst_rc = -1;
    inst_running = 1;
    if (task_spawn(inst_entry, "installer") < 0) { inst_running = 0; return -1; }
    return 0;
}

int uihost_install_poll(int *percent, char *status, int len)
{
    *percent = inst_pct;
    strncpy(status, inst_status, (size_t)len - 1);
    status[len - 1] = 0;
    return inst_running ? -2 : inst_rc;
}

/* the live session's user: "tos", no password, may use sudo (just press Return) */
void uihost_try_live(void)
{
    if (wm_gfx_session() != 1) return;                 /* only Recovery leads to the live session */
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    auth_user_t u;
    if (auth_lookup("tos", &u) != 0) {
        uint32_t uid;
        auth_add_user("tos", 0, "tOS Live", 1, &uid);
        auth_lookup("tos", &u);
    }
    auth_restore(&saved);
    auth_set_session(u.uid, u.gid);
    if (!fsbridge_exists(u.home) || ramfs_chdir(u.home) != 0) ramfs_chdir("/");
    wm_gfx_set_session(0);
}

void uihost_restart(void)
{
    asm volatile("cli");
    outb(0x64, 0xFE);
    for (;;) asm volatile("hlt");
}

void wm_gfx_shutdown(void);
void uihost_shutdown(void) { wm_gfx_shutdown(); }

/* ---- Software Update ---- */
#include "update.h"
#include "build.h"
#include "version.h"

const void *uihost_update_status(void) { return update_info(); }
void uihost_update_check(void) { if (wm_gfx_session() == 0) update_check_async(); }
void uihost_update_download(void) { if (wm_gfx_session() == 0) update_download_async(); }
void uihost_update_restart(void) { if (update_ready()) uihost_restart(); }
unsigned uihost_build(void) { return tos_build; }
const char *uihost_version(void) { return TOS_VERSION; }
