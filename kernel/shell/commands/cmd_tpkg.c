#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "memory.h"
#include "ramfs.h"
#include "dns.h"
#include "http.h"
#include "tarfmt.h"

/* tpkg: a minimal package manager talking to a Flask+Waitress server
 * at a fixed, well-known address -- no host/port argument needed.
 * `tpkg list` hits /api/list (plain text, one
 * "name|version|license|desc" per line); `tpkg install <name>` hits
 * /api/download/<name> (a raw ustar .tar), saves it to ramfs, and
 * extracts it into /programs/<name>/ with the tar support this
 * kernel already has. */
#define TPKG_HOST "pkg.artfical.com"
#define TPKG_PORT 80
#define TPKG_BUF_SIZE (256 * 1024)

static void print_uint(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

#define TPKG_PATH_FILE "/sys/path.tmbl"
#define TPKG_DB_FILE "/sys/tpkg_db.tmbl"     /* installed packages: name|version|destdir */
#define TPKG_CACHE_FILE "/sys/tpkg_cache.tmbl" /* last `tpkg update`: raw server list body */

/* Reads a whole file into a malloc'd, NUL-terminated buffer. NULL if
 * missing/empty/OOM. *len_out (if given) is the byte count, not
 * counting the added NUL. */
static char *tpkg_read_whole(const char *path, int *len_out)
{
    if (!ramfs_exists(path)) return NULL;
    uint32_t size = ramfs_size(path);
    if (size == 0) return NULL;
    char *buf = (char *)malloc(size + 1);
    if (!buf) return NULL;
    int n = ramfs_read(path, buf, size, 0);
    if (n <= 0) { free(buf); return NULL; }
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}

/* Replaces a file's entire contents. ramfs_write() only ever grows a
 * file's reported size, never shrinks it (see ramfs_vfs_write() in
 * ramfs.c), so an in-place overwrite can't be used when the new
 * content might be shorter than the old -- delete and recreate
 * instead, every time, for a real "this is now the whole file"
 * semantic. */
static void tpkg_write_whole(const char *path, const char *content, int len)
{
    if (!ramfs_exists("/sys")) ramfs_mkdir("/sys");
    if (ramfs_exists(path)) ramfs_delete(path);
    if (ramfs_create(path) != 0) return;
    if (len > 0) ramfs_write(path, content, (uint32_t)len, 0);
}

/* Appends one "cmdname=fullpath\n" line to /sys/path.tmbl for the
 * shell's command fallback (shell.c's path_fallback_exec()).
 * Idempotent against the exact same line, so re-installing the same
 * package version doesn't grow the file forever. */
static void tpkg_path_register(const char *cmdname, const char *fullpath)
{
    if (!ramfs_exists("/sys")) ramfs_mkdir("/sys");

    char line[224];
    int i = 0;
    while (cmdname[i] && i < 60) { line[i] = cmdname[i]; i++; }
    line[i++] = '=';
    const char *p = fullpath;
    while (*p && i < 222) { line[i++] = *p++; }
    line[i++] = '\n';

    uint32_t size = ramfs_exists(TPKG_PATH_FILE) ? ramfs_size(TPKG_PATH_FILE) : 0;
    if (size > 0) {
        char *buf = (char *)malloc(size + 1);
        if (buf) {
            int n = ramfs_read(TPKG_PATH_FILE, buf, size, 0);
            if (n > 0) {
                buf[n] = 0;
                char *found = strstr(buf, line);
                /* strstr alone could match a substring spanning two
                 * lines by accident; also require it to start right
                 * after a '\n' (or at the very start of the file). */
                if (found && (found == buf || *(found - 1) == '\n')) {
                    free(buf);
                    return; /* already registered */
                }
            }
            free(buf);
        }
    }

    if (!ramfs_exists(TPKG_PATH_FILE)) ramfs_create(TPKG_PATH_FILE);
    ramfs_write(TPKG_PATH_FILE, line, i, size);
}

/* Trims a trailing \r/\n/space run off a command-name sidecar's
 * content -- files written from a text editor or `echo` almost
 * always carry a trailing newline that isn't part of the name. */
static void trim_trailing_ws(char *s)
{
    int n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

/* Scans an extracted package directory for "<file>.txt" sidecars --
 * each one declares the command name that "<file>" (a .py or .t
 * sitting right next to it) should be invokable as, decoupling the
 * shell command name from the file's own basename and letting one
 * package expose several commands. Falls back to registering
 * <pkgname> -> <destdir>/<pkgname>.py or .t if the package ships no
 * sidecars at all (older/simpler packages). */
static void tpkg_register_package(const char *destdir, const char *pkgname)
{
    vfs_entry_t entries[64];
    int count = ramfs_list(destdir, entries, 64);
    int registered_any = 0;

    for (int i = 0; i < count; i++) {
        if (entries[i].is_dir) continue;
        int name_len = strlen(entries[i].name);
        if (name_len <= 4 || strcmp(entries[i].name + name_len - 4, ".txt") != 0)
            continue;

        char base_name[VFS_NAME_LEN];
        int base_len = name_len - 4;
        int k = 0;
        while (k < base_len && k < VFS_NAME_LEN - 1) { base_name[k] = entries[i].name[k]; k++; }
        base_name[k] = 0;

        int base_ok = 0;
        for (int j = 0; j < count; j++) {
            if (!entries[j].is_dir && strcmp(entries[j].name, base_name) == 0) { base_ok = 1; break; }
        }
        if (!base_ok) continue;

        char sidecar_path[192];
        int p = 0;
        while (destdir[p] && p < 160) { sidecar_path[p] = destdir[p]; p++; }
        sidecar_path[p++] = '/';
        int q = 0;
        while (entries[i].name[q] && p < 190) sidecar_path[p++] = entries[i].name[q++];
        sidecar_path[p] = 0;

        uint32_t sc_size = ramfs_size(sidecar_path);
        if (sc_size == 0 || sc_size > 60) continue;
        char cmdname[64];
        int n = ramfs_read(sidecar_path, cmdname, sc_size, 0);
        if (n <= 0) continue;
        cmdname[n] = 0;
        trim_trailing_ws(cmdname);
        if (!cmdname[0]) continue;

        char full_path[192];
        p = 0;
        while (destdir[p] && p < 160) { full_path[p] = destdir[p]; p++; }
        full_path[p++] = '/';
        q = 0;
        while (base_name[q] && p < 190) full_path[p++] = base_name[q++];
        full_path[p] = 0;

        tpkg_path_register(cmdname, full_path);
        registered_any = 1;
    }

    if (!registered_any) {
        char full_path[192];
        int p = 0;
        while (destdir[p] && p < 150) { full_path[p] = destdir[p]; p++; }
        full_path[p] = 0;
        int base = p;
        full_path[base++] = '/';
        int q = 0;
        while (pkgname[q] && base < 180) full_path[base++] = pkgname[q++];
        full_path[base] = 0;
        int ext_at = base;

        strcat(full_path, ".py");
        if (ramfs_exists(full_path)) {
            tpkg_path_register(pkgname, full_path);
            return;
        }
        full_path[ext_at] = 0;
        strcat(full_path, ".t");
        if (ramfs_exists(full_path)) {
            tpkg_path_register(pkgname, full_path);
        }
    }
}

/* Drops every path.tmbl line whose target lives under `destdir/` --
 * used by `tpkg remove` to undo whatever tpkg_register_package()
 * (or tpkg_path_register() directly) added for this package,
 * whether that was one line (no sidecars) or several (one per
 * sidecar-declared command). */
static void tpkg_path_unregister_dir(const char *destdir)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_PATH_FILE, &len);
    if (!buf) return;

    char *out = (char *)malloc(len + 1);
    if (!out) { free(buf); return; }
    int out_len = 0;
    int dir_len = strlen(destdir);

    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *eq = NULL;
        for (int i = 0; i < line_len; i++) {
            if (line_start[i] == '=') { eq = line_start + i; break; }
        }
        int drop = 0;
        if (eq) {
            char *val = eq + 1;
            int val_len = line_len - (int)(val - line_start);
            if (val_len > dir_len && strncmp(val, destdir, dir_len) == 0 && val[dir_len] == '/')
                drop = 1;
        }
        if (!drop && line_len > 0) {
            memcpy(out + out_len, line_start, line_len);
            out_len += line_len;
            out[out_len++] = '\n';
        }
    }

    tpkg_write_whole(TPKG_PATH_FILE, out, out_len);
    free(out);
    free(buf);
}

