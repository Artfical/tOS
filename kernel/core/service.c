#include "service.h"
#include "proc.h"
#include "scheduler.h"
#include "terminal.h"
#include "fsbridge.h"
#include "string.h"
#include "memory.h"
#include "serial.h"
#include "net.h"
#include "debugmon.h"

#define DEFS "/system/services.tmbl"      /* name|command|description|package */
#define ONFILE "/system/services.on"      /* names, one per line: started when tOS starts */

typedef struct {
    char name[24];
    uint32_t pid;                          /* of the process it started, 0 = not started */
    term_surface_t *log;                   /* where its output goes (no window shows it) */
} run_t;

static run_t runs[SERVICE_MAX];

static char *read_text(const char *path)
{
    if (!fsbridge_exists(path)) return 0;
    uint32_t n = fsbridge_size(path);
    char *b = malloc(n + 1);
    if (!b) return 0;
    int got = n ? fsbridge_read(path, b, n, 0) : 0;
    b[got > 0 ? got : 0] = 0;
    return b;
}

static int write_text(const char *path, const char *text)
{
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0) return -1;
    uint32_t n = (uint32_t)strlen(text);
    if (n && fsbridge_write(path, text, n, 0) != (int)n) return -1;
    return 0;
}

/* the line of definitions for `name` (a malloc'd copy), or 0 */
static char *def_line(const char *name)
{
    char *t = read_text(DEFS);
    char *found = 0;
    uint32_t nl = (uint32_t)strlen(name);
    for (char *p = t; p && *p && !found;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        uint32_t len = (uint32_t)(p - line);
        if (*p) p++;
        if (len > nl && !strncmp(line, name, nl) && line[nl] == '|') {
            found = malloc(len + 1);
            if (found) { memcpy(found, line, len); found[len] = 0; }
        }
    }
    if (t) free(t);
    return found;
}

static int is_on(const char *name)
{
    char *t = read_text(ONFILE);
    int on = 0;
    uint32_t nl = (uint32_t)strlen(name);
    for (char *p = t; p && *p && !on;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        uint32_t len = (uint32_t)(p - line);
        if (*p) p++;
        if (len == nl && !strncmp(line, name, nl)) on = 1;
    }
    if (t) free(t);
    return on;
}

static run_t *run_of(const char *name, int make)
{
    run_t *free_slot = 0;
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (runs[i].name[0] && !strcmp(runs[i].name, name)) return &runs[i];
        if (!runs[i].name[0] && !free_slot) free_slot = &runs[i];
    }
    if (make && free_slot) { memset(free_slot, 0, sizeof(*free_slot)); strncpy(free_slot->name, name, sizeof(free_slot->name) - 1); return free_slot; }
    return 0;
}

static int is_running(run_t *r)
{
    if (!r || !r->pid) return 0;
    proc_t *p = proc_by_pid(r->pid);
    return p && !p->done;
}

int service_list(service_info_t *out, int max)
{
    char *t = read_text(DEFS);
    int n = 0;
    for (char *p = t; p && *p && n < max;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *f[4] = { line, 0, 0, 0 };
        int k = 1;
        for (char *c = line; *c && k < 4; c++) if (*c == '|') { *c = 0; f[k++] = c + 1; }
        if (k < 3 || !f[0][0]) continue;
        service_info_t *s = &out[n++];
        memset(s, 0, sizeof(*s));
        strncpy(s->name, f[0], sizeof(s->name) - 1);
        strncpy(s->desc, f[2], sizeof(s->desc) - 1);
        if (k > 3) strncpy(s->package, f[3], sizeof(s->package) - 1);
        s->enabled = is_on(s->name);
        s->running = is_running(run_of(s->name, 0));
    }
    if (t) free(t);
    return n;
}

const char *service_strerror(int err)
{
    switch (err) {
        case -1: return "no such service";
        case -2: return "it is already running";
        case -3: return "it is not running";
        case -4: return "its program could not be started";
        case -5: return "out of memory";
        case -6: return "only root can do that (try sudo)";
        default: return "failed";
    }
}

