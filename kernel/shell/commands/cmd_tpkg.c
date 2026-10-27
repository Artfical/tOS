#include "commands.h"
#include "auth.h"
#include "terminal.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"
#include "dns.h"
#include "http.h"
#include "https.h"
#include "sha256.h"
#include "png.h"

/* tpkg: tOS's package manager. Packages live on update.artfical.com:
 *   GET https://update.artfical.com/pkg/api/list
 *       one "name|version|license|desc|size|sha256|path" line per package
 *   GET http://update.artfical.com<path>
 *       the package: a gzip-compressed ustar archive. It comes over plain HTTP, the way Software
 *       Update fetches the kernel (its SHA-256 came over HTTPS and is checked; TLS in software is
 *       many times slower), and a broken download carries on from where it stopped.
 * A package unpacks into /system/pkg/<name> (on the disk of an installed system), and the
 * commands listed in its tpkg.commands ("name=path inside the package") go on the PATH. */

#define TPKG_HOST       "update.artfical.com"
#define TPKG_PKG_ROOT   "/system/pkg"
#define TPKG_PATH_FILE  "/system/path.tmbl"      /* name=/full/path, read by the shell */
#define TPKG_DB_FILE    "/system/tpkg_db.tmbl"   /* installed: name|version|destdir */
#define TPKG_CACHE_FILE "/system/tpkg_cache.tmbl"/* the last server list */
#define TPKG_LIST_CAP   (64 * 1024)

typedef struct {
    char name[40], version[32], license[32], desc[120], sha[65], path[128];
    uint32_t size;
} tpkg_entry_t;

static void print_uint(uint32_t n)
{
    char b[12];
    int i = 11;
    b[11] = 0;
    if (!n) { terminal_writestring("0"); return; }
    while (n && i) { b[--i] = (char)('0' + n % 10); n /= 10; }
    terminal_writestring(b + i);
}

static void print_mb(uint32_t bytes)
{
    print_uint(bytes / 1000000);
    terminal_putchar('.');
    print_uint(bytes / 100000 % 10);
    terminal_writestring(" MB");
}

/* ---- files, through the VFS: /system is the disk on an installed system ---- */

static char *read_whole(const char *path, uint32_t *len)
{
    if (!fsbridge_exists(path)) return 0;
    uint32_t size = fsbridge_size(path);
    char *buf = malloc(size + 1);
    if (!buf) return 0;
    int n = size ? fsbridge_read(path, buf, size, 0) : 0;
    if (n < 0) { free(buf); return 0; }
    buf[n] = 0;
    if (len) *len = (uint32_t)n;
    return buf;
}

/* replaces the file's contents (a write never makes a file shorter) */
static int write_whole(const char *path, const void *data, uint32_t len)
{
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0) return -1;
    if (len && fsbridge_write(path, data, len, 0) != (int)len) return -1;
    return 0;
}

