#include "proc.h"
#include "io.h"
#include "linux_abi.h"
#include "tty.h"
#include "scheduler.h"
#include "paging.h"
#include "usermode.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "fsbridge.h"
#include "auth.h"
#include "serial.h"
#include "lsock.h"
#include "syscall.h"
#include "elf32.h"
#include "csprng.h"
#include "fpu.h"
#include "errno.h"
#include "gdt.h"

static proc_t procs[PROC_MAX];

proc_t *proc_current(void)
{
    task_t *t = task_current();
    return t ? (proc_t *)t->proc : 0;
}

proc_t *proc_by_pid(uint32_t pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return 0;
}

int proc_running_count(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && !procs[i].done) n++;
    return n;
}

static proc_pipe_t pipes[PROC_MAX_PIPES];

proc_pipe_t *proc_pipe(int index)
{
    return index >= 0 && index < PROC_MAX_PIPES && pipes[index].used ? &pipes[index] : 0;
}

int proc_pipe_alloc(void)
{
    uint64_t fl = irq_save();
    for (int i = 0; i < PROC_MAX_PIPES; i++) {
        if (pipes[i].used) continue;
        pipes[i].used = 1;                       /* claimed before the allocation can yield */
        irq_restore(fl);
        uint8_t *data = malloc(PROC_PIPE_CAPACITY);
        if (!data) { pipes[i].used = 0; return -1; }
        pipes[i].data = data;
        pipes[i].head = pipes[i].tail = pipes[i].count = 0;
        pipes[i].read_refs = pipes[i].write_refs = 1;
        return i;
    }
    irq_restore(fl);
    return -1;
}

void proc_pipe_ref(int index, int write_end)
{
    proc_pipe_t *pp = proc_pipe(index);
    if (!pp) return;
    if (write_end) pp->write_refs++; else pp->read_refs++;
}

void proc_pipe_unref(int index, int write_end)
{
    proc_pipe_t *pp = proc_pipe(index);
    if (!pp) return;
    if (write_end) { if (pp->write_refs) pp->write_refs--; }
    else if (pp->read_refs) pp->read_refs--;
    if (!pp->read_refs && !pp->write_refs) {
        uint8_t *d = pp->data;
        pp->data = 0;
        pp->used = 0;
        if (d) free(d);
    }
}

/* finished processes nobody waited for give their slot back when a new one is needed */
static proc_t *proc_slot(void)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (!procs[i].used) return &procs[i];
    /* a finished child whose parent is a running process stays until that one waits (wait4) */
    for (int i = 0; i < PROC_MAX; i++) {
        if (!procs[i].done) continue;
        proc_t *parent = proc_by_pid(procs[i].ppid);
        if (parent && !parent->done) continue;
        procs[i].used = 0;
        return &procs[i];
    }
    return 0;
}

void proc_fd_close(proc_t *p, int slot)
{
    if (!p || slot < 0 || slot >= PROC_MAX_FDS || p->fd[slot] == -1) return;
    int value = p->fd[slot];
    if (value <= PROC_FD_SOCK_BASE && value > PROC_FD_SOCK_BASE - LSOCK_MAX) {
        lsock_close(PROC_FD_SOCK_BASE - value);
    } else if (value <= PROC_FD_EVENTFD_BASE && value > PROC_FD_EVENTFD_BASE - PROC_MAX_EVENTFDS) {
        if (p->eventfds[PROC_FD_EVENTFD_BASE - value].used)
            p->eventfds[PROC_FD_EVENTFD_BASE - value].used--;      /* one descriptor fewer */
    } else if (value >= 0) {
        vfs_close(value);
    } else if (PROC_FD_IS_SPAIR(value)) {
        proc_pipe_unref(PROC_FD_SPAIR_R(value), 0);
        proc_pipe_unref(PROC_FD_SPAIR_W(value), 1);
    } else {
        if (value <= PROC_FD_PIPE_READ_BASE && value > PROC_FD_PIPE_READ_BASE - PROC_MAX_PIPES)
            proc_pipe_unref(PROC_FD_PIPE_READ_BASE - value, 0);
        else if (value <= PROC_FD_PIPE_WRITE_BASE && value > PROC_FD_PIPE_WRITE_BASE - PROC_MAX_PIPES)
            proc_pipe_unref(PROC_FD_PIPE_WRITE_BASE - value, 1);
    }
    for (int i = 0; i < PROC_MAX_EPOLL_WATCH; i++)
        if (p->epoll_watch[i].epfd && (p->epoll_watch[i].fd == slot + 3 || p->epoll_watch[i].epfd == slot + 3))
            p->epoll_watch[i].epfd = 0;
    p->fd[slot] = -1;
    p->fd_flags[slot] = 0;
    p->fd_open_flags[slot] = 0;
    p->fdpath[slot][0] = 0;
    p->fd_dir_offset[slot] = 0;
}