/* Adds or updates one "name|version|destdir\n" line in the local
 * installed-packages database (/sys/tpkg_db.tmbl) -- rewrites the
 * whole file with any existing line for this name replaced, since
 * ramfs_write() can't shrink a file for a plain in-place edit. */
static void tpkg_db_upsert(const char *name, const char *version, const char *destdir)
{
    int len = 0;
    char *buf = tpkg_read_whole(TPKG_DB_FILE, &len);
    int name_len = strlen(name);

    int cap = (buf ? len : 0) + strlen(name) + strlen(version) + strlen(destdir) + 8;
    char *out = (char *)malloc(cap);
    if (!out) { if (buf) free(buf); return; }
    int out_len = 0;

    if (buf) {
        char *p = buf;
        while (*p) {
            char *line_start = p;
            while (*p && *p != '\n') p++;
            int line_len = (int)(p - line_start);
            if (*p == '\n') p++;

            char *bar = NULL;
            for (int i = 0; i < line_len; i++) {
                if (line_start[i] == '|') { bar = line_start + i; break; }
            }
            int is_match = bar && (bar - line_start) == name_len &&
                           strncmp(line_start, name, name_len) == 0;
            if (!is_match && line_len > 0) {
                memcpy(out + out_len, line_start, line_len);
                out_len += line_len;
                out[out_len++] = '\n';
            }
        }
        free(buf);
    }

    const char *s = name;
    while (*s) out[out_len++] = *s++;
    out[out_len++] = '|';
    s = version;
    while (*s) out[out_len++] = *s++;
    out[out_len++] = '|';
    s = destdir;
    while (*s) out[out_len++] = *s++;
    out[out_len++] = '\n';

    tpkg_write_whole(TPKG_DB_FILE, out, out_len);
    free(out);
}