/* every folder of `path` (and `path` itself if `self`), made if missing */
static void mkdir_p(const char *path, int self)
{
    char p[VFS_NAME_LEN];
    strncpy(p, path, sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    int n = (int)strlen(p);
    for (int i = 1; i <= n; i++) {
        if (p[i] != '/' && !(self && i == n)) continue;
        char c = p[i];
        p[i] = 0;
        if (!fsbridge_exists(p)) fsbridge_mkdir(p);
        p[i] = c;
    }
}

/* the folder and everything in it */
static void remove_tree(const char *path)
{
    if (fsbridge_is_dir(path)) {
        int n;
        vfs_entry_t *list = fsbridge_list_all(path, &n);
        for (int i = 0; list && i < n; i++) {
            if (!strcmp(list[i].name, ".") || !strcmp(list[i].name, "..")) continue;
            char child[VFS_NAME_LEN];
            strcpy(child, path);
            if (strlen(child) + strlen(list[i].name) + 2 > sizeof(child)) continue;
            strcat(child, "/");
            strcat(child, list[i].name);
            remove_tree(child);
        }
        if (list) free(list);
    }
    fsbridge_delete(path);
}

/* ---- the line files: path.tmbl and the database ---- */

/* drops every line whose value (after '=' or the second '|') starts with `prefix`, or whose
 * key (before '=' or '|') is `key`; then appends `add` if given */
static void edit_lines(const char *file, char sep, const char *key, const char *prefix, const char *add)
{
    uint32_t len = 0;
    char *old = read_whole(file, &len);
    uint32_t cap = len + (add ? strlen(add) : 0) + 2;
    char *out = malloc(cap);
    if (!out) { if (old) free(old); return; }
    uint32_t o = 0;
    for (char *p = old; p && *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        uint32_t ll = (uint32_t)(p - line);
        if (*p) p++;
        if (!ll) continue;
        char *s = 0;
        for (uint32_t i = 0; i < ll; i++) if (line[i] == sep) { s = line + i; break; }
        int drop = 0;
        if (s && key && (uint32_t)(s - line) == strlen(key) && !strncmp(line, key, strlen(key))) drop = 1;
        if (s && prefix) {
            const char *val = s + 1;
            if (sep == '|') { const char *v2 = 0; for (const char *c = val; c < line + ll; c++) if (*c == '|') { v2 = c + 1; break; } val = v2 ? v2 : val; }
            uint32_t pl = (uint32_t)strlen(prefix);
            if ((uint32_t)(line + ll - val) >= pl && !strncmp(val, prefix, pl)) drop = 1;
        }
        if (drop) continue;
        memcpy(out + o, line, ll);
        o += ll;
        out[o++] = '\n';
    }
    if (add) {
        uint32_t al = (uint32_t)strlen(add);
        memcpy(out + o, add, al);
        o += al;
        out[o++] = '\n';
    }
    if (!fsbridge_exists("/system")) fsbridge_mkdir("/system");
    write_whole(file, out, o);
    free(out);
    if (old) free(old);
}

static int db_find(const char *name, char *version, char *destdir)
{
    char *db = read_whole(TPKG_DB_FILE, 0);
    int found = 0;
    for (char *p = db; p && *p && !found;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *a = strchr(line, '|');
        if (!a) continue;
        *a = 0;
        char *b = strchr(a + 1, '|');
        if (!b) continue;
        *b = 0;
        if (strcmp(line, name)) continue;
        if (version) { strncpy(version, a + 1, 31); version[31] = 0; }
        if (destdir) { strncpy(destdir, b + 1, VFS_NAME_LEN - 1); destdir[VFS_NAME_LEN - 1] = 0; }
        found = 1;
    }
    if (db) free(db);
    return found;
}

/* ---- the server ---- */

static int field_copy(char **p, char *out, uint32_t cap)
{
    char *s = *p, *e = s;
    while (*e && *e != '|' && *e != '\n') e++;
    uint32_t n = (uint32_t)(e - s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    *p = *e == '|' ? e + 1 : e;
    return *e == '|';
}

/* the n-th package line of a list (0-based); 0 when there is none */
static int list_entry(const char *list, int n, tpkg_entry_t *e)
{
    const char *p = list;
    for (int i = 0; p && *p; i++) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        uint32_t ll = (uint32_t)(p - line);
        if (*p) p++;
        if (!ll || line[0] == '#') { i--; continue; }
        if (i != n) continue;
        char buf[512];
        if (ll >= sizeof(buf)) ll = sizeof(buf) - 1;
        memcpy(buf, line, ll);
        buf[ll] = 0;
        char *c = buf, size[16] = "";
        memset(e, 0, sizeof(*e));
        field_copy(&c, e->name, sizeof(e->name));
        field_copy(&c, e->version, sizeof(e->version));
        field_copy(&c, e->license, sizeof(e->license));
        field_copy(&c, e->desc, sizeof(e->desc));
        field_copy(&c, size, sizeof(size));
        field_copy(&c, e->sha, sizeof(e->sha));
        field_copy(&c, e->path, sizeof(e->path));
        for (char *s = size; *s >= '0' && *s <= '9'; s++) e->size = e->size * 10 + (uint32_t)(*s - '0');
        return 1;
    }
    return 0;
}

static int find_entry(const char *list, const char *name, tpkg_entry_t *e)
{
    for (int i = 0; list_entry(list, i, e); i++) if (!strcmp(e->name, name)) return 1;
    return 0;
}

static int resolve(uint32_t *ip)
{
    if (dns_resolve(TPKG_HOST, ip) == 0) return 0;
    terminal_writestring("tpkg: can't reach " TPKG_HOST " (check the network)\n");
    return -1;
}

static int header_end(const uint8_t *b, int n)
{
    for (int i = 0; i + 3 < n; i++) if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return i + 4;
    return -1;
}

/* the server's list, saved as the cache; a malloc'd string, or 0 */
static char *fetch_list(uint32_t ip, int quiet)
{
    uint8_t *resp = malloc(TPKG_LIST_CAP);
    if (!resp) return 0;
    int n = https_get(ip, TPKG_HOST, 443, "/pkg/api/list", resp, TPKG_LIST_CAP - 1);
    int he = n > 12 ? header_end(resp, n) : -1;
    if (n <= 12 || strncmp((char *)resp + 9, "200", 3) || he < 0) {
        if (!quiet) terminal_writestring("tpkg: the package server did not answer\n");
        free(resp);
        return 0;
    }
    uint32_t bl = (uint32_t)(n - he);
    char *list = malloc(bl + 1);
    if (list) {
        memcpy(list, resp + he, bl);
        list[bl] = 0;
        write_whole(TPKG_CACHE_FILE, list, bl);
    }
    free(resp);
    return list;
}

static char *cached_or_fetched(uint32_t ip)
{
    char *list = read_whole(TPKG_CACHE_FILE, 0);
    return list ? list : fetch_list(ip, 0);
}

/* ---- downloading ---- */

static uint32_t dl_total, dl_part, dl_shown;

static void on_progress(int received)
{
    uint32_t got = dl_part + (uint32_t)received;
    uint32_t pct = dl_total ? (uint32_t)((uint64_t)got * 100 / dl_total) : 0;
    if (pct > 100) pct = 100;
    if (pct >= dl_shown + 10) {
        dl_shown = pct - pct % 10;
        terminal_writestring(" ");
        print_uint(dl_shown);
        terminal_writestring("%");
    }
}

/* the whole file at `path`, `size` bytes, into *data (inside *buf, which the caller frees) */
static int download(uint32_t ip, const char *path, uint32_t size, uint8_t **buf, uint8_t **data)
{
    int cap = (int)size + 32768;
    uint8_t *b = malloc((uint32_t)cap);
    if (!b) { terminal_writestring(" not enough memory\n"); return -1; }
    uint32_t got = 0;
    int he0 = -1;
    dl_total = size; dl_part = 0; dl_shown = 0;
    http_progress = on_progress;
    for (int attempt = 0; attempt < 8 && (he0 < 0 || got < size); attempt++) {
        if (he0 < 0) {
            int n = http_get(ip, TPKG_HOST, 80, path, b, cap);
            if (n <= 12 || strncmp((char *)b, "HTTP/1.", 7) || strncmp((char *)b + 9, "200", 3)) continue;
            he0 = header_end(b, n);
            if (he0 < 0) continue;
            got = (uint32_t)(n - he0);
        } else {
            /* the rest: read after what is there, then drop the answer's headers */
            uint8_t *dst = b + he0 + got;
            dl_part = got;
            int n = http_get_from(ip, TPKG_HOST, 80, path, got, dst, cap - he0 - (int)got);
            if (n <= 12 || strncmp((char *)dst, "HTTP/1.", 7) || strncmp((char *)dst + 9, "206", 3)) continue;
            int he = header_end(dst, n);
            if (he < 0) continue;
            memmove(dst, dst + he, (size_t)(n - he));
            got += (uint32_t)(n - he);
        }
    }
    http_progress = 0;
    terminal_putchar('\n');
    if (he0 < 0 || got != size) { free(b); terminal_writestring("tpkg: the download stopped (check the network and try again)\n"); return -1; }
    *buf = b;
    *data = b + he0;
    return 0;
}

static int sha_ok(const uint8_t *d, uint32_t n, const char *want)
{
    uint8_t h[32];
    sha256_hash(d, n, h);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        if (want[2 * i] != hx[h[i] >> 4] || want[2 * i + 1] != hx[h[i] & 15]) return 0;
    return 1;
}

/* a gzip member -> its contents (malloc'd), or 0 */
static uint8_t *gunzip(const uint8_t *g, uint32_t n, uint32_t *out_len)
{
    if (n < 18 || g[0] != 0x1F || g[1] != 0x8B || g[2] != 8) return 0;
    uint8_t flags = g[3];
    uint32_t o = 10;
    if (flags & 4) { if (o + 2 > n) return 0; o += 2 + (uint32_t)(g[o] | (g[o + 1] << 8)); }   /* FEXTRA */
    if (flags & 8) { while (o < n && g[o]) o++; o++; }                                         /* FNAME */
    if (flags & 16) { while (o < n && g[o]) o++; o++; }                                        /* FCOMMENT */
    if (flags & 2) o += 2;                                                                     /* FHCRC */
    if (o + 8 > n) return 0;
    uint32_t isize = (uint32_t)g[n - 4] | ((uint32_t)g[n - 3] << 8) | ((uint32_t)g[n - 2] << 16) | ((uint32_t)g[n - 1] << 24);
    uint8_t *out = malloc(isize ? isize : 1);
    if (!out) return 0;
    uint32_t got = 0;
    if (inflate_raw_buffer(g + o, n - o - 8, out, isize, &got) != 0 || got != isize) { free(out); return 0; }
    *out_len = got;
    return out;
}

/* the ustar archive's files and folders under `dest`; returns how many files, or -1 */
static int untar(const uint8_t *t, uint32_t n, const char *dest)
{
    int files = 0;
    for (uint32_t at = 0; at + 512 <= n;) {
        const uint8_t *h = t + at;
        if (!h[0]) break;
        uint32_t size = 0;
        for (int i = 124; i < 136; i++) if (h[i] >= '0' && h[i] <= '7') size = size * 8 + (uint32_t)(h[i] - '0');
        char name[VFS_NAME_LEN];
        int k = 0;
        if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            for (int i = 0; i < 155 && h[345 + i] && k < VFS_NAME_LEN - 2; i++) name[k++] = (char)h[345 + i];
            name[k++] = '/';
        }
        for (int i = 0; i < 100 && h[i] && k < VFS_NAME_LEN - 1; i++) name[k++] = (char)h[i];
        name[k] = 0;
        char *nm = name;
        while (nm[0] == '.' && nm[1] == '/') nm += 2;
        int len = (int)strlen(nm);
        while (len && nm[len - 1] == '/') nm[--len] = 0;
        char full[VFS_NAME_LEN];
        if (strlen(dest) + (uint32_t)len + 2 > sizeof(full)) { terminal_writestring("tpkg: a path in the package is too long\n"); return -1; }
        strcpy(full, dest);
        strcat(full, "/");
        strcat(full, nm);
        char type = (char)h[156];
        if (!len) {
            /* the archive's own "./" */
        } else if (type == '5') {
            mkdir_p(full, 1);
        } else if (type == '0' || type == 0) {
            if (at + 512 + size > n) return -1;
            mkdir_p(full, 0);
            if (write_whole(full, t + at + 512, size) != 0) { terminal_writestring("tpkg: could not write "); terminal_writestring(full); terminal_putchar('\n'); return -1; }
            files++;
        }
        at += 512 + ((size + 511) & ~511u);
    }
    return files;
}