static void proc_entry(void)
{
    proc_t *p = proc_current();
    if (p) p->pid = task_get_pid();
    enter_user_mode_as(p ? p->entry : USER_CODE_BASE,
                       p ? p->user_stack : USER_STACK_TOP, p && p->linux_abi == 2);
}

static int proc_map_stack(uint64_t *dir)
{
    if (!dir) return -1;
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        uint32_t phys = alloc_physical_page();
        if (!phys) return -1;
        memset(P2V(phys), 0, PAGE_SIZE);
        if (paging_map_in(dir, USER_STACK_TOP - i * PAGE_SIZE, phys,
                          PTE_USER | PTE_WRITABLE) != 0) {
            free_physical_page(phys);
            return -1;
        }
    }
    return 0;
}

static int proc_write_user(uint64_t *dir, uint32_t dst, const void *src, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)src;
    while (size) {
        uint32_t phys = paging_virt_to_phys(dir, dst);
        if (!phys) return -1;
        uint32_t n = PAGE_SIZE - (dst & (PAGE_SIZE - 1u));
        if (n > size) n = size;
        memcpy(P2V(phys), bytes, n);
        dst += n;
        bytes += n;
        size -= n;
    }
    return 0;
}

/* Linux i386 process-entry stack for the initial static ELF loader. */
static int proc_stack_copy_string(uint64_t *dir, uint32_t *sp, uint32_t bottom,
                                  const char *s, uint32_t *addr_out)
{
    uint32_t len = 0;
    while (len < PROC_EXEC_ARG_MAX && s[len]) len++;
    if (len == PROC_EXEC_ARG_MAX) return -1;
    len++;
    if (*sp < bottom + len) return -1;
    *sp -= len;
    if (proc_write_user(dir, *sp, s, len) != 0) return -1;
    *addr_out = *sp;
    return 0;
}

static int proc_build_elf_stack(uint64_t *dir, const char *path, uint32_t entry,
                                uint32_t phdr, uint32_t phnum,
                                int argc, const char *const argv[],
                                int envc, const char *const envp[],
                                uint32_t *sp_out)
{
    enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5,
           AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9,
           AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14,
           AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23,
           AT_RANDOM = 25, AT_EXECFN = 31, AT_PLATFORM = 15 };
    uint8_t random_bytes[16];
    uint32_t sp = USER_STACK_TOP;
    uint32_t bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    static const char platform[] = "i686";
    uint32_t path_addr, platform_addr, random_addr;

    if (!path || !sp_out || argc <= 0 || argc > PROC_EXEC_MAX_ARGS ||
        envc < 0 || envc > PROC_EXEC_MAX_ENV) return -1;
    /* argv and envp addresses, then the whole vector (argc, argv, envp, auxv) */
    uint32_t *addrs = malloc(sizeof(uint32_t) * (uint32_t)(argc + envc + 1));
    uint32_t *words = malloc(sizeof(uint32_t) * (uint32_t)(argc + envc + 48));
    if (!addrs || !words) { if (addrs) free(addrs); if (words) free(words); return -1; }
    uint32_t *argv_addr = addrs, *env_addr = addrs + argc;
    int rc = -1;
    if (proc_stack_copy_string(dir, &sp, bottom, path, &path_addr) != 0) goto out;

    for (int i = envc - 1; i >= 0; i--)
        if (proc_stack_copy_string(dir, &sp, bottom, envp[i], &env_addr[i]) != 0) goto out;
    for (int i = argc - 1; i >= 0; i--)
        if (proc_stack_copy_string(dir, &sp, bottom, argv[i], &argv_addr[i]) != 0) goto out;

    if (sp < bottom + sizeof(platform)) goto out;
    sp -= sizeof(platform);
    platform_addr = sp;
    if (proc_write_user(dir, sp, platform, sizeof(platform)) != 0) goto out;

    if (sp < bottom + sizeof(random_bytes)) goto out;
    csprng_fill(random_bytes, sizeof(random_bytes));
    sp -= sizeof(random_bytes);
    random_addr = sp;
    if (proc_write_user(dir, sp, random_bytes, sizeof(random_bytes)) != 0) goto out;

    uint32_t hwcap = 1u; /* FPU */
    if (fpu_has_sse2()) hwcap |= (1u << 24) | (1u << 25) | (1u << 26); /* FXSR/SSE/SSE2 */

    int n = 0;
    words[n++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) words[n++] = argv_addr[i];
    words[n++] = 0;                 /* argv terminator */
    for (int i = 0; i < envc; i++) words[n++] = env_addr[i];
    words[n++] = 0;
