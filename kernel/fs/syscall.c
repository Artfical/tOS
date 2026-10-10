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
#include "tos_api.h"
#include "gui.h"
#include "wm.h"
#include "proc.h"
#include "vfs.h"
#include "scheduler.h"
#include "auth.h"

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
/* The kernel runs with the calling process's page directory loaded, so a user pointer can simply be followed
 * once every page it covers is known to be mapped for the process: these walk the page tables (a pointer into
 * kernel memory, or into a hole, is refused). */
static int user_range_ok(uint32_t ptr, uint32_t len) { return paging_user_range_ok(ptr, len, 0); }
static int user_wr_ok(uint32_t ptr, uint32_t len)    { return paging_user_range_ok(ptr, len, 1); }

/* Same restore sequence cmd_vgatest() uses. Shared by SYS_GFX_EXIT
 * (explicit) and SYS_EXIT's safety net (implicit, for a program that
 * crashed or forgot). */
static void gfx_leave_if_active(void)
{
    if (!gfx_ready) return;
    gfx_ready = 0;
    bochs_set_graphics_active(0);
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

static int user_cstr_ok(uint32_t ptr)
{
    for (uint32_t i = 0; i < FS_NAME_LEN; i++) {
        if (!user_range_ok(ptr + i, 1)) return 0;
        if (((const char *)ptr)[i] == 0) return 1;
    }
    return 0;
}

/* user descriptors 3.. map to slots of the process's own table, which hold VFS descriptors */
static int proc_vfd(proc_t *p, int fd)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return -1;
    return p->fd[fd - 3];
}

