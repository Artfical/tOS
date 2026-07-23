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