#define PUSH_AUX(type, value) do { words[n++] = (type); words[n++] = (value); } while (0)
    PUSH_AUX(AT_PHDR, phdr);
    PUSH_AUX(AT_PHENT, 32);
    PUSH_AUX(AT_PHNUM, phnum);
    PUSH_AUX(AT_PAGESZ, PAGE_SIZE);
    PUSH_AUX(AT_BASE, 0);
    PUSH_AUX(AT_FLAGS, 0);
    PUSH_AUX(AT_ENTRY, entry);
    PUSH_AUX(AT_UID, auth_uid());
    PUSH_AUX(AT_EUID, auth_uid());
    PUSH_AUX(AT_GID, auth_gid());
    PUSH_AUX(AT_EGID, auth_gid());
    PUSH_AUX(AT_HWCAP, hwcap);
    PUSH_AUX(AT_CLKTCK, 100);
    PUSH_AUX(AT_SECURE, 0);
    PUSH_AUX(AT_RANDOM, random_addr);
    PUSH_AUX(AT_EXECFN, path_addr);
    PUSH_AUX(AT_PLATFORM, platform_addr);
    PUSH_AUX(AT_NULL, 0);
#undef PUSH_AUX

    uint32_t bytes = (uint32_t)n * sizeof(uint32_t);
    if (sp < bottom + bytes) goto out;
    sp = (sp - bytes) & ~15u;
    if (sp < bottom || proc_write_user(dir, sp, words, bytes) != 0) goto out;
    *sp_out = sp;
    rc = 0;
out:
    free(addrs);
    free(words);
    return rc;
}

static int proc_start(proc_t *p, const char *path, uint64_t *dir,
                      uint64_t entry, uint64_t user_stack, uint32_t brk_base,
                      int linux_abi)
{
    if (!p || !dir) return -4;

    void *mm64 = linux_abi == 2 ? p->mm64 : 0;        /* made by linux64_load before this */
    memset(p, 0, sizeof(*p));
    p->mm64 = mm64;
    p->used = 1;
    p->pgdir = dir;
    p->entry = entry;
    p->user_stack = user_stack;
    p->brk_base = brk_base;
    p->brk = brk_base;
    p->linux_abi = linux_abi;
    p->ppid = task_get_pid();
    for (int i = 0; i < PROC_MAX_FDS; i++) p->fd[i] = -1;
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);

    /* a setuid / setgid program runs as its owner / group, provided nobody but the owner can have changed it */
    uint32_t run_uid = 0xFFFFFFFFu, run_gid = 0xFFFFFFFFu;
    {
        vfs_entry_t st;
        if (vfs_stat(path, &st) == 0 && vfs_chmod_supported(path) && !(st.mode & 022)) {
            if (st.mode & 04000) run_uid = st.uid;
            if (st.mode & 02000) run_gid = st.gid;
        }
    }
    int pid = task_spawn_proc(proc_entry, p->name, p, dir, run_uid, run_gid);
    if (pid < 0) {
        p->used = 0;
        p->pgdir = 0;
        paging_destroy_dir(dir);
        return -4;
    }
    p->pid = (uint32_t)pid;
    return pid;
}

static int proc_vector_count(const char *const vector[], int max)
{
    if (!vector) return 0;
    for (int i = 0; i <= max; i++)
        if (!vector[i]) return i;
    return -1;
}

static int proc_spawn_argv_locked(const char *path, const char *const argv[], const char *const envp[]);

/* A kernel task that starts a program can be switched out anywhere, and the programs running meanwhile change
 * the same tables (memory, files, processes) as the loader does: loading goes on without a switch. */
int proc_spawn_argv(const char *path, const char *const argv[], const char *const envp[])
{
    task_preempt_disable();
    int r = proc_spawn_argv_locked(path, argv, envp);
    task_preempt_enable();
    return r;
}

