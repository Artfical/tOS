#include "tos_api.h"
#include "shell.h"
#include "fsbridge.h"
#include "vfs.h"
#include "wm.h"
#include "scheduler.h"
#include "string.h"

int tos_exec(const char *cmd, char *out, int out_max)
{
    if (out && out_max > 0) {
        shell_exec_capture(cmd, out, out_max);
    } else {
        char scratch[4];
        shell_exec_capture(cmd, scratch, sizeof(scratch));
    }
    return 0;
}

int tos_read(const char *path, char *out, int out_max)
{
    if (out_max <= 0) return -1;
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) { out[0] = 0; return -1; }

    uint32_t sz = fsbridge_size(path);
    if ((int)sz > out_max - 1) sz = (uint32_t)(out_max - 1);

    int n = fsbridge_read(path, out, sz, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

int tos_write(const char *path, const char *data, int len)
{
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0) return -1;
    if (len > 0 && fsbridge_write(path, data, (uint32_t)len, 0) < 0) return -1;
    return 0;
}

int tos_mkdir(const char *path) { return fsbridge_mkdir(path); }
int tos_delete(const char *path) { return fsbridge_delete(path); }
int tos_exists(const char *path) { return fsbridge_exists(path); }

int tos_list(const char *path, char *out, int out_max)
{
    if (out_max <= 0) return -1;
    out[0] = 0;

    vfs_entry_t entries[128];
    int n = fsbridge_list(path, entries, 128);
    if (n < 0) return -1;

    int k = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        int j = 0;
        while (entries[i].name[j] && k < out_max - 1) out[k++] = entries[i].name[j++];
        if (k < out_max - 1) out[k++] = '\n';
    }
    out[k < out_max ? k : out_max - 1] = 0;
    return n;
}

int tos_open_app(const char *name)
{
    return wm_open_app(name);
}

static char *ps_out;
static int ps_cap;
static int ps_len;

static void ps_append(const char *s)
{
    while (*s && ps_len < ps_cap - 1) ps_out[ps_len++] = *s++;
}

static void ps_append_uint(uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { ps_append("0"); return; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    char buf[12];
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    ps_append(buf);
}

static const char *ps_state_name(uint32_t state)
{
    switch (state) {
        case TASK_STATE_READY: return "READY";
        case TASK_STATE_RUNNING: return "RUN";
        case TASK_STATE_SLEEPING: return "SLEEP";
        case TASK_STATE_ZOMBIE: return "ZOMBIE";
        default: return "?";
    }
}

static void ps_collect(uint32_t pid, const char *name, uint32_t state)
{
    ps_append_uint(pid);
    ps_append(" ");
    ps_append(name);
    ps_append(" ");
    ps_append(ps_state_name(state));
    ps_append("\n");
}

int tos_ps(char *out, int out_max)
{
    if (out_max <= 0) return -1;
    ps_out = out;
    ps_cap = out_max;
    ps_len = 0;
    out[0] = 0;
    task_foreach(ps_collect);
    out[ps_len < ps_cap ? ps_len : ps_cap - 1] = 0;
    return 0;
}

int tos_kill(uint32_t pid)
{
    if (pid == 0) return -1;
    if (pid == task_get_pid()) return -1;
    if (wm_kill_task_window(pid) == 0) return 0;
    return task_kill(pid);
}

uint32_t tos_uptime(void)
{
    return task_get_ticks() / 100;
}
