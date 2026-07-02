// Port-specific stubs for MicroPython on tOS
#include "py/gc.h"
#include "py/mpconfig.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/misc.h"
#include "py/stackctrl.h"
#include "py/mpstate.h"
#include "terminal.h"
#include "../../fs/vfs.h"
#include "../../fs/fsbridge.h"

// ---------------------------------------------------------------------------
// GC: scan the C stack from current SP up to the recorded top
// ---------------------------------------------------------------------------
void gc_collect(void) {
    gc_collect_start();
    volatile uintptr_t sp;
    asm volatile("mov %%esp, %0" : "=r"(sp));
    void *stack_top = MP_STATE_THREAD(stack_top);
    gc_collect_root((void **)sp,
        ((uintptr_t)stack_top - (uintptr_t)sp) / sizeof(uintptr_t));
    gc_collect_end();
}

// ---------------------------------------------------------------------------
// File import support via fsbridge
// ---------------------------------------------------------------------------
mp_import_stat_t mp_import_stat(const char *path) {
    if (fsbridge_is_dir(path))  return MP_IMPORT_STAT_DIR;
    if (fsbridge_exists(path))  return MP_IMPORT_STAT_FILE;
    return MP_IMPORT_STAT_NO_EXIST;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    uint32_t sz = fsbridge_size(path);
    if (sz == 0 || sz > 256 * 1024) return NULL;

    char *buf = (char *)m_malloc(sz + 1);
    if (!buf) return NULL;
    int r = fsbridge_read(path, buf, sz, 0);
    if (r < 0) { m_free(buf); return NULL; }
    buf[sz] = '\0';

    mp_lexer_t *lex = mp_lexer_new_from_str_len(filename, buf, sz, 0);
    m_free(buf);
    return lex;
}

// ---------------------------------------------------------------------------
// File object type for Python open()
// ---------------------------------------------------------------------------
typedef struct {
    mp_obj_base_t base;
    int fd;
    int writable;
} tos_file_obj_t;

static mp_obj_t tos_file_read(mp_obj_t self_in, mp_obj_t size_in) {
    tos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) mp_raise_ValueError(MP_ERROR_TEXT("file closed"));
    int sz = (int)mp_obj_get_int(size_in);
    if (sz < 0) sz = 65536;
    char *buf = (char *)m_malloc(sz + 1);
    if (!buf) mp_raise_OSError(12);
    int n = vfs_read(self->fd, buf, (uint32_t)sz);
    if (n < 0) { m_free(buf); mp_raise_OSError(-n); }
    mp_obj_t ret = mp_obj_new_str(buf, (size_t)n);
    m_free(buf);
    return ret;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tos_file_read_obj, tos_file_read);

static mp_obj_t tos_file_readline(mp_obj_t self_in) {
    tos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) mp_raise_ValueError(MP_ERROR_TEXT("file closed"));
    char buf[512];
    int n = 0;
    while (n < (int)(sizeof(buf) - 1)) {
        char c;
        if (vfs_read(self->fd, &c, 1) <= 0) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    return mp_obj_new_str(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(tos_file_readline_obj, tos_file_readline);

static mp_obj_t tos_file_write(mp_obj_t self_in, mp_obj_t data_in) {
    tos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) mp_raise_ValueError(MP_ERROR_TEXT("file closed"));
    if (!self->writable) mp_raise_OSError(9);
    size_t len;
    const char *data = mp_obj_str_get_data(data_in, &len);
    int n = vfs_write(self->fd, data, (uint32_t)len);
    if (n < 0) mp_raise_OSError(-n);
    return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(tos_file_write_obj, tos_file_write);

static mp_obj_t tos_file_close(mp_obj_t self_in) {
    tos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd >= 0) { vfs_close(self->fd); self->fd = -1; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(tos_file_close_obj, tos_file_close);

static mp_obj_t tos_file_enter(mp_obj_t self_in) { return self_in; }
static MP_DEFINE_CONST_FUN_OBJ_1(tos_file_enter_obj, tos_file_enter);

static mp_obj_t tos_file_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return tos_file_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tos_file_exit_obj, 4, 4, tos_file_exit);

static const mp_rom_map_elem_t tos_file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&tos_file_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),  MP_ROM_PTR(&tos_file_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),     MP_ROM_PTR(&tos_file_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&tos_file_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&tos_file_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&tos_file_exit_obj) },
};
static MP_DEFINE_CONST_DICT(tos_file_locals_dict, tos_file_locals_table);

// Use the new slot-based type API (MicroPython 1.20+).
// Name reuses MP_QSTR_object — only affects repr, not behaviour.
MP_DEFINE_CONST_OBJ_TYPE(
    tos_file_type,
    MP_QSTR_object,
    MP_TYPE_FLAG_NONE,
    locals_dict, &tos_file_locals_dict
);

// ---------------------------------------------------------------------------
// builtin open()
// ---------------------------------------------------------------------------
static mp_obj_t mp_builtin_open_impl(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)kwargs;
    if (n_args < 1) mp_raise_TypeError(MP_ERROR_TEXT("open requires filename"));

    const char *path = mp_obj_str_get_str(args[0]);
    const char *mode = "r";
    if (n_args >= 2) mode = mp_obj_str_get_str(args[1]);

    int flags = VFS_RDONLY;
    int writable = 0;
    for (const char *m = mode; *m; m++) {
        switch (*m) {
            case 'r': flags = VFS_RDONLY; writable = 0; break;
            case 'w': flags = VFS_WRONLY | VFS_CREAT | VFS_TRUNC; writable = 1; break;
            case 'a': flags = VFS_WRONLY | VFS_CREAT | VFS_APPEND; writable = 1; break;
            case '+': flags |= VFS_RDWR; writable = 1; break;
            default: break;
        }
    }

    int fd = vfs_open(path, flags);
    if (fd < 0) mp_raise_OSError(-fd);

    tos_file_obj_t *f = m_new_obj(tos_file_obj_t);
    f->base.type = &tos_file_type;
    f->fd = fd;
    f->writable = writable;
    return MP_OBJ_FROM_PTR(f);
}

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    return mp_builtin_open_impl(n_args, args, kwargs);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 0, mp_builtin_open_impl);

// ---------------------------------------------------------------------------
// sys.stdin / stdout / stderr — minimal placeholder objects
// ---------------------------------------------------------------------------
typedef struct { mp_obj_base_t base; } mp_dummy_t;
mp_dummy_t mp_sys_stdin_obj  = { { &mp_type_object } };
mp_dummy_t mp_sys_stdout_obj = { { &mp_type_object } };
mp_dummy_t mp_sys_stderr_obj = { { &mp_type_object } };

// ---------------------------------------------------------------------------
// nlr_jump_fail — print and halt rather than spinning silently forever
// ---------------------------------------------------------------------------
void nlr_jump_fail(void *val) {
    (void)val;
    terminal_writestring("\r\n[MicroPython] fatal: unhandled exception\r\n");
    for (;;) asm volatile("hlt");
}
