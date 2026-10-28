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
#include "fpu.h"
#include "gdt.h"

static task_t tasks[MAX_TASKS];
static task_t *current = 0;
static task_t *idle_task = 0;
static int task_count_val = 0;
static uint32_t next_pid = 1;
static volatile uint32_t system_ticks = 0;

extern void timer_irq_stub(void);
static void unlink_task(task_t *t);

/* the frame timer_irq_stub returns through, at the top of the task's kernel stack */
static registers_t *initial_frame(task_t *t)
{
    uint64_t top = ((uint64_t)(uintptr_t)t->kernel_stack + KERNEL_STACK_SZ) & ~15ull;
    registers_t *f = (registers_t *)(uintptr_t)(top - sizeof(registers_t));
    memset(f, 0, sizeof(*f));
    t->esp = (uint64_t)(uintptr_t)f;
    return f;
}

static void setup_task_stack(task_t *t, void (*entry)(void))
{
    registers_t *f = initial_frame(t);
    uint64_t top = (uint64_t)(uintptr_t)f + sizeof(registers_t);
    f->rip = (uint64_t)(uintptr_t)entry;
    f->cs = GDT_KERNEL_CS;
    f->rflags = 0x202;
    f->rsp = top - 8;                  /* as if called: (rsp + 8) is 16-byte aligned */
    f->ss = GDT_KERNEL_DS;
    f->ds = f->es = GDT_KERNEL_DS;
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
    idle_task->thread_group = 0;
    idle_task->tls_gdt_entry = -1;
    idle_task->in_use = 1;
    idle_task->state = TASK_STATE_READY;
    fpu_state_init(idle_task->fpu_state);
    idle_task->kernel_stack = malloc(KERNEL_STACK_SZ);
    memset(idle_task->kernel_stack, 0, KERNEL_STACK_SZ);
    strcpy(idle_task->name, "idle");
    setup_task_stack(idle_task, idle_entry);

    task_t *main_task = &tasks[1];
    fpu_state_init(main_task->fpu_state);
    main_task->pid = 1;
    main_task->thread_group = 1;
    main_task->tls_gdt_entry = -1;
    main_task->in_use = 1;
    main_task->state = TASK_STATE_RUNNING;
    asm volatile("mov %%rsp, %0" : "=r"(main_task->esp));
    strcpy(main_task->name, "main");

    idle_task->next = main_task;
    main_task->next = idle_task;

    current = main_task;
    /* Preserve any FP state used during boot as the main task's initial state. */
    fpu_save_state(main_task->fpu_state);
    fpu_restore_state(main_task->fpu_state);
    task_count_val = 2;
    next_pid = 2;

    uint32_t divisor = 11932;
    outb(0x43, 0x36);
    io_wait();
    outb(0x40, divisor & 0xFF);
    io_wait();
    outb(0x40, (divisor >> 8) & 0xFF);

    idt_set_gate(32, (uint64_t)(uintptr_t)timer_irq_stub, GDT_KERNEL_CS, 0x8E);

    serial_write("sched: PIT ~100Hz, main+idle tasks\n");
    terminal_writestring("[OK] Scheduler initialized\n");
}

