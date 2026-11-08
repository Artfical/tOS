#include "ptrace.h"
#include "scheduler.h"
#include "paging.h"
#include "memory.h"
#include "string.h"
#include "errno.h"
#include "linux_abi.h"
#include "io.h"
#include "serial.h"
#include "auth.h"

/* ptrace(2) for 64-bit programs.
 *
 * The tracee stops inside the kernel (in the exception handler, or in its own system call), with the registers it
 * was interrupted with on its kernel stack: p->pt_regs. The tracer's wait() reports the stop; the tracer reads and
 * changes registers and memory, then lets it go on (continue, single step with the trap flag, or until the next
 * system call). Not done: tracing several threads of one program apart, following fork and clone, hardware debug
 * registers. */

#define PT_TRACEME      0
#define PT_PEEKTEXT     1
#define PT_PEEKDATA     2
#define PT_PEEKUSER     3
#define PT_POKETEXT     4
#define PT_POKEDATA     5
#define PT_POKEUSER     6
#define PT_CONT         7
#define PT_KILL         8
#define PT_SINGLESTEP   9
#define PT_GETREGS      12
#define PT_SETREGS      13
#define PT_GETFPREGS    14
#define PT_SETFPREGS    15
#define PT_ATTACH       16
#define PT_DETACH       17
#define PT_SYSCALL      24
#define PT_SETOPTIONS   0x4200
#define PT_GETEVENTMSG  0x4201
#define PT_GETSIGINFO   0x4202
#define PT_SETSIGINFO   0x4203
#define PT_GETREGSET    0x4204
#define PT_SETREGSET    0x4205
#define PT_SEIZE        0x4206
#define PT_INTERRUPT    0x4207

#define OPT_SYSGOOD  0x01
#define OPT_EXEC     0x10
#define OPT_EXITKILL 0x100000

#define EVENT_EXEC 4

typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax,
             rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
} user_regs_t;

#define USER_REGS_SIZE   ((uint32_t)sizeof(user_regs_t))
#define DEBUGREG_OFFSET  848u

#ifdef PTRACE_DEBUG
static void pt_log(const char *what, uint32_t a, uint32_t b, uint32_t c)
{
    char h[12];
    serial_write("PT "); serial_write(what);
    sprintf_u32(h, a); serial_write(" "); serial_write(h);
    sprintf_u32(h, b); serial_write(" "); serial_write(h);
    sprintf_u32(h, c); serial_write(" "); serial_write(h);
    serial_write("\n");
}
#define PT_LOG(w, a, b, c) pt_log(w, (uint32_t)(a), (uint32_t)(b), (uint32_t)(c))
#else
#define PT_LOG(w, a, b, c) do { } while (0)
#endif

static void pt_wake(proc_t *p)
{
    uint32_t fl = irq_save();
    task_t *t = task_by_pid(p->pt_tid ? p->pt_tid : p->pid);
    if (t && t->state == TASK_STATE_SLEEPING && !t->futex_waiting) t->state = TASK_STATE_READY;
    irq_restore(fl);
}

/* the tracee waits here until the tracer lets it go */
static void pt_block(proc_t *p)
{
    task_t *t = task_current();
    for (;;) {
        uint32_t fl = irq_save();
        if (!p->pt_stop || p->done || !p->tracer) { irq_restore(fl); break; }
        t->state = TASK_STATE_SLEEPING;
        irq_restore(fl);
        task_yield();
    }
}

void ptrace_stop(proc_t *p, registers_t *regs, int sig, int event, int code)
{
    task_t *t = task_current();
    p->pt_regs = regs;
    p->pt_sig = sig;
    p->pt_event = event;
    p->pt_code = code;
    p->pt_tid = t ? t->pid : p->pid;
    p->pt_resume_sig = 0;
    p->pt_report = 1;
    p->pt_stop = 1;
    {                                                       /* the tracer is told with SIGCHLD, as on Linux (a debugger waits for it) */
        proc_t *tr = proc_by_pid(p->tracer);
        if (tr && !tr->done) proc_signal(tr, 17);
    }
    PT_LOG("stop pid/sig/event", p->pid, sig, event);
    pt_block(p);
    PT_LOG("go on pid/mode/resumesig", p->pid, p->pt_mode, p->pt_resume_sig);
    p->pt_regs = 0;
    p->pt_stop = 0;
    if (regs && (regs->cs & 3) == 3) {                       /* on the way the tracer chose */
        if (p->pt_mode == 1) regs->rflags |= 0x100ull;
        else regs->rflags &= ~0x100ull;
    }
}