static int proc_spawn_argv_locked(const char *path, const char *const argv[], const char *const envp[])
{
    proc_t *p = proc_slot();
    if (!p) return -4;
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    if (vfs_access(path, VFS_ACC_X) != 0) return -5;

    const char *default_argv[2] = { path, 0 };
    if (!argv) argv = default_argv;
    int argc = proc_vector_count(argv, PROC_EXEC_MAX_ARGS);
    int envc = proc_vector_count(envp, PROC_EXEC_MAX_ENV);
    if (argc < 0 || envc < 0) return -2;
    if (argc == 0) { argv = default_argv; argc = 1; }

    uint32_t size = fsbridge_size(path);
    if (size < 4) return -2;

    uint8_t magic[4];
    if (fsbridge_read(path, magic, sizeof(magic), 0) != (int)sizeof(magic)) return -5;
    if (linux64_is_elf64(path)) {
        /* a 64-bit Linux program: its address space is built in the slot, then it starts */
        uint64_t *dir = paging_create_dir();
        if (!dir) return -3;
        memset(p, 0, sizeof(*p));
        p->pgdir = dir;
        uint64_t entry = 0, sp = 0;
        int rc = linux64_load(p, path, argc, argv, envc, envp, &entry, &sp);
        if (rc != 0) {
            linux64_mm_free(p);
            paging_destroy_dir(dir);
            p->pgdir = 0;
            return -2;
        }
        return proc_start(p, path, dir, entry, sp, 0, 2);
    }
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        uint64_t *dir = paging_create_dir();
        if (!dir) return -3;
        uint32_t entry = 0, brk_base = 0, phdr = 0, phnum = 0;
        int rc = elf32_load_static(path, size, dir, &entry, &brk_base, &phdr, &phnum);
        if (rc != 0) {
            paging_destroy_dir(dir);
            return rc == -2 ? -3 : -2;
        }
        if (proc_map_stack(dir) != 0) {
            paging_destroy_dir(dir);
            return -3;
        }
        uint32_t user_sp;
        if (proc_build_elf_stack(dir, path, entry, phdr, phnum,
                                 argc, argv, envc, envp, &user_sp) != 0) {
            paging_destroy_dir(dir);
            return -3;
        }
        return proc_start(p, path, dir, entry, user_sp, brk_base, 1);
    }

    /* Flat .t binaries end in their real memory size (the .bss is omitted). */
    if (size <= 4 || size > USER_CODE_MAX_SIZE) return -2;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return -3;
    if (fsbridge_read(path, buf, size, 0) != (int)size) { free(buf); return -5; }

    uint32_t content = size - 4;
    uint32_t mem_size = (uint32_t)buf[content] | ((uint32_t)buf[content + 1] << 8) |
                        ((uint32_t)buf[content + 2] << 16) | ((uint32_t)buf[content + 3] << 24);
    if (mem_size < content) mem_size = content;
    if (mem_size > USER_CODE_MAX_SIZE) { free(buf); return -2; }

    uint64_t *dir = paging_create_dir();
    if (!dir) { free(buf); return -3; }
    uint32_t pages = (mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
    int ok = 1;
    for (uint32_t i = 0; i < pages && ok; i++) {
        uint32_t phys = alloc_physical_page();
        if (!phys || paging_map_in(dir, USER_CODE_BASE + i * PAGE_SIZE, phys,
                                   PTE_USER | PTE_WRITABLE) != 0) {
            ok = 0;
            if (phys) free_physical_page(phys);
            break;
        }
        uint32_t off = i * PAGE_SIZE;
        if (off < content)
            memcpy(P2V(phys), buf + off,
                   content - off < PAGE_SIZE ? content - off : PAGE_SIZE);
    }
    if (ok && proc_map_stack(dir) != 0) ok = 0;
    free(buf);
    if (!ok) { paging_destroy_dir(dir); return -3; }
    return proc_start(p, path, dir, USER_CODE_BASE, USER_STACK_TOP, PROC_BRK_BASE, 0);
}

int proc_spawn(const char *path)
{
    return proc_spawn_argv(path, 0, 0);
}

/* where a command is: its path.tmbl entry (tpkg's), else the bin folder of an installed package (npm install -g
 * puts the commands it installs there). Used for "#!/usr/bin/env node" when a program runs a script. */
static int proc_command_lookup_arg(const char *name, char *out, int cap, char *farg, int farg_cap)
{
    if (farg) farg[0] = 0;
    if (!name[0] || strchr(name, '/')) return 0;
    uint32_t nl = (uint32_t)strlen(name);
    int found = 0;
    uint32_t size = fsbridge_exists("/system/path.tmbl") ? fsbridge_size("/system/path.tmbl") : 0;
    char *buf = size ? malloc(size + 1) : 0;
    if (buf) {
        int n = fsbridge_read("/system/path.tmbl", buf, size, 0);
        if (n > 0) {
            buf[n] = 0;
            for (char *q = buf; *q && !found;) {
                char *line = q;
                while (*q && *q != '\n') q++;
                if (*q) *q++ = 0;
                if (!strncmp(line, name, nl) && line[nl] == '=') {
                    strncpy(out, line + nl + 1, (size_t)cap - 1);
                    out[cap - 1] = 0;
                    char *sp = strchr(out, ' ');             /* "program first-argument" entries: the program */
                    if (sp) {
                        *sp = 0;
                        if (farg) { strncpy(farg, sp + 1, (size_t)farg_cap - 1); farg[farg_cap - 1] = 0; }
                    }
                    found = 1;
                }
            }
        }
        free(buf);
    }
    if (found) return 1;
    size = fsbridge_exists("/system/tpkg_db.tmbl") ? fsbridge_size("/system/tpkg_db.tmbl") : 0;
    buf = size ? malloc(size + 1) : 0;
    if (!buf) return 0;
    int n = fsbridge_read("/system/tpkg_db.tmbl", buf, size, 0);
    if (n > 0) {
        buf[n] = 0;
        for (char *q = buf; *q && !found;) {
            char *line = q;
            while (*q && *q != '\n') q++;
            if (*q) *q++ = 0;
            char *a = strchr(line, '|');
            char *b = a ? strchr(a + 1, '|') : 0;
            if (!b) continue;
            char cand[VFS_NAME_LEN];
            if (strlen(b + 1) + nl + 6 >= sizeof(cand)) continue;
            strcpy(cand, b + 1);
            strcat(cand, "/bin/");
            strcat(cand, name);
            if (fsbridge_exists(cand) && !fsbridge_is_dir(cand)) {
                strncpy(out, cand, (size_t)cap - 1);
                out[cap - 1] = 0;
                found = 1;
            }
        }
    }
    free(buf);
    return found;
}

