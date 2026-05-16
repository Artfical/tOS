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

static char heap[16 * 1024];

int micropython_init(void) {
    serial_write("micropython: init\n");
    gc_init(heap, heap + sizeof(heap));
    mp_init();
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

int micropython_run_file(const char *path) {
    (void)path;
    terminal_writestring("micropython: file exec not yet\r\n");
    return -1;
}