/* Drops the line for `name` from the local installed-packages
 * database, if present. */
static void tpkg_db_remove(const char *name)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_DB_FILE, &len);
    if (!buf) return;
    int name_len = strlen(name);

    char *out = (char *)malloc(len + 1);
    if (!out) { free(buf); return; }
    int out_len = 0;

    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *bar = NULL;
        for (int i = 0; i < line_len; i++) {
            if (line_start[i] == '|') { bar = line_start + i; break; }
        }
        int is_match = bar && (bar - line_start) == name_len &&
                       strncmp(line_start, name, name_len) == 0;
        if (!is_match && line_len > 0) {
            memcpy(out + out_len, line_start, line_len);
            out_len += line_len;
            out[out_len++] = '\n';
        }
    }

    tpkg_write_whole(TPKG_DB_FILE, out, out_len);
    free(out);
    free(buf);
}

/* Looks up `name`'s installed destdir (and optionally its recorded
 * version) in the local database. Returns 1 if found. Both out
 * buffers are 128 bytes; pass NULL for version_out to skip it. */
static int tpkg_db_find(const char *name, char *destdir_out, char *version_out)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_DB_FILE, &len);
    if (!buf) return 0;
    int name_len = strlen(name);
    int found = 0;

    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *bar1 = NULL;
        for (int i = 0; i < line_len; i++) {
            if (line_start[i] == '|') { bar1 = line_start + i; break; }
        }
        if (bar1 && (bar1 - line_start) == name_len && strncmp(line_start, name, name_len) == 0) {
            char *bar2 = NULL;
            for (char *q = bar1 + 1; q < line_start + line_len; q++) {
                if (*q == '|') { bar2 = q; break; }
            }
            if (bar2) {
                if (version_out) {
                    int vlen = (int)(bar2 - (bar1 + 1));
                    int k = 0;
                    while (k < vlen && k < 127) { version_out[k] = bar1[1 + k]; k++; }
                    version_out[k] = 0;
                }
                if (destdir_out) {
                    int dlen = (int)(line_start + line_len - (bar2 + 1));
                    int k = 0;
                    while (k < dlen && k < 127) { destdir_out[k] = bar2[1 + k]; k++; }
                    destdir_out[k] = 0;
                }
                found = 1;
            }
            break;
        }
    }
    free(buf);
    return found;
}

/* Looks up `name`'s version in the cached server list
 * (/sys/tpkg_cache.tmbl, refreshed by `tpkg update`). Returns 1 if
 * found. version_out is 64 bytes. */