static int task_spawn_internal(void (*entry)(void), const char *name, void *proc, uint64_t *pgdir, uint32_t uid, uint32_t gid)
{
    int slot = find_free_slot();
    if (slot < 0) return -1;

    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    fpu_state_init(t->fpu_state);
    t->proc = proc;
    t->pgdir = pgdir;
    if (proc && current) t->user_data = current->user_data;      /* output goes to the spawner's window */
    t->pid = next_pid++;
    t->thread_group = t->pid;
    t->tls_gdt_entry = -1;
    t->in_use = 1;
    t->state = TASK_STATE_READY;
    if (current) { t->uid = current->uid; t->gid = current->gid; }   /* children run as their parent */
    if (uid != 0xFFFFFFFFu) t->uid = uid;
    if (gid != 0xFFFFFFFFu) t->gid = gid;
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
    return task_spawn_internal(entry, name, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

int task_spawn_proc(void (*entry)(void), const char *name, void *proc, uint64_t *pgdir, uint32_t uid, uint32_t gid)
{
    return task_spawn_internal(entry, name, proc, pgdir, uid, gid);
}

int task_spawn_user_thread(const registers_t *regs, uint64_t child_stack,
                           void *proc, uint64_t *pgdir, uint32_t thread_group,
                           int tls_gdt_entry, uint32_t tls_base, uint32_t tls_limit,
                           uint32_t tls_flags, uint64_t clear_tid)
{
    if (!regs || !proc || !pgdir || !thread_group) return -1;
    if (!child_stack) child_stack = regs->useresp;
    if (child_stack < PAGING_USER_START || child_stack >= PAGING_USER_END ||
        !paging_user_range_ok(child_stack - 1, 1, 1)) return -1;

    int slot = find_free_slot();
    if (slot < 0) return -1;
    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->proc = proc;
    t->pgdir = pgdir;
    t->user_data = current ? current->user_data : 0;
    t->uid = current ? current->uid : 0;
    t->gid = current ? current->gid : 0;
    t->pid = next_pid++;
    t->thread_group = thread_group;
    t->clear_tid = clear_tid;
    t->tls_gdt_entry = tls_gdt_entry;
    t->tls_base = tls_base;
    t->tls_limit = tls_limit;
    t->tls_flags = tls_flags;
    t->user_thread = 1;
    t->in_use = 1;
    t->state = TASK_STATE_READY;
    t->kernel_stack = malloc(KERNEL_STACK_SZ);
    if (!t->kernel_stack) {
        t->in_use = 0;
        return -1;
    }
    memset(t->kernel_stack, 0, KERNEL_STACK_SZ);

    /* Inherit the parent thread's floating-point/vector registers exactly. */
    if (current) fpu_save_state(current->fpu_state);
    if (current) memcpy(t->fpu_state, current->fpu_state, sizeof(t->fpu_state));
    else fpu_state_init(t->fpu_state);

    uint16_t gs = tls_gdt_entry >= 0 ? (uint16_t)((tls_gdt_entry << 3) | 3) : (uint16_t)regs->gs;
    /* back to user mode at the instruction after the syscall, on the child's stack; the syscall's
     * return register is zero in the child, the other registers are the parent's (clone) */
    registers_t *f = initial_frame(t);
    *f = *regs;
    f->rax = 0;
    f->rsp = child_stack;
    f->rflags = (regs->rflags | 0x202u) & ~(0x3000ull | 0x4000ull | 0x20000ull);
    f->gs = gs;
    strncpy(t->name, "elf-thread", sizeof(t->name) - 1);

    task_t *last = current;
    while (last->next != current) last = last->next;
    t->next = current;
    last->next = t;
    task_count_val++;
    return (int)t->pid;
}

int task_kill_group(uint32_t thread_group, task_t *except)
{
    int killed = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!t->in_use || t == except || t->thread_group != thread_group ||
            t->state == TASK_STATE_ZOMBIE) continue;
        unlink_task(t);
        t->state = TASK_STATE_ZOMBIE;
        if (t->tls_gdt_entry >= 0) {
            gdt_tls_release(t->tls_gdt_entry);
            t->tls_gdt_entry = -1;
        }
        killed++;
    }
    return killed;
}

static void futex_wake_timeouts(void)
{
    uint64_t now = debugmon_uptime_ms64();
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!t->in_use || !t->futex_waiting || !t->futex_deadline_ms ||
            now < t->futex_deadline_ms) continue;
        t->futex_waiting = 0;
        t->futex_result = 1;
        t->state = TASK_STATE_READY;
    }
}

int task_futex_wait(uint64_t user_addr, uint32_t expected, uint64_t deadline_ms)
{
    uint32_t flags;
    flags = irq_save();
    if (!current || *(volatile uint32_t *)(uintptr_t)user_addr != expected) {
        irq_restore(flags);
        return -1;
    }
    current->futex_addr = user_addr;
    current->futex_deadline_ms = deadline_ms;
    current->futex_waiting = 1;
    current->futex_result = 0;
    current->state = TASK_STATE_SLEEPING;
    irq_restore(flags);
    task_yield();
    return current->futex_result;
}