/* ---- the tracee's memory ---- */

int ptrace_read_mem(proc_t *p, uint64_t va, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        if (va >= PAGING_USER_END || !lx_fault_in(p, va & ~0xFFFull, 0)) return -1;
        uint64_t phys = paging_virt_to_phys(p->pgdir, va);
        if (!phys) return -1;
        uint32_t chunk = (uint32_t)(PAGE_SIZE - (va & 0xFFF));
        if (chunk > n) chunk = n;
        memcpy(d, (const uint8_t *)P2V(phys), chunk);
        d += chunk; va += chunk; n -= chunk;
    }
    return 0;
}

static int pt_write(proc_t *p, uint64_t va, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint64_t page = va & ~0xFFFull;
        if (va >= PAGING_USER_END || !lx_fault_in(p, page, 0)) return -1;
        paging_cow_break(p->pgdir, page);                     /* a page shared since a fork is the tracee's own before it changes */
        uint64_t phys = paging_virt_to_phys(p->pgdir, va);
        if (!phys) return -1;
        uint32_t chunk = (uint32_t)(PAGE_SIZE - (va & 0xFFF));
        if (chunk > n) chunk = n;
        memcpy((uint8_t *)P2V(phys), s, chunk);               /* through the kernel's mapping: read-only code (breakpoints) too */
        s += chunk; va += chunk; n -= chunk;
    }
    return 0;
}

/* ---- registers ---- */

static void get_regs(proc_t *p, user_regs_t *u)
{
    registers_t *r = (registers_t *)p->pt_regs;
    task_t *t = task_by_pid(p->pt_tid ? p->pt_tid : p->pid);
    memset(u, 0, sizeof(*u));
    u->r15 = r->r15; u->r14 = r->r14; u->r13 = r->r13; u->r12 = r->r12;
    u->r11 = r->r11; u->r10 = r->r10; u->r9 = r->r9; u->r8 = r->r8;
    u->rbp = r->rbp; u->rbx = r->rbx; u->rcx = r->rcx; u->rdx = r->rdx; u->rsi = r->rsi; u->rdi = r->rdi;
    if (p->pt_in_syscall == 1) { u->rax = (uint64_t)-38; u->orig_rax = r->rax; }              /* -ENOSYS until it has run */
    else if (p->pt_in_syscall == 2) { u->rax = r->rax; u->orig_rax = (uint64_t)(int64_t)p->pt_syscall_nr; }
    else { u->rax = r->rax; u->orig_rax = (uint64_t)-1; }
    u->rip = r->rip; u->eflags = r->rflags; u->rsp = r->rsp;
    u->cs = (r->cs & 0xFFF8u) == 0x28u ? 0x33 : r->cs;        /* the selectors of Linux: a debugger tells 64-bit from 32-bit by CS (0x33) */
    u->ss = (r->ss & 0xFFF8u) == 0x20u ? 0x2B : r->ss;
    u->fs_base = t ? t->fs_base : 0;
    u->ds = r->ds; u->es = r->es; u->fs = r->fs; u->gs = r->gs;
}