static int tpkg_cache_find_version(const char *name, char *version_out)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_CACHE_FILE, &len);
    if (!buf) return 0;
    int name_len = strlen(name);
    int found = 0;

    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *bar1 = NULL;
        for (int i = 0; i < line_len; i++) {
            if (line_start[i] == '|') { bar1 = line_start + i; break; }
        }
        if (bar1 && (bar1 - line_start) == name_len && strncmp(line_start, name, name_len) == 0) {
            char *bar2 = NULL;
            for (char *q = bar1 + 1; q < line_start + line_len; q++) {
                if (*q == '|') { bar2 = q; break; }
            }
            int vend_len = bar2 ? (int)(bar2 - (bar1 + 1)) : (int)(line_start + line_len - (bar1 + 1));
            int k = 0;
            while (k < vend_len && k < 63) { version_out[k] = bar1[1 + k]; k++; }
            version_out[k] = 0;
            found = 1;
            break;
        }
    }
    free(buf);
    return found;
}

/* Looks up `name`'s license (3rd field) in the cached server list
 * (/sys/tpkg_cache.tmbl, refreshed by `tpkg update`). Returns 1 if
 * found. license_out is 64 bytes. */
static int tpkg_cache_find_license(const char *name, char *license_out)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_CACHE_FILE, &len);
    if (!buf) return 0;
    int name_len = strlen(name);
    int found = 0;

    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *bar1 = NULL;
        for (int i = 0; i < line_len; i++) {
            if (line_start[i] == '|') { bar1 = line_start + i; break; }
        }
        if (bar1 && (bar1 - line_start) == name_len && strncmp(line_start, name, name_len) == 0) {
            char *bar2 = NULL;
            for (char *q = bar1 + 1; q < line_start + line_len; q++) {
                if (*q == '|') { bar2 = q; break; }
            }
            if (!bar2) break; /* older 3-field entry, no license present */
            char *bar3 = NULL;
            for (char *q = bar2 + 1; q < line_start + line_len; q++) {
                if (*q == '|') { bar3 = q; break; }
            }
            int lend_len = bar3 ? (int)(bar3 - (bar2 + 1)) : (int)(line_start + line_len - (bar2 + 1));
            int k = 0;
            while (k < lend_len && k < 63) { license_out[k] = bar2[1 + k]; k++; }
            license_out[k] = 0;
            found = 1;
            break;
        }
    }
    free(buf);
    return found;
}

static int tpkg_resolve(uint32_t *ip)
{
    terminal_writestring("Resolving " TPKG_HOST "... ");
    int rc = dns_resolve(TPKG_HOST, ip);
    if (rc != 0) {
        terminal_writestring("FAILED (");
        terminal_writestring(dns_strerror(rc));
        terminal_writestring(")\n");
        return -1;
    }
    terminal_writestring("OK\n");
    return 0;
}

/* Prints a "name|version|description" list (either the raw server
 * response body or the cached copy of it) as a formatted table. */
static void tpkg_print_list_body(char *body)
{
    terminal_writestring("NAME            VERSION    LICENSE              DESCRIPTION\n");
    char *line = body;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line) {
            char *bar1 = strchr(line, '|');
            char *bar2 = bar1 ? strchr(bar1 + 1, '|') : 0;
            char *bar3 = bar2 ? strchr(bar2 + 1, '|') : 0;
            if (bar1 && bar2 && bar3) {
                *bar1 = '\0'; *bar2 = '\0'; *bar3 = '\0';
                const char *name = line, *ver = bar1 + 1, *lic = bar2 + 1, *desc = bar3 + 1;
                terminal_writestring(name);
                int pad = 16 - (int)strlen(name);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(ver);
                pad = 11 - (int)strlen(ver);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(lic);
                pad = 21 - (int)strlen(lic);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(desc);
                terminal_putchar('\n');
            } else if (bar1 && bar2) {
                /* backward-compat: older 3-field cache entries */
                *bar1 = '\0'; *bar2 = '\0';
                const char *name = line, *ver = bar1 + 1, *desc = bar2 + 1;
                terminal_writestring(name);
                int pad = 16 - (int)strlen(name);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(ver);
                pad = 11 - (int)strlen(ver);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring("?");
                pad = 20;
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(desc);
                terminal_putchar('\n');
            }
        }

        if (!nl) break;
        line = nl + 1;
    }
}