/* tpkg.commands in the package: "cmd=path inside it" -> path.tmbl */
static int register_commands(const char *destdir)
{
    char f[VFS_NAME_LEN];
    strcpy(f, destdir);
    strcat(f, "/tpkg.commands");
    char *c = read_whole(f, 0);
    int n = 0;
    for (char *p = c; p && *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        char add[VFS_NAME_LEN + 64];
        strcpy(add, line);
        strcat(add, "=");
        strcat(add, destdir);
        strcat(add, "/");
        strcat(add, eq + 1);
        edit_lines(TPKG_PATH_FILE, '=', line, 0, add);
        terminal_writestring("  ");
        terminal_writestring(line);
        n++;
    }
    if (c) free(c);
    if (n) terminal_writestring("  (on the PATH)\n");
    return n;
}

/* ---- the subcommands ---- */

static void tpkg_list_installed(void)
{
    char *db = read_whole(TPKG_DB_FILE, 0);
    if (!db || !db[0]) { terminal_writestring("No packages installed.\n"); if (db) free(db); return; }
    for (char *p = db; *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *a = strchr(line, '|');
        if (!a) continue;
        *a = 0;
        char *b = strchr(a + 1, '|');
        if (b) *b = 0;
        terminal_writestring(line);
        terminal_writestring("  ");
        terminal_writestring(a + 1);
        if (b) { terminal_writestring("  "); terminal_writestring(b + 1); }
        terminal_putchar('\n');
    }
    free(db);
}

