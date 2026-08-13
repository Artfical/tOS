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
uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

#endif
