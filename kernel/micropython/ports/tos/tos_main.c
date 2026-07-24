#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/nlr.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/builtin.h"
#include "py/repl.h"
#include "py/stackctrl.h"
#include "py/gc.h"
#include "lib/utils/pyexec.h"
#include "terminal.h"
#include "serial.h"
#include "keyboard.h"

static char heap[128 * 1024];

void tos_module_init(void);
void tosgui_module_init(void);
void os_module_init(void);
void time_module_init(void);
void json_module_init(void);
void socket_module_init(void);
void gitcrypto_module_init(void);

int micropython_init(void) {
    serial_write("micropython: init\n");
    mp_stack_ctrl_init();
    mp_stack_set_limit(32 * 1024); // 32 KB stack limit
    gc_init(heap, heap + sizeof(heap));
    mp_init();
    tos_module_init();
    tosgui_module_init();
    os_module_init();
    time_module_init();
    json_module_init();
    socket_module_init();
    gitcrypto_module_init();
    interrupt_callback = mp_sched_keyboard_interrupt;
    serial_write("micropython: ready\n");
    return 0;
}

int micropython_run_repl(void) {
    terminal_writestring("MicroPython REPL on tOS\r\n");
    for (;;) {
        if (pyexec_friendly_repl() == 0) break;
    }
    return 0;
}

int tos_micropython_exec_file(const char *path);

int micropython_run_file(const char *path) {
    return tos_micropython_exec_file(path);
}
