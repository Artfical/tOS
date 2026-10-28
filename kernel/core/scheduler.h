#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "isr.h"

#define TASK_STATE_READY    0
#define TASK_STATE_RUNNING  1
#define TASK_STATE_SLEEPING 2
#define TASK_STATE_ZOMBIE   3

#define TASK_NAME_MAX   32
#define KERNEL_STACK_SZ 65536
#define MAX_TASKS       32

typedef struct task {
    uint64_t esp;           /* the kernel stack pointer of a task that is not running: its registers_t frame */
    uint32_t pid;
    uint32_t state;
    uint32_t sleep_ticks;
    uint32_t cpu_ticks;
    uint32_t in_use;
    uint32_t no_preempt;
    int user_thread;
    uint32_t thread_group;
    uint64_t clear_tid;
    uint64_t fs_base;       /* a 64-bit program's thread pointer (arch_prctl, clone) */
    uint64_t futex_addr;
    uint64_t futex_deadline_ms;
    uint32_t futex_waiting;
    int32_t futex_result;
    int tls_gdt_entry;
    uint32_t tls_base, tls_limit, tls_flags;
    struct task *next;
    uint8_t *kernel_stack;
    char name[TASK_NAME_MAX];
    void *user_data;
    uint32_t uid, gid;      /* whose credentials the task runs with (0 = root / kernel) */
    uint64_t *pgdir;        /* the address space of a user process (NULL = the kernel's) */
    void *proc;             /* the process this task is, if any (see proc.h) */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
} task_t;

void scheduler_init(void);
int  task_spawn(void (*entry)(void), const char *name);
/* Starts a task that is a user process: it gets its own address space, inherits the spawner's credentials and
 * terminal window, and is fully set up before the scheduler can first pick it. */
int  task_spawn_proc(void (*entry)(void), const char *name, void *proc, uint64_t *pgdir, uint32_t uid, uint32_t gid);   /* uid/gid 0xFFFFFFFF = the spawner's */
int  task_spawn_user_thread(const registers_t *parent_regs, uint64_t child_stack,
                            void *proc, uint64_t *pgdir, uint32_t thread_group,
                            int tls_gdt_entry, uint32_t tls_base, uint32_t tls_limit,
                            uint32_t tls_flags, uint64_t clear_tid);
int  task_kill_group(uint32_t thread_group, task_t *except);
int  task_futex_wait(uint64_t user_addr, uint32_t expected, uint64_t deadline_ms);
int  task_futex_wake(uint64_t user_addr, uint32_t count);
task_t *task_by_pid(uint32_t pid);
void task_yield(void);
void task_exit(void);
void task_sleep(uint32_t ms);
uint64_t timer_handler(uint64_t esp);
void task_preempt_disable(void);
void task_preempt_enable(void);
task_t *task_current(void);
void     task_set_userdata(void *p);
void    *task_get_userdata(void);
uint32_t task_count(void);
uint32_t task_get_ticks(void);
int      task_kill(uint32_t pid);
uint32_t task_get_pid(void);
const char *task_get_name(uint32_t pid);
uint32_t task_get_state(uint32_t pid);
uint32_t task_get_cpu_ticks(uint32_t pid);
void     task_get_cred(uint32_t *uid, uint32_t *gid);
void     task_set_cred(uint32_t uid, uint32_t gid);
uint32_t task_get_uid(uint32_t pid);
void     task_foreach(void (*callback)(uint32_t pid, const char *name, uint32_t state));

#endif
