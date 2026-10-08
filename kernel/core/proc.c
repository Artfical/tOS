#include "proc.h"
#include "scheduler.h"
#include "paging.h"
#include "usermode.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "fsbridge.h"
#include "auth.h"
#include "syscall.h"

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

static void proc_entry(void)
{
    proc_t *p = proc_current();
    if (p) p->pid = task_get_pid();
    enter_user_mode(USER_CODE_BASE, USER_STACK_TOP);
}

int proc_spawn(const char *path)
{
    proc_t *p = proc_slot();
    if (!p) return -4;
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    if (vfs_access(path, VFS_ACC_X) != 0) return -5;

    uint32_t size = fsbridge_size(path);
    if (size <= 4 || size > USER_CODE_MAX_SIZE) return -2;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return -3;
    if (fsbridge_read(path, buf, size, 0) < 0) { free(buf); return -5; }

    /* the last 4 bytes are the program's real memory size (the .bss is not in the file) */
    uint32_t content = size - 4;
    uint32_t mem_size = (uint32_t)buf[content] | ((uint32_t)buf[content + 1] << 8) |
                        ((uint32_t)buf[content + 2] << 16) | ((uint32_t)buf[content + 3] << 24);
    if (mem_size < content) mem_size = content;
    if (mem_size > USER_CODE_MAX_SIZE) { free(buf); return -2; }

    uint32_t *dir = paging_create_dir();
    if (!dir) { free(buf); return -3; }
    uint32_t pages = (mem_size + 4095) / 4096;
    int ok = 1;
    for (uint32_t i = 0; i < pages && ok; i++) {
        uint32_t phys = alloc_physical_page();
        if (!phys || paging_map_in(dir, USER_CODE_BASE + i * 4096, phys, PTE_USER | PTE_WRITABLE) != 0) { ok = 0; if (phys) free_physical_page(phys); break; }
        uint32_t off = i * 4096;
        if (off < content) memcpy((void *)phys, buf + off, content - off < 4096 ? content - off : 4096);
    }
    for (int i = 1; i <= USER_STACK_PAGES && ok; i++) {
        uint32_t phys = alloc_physical_page();
        if (!phys || paging_map_in(dir, USER_STACK_TOP - i * 4096, phys, PTE_USER | PTE_WRITABLE) != 0) { ok = 0; if (phys) free_physical_page(phys); }
    }
    free(buf);
    if (!ok) { paging_destroy_dir(dir); return -3; }

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->pgdir = dir;
    p->brk = PROC_BRK_BASE;
    p->ppid = task_get_pid();
    for (int i = 0; i < PROC_MAX_FDS; i++) p->fd[i] = -1;
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);

    int pid = task_spawn_proc(proc_entry, p->name, p, dir);
    if (pid < 0) {
        p->used = 0;
        paging_destroy_dir(dir);
        return -4;
    }
    p->pid = (uint32_t)pid;
    return pid;
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
    if (task_kill(pid) != 0) return -1;
    p->exit_code = code;
    proc_release(p);
    return 0;
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