int service_start(const char *name)
{
    char *def = def_line(name);
    if (!def) return -1;
    run_t *r = run_of(name, 1);
    if (!r) { free(def); return -5; }
    if (is_running(r)) { free(def); return -2; }

    /* name|command|description|package: the command line, split at spaces */
    char *cmd = strchr(def, '|');
    if (!cmd) { free(def); return -1; }
    cmd++;
    char *end = strchr(cmd, '|');
    if (end) *end = 0;
    const char *argv[17];
    int argc = 0;
    for (char *p = cmd; *p && argc < 16;) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    if (!argc) { free(def); return -4; }

    if (!r->log) {
        r->log = malloc(sizeof(term_surface_t));
        if (!r->log) { free(def); return -5; }
    }
    terminal_surface_init(r->log);
    r->log->hidden = 1;
    const char *envp[] = { "HOME=/root", "USER=root", "LOGNAME=root", "SHELL=/system/pkg/busybox/bin/sh", "TERM=xterm",
                           "PATH=/system/pkg/busybox/bin:/programs", "LANG=C.UTF-8", 0 };

    /* the program starts with the log as its terminal: it inherits the spawner's, so borrow it for a moment */
    void *saved = task_get_userdata();
    task_set_userdata(r->log);
    int pid = proc_spawn_argv(argv[0], argv, envp);
    task_set_userdata(saved);
    free(def);
    if (pid < 0) return -4;
    r->pid = (uint32_t)pid;
    serial_write("service: started ");
    serial_write(name);
    serial_write("\n");
    return 0;
}

int service_stop(const char *name)
{
    run_t *r = run_of(name, 0);
    if (!is_running(r)) { char *d = def_line(name); int known = d != 0; if (d) free(d); return known ? -3 : -1; }
    proc_signal_surface(r->log, 15);                      /* the program and what it started */
    for (int i = 0; i < 100 && is_running(r); i++) task_sleep(20);
    if (is_running(r)) proc_kill(r->pid, 137);
    serial_write("service: stopped ");
    serial_write(name);
    serial_write("\n");
    r->pid = 0;
    return 0;
}

int service_enable(const char *name, int on)
{
    char *d = def_line(name);
    if (!d) return -1;
    free(d);
    char *t = read_text(ONFILE);
    uint32_t cap = (t ? (uint32_t)strlen(t) : 0) + strlen(name) + 4;
    char *out = malloc(cap);
    if (!out) { if (t) free(t); return -5; }
    out[0] = 0;
    uint32_t nl = (uint32_t)strlen(name);
    for (char *p = t; p && *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        uint32_t len = (uint32_t)(p - line);
        if (*p) p++;
        if (len == 0 || (len == nl && !strncmp(line, name, nl))) continue;
        strncat(out, line, len);
        strcat(out, "\n");
    }
    if (on) { strcat(out, name); strcat(out, "\n"); }
    int rc = write_text(ONFILE, out);
    free(out);
    if (t) free(t);
    return rc;
}

