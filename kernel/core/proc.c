#include "proc.h"
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

/* finished processes nobody waited for give their slot back when a new one is needed */
static proc_t *proc_slot(void)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (!procs[i].used) return &procs[i];
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].done) { procs[i].used = 0; return &procs[i]; }
    return 0;
}

void proc_fd_close(proc_t *p, int slot)
{
    if (!p || slot < 0 || slot >= PROC_MAX_FDS || p->fd[slot] == -1) return;
    int value = p->fd[slot];
    if (value <= PROC_FD_SOCK_BASE && value > PROC_FD_SOCK_BASE - LSOCK_MAX) {
        lsock_close(PROC_FD_SOCK_BASE - value);
    } else if (value <= PROC_FD_EVENTFD_BASE && value > PROC_FD_EVENTFD_BASE - PROC_MAX_EVENTFDS) {
        p->eventfds[PROC_FD_EVENTFD_BASE - value].used = 0;
    } else if (value >= 0) {
        vfs_close(value);
    } else {
        for (int i = 0; i < PROC_MAX_PIPES; i++) {
            if (value == PROC_FD_PIPE_READ_BASE - i && p->pipes[i].read_refs)
                p->pipes[i].read_refs--;
            if (value == PROC_FD_PIPE_WRITE_BASE - i && p->pipes[i].write_refs)
                p->pipes[i].write_refs--;
            if (!p->pipes[i].read_refs && !p->pipes[i].write_refs)
                memset(&p->pipes[i], 0, sizeof(p->pipes[i]));
        }
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
    enter_user_mode(p ? p->entry : USER_CODE_BASE,
                    p ? p->user_stack : USER_STACK_TOP);
}

static int proc_map_stack(uint32_t *dir)
{
    if (!dir) return -1;
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        uint32_t phys = alloc_physical_page();
        if (!phys) return -1;
        memset((void *)phys, 0, PAGE_SIZE);
        if (paging_map_in(dir, USER_STACK_TOP - i * PAGE_SIZE, phys,
                          PTE_USER | PTE_WRITABLE) != 0) {
            free_physical_page(phys);
            return -1;
        }
    }
    return 0;
}

static int proc_write_user(uint32_t *dir, uint32_t dst, const void *src, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)src;
    while (size) {
        uint32_t phys = paging_virt_to_phys(dir, dst);
        if (!phys) return -1;
        uint32_t n = PAGE_SIZE - (dst & (PAGE_SIZE - 1u));
        if (n > size) n = size;
        memcpy((void *)phys, bytes, n);
        dst += n;
        bytes += n;
        size -= n;
    }
    return 0;
}

