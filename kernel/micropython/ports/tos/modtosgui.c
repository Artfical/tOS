/* "tosgui" — a tiny, tkinter-flavored GUI module for the embedded
 * MicroPython REPL. tOS has no pixel graphics (see Paint/Image
 * Viewer's notes elsewhere in the codebase), so "widgets" are just
 * text drawn at a row/column with a color; the script does its own
 * hit-testing against coordinates it already drew at.
 *
 * Registered at runtime the same way and for the same reason as the
 * "tos" module (modtos.c) — see that file's header comment. This one
 * only needs a handful of extra qstrs (the color constant names),
 * also interned at runtime via qstr_from_str().
 *
 * Window ownership model: opening a window hands the *calling task's*
 * own terminal/window-manager context over to that window (see
 * wm_script_open() in wm.c) rather than spawning a dedicated task the
 * way every C-implemented app does — there's no second task to hand
 * draw commands to, so the script has to drive its own window
 * directly. That means terminal output from anything else running in
 * that same task (e.g. print() in an interactive REPL session) also
 * lands in the window's surface while it's open; this module is
 * really meant for script *files* that open a window, run an event
 * loop, and close it again, not for poking at it line-by-line in the
 * REPL. */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include "terminal.h"
#include "wm.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "string.h"

static void *script_win;
static void *script_prev_userdata;

static uint8_t tg_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static mp_obj_t mp_tosgui_open(mp_obj_t title_obj)
{
    if (script_win) return mp_const_false;
    const char *title = mp_obj_str_get_str(title_obj);
    script_win = wm_script_open(title, &script_prev_userdata);
    if (!script_win) return mp_const_false;
    terminal_clear();
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tosgui_open_obj, mp_tosgui_open);

