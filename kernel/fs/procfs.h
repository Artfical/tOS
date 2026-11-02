#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include "vfs.h"

/* /proc for Linux programs: files made up when they are looked at (cpuinfo, meminfo, uptime, version, the
 * processes' own status...). Nothing is stored: procfs_gen() writes a file's text into a buffer. */

int  procfs_is(const char *abs_path);                                   /* the path is /proc or below it */
int  procfs_gen(const char *abs_path, char *buf, int cap);              /* the text of a file: its length, or -1 (not a file here) */
int  procfs_stat(const char *abs_path, vfs_entry_t *e);                 /* 0 for a file or folder that exists */
int  procfs_readdir(const char *abs_path, vfs_entry_t *out, int max);   /* entries, or -1 */
int  procfs_readlink(const char *abs_path, char *buf, int cap);         /* /proc/self/exe, cwd...: the length, or -1 */

#endif