/* Linux i386 process-entry stack for the initial static ELF loader. */
static int proc_stack_copy_string(uint32_t *dir, uint32_t *sp, uint32_t bottom,
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

static int proc_build_elf_stack(uint32_t *dir, const char *path, uint32_t entry,
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
    uint32_t words[128];
    uint32_t argv_addr[PROC_EXEC_MAX_ARGS];
    uint32_t env_addr[PROC_EXEC_MAX_ENV];
    uint8_t random_bytes[16];
    uint32_t sp = USER_STACK_TOP;
    uint32_t bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    static const char platform[] = "i686";
    uint32_t path_addr, platform_addr, random_addr;

    if (!path || !sp_out || argc <= 0 || argc > PROC_EXEC_MAX_ARGS ||
        envc < 0 || envc > PROC_EXEC_MAX_ENV) return -1;
    if (proc_stack_copy_string(dir, &sp, bottom, path, &path_addr) != 0) return -1;

    for (int i = envc - 1; i >= 0; i--)
        if (proc_stack_copy_string(dir, &sp, bottom, envp[i], &env_addr[i]) != 0) return -1;
    for (int i = argc - 1; i >= 0; i--)
        if (proc_stack_copy_string(dir, &sp, bottom, argv[i], &argv_addr[i]) != 0) return -1;

    if (sp < bottom + sizeof(platform)) return -1;
    sp -= sizeof(platform);
    platform_addr = sp;
    if (proc_write_user(dir, sp, platform, sizeof(platform)) != 0) return -1;

    if (sp < bottom + sizeof(random_bytes)) return -1;
    csprng_fill(random_bytes, sizeof(random_bytes));
    sp -= sizeof(random_bytes);
    random_addr = sp;
    if (proc_write_user(dir, sp, random_bytes, sizeof(random_bytes)) != 0) return -1;

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
    if (sp < bottom + bytes) return -1;
    sp = (sp - bytes) & ~15u;
    if (sp < bottom || proc_write_user(dir, sp, words, bytes) != 0) return -1;
    *sp_out = sp;
    return 0;
}

static int proc_start(proc_t *p, const char *path, uint32_t *dir,
                      uint32_t entry, uint32_t user_stack, uint32_t brk_base,
                      int linux_abi)
{
    if (!p || !dir) return -4;

    memset(p, 0, sizeof(*p));
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

int proc_spawn_argv(const char *path, const char *const argv[], const char *const envp[])
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
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        uint32_t *dir = paging_create_dir();
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

    uint32_t *dir = paging_create_dir();
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
            memcpy((void *)phys, buf + off,
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

int proc_execve_argv(const char *path, const char *const argv[], const char *const envp[])
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    if (!p || !t || !path) return -ESRCH;
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -ENOENT;
    if (vfs_access(path, VFS_ACC_X) != 0) return -EACCES;

    const char *default_argv[2] = { path, 0 };
    const char *const *use_argv = argv ? argv : default_argv;
    int argc = proc_vector_count(use_argv, PROC_EXEC_MAX_ARGS);
    int envc = proc_vector_count(envp, PROC_EXEC_MAX_ENV);
    if (argc < 0 || envc < 0) return -E2BIG;
    if (argc == 0) { use_argv = default_argv; argc = 1; }

    uint32_t size = fsbridge_size(path);
    if (size < 4) return -ENOEXEC;
    uint8_t magic[4];
    if (fsbridge_read(path, magic, sizeof(magic), 0) != (int)sizeof(magic)) return -EIO;
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F')
        return -ENOEXEC;

    uint32_t *dir = paging_create_dir();
    if (!dir) return -ENOMEM;
    uint32_t entry = 0, brk_base = 0, phdr = 0, phnum = 0;
    int rc = elf32_load_static(path, size, dir, &entry, &brk_base, &phdr, &phnum);
    if (rc != 0) {
        paging_destroy_dir(dir);
        return rc == -2 ? -ENOMEM : -ENOEXEC;
    }
    if (proc_map_stack(dir) != 0) {
        paging_destroy_dir(dir);
        return -ENOMEM;
    }
    uint32_t user_sp;
    if (proc_build_elf_stack(dir, path, entry, phdr, phnum,
                             argc, use_argv, envc, envp, &user_sp) != 0) {
        paging_destroy_dir(dir);
        return -E2BIG;
    }

    uint32_t *old_dir = p->pgdir;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!(p->fd_flags[i] & 1u)) continue; /* Linux FD_CLOEXEC */
        proc_fd_close(p, i);
    }
    if (t->tls_gdt_entry >= 0) {
        gdt_tls_release(t->tls_gdt_entry);
        t->tls_gdt_entry = -1;
        t->tls_base = t->tls_limit = t->tls_flags = 0;
    }
    p->pgdir = dir;
    p->entry = entry;
    p->user_stack = user_sp;
    p->brk_base = brk_base;
    p->brk = brk_base;
    p->linux_abi = 1;
    t->pgdir = dir;
    const char *base = path;
    for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = 0;
    asm volatile("mov %0, %%cr3" : : "r"(dir) : "memory");
    if (old_dir) paging_destroy_dir(old_dir);

    enter_user_mode(entry, user_sp);
    __builtin_unreachable();
}

/* releases everything a process holds except its slot (kept until someone waits) */
static void proc_release(proc_t *p)
{
    syscall_proc_cleanup(p);
    uint32_t *dir = p->pgdir;
    p->pgdir = 0;
    p->done = 1;
    if (dir) paging_destroy_dir(dir);
}

void proc_exit(int code)
{
    proc_t *p = proc_current();
    if (p) {
        p->exit_code = code;
        asm volatile("cli");
        task_t *t = task_current();
        task_kill_group(p->pid, t);
        uint32_t *dir = p->pgdir;
        t->pgdir = 0;                                     /* from now on this task runs in the kernel's address space */
        asm volatile("mov %0, %%cr3" : : "r"(paging_kernel_dir()) : "memory");
        syscall_proc_cleanup(p);
        p->pgdir = 0;
        p->done = 1;
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
        *(volatile uint32_t *)t->clear_tid = 0;
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
