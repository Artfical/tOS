/* kernel/wolf3d's source does its own raw open()/read()/write()/
 * close()/lseek() file I/O in several places (savegames, config,
 * screenshots) rather than always going through kernel/lib/stdio.h's
 * buffered fopen/fread/etc -- these bridge to kernel/fs/vfs.h's
 * vfs_open()/vfs_read()/vfs_write()/vfs_close()/vfs_lseek(), the same
 * VFS every other filesystem access in tOS already goes through, the
 * same way kernel/doom/port/doom_compat.c bridges remove()/rename()
 * to vfs_unlink()/vfs_rename() for DOOM. O_TEXT/O_BINARY are DOS-era
 * distinctions tOS has no equivalent of (no text-mode CRLF
 * translation anywhere) -- both are defined as 0, matching how every
 * other freestanding libc.h port handles them. */
#ifndef WOLF3D_FCNTL_H
#define WOLF3D_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_TEXT   0
#define O_BINARY 0

int open(const char *path, int flags, ...);
int close(int fd);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
long lseek(int fd, long offset, int whence);
int unlink(const char *path);

#ifdef __cplusplus
}
#endif

#endif
