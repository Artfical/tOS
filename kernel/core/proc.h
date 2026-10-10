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
#define PROC_BRK_BASE  0x88000000u      /* heap grows up from here */
#define PROC_BRK_LIMIT 0x90000000u

typedef struct proc {
    int used;
    int done;                           /* finished: exit_code is valid until someone waits for it */
    uint32_t pid, ppid;
    uint32_t *pgdir;
    uint32_t brk;
    int exit_code;
    int gfx;                            /* switched the display into graphics mode */
    void *win, *win_prev;               /* a window of its own (SYS_WIN_OPEN), and the one it drew to before */
    int fd[PROC_MAX_FDS];               /* VFS descriptors (-1 = free); 0..2 are the console and not stored here */
    char fdpath[PROC_MAX_FDS][VFS_NAME_LEN];
    char name[32];
} proc_t;

/* loads the .t file at `path` into a new address space and starts it; returns the pid or a negative error */
int  proc_spawn(const char *path);
proc_t *proc_current(void);
proc_t *proc_by_pid(uint32_t pid);
void proc_exit(int code) __attribute__((noreturn));
/* ends another process (the caller checks permission); 0 = done */
int  proc_kill(uint32_t pid, int code);
/* blocks until the process has ended; returns its exit code (or -1 for an unknown pid) and releases it */
int  proc_wait(uint32_t pid);
int  proc_running_count(void);

#endif