static int set_regs(proc_t *p, const user_regs_t *u)
{
    registers_t *r = (registers_t *)p->pt_regs;
    task_t *t = task_by_pid(p->pt_tid ? p->pt_tid : p->pid);
    if (u->rip >= PAGING_USER_END || u->rsp >= PAGING_USER_END) return -EIO;
    r->r15 = u->r15; r->r14 = u->r14; r->r13 = u->r13; r->r12 = u->r12;
    r->r11 = u->r11; r->r10 = u->r10; r->r9 = u->r9; r->r8 = u->r8;
    r->rbp = u->rbp; r->rbx = u->rbx; r->rcx = u->rcx; r->rdx = u->rdx; r->rsi = u->rsi; r->rdi = u->rdi;
    if (p->pt_in_syscall == 1) r->rax = u->orig_rax;                  /* the number of the system call to run */
    else r->rax = u->rax;
    r->rip = u->rip; r->rsp = u->rsp;
    r->rflags = (r->rflags & ~0x40DD5ull) | (u->eflags & 0x40DD5ull) | 0x200ull;     /* the flags a program may change */
    if (t) t->fs_base = u->fs_base < PAGING_USER_END ? u->fs_base : t->fs_base;
    return 0;
}

static uint64_t *reg_slot(user_regs_t *u, uint64_t off)
{
    if (off >= USER_REGS_SIZE || (off & 7)) return 0;
    return &((uint64_t *)u)[off / 8];
}

/* ---- the requests ---- */

static int may_trace(proc_t *me, proc_t *tp)
{
    if (!tp || tp == me || tp->done || tp->tracer || tp->linux_abi != 2) return 0;
    uint32_t mu = task_get_uid(me->pid), tu = task_get_uid(tp->pid);
    return mu == 0 || mu == tu;
}

static uint64_t resume(proc_t *tp, int mode, uint64_t sig)
{
    if (sig > 64) return (uint64_t)-EIO;
    tp->pt_mode = mode;
    tp->pt_resume_sig = (int)sig;
    tp->pt_stop = 0;
    tp->pt_report = 0;
    PT_LOG("resume pid/mode/sig", tp->pid, mode, sig);
    pt_wake(tp);
    return 0;
}

int ptrace_trap(registers_t *regs);

uint64_t ptrace_request(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data, registers_t *regs);

int lx_ptrace(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data, registers_t *regs)
{
    return (int)(int64_t)ptrace_request(req, pid, addr, data, regs);
}

