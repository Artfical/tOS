#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "memory.h"
#include "stdlib.h"
#include "keyboard.h"
#include "shell.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

void cmd_edit(int argc, char **args)
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

    char line[512];
    uint32_t offset = 0;
    if (exists) offset = ramfs_size(filename);

    for (;;) {
        terminal_writestring("> ");
        keyboard_readline(line, 512);

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

void cmd_yes(int argc, char **args)
{
    const char *s = "y";
    if (argc > 1) s = args[1];
    for (int i = 0; i < 100; i++) {
        terminal_writestring(s);
        terminal_putchar('\n');
    }
}

void cmd_seq(int argc, char **args)
{
    int start = 1, end = 1, step = 1;
    if (argc == 2) {
        end = atoi(args[1]);
    } else if (argc == 3) {
        start = atoi(args[1]);
        end = atoi(args[2]);
    } else if (argc >= 4) {
        start = atoi(args[1]);
        step = atoi(args[2]);
        end = atoi(args[3]);
    } else {
        terminal_writestring("usage: seq [start] [step] <end>\n");
        return;
    }
    for (int i = start; (step > 0 ? i <= end : i >= end); i += step) {
        print_num(i);
        terminal_putchar('\n');
    }
}

void cmd_basename(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: basename <path>\n");
        return;
    }
    const char *p = args[1];
    const char *last = p;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    terminal_writestring(last);
    terminal_putchar('\n');
}

void cmd_dirname(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: dirname <path>\n");
        return;
    }
    const char *p = args[1];
    const char *last_slash = NULL;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }
    if (!last_slash) {
        terminal_writestring(".\n");
    } else if (last_slash == args[1]) {
        terminal_writestring("/\n");
    } else {
        char buf[256];
        int len = last_slash - args[1];
        for (int i = 0; i < len && i < 255; i++) buf[i] = args[1][i];
        buf[len] = '\0';
        terminal_writestring(buf);
        terminal_putchar('\n');
    }
}

void cmd_which(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: which <command>\n");
        return;
    }
    static const char *builtins[] = {
        "help", "echo", "clear", "pwd", "ls", "cd", "mkdir", "rmdir", "rm",
        "touch", "cat", "mv", "cp", "edit", "exec", "reboot", "shutdown",
        "version", "beep", "soundinfo", "crash", "about", "uname", "ping", "wget", "python", "tsharp",
        "head", "tail", "wc", "sort", "grep", "find", "date", "whoami",
        "hostname", "cal", "yes", "seq", "sleep", "df", "free", "dmesg",
        "basename", "dirname", "which", "env", "uptime", "ps", "kill",
        "chmod", "hexdump", "tee", "alias", "unalias", "history",
        "disk", "rev", "uniq",
        NULL
    };
    for (int i = 0; i < argc; i++) {
        const char *cmd = args[i];
        int found = 0;
        for (int j = 0; builtins[j]; j++) {
            if (strcmp(cmd, builtins[j]) == 0) {
                terminal_writestring(cmd);
                terminal_writestring(" (built-in)\n");
                found = 1;
                break;
            }
        }
        if (!found && ramfs_exists(cmd) && !ramfs_is_dir(cmd)) {
            terminal_writestring(cmd);
            terminal_writestring(" (file)\n");
            found = 1;
        }
        if (!found) {
            terminal_writestring("which: ");
            terminal_writestring(cmd);
            terminal_writestring(": not found\n");
        }
    }
}

void cmd_env(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("TERM=vt100\n");
    terminal_writestring("SHELL=/bin/sh\n");
    terminal_writestring("USER=root\n");
    terminal_writestring("HOME=/\n");
    terminal_writestring("PATH=/:/bin:/usr/bin\n");
}

void cmd_history(int argc, char **args)
{
    (void)argc; (void)args;
    shell_history_show();
}

void cmd_alias(int argc, char **args)
{
    if (argc == 1) {
        shell_alias_list();
        return;
    }
    const char *eq = args[1];
    const char *equals = NULL;
    for (const char *p = eq; *p; p++) {
        if (*p == '=') { equals = p; break; }
    }
    if (!equals) {
        terminal_writestring("usage: alias <name>=<value>\n");
        return;
    }
    char name[32];
    int nlen = equals - eq;
    for (int i = 0; i < nlen && i < 31; i++) name[i] = eq[i];
    name[nlen] = 0;

    const char *val = equals + 1;
    if (shell_alias_set(name, val) == 0) {
        terminal_writestring("Alias set\n");
    } else {
        terminal_writestring("alias: Failed (max ");
        print_num(16);
        terminal_writestring(" aliases)\n");
    }
}

void cmd_unalias(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: unalias <name>\n");
        return;
    }
    if (shell_alias_unset(args[1]) == 0) {
        terminal_writestring("Unaliased\n");
    } else {
        terminal_writestring("unalias: ");
        terminal_writestring(args[1]);
        terminal_writestring(": not found\n");
    }
}
