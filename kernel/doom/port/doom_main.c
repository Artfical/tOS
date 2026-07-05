/* Shell entry point for DOOM -- not part of id Software/doomgeneric's
 * source, unlike everything else under kernel/doom/. */
#include "terminal.h"

void doomgeneric_tos_run(int argc, char **argv);

void cmd_doom(int argc, char **args)
{
    (void)argc; (void)args;

    terminal_writestring(
        "Starting DOOM (shareware) -- graphics mode has no reliable way\n"
        "back to the desktop yet, see README's \"Linear framebuffer\n"
        "graphics\" section. You will need to `reboot` afterward.\n"
    );

    static char *argv[] = { "doom", "-iwad", "/assets/doom1.wad", 0 };
    doomgeneric_tos_run(3, argv);
}