static void print_list(const char *list)
{
    tpkg_entry_t e;
    int any = 0;
    for (int i = 0; list_entry(list, i, &e); i++) {
        any = 1;
        char ver[32];
        int have = db_find(e.name, ver, 0);
        terminal_writestring(e.name);
        terminal_writestring("  ");
        terminal_writestring(e.version);
        terminal_writestring("  ");
        print_mb(e.size);
        if (have) { terminal_writestring("  [installed "); terminal_writestring(ver); terminal_writestring("]"); }
        terminal_writestring("\n    ");
        terminal_writestring(e.desc);
        if (e.license[0]) { terminal_writestring(" ("); terminal_writestring(e.license); terminal_writestring(")"); }
        terminal_putchar('\n');
    }
    if (!any) terminal_writestring("The server has no packages.\n");
}

static void tpkg_install(uint32_t ip, const char *name, const char *list)
{
    tpkg_entry_t e;
    if (!find_entry(list, name, &e)) {
        terminal_writestring("tpkg: no package called ");
        terminal_writestring(name);
        terminal_writestring(" (see: tpkg look)\n");
        return;
    }
    if (strlen(e.sha) != 64 || !e.size || e.path[0] != '/') { terminal_writestring("tpkg: the server's entry for it is incomplete\n"); return; }
    terminal_writestring("Downloading ");
    terminal_writestring(e.name);
    terminal_writestring(" ");
    terminal_writestring(e.version);
    terminal_writestring(" (");
    print_mb(e.size);
    terminal_writestring(")");
    uint8_t *buf, *data;
    if (download(ip, e.path, e.size, &buf, &data) != 0) return;
    if (!sha_ok(data, e.size, e.sha)) { free(buf); terminal_writestring("tpkg: the download is damaged (checksum); nothing was installed\n"); return; }

    terminal_writestring("Unpacking...\n");
    uint32_t tlen = 0;
    uint8_t *tar = gunzip(data, e.size, &tlen);
    free(buf);
    if (!tar) { terminal_writestring("tpkg: the package could not be unpacked (not enough memory, or not a .tar.gz)\n"); return; }

    char dest[VFS_NAME_LEN];
    strcpy(dest, TPKG_PKG_ROOT "/");
    strcat(dest, e.name);
    char oldv[32], olddir[VFS_NAME_LEN];
    if (db_find(e.name, oldv, olddir)) {                     /* an older version goes first */
        edit_lines(TPKG_PATH_FILE, '=', 0, olddir, 0);
        remove_tree(olddir);
    }
    if (fsbridge_exists(dest)) remove_tree(dest);
    mkdir_p(dest, 1);
    int files = untar(tar, tlen, dest);
    free(tar);
    if (files < 0) { remove_tree(dest); terminal_writestring("tpkg: installing failed; nothing was kept\n"); return; }

    char line[VFS_NAME_LEN + 80];
    strcpy(line, e.name); strcat(line, "|"); strcat(line, e.version); strcat(line, "|"); strcat(line, dest);
    edit_lines(TPKG_DB_FILE, '|', e.name, 0, line);
    terminal_writestring("Installed ");
    terminal_writestring(e.name);
    terminal_writestring(" ");
    terminal_writestring(e.version);
    terminal_writestring(" in ");
    terminal_writestring(dest);
    terminal_writestring(" (");
    print_uint((uint32_t)files);
    terminal_writestring(" files)\n");
    register_commands(dest);
}