uint64_t ptrace_request(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data, registers_t *regs)
{
    (void)regs;
    proc_t *me = proc_current();
    if (!me) return (uint64_t)-ESRCH;
    if (req == PT_TRACEME) {
        proc_t *par = proc_by_pid(me->ppid);
        if (me->tracer || !par || me->linux_abi != 2) return (uint64_t)-EPERM;
        me->tracer = par->pid;
        me->pt_mode = 0; me->pt_options = 0; me->pt_async_mask = 0; me->pt_deliver_mask = 0; me->pt_exit_reported = 0;
        return 0;
    }
    if (req == PT_SEIZE || req == PT_INTERRUPT) return (uint64_t)-EIO;        /* not done: debuggers fall back to PTRACE_ATTACH and PTRACE_TRACEME */
    proc_t *tp = proc_by_pid((uint32_t)pid);
    if (!tp || !tp->used) return (uint64_t)-ESRCH;
    if (req == PT_ATTACH) {
        if (!may_trace(me, tp)) return (uint64_t)-EPERM;
        tp->tracer = me->pid;
        tp->pt_mode = 0; tp->pt_options = req == PT_SEIZE ? (int)data : 0; tp->pt_async_mask = 0; tp->pt_deliver_mask = 0;
        tp->pt_exit_reported = 0; tp->pt_stop = 0;
        if (req == PT_ATTACH) tp->pt_async_mask |= 1ull << (19 - 1);     /* SIGSTOP: it stops, and wait() says so */
        return 0;
    }
    if (tp->tracer != me->pid) return (uint64_t)-ESRCH;
    if (req == PT_INTERRUPT) {
        tp->pt_async_mask |= 1ull << (5 - 1);
        return 0;
    }
    if (req == PT_KILL) {
        tp->pt_stop = 0;
        pt_wake(tp);
        proc_signal(tp, 9);
        return 0;
    }
    if (!tp->pt_stop || !tp->pt_regs) return (uint64_t)-ESRCH;            /* the rest needs a stopped tracee */
    switch (req) {
    case PT_PEEKTEXT: case PT_PEEKDATA: {
        uint64_t w;
        if (ptrace_read_mem(tp, addr, &w, 8) != 0) return (uint64_t)-EIO;
        if (!lx_user_wr_ok(data, 8)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)data, &w, 8);
        return 0;
    }
    case PT_POKETEXT: case PT_POKEDATA:
        return pt_write(tp, addr, &data, 8) == 0 ? 0 : (uint64_t)-EIO;
    case PT_PEEKUSER: {
        uint64_t w = 0;
        if (addr < USER_REGS_SIZE) {
            user_regs_t u; get_regs(tp, &u);
            uint64_t *s = reg_slot(&u, addr);
            if (!s) return (uint64_t)-EIO;
            w = *s;
        } else if (addr < DEBUGREG_OFFSET || addr >= DEBUGREG_OFFSET + 64) return (uint64_t)-EIO;
        if (!lx_user_wr_ok(data, 8)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)data, &w, 8);
        return 0;
    }
    case PT_POKEUSER: {
        if (addr >= DEBUGREG_OFFSET && addr < DEBUGREG_OFFSET + 64) return data == 0 ? 0 : (uint64_t)-EIO;   /* no hardware breakpoints */
        user_regs_t u; get_regs(tp, &u);
        uint64_t *s = reg_slot(&u, addr);
        if (!s) return (uint64_t)-EIO;
        *s = data;
        return (uint64_t)(int64_t)set_regs(tp, &u);
    }
    case PT_GETREGS: {
        user_regs_t u; get_regs(tp, &u);
        if (!lx_user_wr_ok(data, USER_REGS_SIZE)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)data, &u, USER_REGS_SIZE);
        return 0;
    }
    case PT_SETREGS: {
        user_regs_t u;
        if (!lx_user_range_ok(data, USER_REGS_SIZE)) return (uint64_t)-EFAULT;
        memcpy(&u, (const void *)(uintptr_t)data, USER_REGS_SIZE);
        return (uint64_t)(int64_t)set_regs(tp, &u);
    }
    case PT_GETFPREGS: {
        task_t *t = task_by_pid(tp->pt_tid ? tp->pt_tid : tp->pid);
        if (!t || !lx_user_wr_ok(data, 512)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)data, t->fpu_state, 512);
        return 0;
    }
    case PT_SETFPREGS: {
        task_t *t = task_by_pid(tp->pt_tid ? tp->pt_tid : tp->pid);
        if (!t || !lx_user_range_ok(data, 512)) return (uint64_t)-EFAULT;
        memcpy(t->fpu_state, (const void *)(uintptr_t)data, 512);
        uint32_t mxcsr; memcpy(&mxcsr, t->fpu_state + 24, 4); mxcsr &= 0xFFFFu; memcpy(t->fpu_state + 24, &mxcsr, 4);
        return 0;
    }
    case PT_GETREGSET: case PT_SETREGSET: {
        uint64_t iov[2];
        if (!lx_user_range_ok(data, 16)) return (uint64_t)-EFAULT;
        memcpy(iov, (const void *)(uintptr_t)data, 16);
        if (addr == 1) {                                                   /* NT_PRSTATUS: the general registers */
            user_regs_t u;
            uint32_t n = iov[1] < USER_REGS_SIZE ? (uint32_t)iov[1] : USER_REGS_SIZE;
            if (req == PT_GETREGSET) {
                get_regs(tp, &u);
                if (!lx_user_wr_ok(iov[0], n)) return (uint64_t)-EFAULT;
                memcpy((void *)(uintptr_t)iov[0], &u, n);
                iov[1] = n;
                memcpy((void *)(uintptr_t)data, iov, 16);
                return 0;
            }
            get_regs(tp, &u);
            if (!lx_user_range_ok(iov[0], n)) return (uint64_t)-EFAULT;
            memcpy(&u, (const void *)(uintptr_t)iov[0], n);
            return (uint64_t)(int64_t)set_regs(tp, &u);
        }
        if (addr == 2) {                                                   /* NT_PRFPREG: the FPU/SSE registers */
            task_t *t = task_by_pid(tp->pt_tid ? tp->pt_tid : tp->pid);
            uint32_t n = iov[1] < 512 ? (uint32_t)iov[1] : 512;
            if (!t) return (uint64_t)-ESRCH;
            if (req == PT_GETREGSET) {
                if (!lx_user_wr_ok(iov[0], n)) return (uint64_t)-EFAULT;
                memcpy((void *)(uintptr_t)iov[0], t->fpu_state, n);
                iov[1] = n;
                memcpy((void *)(uintptr_t)data, iov, 16);
                return 0;
            }
            if (!lx_user_range_ok(iov[0], n)) return (uint64_t)-EFAULT;
            memcpy(t->fpu_state, (const void *)(uintptr_t)iov[0], n);
            return 0;
        }
        return (uint64_t)-EINVAL;
    }
    case PT_CONT:       return resume(tp, 0, data);
    case PT_SINGLESTEP: return resume(tp, 1, data);
    case PT_SYSCALL:    return resume(tp, 2, data);
    case PT_DETACH: {
        tp->tracer = 0; tp->pt_async_mask = 0; tp->pt_mode = 0; tp->pt_in_syscall = 0;
        if (data && data <= 64) { tp->pt_deliver_mask |= 1ull << (data - 1); tp->pt_stop = 0; pt_wake(tp); proc_signal(tp, (int)data); }
        tp->pt_stop = 0; tp->pt_report = 0;
        pt_wake(tp);
        return 0;
    }
    case PT_SETOPTIONS:
        tp->pt_options = (int)data;
        return 0;
    case PT_GETEVENTMSG:
        if (!lx_user_wr_ok(data, 8)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)data, &tp->pt_eventmsg, 8);
        return 0;
    case PT_GETSIGINFO: {
        int32_t si[32];
        if (!lx_user_wr_ok(data, 128)) return (uint64_t)-EFAULT;
        memset(si, 0, sizeof(si));
        si[0] = tp->pt_sig & 0x7F;
        si[2] = tp->pt_code;
        memcpy(&si[4], &tp->pt_addr, 8);
        memcpy((void *)(uintptr_t)data, si, 128);
        return 0;
    }
    case PT_SETSIGINFO:
        return 0;
    default:
        return (uint64_t)-EIO;
    }
}

