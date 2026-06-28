#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "fsbridge.h"
#include "memory.h"
#include "stdlib.h"
#include "keyboard.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

void cmd_cat(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: cat <file>\n");
        return;
    }
    if (!fsbridge_exists(args[1])) {
        terminal_writestring("cat: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    if (fsbridge_is_dir(args[1])) {
        terminal_writestring("cat: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Is a directory\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        terminal_writestring("cat: Out of memory\n");
        return;
    }
    fsbridge_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    terminal_writestring(buf);
    if (sz > 0 && buf[sz - 1] != '\n')
        terminal_putchar('\n');
    free(buf);
}

void cmd_head(int argc, char **args)
{
    int n = 10;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(args[i], "-n") == 0 && i + 1 < argc)
            n = atoi(args[++i]);
        else
            file = args[i];
    }
    if (!file) {
        terminal_writestring("usage: head [-n <lines>] <file>\n");
        return;
    }
    if (!fsbridge_exists(file) || fsbridge_is_dir(file)) {
        terminal_writestring("head: ");
        terminal_writestring(file);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(file);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(file, buf, sz, 0);
    buf[sz] = '\0';
    int lines = 0;
    for (char *p = buf; *p && lines < n; p++) {
        terminal_putchar(*p);
        if (*p == '\n') lines++;
    }
    if (sz > 0 && buf[sz - 1] != '\n' && lines < n)
        terminal_putchar('\n');
    free(buf);
}

void cmd_tail(int argc, char **args)
{
    int n = 10;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(args[i], "-n") == 0 && i + 1 < argc)
            n = atoi(args[++i]);
        else
            file = args[i];
    }
    if (!file) {
        terminal_writestring("usage: tail [-n <lines>] <file>\n");
        return;
    }
    if (!fsbridge_exists(file) || fsbridge_is_dir(file)) {
        terminal_writestring("tail: ");
        terminal_writestring(file);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(file);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(file, buf, sz, 0);
    buf[sz] = '\0';
    int total = 0;
    for (char *p = buf; *p; p++)
        if (*p == '\n') total++;
    int count = 0;
    char *start = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '\n') {
            count++;
            if (count > total - n) break;
            start = p + 1;
        }
    }
    terminal_writestring(start);
    if (sz > 0 && buf[sz - 1] != '\n')
        terminal_putchar('\n');
    free(buf);
}

void cmd_wc(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: wc <file>\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("wc: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    int lines = 0, words = 0, chars = sz;
    int in_word = 0;
    for (char *p = buf; *p; p++) {
        if (*p == '\n') lines++;
        if (*p == ' ' || *p == '\n' || *p == '\t') { in_word = 0; }
        else if (!in_word) { in_word = 1; words++; }
    }
    print_num(lines);
    terminal_putchar(' ');
    print_num(words);
    terminal_putchar(' ');
    print_num(chars);
    terminal_putchar(' ');
    terminal_writestring(args[1]);
    terminal_putchar('\n');
    free(buf);
}

void cmd_sort(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: sort <file>\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("sort: No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    int lines = 0;
    for (char *p = buf; *p; p++) if (*p == '\n') lines++;
    if (lines == 0) { free(buf); return; }
    char **line = (char **)malloc(sizeof(char *) * lines);
    if (!line) { free(buf); return; }
    int idx = 0;
    line[idx++] = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (*(p + 1)) line[idx++] = p + 1;
        }
    }
    for (int i = 0; i < lines - 1; i++)
        for (int j = 0; j < lines - 1 - i; j++)
            if (strcmp(line[j], line[j + 1]) > 0) {
                char *t = line[j];
                line[j] = line[j + 1];
                line[j + 1] = t;
            }
    for (int i = 0; i < lines; i++) {
        terminal_writestring(line[i]);
        terminal_putchar('\n');
    }
    free(line);
    free(buf);
}

