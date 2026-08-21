#include "syscall.h"
#include "isr.h"
#include "terminal.h"
#include "fs.h"
#include "ramfs.h"
#include "memory.h"
#include "paging.h"
#include "usermode.h"
#include "string.h"
#include "dns.h"
#include "tcp.h"
#include "bochs.h"
#include "keyboard.h"
#include "sha256.h"
#include "aes.h"
#include "bignum.h"
#include "audio.h"
#include "png.h"
#include "debugmon.h"
#include "vga.h"

/* Fixed-layout argument structs for the crypto syscalls -- mirrored
 * by hand in the SDK's tos.h (there's no shared kernel/userspace
 * header; the SDK lives in a separate repo). Keep field order/types
 * in sync if either side changes. */
struct crypto_hash_args {
    const uint8_t *data;
    uint32_t len;
    uint8_t *out; /* 32 bytes */
};
struct crypto_hmac_args {
    const uint8_t *key;
    uint32_t klen;
    const uint8_t *msg;
    uint32_t mlen;
    uint8_t *out; /* 32 bytes */
};
struct crypto_aesctr_args {
    const uint8_t *key16;
    const uint8_t *iv16;
    const uint8_t *in;
    uint8_t *out;
    uint32_t len;
};
struct crypto_modexp_args {
    const uint8_t *base256;
    const uint8_t *exp;
    uint32_t exp_len;
    const uint8_t *mod256;
    uint8_t *out256;
};
struct gfx_blit_args {
    int x, y, w, h;
    const uint32_t *pixels; /* w*h, packed 0x00RRGGBB, row-major */
};
struct inflate_args {
    const uint8_t *src;
    uint32_t src_len;
    uint8_t *out;
    uint32_t out_cap;
    uint32_t *out_len; /* written on success */
};

/* Seeded from RDTSC on first use rather than a fixed constant, so it
 * at least varies boot to boot -- still just an LCG, not
 * cryptographically secure (same tradeoff tls.c's own prng_state
 * already makes for TLS's client-random/premaster secret). Good
 * enough for this v1's threat model (see the security-limitations
 * note in the SSH client's own source), not a real CSPRNG. */
static uint32_t prng_state_syscall = 0;
static uint8_t prng_syscall_byte(void)
{
    if (!prng_state_syscall) {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        prng_state_syscall = lo ^ hi ^ 0x2AF7C1D3;
        if (!prng_state_syscall) prng_state_syscall = 0x2AF7C1D3;
    }
    prng_state_syscall = prng_state_syscall * 1664525 + 1013904223;
    return (uint8_t)(prng_state_syscall >> 16);
}

/* AES-128-CTR: not in aes.h (which only has the raw block primitive)
 * -- built here from aes128_encrypt() the same way tls.c builds its
 * own CBC framing on top of the same block primitive. CTR's
 * keystream block i is AES(key, iv_as_128bit_counter + i); the
 * caller's data is XORed with it, which is its own inverse, so this
 * same function both encrypts and decrypts. */
static void aes128_ctr_crypt(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t counter[16];
    memcpy(counter, iv, 16);
    uint32_t off = 0;
    while (off < len) {
        uint8_t stream[16];
        aes128_encrypt(key, counter, stream);
        uint32_t chunk = len - off;
        if (chunk > 16) chunk = 16;
        for (uint32_t i = 0; i < chunk; i++) out[off + i] = in[off + i] ^ stream[i];
        off += chunk;
        for (int i = 15; i >= 0; i--) { if (++counter[i]) break; }
    }
}

#define TOS_O_WRONLY 0x0001
#define TOS_O_RDWR   0x0002
#define TOS_O_CREAT  0x0040
#define TOS_O_TRUNC  0x0200

static bochs_device_t gfx_dev;
static int gfx_ready = 0;

