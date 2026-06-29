#ifndef TOS_API_H
#define TOS_API_H

#include <stdint.h>

/* Single API surface shared by every scripting front-end (T#, embedded
 * MicroPython, and anything added later) so each language binding stays
 * a thin wrapper instead of reimplementing file/process/GUI access. */

/* Runs a shell command line (any builtin: ls, cat, mkdir, ping, disk,
 * ps, kill, ...) with its output captured into `out` instead of going
 * to the visible terminal. Always returns 0; a failing command's error
 * text (e.g. "No such file") ends up in `out` like any other output. */
int tos_exec(const char *cmd, char *out, int out_max);

/* Reads a whole file into `out` (NUL-terminated, truncated to out_max-1
 * bytes). Returns the number of bytes read, or -1 if the path doesn't
 * exist or is a directory. */
int tos_read(const char *path, char *out, int out_max);

/* Overwrites (or creates) a file with exactly `len` bytes. Returns 0 on
 * success, -1 on failure. */
int tos_write(const char *path, const char *data, int len);

int tos_mkdir(const char *path);
int tos_delete(const char *path);
int tos_exists(const char *path);

/* Lists a directory's entries (excluding "." and "..") as newline-
 * separated names into `out`. Returns the entry count, or -1 if the
 * path isn't a directory. */
int tos_list(const char *path, char *out, int out_max);

/* Opens a GUI app window by name: "notepad", "paint", "files", "viewer",
 * "calculator", "clock", "about", "diskutil", "taskmgr", "terminal".
 * Only meaningful in GUI mode. Returns 0 if the name was recognized,
 * -1 otherwise. */
int tos_open_app(const char *name);

/* Lists running tasks as "pid name state\n" lines into `out`. */
int tos_ps(char *out, int out_max);

/* Kills a task by pid, going through the same window-aware cleanup
 * Task Manager uses. Refuses the idle task and the caller's own task. */
int tos_kill(uint32_t pid);

/* Seconds since boot. */
uint32_t tos_uptime(void);

#endif
