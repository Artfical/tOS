#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include "vfs.h"

/*
 * User processes.
 *
 * A `.t` program runs as its own scheduler task in its own address space: only the kernel half of the page
 * directory is shared (and not user-accessible), the code, heap and stack pages belong to the process and are
 * freed when it ends. It runs with the credentials of whoever started it, so every file it opens goes through the
 * same permission checks as the shell's. A fault in ring 3 ends that process, not the system.
 */

#define PROC_MAX     16
#define PROC_MAX_FDS 24
#define PROC_EXEC_MAX_ARGS 32
#define PROC_EXEC_MAX_ENV  32
#define PROC_EXEC_ARG_MAX  1024
#define PROC_BRK_BASE  0x88000000u      /* heap grows up from here */
#define PROC_BRK_LIMIT 0x90000000u
#define PROC_MAX_VM_REGIONS 256
#define PROC_FD_DIRECTORY (-2)
#define PROC_FD_EPOLL     (-4)
#define PROC_FD_URANDOM   (-5)          /* /dev/urandom and /dev/random */
#define PROC_FD_NULL      (-6)          /* /dev/null */
#define PROC_MAX_PIPES 8
#define PROC_PIPE_CAPACITY 256
#define PROC_FD_PIPE_READ_BASE  (-16)
#define PROC_FD_PIPE_WRITE_BASE (-32)
#define PROC_MAX_EVENTFDS 8
#define PROC_FD_EVENTFD_BASE    (-48)   /* eventfd i is -48 - i */
#define PROC_FD_SOCK_BASE       (-64)   /* socket i (kernel/net/lsock.c) is -64 - i, 32 of them */

typedef struct {
    uint8_t used;
    uint8_t semaphore;                  /* EFD_SEMAPHORE: each read takes one */
    uint64_t count;
} proc_eventfd_t;

typedef struct {
    uint32_t start;
    uint32_t end;       /* exclusive */
    int used;
} proc_vm_region_t;

typedef struct {
    uint16_t head, tail, count;
    uint8_t used;
    uint16_t read_refs;
    uint16_t write_refs;
    uint8_t data[PROC_PIPE_CAPACITY];
} proc_pipe_t;

/* one descriptor an epoll instance watches (epoll_ctl), level-triggered */
#define PROC_MAX_EPOLL_WATCH 32
typedef struct {
    int epfd, fd;           /* user descriptor numbers; epfd 0 = a free entry */
    uint32_t events;
    uint32_t data_lo, data_hi;
} proc_epoll_watch_t;

typedef struct proc {
    int used;
    int done;                           /* finished: exit_code is valid until someone waits for it */
    uint32_t pid, ppid;
    uint64_t *pgdir;
    uint32_t brk;
    uint32_t brk_base;
    uint32_t entry;
    uint32_t user_stack;
    int linux_abi;                     /* ELF32 processes use the Linux i386 syscall ABI */
    int exit_code;
    int gfx;                            /* switched the display into graphics mode */
    void *win, *win_prev;               /* a window of its own (SYS_WIN_OPEN), and the one it drew to before */
    int fd[PROC_MAX_FDS];               /* VFS descriptors (-1 = free); 0..2 are the console and not stored here */
    uint32_t fd_flags[PROC_MAX_FDS];
    uint32_t fd_open_flags[PROC_MAX_FDS];
    char fdpath[PROC_MAX_FDS][VFS_NAME_LEN];
    uint32_t fd_dir_offset[PROC_MAX_FDS];
    proc_pipe_t pipes[PROC_MAX_PIPES];
    proc_epoll_watch_t epoll_watch[PROC_MAX_EPOLL_WATCH];
    proc_eventfd_t eventfds[PROC_MAX_EVENTFDS];
    uint32_t umask;                     /* Linux umask, 022 until the program sets one */
    int umask_set;
    proc_vm_region_t vm_regions[PROC_MAX_VM_REGIONS];
    char name[32];
} proc_t;

/* loads the .t file at `path` into a new address space and starts it; returns the pid or a negative error */
int  proc_spawn(const char *path);
int  proc_spawn_argv(const char *path, const char *const argv[], const char *const envp[]);
int  proc_execve_argv(const char *path, const char *const argv[], const char *const envp[]);
proc_t *proc_current(void);
proc_t *proc_by_pid(uint32_t pid);
void proc_exit(int code) __attribute__((noreturn));
void proc_thread_exit(int code) __attribute__((noreturn));
/* ends another process (the caller checks permission); 0 = done */
int  proc_kill(uint32_t pid, int code);
/* blocks until the process has ended; returns its exit code (or -1 for an unknown pid) and releases it */
int  proc_wait(uint32_t pid);
void proc_fd_close(proc_t *p, int slot);
int  proc_running_count(void);

#endif
