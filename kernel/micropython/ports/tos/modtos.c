/* "tos" module for the embedded MicroPython REPL — same scripting API
 * surface T# gets (see kernel/shell/tos_api.h), so either language can
 * run shell commands, touch the filesystem, open GUI apps, and inspect/
 * kill tasks.
 *
 * This is registered entirely at runtime (tos_module_init(), called once
 * from micropython_init()) instead of through MicroPython's usual
 * compile-time MP_REGISTER_MODULE()/qstr-table mechanism: this port's
 * genhdr/qstr tables are a frozen, pre-generated snapshot with no
 * Makefile rule that regenerates them, so adding compile-time qstrs here
 * would require manually re-running MicroPython's host-side qstr
 * extraction scripts across the whole tree — high risk of subtly
 * corrupting the global qstr table for a freestanding kernel build with
 * no easy way to verify it afterwards. qstr_from_str() (interning a
 * qstr at runtime, explicitly documented for this purpose in py/qstr.h)
 * combined with mp_obj_new_module() (which inserts directly into
 * sys.modules, so `import tos` finds it with no source file at all)
 * sidesteps that entirely — nothing here touches the static tables. */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include "string.h"
#include "vfs.h"
#include "fsbridge.h"
#include "tos_api.h"

static mp_obj_t mp_tos_exec(mp_obj_t cmd_obj)
{
    const char *cmd = mp_obj_str_get_str(cmd_obj);
    char buf[512];
    tos_exec(cmd, buf, sizeof(buf));
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_exec_obj, mp_tos_exec);

static mp_obj_t mp_tos_read(mp_obj_t path_obj)
{
    const char *path = mp_obj_str_get_str(path_obj);
    char buf[2048];
    int n = tos_read(path, buf, sizeof(buf));
    if (n < 0) return mp_obj_new_str("", 0);
    return mp_obj_new_str(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_read_obj, mp_tos_read);

static mp_obj_t mp_tos_write(mp_obj_t path_obj, mp_obj_t data_obj)
{
    const char *path = mp_obj_str_get_str(path_obj);
    const char *data = mp_obj_str_get_str(data_obj);
    return mp_obj_new_bool(tos_write(path, data, (int)strlen(data)) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_tos_write_obj, mp_tos_write);

static mp_obj_t mp_tos_mkdir(mp_obj_t path_obj)
{
    return mp_obj_new_bool(tos_mkdir(mp_obj_str_get_str(path_obj)) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_mkdir_obj, mp_tos_mkdir);

static mp_obj_t mp_tos_delete(mp_obj_t path_obj)
{
    return mp_obj_new_bool(tos_delete(mp_obj_str_get_str(path_obj)) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_delete_obj, mp_tos_delete);

static mp_obj_t mp_tos_exists(mp_obj_t path_obj)
{
    return mp_obj_new_bool(tos_exists(mp_obj_str_get_str(path_obj)) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_exists_obj, mp_tos_exists);

static mp_obj_t mp_tos_list(mp_obj_t path_obj)
{
    const char *path = mp_obj_str_get_str(path_obj);
    vfs_entry_t entries[64];
    int n = fsbridge_list(path, entries, 64);
    mp_obj_t lst = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        mp_obj_list_append(lst, mp_obj_new_str(entries[i].name, strlen(entries[i].name)));
    }
    return lst;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_list_obj, mp_tos_list);

static mp_obj_t mp_tos_open_app(mp_obj_t name_obj)
{
    return mp_obj_new_bool(tos_open_app(mp_obj_str_get_str(name_obj)) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_open_app_obj, mp_tos_open_app);

static mp_obj_t mp_tos_ps(void)
{
    char buf[1024];
    tos_ps(buf, sizeof(buf));
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tos_ps_obj, mp_tos_ps);

static mp_obj_t mp_tos_kill(mp_obj_t pid_obj)
{
    return mp_obj_new_bool(tos_kill((uint32_t)mp_obj_get_int(pid_obj)) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_kill_obj, mp_tos_kill);

static mp_obj_t mp_tos_uptime(void)
{
    return mp_obj_new_int((mp_int_t)tos_uptime());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_tos_uptime_obj, mp_tos_uptime);

static mp_obj_t mp_tos_http_get(mp_obj_t url_obj)
{
    const char *url = mp_obj_str_get_str(url_obj);
    static char buf[8192];
    int n = tos_http_get(url, buf, sizeof(buf));
    if (n < 0) return mp_obj_new_str("", 0);
    return mp_obj_new_str(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_tos_http_get_obj, mp_tos_http_get);

static void tos_module_store(mp_obj_dict_t *globals, const char *name, const void *fun_obj)
{
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str(name)), MP_OBJ_FROM_PTR(fun_obj));
}

void tos_module_init(void)
{
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("tos"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);

    tos_module_store(g, "exec", &mp_tos_exec_obj);
    tos_module_store(g, "read", &mp_tos_read_obj);
    tos_module_store(g, "write", &mp_tos_write_obj);
    tos_module_store(g, "mkdir", &mp_tos_mkdir_obj);
    tos_module_store(g, "delete", &mp_tos_delete_obj);
    tos_module_store(g, "exists", &mp_tos_exists_obj);
    tos_module_store(g, "list", &mp_tos_list_obj);
    tos_module_store(g, "open_app", &mp_tos_open_app_obj);
    tos_module_store(g, "ps", &mp_tos_ps_obj);
    tos_module_store(g, "kill", &mp_tos_kill_obj);
    tos_module_store(g, "uptime", &mp_tos_uptime_obj);
    tos_module_store(g, "http_get", &mp_tos_http_get_obj);
}