void syscall_proc_cleanup(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fd[i] >= 0) { vfs_close(p->fd[i]); p->fd[i] = -1; }
    if (p->gfx) { p->gfx = 0; gfx_leave_if_active(); }
    if (p->win) { wm_script_close(p->win, p->win_prev); p->win = 0; p->win_prev = 0; }
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
            /* the process's files and any graphics mode are released by proc_exit() */
            proc_exit((int)a);

        case SYS_FORK:
            return -1;

        case SYS_READ: {
            int fd = (int)a;
            char *buf = (char *)b;
            int count = (int)c;
            if (count <= 0) return 0;
            if (!user_wr_ok(b, (uint32_t)count)) return -1;
            if (fd == 0) {
                int i;
                for (i = 0; i < count; i++) {
                    /* keyboard_getchar_ring3(), not keyboard_getchar():
                     * see its own comment in keyboard.c for why a ring3
                     * program's blocking read needs a genuinely
                     * different wait than the kernel's own interactive
                     * readline uses. */
                    char ch = keyboard_getchar_ring3();
                    terminal_putchar(ch);
                    buf[i] = ch;
                    if (ch == '\n') { i++; break; }
                }
                return i;
            }
            int vfd = proc_vfd(proc_current(), fd);
            if (vfd < 0) return -1;
            int n = vfs_read(vfd, buf, (uint32_t)count);
            return n < 0 ? -1 : n;
        }

        case SYS_WRITE: {
            int fd = (int)a;
            const char *buf = (const char *)b;
            int count = (int)c;
            if (count <= 0) return 0;
            if (!user_range_ok(b, (uint32_t)count)) return -1;
            if (fd == 1 || fd == 2) {
                for (int i = 0; i < count; i++) {
                    if (buf[i] == '\n')
                        terminal_putchar('\n');
                    else
                        terminal_putchar(buf[i]);
                }
                return count;
            }
            int vfd = proc_vfd(proc_current(), fd);
            if (vfd < 0) return -1;
            int n = vfs_write(vfd, buf, (uint32_t)count);
            return n < 0 ? -1 : n;
        }

        case SYS_OPEN: {
            /* Every file goes through the VFS with the credentials of the process (those of whoever started
             * it), so the owner/group/other permission bits and the /system rules apply exactly as they do
             * for the shell. */
            proc_t *p = proc_current();
            int flags = (int)b;
            if (!p || !user_cstr_ok(a)) return -1;
            char path[FS_NAME_LEN];
            strncpy(path, (const char *)a, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            int slot = -1;
            for (int i = 0; i < PROC_MAX_FDS; i++) if (p->fd[i] < 0) { slot = i; break; }
            if (slot < 0) return -1;
            int vflags = (flags & TOS_O_RDWR) ? VFS_RDWR : (flags & TOS_O_WRONLY) ? VFS_WRONLY : VFS_RDONLY;
            if (flags & TOS_O_CREAT) vflags |= VFS_CREAT;
            if (flags & TOS_O_TRUNC) vflags |= VFS_TRUNC;
            if (flags & 0x400) vflags |= VFS_APPEND;
            int vfd = vfs_open(path, vflags);
            if (vfd < 0) return -1;
            p->fd[slot] = vfd;
            strncpy(p->fdpath[slot], path, VFS_NAME_LEN - 1);
            p->fdpath[slot][VFS_NAME_LEN - 1] = 0;
            return slot + 3;
        }

        case SYS_CLOSE: {
            int fd = (int)a;
            proc_t *p = proc_current();
            if (p && fd >= 3 && fd - 3 < PROC_MAX_FDS && p->fd[fd - 3] >= 0) {
                vfs_close(p->fd[fd - 3]);
                p->fd[fd - 3] = -1;
                return 0;
            }
            return -1;
        }

        case SYS_WAITPID: {
            proc_t *child = proc_by_pid(a);
            if (!child || child->ppid != task_get_pid()) return -1;
            return (uint32_t)proc_wait(a);
        }

        case SYS_SPAWN: {
            if (!user_cstr_ok(a)) return -1;
            char path[FS_NAME_LEN];
            strncpy(path, (const char *)a, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            int pid = proc_spawn(path);
            return pid < 0 ? -1 : pid;
        }

        /* execve replacing the running image is still not offered: SYS_SPAWN starts a program as a new
         * process in its own address space instead. */
        case SYS_EXECVE:
            (void)a;
            return -1;

        case SYS_CHDIR:
            return 0;

        case SYS_BRK: {
            proc_t *p = proc_current();
            if (!p) return -1;
            uint32_t addr = a;
            if (addr == 0) return p->brk;
            if (addr < PROC_BRK_BASE) addr = PROC_BRK_BASE;
            if (addr > PROC_BRK_LIMIT) return -1;
            uint32_t old = p->brk;
            for (uint32_t pg = (old + 0xFFF) & ~0xFFFu; pg < ((addr + 0xFFF) & ~0xFFFu); pg += 0x1000) {
                uint32_t phys = alloc_physical_page();
                if (!phys) return -1;
                if (paging_map_in(p->pgdir, pg, phys, PTE_USER | PTE_WRITABLE) != 0) { free_physical_page(phys); return -1; }
            }
            p->brk = addr;
            return old;
        }

        case SYS_LSEEK: {
            int vfd = proc_vfd(proc_current(), (int)a);
            if (vfd < 0) return -1;
            int r = vfs_lseek(vfd, (uint32_t)b, (int)c);
            return r < 0 ? (uint32_t)-1 : (uint32_t)r;
        }

        case SYS_GETPID:
            return task_get_pid();

        /* the credentials the process actually runs with -- its starter's, or the owner's for a setuid program */
        case SYS_GETUID:
            return auth_uid();

        case SYS_GETGID:
            return auth_gid();

        case SYS_KILL: {
            uint32_t pid = a;
            if (b != 15 && b != 9) return -1;
            if (pid == task_get_pid()) proc_exit(128 + (int)b);
            proc_t *t = proc_by_pid(pid);
            if (!t || t->done) return -1;
            /* only your own processes, unless you are root */
            if (!auth_is_root() && task_get_uid(pid) != auth_uid()) return -1;
            return proc_kill(pid, 128 + (int)b) == 0 ? 0 : (uint32_t)-1;
        }

        case SYS_ISATTY: {
            int fd = (int)a;
            if (fd >= 0 && fd <= 2) return 1;
            return 0;
        }

        case SYS_FSTAT: {
            int fd = (int)a;
            struct tos_stat *st = (struct tos_stat *)b;
            if (!user_wr_ok(b, sizeof(*st))) return -1;
            memset(st, 0, sizeof(*st));
            if (fd >= 0 && fd <= 2) {
                st->st_mode = 0x2000;
                return 0;
            }
            proc_t *p = proc_current();
            if (proc_vfd(p, fd) >= 0) {
                vfs_entry_t e;
                if (vfs_stat(p->fdpath[fd - 3], &e) != 0) return -1;
                st->st_mode = (e.is_dir ? 0x4000 : 0x8000) | (e.mode & 07777);
                st->st_size = e.size;
                st->st_blksize = 512;
                st->st_blocks = (st->st_size + 511) / 512;
                return 0;
            }
            return -1;
        }

        case SYS_WIN_OPEN: {
            proc_t *p = proc_current();
            if (!p || p->win || !gui_is_active() || !user_cstr_ok(a)) return (uint32_t)-1;
            char title[64];
            strncpy(title, (const char *)a, sizeof(title) - 1);
            title[sizeof(title) - 1] = 0;
            void *prev = 0;
            void *w = wm_script_open(title, &prev);
            if (!w) return (uint32_t)-1;
            p->win = w;
            p->win_prev = prev;
            return 0;
        }

        case SYS_WIN_CLOSE: {
            proc_t *p = proc_current();
            if (!p || !p->win) return (uint32_t)-1;
            wm_script_close(p->win, p->win_prev);
            p->win = 0;
            p->win_prev = 0;
            return 0;
        }

        case SYS_WIN_FOCUS:
            return (uint32_t)wm_current_task_has_focus();

        case SYS_WIN_CLICK: {
            if (!user_wr_ok(a, 2 * sizeof(int))) return (uint32_t)-1;
            int x = 0, y = 0;
            if (!wm_get_content_click(&x, &y)) return 0;
            ((int *)a)[0] = x;
            ((int *)a)[1] = y;
            return 1;
        }

        case SYS_TERM_CLEAR:
            terminal_clear();
            return 0;

        case SYS_TERM_SETPOS:
            terminal_setpos(a, b);
            return 0;

        case SYS_TERM_COLOR:
            terminal_setcolor((uint8_t)a);
            return 0;

        case SYS_OPEN_APP: {
            if (!user_cstr_ok(a)) return (uint32_t)-1;
            if (!gui_is_active()) return (uint32_t)-1;
            char app[FS_NAME_LEN];
            strncpy(app, (const char *)a, sizeof(app) - 1);
            app[sizeof(app) - 1] = 0;
            return (uint32_t)tos_open_app(app);
        }

        case SYS_NET_RESOLVE: {
            const char *host = (const char *)a;
            uint32_t ip = 0;
            if (!user_cstr_ok(a)) return 0;
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
            if (len <= 0 || !user_range_ok(a, (uint32_t)len)) return -1;
            int rc = tcp_send(data, len);
            return (rc == 0) ? len : -1;
        }

        case SYS_NET_RECV: {
            uint8_t *buf = (uint8_t *)a;
            int max_len = (int)b;
            if (max_len <= 0 || !user_wr_ok(a, (uint32_t)max_len)) return -1;
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
            { proc_t *gp = proc_current(); if (gp) gp->gfx = 1; }
            bochs_set_graphics_active(1);
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
            if (!user_wr_ok((uint32_t)(unsigned long)args->out, args->out_cap)) return (uint32_t)-1;
            if (!user_wr_ok((uint32_t)(unsigned long)args->out_len, sizeof(uint32_t))) return (uint32_t)-1;
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
            if ((int)b < 0 || !user_wr_ok(a, b)) return (uint32_t)-1;
            uint8_t *buf = (uint8_t *)a;
            int len = (int)b;
            for (int i = 0; i < len; i++) buf[i] = prng_syscall_byte();
            return 0;
        }

        case SYS_CRYPTO_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hash_args))) return (uint32_t)-1;
            struct crypto_hash_args *args = (struct crypto_hash_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->data, args->len)) return (uint32_t)-1;
            if (!user_wr_ok((uint32_t)(unsigned long)args->out, 32)) return (uint32_t)-1;
            sha256_hash(args->data, args->len, args->out);
            return 0;
        }

        case SYS_CRYPTO_HMAC_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hmac_args))) return (uint32_t)-1;
            struct crypto_hmac_args *args = (struct crypto_hmac_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->key, args->klen)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->msg, args->mlen)) return (uint32_t)-1;
            if (!user_wr_ok((uint32_t)(unsigned long)args->out, 32)) return (uint32_t)-1;
            hmac_sha256(args->key, args->klen, args->msg, args->mlen, args->out);
            return 0;
        }

        case SYS_CRYPTO_AES128_CTR: {
            if (!user_range_ok(a, sizeof(struct crypto_aesctr_args))) return (uint32_t)-1;
            struct crypto_aesctr_args *args = (struct crypto_aesctr_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->key16, 16)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->iv16, 16)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->in, args->len)) return (uint32_t)-1;
            if (!user_wr_ok((uint32_t)(unsigned long)args->out, args->len)) return (uint32_t)-1;
            aes128_ctr_crypt(args->key16, args->iv16, args->in, args->out, args->len);
            return 0;
        }

        case SYS_CRYPTO_MODEXP: {
            if (!user_range_ok(a, sizeof(struct crypto_modexp_args))) return (uint32_t)-1;
            struct crypto_modexp_args *args = (struct crypto_modexp_args *)a;
            if (!user_range_ok((uint32_t)(unsigned long)args->base256, 256)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->exp, args->exp_len)) return (uint32_t)-1;
            if (!user_range_ok((uint32_t)(unsigned long)args->mod256, 256)) return (uint32_t)-1;
            if (!user_wr_ok((uint32_t)(unsigned long)args->out256, 256)) return (uint32_t)-1;
            bignum_modexp(args->base256, args->exp, args->exp_len, args->mod256, args->out256);
            return 0;
        }

        default:
            return -1;
    }
}

void syscall_init(void)
{
    isr_register_handler(0x80, syscall_stub);
}