static int proc_command_lookup(const char *name, char *out, int cap)
{
    return proc_command_lookup_arg(name, out, cap, 0, 0);
}

/* "/bin/ls", "/usr/bin/git"...: a command in one of the folders a Linux program expects them in; the name */
static const char *proc_std_bin_name(const char *path)
{
    static const char *const dirs[] = { "/bin/", "/usr/bin/", "/sbin/", "/usr/sbin/", "/usr/local/bin/", 0 };
    for (int i = 0; dirs[i]; i++) {
        size_t n = strlen(dirs[i]);
        if (!strncmp(path, dirs[i], n) && path[n] && !strchr(path + n, '/')) return path + n;
    }
    return 0;
}

/* the program a command in /bin, /usr/bin... stands for (stat and access of a file that is not there) */
int proc_std_bin_alias(const char *abs, char *out, int cap)
{
    const char *base = proc_std_bin_name(abs);
    return base && proc_command_lookup(base, out, cap);
}

/* "#!interpreter [one argument]" at the start of a file: 1 and the two parts (malloc'd) */
static int proc_shebang(const char *path, char **interp, char **arg)
{
    char line[256];
    int n = fsbridge_read(path, line, sizeof(line) - 1, 0);
    if (n < 3 || line[0] != '#' || line[1] != '!') return 0;
    line[n] = 0;
    char *e = line + 2;
    while (*e && *e != '\n') e++;
    if (!*e && n == (int)sizeof(line) - 1) return -1;              /* too long */
    *e = 0;
    char *i = line + 2;
    while (*i == ' ' || *i == '\t') i++;
    if (!*i) return -1;
    char *ie = i;
    while (*ie && *ie != ' ' && *ie != '\t') ie++;
    char *rest = ie;
    while (*rest == ' ' || *rest == '\t') rest++;
    char *re = rest + strlen(rest);
    while (re > rest && (re[-1] == ' ' || re[-1] == '\t' || re[-1] == '\r')) re--;
    *re = 0;
    *ie = 0;
    *interp = malloc((uint32_t)strlen(i) + 1);
    *arg = *rest ? malloc((uint32_t)strlen(rest) + 1) : 0;
    if (!*interp || (*rest && !*arg)) return -1;
    strcpy(*interp, i);
    if (*arg) strcpy(*arg, rest);
    return 1;
}

static void proc_after_vfork(proc_t *p);