/* "name|command|description": the package's folder fills in %PKG% */
int service_register(const char *line, const char *package_dir)
{
    const char *a = strchr(line, '|');
    const char *b = a ? strchr(a + 1, '|') : 0;
    if (!a || !b || a == line) return -1;
    char name[24];
    uint32_t nl = (uint32_t)(a - line);
    if (nl >= sizeof(name)) return -1;
    memcpy(name, line, nl);
    name[nl] = 0;
    const char *pkg = package_dir + strlen(package_dir);
    while (pkg > package_dir && pkg[-1] != '/') pkg--;

    char entry[512];
    entry[0] = 0;
    strcat(entry, name);
    strcat(entry, "|");
    for (const char *c = a + 1; c < b && strlen(entry) + strlen(package_dir) + 2 < sizeof(entry); c++) {
        if (!strncmp(c, "%PKG%", 5)) { strcat(entry, package_dir); c += 4; }
        else { char one[2] = { *c, 0 }; strcat(entry, one); }
    }
    strcat(entry, "|");
    strncat(entry, b + 1, sizeof(entry) - strlen(entry) - 1);
    strncat(entry, "|", sizeof(entry) - strlen(entry) - 1);
    strncat(entry, pkg, sizeof(entry) - strlen(entry) - 1);

    char *t = read_text(DEFS);
    uint32_t cap = (t ? (uint32_t)strlen(t) : 0) + (uint32_t)strlen(entry) + 4;
    char *out = malloc(cap);
    if (!out) { if (t) free(t); return -5; }
    out[0] = 0;
    for (char *p = t; p && *p;) {                          /* the same name again replaces its line */
        char *ln = p;
        while (*p && *p != '\n') p++;
        uint32_t len = (uint32_t)(p - ln);
        if (*p) p++;
        if (len == 0 || (len > nl && !strncmp(ln, name, nl) && ln[nl] == '|')) continue;
        strncat(out, ln, len);
        strcat(out, "\n");
    }
    strcat(out, entry);
    strcat(out, "\n");
    int rc = write_text(DEFS, out);
    free(out);
    if (t) free(t);
    return rc;
}

void service_unregister_package(const char *package)
{
    char *t = read_text(DEFS);
    if (!t) return;
    char *out = malloc(strlen(t) + 1);
    if (!out) { free(t); return; }
    out[0] = 0;
    for (char *p = t; *p;) {
        char *ln = p;
        while (*p && *p != '\n') p++;
        uint32_t len = (uint32_t)(p - ln);
        if (*p) p++;
        if (!len) continue;
        char tmp[512];
        if (len >= sizeof(tmp)) continue;
        memcpy(tmp, ln, len);
        tmp[len] = 0;
        char *f[4] = { tmp, 0, 0, 0 };
        int k = 1;
        for (char *c = tmp; *c && k < 4; c++) if (*c == '|') { *c = 0; f[k++] = c + 1; }
        if (k > 3 && !strcmp(f[3], package)) {
            service_stop(f[0]);
            service_enable(f[0], 0);
            continue;
        }
        strncat(out, ln, len);
        strcat(out, "\n");
    }
    write_text(DEFS, out);
    free(out);
    free(t);
}

int service_log(const char *name, char *out, int cap)
{
    run_t *r = run_of(name, 0);
    out[0] = 0;
    if (!r || !r->log) return -1;
    int n = 0;
    /* the history that scrolled off, then the screen */
    for (int b = r->log->scrollback_count; b >= 1 && n < cap - 82; b--) {
        const uint16_t *row = terminal_scrollback_row(r->log, b);
        int last = TERM_SURFACE_W - 1;
        while (last >= 0 && (uint8_t)row[last] <= ' ') last--;
        for (int x = 0; x <= last && n < cap - 2; x++) out[n++] = (char)(uint8_t)row[x];
        out[n++] = '\n';
    }
    for (int y = 0; y < TERM_SURFACE_H && n < cap - 82; y++) {
        const uint16_t *row = &r->log->cells[y * TERM_SURFACE_W];
        int last = TERM_SURFACE_W - 1;
        while (last >= 0 && (uint8_t)row[last] <= ' ') last--;
        if (last < 0 && y > r->log->cur_row) break;
        for (int x = 0; x <= last && n < cap - 2; x++) out[n++] = (char)(uint8_t)row[x];
        out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

/* the services that are switched on start once the network has an address (a server wants one), or after a
 * minute without */
void service_boot_task(void)
{
    uint64_t t0 = debugmon_uptime_ms64();
    while (!net_ip && debugmon_uptime_ms64() - t0 < 60000) task_sleep(200);
    service_info_t list[SERVICE_MAX];
    int n = service_list(list, SERVICE_MAX);
    for (int i = 0; i < n; i++)
        if (list[i].enabled && !list[i].running) service_start(list[i].name);
    task_exit();
}
