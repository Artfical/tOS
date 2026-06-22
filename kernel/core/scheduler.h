#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

#define TASK_STATE_READY    0
#define TASK_STATE_RUNNING  1
#define TASK_STATE_SLEEPING 2
#define TASK_STATE_ZOMBIE   3

#define TASK_NAME_MAX   32
#define KERNEL_STACK_SZ 4096
#define MAX_TASKS       32

typedef struct task {
    uint32_t esp;
    uint32_t pid;
    uint32_t state;
    uint32_t sleep_ticks;
    uint32_t in_use;
    struct task *next;
    uint8_t *kernel_stack;
    char name[TASK_NAME_MAX];
    void *user_data;
} task_t;

void scheduler_init(void);
int  task_spawn(void (*entry)(void), const char *name);
void task_yield(void);
void task_exit(void);
void task_sleep(uint32_t ms);
uint32_t timer_handler(uint32_t esp);
task_t *task_current(void);
void     task_set_userdata(void *p);
void    *task_get_userdata(void);
uint32_t task_count(void);
uint32_t task_get_ticks(void);
int      task_kill(uint32_t pid);
uint32_t task_get_pid(void);
const char *task_get_name(uint32_t pid);
uint32_t task_get_state(uint32_t pid);
void     task_foreach(void (*callback)(uint32_t pid, const char *name, uint32_t state));

#endif
