#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "string.h"
#include "memory.h"
#include "fs.h"
#include "elf.h"
#include "io.h"
#include "version.h"

#define MAX_ARGS 16
#define MAX_CMD_LEN 256

static void cmd_help(void)
{
    terminal_writestring("tOS Shell Commands:\n");
    terminal_writestring("  help     - Show this help\n");
    terminal_writestring("  echo     - Echo text\n");
    terminal_writestring("  clear    - Clear screen\n");
    terminal_writestring("  ls       - List files in initrd\n");
    terminal_writestring("  cat      - Display file contents\n");
    terminal_writestring("  exec     - Execute an ELF program\n");
    terminal_writestring("  reboot   - Reboot system\n");
    terminal_writestring("  shutdown - Halt system\n");
    terminal_writestring("  version  - Show OS version\n");
    terminal_writestring("  about    - About tOS\n");
    terminal_writestring("  uname    - Print system info\n");
}

static void cmd_echo(char *args)
{
    if (args) terminal_writestring(args);
    terminal_putchar('\n');
}

static void cmd_clear(void)
{
    terminal_clear();
}

static void cmd_ls(void)
{
    fs_file_t files[FS_MAX_FILES];
    int count = fs_list(files, FS_MAX_FILES);
    if (count == 0) {
        terminal_writestring("No files found.\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        terminal_writestring(files[i].name);
        terminal_writestring("  (");
        char buf[16];
        int di = 0;
        uint32_t sz = files[i].size;
        if (sz >= 1000) { buf[di++] = '0' + sz / 1000; sz %= 1000; }
        if (di > 0 || sz >= 100) { buf[di++] = '0' + sz / 100; sz %= 100; }
        if (di > 0 || sz >= 10) { buf[di++] = '0' + sz / 10; sz %= 10; }
        buf[di++] = '0' + sz;
        buf[di] = '\0';
        terminal_writestring(buf);
        terminal_writestring(" bytes)\n");
    }
}

static void cmd_cat(char *args)
{
    if (!args || !*args) {
        terminal_writestring("Usage: cat <filename>\n");
        return;
    }
    char *filename = args;
    while (*filename == ' ') filename++;
    char *end = filename;
    while (*end && *end != ' ') end++;
    char saved = *end;
    *end = '\0';

    fs_file_t file;
    if (fs_open(filename, &file) != 0) {
        terminal_writestring("File not found: ");
        terminal_writestring(filename);
        terminal_putchar('\n');
        *end = saved;
        return;
    }
    *end = saved;

    char *buf = (char *)malloc(file.size + 1);
    if (!buf) {
        terminal_writestring("Out of memory\n");
        return;
    }
    fs_read(&file, buf, file.size, 0);
    buf[file.size] = '\0';
    terminal_writestring(buf);
    if (file.size > 0 && buf[file.size - 1] != '\n')
        terminal_putchar('\n');
    free(buf);
}

static void cmd_exec(char *args)
{
    if (!args || !*args) {
        terminal_writestring("Usage: exec <program>\n");
        return;
    }
    char *filename = args;
    while (*filename == ' ') filename++;
    char *end = filename;
    while (*end && *end != ' ') end++;
    char saved = *end;
    *end = '\0';

    fs_file_t file;
    if (fs_open(filename, &file) != 0) {
        terminal_writestring("Program not found: ");
        terminal_writestring(filename);
        terminal_putchar('\n');
        *end = saved;
        return;
    }
    *end = saved;

    void *prog_data = (void *)file.offset;
    if ((uint32_t)prog_data < 0x100000) {
        prog_data = malloc(file.size);
        if (!prog_data) {
            terminal_writestring("Out of memory\n");
            return;
        }
        fs_read(&file, prog_data, file.size, 0);
    }

    terminal_writestring("Loading: ");
    terminal_writestring(filename);
    terminal_putchar('\n');

    uint32_t entry = 0;
    if (elf_load(prog_data, &entry) != 0) {
        terminal_writestring("Failed to load ELF\n");
        if ((uint32_t)prog_data >= 0x100000) free(prog_data);
        return;
    }

    terminal_writestring("Entry point: ");
    char buf[16];
    for (int i = 0; i < 8; i++) {
        buf[7-i] = "0123456789ABCDEF"[entry & 0xF];
        entry >>= 4;
    }
    buf[8] = '\0';
    terminal_writestring(buf);
    terminal_putchar('\n');
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
    terminal_writestring("A from-scratch x86 kernel\n");
    terminal_writestring("License: GNU AGPL v3\n");
    terminal_writestring("Copyright (c) 2026 Artfical\n");
}

static void cmd_uname(void)
{
    terminal_writestring("tOS\n");
}

void shell_init(void)
{
    terminal_writestring(TOS_WELCOME_STRING);
    terminal_writestring("Type 'help' for commands\n\n");
}

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

void shell_run(void)
{
    char cmd_line[MAX_CMD_LEN];
    char *args[MAX_ARGS];

    for (;;) {
        terminal_writestring("tOS$ ");

        keyboard_readline(cmd_line, MAX_CMD_LEN);

        int argc = parse_args(cmd_line, args);

        if (argc == 0) continue;

        if (strcmp(args[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(args[0], "echo") == 0) {
            char *text = NULL;
            if (argc > 1) {
                text = args[1];
                for (int i = 2; i < argc; i++) {
                    char *p = text + strlen(text);
                    *p = ' ';
                    text = args[i];
                }
            }
            cmd_echo(text);
        } else if (strcmp(args[0], "clear") == 0) {
            cmd_clear();
        } else if (strcmp(args[0], "ls") == 0) {
            cmd_ls();
        } else if (strcmp(args[0], "cat") == 0) {
            if (argc > 1) cmd_cat(args[1]);
            else cmd_cat(NULL);
        } else if (strcmp(args[0], "exec") == 0) {
            if (argc > 1) cmd_exec(args[1]);
            else cmd_exec(NULL);
        } else if (strcmp(args[0], "reboot") == 0) {
            cmd_reboot();
        } else if (strcmp(args[0], "shutdown") == 0) {
            cmd_shutdown();
        } else if (strcmp(args[0], "version") == 0) {
            cmd_version();
        } else if (strcmp(args[0], "about") == 0) {
            cmd_about();
        } else if (strcmp(args[0], "uname") == 0) {
            cmd_uname();
        } else {
            terminal_writestring("Unknown command: ");
            terminal_writestring(args[0]);
            terminal_putchar('\n');
        }
    }
}
