#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

#define TASK_STATE_READY    0
#define TASK_STATE_RUNNING  1
#define TASK_STATE_SLEEPING 2
#define TASK_STATE_ZOMBIE   3

#define TASK_NAME_MAX   32
#define KERNEL_STACK_SZ 32768
#define MAX_TASKS       32

typedef struct task {
    uint32_t esp;
    uint32_t pid;
    uint32_t state;
    uint32_t sleep_ticks;
    uint32_t cpu_ticks;
    uint32_t in_use;
    uint32_t no_preempt;
    struct task *next;
    uint8_t *kernel_stack;
    char name[TASK_NAME_MAX];
    void *user_data;
    uint32_t uid, gid;      /* whose credentials the task runs with (0 = root / kernel) */
    uint32_t *pgdir;        /* the address space of a user process (NULL = the kernel's) */
    void *proc;             /* the process this task is, if any (see proc.h) */
} task_t;

void scheduler_init(void);
int  task_spawn(void (*entry)(void), const char *name);
/* Starts a task that is a user process: it gets its own address space, inherits the spawner's credentials and
 * terminal window, and is fully set up before the scheduler can first pick it. */
int  task_spawn_proc(void (*entry)(void), const char *name, void *proc, uint32_t *pgdir, uint32_t uid, uint32_t gid);   /* uid/gid 0xFFFFFFFF = the spawner's */
task_t *task_by_pid(uint32_t pid);
void task_yield(void);
void task_exit(void);
void task_sleep(uint32_t ms);
uint32_t timer_handler(uint32_t esp);
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
