#ifndef PTRACE_H
#define PTRACE_H

#include <stdint.h>
#include "proc.h"
#include "isr.h"

/* ptrace(2) for 64-bit programs: a tracer (a debugger, strace) controls a tracee that stops at breakpoints,
 * single steps, system calls and signals, and reads and writes its registers and memory */
int  lx_ptrace(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data, registers_t *regs);

void ptrace_stop(proc_t *p, registers_t *regs, int sig, int event, int code);   /* the caller (the tracee) waits here */
int  ptrace_trap(registers_t *regs);                  /* int3 / single step of a traced program: 1 = handled */
int  ptrace_fault_stop(registers_t *regs, int sig);   /* a fault of a traced program: 1 = the tracer lets it retry */
void ptrace_syscall_stop(proc_t *p, registers_t *r, int phase);   /* 1 = entry, 2 = exit, when stopping at system calls */
void ptrace_exec_stop(proc_t *p, uint64_t *entry, uint64_t *sp);   /* a program that has just been exec'ed, before its first instruction */
int  ptrace_signal_stop(proc_t *p, registers_t *regs, int sig);   /* a signal about to be delivered: the one to deliver (0 = none) */
int  ptrace_intercept(proc_t *q, int sig);            /* proc_signal: 1 = the tracer sees it first */
int  ptrace_async_stop(proc_t *p, registers_t *regs); /* a queued signal stops the tracee now (at a system call's end): 1 = stopped */
void ptrace_fork_event(proc_t *p, registers_t *regs, uint32_t child, int is_vfork);
int  ptrace_read_mem(proc_t *p, uint64_t va, void *dst, uint32_t n);   /* another process's memory (0 = read) */
void ptrace_proc_gone(proc_t *p);                     /* a process ended: its tracees are let go */

#endif