int proc_execve_argv(const char *path_in, const char *const argv[], const char *const envp[], void *scratch)
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    if (!p || !t || !path_in) return -ESRCH;

    const char *default_argv[2] = { path_in, 0 };
    const char *const *use_argv = argv && argv[0] ? argv : default_argv;
    int argc = proc_vector_count(use_argv, PROC_EXEC_MAX_ARGS);
    int envc = proc_vector_count(envp, PROC_EXEC_MAX_ENV);
    if (argc < 0 || envc < 0) return -E2BIG;

    /* scripts: "#!/bin/sh -e" makes argv { "/bin/sh", "-e", path, argv[1]... }, up to 4 deep */
    char *owned[12];
    int owned_n = 0;
    const char **vec = 0;
    const char *path = path_in;
    int rc = 0;
    for (int depth = 0;; depth++) {
        if (depth == 0 && !vfs_exists_raw(path)) {
            /* /bin/ls is not a file here: tOS keeps its commands where tpkg registered them (path.tmbl) */
            const char *base = proc_std_bin_name(path);
            char prog[VFS_NAME_LEN], farg[48];
            if (base && proc_command_lookup_arg(base, prog, sizeof(prog), farg, sizeof(farg)) && fsbridge_exists(prog)) {
                char *np = malloc((uint32_t)strlen(prog) + 1);
                int extra = farg[0] ? 1 : 0;
                char *fa = 0;
                if (extra) { fa = malloc((uint32_t)strlen(farg) + 1); if (fa) strcpy(fa, farg); else extra = 0; }
                const char **nv = np ? malloc(sizeof(char *) * (uint32_t)(argc + extra + 1)) : 0;
                if (np && nv && argc + extra <= PROC_EXEC_MAX_ARGS) {
                    int k = 0;
                    strcpy(np, prog);
                    nv[k++] = extra ? np : use_argv[0];      /* an applet runner is called by its own name, the rest keep theirs */
                    if (extra) nv[k++] = fa;
                    for (int i = 1; i < argc; i++) nv[k++] = use_argv[i];
                    nv[k] = 0;
                    owned[owned_n++] = np;
                    if (fa) owned[owned_n++] = fa;
                    if (vec) free(vec);
                    vec = nv;
                    use_argv = nv;
                    argc = k;
                    path = np;
                } else { if (np) free(np); if (fa) free(fa); if (nv) free(nv); }
            }
        }
        if (!fsbridge_exists(path)) { rc = -ENOENT; goto fail; }
        if (fsbridge_is_dir(path)) { rc = -EACCES; goto fail; }
        if (vfs_access(path, VFS_ACC_X) != 0) { rc = -EACCES; goto fail; }
        char *interp = 0, *arg = 0;
        int sb = proc_shebang(path, &interp, &arg);
        if (sb == 0) break;
        if (sb == 1 && interp && arg && !strcmp(interp, "/usr/bin/env")) {      /* env name: where name is */
            char r[VFS_NAME_LEN];
            if (proc_command_lookup(arg, r, sizeof(r))) {
                char *ni = malloc((uint32_t)strlen(r) + 1);
                if (ni) { strcpy(ni, r); free(interp); free(arg); interp = ni; arg = 0; }
            }
        }
        if (interp) owned[owned_n++] = interp;
        if (arg) owned[owned_n++] = arg;
        if (sb < 0) { rc = -ENOEXEC; goto fail; }
        if (depth == 3 || owned_n > 8) { rc = -ELOOP; goto fail; }
        int extra = arg ? 2 : 1;
        if (argc + extra > PROC_EXEC_MAX_ARGS) { rc = -E2BIG; goto fail; }
        const char **nv = malloc(sizeof(char *) * (uint32_t)(argc + extra + 1));
        if (!nv) { rc = -ENOMEM; goto fail; }
        int k = 0;
        nv[k++] = interp;
        if (arg) nv[k++] = arg;
        nv[k++] = path;
        for (int i = 1; i < argc; i++) nv[k++] = use_argv[i];
        nv[k] = 0;
        if (vec) free(vec);
        vec = nv;
        use_argv = nv;
        argc = k;
        path = interp;
    }

    uint64_t *dir = paging_create_dir();
    if (!dir) { rc = -ENOMEM; goto fail; }
    uint64_t *old_dir = p->pgdir;
    void *old_mm = p->mm64;
    uint64_t entry = 0, user_sp = 0;
    uint32_t brk_base = 0;
    int abi;
    if (linux64_is_elf64(path)) {
        p->pgdir = dir;
        p->mm64 = 0;
        rc = linux64_load(p, path, argc, use_argv, envc, envp, &entry, &user_sp);
        if (rc != 0) {
            linux64_mm_free(p);
            p->pgdir = old_dir;
            p->mm64 = old_mm;
            paging_destroy_dir(dir);
            if (rc > 0) rc = -ENOEXEC;
            goto fail;
        }
        abi = 2;
    } else {
        uint8_t magic[4];
        uint32_t size = fsbridge_size(path);
        if (size < 4 || fsbridge_read(path, magic, sizeof(magic), 0) != (int)sizeof(magic) ||
            magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
            paging_destroy_dir(dir);
            rc = -ENOEXEC;
            goto fail;
        }
        uint32_t e32 = 0, phdr = 0, phnum = 0, sp32 = 0;
        int lr = elf32_load_static(path, size, dir, &e32, &brk_base, &phdr, &phnum);
        if (lr != 0 || proc_map_stack(dir) != 0 ||
            proc_build_elf_stack(dir, path, e32, phdr, phnum, argc, use_argv, envc, envp, &sp32) != 0) {
            paging_destroy_dir(dir);
            rc = lr == -2 ? -ENOMEM : lr ? -ENOEXEC : -E2BIG;
            goto fail;
        }
        entry = e32;
        user_sp = sp32;
        abi = 1;
        p->mm64 = 0;
    }

    /* no way back from here: the old program goes */
    task_kill_group(p->pid, t);                         /* its other threads */
    t->user_thread = 0;
    t->clear_tid = 0;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!(p->fd_flags[i] & 1u)) continue;           /* Linux FD_CLOEXEC */
        proc_fd_close(p, i);
    }
    if (t->tls_gdt_entry >= 0) {
        gdt_tls_release(t->tls_gdt_entry);
        t->tls_gdt_entry = -1;
        t->tls_base = t->tls_limit = t->tls_flags = 0;
    }
    t->fs_base = 0;
    for (int i = 1; i <= 64; i++)                       /* handlers do not survive; ignoring does */
        if (p->sig_handler[i] > 1) { p->sig_handler[i] = 0; p->sig_flags[i] = 0; }
    p->pgdir = dir;
    p->entry = entry;
    p->user_stack = user_sp;
    p->brk_base = brk_base;
    p->brk = brk_base;
    p->linux_abi = abi;
    t->pgdir = dir;
    {
        const char *base = path;
        for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
        memset(p->name, 0, sizeof(p->name));
        strncpy(p->name, base, sizeof(p->name) - 1);
        strncpy(t->name, base, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = 0;
    }
    paging_switch(dir);
    asm volatile("wrmsr" : : "c"(0xC0000100u), "a"(0u), "d"(0u));
    if (p->vfork_parent) {
        proc_after_vfork(p);                            /* the memory was the parent's: it keeps it */
    } else {
        if (old_mm) { void *keep = p->mm64; p->mm64 = old_mm; linux64_mm_free(p); p->mm64 = keep; }
        if (old_dir) paging_destroy_dir(old_dir);
    }
    for (int i = 0; i < owned_n; i++) free(owned[i]);
    if (vec) free(vec);
    if (scratch) free(scratch);
    fpu_state_init(t->fpu_state);                       /* a new program starts with a clean FPU */
    fpu_restore_state(t->fpu_state);
    enter_user_mode_as(entry, user_sp, abi == 2);
    __builtin_unreachable();

fail:
    for (int i = 0; i < owned_n; i++) free(owned[i]);
    if (vec) free(vec);
    return rc;
}