/* Fetches /api/list from the server and saves the raw body to the
 * local cache (/sys/tpkg_cache.tmbl) -- `tpkg look`/`tpkg upgrade`
 * read that cache instead of hitting the network every time.
 * Returns 1 on success. */
static int tpkg_update(uint32_t ip, int quiet)
{
    uint8_t *resp = (uint8_t *)malloc(TPKG_BUF_SIZE);
    if (!resp) { terminal_writestring("tpkg: out of memory\n"); return 0; }

    int n = http_get(ip, TPKG_HOST, TPKG_PORT, "/api/list", resp, TPKG_BUF_SIZE - 1);
    if (n < 0) {
        terminal_writestring("tpkg: update FAILED (");
        terminal_writestring(http_strerror(n));
        terminal_writestring(")\n");
        free(resp);
        return 0;
    }
    if (n == 0) { terminal_writestring("tpkg: server returned no data\n"); free(resp); return 0; }
    resp[n] = '\0';

    char *body = strstr((char *)resp, "\r\n\r\n");
    if (!body) { terminal_writestring("tpkg: malformed server response\n"); free(resp); return 0; }
    body += 4;
    int body_len = n - (int)(body - (char *)resp);

    tpkg_write_whole(TPKG_CACHE_FILE, body, body_len);
    if (!quiet) {
        terminal_writestring("Package list updated (");
        int count = 0;
        for (char *p = body; *p; p++) if (*p == '\n') count++;
        print_uint((uint32_t)count);
        terminal_writestring(" package(s))\n");
    }
    free(resp);
    return 1;
}

/* `tpkg look`: shows the server's package list from the local cache,
 * auto-refreshing it first if there isn't one yet. */
static void tpkg_look(uint32_t ip)
{
    if (!ramfs_exists(TPKG_CACHE_FILE)) {
        if (!tpkg_update(ip, 1)) return;
    }
    int len;
    char *buf = tpkg_read_whole(TPKG_CACHE_FILE, &len);
    if (!buf) { terminal_writestring("tpkg: no cached package list\n"); return; }
    tpkg_print_list_body(buf);
    free(buf);
}

/* `tpkg list`: shows locally *installed* packages (from
 * /sys/tpkg_db.tmbl), purely local -- no network needed. */
