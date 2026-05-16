#ifndef MICROPY_CONFIG_PORT_H
#define MICROPY_CONFIG_PORT_H

#define MICROPY_ALLOC_PATH_MAX      PATH_MAX
#define MICROPY_ENABLE_GC           1
#define MICROPY_ENABLE_FINALISER    0
#define MICROPY_HELPER_REPL         1
#define MICROPY_REPL_EMACS_KEYS     1
#define MICROPY_REPL_AUTO_INDENT    0
#define MICROPY_KBD_EXCEPTION       1
#define MICROPY_USE_INTERNAL_PRINTF 0
#define MICROPY_USE_INTERNAL_ERRNO  0
#define MICROPY_ERROR_REPORTING     MICROPY_ERROR_REPORTING_DETAILED
#define MICROPY_FLOAT_IMPL          MICROPY_FLOAT_IMPL_FLOAT
#define MICROPY_LONGINT_IMPL        MICROPY_LONGINT_IMPL_NONE
#define MICROPY_STREAMS_NON_BLOCK   0
#define MICROPY_MODULE_ATTR         MICROPY_MODULE_ATTR_STR
#define MICROPY_CPYTHON_COMPAT      1
#define MICROPY_PY_BUILTINS_COMPILE 1
#define MICROPY_PY_BUILTINS_FROZEN  0
#define MICROPY_PY_BUILTINS_FLOAT   1
#define MICROPY_PY___FILE__         0
#define MICROPY_PY_GC               1
#define MICROPY_PY_IO               1
#define MICROPY_PY_IO_FILEIO        0
#define MICROPY_PY_OS               0
#define MICROPY_PY_SYS              1
#define MICROPY_PY_SYS_EXIT         1
#define MICROPY_PY_SYS_STDFILES     0
#define MICROPY_PY_SYS_PLATFORM     "tOS"
#define MICROPY_PY_MATH             1
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS 0
#define MICROPY_PY_RANDOM           1
#define MICROPY_PY_STRUCT           1
#define MICROPY_PY_JSON             0
#define MICROPY_PY_UERRNO           0
#define MICROPY_PY_UBINASCII        1
#define MICROPY_PY_THREAD           0
#define MICROPY_PY_CMATH            0
#define MICROPY_PY_UCTYPE           0
#define MICROPY_PY_UZLIB            0
#define MICROPY_PY_UJSON            0
#define MICROPY_PY_URE              1
#define MICROPY_PY_UHEAPQ           0
#define MICROPY_PY_ARRAY            1
#define MICROPY_PY_COLLECTIONS      1
#define MICROPY_PY_MICROPYTHON_MEM_INFO 1

#define MICROPY_PY_FFI              0
#define MICROPY_PY_SOCKET           0
#define MICROPY_PY_NETWORK          0
#define MICROPY_PY_USSL             0
#define MICROPY_PY_WEBSOCKET        0
#define MICROPY_PY_BTREE            0

typedef int mp_int_t;
typedef unsigned int mp_uint_t;
typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_PORT_BUILTIN_MODULES \
    MICROPY_REGISTER_MODULE("math", mod_math) \
    MICROPY_REGISTER_MODULE("random", urandom_mod_info) \
    MICROPY_REGISTER_MODULE("struct", mod_struct_info) \
    MICROPY_REGISTER_MODULE("ubinascii", mod_ubinascii_info) \
    MICROPY_REGISTER_MODULE("ure", mod_ure_info) \
    MICROPY_REGISTER_MODULE("array", mod_array_info) \
    MICROPY_REGISTER_MODULE("collections", mod_collections_info)

void mp_hal_set_interrupt_char(int c);

#endif
