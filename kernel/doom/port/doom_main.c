/* Shell entry point for DOOM -- not part of id Software/doomgeneric's
 * source, unlike everything else under kernel/doom/. */
#include "terminal.h"

void doomgeneric_tos_run(int argc, char **argv);

static char *doom_argv[] = { "doom", "-iwad", "/assets/doom1.wad", 0 };

void cmd_doom(int argc, char **args)
{
    (void)argc; (void)args;

    terminal_writestring(
        "Starting DOOM (shareware) -- graphics mode has no reliable way\n"
        "back to the desktop yet, see README's \"Linear framebuffer\n"
        "graphics\" section. You will need to `reboot` afterward.\n"
    );

    doomgeneric_tos_run(3, doom_argv);
}

/* Launched as a normal desktop window (see kernel/display/wm.c's
 * wm_open_doom(), Special-menu "DOOM" entry, and dock icon) instead of
 * the CLI `doom` command's fullscreen takeover. Renders in the same
 * real VBE/Bochs pixel graphics as cmd_doom -- the difference is
 * entirely on the way out: closing the window calls
 * wm_close_window()'s WIN_KIND_DOOM cleanup (bochs_disable() +
 * vga_set_mode(VGA_MODE_TEXT)) before killing this task, so the
 * desktop's text mode comes back without needing a `reboot`. */
void doom_window_run(void)
{
    doomgeneric_tos_run(3, doom_argv);
}