static void tpkg_remove(const char *name)
{
    char ver[32], dir[VFS_NAME_LEN];
    if (!db_find(name, ver, dir)) { terminal_writestring("tpkg: "); terminal_writestring(name); terminal_writestring(" is not installed\n"); return; }
    edit_lines(TPKG_PATH_FILE, '=', 0, dir, 0);
    remove_tree(dir);
    edit_lines(TPKG_DB_FILE, '|', name, 0, 0);
    terminal_writestring("Removed ");
    terminal_writestring(name);
    terminal_putchar('\n');
}

static void tpkg_upgrade(uint32_t ip)
{
    char *list = fetch_list(ip, 0);
    if (!list) return;
    char *db = read_whole(TPKG_DB_FILE, 0);
    int any = 0;
    for (char *p = db; p && *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *a = strchr(line, '|');
        if (!a) continue;
        *a = 0;
        char *b = strchr(a + 1, '|');
        if (b) *b = 0;
        tpkg_entry_t e;
        if (!find_entry(list, line, &e) || !strcmp(e.version, a + 1)) continue;
        any = 1;
        tpkg_install(ip, line, list);
    }
    if (!any) terminal_writestring("Everything is up to date.\n");
    if (db) free(db);
    free(list);
}

void cmd_tpkg(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(
            "usage: tpkg <subcommand>\n"
            "  list             installed packages\n"
            "  look             the packages on " TPKG_HOST "\n"
            "  update           refresh that list\n"
            "  install <name>   install a package (its commands go on the PATH)\n"
            "  remove <name>    uninstall a package\n"
            "  upgrade          install newer versions of what is installed\n");
        return;
    }
    /* everything that changes the system is for root */
    if (strcmp(args[1], "list") && strcmp(args[1], "look") && !auth_is_root()) {
        terminal_writestring("tpkg: permission denied: package management needs root (try: sudo tpkg ");
        terminal_writestring(args[1]);
        terminal_writestring(" ...)\n");
        return;
    }
    if (!strcmp(args[1], "list")) { tpkg_list_installed(); return; }
    if (!strcmp(args[1], "remove")) {
        if (argc < 3) { terminal_writestring("usage: tpkg remove <name>\n"); return; }
        tpkg_remove(args[2]);
        return;
    }
    uint32_t ip;
    if (resolve(&ip) != 0) return;
    if (!strcmp(args[1], "look") || !strcmp(args[1], "update")) {
        char *list = !strcmp(args[1], "look") ? cached_or_fetched(ip) : fetch_list(ip, 0);
        if (list) { print_list(list); free(list); }
    } else if (!strcmp(args[1], "install")) {
        if (argc < 3) { terminal_writestring("usage: tpkg install <name>\n"); return; }
        char *list = fetch_list(ip, 0);
        if (!list) return;
        tpkg_install(ip, args[2], list);
        free(list);
    } else if (!strcmp(args[1], "upgrade")) {
        tpkg_upgrade(ip);
    } else {
        terminal_writestring("tpkg: unknown subcommand (try: list, look, update, install, remove, upgrade)\n");
    }
}
