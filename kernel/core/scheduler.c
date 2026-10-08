#include "scheduler.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"
#include "debugmon.h"
#include "paging.h"
#include "tss.h"

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
    for (int i = 0; i < MAX_TASKS; i++) {
        if (&tasks[i] == current) continue;
        if (tasks[i].state == TASK_STATE_ZOMBIE || !tasks[i].in_use) {
            /* the finished task's kernel stack is nobody's now */
            if (tasks[i].in_use && tasks[i].kernel_stack && i > 1) { free(tasks[i].kernel_stack); tasks[i].kernel_stack = 0; }
            return i;
        }
    }
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

static int task_spawn_internal(void (*entry)(void), const char *name, void *proc, uint32_t *pgdir)
{
    int slot = find_free_slot();
    if (slot < 0) return -1;

    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    t->proc = proc;
    t->pgdir = pgdir;
    if (proc && current) t->user_data = current->user_data;      /* output goes to the spawner's window */
    t->pid = next_pid++;
    t->in_use = 1;
    t->state = TASK_STATE_READY;
    if (current) { t->uid = current->uid; t->gid = current->gid; }   /* children run as their parent */
    t->kernel_stack = malloc(KERNEL_STACK_SZ);
    if (!t->kernel_stack) {
        t->in_use = 0;
        return -1;
    }
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

int task_spawn(void (*entry)(void), const char *name)
{
    return task_spawn_internal(entry, name, 0, 0);
}

int task_spawn_proc(void (*entry)(void), const char *name, void *proc, uint32_t *pgdir)
{
    return task_spawn_internal(entry, name, proc, pgdir);
}

task_t *task_by_pid(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].in_use && tasks[i].pid == pid) return &tasks[i];
    return 0;
}

uint32_t timer_handler(uint32_t esp)
{
    outb(0x20, 0x20);
    system_ticks++;
    /* debugmon_uptime_ms() is TSC-based (calibrated once in kernel.c
     * before this handler ever takes over IDT gate 32), so it no longer
     * matters that task_yield()'s software "int $32" also lands here —
     * tick_count itself isn't load-bearing for wall-clock timing
     * anymore. (An earlier attempt distinguished real IRQ0 from a
     * software self-yield via the PIC's In-Service Register, but that
     * didn't hold up consistently across hypervisors.) */
    debugmon_tick();
    current->cpu_ticks++;
    current->esp = esp;

    if (current->no_preempt) {
        /* This task is inside a wait loop that must not be switched
         * away from mid-stride (e.g. a blocking ring3 keyboard read
         * that only masks IRQ0 at the PIC, which cannot cancel an
         * IRQ0 already in flight when the mask was applied). Still
         * counting ticks/cpu_ticks above keeps wall-clock timing and
         * accounting correct; we just skip the actual task switch. */
        return current->esp;
    }

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
    /* each user process runs in its own address space; kernel tasks run in the kernel's (all of them see
     * the kernel half, so a kernel task does not care which one is loaded) */
    {
        uint32_t *want = current->pgdir ? current->pgdir : paging_kernel_dir();
        if (want && paging_current_dir() != want) asm volatile("mov %0, %%cr3" : : "r"(want) : "memory");
        if (current->kernel_stack) tss_set_kernel_stack((uint32_t)current->kernel_stack + KERNEL_STACK_SZ);
    }
    return current->esp;
}

void task_preempt_disable(void) { current->no_preempt++; }
void task_preempt_enable(void)  { if (current->no_preempt) current->no_preempt--; }

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

void task_get_cred(uint32_t *uid, uint32_t *gid)
{
    *uid = current ? current->uid : 0;
    *gid = current ? current->gid : 0;
}

void task_set_cred(uint32_t uid, uint32_t gid)
{
    if (!current) return;
    current->uid = uid;
    current->gid = gid;
}

uint32_t task_get_uid(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].in_use && tasks[i].pid == pid) return tasks[i].uid;
    return 0;
}
void     task_set_userdata(void *p) { current->user_data = p; }
void    *task_get_userdata(void)    { return current ? current->user_data : 0; }
uint32_t task_count(void)  { return task_count_val; }
uint32_t task_get_ticks(void) { return system_ticks; }

int task_kill(uint32_t pid)
{
    if (pid == 0) return -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].in_use && tasks[i].pid == pid && tasks[i].state != TASK_STATE_ZOMBIE) {
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
