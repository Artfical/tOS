/* os module for tOS MicroPython — runtime-registered, no frozen qstrs */
#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include <string.h>
#include <strings.h>
#include "../../fs/vfs.h"
#include "../../fs/fsbridge.h"

/* --- minimal string helpers that may not be in the freestanding libc --- */
static char *mp_strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (char *)last;
}
static char *mp_strncat(char *dst, const char *src, size_t n) {
    size_t dlen = strlen(dst);
    size_t i = 0;
    while (i < n && src[i]) { dst[dlen + i] = src[i]; i++; }
    dst[dlen + i] = '\0';
    return dst;
}

static char cwd[256] = "/";

/* os.getcwd() */
static mp_obj_t mp_os_getcwd(void) {
    return mp_obj_new_str(cwd, strlen(cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_os_getcwd_obj, mp_os_getcwd);

/* os.chdir(path) */
static mp_obj_t mp_os_chdir(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    if (!fsbridge_is_dir(path))
        mp_raise_OSError(20); /* ENOTDIR */
    if (path[0] == '/') {
        strncpy(cwd, path, sizeof(cwd) - 1);
    } else {
        /* relative: append to cwd */
        size_t n = strlen(cwd);
        if (cwd[n - 1] != '/') { cwd[n++] = '/'; cwd[n] = '\0'; }
        mp_strncat(cwd, path, sizeof(cwd) - n - 1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_chdir_obj, mp_os_chdir);

/* os.listdir(path='') */
static mp_obj_t mp_os_listdir(size_t n_args, const mp_obj_t *args) {
    const char *path = (n_args > 0) ? mp_obj_str_get_str(args[0]) : cwd;
    vfs_entry_t entries[128];
    int n = vfs_readdir(path, entries, 128);
    if (n < 0) mp_raise_OSError(-n);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        mp_obj_list_append(list, mp_obj_new_str(entries[i].name, strlen(entries[i].name)));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_listdir_obj, 0, 1, mp_os_listdir);

/* os.mkdir(path) */
static mp_obj_t mp_os_mkdir(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    int r = fsbridge_mkdir(path);
    if (r < 0) mp_raise_OSError(-r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_mkdir_obj, mp_os_mkdir);

/* os.remove(path) */
static mp_obj_t mp_os_remove(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    int r = fsbridge_delete(path);
    if (r < 0) mp_raise_OSError(-r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_remove_obj, mp_os_remove);

/* os.rename(src, dst) */
static mp_obj_t mp_os_rename(mp_obj_t src_in, mp_obj_t dst_in) {
    const char *src = mp_obj_str_get_str(src_in);
    const char *dst = mp_obj_str_get_str(dst_in);
    int r = fsbridge_rename(src, dst);
    if (r < 0) mp_raise_OSError(-r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_rename_obj, mp_os_rename);

/* os.stat(path) -> (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime) */
static mp_obj_t mp_os_stat(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    if (!fsbridge_exists(path) && !fsbridge_is_dir(path))
        mp_raise_OSError(2); /* ENOENT */
    uint32_t sz  = fsbridge_size(path);
    int is_dir   = fsbridge_is_dir(path);
    mp_obj_t items[10];
    items[0] = mp_obj_new_int(is_dir ? 0x4000 : 0x8000); /* mode */
    items[1] = mp_obj_new_int(0);  /* ino */
    items[2] = mp_obj_new_int(0);  /* dev */
    items[3] = mp_obj_new_int(1);  /* nlink */
    items[4] = mp_obj_new_int(0);  /* uid */
    items[5] = mp_obj_new_int(0);  /* gid */
    items[6] = mp_obj_new_int((mp_int_t)sz);
    items[7] = mp_obj_new_int(0);  /* atime */
    items[8] = mp_obj_new_int(0);  /* mtime */
    items[9] = mp_obj_new_int(0);  /* ctime */
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_stat_obj, mp_os_stat);

/* os.path.exists / os.path.isdir / os.path.isfile — stored as plain functions */
static mp_obj_t mp_os_path_exists(mp_obj_t p) {
    const char *path = mp_obj_str_get_str(p);
    return mp_obj_new_bool(fsbridge_exists(path) || fsbridge_is_dir(path));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_path_exists_obj, mp_os_path_exists);

static mp_obj_t mp_os_path_isdir(mp_obj_t p) {
    return mp_obj_new_bool(fsbridge_is_dir(mp_obj_str_get_str(p)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_path_isdir_obj, mp_os_path_isdir);

static mp_obj_t mp_os_path_isfile(mp_obj_t p) {
    const char *path = mp_obj_str_get_str(p);
    return mp_obj_new_bool(fsbridge_exists(path) && !fsbridge_is_dir(path));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_path_isfile_obj, mp_os_path_isfile);

/* os.path.join(a, b) */
static mp_obj_t mp_os_path_join(mp_obj_t a_in, mp_obj_t b_in) {
    const char *a = mp_obj_str_get_str(a_in);
    const char *b = mp_obj_str_get_str(b_in);
    if (b[0] == '/') return mp_obj_new_str(b, strlen(b));
    static char buf[512];
    size_t n = strlen(a);
    strncpy(buf, a, sizeof(buf) - 1);
    if (n > 0 && buf[n - 1] != '/') { buf[n++] = '/'; buf[n] = '\0'; }
    mp_strncat(buf, b, sizeof(buf) - n - 1);
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_path_join_obj, mp_os_path_join);

/* os.path.basename(path) */
static mp_obj_t mp_os_path_basename(mp_obj_t p) {
    const char *path = mp_obj_str_get_str(p);
    const char *s = mp_strrchr(path, '/');
    return mp_obj_new_str(s ? s + 1 : path, strlen(s ? s + 1 : path));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_path_basename_obj, mp_os_path_basename);

/* os.path.dirname(path) */
static mp_obj_t mp_os_path_dirname(mp_obj_t p) {
    const char *path = mp_obj_str_get_str(p);
    const char *s = mp_strrchr(path, '/');
    if (!s) return mp_obj_new_str("", 0);
    if (s == path) return mp_obj_new_str("/", 1);
    return mp_obj_new_str(path, (size_t)(s - path));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_path_dirname_obj, mp_os_path_dirname);

static void os_store(mp_obj_dict_t *g, const char *name, const void *fn) {
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g),
        MP_OBJ_NEW_QSTR(qstr_from_str(name)), MP_OBJ_FROM_PTR(fn));
}

void os_module_init(void) {
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("os"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);

    os_store(g, "getcwd",   &mp_os_getcwd_obj);
    os_store(g, "chdir",    &mp_os_chdir_obj);
    os_store(g, "listdir",  &mp_os_listdir_obj);
    os_store(g, "mkdir",    &mp_os_mkdir_obj);
    os_store(g, "remove",   &mp_os_remove_obj);
    os_store(g, "unlink",   &mp_os_remove_obj);   /* alias */
    os_store(g, "rename",   &mp_os_rename_obj);
    os_store(g, "stat",     &mp_os_stat_obj);

    /* os.path sub-module */
    mp_obj_t path_mod = mp_obj_new_module(qstr_from_str("os.path"));
    mp_obj_dict_t *pg  = mp_obj_module_get_globals(path_mod);
    os_store(pg, "exists",   &mp_os_path_exists_obj);
    os_store(pg, "isdir",    &mp_os_path_isdir_obj);
    os_store(pg, "isfile",   &mp_os_path_isfile_obj);
    os_store(pg, "join",     &mp_os_path_join_obj);
    os_store(pg, "basename", &mp_os_path_basename_obj);
    os_store(pg, "dirname",  &mp_os_path_dirname_obj);
    os_store(g,  "path",     MP_OBJ_FROM_PTR(path_mod));
}
