#include "procfs.h"
#include "proc.h"
#include "string.h"
#include "memory.h"
#include "debugmon.h"
#include "cmos.h"
#include "auth.h"
#include "net.h"
#include "ip.h"
#include "scheduler.h"

static int app(char *buf, int cap, int n, const char *s)
{
    while (*s && n < cap - 1) buf[n++] = *s++;
    buf[n] = 0;
    return n;
}

static int app_u(char *buf, int cap, int n, uint64_t v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n < cap - 1) buf[n++] = t[--k];
    buf[n] = 0;
    return n;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(sub));
}

int procfs_is(const char *p) { return !strncmp(p, "/proc", 5) && (p[5] == 0 || p[5] == '/'); }

static proc_t *find_pid(uint32_t pid) { return proc_by_pid(pid); }

/* "self" or a number at the start of s: the pid (0 if neither), and where the rest begins */
static uint32_t parse_pid(const char *s, const char **rest)
{
    if (!strncmp(s, "self", 4) && (s[4] == 0 || s[4] == '/')) {
        proc_t *p = proc_current();
        *rest = s + 4;
        return p ? p->pid : 0;
    }
    uint32_t v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; any = 1; }
    if (!any || (*s != 0 && *s != '/')) return 0;
    *rest = s;
    return v;
}