static void tpkg_list_installed(void)
{
    int len;
    char *buf = tpkg_read_whole(TPKG_DB_FILE, &len);
    if (!buf) { terminal_writestring("(no packages installed)\n"); return; }

    terminal_writestring("NAME            VERSION    DIR\n");
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (*line) {
            char *bar1 = strchr(line, '|');
            char *bar2 = bar1 ? strchr(bar1 + 1, '|') : 0;
            if (bar1 && bar2) {
                *bar1 = '\0'; *bar2 = '\0';
                const char *name = line, *ver = bar1 + 1, *dir = bar2 + 1;
                terminal_writestring(name);
                int pad = 16 - (int)strlen(name);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(ver);
                pad = 11 - (int)strlen(ver);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(dir);
                terminal_putchar('\n');
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
}

static void tpkg_install(uint32_t ip, const char *name)
{
    char path[160];
    int pi = 0;
    const char *prefix = "/api/download/";
    while (*prefix) path[pi++] = *prefix++;
    const char *p = name;
    while (*p && pi < 150) path[pi++] = *p++;
    path[pi] = '\0';

    uint8_t *resp = (uint8_t *)malloc(TPKG_BUF_SIZE);
    if (!resp) { terminal_writestring("tpkg: out of memory\n"); return; }

    terminal_writestring("Downloading ");
    terminal_writestring(path);
    terminal_writestring("... ");
    int n = http_get(ip, TPKG_HOST, TPKG_PORT, path, resp, TPKG_BUF_SIZE - 1);
    if (n < 0) {
        terminal_writestring("FAILED (");
        terminal_writestring(http_strerror(n));
        terminal_writestring(")\n");
        free(resp);
        return;
    }
    if (n == 0) { terminal_writestring("FAILED (no data)\n"); free(resp); return; }

    char *body = strstr((char *)resp, "\r\n\r\n");
    if (!body) { terminal_writestring("FAILED (malformed server response)\n"); free(resp); return; }
    body += 4;
    int body_len = n - (int)(body - (char *)resp);
    if (body_len <= 0) { terminal_writestring("FAILED (empty package archive -- check the name)\n"); free(resp); return; }

    terminal_writestring("OK (");
    print_uint((uint32_t)body_len);
    terminal_writestring(" bytes)\n");

    char tarfile[160];
    pi = 0;
    const char *tprefix = "/tmp_tpkg_";
    while (*tprefix) tarfile[pi++] = *tprefix++;
    p = name;
    while (*p && pi < 140) tarfile[pi++] = *p++;
    const char *tsuffix = ".tar";
    while (*tsuffix) tarfile[pi++] = *tsuffix++;
    tarfile[pi] = '\0';

    if (ramfs_exists(tarfile)) ramfs_delete(tarfile);
    if (ramfs_create(tarfile) != 0) {
        terminal_writestring("tpkg: failed to create ");
        terminal_writestring(tarfile);
        terminal_putchar('\n');
        free(resp);
        return;
    }
    ramfs_write(tarfile, body, (uint32_t)body_len, 0);
    free(resp);

    char destdir[160];
    pi = 0;
    const char *dprefix = "/programs/";
    while (*dprefix) destdir[pi++] = *dprefix++;
    p = name;
    while (*p && pi < 150) destdir[pi++] = *p++;
    destdir[pi] = '\0';

    if (!ramfs_exists(destdir)) {
        if (ramfs_mkdir(destdir) != 0) {
            terminal_writestring("tpkg: failed to create ");
            terminal_writestring(destdir);
            terminal_putchar('\n');
            ramfs_delete(tarfile);
            return;
        }
    }

    char err[128];
    int cnt = tar_extract(tarfile, destdir, err, sizeof(err));
    ramfs_delete(tarfile);

    if (cnt < 0) {
        terminal_writestring("tpkg: extract FAILED (");
        terminal_writestring(err);
        terminal_writestring(")\n");
        return;
    }
    tpkg_register_package(destdir, name);

    char version[64];
    if (!tpkg_cache_find_version(name, version)) {
        version[0] = '?'; version[1] = 0;
    }
    tpkg_db_upsert(name, version, destdir);

    char license[64];
    if (!tpkg_cache_find_license(name, license)) {
        license[0] = '?'; license[1] = 0;
    }

    terminal_writestring("Installed to ");
    terminal_writestring(destdir);
    terminal_writestring(" (");
    print_uint((uint32_t)cnt);
    terminal_writestring(" files)\n");
    terminal_writestring("Version: ");
    terminal_writestring(version);
    terminal_writestring("  License: ");
    terminal_writestring(license);
    terminal_putchar('\n');
}

/* `tpkg remove <name>`: deletes every file the package's directory
 * holds, the (now-empty) directory itself, its path.tmbl entries,
 * and its line in the local database. Purely local -- no network. */
static void tpkg_remove(const char *name)
{
    char destdir[128];
    if (!tpkg_db_find(name, destdir, NULL)) {
        /* not in the db (installed before this feature existed?) --
         * fall back to the standard convention so removal still
         * works for it. */
        int p = 0;
        const char *prefix = "/programs/";
        while (prefix[p]) { destdir[p] = prefix[p]; p++; }
        int q = 0;
        while (name[q] && p < 120) destdir[p++] = name[q++];
        destdir[p] = 0;
        if (!ramfs_exists(destdir)) {
            terminal_writestring("tpkg: not installed: ");
            terminal_writestring(name);
            terminal_putchar('\n');
            return;
        }
    }

    vfs_entry_t entries[64];
    int count = ramfs_list(destdir, entries, 64);
    for (int i = 0; i < count; i++) {
        if (entries[i].is_dir) continue;
        char file_path[192];
        int p = 0;
        while (destdir[p] && p < 160) { file_path[p] = destdir[p]; p++; }
        file_path[p++] = '/';
        int q = 0;
        while (entries[i].name[q] && p < 190) file_path[p++] = entries[i].name[q++];
        file_path[p] = 0;
        ramfs_delete(file_path);
    }
    ramfs_rmdir(destdir);

    tpkg_path_unregister_dir(destdir);
    tpkg_db_remove(name);

    terminal_writestring("Removed ");
    terminal_writestring(name);
    terminal_writestring(" (");
    terminal_writestring(destdir);
    terminal_writestring(")\n");
}

/* `tpkg upgrade`: reinstalls any locally-installed package whose
 * recorded version doesn't match what's in the cached server list.
 * Needs a cache -- run `tpkg update` first if there isn't one. */
static void tpkg_upgrade(uint32_t ip)
{
    if (!ramfs_exists(TPKG_CACHE_FILE)) {
        terminal_writestring("tpkg: no cached package list, run `tpkg update` first\n");
        return;
    }
    int len;
    char *buf = tpkg_read_whole(TPKG_DB_FILE, &len);
    if (!buf) { terminal_writestring("(no packages installed)\n"); return; }

    /* names are extracted up front into a fixed table before doing
     * any installs, since tpkg_install() rewrites tpkg_db.tmbl (via
     * tpkg_db_upsert()) -- walking `buf` (a snapshot from before any
     * of that) while also calling functions that mutate the live
     * file on disk is fine, but the *installed* line for whichever
     * name update it changes for should never be re-read out of buf
     * again. */
    char names[32][64];
    char installed_vers[32][64];
    int n = 0;
    char *p = buf;
    while (*p && n < 32) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;
        char *bar1 = NULL;
        for (int i = 0; i < line_len; i++) if (line_start[i] == '|') { bar1 = line_start + i; break; }
        if (bar1 && line_len > 0) {
            int nlen = (int)(bar1 - line_start);
            int k = 0;
            while (k < nlen && k < 63) { names[n][k] = line_start[k]; k++; }
            names[n][k] = 0;
            char *bar2 = NULL;
            for (char *q = bar1 + 1; q < line_start + line_len; q++) if (*q == '|') { bar2 = q; break; }
            int vlen = bar2 ? (int)(bar2 - (bar1 + 1)) : 0;
            k = 0;
            while (k < vlen && k < 63) { installed_vers[n][k] = bar1[1 + k]; k++; }
            installed_vers[n][k] = 0;
            n++;
        }
    }
    free(buf);

    int upgraded = 0;
    for (int i = 0; i < n; i++) {
        char server_ver[64];
        if (!tpkg_cache_find_version(names[i], server_ver)) continue; /* no longer on the server */
        if (strcmp(server_ver, installed_vers[i]) == 0) continue; /* already current */
        terminal_writestring("Upgrading ");
        terminal_writestring(names[i]);
        terminal_writestring(" (");
        terminal_writestring(installed_vers[i]);
        terminal_writestring(" -> ");
        terminal_writestring(server_ver);
        terminal_writestring(")...\n");
        tpkg_install(ip, names[i]);
        upgraded++;
    }
    if (!upgraded) terminal_writestring("Everything up to date.\n");
}

void cmd_tpkg(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(
            "usage: tpkg <subcommand>\n"
            "  list             locally installed packages (no network)\n"
            "  look             server's available packages (cached; use `update` to refresh)\n"
            "  update           refresh the cached server package list\n"
            "  install <name>   install a package\n"
            "  remove <name>    uninstall a package (no network)\n"
            "  upgrade          reinstall any installed package with a newer server version\n"
        );
        return;
    }

    /* list/remove are purely local -- don't force a DNS resolve for them. */
    if (strcmp(args[1], "list") == 0) {
        tpkg_list_installed();
        return;
    }
    if (strcmp(args[1], "remove") == 0) {
        if (argc < 3) { terminal_writestring("usage: tpkg remove <name>\n"); return; }
        tpkg_remove(args[2]);
        return;
    }

    uint32_t ip;
    if (tpkg_resolve(&ip) != 0) return;

    if (strcmp(args[1], "look") == 0) {
        tpkg_look(ip);
    } else if (strcmp(args[1], "update") == 0) {
        tpkg_update(ip, 0);
    } else if (strcmp(args[1], "install") == 0) {
        if (argc < 3) { terminal_writestring("usage: tpkg install <name>\n"); return; }
        tpkg_install(ip, args[2]);
    } else if (strcmp(args[1], "upgrade") == 0) {
        tpkg_upgrade(ip);
    } else {
        terminal_writestring("tpkg: unknown subcommand (try: list, look, update, install, remove, upgrade)\n");
    }
}