/* ---- fork, signals, wait ---- */

/* a vfork child stops using its parent's memory (execve or exit): the parent goes on */
static void proc_after_vfork(proc_t *p)
{
    p->vfork_parent = 0;
    p->vfork_done = 1;
}

int proc_fork(void *regs_ptr, int share_vm, uint64_t child_stack)
{
    registers_t *regs = regs_ptr;
    proc_t *parent = proc_current();
    task_t *pt = task_current();
    if (!parent || !pt || !regs) return -ESRCH;
    proc_t *c = proc_slot();
    if (!c) return -EAGAIN;

    task_preempt_disable();
    memcpy(c, parent, sizeof(*c));
    c->used = 1;
    c->done = 0;
    c->exit_code = 0;
    c->term_sig = 0;
    c->gfx = 0;
    c->win = c->win_prev = 0;
    c->sig_pending = 0;
    c->ppid = parent->pid;
    c->vfork_parent = 0;
    c->vfork_done = 0;
    c->mm64 = 0;
    c->pgdir = 0;

    uint64_t *dir;
    if (share_vm) {
        dir = parent->pgdir;
        c->mm64 = parent->mm64;
        c->vfork_parent = parent->pid;
    } else {
        if (parent->mm64 && linux64_mm_clone(c, parent) != 0) goto nomem;
        dir = paging_fork_dir(parent->pgdir);
        if (!dir) { linux64_mm_free(c); goto nomem; }
    }
    c->pgdir = dir;
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (c->fd[i] != -1 && syscall_fd_inherit(parent, i, c, i) != 0) {
            c->fd[i] = -1;
            c->fd_flags[i] = 0;
        }

    int tls = -1;
    if (pt->tls_gdt_entry >= 0) {                       /* a 32-bit program's TLS: a GDT entry of its own */
        tls = lx_tls_clone(pt->tls_base, pt->tls_limit, pt->tls_flags);
    }
    int pid = task_spawn_fork(regs, (const uint8_t *)regs - 520, c, dir, child_stack, tls);
    if (pid < 0) {
        if (tls >= 0) gdt_tls_release(tls);
        for (int i = 0; i < PROC_MAX_FDS; i++) proc_fd_close(c, i);
        if (!share_vm) { linux64_mm_free(c); paging_destroy_dir(dir); }
        c->used = 0;
        task_preempt_enable();
        return -EAGAIN;
    }
    c->pid = (uint32_t)pid;
    task_preempt_enable();

    if (share_vm) {
        /* vfork: the parent waits while the child runs in its memory */
        while (!c->vfork_done && c->used && c->pid == (uint32_t)pid) task_yield();
    }
    return pid;

nomem:
    c->used = 0;
    task_preempt_enable();
    return -ENOMEM;
}

int proc_signal(proc_t *q, int sig)
{
    if (!q || !q->used || q->done) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > 64) return -EINVAL;
    uint64_t h = q->sig_handler[sig];
    if (sig != 9 && sig != 19) {
        if (q->linux_abi == 2 && h > 1) {               /* its handler runs (linux64.c) */
            q->sig_pending |= 1ull << (sig - 1);
            return 0;
        }
        if (h == 1) return 0;                           /* SIG_IGN */
    }
    switch (sig) {
        case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 28:
            return 0;                                   /* by default: nothing (stopping is not supported) */
    }
    q->term_sig = sig;
    if (q == proc_current()) proc_exit(128 + sig);
    return proc_kill(q->pid, 128 + sig) == 0 ? 0 : -ESRCH;
}