/* ---- stops, from the tracee's side ---- */

void ptrace_syscall_stop(proc_t *p, registers_t *r, int phase)
{
    p->pt_in_syscall = phase;
    if (phase == 1) p->pt_syscall_nr = (int)r->rax;
    ptrace_stop(p, r, (p->pt_options & OPT_SYSGOOD) ? (5 | 0x80) : 5, 0, 0x80);
    p->pt_in_syscall = 0;
}

void ptrace_exec_stop(proc_t *p, uint64_t *entry, uint64_t *sp)
{
    registers_t f;
    memset(&f, 0, sizeof(f));
    f.rip = *entry; f.rsp = *sp; f.cs = 0x2B; f.ss = 0x23; f.rflags = 0x202; f.int_no = 0x81;
    /* as on Linux: with the exec event option the event comes first, then the end of the execve call (when tracing
     * system calls); without it the end of the call, then the SIGTRAP every successful exec sends */
    if (p->pt_options & OPT_EXEC) {
        ptrace_stop(p, &f, 5, EVENT_EXEC, 0x80);
        if (p->pt_mode == 2) { f.rax = 0; ptrace_syscall_stop(p, &f, 2); }
    } else {
        if (p->pt_mode == 2) { f.rax = 0; ptrace_syscall_stop(p, &f, 2); }
        ptrace_stop(p, &f, 5, 0, 0x80);
    }
    *entry = f.rip; *sp = f.rsp;
    extern uint64_t user_entry_extra_flags;
    user_entry_extra_flags = p->pt_mode == 1 ? 0x100ull : 0;         /* PTRACE_SINGLESTEP from the exec stop: the first instruction traps */
}

