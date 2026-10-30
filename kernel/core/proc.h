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

#define PROC_MAX     32
#define PROC_MAX_FDS 64
#define PROC_EXEC_MAX_ARGS 1024
#define PROC_EXEC_MAX_ENV  1024
#define PROC_EXEC_ARG_MAX  65536             /* one string */
#define PROC_EXEC_BYTES    (256u * 1024u)    /* all of argv and envp */
#define PROC_BRK_BASE  0x88000000u      /* heap grows up from here */
#define PROC_BRK_LIMIT 0x90000000u
#define PROC_MAX_VM_REGIONS 256
#define PROC_FD_DIRECTORY (-2)
#define PROC_FD_EPOLL     (-4)
#define PROC_FD_URANDOM   (-5)          /* /dev/urandom and /dev/random */
#define PROC_FD_NULL      (-6)          /* /dev/null */
#define PROC_FD_CONSOLE(i) (-7 - (i))   /* a dup() of the console's 0, 1 or 2 (-7..-9) */
#define PROC_FD_IS_CONSOLE(v) ((v) <= -7 && (v) >= -9)
/* Pipes are shared between processes (fork, then the child's stdio): one table for the system,
 * a descriptor holds an end, PROC_FD_PIPE_*_BASE - index. */
#define PROC_MAX_PIPES 128
#define PROC_PIPE_CAPACITY 65536
#define PROC_FD_PIPE_READ_BASE  (-1000)
#define PROC_FD_PIPE_WRITE_BASE (-2000)
/* a socketpair end: reads pipe r, writes pipe w (the other end the opposite) */
#define PROC_FD_SPAIR_BASE (-10000)
#define PROC_FD_IS_SPAIR(v) ((v) <= PROC_FD_SPAIR_BASE && (v) > PROC_FD_SPAIR_BASE - PROC_MAX_PIPES * PROC_MAX_PIPES)
#define PROC_FD_SPAIR(r, w) (PROC_FD_SPAIR_BASE - ((r) * PROC_MAX_PIPES + (w)))
#define PROC_FD_SPAIR_R(v) ((PROC_FD_SPAIR_BASE - (v)) / PROC_MAX_PIPES)
#define PROC_FD_SPAIR_W(v) ((PROC_FD_SPAIR_BASE - (v)) % PROC_MAX_PIPES)
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
    uint32_t head, tail, count;
    uint8_t used;
    uint32_t read_refs;                 /* descriptors on its ends, in every process */
    uint32_t write_refs;
    uint8_t *data;                      /* PROC_PIPE_CAPACITY bytes, while it is used */
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
    uint64_t entry;
    uint64_t user_stack;
    int linux_abi;                     /* 1: a 32-bit Linux program (int 0x80, i386); 2: a 64-bit one (syscall) */
    void *mm64;                        /* a 64-bit program's memory map (linux64.c) */
    int exit_code;
    int gfx;                            /* switched the display into graphics mode */
    void *win, *win_prev;               /* a window of its own (SYS_WIN_OPEN), and the one it drew to before */
    int fd[PROC_MAX_FDS];               /* VFS descriptors (-1 = free); 0..2 are the console and not stored here */
    uint32_t fd_flags[PROC_MAX_FDS];
    uint32_t fd_open_flags[PROC_MAX_FDS];
    char fdpath[PROC_MAX_FDS][VFS_NAME_LEN];
    uint32_t fd_dir_offset[PROC_MAX_FDS];
    proc_epoll_watch_t epoll_watch[PROC_MAX_EPOLL_WATCH];
    proc_eventfd_t eventfds[PROC_MAX_EVENTFDS];
    uint32_t std_flags[3];              /* O_NONBLOCK and the like of the console's descriptors 0..2 */
    int std_fd[3];                      /* 0: fd 0..2 is the console; else the descriptor it stands for (dup2) */
    /* signals (64-bit programs run handlers): what each does, which are waiting */
    uint64_t sig_handler[65], sig_flags[65], sig_restorer[65], sig_mask[65];
    uint64_t sig_pending;
    int term_sig;                       /* ended by this signal (wait status) */
    uint32_t vfork_parent;              /* vfork: shares its parent's memory until execve or exit */
    int vfork_done;
    uint32_t umask;                     /* Linux umask, 022 until the program sets one */
    int umask_set;
    proc_vm_region_t vm_regions[PROC_MAX_VM_REGIONS];
    char name[32];
} proc_t;

/* loads the .t file at `path` into a new address space and starts it; returns the pid or a negative error */
int  proc_spawn(const char *path);
int  proc_spawn_argv(const char *path, const char *const argv[], const char *const envp[]);
/* replaces the calling process's program (ELF64, ELF32, #! scripts); returns only on failure,
 * with a negative errno. `scratch` (where argv/envp are, malloc'd) is freed on success. */
int  proc_execve_argv(const char *path, const char *const argv[], const char *const envp[], void *scratch);
/* fork: share_vm (vfork, CLONE_VM) runs the child in the parent's memory until it execs or exits,
 * on child_stack (0 = the parent's), the caller waiting; returns the child's pid or -errno */
int  proc_fork(void *regs, int share_vm, uint64_t child_stack);
/* a signal for a process: its handler (pending), or what the signal does by default */
int  proc_signal(proc_t *p, int sig);
void proc_signal_surface(void *surface, int sig);   /* every program on one terminal window (^C) */
/* wait4: a child that has ended (pid -1 = any); 0 with nohang when none has yet, -ECHILD */
int  proc_wait_child(int pid, int nohang, int *status);
proc_t *proc_current(void);
proc_pipe_t *proc_pipe(int index);              /* the system's pipe table */
int  proc_pipe_alloc(void);                       /* a new pipe, one reference on each end; -1 */
void proc_pipe_ref(int index, int write_end);
void proc_pipe_unref(int index, int write_end);
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
