// Port-specific stubs for MicroPython
#include "py/gc.h"
#include "py/mpconfig.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/misc.h"

// GC collect - stub that scans nothing
void gc_collect(void) {
    gc_collect_start();
    gc_collect_end();
}

// File import - not supported
mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

// File lexer - not supported
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    return NULL;
}

// Builtin open - always returns None
static mp_obj_t mp_builtin_open_wrapper(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)n_args;
    (void)args;
    (void)kwargs;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 0, mp_builtin_open_wrapper);

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    return mp_builtin_open_wrapper(n_args, args, kwargs);
}

// sys.stdin/stdout/stderr - dummy textio objects
// These must be actual objects because they're referenced
// as extern structs in modsys.c and their address is taken.
typedef struct _mp_dummy_t {
    mp_obj_base_t base;
} mp_dummy_t;

mp_dummy_t mp_sys_stdin_obj = { { &mp_type_object } };
mp_dummy_t mp_sys_stdout_obj = { { &mp_type_object } };
mp_dummy_t mp_sys_stderr_obj = { { &mp_type_object } };

// NLR jump failure handler
void nlr_jump_fail(void *val) {
    (void)val;
    for (;;);
}
