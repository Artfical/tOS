#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "isr.h"

#define SYS_EXIT    1
#define SYS_FORK    2
#define LINUX_SYS_CLONE 120
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
#define SYS_GETUID  24
#define SYS_GETGID  47
#define SYS_KILL    37
#define SYS_ISATTY  71
#define SYS_FSTAT   108
#define SYS_MUNMAP  91
#define SYS_MPROTECT 125
#define SYS_MMAP2   192

/* Linux i386 syscalls needed by a static ELF runtime. Kept explicitly
 * prefixed because 199-204 overlap tOS-specific network extensions. */
#define LINUX_SYS_GETPPID          64
#define LINUX_SYS_OPENAT           295
#define LINUX_SYS_WRITEV           146
#define LINUX_SYS_BRK              45
#define LINUX_SYS_UNLINK           10
#define LINUX_SYS_PIPE             42
#define LINUX_SYS_CHMOD            15
#define LINUX_SYS_ACCESS           33
#define LINUX_SYS_RENAME           38
#define LINUX_SYS_MKDIR            39
#define LINUX_SYS_FCNTL            55
#define LINUX_SYS_DUP              41
#define LINUX_SYS_DUP2             63
#define LINUX_SYS_READLINK         85
#define LINUX_SYS_SYMLINK          83
#define LINUX_SYS_GETCWD           183
#define LINUX_SYS_GETTIMEOFDAY     78
#define LINUX_SYS_EPOLL_CREATE     254
#define LINUX_SYS_EPOLL_CTL        255
#define LINUX_SYS_EPOLL_WAIT       256
#define LINUX_SYS_RT_SIGACTION     174
#define LINUX_SYS_IOCTL            54
#define LINUX_SYS_UGETRLIMIT       191
#define LINUX_SYS_TKILL            238
#define LINUX_SYS_CLOCK_GETRES     266
#define LINUX_SYS_TGKILL           270
#define LINUX_SYS_PRLIMIT64        340
#define LINUX_SYS_ARCH_PRCTL       384
#define LINUX_SYS_CLOCK_GETRES64   406
#define LINUX_SYS_RT_SIGPROCMASK   175
#define LINUX_SYS_SIGALTSTACK      186
#define LINUX_SYS_GETDENTS64       220
#define LINUX_SYS_UNAME            122
#define LINUX_SYS_NANOSLEEP        162
#define LINUX_SYS_GETEUID          49
#define LINUX_SYS_GETEGID          50
#define LINUX_SYS_GETUID32         199
#define LINUX_SYS_GETGID32         200
#define LINUX_SYS_GETEUID32        201
#define LINUX_SYS_GETEGID32        202
#define LINUX_SYS_GETTID           224
#define LINUX_SYS_FUTEX            240
#define LINUX_SYS_EPOLL_PWAIT      319
#define LINUX_SYS_EPOLL_CREATE1    329
#define LINUX_SYS_PIPE2            331
#define LINUX_SYS_SET_THREAD_AREA  243
#define LINUX_SYS_GET_THREAD_AREA  244
#define LINUX_SYS_LLSEEK           140
#define LINUX_SYS_SET_TID_ADDRESS  258
#define LINUX_SYS_CLOCK_GETTIME    265
#define LINUX_SYS_SET_ROBUST_LIST  311
#define LINUX_SYS_GETRANDOM        355
#define LINUX_SYS_STAT64           195
#define LINUX_SYS_LSTAT64          196
#define LINUX_SYS_FSTAT64          197
#define LINUX_SYS_FSTATAT64        300
#define LINUX_SYS_CLOCK_GETTIME64  403
#define LINUX_SYS_NANOSLEEP64      407
#define LINUX_SYS_FCNTL64          221
#define LINUX_SYS_READLINKAT       305

/* tOS-specific extensions. ELF processes dispatch through the isolated
 * Linux-i386 syscall table, so matching numeric values are ABI-specific. */
/* Windows and text output, so a program can be a real windowed app of its own instead of only writing to
 * whatever window started it. A window belongs to the process that opened it and is closed with it. */
