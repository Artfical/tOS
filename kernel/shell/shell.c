#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "string.h"
#include "memory.h"
#include "ramfs.h"
#include "tsharp.h"
#include "micropython.h"
#include "elf.h"
#include "io.h"
#include "version.h"
#include "net.h"
#include "icmp.h"
#include "http.h"
#include "dns.h"

#define MAX_ARGS 16
#define MAX_CMD_LEN 512
#define EDIT_LINE_LEN 512

static int parse_args(char *cmd, char **args)
{
    int argc = 0;
    char *p = cmd;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    args[argc++] = p;
    while (*p) {
        if (*p == ' ') {
            *p = '\0';
            p++;
            while (*p == ' ') p++;
            if (*p && argc < MAX_ARGS) {
                args[argc++] = p;
                continue;
            }
            break;
        }
        p++;
    }
    return argc;
}

static void cmd_help(void)
{
    terminal_writestring("tOS Commands:\n");
    terminal_writestring("  help       - show help\n");
    terminal_writestring("  echo       - echo text\n");
    terminal_writestring("  clear      - clear screen\n");
    terminal_writestring("  pwd        - print working directory\n");
    terminal_writestring("  ls         - list files\n");
    terminal_writestring("  cd         - change directory\n");
    terminal_writestring("  mkdir      - create directory\n");
    terminal_writestring("  rmdir      - remove directory\n");
    terminal_writestring("  rm         - remove file\n");
    terminal_writestring("  touch      - create file\n");
    terminal_writestring("  cat        - show file contents\n");
    terminal_writestring("  mv         - move/rename file\n");
    terminal_writestring("  cp         - copy file\n");
    terminal_writestring("  edit       - simple line editor\n");
    terminal_writestring("  exec       - run ELF program\n");
    terminal_writestring("  tsharp     - run T# 4.1 Lite (interactive or file)\n");
    terminal_writestring("  reboot     - reboot system\n");
    terminal_writestring("  shutdown   - halt system\n");
    terminal_writestring("  version    - show version\n");
    terminal_writestring("  about      - about tOS\n");
    terminal_writestring("  uname      - system info\n");
    terminal_writestring("  ping       - ping a host\n");
    terminal_writestring("  wget       - download a file (HTTP)\n");
    terminal_writestring("  python     - MicroPython REPL (coming soon)\n");
}

static void cmd_echo(int argc, char **args)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) terminal_putchar(' ');
        terminal_writestring(args[i]);
    }
    terminal_putchar('\n');
}

static void cmd_clear(void)
{
    terminal_clear();
}

static void cmd_pwd(void)
{
    terminal_writestring(ramfs_getcwd());
    terminal_putchar('\n');
}

static void cmd_ls(int argc, char **args)
{
    const char *path = ramfs_getcwd();
    if (argc > 1) path = args[1];

    ramfs_entry_t entries[256];
    int count = ramfs_list(path, entries, 256);
    if (count < 0) {
        terminal_writestring("ls: ");
        terminal_writestring(path);
        terminal_writestring(": No such directory\n");
        return;
    }
    if (count == 0) return;
    for (int i = 0; i < count; i++) {
        if (entries[i].is_dir) terminal_writestring("d  ");
        else terminal_writestring("   ");
        terminal_writestring(entries[i].name);
        if (!entries[i].is_dir) {
            terminal_writestring(" (");
            char buf[16];
            int di = 0;
            uint32_t sz = entries[i].size;
            if (sz >= 10000000) { buf[di++] = '0' + sz / 10000000; sz %= 10000000; }
            if (di > 0 || sz >= 1000000) { buf[di++] = '0' + sz / 1000000; sz %= 1000000; }
            if (di > 0 || sz >= 100000) { buf[di++] = '0' + sz / 100000; sz %= 100000; }
            if (di > 0 || sz >= 10000) { buf[di++] = '0' + sz / 10000; sz %= 10000; }
            if (di > 0 || sz >= 1000) { buf[di++] = '0' + sz / 1000; sz %= 1000; }
            if (di > 0 || sz >= 100) { buf[di++] = '0' + sz / 100; sz %= 100; }
            if (di > 0 || sz >= 10) { buf[di++] = '0' + sz / 10; sz %= 10; }
            buf[di++] = '0' + sz;
            buf[di] = '\0';
            terminal_writestring(buf);
            terminal_writestring(" B)");
        }
        terminal_putchar('\n');
    }
}

