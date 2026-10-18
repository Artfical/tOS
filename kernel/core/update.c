#include "update.h"
#include "build.h"
#include "string.h"
#include "memory.h"
#include "vfs.h"
#include "auth.h"
#include "klog.h"
#include "scheduler.h"
#include "debugmon.h"
#include "dns.h"
#include "http.h"
#include "https.h"
#include "sha256.h"
#include "imgload.h"
#include "bootsplash.h"
#include "kexec.h"

#define HOST "update.artfical.com"
#define STAGE "/mnt/system/update"
#define BOOTDIR "/mnt/system/boot"

static update_info_t info;
static volatile int busy;

const update_info_t *update_info(void) { return &info; }

/* ------------------------------------------------------------------ files */

static int write_file(const char *path, const void *data, uint32_t len)
{
    int fd = vfs_open(path, VFS_WRONLY | VFS_CREAT | VFS_TRUNC);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    uint32_t done = 0;
    while (done < len) {
        int w = vfs_write(fd, p + done, len - done > 65536 ? 65536 : len - done);
        if (w <= 0) { vfs_close(fd); return -1; }
        done += (uint32_t)w;
    }
    vfs_close(fd);
    return 0;
}

static uint8_t *read_file(const char *path, uint32_t *len)
{
    vfs_entry_t e;
    if (vfs_stat(path, &e) != 0 || e.is_dir) return 0;
    uint8_t *b = malloc(e.size + 1);
    if (!b) return 0;
    int fd = vfs_open(path, VFS_RDONLY);
    if (fd < 0) { free(b); return 0; }
    uint32_t got = 0;
    while (got < e.size) {
        int r = vfs_read(fd, b + got, e.size - got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    vfs_close(fd);
    b[got] = 0;
    *len = got;
    return b;
}

static void hex(const uint8_t *d, int n, char *out)
{
    for (int i = 0; i < n; i++) { out[2 * i] = "0123456789abcdef"[d[i] >> 4]; out[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    out[2 * n] = 0;
}

static int sha_matches(const uint8_t *d, uint32_t n, const char *want)
{
    uint8_t h[32];
    char s[65];
    sha256_hash(d, n, h);
    hex(h, 32, s);
    return !strcmp(s, want);
}

/* "key=value" lines; value is returned without the line end */
static const char *field(const char *text, const char *key, char *out, int cap)
{
    int kl = (int)strlen(key);
    for (const char *p = text; p && *p; ) {
        if (!strncmp(p, key, (size_t)kl) && p[kl] == '=') {
            p += kl + 1;
            int o = 0;
            while (*p && *p != '\n' && *p != '\r' && o < cap - 1) out[o++] = *p++;
            out[o] = 0;
            return out;
        }
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : 0;
    }
    out[0] = 0;
    return 0;
}

static unsigned num(const char *s) { unsigned v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0'); return v; }

/* ------------------------------------------------------------------ network */

/* GET https://update.artfical.com<path>: the body in *body (inside *buf, which the caller frees) */
static int fetch(const char *path, uint32_t expect, uint8_t **buf, uint8_t **body, uint32_t *blen)
{
    uint32_t ip;
    if (dns_resolve(HOST, &ip) != 0) { strcpy(info.err, "update.artfical.com could not be found. Check the network."); return -1; }
    int cap = expect ? (int)expect + 32768 : 256 * 1024;
    uint8_t *b = malloc((uint32_t)cap);
    if (!b) { strcpy(info.err, "Not enough memory for the update."); return -1; }
    int n = https_get(ip, HOST, 443, path, b, cap);
    if (n <= 12 || strncmp((char *)b, "HTTP/1.", 7) || strncmp((char *)b + 9, "200", 3)) {
        free(b);
        strcpy(info.err, "The update server did not answer.");
        return -1;
    }
    uint8_t *e = 0;
    for (int i = 0; i + 3 < n; i++) if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') { e = b + i + 4; break; }
    if (!e) { free(b); strcpy(info.err, "The update server's answer was cut off."); return -1; }
    *buf = b;
    *body = e;
    *blen = (uint32_t)(n - (e - b));
    if (expect && *blen != expect) { free(b); strcpy(info.err, "The download was incomplete."); return -1; }
    return 0;
}

/* where the staged update is installable: an installed system, not the live session */
static int installed(void)
{
    return vfs_exists("/mnt/system") && auth_have_accounts();
}

static void check_task(void)
{
    info.err[0] = 0;
    char path[64] = "/api/check?build=";
    char n[12]; int o = 0; unsigned v = tos_build;
    char r[12]; int ro = 0;
    do { r[ro++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (ro) n[o++] = r[--ro];
    n[o] = 0;
    strcat(path, n);
    uint8_t *buf, *body; uint32_t len;
    if (fetch(path, 0, &buf, &body, &len) != 0) { info.state = UPD_ERROR; busy = 0; task_exit(); }
    body[len] = 0;
    const char *t = (const char *)body;
    char v2[200];
    info.current = tos_build;
    info.latest = num(field(t, "latest", v2, sizeof v2) ? v2 : "0");
    info.behind = (int)num(field(t, "behind", v2, sizeof v2) ? v2 : "0");
    info.far = field(t, "far", v2, sizeof v2) && v2[0] == '1';
    field(t, "commit", info.commit, sizeof info.commit);
    field(t, "date", info.date, sizeof info.date);
    field(t, "title", info.title, sizeof info.title);
    if (field(t, "kernel", v2, sizeof v2)) {
        char *sp1 = strchr(v2, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : 0;
        if (sp1 && sp2) { *sp1 = 0; *sp2 = 0; strncpy(info.kpath, v2, 63); info.ksize = num(sp1 + 1); strncpy(info.ksha, sp2 + 1, 64); }
    }
    if (field(t, "initrd", v2, sizeof v2)) {
        char *sp1 = strchr(v2, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : 0;
        if (sp1 && sp2) { *sp1 = 0; *sp2 = 0; strncpy(info.ipath, v2, 63); info.isize = num(sp1 + 1); strncpy(info.isha, sp2 + 1, 64); }
    }
    /* the notes: note=<build>\t<icon>\t<app>\t<text>\t<url>, shot=<build>\t<path>\t<caption>\t<app> */
    for (int i = 0; i < info.nshots; i++) if (info.shots[i].rgba) free(info.shots[i].rgba);
    info.nnotes = info.nshots = 0;
    for (const char *p = t; p && *p; ) {
        const char *nl = strchr(p, '\n');
        int ll = nl ? (int)(nl - p) : (int)strlen(p);
        char line[420];
        if (ll > (int)sizeof(line) - 1) ll = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)ll);
        line[ll] = 0;
        char *f[5] = { 0 }; int k = 0;
        int is_note = !strncmp(line, "note=", 5), is_shot = !strncmp(line, "shot=", 5);
        if (is_note || is_shot) {
            f[k++] = line + 5;
            for (char *q = line + 5; *q && k < 5; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        }
        if (is_note && k == 5 && info.nnotes < UPD_MAX_NOTES) {
            update_note_t *nt = &info.notes[info.nnotes++];
            nt->build = num(f[0]);
            strncpy(nt->icon, f[1], sizeof nt->icon - 1); strncpy(nt->app, f[2], sizeof nt->app - 1);
            strncpy(nt->text, f[3], sizeof nt->text - 1); strncpy(nt->url, f[4], sizeof nt->url - 1);
        } else if (is_shot && k >= 3 && info.nshots < UPD_MAX_SHOTS) {
            update_shot_t *sh = &info.shots[info.nshots++];
            memset(sh, 0, sizeof *sh);
            sh->build = num(f[0]);
            strncpy(sh->path, f[1], sizeof sh->path - 1); strncpy(sh->caption, f[2], sizeof sh->caption - 1);
            if (k > 3) strncpy(sh->app, f[3], sizeof sh->app - 1);
        }
        p = nl ? nl + 1 : 0;
    }
    free(buf);
    info.state = update_ready() ? UPD_READY : info.latest > tos_build && info.kpath[0] ? UPD_AVAILABLE : UPD_UPTODATE;
    /* the pictures of the notes, small PNGs */
    for (int i = 0; i < info.nshots; i++) {
        uint8_t *b2, *bd; uint32_t bl;
        if (fetch(info.shots[i].path, 0, &b2, &bd, &bl) != 0) continue;
        uint8_t *rgba; uint32_t w, h; const char *err;
        if (img_decode(bd, bl, &rgba, &w, &h, &err) == 0) { info.shots[i].rgba = rgba; info.shots[i].w = (int)w; info.shots[i].h = (int)h; }
        free(b2);
    }
    info.err[0] = 0;
    busy = 0;
    task_exit();
}

void update_check_async(void)
{
    if (busy || info.state == UPD_DOWNLOADING) return;
    if (!installed()) { info.state = UPD_LIVE; return; }
    busy = 1;
    info.state = UPD_CHECKING;
    if (task_spawn(check_task, "softwareupdate") < 0) { busy = 0; info.state = UPD_ERROR; strcpy(info.err, "Could not start checking."); }
}

/* ------------------------------------------------------------------ downloading */

static uint32_t dl_base, dl_last_ms, dl_last_bytes;

static void on_progress(int received)
{
    uint32_t got = dl_base + (uint32_t)received;
    if (got > info.total) got = info.total;
    info.got = got;
    uint32_t now = debugmon_uptime_ms();
    if (now - dl_last_ms >= 500) {
        info.speed = (got - dl_last_bytes) * 1000 / (now - dl_last_ms ? now - dl_last_ms : 1);
        dl_last_ms = now;
        dl_last_bytes = got;
    }
}

static void download_task(void)
{
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    info.err[0] = 0;
    info.total = info.ksize + info.isize;
    info.got = 0; info.speed = 0;
    dl_base = 0; dl_last_ms = debugmon_uptime_ms(); dl_last_bytes = 0;
    if (!vfs_exists(STAGE)) vfs_mkdir(STAGE, 0700);
    vfs_unlink(STAGE "/ready");                      /* an older staged update is replaced */
    uint8_t *buf, *body; uint32_t len;
    http_progress = on_progress;
    int ok = fetch(info.kpath, info.ksize, &buf, &body, &len) == 0;
    if (ok && !sha_matches(body, len, info.ksha)) { ok = 0; strcpy(info.err, "The downloaded kernel is damaged (checksum)."); }
    if (ok) ok = write_file(STAGE "/tOS.elf", body, len) == 0;
    if (ok) free(buf);
    dl_base = info.ksize;
    if (ok) {
        ok = fetch(info.ipath, info.isize, &buf, &body, &len) == 0;
        if (ok && !sha_matches(body, len, info.isha)) { ok = 0; strcpy(info.err, "The downloaded system files are damaged (checksum)."); }
        if (ok) ok = write_file(STAGE "/initrd.tar", body, len) == 0;
        if (ok) free(buf);
    }
    http_progress = 0;
    if (ok) {
        /* the mark goes last: an update counts as ready only once all of it is on the disk */
        char m[200] = "build=";
        char n[12]; int o = 0; unsigned v = info.latest; char r[12]; int ro = 0;
        do { r[ro++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (ro) n[o++] = r[--ro];
        n[o] = 0;
        strcat(m, n); strcat(m, "\nkernel="); strcat(m, info.ksha); strcat(m, "\ninitrd="); strcat(m, info.isha); strcat(m, "\n");
        ok = write_file(STAGE "/ready", m, (uint32_t)strlen(m)) == 0;
    }
    if (!ok && !info.err[0]) strcpy(info.err, "The update could not be saved on the disk.");
    info.got = ok ? info.total : info.got;
    info.state = ok ? UPD_READY : UPD_ERROR;
    klog_write(ok ? "update: downloaded and staged\n" : "update: the download failed\n");
    auth_restore(&saved);
    busy = 0;
    task_exit();
}

void update_download_async(void)
{
    if (busy || info.state != UPD_AVAILABLE) return;
    if (!installed()) { info.state = UPD_LIVE; return; }
    busy = 1;
    info.state = UPD_DOWNLOADING;
    if (task_spawn(download_task, "softwareupdate") < 0) { busy = 0; info.state = UPD_ERROR; }
}

int update_ready(void) { return vfs_exists(STAGE "/ready"); }

/* ------------------------------------------------------------------ at boot */

static void build_dir(unsigned b, char *out)
{
    strcpy(out, BOOTDIR "/");
    char r[12]; int ro = 0;
    do { r[ro++] = (char)('0' + b % 10); b /= 10; } while (b);
    int o = (int)strlen(out);
    while (ro) out[o++] = r[--ro];
    out[o] = 0;
}

int update_apply_at_boot(void)
{
    uint32_t ml;
    uint8_t *mark = read_file(STAGE "/ready", &ml);
    if (!mark) return 0;
    char v[80], ksha[80], isha[80];
    unsigned b = num(field((char *)mark, "build", v, sizeof v) ? v : "0");
    field((char *)mark, "kernel", ksha, sizeof ksha);
    field((char *)mark, "initrd", isha, sizeof isha);
    free(mark);
    klog_write("update: installing a staged update\n");
    bootsplash_status("Updating tOS\xE2\x80\xA6");
    bootsplash_progress(70);
    uint32_t kl, il;
    uint8_t *k = read_file(STAGE "/tOS.elf", &kl), *in = read_file(STAGE "/initrd.tar", &il);
    int ok = b && k && in && sha_matches(k, kl, ksha) && sha_matches(in, il, isha);
    bootsplash_progress(74);
    if (ok) {
        if (!vfs_exists(BOOTDIR)) vfs_mkdir(BOOTDIR, 0755);
        char d[64], p[96];
        build_dir(b, d);
        if (!vfs_exists(d)) vfs_mkdir(d, 0755);
        strcpy(p, d); strcat(p, "/tOS.elf");
        ok = write_file(p, k, kl) == 0;
        bootsplash_progress(78);
        strcpy(p, d); strcat(p, "/initrd.tar");
        if (ok) ok = write_file(p, in, il) == 0;
        bootsplash_progress(80);
        /* one small file says which build to start, with the checksums to prove its files whole */
        if (ok) {
            char cur[220] = "build=";
            strcat(cur, v); strcat(cur, "\nkernel="); strcat(cur, ksha); strcat(cur, "\ninitrd="); strcat(cur, isha); strcat(cur, "\n");
            ok = write_file(BOOTDIR "/current", cur, (uint32_t)strlen(cur)) == 0;
            write_file(BOOTDIR "/attempts", "0\n", 2);
        }
    }
    if (k) free(k);
    if (in) free(in);
    /* the staged copy goes either way: a damaged one would only be tried again and again */
    vfs_unlink(STAGE "/ready");
    vfs_unlink(STAGE "/tOS.elf");
    vfs_unlink(STAGE "/initrd.tar");
    klog_write(ok ? "update: installed\n" : "update: the staged update was damaged; kept the current system\n");
    return ok;
}

/* the CD's kernel hands over to the newest build on the disk, unless that one failed before */
void update_boot_chain(void)
{
    uint32_t cl;
    uint8_t *cur = read_file(BOOTDIR "/current", &cl);
    if (!cur) return;
    char v[80], ksha[80], isha[80];
    unsigned b = num(field((char *)cur, "build", v, sizeof v) ? v : "0");
    field((char *)cur, "kernel", ksha, sizeof ksha);
    field((char *)cur, "initrd", isha, sizeof isha);
    free(cur);
    if (b <= tos_build) return;                       /* this kernel is as new or newer */
    uint32_t al;
    uint8_t *att = read_file(BOOTDIR "/attempts", &al);
    unsigned attempts = att ? num((char *)att) : 0;
    if (att) free(att);
    if (attempts >= 2) { klog_write("update: the newer build did not start twice; staying on this one\n"); return; }
    char d[64], p[96];
    build_dir(b, d);
    strcpy(p, d); strcat(p, "/tOS.elf");
    uint32_t kl, il;
    uint8_t *k = read_file(p, &kl);
    strcpy(p, d); strcat(p, "/initrd.tar");
    uint8_t *in = read_file(p, &il);
    if (!k || !in || !sha_matches(k, kl, ksha) || !sha_matches(in, il, isha)) {
        klog_write("update: the newer build's files are damaged; staying on this one\n");
        if (k) free(k);
        if (in) free(in);
        return;
    }
    char a2[4] = { (char)('0' + (attempts + 1) % 10), '\n', 0 };
    write_file(BOOTDIR "/attempts", a2, 2);
    klog_write("update: starting the newer build from the disk\n");
    kexec_boot(k, kl, in, il);                        /* returns only if it could not */
    klog_write("update: could not start the newer build\n");
    free(k);
    free(in);
}

void update_session_ok(void)
{
    if (vfs_exists(BOOTDIR "/attempts")) {
        auth_saved_t saved;
        auth_become(0, 0, &saved);
        write_file(BOOTDIR "/attempts", "0\n", 2);
        auth_restore(&saved);
    }
}