static int gen_cpuinfo(char *b, int cap)
{
    int n = 0;
    uint32_t a, bb, c, d;
    char vendor[13], brand[49];
    cpuid(0, 0, &a, &bb, &c, &d);
    memcpy(vendor, &bb, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
    uint32_t maxext; cpuid(0x80000000u, 0, &maxext, &bb, &c, &d);
    strcpy(brand, "unknown");
    if (maxext >= 0x80000004u) {
        for (int i = 0; i < 3; i++) { cpuid(0x80000002u + (uint32_t)i, 0, &a, &bb, &c, &d); memcpy(brand + 16 * i, &a, 4); memcpy(brand + 16 * i + 4, &bb, 4); memcpy(brand + 16 * i + 8, &c, 4); memcpy(brand + 16 * i + 12, &d, 4); }
        brand[48] = 0;
    }
    char *name = brand; while (*name == ' ') name++;
    uint32_t f1a, f1b, f1c, f1d; cpuid(1, 0, &f1a, &f1b, &f1c, &f1d);
    uint32_t f7a = 0, f7b = 0, f7c = 0, f7d = 0; cpuid(7, 0, &f7a, &f7b, &f7c, &f7d);
    n = app(b, cap, n, "processor\t: 0\nvendor_id\t: "); n = app(b, cap, n, vendor);
    n = app(b, cap, n, "\ncpu family\t: "); n = app_u(b, cap, n, ((f1a >> 8) & 15) + (((f1a >> 8) & 15) == 15 ? (f1a >> 20) & 255 : 0));
    n = app(b, cap, n, "\nmodel\t\t: "); n = app_u(b, cap, n, ((f1a >> 4) & 15) | (((f1a >> 16) & 15) << 4));
    n = app(b, cap, n, "\nmodel name\t: "); n = app(b, cap, n, name);
    n = app(b, cap, n, "\nstepping\t: "); n = app_u(b, cap, n, f1a & 15);
    n = app(b, cap, n, "\ncpu MHz\t\t: 2000.000\ncache size\t: 4096 KB\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 13\nwp\t\t: yes\nflags\t\t:");
    static const struct { int reg; int bit; const char *n; } fl[] = {
        {0,0,"fpu"},{0,3,"pse"},{0,4,"tsc"},{0,5,"msr"},{0,6,"pae"},{0,8,"cx8"},{0,9,"apic"},{0,11,"sep"},{0,13,"pge"},{0,15,"cmov"},{0,16,"pat"},
        {0,19,"clflush"},{0,23,"mmx"},{0,24,"fxsr"},{0,25,"sse"},{0,26,"sse2"},{1,0,"pni"},{1,1,"pclmulqdq"},{1,9,"ssse3"},{1,12,"fma"},{1,13,"cx16"},
        {1,19,"sse4_1"},{1,20,"sse4_2"},{1,22,"movbe"},{1,23,"popcnt"},{1,25,"aes"},{1,26,"xsave"},{1,28,"avx"},{1,29,"f16c"},{1,30,"rdrand"},
        {2,3,"bmi1"},{2,5,"avx2"},{2,8,"bmi2"},{2,9,"erms"},{2,18,"rdseed"},{2,19,"adx"},{2,29,"sha_ni"},
    };
    for (unsigned i = 0; i < sizeof(fl) / sizeof(fl[0]); i++) {
        uint32_t v = fl[i].reg == 0 ? f1d : fl[i].reg == 1 ? f1c : f7b;
        if (v & (1u << fl[i].bit)) { n = app(b, cap, n, " "); n = app(b, cap, n, fl[i].n); }
    }
    n = app(b, cap, n, " lm\nbogomips\t: 4000.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 40 bits physical, 48 bits virtual\n\n");
    return n;
}

static int gen_meminfo(char *b, int cap)
{
    uint64_t total = (uint64_t)get_total_pages() * 4, used = (uint64_t)get_used_pages() * 4;
    uint64_t freek = total > used ? total - used : 0;
    int n = 0;
    struct { const char *k; uint64_t v; } rows[] = {
        {"MemTotal", total}, {"MemFree", freek}, {"MemAvailable", freek}, {"Buffers", 0}, {"Cached", 0}, {"SwapCached", 0},
        {"Active", used}, {"Inactive", 0}, {"SwapTotal", 0}, {"SwapFree", 0}, {"Dirty", 0}, {"Writeback", 0}, {"AnonPages", used},
        {"Mapped", 0}, {"Shmem", 0}, {"Slab", 0}, {"CommitLimit", total}, {"Committed_AS", used},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = app(b, cap, n, rows[i].k); n = app(b, cap, n, ":");
        int pad = 15 - (int)strlen(rows[i].k); while (pad-- > 0) n = app(b, cap, n, " ");
        char t[24]; int before = n; n = app_u(b, cap, n, rows[i].v); (void)t; (void)before;
        n = app(b, cap, n, " kB\n");
    }
    return n;
}

static int count_procs(uint32_t *last)
{
    int c = 0; *last = 0;
    for (uint32_t pid = 1; pid < 4096; pid++) { proc_t *p = find_pid(pid); if (p && !p->done) { c++; *last = pid; } }
    return c;
}

static char state_of(const proc_t *p) { return p->done ? 'Z' : 'R'; }

static int gen_stat_pid(proc_t *p, char *b, int cap)
{
    int n = 0;
    n = app_u(b, cap, n, p->pid); n = app(b, cap, n, " ("); n = app(b, cap, n, p->name); n = app(b, cap, n, ") ");
    char st[2] = { state_of(p), 0 }; n = app(b, cap, n, st);
    n = app(b, cap, n, " "); n = app_u(b, cap, n, p->ppid);
    n = app(b, cap, n, " "); n = app_u(b, cap, n, p->pid); n = app(b, cap, n, " "); n = app_u(b, cap, n, p->pid);
    n = app(b, cap, n, " 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    return n;
}

static int gen_status_pid(proc_t *p, char *b, int cap)
{
    int n = 0;
    n = app(b, cap, n, "Name:\t"); n = app(b, cap, n, p->name);
    n = app(b, cap, n, "\nUmask:\t0022\nState:\t"); char st[4] = { state_of(p), ' ', '(', 0 }; n = app(b, cap, n, st);
    n = app(b, cap, n, p->done ? "zombie)" : "running)");
    n = app(b, cap, n, "\nTgid:\t"); n = app_u(b, cap, n, p->pid);
    n = app(b, cap, n, "\nPid:\t"); n = app_u(b, cap, n, p->pid);
    n = app(b, cap, n, "\nPPid:\t"); n = app_u(b, cap, n, p->ppid);
    n = app(b, cap, n, "\nTracerPid:\t0\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t256\nThreads:\t1\nVmPeak:\t0 kB\nVmSize:\t0 kB\nVmRSS:\t0 kB\nSigQ:\t0/0\nCapEff:\t0000003fffffffff\nCpus_allowed:\t1\n");
    return n;
}

int procfs_gen(const char *path, char *b, int cap)
{
    if (!procfs_is(path) || path[5] != '/') return -1;
    const char *s = path + 6;
    uint32_t up = (uint32_t)(debugmon_uptime_ms64() / 10);                 /* hundredths of a second */
    if (!strcmp(s, "cpuinfo")) return gen_cpuinfo(b, cap);
    if (!strcmp(s, "meminfo")) return gen_meminfo(b, cap);
    if (!strcmp(s, "version")) return app(b, cap, 0, "Linux version 3.2.0-tOS (tOS@artfical) (gcc 15.3.0) #1 SMP tOS\n");
    if (!strcmp(s, "cmdline")) return app(b, cap, 0, "tOS\n");
    if (!strcmp(s, "filesystems")) return app(b, cap, 0, "nodev\tramfs\nnodev\tproc\n\ttfs\n");
    if (!strcmp(s, "mounts")) return app(b, cap, 0, "rootfs / rootfs rw 0 0\nproc /proc proc rw 0 0\n");
    if (!strcmp(s, "uptime")) { int n = app_u(b, cap, 0, up / 100); n = app(b, cap, n, "."); if (up % 100 < 10) n = app(b, cap, n, "0"); n = app_u(b, cap, n, up % 100); return app(b, cap, n, " 0.00\n"); }
    if (!strcmp(s, "loadavg")) { uint32_t last; int c = count_procs(&last); int n = app(b, cap, 0, "0.00 0.00 0.00 1/"); n = app_u(b, cap, n, (uint64_t)c); n = app(b, cap, n, " "); n = app_u(b, cap, n, last); return app(b, cap, n, "\n"); }
    if (!strcmp(s, "stat")) {
        cmos_time_t t; uint64_t btime = 0;
        if (cmos_get_time(&t) == 0) { /* seconds since 1970 of the boot, roughly */
            int y = t.year, m = t.month; uint64_t days = 0;
            for (int yy = 1970; yy < y; yy++) days += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
            static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            for (int mm = 1; mm < m; mm++) days += (uint64_t)dm[mm - 1] + ((mm == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
            days += (uint64_t)(t.day - 1);
            btime = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + (uint64_t)t.second - up / 100;
        }
        uint32_t last; int c = count_procs(&last);
        int n = app(b, cap, 0, "cpu  0 0 0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0 0 0\nintr 0\nctxt 0\nbtime "); n = app_u(b, cap, n, btime);
        n = app(b, cap, n, "\nprocesses "); n = app_u(b, cap, n, last); n = app(b, cap, n, "\nprocs_running "); n = app_u(b, cap, n, (uint64_t)c);
        return app(b, cap, n, "\nprocs_blocked 0\n");
    }
    if (!strcmp(s, "sys/kernel/osrelease")) return app(b, cap, 0, "3.2.0-tOS\n");
    if (!strcmp(s, "sys/kernel/ostype")) return app(b, cap, 0, "Linux\n");
    if (!strcmp(s, "sys/kernel/hostname")) { int n = app(b, cap, 0, auth_hostname() ? auth_hostname() : "tOS"); return app(b, cap, n, "\n"); }
    if (!strcmp(s, "sys/kernel/pid_max")) return app(b, cap, 0, "32768\n");
    if (!strcmp(s, "sys/kernel/random/boot_id")) return app(b, cap, 0, "00000000-0000-4000-8000-000000000001\n");
    if (!strcmp(s, "sys/vm/overcommit_memory")) return app(b, cap, 0, "1\n");
    if (!strcmp(s, "sys/vm/max_map_count")) return app(b, cap, 0, "65530\n");
    if (!strcmp(s, "sys/net/core/somaxconn")) return app(b, cap, 0, "128\n");
    const char *rest;
    uint32_t pid = parse_pid(s, &rest);
    if (pid) {
        proc_t *p = find_pid(pid);
        if (!p) return -1;
        if (!strcmp(rest, "/stat")) return gen_stat_pid(p, b, cap);
        if (!strcmp(rest, "/status")) return gen_status_pid(p, b, cap);
        if (!strcmp(rest, "/cmdline")) { int n = app(b, cap, 0, p->exe[0] ? p->exe : p->name); if (n < cap - 1) b[n++] = 0; return n; }
        if (!strcmp(rest, "/comm")) { int n = app(b, cap, 0, p->name); return app(b, cap, n, "\n"); }
        if (!strcmp(rest, "/environ") || !strcmp(rest, "/maps") || !strcmp(rest, "/smaps") || !strcmp(rest, "/mountinfo")) { b[0] = 0; return 0; }
        if (!strcmp(rest, "/statm")) return app(b, cap, 0, "0 0 0 0 0 0 0\n");
        if (!strcmp(rest, "/limits")) return app(b, cap, 0, "Limit                     Soft Limit           Hard Limit           Units\nMax open files            256                  256                  files\n");
    }
    return -1;
}

static void fill(vfs_entry_t *e, const char *name, int dir, uint32_t inode)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->is_dir = dir; e->inode = inode;
    e->mode = dir ? (0x4000u | 0555u) : (0x8000u | 0444u);
}

static const char *const top_files[] = { "cpuinfo", "meminfo", "version", "cmdline", "filesystems", "mounts", "uptime", "loadavg", "stat", 0 };
static const char *const pid_files[] = { "stat", "status", "cmdline", "comm", "environ", "maps", "statm", "limits", 0 };

int procfs_stat(const char *path, vfs_entry_t *e)
{
    if (!procfs_is(path)) return -1;
    if (path[5] == 0) { fill(e, "proc", 1, 1); return 0; }
    const char *s = path + 6;
    char tmp[4];
    if (procfs_gen(path, tmp, 1) >= 0) { fill(e, s, 0, 2); return 0; }          /* a file */
    if (!strcmp(s, "sys") || !strcmp(s, "sys/kernel") || !strcmp(s, "sys/vm") || !strcmp(s, "sys/net") || !strcmp(s, "sys/net/core") || !strcmp(s, "sys/kernel/random")) { fill(e, s, 1, 3); return 0; }
    const char *rest;
    uint32_t pid = parse_pid(s, &rest);
    if (pid && find_pid(pid)) {
        if (!*rest) { fill(e, s, 1, 100 + pid); return 0; }
        if (!strcmp(rest, "/fd") || !strcmp(rest, "/task")) { fill(e, s, 1, 100 + pid); return 0; }
        if (!strcmp(rest, "/exe") || !strcmp(rest, "/cwd") || !strcmp(rest, "/root")) { fill(e, s, 0, 100 + pid); e->mode = 0xA000u | 0777u; return 0; }
    }
    return -1;
}

int procfs_readdir(const char *path, vfs_entry_t *out, int max)
{
    if (!procfs_is(path)) return -1;
    int n = 0;
    if (path[5] == 0) {
        for (int i = 0; top_files[i] && n < max; i++) fill(&out[n++], top_files[i], 0, 2);
        if (n < max) fill(&out[n++], "self", 1, 4);
        if (n < max) fill(&out[n++], "sys", 1, 3);
        for (uint32_t pid = 1; pid < 4096 && n < max; pid++) {
            proc_t *p = find_pid(pid);
            if (!p) continue;
            char name[12]; int k = 0; uint32_t v = pid; char t[12]; int tk = 0;
            while (v) { t[tk++] = (char)('0' + v % 10); v /= 10; }
            while (tk) name[k++] = t[--tk];
            name[k] = 0;
            fill(&out[n++], name, 1, 100 + pid);
        }
        return n;
    }
    const char *s = path + 6;
    if (!strcmp(s, "sys")) { if (n < max) fill(&out[n++], "kernel", 1, 3); if (n < max) fill(&out[n++], "vm", 1, 3); if (n < max) fill(&out[n++], "net", 1, 3); return n; }
    if (!strcmp(s, "sys/kernel")) { const char *f[] = { "osrelease", "ostype", "hostname", "pid_max", 0 }; for (int i = 0; f[i] && n < max; i++) fill(&out[n++], f[i], 0, 2); if (n < max) fill(&out[n++], "random", 1, 3); return n; }
    if (!strcmp(s, "sys/vm")) { const char *f[] = { "overcommit_memory", "max_map_count", 0 }; for (int i = 0; f[i] && n < max; i++) fill(&out[n++], f[i], 0, 2); return n; }
    const char *rest;
    uint32_t pid = parse_pid(s, &rest);
    if (pid) {
        proc_t *p = find_pid(pid);
        if (!p) return -1;
        if (!*rest) {
            for (int i = 0; pid_files[i] && n < max; i++) fill(&out[n++], pid_files[i], 0, 2);
            if (n < max) fill(&out[n++], "fd", 1, 100 + pid);
            if (n < max) fill(&out[n++], "exe", 0, 100 + pid);
            return n;
        }
        if (!strcmp(rest, "/fd")) {
            for (int fd = 0; fd < 3 + PROC_MAX_FDS && n < max; fd++) {
                int open = fd < 3 || p->fd[fd - 3] != -1;
                if (!open) continue;
                char name[8]; int k = 0; int v = fd; char t[8]; int tk = 0;
                if (!v) t[tk++] = '0';
                while (v) { t[tk++] = (char)('0' + v % 10); v /= 10; }
                while (tk) name[k++] = t[--tk];
                name[k] = 0;
                fill(&out[n], name, 0, 5);
                out[n].mode = 0xA000u | 0700u;
                n++;
            }
            return n;
        }
    }
    return -1;
}

int procfs_readlink(const char *path, char *buf, int cap)
{
    if (!procfs_is(path) || path[5] != '/') return -1;
    const char *rest;
    uint32_t pid = parse_pid(path + 6, &rest);
    if (!pid) return -1;
    proc_t *p = find_pid(pid);
    if (!p) return -1;
    const char *target = 0;
    if (!strcmp(rest, "/exe")) target = p->exe[0] ? p->exe : p->name;
    else if (!strcmp(rest, "/cwd")) target = p->cwd[0] ? p->cwd : "/";
    else if (!strcmp(rest, "/root")) target = "/";
    if (!target) return -1;
    int n = (int)strlen(target);
    if (n > cap) n = cap;
    memcpy(buf, target, (size_t)n);
    return n;
}
