#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "tarfmt.h"
#include "zipfmt.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

static void list_cb(const char *name, int is_dir, unsigned int size)
{
    terminal_writestring(name);
    if (!is_dir) {
        terminal_writestring("  (");
        print_num(size);
        terminal_writestring(" bytes)");
    }
    terminal_putchar('\n');
}

void cmd_tar(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: tar c <archive.tar> <file...>   (create)\n");
        terminal_writestring("       tar x <archive.tar> [dest_dir]  (extract)\n");
        terminal_writestring("       tar t <archive.tar>             (list)\n");
        return;
    }

    char err[80];
    if (strcmp(args[1], "c") == 0) {
        if (argc < 4) { terminal_writestring("usage: tar c <archive.tar> <file...>\n"); return; }
        if (tar_create(args[2], (const char **)&args[3], argc - 3, err, sizeof(err)) != 0) {
            terminal_writestring("tar: "); terminal_writestring(err); terminal_putchar('\n');
            return;
        }
        terminal_writestring("Created "); terminal_writestring(args[2]); terminal_putchar('\n');
    } else if (strcmp(args[1], "x") == 0) {
        const char *dest = (argc > 3) ? args[3] : ".";
        int n = tar_extract(args[2], dest, err, sizeof(err));
        if (n < 0) {
            terminal_writestring("tar: "); terminal_writestring(err); terminal_putchar('\n');
            return;
        }
        terminal_writestring("Extracted "); print_num((uint32_t)n); terminal_writestring(" entries\n");
    } else if (strcmp(args[1], "t") == 0) {
        if (tar_list(args[2], list_cb, err, sizeof(err)) < 0) {
            terminal_writestring("tar: "); terminal_writestring(err); terminal_putchar('\n');
        }
    } else {
        terminal_writestring("tar: unknown mode '"); terminal_writestring(args[1]); terminal_writestring("' (use c, x, or t)\n");
    }
}

void cmd_zip(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: zip <archive.zip> <file...>\n");
        return;
    }
    char err[80];
    if (zip_create(args[1], (const char **)&args[2], argc - 2, err, sizeof(err)) != 0) {
        terminal_writestring("zip: "); terminal_writestring(err); terminal_putchar('\n');
        return;
    }
    terminal_writestring("Created "); terminal_writestring(args[1]); terminal_putchar('\n');
}

void cmd_unzip(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: unzip <archive.zip> [dest_dir]\n");
        return;
    }
    const char *dest = (argc > 2) ? args[2] : ".";
    char err[80];
    int n = zip_extract(args[1], dest, err, sizeof(err));
    if (n < 0) {
        terminal_writestring("unzip: "); terminal_writestring(err); terminal_putchar('\n');
        return;
    }
    terminal_writestring("Extracted "); print_num((uint32_t)n); terminal_writestring(" entries\n");
}
