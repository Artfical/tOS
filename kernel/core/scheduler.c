#include "scheduler.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"

static task_t tasks[MAX_TASKS];
static task_t *current = 0;
static task_t *idle_task = 0;
static int task_count_val = 0;
static uint32_t next_pid = 1;
static volatile uint32_t system_ticks = 0;

extern void timer_irq_stub(void);

static void setup_task_stack(task_t *t, void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)((uint32_t)t->kernel_stack + KERNEL_STACK_SZ);
    *(--sp) = 0x202;
    *(--sp) = 0x08;
    *(--sp) = (uint32_t)entry;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0x10; *(--sp) = 0x10; *(--sp) = 0x10; *(--sp) = 0x10;
    t->esp = (uint32_t)sp;
}

static void idle_entry(void)
{
    for (;;) asm volatile("hlt");
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_STATE_ZOMBIE || !tasks[i].in_use)
            return i;
    return -1;
}

void scheduler_init(void)
{
    serial_write("sched: init\n");
    memset(tasks, 0, sizeof(tasks));

    idle_task = &tasks[0];
    idle_task->pid = 0;
    idle_task->in_use = 1;
    idle_task->state = TASK_STATE_READY;
    idle_task->kernel_stack = malloc(KERNEL_STACK_SZ);
    memset(idle_task->kernel_stack, 0, KERNEL_STACK_SZ);
    strcpy(idle_task->name, "idle");
    setup_task_stack(idle_task, idle_entry);

    task_t *main_task = &tasks[1];
    main_task->pid = 1;
    main_task->in_use = 1;
    main_task->state = TASK_STATE_RUNNING;
    asm volatile("mov %%esp, %0" : "=r"(main_task->esp));
    strcpy(main_task->name, "main");

    idle_task->next = main_task;
    main_task->next = idle_task;

    current = main_task;
    task_count_val = 2;
    next_pid = 2;

    uint32_t divisor = 11932;
    outb(0x43, 0x36);
    io_wait();
    outb(0x40, divisor & 0xFF);
    io_wait();
    outb(0x40, (divisor >> 8) & 0xFF);

    idt_set_gate(32, (uint32_t)timer_irq_stub, 0x08, 0x8E);

    serial_write("sched: PIT ~100Hz, main+idle tasks\n");
    terminal_writestring("[OK] Scheduler initialized\n");
}

int task_spawn(void (*entry)(void), const char *name)
{
    int slot = find_free_slot();
    if (slot < 0) return -1;

    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    t->pid = next_pid++;
    t->in_use = 1;
    t->state = TASK_STATE_READY;
    t->kernel_stack = malloc(KERNEL_STACK_SZ);
    if (!t->kernel_stack) return -1;
    memset(t->kernel_stack, 0, KERNEL_STACK_SZ);
    setup_task_stack(t, entry);

    if (name) {
        int i = 0;
        while (name[i] && i < TASK_NAME_MAX - 1) {
            t->name[i] = name[i];
            i++;
        }
        t->name[i] = 0;
    }

    task_t *last = current;
    while (last->next != current) last = last->next;
    t->next = current;
    last->next = t;
    task_count_val++;
    return t->pid;
}

uint32_t timer_handler(uint32_t esp)
{
    outb(0x20, 0x20);
    system_ticks++;
    current->cpu_ticks++;
    current->esp = esp;
    if (current->state == TASK_STATE_RUNNING)
        current->state = TASK_STATE_READY;

    task_t *next = current->next;
    int n = 0;
    while (next->state != TASK_STATE_READY) {
        next = next->next;
        if (++n > MAX_TASKS) { next = idle_task; break; }
    }

    current = next;
    current->state = TASK_STATE_RUNNING;
    return current->esp;
}

void task_yield(void)   { asm volatile("int $32"); }

static void unlink_task(task_t *t)
{
    task_t *p = t;
    while (p->next != t) p = p->next;
    if (p != t) p->next = t->next;
}

void task_exit(void)
{
    unlink_task(current);
    current->state = TASK_STATE_ZOMBIE;
    for (;;) asm volatile("int $32");
}

void task_sleep(uint32_t ms)
{
    for (uint32_t i = ms / 10; i > 0; i--)
        asm volatile("int $32");
}

task_t *task_current(void) { return current; }
void     task_set_userdata(void *p) { current->user_data = p; }
void    *task_get_userdata(void)    { return current ? current->user_data : 0; }
uint32_t task_count(void)  { return task_count_val; }
uint32_t task_get_ticks(void) { return system_ticks; }

int task_kill(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state != TASK_STATE_ZOMBIE) {
            unlink_task(&tasks[i]);
            tasks[i].state = TASK_STATE_ZOMBIE;
            return 0;
        }
    }
    return -1;
}

uint32_t task_get_pid(void)
{
    return current ? current->pid : 0;
}

const char *task_get_name(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == pid) return tasks[i].name;
    return "unknown";
}

uint32_t task_get_state(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == pid) return tasks[i].state;
    return 0xFFFFFFFF;
}

uint32_t task_get_cpu_ticks(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == pid) return tasks[i].cpu_ticks;
    return 0;
}

void task_foreach(void (*callback)(uint32_t pid, const char *name, uint32_t state))
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid || i == 0)
            callback(tasks[i].pid, tasks[i].name, tasks[i].state);
}
