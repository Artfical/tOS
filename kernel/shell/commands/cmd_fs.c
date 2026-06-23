#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "memory.h"
#include "vfs.h"
#include "fsbridge.h"

void cmd_pwd(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring(ramfs_getcwd());
    terminal_putchar('\n');
}

void cmd_ls(int argc, char **args)
{
    const char *path = ramfs_getcwd();
    if (argc > 1) path = args[1];

    vfs_entry_t entries[256];
    int count = fsbridge_list(path, entries, 256);
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

void cmd_cd(int argc, char **args)
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

void cmd_mkdir(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: mkdir <dir>\n");
        return;
    }
    if (fsbridge_mkdir(args[1]) != 0) {
        terminal_writestring("mkdir: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

void cmd_rmdir(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: rmdir <dir>\n");
        return;
    }
    if (fsbridge_delete(args[1]) != 0) {
        terminal_writestring("rmdir: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

void cmd_rm(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: rm <file>\n");
        return;
    }
    if (fsbridge_delete(args[1]) != 0) {
        terminal_writestring("rm: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

void cmd_touch(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: touch <file>\n");
        return;
    }
    if (fsbridge_create(args[1]) != 0) {
        terminal_writestring("touch: ");
        terminal_writestring(args[1]);
        terminal_writestring(": Failed\n");
    }
}

void cmd_mv(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: mv <src> <dst>\n");
        return;
    }
    if (fsbridge_rename(args[1], args[2]) != 0)
        terminal_writestring("mv: Failed\n");
}

void cmd_cp(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: cp <src> <dst>\n");
        return;
    }
    if (!fsbridge_exists(args[1]) || fsbridge_is_dir(args[1])) {
        terminal_writestring("cp: Source not found or is a directory\n");
        return;
    }
    if (fsbridge_exists(args[2])) {
        terminal_writestring("cp: Destination exists\n");
        return;
    }
    if (fsbridge_create(args[2]) != 0) {
        terminal_writestring("cp: Failed to create destination\n");
        return;
    }
    uint32_t sz = fsbridge_size(args[1]);
    char *buf = (char *)malloc(sz);
    if (!buf) {
        terminal_writestring("cp: Out of memory\n");
        fsbridge_delete(args[2]);
        return;
    }
    fsbridge_read(args[1], buf, sz, 0);
    fsbridge_write(args[2], buf, sz, 0);
    free(buf);
}

static void find_recursive(const char *path, const char *pattern)
{
    vfs_entry_t entries[256];
    int count = fsbridge_list(path, entries, 256);
    if (count <= 0) return;
    for (int i = 0; i < count; i++) {
        char full[512];
        strcpy(full, path);
        int plen = strlen(full);
        if (plen > 0 && full[plen - 1] != '/') strcat(full, "/");
        strcat(full, entries[i].name);
        if (strstr(entries[i].name, pattern)) {
            terminal_writestring(full);
            terminal_putchar('\n');
        }
        if (entries[i].is_dir) {
            if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0)
                find_recursive(full, pattern);
        }
    }
}

void cmd_find(int argc, char **args)
{
    const char *path = ".";
    const char *pattern = NULL;
    if (argc < 2) {
        terminal_writestring("usage: find [path] -name <pattern>\n");
        return;
    }
    if (argc >= 3 && strcmp(args[1], "-name") == 0) {
        if (argc > 2) pattern = args[2];
    } else if (argc >= 4 && strcmp(args[2], "-name") == 0) {
        path = args[1];
        pattern = args[3];
    } else {
        pattern = args[1];
    }
    if (!pattern) {
        terminal_writestring("usage: find [path] -name <pattern>\n");
        return;
    }
    find_recursive(path, pattern);
}