/* Checks that [ptr, ptr+len) lies entirely within memory a ring3 .t
 * program actually legitimately owns (its own code/data region or its
 * own stack), rejecting overflow (ptr+len wrapping past 0xFFFFFFFF)
 * and zero-length ranges. Several syscalls added this session
 * (SYS_GFX_BLIT, SYS_INFLATE) take a pointer straight from ring3 and
 * either read from it in bulk (memcpy into the framebuffer -- an
 * arbitrary kernel-memory-read primitive if unchecked) or write to it
 * in bulk (inflate's decompression output -- an arbitrary kernel-
 * memory-write primitive, strictly worse, since the attacker also
 * controls the compressed input driving what gets written) without
 * ever checking the pointer was ring3's to begin with. Ring0 ignores
 * the page tables' U/S bit entirely (see the PTE_USER hardening
 * commit's own comment), so nothing about the paging setup stops the
 * kernel itself from touching any address a ring3 program hands it
 * through a syscall -- that check has to happen here, explicitly, per
 * syscall that takes a buffer pointer. */
static int user_range_ok(uint32_t ptr, uint32_t len)
{
    if (len == 0) return 0;
    uint32_t end = ptr + len;
    if (end < ptr) return 0; /* overflow */
    if (ptr >= USER_CODE_BASE && end <= USER_CODE_BASE + USER_CODE_MAX_SIZE) return 1;
    uint32_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * 4096;
    if (ptr >= stack_bottom && end <= USER_STACK_TOP) return 1;
    return 0;
}

/* Same restore sequence cmd_vgatest() uses. Shared by SYS_GFX_EXIT
 * (explicit) and SYS_EXIT's safety net (implicit, for a program that
 * crashed or forgot). */
static void gfx_leave_if_active(void)
{
    if (!gfx_ready) return;
    gfx_ready = 0;
    /* Same reasoning as vga_set_mode()'s own interrupt-disable (see its
     * comment): GUI mode's desktop task repaints on every timer tick
     * regardless of what this sequence is doing, and bochs_disable()
     * plus the terminal state resets below are just as vulnerable to
     * landing mid-sequence as vga_set_mode()'s own register writes --
     * vga_set_mode() only protects its own body, not bochs_disable()
     * before it or the terminal calls after it. pushfl/popfl nests
     * safely with vga_set_mode()'s own inner disable. */
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
    asm volatile("pushl %0; popfl" :: "r"(flags));
}

#define FD_MAX 64
#define FD_FREE 0
#define FD_FILE 1
#define FD_CONSOLE 2

typedef struct {
    int type;
    fs_file_t file;
} fd_entry_t;

static fd_entry_t fd_table[FD_MAX];
static uint32_t program_break = 0x800000;

static int fd_alloc(void)
{
    for (int i = 3; i < FD_MAX; i++) {
        if (fd_table[i].type == FD_FREE) return i;
    }
    return -1;
}

static void syscall_stub(registers_t *regs)
{
    uint32_t result = syscall_handler(regs->eax, regs->ebx, regs->ecx, regs->edx, regs->esi);
    regs->eax = result;
}

uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    (void)d;

    switch (syscall) {
        case SYS_EXIT:
            /* If the exiting program left Bochs/VBE graphics mode
             * active (crashed, forgot, or just never called
             * SYS_GFX_EXIT), restore VGA text mode before handing
             * control back to the shell -- returning to the shell
             * mid-graphics-mode with no restore is exactly the bug
             * class DOOM/vgatest/wolf3d all had to solve explicitly
             * (see the kernel README's Linear framebuffer graphics
             * section); reproduced here directly by a video player
             * that never called any restore path at all, causing a
             * page fault right after "done." on return to the shell. */
            gfx_leave_if_active();
            sys_exit_longjmp();

        case SYS_FORK:
            return -1;

        case SYS_READ: {
            int fd = (int)a;
            char *buf = (char *)b;
            int count = (int)c;
            if (fd == 0) {
                int i;
                for (i = 0; i < count; i++) {
                    char ch = keyboard_getchar();
                    terminal_putchar(ch);
                    buf[i] = ch;
                    if (ch == '\n') { i++; break; }
                }
                return i;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                /* ramfs_read()/ramfs_write() return a byte count on
                 * success (or negative on failure), not a 0/-1 status
                 * -- checking `== 0` here always failed (to_read is
                 * essentially never exactly 0), so file reads through
                 * this syscall never actually worked before. */
                fs_file_t *f = &fd_table[fd].file;
                uint32_t to_read = count;
                if (f->offset + to_read > f->size)
                    to_read = f->size - f->offset;
                int n = ramfs_read(f->name, buf, to_read, f->offset);
                if (n < 0) return -1;
                f->offset += n;
                return n;
            }
            return -1;
        }

        case SYS_WRITE: {
            int fd = (int)a;
            const char *buf = (const char *)b;
            int count = (int)c;
            if (fd == 1 || fd == 2) {
                for (int i = 0; i < count; i++) {
                    if (buf[i] == '\n')
                        terminal_putchar('\n');
                    else
                        terminal_putchar(buf[i]);
                }
                return count;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                fs_file_t *f = &fd_table[fd].file;
                int n = ramfs_write(f->name, buf, count, f->offset);
                if (n < 0) return -1;
                f->offset += n;
                if (f->offset > f->size) f->size = f->offset;
                return n;
            }
            return -1;
        }

        case SYS_OPEN: {
            /* Deliberately does NOT go through fs_open()/fs.c -- that's
             * a separate, never-initialized static file table
             * (fs_init() is never called anywhere in the boot path)
             * left over from before ramfs existed, always empty, so
             * SYS_OPEN silently failed for every path until now. ramfs
             * is the filesystem everything else in tOS actually uses. */
            const char *path = (const char *)a;
            int flags = (int)b;
            if (!ramfs_exists(path)) {
                if (!(flags & TOS_O_CREAT)) return -1;
                if (ramfs_create(path) != 0) return -1;
            }
            int fd = fd_alloc();
            if (fd < 0) return -1;
            fs_file_t *f = &fd_table[fd].file;
            int i = 0;
            while (path[i] && i < FS_NAME_LEN - 1) { f->name[i] = path[i]; i++; }
            f->name[i] = 0;
            f->size = ramfs_size(path);
            f->offset = 0;
            f->exists = 1;
            fd_table[fd].type = FD_FILE;
            return fd;
        }

        case SYS_CLOSE: {
            int fd = (int)a;
            if (fd >= 0 && fd < FD_MAX) {
                fd_table[fd].type = FD_FREE;
                return 0;
            }
            return -1;
        }

        case SYS_WAITPID:
            return -1;

        /* ELF loading/exec support has been removed: the loader wrote
         * PT_LOAD segment data straight to phdr->p_vaddr with no
         * bounds check, and this kernel's paging_init() maps *all*
         * physical RAM (including kernel memory) with PTE_USER from
         * boot -- so any user-supplied binary could point a segment
         * at kernel memory and overwrite it via a plain memcpy(), a
         * straightforward privilege-escalation primitive. Fixing that
         * properly needs real per-process address space isolation
         * (kernel pages not user-accessible, non-identity per-process
         * mappings), which is a much larger change than a point fix,
         * so exec is disabled rather than shipped half-fixed. */
        case SYS_EXECVE:
            (void)a;
            return -1;

        case SYS_CHDIR:
            return 0;

        case SYS_BRK: {
            uint32_t addr = a;
            if (addr == 0)
                return program_break;
            if (addr < 0x800000)
                addr = 0x800000;
            uint32_t old = program_break;
            for (uint32_t p = old; p < addr; p += 0x1000) {
                if (!paging_virt_to_phys(NULL, p)) {
                    uint32_t phys = alloc_physical_page();
                    if (!phys) return -1;
                    paging_map(p, phys, PTE_USER | PTE_WRITABLE);
                }
            }
            program_break = addr;
            return old;
        }

        case SYS_LSEEK: {
            int fd = (int)a;
            int offset = (int)b;
            int whence = (int)c;
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                fs_file_t *f = &fd_table[fd].file;
                uint32_t new_off;
                if (whence == 0) new_off = offset;
                else if (whence == 1) new_off = f->offset + offset;
                else if (whence == 2) new_off = f->size + offset;
                else return -1;
                if (new_off > f->size) new_off = f->size;
                f->offset = new_off;
                return new_off;
            }
            return -1;
        }

        case SYS_GETPID:
            return 1;

        case SYS_KILL:
            if (b == 15 || b == 9) {
                sys_exit_longjmp();
            }
            return -1;

        case SYS_ISATTY: {
            int fd = (int)a;
            if (fd >= 0 && fd <= 2) return 1;
            return 0;
        }

        case SYS_FSTAT: {
            int fd = (int)a;
            struct tos_stat *st = (struct tos_stat *)b;
            if (!st) return -1;
            memset(st, 0, sizeof(*st));
            if (fd >= 0 && fd <= 2) {
                st->st_mode = 0x2000;
                return 0;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                st->st_mode = 0x8000;
                st->st_size = fd_table[fd].file.size;
                st->st_blksize = 512;
                st->st_blocks = (st->st_size + 511) / 512;
                return 0;
            }
            return -1;
        }

        case SYS_NET_RESOLVE: {
            const char *host = (const char *)a;
            uint32_t ip = 0;
            if (dns_resolve(host, &ip) != 0) return 0;
            return ip;
        }

        case SYS_NET_CONNECT: {
            uint32_t ip = a;
            uint16_t port = (uint16_t)b;
            return tcp_connect(ip, port);
        }

        case SYS_NET_SEND: {
            /* tcp_send() returns a status code (0 success, negative
             * failure), not a byte count -- SYS_NET_SEND's userspace
             * contract (tos_net_send() in the SDK) is the usual
             * write()-style "returns bytes sent, <=0 on failure" that
             * callers loop on, so translate here rather than exposing
             * the status-code convention directly, which a caller
             * checking `r <= 0` would misread a *successful* 0 as a
             * failure. */
            void *data = (void *)a;
            int len = (int)b;
            int rc = tcp_send(data, len);
            return (rc == 0) ? len : -1;
        }

        case SYS_NET_RECV: {
            uint8_t *buf = (uint8_t *)a;
            int max_len = (int)b;
            return tcp_recv(buf, max_len);
        }

        case SYS_NET_CLOSE:
            tcp_close();
            return 0;

        case SYS_GFX_INIT: {
            int width = (int)a;
            int height = (int)b;
            /* Snapshot the real, valid boot-time text-mode VGA
             * registers before anything touches VBE -- has to happen
             * before the first-ever mode switch (idempotent past that,
             * see vga_init()'s own guard) or there's nothing correct
             * left to restore later. Same ordering cmd_vgatest() uses. */
            vga_init();
            if (bochs_init(&gfx_dev) != 0) return -1;
            /* Same reasoning as vga_set_mode()'s own interrupt-disable:
             * GUI mode's desktop task repaints on every timer tick
             * regardless of what this mode switch is doing, and a timer
             * interrupt landing mid-sequence here could corrupt VGA/VBE
             * state exactly like the DOOM/vgatest/wolf3d cases already
             * documented for the equivalent restore path. Covers the
             * actual mode switch through the framebuffer page mapping;
             * both early-return points below are before this or
             * restore flags themselves. */
            uint32_t flags;
            asm volatile("pushfl; popl %0; cli" : "=r"(flags));
            if (bochs_set_mode(&gfx_dev, width, height, 32) != 0) {
                asm volatile("pushl %0; popfl" :: "r"(flags));
                return -1;
            }
            /* The LFB is a PCI BAR address, not RAM -- it sits well
             * above paging_init()'s identity-mapped [0, total_mem)
             * range and was never actually paged in, so bochs_put_pixel
             * dereferencing dev->lfb directly would fault. Map it here
             * (identity: virt == phys, matching what bochs_put_pixel
             * assumes) before anything writes through it. */
            uint32_t fb_bytes = (uint32_t)width * (uint32_t)height * 4;
            uint32_t fb_pages = (fb_bytes + 4095) / 4096;
            for (uint32_t i = 0; i < fb_pages; i++) {
                uint32_t addr = gfx_dev.lfb + i * 4096;
                paging_map(addr, addr, PTE_PRESENT | PTE_WRITABLE);
            }
            gfx_ready = 1;
            asm volatile("pushl %0; popfl" :: "r"(flags));
            return 0;
        }

        case SYS_GFX_PUTPIXEL: {
            if (!gfx_ready) return -1;
            int x = (int)a;
            int y = (int)b;
            uint32_t color = c;
            bochs_put_pixel(&gfx_dev, x, y, color);
            return 0;
        }

        case SYS_GFX_BLIT: {
            if (!gfx_ready) return -1;
            if (!user_range_ok(a, sizeof(struct gfx_blit_args))) return -1;
            struct gfx_blit_args *args = (struct gfx_blit_args *)a;
            if (args->x < 0 || args->y < 0 || args->w <= 0 || args->h <= 0) return -1;
            if (args->x + args->w > gfx_dev.width || args->y + args->h > gfx_dev.height) return -1;
            /* args->pixels is a second, independent pointer read out of
             * ring3-controlled memory -- validating the struct itself
             * says nothing about where this points. Without this check
             * a ring3 program could point it at arbitrary kernel memory
             * and have this syscall copy it straight into the
             * framebuffer, an arbitrary kernel-read primitive. */
            if (!user_range_ok((uint32_t)(unsigned long)args->pixels, (uint32_t)args->w * (uint32_t)args->h * 4))
                return -1;
            uint32_t *fb = (uint32_t *)(unsigned long)gfx_dev.lfb;
            for (int row = 0; row < args->h; row++) {
                memcpy(fb + (args->y + row) * gfx_dev.width + args->x,
                       args->pixels + row * args->w,
                       (uint32_t)args->w * 4);
            }
            return 0;
        }

        case SYS_GFX_EXIT:
            gfx_leave_if_active();
            return 0;

        case SYS_KEY_POLL: {
            char ch;
            if (keyboard_try_getchar(&ch)) return (uint32_t)(uint8_t)ch;
            return (uint32_t)-1;
        }

        case SYS_UPTIME_MS:
            return debugmon_uptime_ms();

        case SYS_AUDIO_SUBMIT: {
            /* audio_init() re-probes hardware I/O ports with several
             * busy-wait loops per backend tried (SB16 at up to 3 base
             * ports, then an AC97 PCI scan) -- fine as a one-time cost,
             * but calling it on every single failed submit (as a video
             * player does, once per frame, when no audio backend is
             * present) made a whole video noticeably slower to play
             * back, entirely from repeated failed hardware probing.
             * Try exactly once. */
            static int audio_probe_done = 0;
            if (!audio_probe_done) {
                audio_probe_done = 1;
                if (!audio_available()) audio_init();
            }
            /* buf is read from directly (memcpy'd into the backend's
             * DMA buffer) -- same unchecked-ring3-pointer class as
             * SYS_GFX_BLIT's pixel source, just a smaller/less directly
             * observable read (audio output rather than a visible
             * framebuffer), capped at AUDIO_DMA_SIZE (4096) either way. */
            if (!user_range_ok(a, b)) return (uint32_t)-1;
            const uint8_t *buf = (const uint8_t *)a;
            uint32_t len = b;
            return (uint32_t)audio_submit(buf, len);
        }

        case SYS_AUDIO_BUSY:
            return (uint32_t)audio_busy();

        case SYS_AUDIO_STOP:
            audio_stop();
            return 0;

        case SYS_INFLATE: {
            /* args->out is the decompression *output* target -- an
             * unchecked pointer here is an arbitrary kernel-memory-write
             * primitive with attacker-controlled content (the caller
             * also controls args->src, the compressed input driving
             * what gets written), strictly worse than SYS_GFX_BLIT's
             * read-only equivalent. args->out_len is a second write
             * target (the decompressed length gets stored there) and
             * needs the same check. */
            if (!user_range_ok(a, sizeof(struct inflate_args))) return (uint32_t)-1;
            struct inflate_args *args = (struct inflate_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->src, args->src_len)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out, args->out_cap)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out_len, sizeof(uint32_t))) return (uint32_t)-1;
            uint32_t out_len = 0;
            int rc = inflate_raw_buffer(args->src, args->src_len, args->out, args->out_cap, &out_len);
            if (rc != 0) return (uint32_t)-1;
            *args->out_len = out_len;
            return 0;
        }

        /* None of the five crypto syscalls below validated their
         * ring3-supplied pointers before this either -- the same
         * missing-check class as SYS_GFX_BLIT/SYS_INFLATE/
         * SYS_AUDIO_SUBMIT, just generally lower-impact per call:
         * SYS_CRYPTO_RANDOM and SYS_CRYPTO_AES128_CTR can still be
         * driven into a fully attacker-chosen arbitrary write (CTR
         * mode's output is plaintext XOR a keystream the caller can
         * already compute from its own chosen key/iv, so `in` can be
         * set to cancel the keystream out to any desired byte), but
         * SYS_CRYPTO_SHA256/HMAC_SHA256 only ever leak a 32-byte
         * *digest* of whatever memory `data`/`key`/`msg` pointed at --
         * real disclosure in principle, but not practically usable
         * without also breaking SHA-256 preimage resistance. Hardened
         * all five regardless while already auditing this file. */
        case SYS_CRYPTO_RANDOM: {
            if ((int)b < 0 || !user_range_ok(a, b)) return (uint32_t)-1;
            uint8_t *buf = (uint8_t *)a;
            int len = (int)b;
            for (int i = 0; i < len; i++) buf[i] = prng_syscall_byte();
            return 0;
        }

        case SYS_CRYPTO_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hash_args))) return (uint32_t)-1;
            struct crypto_hash_args *args = (struct crypto_hash_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->data, args->len)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out, 32)) return (uint32_t)-1;
            sha256_hash(args->data, args->len, args->out);
            return 0;
        }

        case SYS_CRYPTO_HMAC_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hmac_args))) return (uint32_t)-1;
            struct crypto_hmac_args *args = (struct crypto_hmac_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->key, args->klen)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->msg, args->mlen)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out, 32)) return (uint32_t)-1;
            hmac_sha256(args->key, args->klen, args->msg, args->mlen, args->out);
            return 0;
        }

        case SYS_CRYPTO_AES128_CTR: {
            if (!user_range_ok(a, sizeof(struct crypto_aesctr_args))) return (uint32_t)-1;
            struct crypto_aesctr_args *args = (struct crypto_aesctr_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->key16, 16)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->iv16, 16)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->in, args->len)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out, args->len)) return (uint32_t)-1;
            aes128_ctr_crypt(args->key16, args->iv16, args->in, args->out, args->len);
            return 0;
        }

        case SYS_CRYPTO_MODEXP: {
            if (!user_range_ok(a, sizeof(struct crypto_modexp_args))) return (uint32_t)-1;
            struct crypto_modexp_args *args = (struct crypto_modexp_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->base256, 256)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->exp, args->exp_len)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->mod256, 256)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->out256, 256)) return (uint32_t)-1;
            bignum_modexp(args->base256, args->exp, args->exp_len, args->mod256, args->out256);
            return 0;
        }

        default:
            return -1;
    }
}

void syscall_init(void)
{
    for (int i = 0; i < FD_MAX; i++)
        fd_table[i].type = FD_FREE;
    fd_table[0].type = FD_CONSOLE;
    fd_table[1].type = FD_CONSOLE;
    fd_table[2].type = FD_CONSOLE;
    isr_register_handler(0x80, syscall_stub);
}