void proc_signal_surface(void *surface, int sig)
{
    proc_t *self = proc_current();
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *q = &procs[i];
        if (!q->used || q->done || q == self) continue;
        task_t *t = task_by_pid(q->pid);
        if (t && t->user_data == surface) proc_signal(q, sig);
    }
    if (self && task_current() && task_current()->user_data == surface) proc_signal(self, sig);   /* last: it may not return */
}

/* a program that was started from the shell has ended: the shell's terminal is cooked again (a program that
 * died inside a full-screen mode, in raw mode, with the mouse on...) */
static void proc_tty_release(proc_t *p, void *surface)
{
    proc_t *parent = proc_by_pid(p->ppid);
    if (surface && !(parent && !parent->done && parent != p)) tty_detach(surface);
}

/* a process has ended: its parent hears of it (SIGCHLD), a vfork parent goes on */
static void proc_tell_parent(proc_t *p)
{
    if (p->vfork_parent) proc_after_vfork(p);
    proc_t *parent = proc_by_pid(p->ppid);
    if (parent && parent != p && !parent->done) proc_signal(parent, 17);
}

int proc_wait_child(int pid, int nohang, int *status)
{
    proc_t *me = proc_current();
    if (!me) return -ECHILD;
    for (;;) {
        int any = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t *q = &procs[i];
            if (!q->used || q->ppid != me->pid || q == me) continue;
            if (pid > 0 && q->pid != (uint32_t)pid) continue;
            any = 1;
            if (!q->done) continue;
            if (status) *status = q->term_sig ? (q->term_sig & 0x7F) : (q->exit_code & 0xFF) << 8;
            int got = (int)q->pid;
            q->used = 0;
            return got;
        }
        if (!any) return -ECHILD;
        if (nohang) return 0;
        if (me->sig_pending & ~task_current()->sig_blocked) return -EINTR;
        task_sleep(10);
    }
}

/* releases everything a process holds except its slot (kept until someone waits) */
static void proc_release(proc_t *p)
{
    {
        task_t *tt = task_by_pid(p->pid);
        proc_tty_release(p, tt ? tt->user_data : 0);
    }
    if (p->vfork_parent) { p->mm64 = 0; p->pgdir = 0; }      /* the parent's memory */
    syscall_proc_cleanup(p);
    uint64_t *dir = p->pgdir;
    p->pgdir = 0;
    p->done = 1;
    proc_tell_parent(p);
    if (dir) paging_destroy_dir(dir);
}

void proc_exit(int code)
{
    proc_t *p = proc_current();
    if (p) {
        p->exit_code = code;
        if (code != 0) {                                   /* a program that did not end well: say so on the serial line */
            char h[12];
            serial_write("proc: "); serial_write(p->name); serial_write(" (pid ");
            sprintf_u32(h, p->pid); serial_write(h); serial_write(") ended with code ");
            sprintf_u32(h, (uint32_t)code); serial_write(h);
            serial_write(p->term_sig ? " (killed by a signal)\n" : "\n");
        }
        proc_tty_release(p, task_current() ? task_current()->user_data : 0);
        asm volatile("cli");
        task_t *t = task_current();
        task_kill_group(p->pid, t);
        uint64_t *dir = p->pgdir;
        if (p->vfork_parent) { dir = 0; p->mm64 = 0; }     /* the parent's memory */
        t->pgdir = 0;                                     /* from now on this task runs in the kernel's address space */
        paging_switch(paging_kernel_dir());
        syscall_proc_cleanup(p);
        p->pgdir = 0;
        p->done = 1;
        proc_tell_parent(p);
        asm volatile("sti");
        if (dir) paging_destroy_dir(dir);
    }
    task_exit();
    for (;;) asm volatile("hlt");
}

int proc_kill(uint32_t pid, int code)
{
    proc_t *p = proc_by_pid(pid);
    if (!p || p->done) return -1;
    if (p == proc_current()) proc_exit(code);
    if (task_kill_group(pid, 0) == 0) return -1;
    p->exit_code = code;
    proc_release(p);
    return 0;
}

void proc_thread_exit(int code)
{
    (void)code;
    task_t *t = task_current();
    proc_t *p = proc_current();
    if (!t || !p || !t->user_thread) proc_exit(code);
    if (t->clear_tid && paging_user_range_ok(t->clear_tid, sizeof(uint32_t), 1)) {
        *(volatile uint32_t *)(uintptr_t)t->clear_tid = 0;
        task_futex_wake(t->clear_tid, 1);
    }
    task_exit();
    __builtin_unreachable();
}

int proc_wait(uint32_t pid)
{
    proc_t *p = proc_by_pid(pid);
    if (!p) return -1;
    while (!p->done) task_sleep(10);
    int code = p->exit_code;
    p->used = 0;
    return code;
}
