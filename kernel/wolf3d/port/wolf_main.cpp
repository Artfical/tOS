/* Shell entry point for Wolfenstein 3D -- not part of the vendored
 * Wolf4SDL source (kernel/wolf3d/), same role kernel/doom/port/
 * doom_main.c plays for DOOM.
 *
 * Unlike doomgeneric, Wolf4SDL's main()/DemoLoop() has no per-frame
 * callback to return control through cooperatively -- it's one C++
 * function call that loops forever internally, polling input via
 * SDL_PollEvent() (kernel/wolf3d/port/sdl_shim.cpp). So both a normal
 * in-game Quit and Ctrl+C have to escape back here via wolf_longjmp()
 * (see wolf_jmp.h) instead of doomgeneric_tos_run()'s simple "return
 * from the loop" used for DOOM.
 */
extern "C" {
#include "bochs.h"
#include "vga.h"
#include "keyboard.h"
#include "terminal.h"
#include "vfs.h"
}
#include "wolf_compat.h"

wolf_jmp_buf g_wolf_quit_jmp;
volatile int g_wolf_ctrlc_requested = 0;

extern "C" int main(int argc, char *argv[]);

static void wolf_ctrlc_handler(void)
{
    g_wolf_ctrlc_requested = 1;
}

static char *wolf_argv[] = { (char *)"wolf3d", 0 };

extern "C" void cmd_wolf3d(int argc, char **args)
{
    (void)argc; (void)args;

    terminal_writestring(
        "Starting Wolfenstein 3D (shareware) -- press Ctrl+C to return to the desktop.\n"
    );

    int old_interrupt_char = interrupt_char;
    void (*old_interrupt_callback)(void) = interrupt_callback;
    g_wolf_ctrlc_requested = 0;
    interrupt_char = 3;
    interrupt_callback = wolf_ctrlc_handler;

    /* Wolf4SDL's id_ca.cpp opens all its data files (VSWAP.WL1 etc,
     * see assets/wolf3d/) by their bare filename with no path prefix
     * -- resolved relative to vfs.c's process-wide cwd (see
     * vfs_chdir(), added for this). Restored to "/" below regardless
     * of which way main() exits. */
    vfs_chdir("/assets/wolf3d");

    /* Quit()/SDL_PollEvent() longjmp() back here on both a normal
     * in-game Quit and Ctrl+C -- wolf_setjmp()'s nonzero return path
     * below is that landing spot, not a real second call. */
    if (wolf_setjmp(&g_wolf_quit_jmp) == 0) {
        main(1, wolf_argv);
    }

    vfs_chdir("/");
    interrupt_char = old_interrupt_char;
    interrupt_callback = old_interrupt_callback;

    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
}