#define SYS_WIN_OPEN     260 /* a=title -- opens this process's own window; 0 = ok, -1 without a GUI or when none is free */
#define SYS_WIN_CLOSE    261
#define SYS_WIN_FOCUS    262 /* 1 = this process's window is the focused one (so its keys are meant for it) */
#define SYS_WIN_CLICK    263 /* a=int[2] -- 1 and fills x,y with a pending click inside the window, else 0 */
#define SYS_TERM_CLEAR   264
#define SYS_TERM_SETPOS  265 /* a=column, b=row */
#define SYS_TERM_COLOR   266 /* a=VGA attribute byte (foreground | background << 4) */

#define SYS_SPAWN        251 /* a=path; b=optional argv[]; c=optional envp[]; starts a new .t/ELF process */
#define SYS_OPEN_APP     250 /* a=app name -- opens a built-in window-manager app (what the programs in /system/apps do); -1 without a GUI */
#define SYS_NET_RESOLVE  200
#define SYS_NET_CONNECT  201
#define SYS_NET_SEND     202
#define SYS_NET_RECV     203
#define SYS_NET_CLOSE    204
#define SYS_GFX_INIT     210
#define SYS_GFX_PUTPIXEL 211
#define SYS_GFX_BLIT     212 /* a=struct gfx_blit_args* -- copies a whole
                               * rectangle of 32bpp pixels into the
                               * framebuffer in one call; too slow to do
                               * real-time video via per-pixel syscalls. */
#define SYS_GFX_EXIT     213 /* Leaves Bochs/VBE graphics mode and
                               * restores VGA text mode -- SYS_EXIT does
                               * this automatically too as a safety net
                               * if a program forgets, but call this
                               * explicitly to return to text mode
                               * without actually exiting. */
#define SYS_KEY_POLL     206 /* non-blocking: returns -1 if no key
                               * buffered, else the key byte (same
                               * decoding as SYS_READ's fd 0 path) --
                               * doesn't echo or block, for a playback
                               * loop that needs to check for a pause
                               * key without stalling frame timing. */
#define SYS_UPTIME_MS    207 /* wraps debugmon_uptime_ms() -- monotonic
                               * milliseconds since boot, for pacing a
                               * fixed-framerate playback loop. */

/* Audio -- thin wrappers over the kernel's existing audio.c mixer
 * (already used by DOOM/media player), fixed 8-bit unsigned mono PCM
 * at the backend's fixed output rate (22050Hz), max 4096 bytes per
 * submit (AUDIO_DMA_SIZE) -- same constraints audio.c already has. */
#define SYS_AUDIO_SUBMIT 221 /* a=buf, b=len -- returns 0 or -1 */
#define SYS_AUDIO_BUSY   222 /* returns 1 if the last submit is still playing */
#define SYS_AUDIO_STOP   223

/* General-purpose raw DEFLATE (RFC1951) decompression -- exposes the
 * kernel's existing inflate_raw_buffer() (kernel/display/png.c, used
 * by the PNG decoder) directly to userspace rather than shipping a
 * second copy of an inflate implementation in the SDK. */
#define SYS_INFLATE      240 /* a=struct inflate_args* */

/* Crypto primitives -- thin wrappers over the kernel's existing,
 * already-in-production TLS crypto (kernel/net/sha256.c, aes.c,
 * bignum.c) instead of reimplementing any of it in userspace, where
 * a subtle bug would be much easier to introduce and much harder to
 * notice than a protocol-logic bug. Used by the SSH client SDK
 * example. All take a pointer to a fixed-layout argument struct in
 * ebx (see the matching structs in tos.h) since most need more than
 * the 4 plain register args a syscall gets. */
#define SYS_CRYPTO_RANDOM      230 /* a=buf, b=len */
#define SYS_CRYPTO_SHA256      231 /* a=struct crypto_hash_args* */
#define SYS_CRYPTO_HMAC_SHA256 232 /* a=struct crypto_hmac_args* */
#define SYS_CRYPTO_AES128_CTR  233 /* a=struct crypto_aesctr_args* */
#define SYS_CRYPTO_MODEXP      234 /* a=struct crypto_modexp_args* */

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
uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d, uint32_t e, uint32_t f, registers_t *regs);


struct proc;
void syscall_proc_cleanup(struct proc *p);   /* closes a finished process's files and display mode */

void syscall_dump_recent(void);   /* the last Linux syscalls, to the serial port */

#endif