void cmd_grep(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: grep <pattern> <file> [file...]\n");
        return;
    }
    const char *pattern = args[1];
    int any = 0;
    for (int fi = 2; fi < argc; fi++) {
        if (!fsbridge_exists(args[fi]) || fsbridge_is_dir(args[fi])) continue;
        uint32_t sz = fsbridge_size(args[fi]);
        char *buf = (char *)malloc(sz + 1);
        if (!buf) continue;
        fsbridge_read(args[fi], buf, sz, 0);
        buf[sz] = '\0';
        char *line = buf;
        while (*line) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';
            if (strstr(line, pattern)) {
                any = 1;
                if (argc > 3) {
                    terminal_writestring(args[fi]);
                    terminal_writestring(": ");
                }
                terminal_writestring(line);
                terminal_putchar('\n');
            }
            if (!next) break;
            line = next + 1;
        }
        free(buf);
    }
    if (!any) {
        terminal_writestring("grep: No matches\n");
    }
}

static void print_hex_byte(uint8_t b)
{
    char hex[3];
    hex[0] = "0123456789ABCDEF"[(b >> 4) & 0xF];
    hex[1] = "0123456789ABCDEF"[b & 0xF];
    hex[2] = 0;
    terminal_writestring(hex);
}

void cmd_hexdump(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: hexdump <file> [length]\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("hexdump: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    uint32_t len = sz;
    if (argc > 2) {
        len = (uint32_t)atoi(args[2]);
        if (len > sz) len = sz;
    }
    char *buf = (char *)malloc(len);
    if (!buf) { terminal_writestring("hexdump: Out of memory\n"); return; }
    fsbridge_read(args[1], buf, len, 0);

    uint32_t offset = 0;
    while (offset < len) {
        print_hex_byte((offset >> 24) & 0xFF);
        print_hex_byte((offset >> 16) & 0xFF);
        print_hex_byte((offset >> 8) & 0xFF);
        print_hex_byte(offset & 0xFF);
        terminal_writestring("  ");

        for (int i = 0; i < 16; i++) {
            if (offset + i < len)
                print_hex_byte((uint8_t)buf[offset + i]);
            else
                terminal_writestring("  ");
            if (i == 7) terminal_putchar(' ');
        }

        terminal_writestring("  |");
        for (int i = 0; i < 16 && offset + i < len; i++) {
            char c = buf[offset + i];
            if (c >= 32 && c < 127) terminal_putchar(c);
            else terminal_putchar('.');
        }
        terminal_writestring("|\n");
        offset += 16;
    }

    free(buf);
}

void cmd_rev(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: rev <file>\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("rev: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    char *line = buf;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        for (int i = strlen(line) - 1; i >= 0; i--)
            terminal_putchar(line[i]);
        terminal_putchar('\n');
        if (!next) break;
        line = next + 1;
    }
    free(buf);
}

void cmd_uniq(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: uniq <file>\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("uniq: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    fsbridge_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    char *line = buf;
    char *prev = NULL;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        if (!prev || strcmp(prev, line) != 0) {
            terminal_writestring(line);
            terminal_putchar('\n');
        }
        prev = line;
        if (!next) break;
        line = next + 1;
    }
    free(buf);
}

void cmd_tee(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: tee <file>\n");
        terminal_writestring("  Reads stdin, writes to both terminal and file.\n");
        terminal_writestring("  Press Ctrl+D on empty line to stop.\n");
        return;
    }
    if (fsbridge_exists(args[1]) && !fsbridge_is_dir(args[1])) {
        terminal_writestring("tee: File exists, overwrite? (y/n) ");
        char c = keyboard_getchar();
        terminal_putchar(c);
        terminal_putchar('\n');
        if (c != 'y' && c != 'Y') return;
        fsbridge_delete(args[1]);
    }
    if (fsbridge_create(args[1]) != 0) {
        terminal_writestring("tee: Failed to create file\n");
        return;
    }
    terminal_writestring("tee: Enter lines (Ctrl+D or .q to stop)\n");
    char line[512];
    uint32_t offset = 0;
    for (;;) {
        terminal_writestring("> ");
        keyboard_readline(line, 512);
        if (line[0] == '\0' || strcmp(line, ".q") == 0) break;
        terminal_writestring(line);
        terminal_putchar('\n');
        fsbridge_write(args[1], line, strlen(line), offset);
        fsbridge_write(args[1], "\n", 1, offset + strlen(line));
        offset += strlen(line) + 1;
    }
}