int task_futex_wake(uint64_t user_addr, uint32_t count)
{
    if (!current || count == 0) return 0;
    uint32_t flags;
    int woke = 0;
    flags = irq_save();
    for (int i = 0; i < MAX_TASKS && (uint32_t)woke < count; i++) {
        task_t *t = &tasks[i];
        if (!t->in_use || !t->futex_waiting || t->futex_addr != user_addr ||
            t->pgdir != current->pgdir) continue;
        t->futex_waiting = 0;
        t->futex_deadline_ms = 0;
        t->futex_result = 0;
        t->state = TASK_STATE_READY;
        woke++;
    }
    irq_restore(flags);
    return woke;
}

#ifdef TOS_FPU_CONTEXT_SELFTEST
static void fpu_context_worker(uint32_t expected, const char *label)
{
    /* These self-test functions are built with the kernel's -mno-sse flags;
     * keep XMM0 out of the compiler's register model and use it only here. */
    asm volatile("movd %0, %%xmm0" : : "r"(expected) : "memory");
    for (uint32_t i = 0; i < 2048; i++) {
        uint32_t actual;
        asm volatile("movd %%xmm0, %0" : "=r"(actual));
        if (actual != expected) {
            serial_write("[FPU-SELFTEST] FAIL: task XMM state was not restored\n");
            task_exit();
        }
        task_yield();
    }
    serial_write(label);
    task_exit();
}

static void fpu_context_worker_a(void)
{
    fpu_context_worker(0x13579BDFu, "[FPU-SELFTEST] PASS worker A\n");
}

static void fpu_context_worker_b(void)
{
    fpu_context_worker(0x2468ACE0u, "[FPU-SELFTEST] PASS worker B\n");
}

void scheduler_fpu_context_selftest(void)
{
    if (!fpu_has_sse2()) {
        serial_write("[FPU-SELFTEST] SKIP: SSE2 unavailable\n");
        return;
    }
    if (task_spawn(fpu_context_worker_a, "fpu-test-a") < 0 ||
        task_spawn(fpu_context_worker_b, "fpu-test-b") < 0) {
        serial_write("[FPU-SELFTEST] FAIL: could not spawn workers\n");
        return;
    }
    serial_write("[FPU-SELFTEST] two SSE2 workers started\n");
}
#endif

task_t *task_by_pid(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].in_use && tasks[i].pid == pid) return &tasks[i];
    return 0;
}

uint64_t timer_handler(uint64_t esp)
{
    /* The IRQ stub and scheduler run with integer-only kernel flags; capture
     * the interrupted task before changing its saved stack or selecting next. */
    if (current) fpu_save_state(current->fpu_state);
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
    futex_wake_timeouts();
    current->cpu_ticks++;
    current->esp = esp;

    if (current->no_preempt) {
        /* This task is inside a wait loop that must not be switched
         * away from mid-stride (e.g. a blocking ring3 keyboard read
         * that only masks IRQ0 at the PIC, which cannot cancel an
         * IRQ0 already in flight when the mask was applied). Still
         * counting ticks/cpu_ticks above keeps wall-clock timing and
         * accounting correct; we just skip the actual task switch. */
        fpu_restore_state(current->fpu_state);
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
        uint64_t *want = current->pgdir ? current->pgdir : paging_kernel_dir();
        if (want && paging_current_dir() != want) paging_switch(want);
        if (current->kernel_stack) tss_set_kernel_stack(((uint64_t)(uintptr_t)current->kernel_stack + KERNEL_STACK_SZ) & ~15ull);
    }
    fpu_restore_state(current->fpu_state);
    /* FS base: a 64-bit program's thread pointer */
    {
        uint64_t fs = current->fs_base;
        asm volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)fs), "d"((uint32_t)(fs >> 32)));
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
    if (current && current->tls_gdt_entry >= 0) {
        gdt_tls_release(current->tls_gdt_entry);
        current->tls_gdt_entry = -1;
    }
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
            if (tasks[i].tls_gdt_entry >= 0) {
                gdt_tls_release(tasks[i].tls_gdt_entry);
                tasks[i].tls_gdt_entry = -1;
            }
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
