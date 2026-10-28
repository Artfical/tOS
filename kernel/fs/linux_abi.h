#ifndef LINUX_ABI_H
#define LINUX_ABI_H

/* What syscall.c (the Linux syscalls, i386 numbering) shares with linux64.c (x86-64). */
#include <stdint.h>
#include "isr.h"

/* i386 struct stat64, which the shared code fills; linux64.c turns it into x86-64's struct stat */
typedef struct __attribute__((packed)) {
    uint64_t st_dev;
    uint32_t pad1;
    uint32_t ino32;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t pad2;
    int64_t st_size;
    int32_t st_blksize;
    int64_t st_blocks;
    int32_t atime_sec;
    uint32_t atime_nsec;
    int32_t mtime_sec;
    uint32_t mtime_nsec;
    int32_t ctime_sec;
    uint32_t ctime_nsec;
    uint64_t st_ino;
} linux_stat64_t;

uint64_t lx_dispatch32(uint32_t number, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                       uint64_t e, uint64_t f, registers_t *regs);
int lx_stat_at(int dirfd, uint64_t path_ptr, uint32_t flags, linux_stat64_t *st);
int lx_fd_stat(int fd, linux_stat64_t *st);
uint32_t lx_clone(registers_t *regs, uint32_t flags, uint64_t child_stack,
                  uint64_t parent_tid, uint64_t tls, uint64_t child_tid);
int lx_user_range_ok(uint64_t p, uint64_t len);
int lx_user_wr_ok(uint64_t p, uint64_t len);
int lx_user_cstr_ok(uint64_t p);
int lx_execve(uint64_t path, uint64_t argv, uint64_t envp);
uint32_t lx_wait4(int pid, uint64_t status, uint32_t options, uint64_t rusage);
int lx_tls_clone(uint32_t base, uint32_t limit, uint32_t flags);
int lx_clock_now(uint32_t clock_id, int64_t *sec, int32_t *nsec);
void linux64_prefault(uint64_t addr, uint64_t len, int write);
int linux64_is_elf64(const char *path);
struct proc;
int linux64_load(struct proc *p, const char *path, int argc, const char *const argv[],
                 int envc, const char *const envp[], uint64_t *entry_out, uint64_t *sp_out);
void linux64_mm_free(struct proc *p);
int  linux64_mm_clone(struct proc *child, struct proc *parent);   /* fork: a copy of the map */

void linux64_init(void);                      /* the syscall instruction's MSRs */
void linux64_syscall(registers_t *regs);      /* from syscall_entry64 */
/* a page fault in a 64-bit process: 1 when it was a page it may have (now mapped) */
int linux64_page_fault(uint64_t addr, uint64_t err);

#endif
