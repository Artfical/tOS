#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "string.h"
#include "version.h"
#include "ramfs.h"
#include "commands.h"

#define MAX_ARGS 16
#define MAX_CMD_LEN 512

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
            cmd_help(argc, args);
        } else if (strcmp(c, "echo") == 0) {
            cmd_echo(argc, args);
        } else if (strcmp(c, "clear") == 0) {
            cmd_clear(argc, args);
        } else if (strcmp(c, "pwd") == 0) {
            cmd_pwd(argc, args);
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
        } else if (strcmp(c, "head") == 0) {
            cmd_head(argc, args);
        } else if (strcmp(c, "tail") == 0) {
            cmd_tail(argc, args);
        } else if (strcmp(c, "wc") == 0) {
            cmd_wc(argc, args);
        } else if (strcmp(c, "sort") == 0) {
            cmd_sort(argc, args);
        } else if (strcmp(c, "grep") == 0) {
            cmd_grep(argc, args);
        } else if (strcmp(c, "mv") == 0) {
            cmd_mv(argc, args);
        } else if (strcmp(c, "cp") == 0) {
            cmd_cp(argc, args);
        } else if (strcmp(c, "find") == 0) {
            cmd_find(argc, args);
        } else if (strcmp(c, "edit") == 0) {
            cmd_edit(argc, args);
        } else if (strcmp(c, "exec") == 0) {
            cmd_exec(argc, args);
        } else if (strcmp(c, "tsharp") == 0) {
            cmd_tsharp(argc, args);
        } else if (strcmp(c, "python") == 0) {
            cmd_python(argc, args);
        } else if (strcmp(c, "reboot") == 0) {
            cmd_reboot(argc, args);
        } else if (strcmp(c, "shutdown") == 0) {
            cmd_shutdown(argc, args);
        } else if (strcmp(c, "version") == 0) {
            cmd_version(argc, args);
        } else if (strcmp(c, "about") == 0) {
            cmd_about(argc, args);
        } else if (strcmp(c, "uname") == 0) {
            cmd_uname(argc, args);
        } else if (strcmp(c, "whoami") == 0) {
            cmd_whoami(argc, args);
        } else if (strcmp(c, "hostname") == 0) {
            cmd_hostname(argc, args);
        } else if (strcmp(c, "date") == 0) {
            cmd_date(argc, args);
        } else if (strcmp(c, "cal") == 0) {
            cmd_cal(argc, args);
        } else if (strcmp(c, "df") == 0) {
            cmd_df(argc, args);
        } else if (strcmp(c, "free") == 0) {
            cmd_free(argc, args);
        } else if (strcmp(c, "dmesg") == 0) {
            cmd_dmesg(argc, args);
        } else if (strcmp(c, "yes") == 0) {
            cmd_yes(argc, args);
        } else if (strcmp(c, "seq") == 0) {
            cmd_seq(argc, args);
        } else if (strcmp(c, "sleep") == 0) {
            cmd_sleep(argc, args);
        } else if (strcmp(c, "basename") == 0) {
            cmd_basename(argc, args);
        } else if (strcmp(c, "dirname") == 0) {
            cmd_dirname(argc, args);
        } else if (strcmp(c, "which") == 0) {
            cmd_which(argc, args);
        } else if (strcmp(c, "env") == 0) {
            cmd_env(argc, args);
        } else if (strcmp(c, "ping") == 0) {
            cmd_ping(argc, args);
        } else if (strcmp(c, "wget") == 0) {
            cmd_wget(argc, args);
        } else {
            terminal_writestring("Unknown command: ");
            terminal_writestring(c);
            terminal_putchar('\n');
        }
    }
}
