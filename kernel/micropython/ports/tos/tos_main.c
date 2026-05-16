#include "terminal.h"
#include "serial.h"

int micropython_init(void) {
    serial_write("micropython: placeholder init\n");
    return 0;
}

int micropython_run_repl(void) {
    serial_write("micropython: placeholder repl\n");
    return 0;
}

int micropython_run_file(const char *path) {
    (void)path;
    serial_write("micropython: placeholder file\n");
    return 0;
}