static void cmd_cd(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(ramfs_getcwd());
        terminal_putchar('\n');
        return;
    }
    if (ramfs_chdir(args[1]) != 0) {
        terminal_writestring("cd: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such directory\n");
    }
}

static void cmd_mkdir(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: mkdir <dir>\n");
        return;
    }
    if (ramfs_mkdir(args[1]) != 0) {
        terminal_writestring("mkdir: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

static void cmd_rmdir(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: rmdir <dir>\n");
        return;
    }
    if (ramfs_delete(args[1]) != 0) {
        terminal_writestring("rmdir: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

static void cmd_rm(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: rm <file>\n");
        return;
    }
    if (ramfs_delete(args[1]) != 0) {
        terminal_writestring("rm: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

static void cmd_touch(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: touch <file>\n");
        return;
    }
    if (ramfs_create(args[1]) != 0) {
        terminal_writestring("touch: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

static void cmd_cat(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: cat <file>\n");
        return;
    }
    if (!ramfs_exists(args[1])) {
        terminal_writestring("cat: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    if (ramfs_is_dir(args[1])) {
        terminal_writestring("cat: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Is a directory\n");
        return;
    }
    uint32_t sz = ramfs_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        terminal_writestring("cat: Out of memory\n");
        return;
    }
    ramfs_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    terminal_writestring(buf);
    if (sz > 0 && buf[sz - 1] != '\n')
        terminal_putchar('\n');
    free(buf);
}

static void cmd_mv(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: mv <src> <dst>\n");
        return;
    }
    if (ramfs_rename(args[1], args[2]) != 0)
        terminal_writestring("mv: Failed\n");
}

static void cmd_cp(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: cp <src> <dst>\n");
        return;
    }
    if (!ramfs_exists(args[1]) || ramfs_is_dir(args[1])) {
        terminal_writestring("cp: Source not found or is a directory\n");
        return;
    }
    if (ramfs_exists(args[2])) {
        terminal_writestring("cp: Destination exists\n");
        return;
    }
    if (ramfs_create(args[2]) != 0) {
        terminal_writestring("cp: Failed to create destination\n");
        return;
    }
    uint32_t sz = ramfs_size(args[1]);
    char *buf = (char *)malloc(sz);
    if (!buf) {
        terminal_writestring("cp: Out of memory\n");
        ramfs_delete(args[2]);
        return;
    }
    ramfs_read(args[1], buf, sz, 0);
    ramfs_write(args[2], buf, sz, 0);
    free(buf);
}

static void cmd_edit(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: edit <file>\n");
        terminal_writestring("  Type each line then press Enter\n");
        terminal_writestring("  Type .s on a line to save\n");
        terminal_writestring("  Type .q on a line to quit\n");
        return;
    }
    const char *filename = args[1];
    int exists = ramfs_exists(filename);
    if (!exists) {
        if (ramfs_create(filename) != 0) {
            terminal_writestring("edit: Failed to create file\n");
            return;
        }
    }

    terminal_writestring("edit: ");
    terminal_writestring(filename);
    terminal_writestring(" - (.s=save, .q=quit)\n");

    char line[EDIT_LINE_LEN];
    uint32_t offset = 0;
    if (exists) offset = ramfs_size(filename);

    for (;;) {
        terminal_writestring("> ");
        keyboard_readline(line, EDIT_LINE_LEN);

        if (strcmp(line, ".q") == 0) {
            terminal_writestring("Not saved.\n");
            if (!exists) ramfs_delete(filename);
            return;
        }
        if (strcmp(line, ".s") == 0) {
            terminal_writestring("Saved.\n");
            return;
        }

        ramfs_write(filename, line, strlen(line), offset);
        ramfs_write(filename, "\n", 1, offset + strlen(line));
        offset += strlen(line) + 1;
    }
}

static void cmd_exec(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: exec <program>\n");
        return;
    }
    if (!ramfs_exists(args[1]) || ramfs_is_dir(args[1])) {
        terminal_writestring("exec: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Not found\n");
        return;
    }

    uint32_t sz = ramfs_size(args[1]);
    void *prog = malloc(sz);
    if (!prog) {
        terminal_writestring("exec: Out of memory\n");
        return;
    }
    ramfs_read(args[1], prog, sz, 0);

    terminal_writestring("Loading: ");
    terminal_writestring(args[1]);
    terminal_putchar('\n');

    uint32_t entry = 0;
    if (elf_load(prog, &entry) != 0) {
        terminal_writestring("exec: ELF load failed\n");
        free(prog);
        return;
    }

    char buf[16];
    for (int i = 0; i < 8; i++) {
        buf[7 - i] = "0123456789ABCDEF"[entry & 0xF];
        entry >>= 4;
    }
    buf[8] = '\0';
    terminal_writestring("Entry: 0x");
    terminal_writestring(buf);
    terminal_putchar('\n');
    free(prog);
}

static void cmd_reboot(void)
{
    terminal_writestring("Rebooting...\n");
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    asm volatile("hlt");
}

static void cmd_shutdown(void)
{
    terminal_writestring("System halted.\n");
    for (;;) { asm volatile("hlt"); }
}

static void cmd_version(void)
{
    terminal_writestring(TOS_VERSION_STRING "\n");
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");
}

static void cmd_about(void)
{
    terminal_writestring("tOS - talOS\n");
    terminal_writestring("License: GNU AGPL v3\n");
}

static void cmd_uname(void)
{
    terminal_writestring("tOS\n");
}

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

static void cmd_ping(int argc, char **args)
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

static void cmd_wget(int argc, char **args)
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

void shell_init(void)
{
    terminal_writestring(TOS_WELCOME_STRING);
    terminal_writestring("Type 'help' for commands\n\n");
}

void shell_run(void)
{
    char cmd_line[MAX_CMD_LEN];
    char *args[MAX_ARGS];

    for (;;) {
        gui_poll();
        terminal_writestring(ramfs_getcwd());
        terminal_writestring("> ");

        keyboard_readline(cmd_line, MAX_CMD_LEN);

        int argc = parse_args(cmd_line, args);

        if (argc == 0) continue;

        const char *c = args[0];

        if (strcmp(c, "help") == 0) {
            cmd_help();
        } else if (strcmp(c, "echo") == 0) {
            cmd_echo(argc, args);
        } else if (strcmp(c, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(c, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(c, "ls") == 0) {
            cmd_ls(argc, args);
        } else if (strcmp(c, "cd") == 0) {
            cmd_cd(argc, args);
        } else if (strcmp(c, "mkdir") == 0) {
            cmd_mkdir(argc, args);
        } else if (strcmp(c, "rmdir") == 0) {
            cmd_rmdir(argc, args);
        } else if (strcmp(c, "rm") == 0) {
            cmd_rm(argc, args);
        } else if (strcmp(c, "touch") == 0) {
            cmd_touch(argc, args);
        } else if (strcmp(c, "cat") == 0) {
            cmd_cat(argc, args);
        } else if (strcmp(c, "mv") == 0) {
            cmd_mv(argc, args);
        } else if (strcmp(c, "cp") == 0) {
            cmd_cp(argc, args);
        } else if (strcmp(c, "edit") == 0) {
            cmd_edit(argc, args);
        } else if (strcmp(c, "tsharp") == 0) {
            if (argc > 1) tsharp_run_file(args[1]);
            else tsharp_run_interactive();
        } else if (strcmp(c, "exec") == 0) {
            cmd_exec(argc, args);
        } else if (strcmp(c, "reboot") == 0) {
            cmd_reboot();
        } else if (strcmp(c, "shutdown") == 0) {
            cmd_shutdown();
        } else if (strcmp(c, "version") == 0) {
            cmd_version();
        } else if (strcmp(c, "about") == 0) {
            cmd_about();
        } else if (strcmp(c, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(c, "ping") == 0) {
            cmd_ping(argc, args);
        } else if (strcmp(c, "wget") == 0) {
            cmd_wget(argc, args);
        } else if (strcmp(c, "python") == 0) {
            terminal_writestring("MicroPython: not yet available\n");
            terminal_writestring("  Build MicroPython from https://github.com/micropython/micropython\n");
            terminal_writestring("  and place in kernel/micropython/\n");
        } else {
            terminal_writestring("Unknown command: ");
            terminal_writestring(c);
            terminal_putchar('\n');
        }
    }
}
