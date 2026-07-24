/* tos_micropython_exec_file() — run a MicroPython script straight out
 * of tOS's VFS (via fsbridge), without any of the real-file-handle
 * machinery upstream pyexec_file() needs (this port has no POSIX/VFS
 * mp_reader, just fsbridge_read() into a flat buffer).
 *
 * This mirrors parse_compile_execute()'s EXEC_FLAG_SOURCE_IS_VSTR path
 * in shared/runtime/pyexec.c, but is written here instead of patched
 * into that vendored file so upstream pyexec.c stays untouched. */

#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/lexer.h"
#include "py/parse.h"
#include "py/qstr.h"
#include "py/mpprint.h"
#include "py/objlist.h"
#include "terminal.h"
#include "memory.h"
#include "fsbridge.h"
#include <string.h>

/* sys.argv[0] is conventionally the script path; argv[1:] are the
 * extra words the shell passed after it (e.g. `git clone <url>` runs
 * /programs/git/git.py with sys.argv == ["git", "clone", "<url>"]). */
static void set_sys_argv(int argc, char **argv)
{
    mp_obj_list_t *list = MP_OBJ_TO_PTR(mp_sys_argv);
    mp_obj_list_init(list, (size_t)argc);
    for (int i = 0; i < argc; i++) {
        list->items[i] = mp_obj_new_str(argv[i], strlen(argv[i]));
    }
}

int tos_micropython_exec_file(const char *path);

int tos_micropython_exec_file_argv(const char *path, int argc, char **argv)
{
    set_sys_argv(argc, argv);
    return tos_micropython_exec_file(path);
}

int tos_micropython_exec_file(const char *path)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) {
        terminal_writestring("micropython: file not found: ");
        terminal_writestring(path);
        terminal_writestring("\r\n");
        return -1;
    }

    uint32_t size = fsbridge_size(path);
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        terminal_writestring("micropython: out of memory\r\n");
        return -1;
    }
    if (fsbridge_read(path, buf, size, 0) < 0) {
        free(buf);
        terminal_writestring("micropython: read failed\r\n");
        return -1;
    }
    buf[size] = 0;

    int ret = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        /* free_len=0: we own buf and free it ourselves below, rather
         * than handing ownership to the lexer/reader. */
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(path), buf, size, 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&pt, source_name, false);
        mp_call_function_0(module_fun);
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        ret = -1;
    }

    free(buf);
    return ret;
}