int ptrace_trap(registers_t *regs)
{
    proc_t *p = proc_current();
    if (!p || !p->tracer || (regs->cs & 3) != 3) return 0;
    if (regs->int_no == 3) {                                       /* int3: the breakpoint of a debugger */
        ptrace_stop(p, regs, 5, 0, 0x80);
        return 1;
    }
    regs->rflags &= ~0x100ull;                                     /* the step is done */
    if (p->pt_mode == 1) { ptrace_stop(p, regs, 5, 0, 2); return 1; }
    if (p->pt_async_mask) { ptrace_async_stop(p, regs); return 1; }
    return 1;                                                      /* a step nobody asked for */
}

int ptrace_fault_stop(registers_t *regs, int sig)
{
    proc_t *p = proc_current();
    if (!p || !p->tracer || (regs->cs & 3) != 3 || p->linux_abi != 2) return 0;
    uint64_t cr2 = 0;
    if (regs->int_no == 14) asm volatile("mov %%cr2, %0" : "=r"(cr2));
    p->pt_addr = cr2;
    ptrace_stop(p, regs, sig, 0, regs->int_no == 14 ? 1 : 0x80);
    return p->pt_resume_sig == 0;                                  /* let through: the program ends; else it tries again */
}

int ptrace_signal_stop(proc_t *p, registers_t *regs, int sig)
{
    uint64_t bit = 1ull << (sig - 1);
    if (p->pt_deliver_mask & bit) { p->pt_deliver_mask &= ~bit; return sig; }
    ptrace_stop(p, regs, sig, 0, 0);
    return p->pt_resume_sig;
}

int ptrace_intercept(proc_t *q, int sig)
{
    if (!q->tracer || sig == 9 || sig == 18 || sig < 1 || sig > 64) return 0;     /* SIGCONT's effect (going on after a stop) is not held back */
    uint64_t bit = 1ull << (sig - 1);
    if (q->pt_deliver_mask & bit) { q->pt_deliver_mask &= ~bit; return 0; }
    if (q->linux_abi == 2 && q->sig_handler[sig] > 1) return 0;    /* its handler: deliver_signal stops for it */
    q->pt_async_mask |= bit;
    return 1;
}

int ptrace_async_stop(proc_t *p, registers_t *regs)
{
    if (!p->tracer || !p->pt_async_mask) return 0;
    int sig = 1;
    while (!(p->pt_async_mask & (1ull << (sig - 1)))) sig++;
    p->pt_async_mask &= ~(1ull << (sig - 1));
    ptrace_stop(p, regs, sig, 0, 0);
    int ns = p->pt_resume_sig;
    if (ns > 0 && ns <= 64) {                                      /* the tracer lets it through: as if it had just been sent */
        p->pt_deliver_mask |= 1ull << (ns - 1);
        proc_signal(p, ns);
    }
    return 1;
}

/* a traced program made a child (fork or vfork): the tracer is told, with the child's pid (PTRACE_GETEVENTMSG) */
void ptrace_fork_event(proc_t *p, registers_t *regs, uint32_t child, int is_vfork)
{
    int ev = is_vfork ? 2 : 1;
    if (!p->tracer || !(p->pt_options & (is_vfork ? 0x4 : 0x2))) return;
    p->pt_eventmsg = child;
    ptrace_stop(p, regs, 5, ev, 0x80);
}

/* a process ended: the programs it traced are let go (or end with it) */
void ptrace_proc_gone(proc_t *p)
{
    if (p->tracer && p->tracer != p->ppid) {                       /* the tracer hears of the end too (its parent hears anyway) */
        proc_t *tr = proc_by_pid(p->tracer);
        if (tr && !tr->done) proc_signal(tr, 17);
    }
    extern proc_t *proc_table_at(int i);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *q = proc_table_at(i);
        if (!q || !q->used || q->tracer != p->pid || q == p) continue;
        q->tracer = 0; q->pt_async_mask = 0; q->pt_mode = 0;
        if (q->pt_options & OPT_EXITKILL) { q->pt_stop = 0; pt_wake(q); proc_signal(q, 9); continue; }
        q->pt_stop = 0;
        pt_wake(q);
    }
}
