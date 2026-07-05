/* Shell entry point for DOOM -- not part of id Software/doomgeneric's
 * source, unlike everything else under kernel/doom/. */
#include "terminal.h"

void doomgeneric_tos_run(int argc, char **argv);
void doomgeneric_tos_set_windowed(int windowed);

static char *doom_argv[] = { "doom", "-iwad", "/assets/doom1.wad", 0 };

void cmd_doom(int argc, char **args)
{
    (void)argc; (void)args;

    terminal_writestring(
        "Starting DOOM (shareware) -- graphics mode has no reliable way\n"
        "back to the desktop yet, see README's \"Linear framebuffer\n"
        "graphics\" section. You will need to `reboot` afterward.\n"
    );

    doomgeneric_tos_set_windowed(0);
    doomgeneric_tos_run(3, doom_argv);
}

/* Launched as a normal desktop window (see kernel/display/wm.c's
 * wm_open_doom(), Special-menu "DOOM" entry, and dock icon) instead of
 * the CLI `doom` command's fullscreen takeover -- renders into the
 * window's own text-cell surface (see doomgeneric_tos.c's
 * draw_frame_windowed()) rather than switching the real hardware
 * video mode, so it behaves like any other app: no one-way trip, no
 * `reboot` needed to get the desktop back afterward. */
void doom_window_run(void)
{
    doomgeneric_tos_set_windowed(1);
    doomgeneric_tos_run(3, doom_argv);
}
