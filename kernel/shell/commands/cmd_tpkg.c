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
 * `tpkg list` hits /api/list (plain text, one "name|version|desc" per
 * line); `tpkg install <name>` hits /api/download/<name> (a raw
 * ustar .tar), saves it to ramfs, and extracts it into
 * /programs/<name>/ with the tar support this kernel already has. */
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

static void tpkg_list(uint32_t ip)
{
    uint8_t *resp = (uint8_t *)malloc(TPKG_BUF_SIZE);
    if (!resp) { terminal_writestring("tpkg: out of memory\n"); return; }

    int n = http_get(ip, TPKG_HOST, TPKG_PORT, "/api/list", resp, TPKG_BUF_SIZE - 1);
    if (n < 0) {
        terminal_writestring("tpkg: list FAILED (");
        terminal_writestring(http_strerror(n));
        terminal_writestring(")\n");
        free(resp);
        return;
    }
    if (n == 0) { terminal_writestring("tpkg: server returned no data\n"); free(resp); return; }
    resp[n] = '\0';

    char *body = strstr((char *)resp, "\r\n\r\n");
    if (!body) { terminal_writestring("tpkg: malformed server response\n"); free(resp); return; }
    body += 4;

    terminal_writestring("NAME            VERSION    DESCRIPTION\n");
    char *line = body;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line) {
            char *bar1 = strchr(line, '|');
            char *bar2 = bar1 ? strchr(bar1 + 1, '|') : 0;
            if (bar1 && bar2) {
                *bar1 = '\0'; *bar2 = '\0';
                const char *name = line, *ver = bar1 + 1, *desc = bar2 + 1;
                terminal_writestring(name);
                int pad = 16 - (int)strlen(name);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(ver);
                pad = 11 - (int)strlen(ver);
                for (int k = 0; k < pad; k++) terminal_putchar(' ');
                terminal_writestring(desc);
                terminal_putchar('\n');
            }
        }

        if (!nl) break;
        line = nl + 1;
    }
    free(resp);
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

    terminal_writestring("Installed to ");
    terminal_writestring(destdir);
    terminal_writestring(" (");
    print_uint((uint32_t)cnt);
    terminal_writestring(" files)\n");
}

void cmd_tpkg(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: tpkg list | tpkg install <name>\n");
        return;
    }

    uint32_t ip;
    if (tpkg_resolve(&ip) != 0) return;

    if (strcmp(args[1], "list") == 0) {
        tpkg_list(ip);
    } else if (strcmp(args[1], "install") == 0) {
        if (argc < 3) { terminal_writestring("usage: tpkg install <name>\n"); return; }
        tpkg_install(ip, args[2]);
    } else {
        terminal_writestring("tpkg: unknown subcommand (try: list, install)\n");
    }
}
