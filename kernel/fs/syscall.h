#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_EXECVE  11
#define SYS_CHDIR   12
#define SYS_BRK     17
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_KILL    37
#define SYS_ISATTY  71
#define SYS_FSTAT   108

/* tOS-specific extensions -- deliberately numbered well above the
 * Linux-i386-compatible range above (highest real one there is 108)
 * so a future real syscall never collides with these. */
#define SYS_NET_RESOLVE  200
#define SYS_NET_CONNECT  201
#define SYS_NET_SEND     202
#define SYS_NET_RECV     203
#define SYS_NET_CLOSE    204
#define SYS_GFX_INIT     210
#define SYS_GFX_PUTPIXEL 211

struct tos_stat {
    uint16_t st_dev;
    uint16_t st_ino;
    uint32_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint16_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

void syscall_init(void);
uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

#endif
