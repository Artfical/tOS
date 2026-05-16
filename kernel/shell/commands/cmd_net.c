#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "dns.h"
#include "icmp.h"
#include "http.h"
#include "tsharp.h"
#include "micropython.h"
#include "memory.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

static uint32_t parse_ip(const char *s)
{
    uint32_t ip = 0;
    int shift = 0, val = 0;
    while (*s) {
        if (*s == '.') { ip |= (val & 0xFF) << shift; shift += 8; val = 0; }
        else if (*s >= '0' && *s <= '9') val = val * 10 + (*s - '0');
        else return 0;
        s++;
    }
    ip |= (val & 0xFF) << shift;
    return ip;
}

void cmd_ping(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: ping <ip> or <hostname>\n"); return; }
    uint32_t ip;
    int is_ip = 1;
    for (char *p = args[1]; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { is_ip = 0; break; }
    if (is_ip) ip = parse_ip(args[1]);
    else {
        terminal_writestring("Resolving... ");
        if (dns_resolve(args[1], &ip) != 0)
            { terminal_writestring("FAILED\n"); return; }
        terminal_writestring("OK\n");
    }
    terminal_writestring("Pinging... ");
    if (icmp_ping(ip) == 0) terminal_writestring("Reply received\n");
    else terminal_writestring("No reply\n");
}

void cmd_wget(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: wget <url>\n");
        terminal_writestring("  e.g. wget http://example.com/file\n");
        return;
    }
    const char *url = args[1];
    if (strncmp(url, "http://", 7) != 0)
        { terminal_writestring("wget: Only http:// supported\n"); return; }
    url += 7;

    char host[256], path[256];
    int i = 0, j = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) host[i++] = *url++;
    host[i] = '\0';

    uint16_t port = 80;
    if (*url == ':') { url++; port = 0; while (*url >= '0' && *url <= '9') { port = port * 10 + (*url - '0'); url++; } }

    if (*url == '/') { while (*url && j < 255) path[j++] = *url++; path[j] = '\0'; }
    else { path[0] = '/'; path[1] = '\0'; }

    const char *fname = path;
    for (const char *p = path; *p; p++) if (*p == '/') fname = p + 1;
    if (*fname == '\0') fname = "downloaded";

    terminal_writestring("Resolving... ");
    uint32_t ip;
    if (dns_resolve(host, &ip) != 0) { terminal_writestring("FAILED\n"); return; }
    terminal_writestring("OK\nConnecting... ");

    uint8_t resp[4096];
    int n = http_get(host, port, path, resp, sizeof(resp) - 1);
    if (n <= 0) { terminal_writestring("FAILED\n"); return; }
    resp[n] = '\0';
    terminal_writestring("OK (");
    print_num(n);
    terminal_writestring(" bytes)\n");

    if (ramfs_create(fname) == 0) {
        ramfs_write(fname, (char *)resp, n, 0);
        terminal_writestring("Saved to: ");
        terminal_writestring(fname);
        terminal_writestring("\n");
    } else {
        terminal_writestring("(Cannot write, showing content)\n");
        terminal_writestring((char *)resp);
        terminal_putchar('\n');
    }
}

void cmd_tsharp(int argc, char **args)
{
    if (argc > 1) tsharp_run_file(args[1]);
    else tsharp_run_interactive();
}

void cmd_python(int argc, char **args)
{
    if (argc > 1) micropython_run_file(args[1]);
    else micropython_run_repl();
}