static mp_obj_t mp_tosgui_close(void)
{
    if (script_win) {
        wm_script_close(script_win, script_prev_userdata);
        script_win = 0;
        script_prev_userdata = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_close_obj, mp_tosgui_close);

static mp_obj_t mp_tosgui_clear(void)
{
    terminal_clear();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_clear_obj, mp_tosgui_clear);

/* text(x, y, s, fg=LIGHT_GREY, bg=BLACK) */
static mp_obj_t mp_tosgui_text(size_t n_args, const mp_obj_t *args)
{
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    const char *s = mp_obj_str_get_str(args[2]);
    mp_int_t fg = (n_args > 3) ? mp_obj_get_int(args[3]) : VGA_LIGHT_GREY;
    mp_int_t bg = (n_args > 4) ? mp_obj_get_int(args[4]) : VGA_BLACK;

    terminal_setcolor(tg_color((uint8_t)fg, (uint8_t)bg));
    terminal_setpos((size_t)x, (size_t)y);
    terminal_writestring(s);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_tosgui_text_obj, 3, 5, mp_tosgui_text);

/* button(x, y, s, fg=WHITE, bg=BLUE) — purely visual; the script
 * compares poll_click()'s (x, y) against where it drew this itself. */
static mp_obj_t mp_tosgui_button(size_t n_args, const mp_obj_t *args)
{
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    const char *s = mp_obj_str_get_str(args[2]);
    mp_int_t fg = (n_args > 3) ? mp_obj_get_int(args[3]) : VGA_WHITE;
    mp_int_t bg = (n_args > 4) ? mp_obj_get_int(args[4]) : VGA_BLUE;

    terminal_setcolor(tg_color((uint8_t)fg, (uint8_t)bg));
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar('[');
    terminal_putchar(' ');
    terminal_writestring(s);
    terminal_putchar(' ');
    terminal_putchar(']');
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_tosgui_button_obj, 3, 5, mp_tosgui_button);

/* input(x, y, prompt="") — draws `prompt` at (x,y) and blocks (yielding
 * to the rest of the OS each iteration, like update() does) until the
 * user types a line and presses Enter, echoing characters right after
 * the prompt and supporting backspace. Returns the typed string. Meant
 * for simple "ask a question, then keep drawing" scripts; for anything
 * that needs to keep its own event loop running (clicks, animation)
 * while also taking text input, poll_key() is the lower-level building
 * block this is built on. */
static mp_obj_t mp_tosgui_input(size_t n_args, const mp_obj_t *args)
{
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    const char *prompt = (n_args > 2) ? mp_obj_str_get_str(args[2]) : "";
    int plen = (int)strlen(prompt);

    terminal_setcolor(tg_color(VGA_LIGHT_GREY, VGA_BLACK));
    terminal_setpos((size_t)x, (size_t)y);
    terminal_writestring(prompt);

    static char buf[256];
    int i = 0;

    for (;;) {
        gui_poll();
        if (!wm_current_task_has_focus() || !keyboard_data_available()) {
            task_yield();
            continue;
        }
        char c = keyboard_getchar();
        if (c == '\n') {
            break;
        } else if ((c == '\b' || c == 127) && i > 0) {
            i--;
            terminal_setpos((size_t)(x + plen + i), (size_t)y);
            terminal_putchar(' ');
            terminal_setpos((size_t)(x + plen + i), (size_t)y);
        } else if ((unsigned char)c >= ' ' && i < (int)sizeof(buf) - 1) {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
    buf[i] = 0;
    return mp_obj_new_str(buf, (size_t)i);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_tosgui_input_obj, 2, 3, mp_tosgui_input);

static mp_obj_t mp_tosgui_poll_click(void)
{
    int x, y;
    if (!wm_get_content_click(&x, &y)) return mp_const_none;
    mp_obj_t tup[2] = { mp_obj_new_int(x), mp_obj_new_int(y) };
    return mp_obj_new_tuple(2, tup);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_poll_click_obj, mp_tosgui_poll_click);

static mp_obj_t mp_tosgui_poll_key(void)
{
    if (!wm_current_task_has_focus() || !keyboard_data_available()) return mp_const_none;
    char c = keyboard_getchar();
    return mp_obj_new_int((mp_int_t)(unsigned char)c);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_poll_key_obj, mp_tosgui_poll_key);

static mp_obj_t mp_tosgui_has_focus(void)
{
    return mp_obj_new_bool(wm_current_task_has_focus());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_has_focus_obj, mp_tosgui_has_focus);

static mp_obj_t mp_tosgui_update(void)
{
    gui_poll();
    task_yield();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tosgui_update_obj, mp_tosgui_update);

static void tosgui_store(mp_obj_dict_t *globals, const char *name, mp_obj_t val)
{
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str(name)), val);
}

static void tosgui_store_fun(mp_obj_dict_t *globals, const char *name, const void *fun_obj)
{
    tosgui_store(globals, name, MP_OBJ_FROM_PTR(fun_obj));
}

void tosgui_module_init(void)
{
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("tosgui"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);

    tosgui_store_fun(g, "open", &mp_tosgui_open_obj);
    tosgui_store_fun(g, "close", &mp_tosgui_close_obj);
    tosgui_store_fun(g, "clear", &mp_tosgui_clear_obj);
    tosgui_store_fun(g, "text", &mp_tosgui_text_obj);
    tosgui_store_fun(g, "button", &mp_tosgui_button_obj);
    tosgui_store_fun(g, "poll_click", &mp_tosgui_poll_click_obj);
    tosgui_store_fun(g, "poll_key", &mp_tosgui_poll_key_obj);
    tosgui_store_fun(g, "input", &mp_tosgui_input_obj);
    tosgui_store_fun(g, "has_focus", &mp_tosgui_has_focus_obj);
    tosgui_store_fun(g, "update", &mp_tosgui_update_obj);

    tosgui_store(g, "BLACK", mp_obj_new_int(VGA_BLACK));
    tosgui_store(g, "BLUE", mp_obj_new_int(VGA_BLUE));
    tosgui_store(g, "GREEN", mp_obj_new_int(VGA_GREEN));
    tosgui_store(g, "CYAN", mp_obj_new_int(VGA_CYAN));
    tosgui_store(g, "RED", mp_obj_new_int(VGA_RED));
    tosgui_store(g, "MAGENTA", mp_obj_new_int(VGA_MAGENTA));
    tosgui_store(g, "BROWN", mp_obj_new_int(VGA_BROWN));
    tosgui_store(g, "LIGHT_GREY", mp_obj_new_int(VGA_LIGHT_GREY));
    tosgui_store(g, "DARK_GREY", mp_obj_new_int(VGA_DARK_GREY));
    tosgui_store(g, "LIGHT_BLUE", mp_obj_new_int(VGA_LIGHT_BLUE));
    tosgui_store(g, "LIGHT_GREEN", mp_obj_new_int(VGA_LIGHT_GREEN));
    tosgui_store(g, "LIGHT_CYAN", mp_obj_new_int(VGA_LIGHT_CYAN));
    tosgui_store(g, "LIGHT_RED", mp_obj_new_int(VGA_LIGHT_RED));
    tosgui_store(g, "LIGHT_MAGENTA", mp_obj_new_int(VGA_LIGHT_MAGENTA));
    tosgui_store(g, "YELLOW", mp_obj_new_int(VGA_LIGHT_BROWN));
    tosgui_store(g, "WHITE", mp_obj_new_int(VGA_WHITE));
}
