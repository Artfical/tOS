#include "io.h"
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
#include "serial.h"
#include "vga.h"
#include "tos_api.h"
#include "gui.h"
#include "wm.h"
#include "proc.h"
#include "vfs.h"
#include "scheduler.h"
#include "auth.h"
#include "errno.h"
#include "cmos.h"
#include "csprng.h"
#include "gdt.h"
#include "lsock.h"
#include "net.h"
#include "fsbridge.h"

/* Fixed-layout argument structs for the crypto syscalls -- mirrored
 * by hand in the SDK's tos.h (there's no shared kernel/userspace
 * header; the SDK lives in a separate repo). Keep field order/types
 * in sync if either side changes. */
/* What 32-bit tOS programs pass: their pointers are 32-bit (uint32_t here), whatever the
 * kernel's own pointer size. */
#define UP(x) ((void *)(uintptr_t)(x))
struct crypto_hash_args {
    uint32_t data;
    uint32_t len;
    uint32_t out; /* 32 bytes */
};
struct crypto_hmac_args {
    uint32_t key;
    uint32_t klen;
    uint32_t msg;
    uint32_t mlen;
    uint32_t out; /* 32 bytes */
};
struct crypto_aesctr_args {
    uint32_t key16;
    uint32_t iv16;
    uint32_t in;
    uint32_t out;
    uint32_t len;
};
struct crypto_modexp_args {
    uint32_t base256;
    uint32_t exp;
    uint32_t exp_len;
    uint32_t mod256;
    uint32_t out256;
};
struct gfx_blit_args {
    int x, y, w, h;
    uint32_t pixels; /* w*h, packed 0x00RRGGBB, row-major */
};
struct inflate_args {
    uint32_t src;
    uint32_t src_len;
    uint32_t out;
    uint32_t out_cap;
    uint32_t out_len; /* written on success */
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
    flags = irq_save();
    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
    irq_restore(flags);
}

/* set when a path was readable but longer than tOS allows: the Linux answer is ENAMETOOLONG,
 * not EFAULT (npm's cache tells "not there" from "something is wrong" by the code) */
static int cstr_too_long;

static int user_cstr_ok(uint32_t ptr)
{
    for (uint32_t i = 0; i < FS_NAME_LEN; i++) {
        if (!user_range_ok(ptr + i, 1)) return 0;
        if (((const char *)(uintptr_t)ptr)[i] == 0) return 1;
    }
    cstr_too_long = 1;
    return 0;
}

#define EXEC_VECTOR_BYTES_MAX  4096

/* Copy a user argv/envp vector into bounded kernel-owned scratch storage. */
static int copy_user_vector(uint32_t vector, const char **out, int max,
                            char *storage, uint32_t capacity, uint32_t *used)
{
    if (!vector) {
        out[0] = 0;
        return 0;
    }
    for (int i = 0; i <= max; i++) {
        uint32_t slot = vector + (uint32_t)i * sizeof(uint32_t);
        if (slot < vector || !user_range_ok(slot, sizeof(uint32_t))) return -EFAULT;
        uint32_t src = ((const uint32_t *)(uintptr_t)slot)[0];
        if (!src) {
            out[i] = 0;
            return i;
        }
        if (i == max) return -E2BIG;
        if (*used >= capacity) return -E2BIG;
        uint32_t start = *used;
        int terminated = 0;
        for (uint32_t n = 0; n < PROC_EXEC_ARG_MAX; n++) {
            uint32_t at = src + n;
            if (at < src || !user_range_ok(at, 1)) return -EFAULT;
            if (*used >= capacity) return -E2BIG;
            char c = ((const char *)(uintptr_t)src)[n];
            storage[(*used)++] = c;
            if (c == 0) { terminated = 1; break; }
        }
        if (!terminated) return -E2BIG;
        out[i] = storage + start;
    }
    return -E2BIG;
}

static int syscall_spawn_user(uint32_t path_ptr, uint32_t argv_ptr, uint32_t envp_ptr)
{
    if (!user_cstr_ok(path_ptr)) return -EFAULT;
    char path[FS_NAME_LEN];
    strncpy(path, (const char *)(uintptr_t)path_ptr, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    if (!argv_ptr && !envp_ptr) return proc_spawn(path);

    char arg_storage[EXEC_VECTOR_BYTES_MAX];
    char env_storage[EXEC_VECTOR_BYTES_MAX];
    const char *argv[PROC_EXEC_MAX_ARGS + 1];
    const char *envp[PROC_EXEC_MAX_ENV + 1];
    uint32_t arg_used = 0, env_used = 0;
    int rc = copy_user_vector(argv_ptr, argv, PROC_EXEC_MAX_ARGS,
                              arg_storage, sizeof(arg_storage), &arg_used);
    if (rc < 0) return rc;
    rc = copy_user_vector(envp_ptr, envp, PROC_EXEC_MAX_ENV,
                          env_storage, sizeof(env_storage), &env_used);
    if (rc < 0) return rc;
    return proc_spawn_argv(path, argv, envp);
}

static int syscall_exec_user(uint32_t path_ptr, uint32_t argv_ptr, uint32_t envp_ptr)
{
    if (!user_cstr_ok(path_ptr)) return -EFAULT;
    char path[FS_NAME_LEN];
    strncpy(path, (const char *)(uintptr_t)path_ptr, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    char arg_storage[EXEC_VECTOR_BYTES_MAX];
    char env_storage[EXEC_VECTOR_BYTES_MAX];
    const char *argv[PROC_EXEC_MAX_ARGS + 1];
    const char *envp[PROC_EXEC_MAX_ENV + 1];
    uint32_t arg_used = 0, env_used = 0;
    int rc = copy_user_vector(argv_ptr, argv, PROC_EXEC_MAX_ARGS,
                              arg_storage, sizeof(arg_storage), &arg_used);
    if (rc < 0) return rc;
    rc = copy_user_vector(envp_ptr, envp, PROC_EXEC_MAX_ENV,
                          env_storage, sizeof(env_storage), &env_used);
    if (rc < 0) return rc;
    return proc_execve_argv(path, argv, envp);
}

/* user descriptors 3.. map to slots of the process's own table, which hold VFS descriptors */
#define LINUX_O_CLOEXEC       0x80000u
#define LINUX_FD_CLOEXEC      1u
static int proc_vfd(proc_t *p, int fd)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return -1;
    return p->fd[fd - 3];
}

static int proc_pipe_endpoint(proc_t *p, int fd, int *is_write)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return -1;
    int v = p->fd[fd - 3];
    for (int i = 0; i < PROC_MAX_PIPES; i++) {
        if (v == PROC_FD_PIPE_READ_BASE - i) { if (is_write) *is_write = 0; return i; }
        if (v == PROC_FD_PIPE_WRITE_BASE - i) { if (is_write) *is_write = 1; return i; }
    }
    return -1;
}

static uint32_t linux_pipe_read(proc_t *p, int fd, uint32_t user_buf, uint32_t count)
{
    int is_write = 0;
    int pi = proc_pipe_endpoint(p, fd, &is_write);
    if (pi < 0 || is_write) return (uint32_t)-EBADF;
    if (count == 0) return 0;
    if (!user_wr_ok(user_buf, count)) return (uint32_t)-EFAULT;
    proc_pipe_t *pipe = &p->pipes[pi];
    if (!pipe->count) return pipe->write_refs ? (uint32_t)-EAGAIN : 0;
    uint32_t n = count < pipe->count ? count : pipe->count;
    uint8_t *dst = (uint8_t *)(uintptr_t)user_buf;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = pipe->data[pipe->head];
        pipe->head = (pipe->head + 1u) % PROC_PIPE_CAPACITY;
    }
    pipe->count -= (uint16_t)n;
    return n;
}

static uint32_t linux_pipe_write(proc_t *p, int fd, uint32_t user_buf, uint32_t count)
{
    int is_write = 0;
    int pi = proc_pipe_endpoint(p, fd, &is_write);
    if (pi < 0 || !is_write) return (uint32_t)-EBADF;
    if (count == 0) return 0;
    if (!user_range_ok(user_buf, count)) return (uint32_t)-EFAULT;
    proc_pipe_t *pipe = &p->pipes[pi];
    if (!pipe->read_refs) return (uint32_t)-EPIPE;
    uint32_t available = PROC_PIPE_CAPACITY - pipe->count;
    if (!available) return (uint32_t)-EAGAIN;
    uint32_t n = count < available ? count : available;
    const uint8_t *src = (const uint8_t *)(uintptr_t)user_buf;
    for (uint32_t i = 0; i < n; i++) {
        pipe->data[pipe->tail] = src[i];
        pipe->tail = (pipe->tail + 1u) % PROC_PIPE_CAPACITY;
    }
    pipe->count += (uint16_t)n;
    return n;
}

static uint32_t linux_pipe_create(uint32_t user_fds, uint32_t flags)
{
    proc_t *p = proc_current();
    int slots[2] = { -1, -1 };
    int pipe_index = -1;
    if (!p || !user_wr_ok(user_fds, 2 * sizeof(uint32_t))) return (uint32_t)-EFAULT;
    if (flags & ~(0x800u | LINUX_O_CLOEXEC)) return (uint32_t)-EINVAL;
    for (int i = 0; i < PROC_MAX_FDS && slots[1] < 0; i++) {
        if (p->fd[i] != -1) continue;
        if (slots[0] < 0) slots[0] = i;
        else slots[1] = i;
    }
    if (slots[1] < 0) return (uint32_t)-EMFILE;
    for (int i = 0; i < PROC_MAX_PIPES; i++)
        if (!p->pipes[i].used) { pipe_index = i; break; }
    if (pipe_index < 0) return (uint32_t)-ENFILE;

    proc_pipe_t *pipe = &p->pipes[pipe_index];
    memset(pipe, 0, sizeof(*pipe));
    pipe->used = 1;
    pipe->read_refs = pipe->write_refs = 1;
    p->fd[slots[0]] = PROC_FD_PIPE_READ_BASE - pipe_index;
    p->fd[slots[1]] = PROC_FD_PIPE_WRITE_BASE - pipe_index;
    p->fd_flags[slots[0]] = p->fd_flags[slots[1]] =
        (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fd_open_flags[slots[0]] = flags & 0x800u;
    p->fd_open_flags[slots[1]] = 1u | (flags & 0x800u);
    p->fdpath[slots[0]][0] = p->fdpath[slots[1]][0] = 0;
    p->fd_dir_offset[slots[0]] = p->fd_dir_offset[slots[1]] = 0;
    uint32_t fds[2] = { (uint32_t)slots[0] + 3u, (uint32_t)slots[1] + 3u };
    memcpy((void *)(uintptr_t)user_fds, fds, sizeof(fds));
    return 0;
}




/* ---- sockets: the fd side of kernel/net/lsock.c ---- */
#define LINUX_SOCK_NONBLOCK 0x800u
#define LINUX_MSG_PEEK      0x02u
#define LINUX_MSG_DONTWAIT  0x40u
#define LINUX_E_INPROGRESS  115
#define LINUX_E_AGAIN       11

static int sock_of(proc_t *p, int fd)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return -1;
    int v = p->fd[fd - 3];
    if (v > PROC_FD_SOCK_BASE || v <= PROC_FD_SOCK_BASE - LSOCK_MAX) return -1;
    return PROC_FD_SOCK_BASE - v;
}

static int sock_nonblock(proc_t *p, int fd, uint32_t flags)
{
    return (flags & LINUX_MSG_DONTWAIT) || (p->fd_open_flags[fd - 3] & LINUX_SOCK_NONBLOCK);
}

/* /etc/resolv.conf from what DHCP gave, so glibc's resolver asks tOS's DNS server. One query
 * at a time (single-request): tOS keeps a single waiting datagram per UDP port. */
static void write_net_files(void)
{
    char rc[96] = "nameserver ", n[12];
    for (int i = 0; i < 4; i++) {
        sprintf_u32(n, (net_dns >> (8 * i)) & 255u);
        strcat(rc, n);
        if (i < 3) strcat(rc, ".");
    }
    strcat(rc, "\noptions single-request timeout:3 attempts:2\n");
    if (!fsbridge_exists("/etc")) fsbridge_mkdir("/etc");
    if (fsbridge_exists("/etc/resolv.conf")) fsbridge_delete("/etc/resolv.conf");
    if (fsbridge_create("/etc/resolv.conf") == 0)
        fsbridge_write("/etc/resolv.conf", rc, (uint32_t)strlen(rc), 0);
    if (!fsbridge_exists("/etc/hosts") && fsbridge_create("/etc/hosts") == 0) {
        static const char hosts[] = "127.0.0.1 localhost\n";
        fsbridge_write("/etc/hosts", hosts, sizeof(hosts) - 1, 0);
    }
}

static uint32_t linux_socket(int domain, int type, int protocol)
{
    proc_t *p = proc_current();
    if (!p) return (uint32_t)-ESRCH;
    int slot = -1;
    for (int i = 0; i < PROC_MAX_FDS; i++) if (p->fd[i] == -1) { slot = i; break; }
    if (slot < 0) return (uint32_t)-EMFILE;
    int s = lsock_create(domain, type, protocol);
    if (s < 0) return (uint32_t)s;
    p->fd[slot] = PROC_FD_SOCK_BASE - s;
    p->fd_flags[slot] = (type & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fd_open_flags[slot] = 2u | (type & LINUX_SOCK_NONBLOCK);
    p->fdpath[slot][0] = 0;
    p->fd_dir_offset[slot] = 0;
    return (uint32_t)slot + 3u;
}

/* the data goes to / comes from a kernel buffer (readv/writev gather into one) */
static uint32_t sock_recv_k(proc_t *p, int fd, int s, uint8_t *buf, uint32_t len, uint32_t flags,
                            uint32_t from, uint32_t fromlen_ptr)
{
    uint8_t addr[16];
    uint32_t alen = sizeof(addr);
    int nb = sock_nonblock(p, fd, flags);
    for (;;) {
        lsock_pump();
        int r = lsock_recv(s, buf, len, addr, &alen, (flags & LINUX_MSG_PEEK) != 0);
        if (r != -LINUX_E_AGAIN || nb) {
            if (r >= 0 && from && fromlen_ptr && user_wr_ok(fromlen_ptr, 4)) {
                uint32_t cap = *(uint32_t *)(uintptr_t)fromlen_ptr;
                if (cap && user_wr_ok(from, cap)) memcpy((void *)(uintptr_t)from, addr, cap < alen ? cap : alen);
                *(uint32_t *)(uintptr_t)fromlen_ptr = alen;
            }
            return (uint32_t)r;
        }
        task_yield();
    }
}

static uint32_t sock_recv_on(proc_t *p, int fd, int s, uint32_t buf, uint32_t len, uint32_t flags,
                             uint32_t from, uint32_t fromlen_ptr)
{
    if (len && !user_wr_ok(buf, len)) return (uint32_t)-EFAULT;
    return sock_recv_k(p, fd, s, (uint8_t *)(uintptr_t)buf, len, flags, from, fromlen_ptr);
}

static uint32_t sock_send_k(proc_t *p, int fd, int s, const uint8_t *buf, uint32_t len, uint32_t flags,
                            uint32_t to, uint32_t tolen)
{
    if (to && tolen && !user_range_ok(to, tolen)) return (uint32_t)-EFAULT;
    int nb = sock_nonblock(p, fd, flags);
    for (;;) {
        lsock_pump();
        int r = lsock_send(s, buf, len, to ? (const uint8_t *)(uintptr_t)to : 0, to ? tolen : 0);
        if (r != -LINUX_E_AGAIN || nb) return (uint32_t)r;
        task_yield();
    }
}

static uint32_t sock_send_on(proc_t *p, int fd, int s, uint32_t buf, uint32_t len, uint32_t flags,
                             uint32_t to, uint32_t tolen)
{
    if (len && !user_range_ok(buf, len)) return (uint32_t)-EFAULT;
    return sock_send_k(p, fd, s, (const uint8_t *)(uintptr_t)buf, len, flags, to, tolen);
}

/* one socket call, as socketcall() numbers them; args are the call's own arguments */
static uint32_t linux_sockcall(int call, const uint32_t *v)
{
    proc_t *p = proc_current();
    if (call == 1) return linux_socket((int)v[0], (int)v[1], (int)v[2]);
    if (call == 8) return (uint32_t)-97;                         /* socketpair: AF_UNIX is not here */
    int fd = (int)v[0];
    int s = sock_of(p, fd);
    if (s < 0) return fd >= 0 && fd < 3 + PROC_MAX_FDS && (fd < 3 || p->fd[fd - 3] != -1) ? (uint32_t)-88 : (uint32_t)-EBADF;
    switch (call) {
    case 2:                                                      /* bind */
        if (!user_range_ok(v[1], v[2])) return (uint32_t)-EFAULT;
        return (uint32_t)lsock_bind(s, (const uint8_t *)(uintptr_t)v[1], v[2]);
    case 3: {                                                    /* connect */
        if (!user_range_ok(v[1], v[2])) return (uint32_t)-EFAULT;
        int r = lsock_connect_start(s, (const uint8_t *)(uintptr_t)v[1], v[2]);
        if (r != -LINUX_E_INPROGRESS || sock_nonblock(p, fd, 0)) return (uint32_t)r;
        for (;;) {
            lsock_pump();
            r = lsock_connect_check(s);
            if (r != -LINUX_E_INPROGRESS) return (uint32_t)r;
            task_yield();
        }
    }
    case 4: case 5: case 18: return (uint32_t)-95;              /* listen, accept: not yet */
    case 6: case 7: {                                            /* getsockname, getpeername */
        if (!user_wr_ok(v[2], 4)) return (uint32_t)-EFAULT;
        uint32_t cap = *(uint32_t *)(uintptr_t)v[2], alen = 16;
        uint8_t addr[16];
        int r = lsock_name(s, call == 7, addr, &alen);
        if (r < 0) return (uint32_t)r;
        if (cap && !user_wr_ok(v[1], cap)) return (uint32_t)-EFAULT;
        memcpy((void *)(uintptr_t)v[1], addr, cap < alen ? cap : alen);
        *(uint32_t *)(uintptr_t)v[2] = alen;
        return 0;
    }
    case 9:  return sock_send_on(p, fd, s, v[1], v[2], v[3], 0, 0);              /* send */
    case 10: return sock_recv_on(p, fd, s, v[1], v[2], v[3], 0, 0);              /* recv */
    case 11: return sock_send_on(p, fd, s, v[1], v[2], v[3], v[4], v[5]);        /* sendto */
    case 12: return sock_recv_on(p, fd, s, v[1], v[2], v[3], v[4], v[5]);        /* recvfrom */
    case 13: return (uint32_t)lsock_shutdown(s, (int)v[1]);
    case 14: return 0;                                           /* setsockopt: accepted */
    case 15: {                                                   /* getsockopt */
        if (!user_wr_ok(v[4], 4)) return (uint32_t)-EFAULT;
        uint32_t cap = *(uint32_t *)(uintptr_t)v[4];
        int val = 0;
        int r = lsock_getsockopt(s, (int)v[1], (int)v[2], &val);
        if (r < 0) return (uint32_t)r;
        if (cap >= 4 && user_wr_ok(v[3], 4)) { memcpy((void *)(uintptr_t)v[3], &val, 4); *(uint32_t *)(uintptr_t)v[4] = 4; }
        return 0;
    }
    case 16: case 17: case 20: {                                 /* sendmsg, recvmsg, sendmmsg */
        uint32_t mp = v[1], count = call == 20 ? v[2] : 1, flags = call == 20 ? v[3] : v[2];
        uint32_t stride = call == 20 ? 32u : 28u, done = 0;
        for (; done < count; done++) {
            uint32_t m = mp + done * stride;
            if (!user_wr_ok(m, stride)) return done ? done : (uint32_t)-EFAULT;
            const uint32_t *h = (const uint32_t *)(uintptr_t)m;          /* name, namelen, iov, iovlen, control, controllen, flags */
            uint32_t iov = h[2], iovlen = h[3], total = 0;
            if (iovlen > 64 || !user_range_ok(iov, iovlen * 8u)) return done ? done : (uint32_t)-EINVAL;
            for (uint32_t i = 0; i < iovlen; i++) total += ((const uint32_t *)(uintptr_t)iov)[2 * i + 1];
            if (total > 65536) total = 65536;
            uint8_t *tmp = malloc(total ? total : 1);
            if (!tmp) return done ? done : (uint32_t)-ENOMEM;
            uint32_t r;
            if (call == 17) {
                r = sock_recv_k(p, fd, s, tmp, total, flags | LINUX_MSG_DONTWAIT * sock_nonblock(p, fd, flags), h[0], h[0] ? m + 4 : 0);
                if ((int32_t)r > 0) {
                    uint32_t off = 0;
                    for (uint32_t i = 0; i < iovlen && off < r; i++) {
                        uint32_t base = ((const uint32_t *)(uintptr_t)iov)[2 * i], l = ((const uint32_t *)(uintptr_t)iov)[2 * i + 1];
                        if (l > r - off) l = r - off;
                        if (l && user_wr_ok(base, l)) memcpy((void *)(uintptr_t)base, tmp + off, l);
                        off += l;
                    }
                }
                ((uint32_t *)(uintptr_t)m)[5] = 0;                         /* no control data */
                ((uint32_t *)(uintptr_t)m)[6] = 0;
            } else {
                uint32_t off = 0;
                for (uint32_t i = 0; i < iovlen && off < total; i++) {
                    uint32_t base = ((const uint32_t *)(uintptr_t)iov)[2 * i], l = ((const uint32_t *)(uintptr_t)iov)[2 * i + 1];
                    if (l > total - off) l = total - off;
                    if (l && user_range_ok(base, l)) memcpy(tmp + off, (const void *)(uintptr_t)base, l);
                    off += l;
                }
                r = sock_send_k(p, fd, s, tmp, total, flags, h[0], h[1]);
            }
            free(tmp);
            if (call != 20) return r;
            if ((int32_t)r < 0) return done ? done : r;
            ((uint32_t *)(uintptr_t)m)[7] = r;                              /* mmsghdr.msg_len */
        }
        return done;
    }
    default: return (uint32_t)-EINVAL;
    }
}

/* ---- eventfd: a 64-bit counter libuv uses to wake its event loop ---- */
static proc_eventfd_t *eventfd_of(proc_t *p, int fd)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return 0;
    int v = p->fd[fd - 3];
    if (v > PROC_FD_EVENTFD_BASE || v <= PROC_FD_EVENTFD_BASE - PROC_MAX_EVENTFDS) return 0;
    proc_eventfd_t *e = &p->eventfds[PROC_FD_EVENTFD_BASE - v];
    return e->used ? e : 0;
}

static uint32_t linux_eventfd(uint32_t initval, uint32_t flags)
{
    proc_t *p = proc_current();
    if (!p) return (uint32_t)-ESRCH;
    if (flags & ~(1u | 0x800u | LINUX_O_CLOEXEC)) return (uint32_t)-EINVAL;   /* SEMAPHORE, NONBLOCK, CLOEXEC */
    int slot = -1, ei = -1;
    for (int i = 0; i < PROC_MAX_FDS; i++) if (p->fd[i] == -1) { slot = i; break; }
    for (int i = 0; i < PROC_MAX_EVENTFDS; i++) if (!p->eventfds[i].used) { ei = i; break; }
    if (slot < 0) return (uint32_t)-EMFILE;
    if (ei < 0) return (uint32_t)-ENFILE;
    p->eventfds[ei].used = 1;
    p->eventfds[ei].semaphore = flags & 1u;
    p->eventfds[ei].count = initval;
    p->fd[slot] = PROC_FD_EVENTFD_BASE - ei;
    p->fd_flags[slot] = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fd_open_flags[slot] = 2u | (flags & 0x800u);
    p->fdpath[slot][0] = 0;
    p->fd_dir_offset[slot] = 0;
    return (uint32_t)slot + 3u;
}

static uint32_t eventfd_read(proc_t *p, int fd, proc_eventfd_t *e, uint32_t buf, uint32_t count)
{
    if (count < 8) return (uint32_t)-EINVAL;
    if (!user_wr_ok(buf, 8)) return (uint32_t)-EFAULT;
    while (!e->count) {
        if (p->fd_open_flags[fd - 3] & 0x800u) return (uint32_t)-EAGAIN;
        task_yield();                            /* another thread will write it */
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    memcpy((void *)(uintptr_t)buf, &v, 8);
    return 8;
}

static uint32_t eventfd_write(proc_eventfd_t *e, uint32_t buf, uint32_t count)
{
    if (count < 8) return (uint32_t)-EINVAL;
    if (!user_range_ok(buf, 8)) return (uint32_t)-EFAULT;
    uint64_t v;
    memcpy(&v, (const void *)(uintptr_t)buf, 8);
    if (v == 0xFFFFFFFFFFFFFFFFull) return (uint32_t)-EINVAL;
    if (e->count + v < e->count || e->count + v == 0xFFFFFFFFFFFFFFFFull) return (uint32_t)-EAGAIN;
    e->count += v;
    return 8;
}

/* ---- epoll: level-triggered readiness of pipes and files ---- */
#define LINUX_EPOLLIN      0x001u
#define LINUX_EPOLLOUT     0x004u
#define LINUX_EPOLLERR     0x008u
#define LINUX_EPOLLHUP     0x010u
#define LINUX_EPOLLONESHOT (1u << 30)

static int is_epoll_fd(proc_t *p, int fd)
{
    return p && fd >= 3 && fd - 3 < PROC_MAX_FDS && p->fd[fd - 3] == PROC_FD_EPOLL;
}

/* what a descriptor is ready for right now */
static uint32_t fd_ready(proc_t *p, int fd)
{
    if (fd == 1 || fd == 2) return LINUX_EPOLLOUT;
    if (fd == 0) return 0;                       /* the console is read by blocking only */
    int is_write = 0;
    int pi = proc_pipe_endpoint(p, fd, &is_write);
    if (pi >= 0) {
        proc_pipe_t *pipe = &p->pipes[pi];
        if (!is_write) {
            uint32_t r = pipe->count ? LINUX_EPOLLIN : 0;
            if (!pipe->write_refs) r |= LINUX_EPOLLIN | LINUX_EPOLLHUP;
            return r;
        }
        if (!pipe->read_refs) return LINUX_EPOLLERR;
        return pipe->count < PROC_PIPE_CAPACITY ? LINUX_EPOLLOUT : 0;
    }
    proc_eventfd_t *ev = eventfd_of(p, fd);
    if (ev) return (ev->count ? LINUX_EPOLLIN : 0) | LINUX_EPOLLOUT;
    int sk = sock_of(p, fd);
    if (sk >= 0) return lsock_ready(sk);
    if (proc_vfd(p, fd) >= 0) return LINUX_EPOLLIN | LINUX_EPOLLOUT;   /* files never block */
    if (fd >= 3 && fd - 3 < PROC_MAX_FDS && (p->fd[fd - 3] == PROC_FD_URANDOM || p->fd[fd - 3] == PROC_FD_NULL))
        return LINUX_EPOLLIN | LINUX_EPOLLOUT;
    return 0;
}

static uint32_t linux_epoll_ctl(int epfd, int op, int fd, uint32_t user_event)
{
    proc_t *p = proc_current();
    if (!is_epoll_fd(p, epfd)) return (uint32_t)-EBADF;
    if (fd == epfd) return (uint32_t)-EINVAL;
    if (fd < 0 || (fd > 2 && (fd - 3 >= PROC_MAX_FDS || p->fd[fd - 3] == -1))) return (uint32_t)-EBADF;
    if (op < 1 || op > 3) return (uint32_t)-EINVAL;
    uint32_t ev[3] = { 0, 0, 0 };
    if (op != 2) {
        if (!user_range_ok(user_event, 12)) return (uint32_t)-EFAULT;
        memcpy(ev, (const void *)(uintptr_t)user_event, 12);   /* struct epoll_event is packed on i386: events, data */
    }
    int found = -1, free_slot = -1;
    for (int i = 0; i < PROC_MAX_EPOLL_WATCH; i++) {
        proc_epoll_watch_t *w = &p->epoll_watch[i];
        if (w->epfd == epfd && w->fd == fd) found = i;
        if (!w->epfd && free_slot < 0) free_slot = i;
    }
    if (op == 1) {                                /* EPOLL_CTL_ADD */
        if (found >= 0) return (uint32_t)-EEXIST;
        if (free_slot < 0) return (uint32_t)-ENOSPC;
        found = free_slot;
    } else if (found < 0) return (uint32_t)-ENOENT;
    proc_epoll_watch_t *w = &p->epoll_watch[found];
    if (op == 2) { w->epfd = 0; return 0; }       /* EPOLL_CTL_DEL */
    w->epfd = epfd; w->fd = fd;
    w->events = ev[0]; w->data_lo = ev[1]; w->data_hi = ev[2];
    return 0;
}

/* poll(): the same readiness as epoll, for an array of struct pollfd (fd, events, revents) */
static uint32_t linux_poll(uint32_t user_fds, uint32_t nfds, int timeout_ms)
{
    proc_t *p = proc_current();
    if (nfds > 1024) return (uint32_t)-EINVAL;
    if (nfds && !user_wr_ok(user_fds, nfds * 8u)) return (uint32_t)-EFAULT;
    uint64_t deadline = debugmon_uptime_ms64() + (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
    for (;;) {
        lsock_pump();
        uint32_t n = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            uint8_t *pf = (uint8_t *)(uintptr_t)(user_fds + i * 8u);
            int fd;
            uint16_t events, rev = 0;
            memcpy(&fd, pf, 4);
            memcpy(&events, pf + 4, 2);
            if (fd >= 0) {
                if (fd > 2 && (fd - 3 >= PROC_MAX_FDS || p->fd[fd - 3] == -1)) rev = 0x20;   /* POLLNVAL */
                else rev = (uint16_t)(fd_ready(p, fd) & (events | LINUX_EPOLLERR | LINUX_EPOLLHUP));
            }
            memcpy(pf + 6, &rev, 2);
            if (rev) n++;
        }
        if (n || timeout_ms == 0) return n;
        if (timeout_ms > 0 && debugmon_uptime_ms64() >= deadline) return 0;
        task_yield();
    }
}

static uint32_t linux_epoll_wait(int epfd, uint32_t user_events, int maxevents, int timeout_ms)
{
    proc_t *p = proc_current();
    if (!is_epoll_fd(p, epfd)) return (uint32_t)-EBADF;
    if (maxevents <= 0 || maxevents > 4096) return (uint32_t)-EINVAL;
    if (!user_wr_ok(user_events, (uint32_t)maxevents * 12u)) return (uint32_t)-EFAULT;
    uint64_t deadline = debugmon_uptime_ms64() + (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
    for (;;) {
        lsock_pump();
        int n = 0;
        for (int i = 0; i < PROC_MAX_EPOLL_WATCH && n < maxevents; i++) {
            proc_epoll_watch_t *w = &p->epoll_watch[i];
            if (w->epfd != epfd) continue;
            uint32_t got = fd_ready(p, w->fd) & (w->events | LINUX_EPOLLERR | LINUX_EPOLLHUP);
            if (!got) continue;
            uint32_t ev[3] = { got, w->data_lo, w->data_hi };
            memcpy((void *)(uintptr_t)(user_events + (uint32_t)n * 12u), ev, 12);
            n++;
            if (w->events & LINUX_EPOLLONESHOT) w->events = LINUX_EPOLLONESHOT;   /* disarmed until EPOLL_CTL_MOD */
        }
        if (n || timeout_ms == 0) return (uint32_t)n;
        if (timeout_ms > 0 && debugmon_uptime_ms64() >= deadline) return 0;
        task_yield();                            /* a thread that writes the pipe gets to run */
    }
}


/* ---- ioctl and resource limits: what glibc, libuv and V8 ask at start-up ---- */
static uint32_t linux_ioctl(int fd, uint32_t req, uint32_t arg)
{
    proc_t *p = proc_current();
    int slot = fd - 3;
    if (fd < 0 || (fd > 2 && (!p || slot >= PROC_MAX_FDS || p->fd[slot] == -1))) return (uint32_t)-EBADF;
    switch (req) {
    case 0x5421:                                 /* FIONBIO */
        if (!user_range_ok(arg, 4)) return (uint32_t)-EFAULT;
        if (fd > 2) {
            if (*(const int *)(uintptr_t)arg) p->fd_open_flags[slot] |= 0x800u;
            else p->fd_open_flags[slot] &= ~0x800u;
        }
        return 0;
    case 0x541B: {                               /* FIONREAD */
        if (!user_wr_ok(arg, 4)) return (uint32_t)-EFAULT;
        int sk = sock_of(p, fd);
        int n = sk >= 0 ? lsock_pending(sk) : 0;
        if (sk < 0 && fd > 2) {
            int is_write = 0, pi = proc_pipe_endpoint(p, fd, &is_write);
            if (pi >= 0 && !is_write) n = p->pipes[pi].count;
        }
        memcpy((void *)(uintptr_t)arg, &n, 4);
        return 0;
    }
    case 0x5451:                                 /* FIOCLEX */
        if (fd > 2) p->fd_flags[slot] |= LINUX_FD_CLOEXEC;
        return 0;
    case 0x5450:                                 /* FIONCLEX */
        if (fd > 2) p->fd_flags[slot] &= ~LINUX_FD_CLOEXEC;
        return 0;
    default:                                     /* TCGETS, TIOCGWINSZ...: the console is not a Linux tty */
        return (uint32_t)-ENOTTY;
    }
}

/* getrlimit/prlimit64: an 8 MB stack and the descriptor table's size; the rest unlimited.
 * New limits are accepted and not enforced. */
static uint32_t linux_rlimit(int resource, uint32_t new_ptr, uint32_t old_ptr, int wide)
{
    if (resource < 0 || resource >= 16) return (uint32_t)-EINVAL;
    uint32_t size = wide ? 16u : 8u;
    if (new_ptr && !user_range_ok(new_ptr, size)) return (uint32_t)-EFAULT;
    if (!old_ptr) return 0;
    if (!user_wr_ok(old_ptr, size)) return (uint32_t)-EFAULT;
    uint32_t cur = 0xFFFFFFFFu, max = 0xFFFFFFFFu;
    if (resource == 3) cur = 8u << 20;                                   /* RLIMIT_STACK */
    if (resource == 7) cur = max = PROC_MAX_FDS + 3;                     /* RLIMIT_NOFILE */
    if (wide) {
        uint32_t v[4] = { cur, cur == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0, max, max == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0 };
        memcpy((void *)(uintptr_t)old_ptr, v, 16);
    } else {
        uint32_t v[2] = { cur, max };
        memcpy((void *)(uintptr_t)old_ptr, v, 8);
    }
    return 0;
}

#define LINUX_PROT_READ      0x1u
#define LINUX_PROT_WRITE     0x2u
#define LINUX_PROT_EXEC      0x4u
#define LINUX_MAP_SHARED     0x01u
#define LINUX_MAP_PRIVATE    0x02u
#define LINUX_MAP_FIXED      0x10u
#define LINUX_MAP_ANONYMOUS  0x20u
#define LINUX_F_GETFD         1
#define LINUX_F_SETFD         2
#define LINUX_F_GETFL         3
#define LINUX_F_SETFL         4
#define TOS_MMAP_BASE        PROC_BRK_LIMIT
#define TOS_MMAP_END          (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

static int proc_vm_region_contains(proc_t *p, uint32_t page)
{
    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++)
        if (p->vm_regions[i].used && page >= p->vm_regions[i].start &&
            page < p->vm_regions[i].end) return 1;
    return 0;
}

static int proc_vm_region_overlaps(proc_t *p, uint32_t start, uint32_t end)
{
    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++)
        if (p->vm_regions[i].used && start < p->vm_regions[i].end &&
            end > p->vm_regions[i].start) return 1;
    return 0;
}

static int proc_vm_region_add(proc_t *p, uint32_t start, uint32_t end)
{
    if (proc_vm_region_overlaps(p, start, end)) return -1;
    /* a region right next to an existing one joins it: V8 maps its heap a page group at a time
     * and frees parts of it again, which used to fill the table with neighbours */
    int joined = -1;
    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++) {
        proc_vm_region_t *r = &p->vm_regions[i];
        if (!r->used || (r->end != start && r->start != end)) continue;
        if (joined < 0) {
            if (r->end == start) r->end = end; else r->start = start;
            joined = i;
        } else {
            proc_vm_region_t *j = &p->vm_regions[joined];
            if (r->start < j->start) j->start = r->start;
            if (r->end > j->end) j->end = r->end;
            r->used = 0;
            break;
        }
    }
    if (joined >= 0) return 0;
    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++) {
        if (p->vm_regions[i].used) continue;
        p->vm_regions[i].start = start;
        p->vm_regions[i].end = end;
        p->vm_regions[i].used = 1;
        return 0;
    }
    return -1;
}

/* Removes/splits VMA metadata without changing page-table contents. */
static int proc_vm_region_remove(proc_t *p, uint32_t start, uint32_t end)
{
    int free_slots = 0, splits_needed = 0;
    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++) {
        proc_vm_region_t *r = &p->vm_regions[i];
        if (!r->used) { free_slots++; continue; }
        if (start < r->end && end > r->start && start > r->start && end < r->end)
            splits_needed++;
    }
    if (splits_needed > free_slots) return -1;

    for (int i = 0; i < PROC_MAX_VM_REGIONS; i++) {
        proc_vm_region_t *r = &p->vm_regions[i];
        if (!r->used || start >= r->end || end <= r->start) continue;
        if (start <= r->start && end >= r->end) {
            r->used = 0;
        } else if (start <= r->start) {
            r->start = end;
        } else if (end >= r->end) {
            r->end = start;
        } else {
            for (int j = 0; j < PROC_MAX_VM_REGIONS; j++) {
                if (p->vm_regions[j].used) continue;
                p->vm_regions[j].start = end;
                p->vm_regions[j].end = r->end;
                p->vm_regions[j].used = 1;
                r->end = start;
                break;
            }
        }
    }
    return 0;
}

static int proc_mmap_range_free(proc_t *p, uint32_t start, uint32_t end)
{
    if (proc_vm_region_overlaps(p, start, end)) return 0;
    for (uint32_t page = start; page < end; page += PAGE_SIZE)
        if (paging_virt_to_phys(p->pgdir, page) != 0) return 0;
    return 1;
}

static void proc_mmap_unmap_pages(proc_t *p, uint32_t start, uint32_t end)
{
    for (uint32_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t phys = 0;
        if (paging_unmap_in(p->pgdir, page, &phys) > 0 && phys)
            free_physical_page(phys);
    }
}

static uint32_t syscall_mmap2(uint32_t hint, uint32_t len, uint32_t prot,
                              uint32_t flags, uint32_t fd, uint32_t page_offset)
{
    proc_t *p = proc_current();
    if (!p || !p->pgdir || !len || (prot & ~7u)) return (uint32_t)-EINVAL;
    if (!(flags & (LINUX_MAP_PRIVATE | LINUX_MAP_SHARED))) return (uint32_t)-EINVAL;
    if (!(flags & LINUX_MAP_ANONYMOUS)) return (uint32_t)-ENOSYS;
    if (page_offset != 0) return (uint32_t)-EINVAL;
    (void)fd;

    if (len > UINT32_MAX - (PAGE_SIZE - 1u)) return (uint32_t)-ENOMEM;
    uint32_t size = (len + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (!size || size > TOS_MMAP_END - TOS_MMAP_BASE) return (uint32_t)-ENOMEM;

    uint32_t start = 0;
    if (flags & LINUX_MAP_FIXED) {
        if ((hint & (PAGE_SIZE - 1u)) != 0 || hint < TOS_MMAP_BASE ||
            (uint64_t)hint + size > TOS_MMAP_END) return (uint32_t)-EINVAL;
        start = hint;
        if (proc_vm_region_remove(p, start, start + size) != 0)
            return (uint32_t)-ENOMEM;
        proc_mmap_unmap_pages(p, start, start + size);
    } else {
        if (hint) {
            uint32_t candidate = hint & ~(PAGE_SIZE - 1u);
            if (candidate >= TOS_MMAP_BASE && (uint64_t)candidate + size <= TOS_MMAP_END &&
                proc_mmap_range_free(p, candidate, candidate + size)) start = candidate;
        }
        if (!start) {
            for (uint32_t candidate = TOS_MMAP_BASE;
                 (uint64_t)candidate + size <= TOS_MMAP_END;
                 candidate += PAGE_SIZE) {
                if (proc_mmap_range_free(p, candidate, candidate + size)) {
                    start = candidate;
                    break;
                }
            }
        }
        if (!start) return (uint32_t)-ENOMEM;
    }

    uint32_t end = start + size;
    if (proc_vm_region_add(p, start, end) != 0) return (uint32_t)-ENOMEM;

    /* PROT_NONE reserves virtual space without consuming physical pages. */
    if (prot != 0) {
        uint32_t pte_flags = PTE_USER | ((prot & LINUX_PROT_WRITE) ? PTE_WRITABLE : 0);
        for (uint32_t page = start; page < end; page += PAGE_SIZE) {
            uint32_t phys = alloc_physical_page();
            if (!phys) {
                proc_mmap_unmap_pages(p, start, page);
                proc_vm_region_remove(p, start, end);
                return (uint32_t)-ENOMEM;
            }
            memset(P2V(phys), 0, PAGE_SIZE);
            if (paging_map_in(p->pgdir, page, phys, pte_flags) != 0) {
                free_physical_page(phys);
                proc_mmap_unmap_pages(p, start, page);
                proc_vm_region_remove(p, start, end);
                return (uint32_t)-ENOMEM;
            }
        }
    }
    return start;
}

static uint32_t syscall_munmap(uint32_t addr, uint32_t len)
{
    proc_t *p = proc_current();
    if (!p || !p->pgdir || !len || (addr & (PAGE_SIZE - 1u)) ||
        len > UINT32_MAX - (PAGE_SIZE - 1u)) return (uint32_t)-EINVAL;
    uint32_t size = (len + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint64_t end64 = (uint64_t)addr + size;
    if (addr < TOS_MMAP_BASE || end64 > TOS_MMAP_END) return (uint32_t)-EINVAL;
    uint32_t end = (uint32_t)end64;
    if (proc_vm_region_remove(p, addr, end) != 0) return (uint32_t)-ENOMEM;
    proc_mmap_unmap_pages(p, addr, end);
    return 0;
}

static uint32_t syscall_mprotect(uint32_t addr, uint32_t len, uint32_t prot)
{
    proc_t *p = proc_current();
    if (!p || !p->pgdir || !len || (addr & (PAGE_SIZE - 1u)) || (prot & ~7u) ||
        len > UINT32_MAX - (PAGE_SIZE - 1u)) return (uint32_t)-EINVAL;
    uint32_t size = (len + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint64_t end64 = (uint64_t)addr + size;
    if (addr < PAGING_USER_START || end64 > PAGING_USER_END) return (uint32_t)-EINVAL;
    uint32_t end = (uint32_t)end64;

    for (uint32_t page = addr; page < end; page += PAGE_SIZE) {
        if (paging_virt_to_phys(p->pgdir, page) == 0 &&
            !proc_vm_region_contains(p, page)) return (uint32_t)-ENOMEM;
    }
    uint32_t pte_flags = prot ? PTE_USER : 0;
    if (prot & LINUX_PROT_WRITE) pte_flags |= PTE_WRITABLE;
    for (uint32_t page = addr; page < end; page += PAGE_SIZE) {
        uint32_t phys = paging_virt_to_phys(p->pgdir, page);
        if (!phys && prot) {
            phys = alloc_physical_page();
            if (!phys) return (uint32_t)-ENOMEM;
            memset(P2V(phys), 0, PAGE_SIZE);
            if (paging_map_in(p->pgdir, page, phys, pte_flags) != 0) {
                free_physical_page(phys);
                return (uint32_t)-ENOMEM;
            }
        } else if (phys && paging_protect_in(p->pgdir, page, pte_flags) != 0) {
            return (uint32_t)-ENOMEM;
        }
    }
    return 0;
}

static uint32_t linux_sys_brk(uint32_t requested)
{
    proc_t *p = proc_current();
    if (!p || !p->pgdir) return 0;
    if (!requested) return p->brk;
    if (requested < p->brk_base || requested > PROC_BRK_LIMIT) return p->brk;

    uint32_t old = p->brk;
    uint32_t old_page = (old + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint32_t new_page = (requested + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (new_page > old_page) {
        uint32_t page = old_page;
        for (; page < new_page; page += PAGE_SIZE) {
            uint32_t phys = alloc_physical_page();
            if (!phys) break;
            memset(P2V(phys), 0, PAGE_SIZE);
            if (paging_map_in(p->pgdir, page, phys, PTE_USER | PTE_WRITABLE) != 0) {
                free_physical_page(phys);
                break;
            }
        }
        if (page < new_page) {
            while (page > old_page) {
                page -= PAGE_SIZE;
                uint64_t phys = 0;
                if (paging_unmap_in(p->pgdir, page, &phys) > 0 && phys)
                    free_physical_page(phys);
            }
            return old;
        }
    } else if (new_page < old_page) {
        for (uint32_t page = new_page; page < old_page; page += PAGE_SIZE) {
            uint64_t phys = 0;
            if (paging_unmap_in(p->pgdir, page, &phys) > 0 && phys)
                free_physical_page(phys);
        }
    }
    p->brk = requested;
    return requested;
}

typedef struct { int32_t sec, nsec; } linux_timespec32_t;
typedef struct { int64_t sec, nsec; } linux_timespec64_t;
typedef struct { int32_t sec, usec; } linux_timeval32_t;
typedef struct { uint32_t base, len; } linux_iovec32_t;
typedef struct {
    char sysname[65], nodename[65], release[65];
    char version[65], machine[65], domainname[65];
} linux_utsname_t;
typedef struct {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
} linux_sigaction32_t;
typedef struct __attribute__((packed)) {
    uint64_t st_dev;
    uint32_t pad1;
    uint32_t ino32;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t pad2;
    int64_t st_size;
    int32_t st_blksize;
    int64_t st_blocks;
    int32_t atime_sec;
    uint32_t atime_nsec;
    int32_t mtime_sec;
    uint32_t mtime_nsec;
    int32_t ctime_sec;
    uint32_t ctime_nsec;
    uint64_t st_ino;
} linux_stat64_t;

static void linux_stat64_fill(linux_stat64_t *st, const vfs_entry_t *entry)
{
    memset(st, 0, sizeof(*st));
    if (!entry) {
        st->st_mode = 0x2000; /* character device: console */
        st->st_nlink = 1;
        st->st_blksize = 512;
        return;
    }
    st->ino32 = entry->inode;
    st->st_ino = entry->inode;
    uint32_t type = entry->mode & 0170000u;
    if (type != 0x1000u && type != 0x2000u && type != 0x4000u &&
        type != 0x6000u && type != 0x8000u && type != 0xA000u && type != 0xC000u)
        type = entry->is_dir ? 0x4000u : 0x8000u;
    st->st_mode = type | (entry->mode & 07777u);
    st->st_nlink = 1;
    st->st_uid = entry->uid;
    st->st_gid = entry->gid;
    st->st_size = entry->size;
    st->st_blksize = 4096;
    st->st_blocks = (entry->size + 511u) / 512u;
}

/* fstat of a descriptor, whatever it is: the console, a device, a pipe, an epoll, or a file */
static int linux_fd_stat(int fd, linux_stat64_t *st)
{
    if (fd >= 0 && fd <= 2) { linux_stat64_fill(st, 0); return 0; }
    proc_t *p = proc_current();
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS || p->fd[fd - 3] == -1) return -EBADF;
    int v = p->fd[fd - 3];
    if (v == PROC_FD_URANDOM || v == PROC_FD_NULL) {
        /* a character device with Linux's numbers: 1,3 null, 1,8 random, 1,9 urandom */
        linux_stat64_fill(st, 0);
        st->st_mode = 0x2000u | 0666u;
        uint32_t minor = v == PROC_FD_NULL ? 3u : p->fdpath[fd - 3][5] == 'r' ? 8u : 9u;
        st->st_rdev = (1u << 8) | minor;
        st->st_ino = st->ino32 = 100u + minor;
        return 0;
    }
    if (sock_of(p, fd) >= 0) {
        linux_stat64_fill(st, 0);
        st->st_mode = 0140000u | 0777u;
        st->st_rdev = 0;
        st->st_ino = st->ino32 = 300u + (uint32_t)fd;
        return 0;
    }
    if (proc_pipe_endpoint(p, fd, 0) >= 0 || v == PROC_FD_EPOLL || eventfd_of(p, fd)) {
        linux_stat64_fill(st, 0);
        st->st_mode = proc_pipe_endpoint(p, fd, 0) >= 0 ? (0x1000u | 0600u) : 0600u;   /* epoll, eventfd: anonymous inodes */
        st->st_rdev = 0;
        st->st_ino = st->ino32 = 200u + (uint32_t)fd;
        return 0;
    }
    vfs_entry_t entry;
    if (vfs_stat(p->fdpath[fd - 3], &entry) != 0) return vfs_denied() ? -EACCES : -ENOENT;
    linux_stat64_fill(st, &entry);
    return 0;
}

static uint32_t linux_getdents64(uint32_t fd, uint32_t user_buf, uint32_t count)
{
    proc_t *p = proc_current();
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS) return (uint32_t)-EBADF;
    int slot = (int)fd - 3;
    if (p->fd[slot] != PROC_FD_DIRECTORY) return (uint32_t)-ENOTDIR;
    if (count < 24 || count > 1024u * 1024u) return (uint32_t)-EINVAL;
    if (!user_wr_ok(user_buf, count)) return (uint32_t)-EFAULT;

    uint32_t offset = p->fd_dir_offset[slot];
    uint32_t max_entries = offset + 128;
    if (max_entries < offset || max_entries > 2048) max_entries = 2048;
    if (max_entries <= offset) return 0;
    vfs_entry_t *entries = (vfs_entry_t *)malloc(max_entries * sizeof(vfs_entry_t));
    if (!entries) return (uint32_t)-ENOMEM;
    int n = vfs_readdir(p->fdpath[slot], entries, (int)max_entries);
    if (n < 0) { free(entries); return (uint32_t)-EIO; }
    if (offset >= (uint32_t)n) { free(entries); return 0; }

    uint32_t written = 0, next = offset;
    for (uint32_t i = offset; i < (uint32_t)n; i++) {
        uint32_t namelen = 0;
        while (namelen < VFS_NAME_LEN && entries[i].name[namelen]) namelen++;
        uint32_t reclen = (19u + namelen + 1u + 7u) & ~7u;
        if (reclen > count - written) break;

        uint8_t *record = (uint8_t *)(uintptr_t)(user_buf + written);
        uint64_t ino = entries[i].inode;
        int64_t d_off = (int64_t)i + 1;
        uint16_t d_reclen = (uint16_t)reclen;
        uint8_t d_type = entries[i].is_dir ? 4 :
                         ((entries[i].mode & 0170000u) == 0120000u ? 10 : 8);
        memset(record, 0, reclen);
        memcpy(record, &ino, sizeof(ino));
        memcpy(record + 8, &d_off, sizeof(d_off));
        memcpy(record + 16, &d_reclen, sizeof(d_reclen));
        record[18] = d_type;
        memcpy(record + 19, entries[i].name, namelen);
        record[19 + namelen] = 0;
        written += reclen;
        next = i + 1;
    }
    free(entries);
    if (!written) return (uint32_t)-EINVAL;
    p->fd_dir_offset[slot] = next;
    return written;
}

static uint32_t linux_reopen_proc_fd(proc_t *p, int source_fd, uint32_t flags)
{
    if (!p || source_fd < 3 || source_fd - 3 >= PROC_MAX_FDS) return (uint32_t)-EBADF;
    int src = source_fd - 3;
    int kind = p->fd[src];
    if (kind == -1) return (uint32_t)-EBADF;
    int dst = -1;
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fd[i] == -1) { dst = i; break; }
    if (dst < 0) return (uint32_t)-EMFILE;

    int is_write = 0;
    int pipe_index = proc_pipe_endpoint(p, source_fd, &is_write);
    if (pipe_index >= 0) {
        if (is_write) {
            if (p->pipes[pipe_index].write_refs == UINT16_MAX) return (uint32_t)-EMFILE;
            p->pipes[pipe_index].write_refs++;
        } else {
            if (p->pipes[pipe_index].read_refs == UINT16_MAX) return (uint32_t)-EMFILE;
            p->pipes[pipe_index].read_refs++;
        }
        p->fd[dst] = kind;
    } else if (kind == PROC_FD_EPOLL || kind == PROC_FD_DIRECTORY) {
        p->fd[dst] = kind;
        p->fd_dir_offset[dst] = p->fd_dir_offset[src];
    } else if (kind >= 0) {
        int vflags = (flags & 3u) == 2 ? VFS_RDWR :
                     (flags & 3u) == 1 ? VFS_WRONLY : VFS_RDONLY;
        if (flags & 0x40u) vflags |= VFS_CREAT;
        if (flags & 0x200u) vflags |= VFS_TRUNC;
        if (flags & 0x400u) vflags |= VFS_APPEND;
        int new_vfd = vfs_open(p->fdpath[src], vflags);
        if (new_vfd < 0) return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
        p->fd[dst] = new_vfd;
        strncpy(p->fdpath[dst], p->fdpath[src], VFS_NAME_LEN - 1);
        p->fdpath[dst][VFS_NAME_LEN - 1] = 0;
    } else {
        return (uint32_t)-EBADF;
    }
    p->fd_flags[dst] = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fd_open_flags[dst] = flags & ~LINUX_O_CLOEXEC;
    p->fdpath[dst][VFS_NAME_LEN - 1] = 0;
    return (uint32_t)(dst + 3);
}

static uint32_t linux_open_path(uint32_t path_ptr, uint32_t flags, uint32_t mode,
                                int32_t dirfd)
{
    proc_t *p = proc_current();
    if (!p || !user_cstr_ok(path_ptr)) return (uint32_t)-EFAULT;
    char path[FS_NAME_LEN];
    strncpy(path, (const char *)(uintptr_t)path_ptr, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    if (!path[0]) return (uint32_t)-ENOENT;

    static const char proc_fd_prefix[] = "/proc/self/fd/";
    if (strncmp(path, proc_fd_prefix, sizeof(proc_fd_prefix) - 1) == 0) {
        const char *s = path + sizeof(proc_fd_prefix) - 1;
        if (!*s) return (uint32_t)-ENOENT;
        uint32_t fd = 0;
        while (*s >= '0' && *s <= '9') {
            uint32_t digit = (uint32_t)(*s++ - '0');
            if (fd > (UINT32_MAX - digit) / 10u) return (uint32_t)-EBADF;
            fd = fd * 10u + digit;
        }
        if (*s) return (uint32_t)-ENOENT;
        return linux_reopen_proc_fd(p, (int)fd, flags);
    }
    /* glibc reads these before it opens any socket: written from the DHCP lease when asked for */
    if (!strcmp(path, "/etc/resolv.conf") || !strcmp(path, "/etc/hosts")) write_net_files();

    /* the few devices programs expect: OpenSSL waits for /dev/urandom before it will start */
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random") || !strcmp(path, "/dev/null")) {
        int dslot = -1;
        for (int i = 0; i < PROC_MAX_FDS; i++)
            if (p->fd[i] == -1) { dslot = i; break; }
        if (dslot < 0) return (uint32_t)-EMFILE;
        p->fd[dslot] = path[5] == 'n' ? PROC_FD_NULL : PROC_FD_URANDOM;
        p->fd_flags[dslot] = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
        p->fd_open_flags[dslot] = flags & ~LINUX_O_CLOEXEC;
        strcpy(p->fdpath[dslot], path);
        p->fd_dir_offset[dslot] = 0;
        return (uint32_t)dslot + 3u;
    }
    if (path[0] != '/' && dirfd != -100) {
        if (dirfd < 3 || dirfd - 3 >= PROC_MAX_FDS) return (uint32_t)-EBADF;
        int slot = dirfd - 3;
        if (p->fd[slot] == -1) return (uint32_t)-EBADF;
        if (p->fd[slot] != PROC_FD_DIRECTORY) return (uint32_t)-ENOTDIR;
        char joined[FS_NAME_LEN];
        uint32_t base_len = (uint32_t)strlen(p->fdpath[slot]);
        uint32_t tail_len = (uint32_t)strlen(path);
        if (base_len + tail_len + 2u > sizeof(joined)) return (uint32_t)-ENAMETOOLONG;
        memcpy(joined, p->fdpath[slot], base_len);
        if (base_len && joined[base_len - 1] != '/') joined[base_len++] = '/';
        memcpy(joined + base_len, path, tail_len + 1u);
        strcpy(path, joined);
    }

    char absolute_path[FS_NAME_LEN];
    strncpy(absolute_path, vfs_abspath(path), sizeof(absolute_path) - 1);
    absolute_path[sizeof(absolute_path) - 1] = 0;
    int slot = -1;
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fd[i] == -1) { slot = i; break; }
    if (slot < 0) return (uint32_t)-EMFILE;

    vfs_entry_t path_stat;
    int is_dir = vfs_stat(path, &path_stat) == 0 && path_stat.is_dir;
    if (is_dir) {
        if ((flags & 3u) != 0 || (flags & 0x200u) || (flags & 0x40u))
            return (uint32_t)-EISDIR;
        if (vfs_access(path, VFS_ACC_R) != 0) return (uint32_t)-EACCES;
        p->fd[slot] = PROC_FD_DIRECTORY;
        strncpy(p->fdpath[slot], absolute_path, VFS_NAME_LEN - 1);
        p->fdpath[slot][VFS_NAME_LEN - 1] = 0;
        p->fd_dir_offset[slot] = 0;
    } else {
        if (flags & 0x10000u) return (uint32_t)-ENOTDIR; /* O_DIRECTORY */
        int vflags = (flags & 3u) == 2 ? VFS_RDWR :
                     (flags & 3u) == 1 ? VFS_WRONLY : VFS_RDONLY;
        if (flags & 0x40u) vflags |= VFS_CREAT;
        if (flags & 0x200u) vflags |= VFS_TRUNC;
        if (flags & 0x400u) vflags |= VFS_APPEND;
        (void)mode;
        int vfd = vfs_open(path, vflags);
        if (vfd < 0)
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
        p->fd[slot] = vfd;
        if (vfs_fd_get_path(vfd, p->fdpath[slot], VFS_NAME_LEN) < 0)
            strncpy(p->fdpath[slot], absolute_path, VFS_NAME_LEN - 1);
        p->fdpath[slot][VFS_NAME_LEN - 1] = 0;
    }
    p->fd_flags[slot] = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fd_open_flags[slot] = flags & ~LINUX_O_CLOEXEC;
    return (uint32_t)(slot + 3);
}

static int linux_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int linux_clock_cmos(int64_t *sec);

/* MONOTONIC (1), its RAW (4), COARSE (6) and BOOTTIME (7) count from boot; the CPU-time clocks
 * (2, 3) are approximated by it. REALTIME (0) and its COARSE (5): the CMOS time at the first
 * call, carried forward in milliseconds (the CMOS clock itself only has whole seconds, and
 * Date.now() measuring a 50 ms timer as 0 ms broke programs). */
static int linux_clock_now(uint32_t clock_id, int64_t *sec, int32_t *nsec)
{
    if (clock_id == 0 || clock_id == 5) {
        static int64_t boot_epoch_ms = -1;
        if (boot_epoch_ms < 0) {
            int64_t now_s;
            if (linux_clock_cmos(&now_s) != 0) return -1;
            boot_epoch_ms = now_s * 1000 - (int64_t)debugmon_uptime_ms64();
        }
        int64_t ms = boot_epoch_ms + (int64_t)debugmon_uptime_ms64();
        *sec = ms / 1000;
        *nsec = (int32_t)((ms % 1000) * 1000000);
        return 0;
    }
    if (clock_id == 1 || clock_id == 2 || clock_id == 3 || clock_id == 4 || clock_id == 6 || clock_id == 7) {
        uint64_t ms = debugmon_uptime_ms64();
        *sec = (int64_t)(ms / 1000u);
        *nsec = (int32_t)((ms % 1000u) * 1000000u);
        return 0;
    }
    return -1;
}

static int linux_clock_cmos(int64_t *sec)
{
    cmos_time_t t;
    if (cmos_get_time(&t) != 0 || t.year < 1970 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour < 0 || t.hour > 23 ||
        t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 60) return -1;
    static const uint8_t month_days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    uint64_t days = 0;
    for (int y = 1970; y < t.year; y++) days += linux_leap_year(y) ? 366 : 365;
    for (int m = 1; m < t.month; m++) {
        days += month_days[m - 1];
        if (m == 2 && linux_leap_year(t.year)) days++;
    }
    days += (uint32_t)t.day - 1u;
    *sec = (int64_t)(days * 86400u + (uint32_t)t.hour * 3600u +
                     (uint32_t)t.minute * 60u + (uint32_t)t.second);
    return 0;
}

static uint32_t linux_nanosleep_on(uint32_t clock_id, int absolute, uint32_t req_ptr, uint32_t rem_ptr, int time64);

static uint32_t linux_nanosleep(uint32_t req_ptr, uint32_t rem_ptr, int time64)
{
    return linux_nanosleep_on(1, 0, req_ptr, rem_ptr, time64);
}

/* clock_nanosleep: relative, or until the clock reads the given time (TIMER_ABSTIME) */
static uint32_t linux_nanosleep_on(uint32_t clock_id, int absolute, uint32_t req_ptr, uint32_t rem_ptr, int time64)
{
    int64_t sec;
    int64_t nsec;
    if (time64) {
        linux_timespec64_t req;
        if (!user_range_ok(req_ptr, sizeof(req))) return (uint32_t)-EFAULT;
        memcpy(&req, (const void *)(uintptr_t)req_ptr, sizeof(req));
        sec = req.sec;
        nsec = req.nsec;
    } else {
        linux_timespec32_t req;
        if (!user_range_ok(req_ptr, sizeof(req))) return (uint32_t)-EFAULT;
        memcpy(&req, (const void *)(uintptr_t)req_ptr, sizeof(req));
        sec = req.sec;
        nsec = req.nsec;
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000ll || sec > 8640000ll)
        return (uint32_t)-EINVAL;
    if (rem_ptr) {
        uint32_t rem_size = time64 ? sizeof(linux_timespec64_t) : sizeof(linux_timespec32_t);
        if (!user_wr_ok(rem_ptr, rem_size)) return (uint32_t)-EFAULT;
        memset((void *)(uintptr_t)rem_ptr, 0, rem_size);
    }
    if (absolute) {
        int64_t now_s;
        int32_t now_ns;
        if (linux_clock_now(clock_id, &now_s, &now_ns) != 0) return (uint32_t)-EINVAL;
        int64_t left = (sec - now_s) * 1000000000ll + (nsec - now_ns);
        if (left <= 0) return 0;
        sec = left / 1000000000ll;
        nsec = left % 1000000000ll;
    }
    uint64_t ms = (uint64_t)sec * 1000u + ((uint64_t)nsec + 999999u) / 1000000u;
    uint64_t deadline = debugmon_uptime_ms64() + ms;
    while (debugmon_uptime_ms64() < deadline) task_yield();
    return 0;
}

typedef struct {
    int32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
} linux_user_desc_t;

static uint32_t linux_set_thread_area(uint32_t user_desc_ptr)
{
    task_t *t = task_current();
    linux_user_desc_t desc;
    if (!t || !user_wr_ok(user_desc_ptr, sizeof(desc))) return (uint32_t)-EFAULT;
    memcpy(&desc, (const void *)(uintptr_t)user_desc_ptr, sizeof(desc));
    if ((desc.flags & ~0xFFu) || (desc.flags & 0x80u) || desc.limit > 0xFFFFFu)
        return (uint32_t)-EINVAL;
    if (!(desc.flags & (1u << 5)) && !user_range_ok(desc.base_addr, 1))
        return (uint32_t)-EINVAL;

    int entry = desc.entry_number;
    if (entry == -1) {
        if (t->tls_gdt_entry >= 0) entry = t->tls_gdt_entry;
        else entry = gdt_tls_alloc(-1);
    } else if (t->tls_gdt_entry >= 0) {
        if (entry != t->tls_gdt_entry) return (uint32_t)-EINVAL;
    } else {
        entry = gdt_tls_alloc(entry);
    }
    if (entry < 0) return (uint32_t)-ESRCH;

    uint32_t contents = (desc.flags >> 1) & 3u;
    uint32_t read_only = (desc.flags >> 3) & 1u;
    uint32_t not_present = (desc.flags >> 5) & 1u;
    uint32_t useable = (desc.flags >> 6) & 1u;
    uint32_t limit_pages = (desc.flags >> 4) & 1u;
    uint32_t seg32 = desc.flags & 1u;
    uint8_t access = (uint8_t)(0x70u | (contents << 2) | (read_only ? 0u : 2u));
    uint8_t gran = (uint8_t)((seg32 ? 0x40u : 0u) |
                             (limit_pages ? 0x80u : 0u) |
                             (useable ? 0x10u : 0u));
    if (!not_present) access |= 0x80u;

    if (gdt_tls_set(entry, desc.base_addr, desc.limit, access, gran) != 0) {
        if (entry != t->tls_gdt_entry) gdt_tls_release(entry);
        return (uint32_t)-EINVAL;
    }
    t->tls_gdt_entry = entry;
    t->tls_base = desc.base_addr;
    t->tls_limit = desc.limit;
    t->tls_flags = desc.flags;
    desc.entry_number = entry;
    memcpy((void *)(uintptr_t)user_desc_ptr, &desc, sizeof(desc));
    return 0;
}

static uint32_t linux_get_thread_area(uint32_t user_desc_ptr)
{
    task_t *t = task_current();
    linux_user_desc_t desc;
    if (!t || !user_wr_ok(user_desc_ptr, sizeof(desc))) return (uint32_t)-EFAULT;
    memcpy(&desc, (const void *)(uintptr_t)user_desc_ptr, sizeof(desc));
    if (t->tls_gdt_entry < 0 || desc.entry_number != t->tls_gdt_entry)
        return (uint32_t)-EINVAL;
    desc.base_addr = t->tls_base;
    desc.limit = t->tls_limit;
    desc.flags = t->tls_flags;
    memcpy((void *)(uintptr_t)user_desc_ptr, &desc, sizeof(desc));
    return 0;
}

static int linux_clone_tls(uint32_t user_desc_ptr, int *entry_out,
                           uint32_t *base_out, uint32_t *limit_out, uint32_t *flags_out)
{
    linux_user_desc_t desc;
    if (!user_range_ok(user_desc_ptr, sizeof(desc))) return -EFAULT;
    memcpy(&desc, (const void *)(uintptr_t)user_desc_ptr, sizeof(desc));
    if ((desc.flags & ~0xFFu) || (desc.flags & 0x80u) || desc.limit > 0xFFFFFu ||
        (!(desc.flags & (1u << 5)) && !user_range_ok(desc.base_addr, 1))) return -EINVAL;

    int entry = gdt_tls_alloc(-1); /* a global GDT needs a distinct descriptor per host task */
    if (entry < 0) return -ESRCH;
    uint32_t contents = (desc.flags >> 1) & 3u;
    uint32_t read_only = (desc.flags >> 3) & 1u;
    uint32_t not_present = (desc.flags >> 5) & 1u;
    uint32_t useable = (desc.flags >> 6) & 1u;
    uint32_t limit_pages = (desc.flags >> 4) & 1u;
    uint32_t seg32 = desc.flags & 1u;
    uint8_t access = (uint8_t)(0x70u | (contents << 2) | (read_only ? 0u : 2u));
    uint8_t gran = (uint8_t)((seg32 ? 0x40u : 0u) |
                             (limit_pages ? 0x80u : 0u) |
                             (useable ? 0x10u : 0u));
    if (!not_present) access |= 0x80u;
    if (gdt_tls_set(entry, desc.base_addr, desc.limit, access, gran) != 0) {
        gdt_tls_release(entry);
        return -EINVAL;
    }
    *entry_out = entry;
    *base_out = desc.base_addr;
    *limit_out = desc.limit;
    *flags_out = desc.flags;
    return 0;
}

#define LINUX_CLONE_VM              0x00000100u
#define LINUX_CLONE_FS              0x00000200u
#define LINUX_CLONE_FILES           0x00000400u
#define LINUX_CLONE_SIGHAND         0x00000800u
#define LINUX_CLONE_PARENT          0x00008000u
#define LINUX_CLONE_THREAD          0x00010000u
#define LINUX_CLONE_SYSVSEM         0x00040000u
#define LINUX_CLONE_SETTLS          0x00080000u
#define LINUX_CLONE_PARENT_SETTID   0x00100000u
#define LINUX_CLONE_CHILD_CLEARTID  0x00200000u
#define LINUX_CLONE_DETACHED        0x00400000u
#define LINUX_CLONE_CHILD_SETTID    0x01000000u

static uint32_t linux_clone(registers_t *regs, uint32_t flags, uint32_t child_stack,
                            uint32_t parent_tid, uint32_t tls_ptr, uint32_t child_tid)
{
    proc_t *p = proc_current();
    task_t *parent = task_current();
    const uint32_t supported = LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES |
        LINUX_CLONE_SIGHAND | LINUX_CLONE_PARENT | LINUX_CLONE_THREAD |
        LINUX_CLONE_SYSVSEM | LINUX_CLONE_SETTLS | LINUX_CLONE_PARENT_SETTID |
        LINUX_CLONE_CHILD_CLEARTID | LINUX_CLONE_DETACHED | LINUX_CLONE_CHILD_SETTID;
    if (!p || !parent || !regs) return (uint32_t)-ESRCH;
    if (flags & ~supported) return (uint32_t)-EINVAL;
    if (!(flags & LINUX_CLONE_VM)) return (uint32_t)-ENOSYS;
    if (!(flags & LINUX_CLONE_THREAD) || !(flags & LINUX_CLONE_SIGHAND))
        return (uint32_t)-ENOSYS;
    if (parent_tid && (flags & LINUX_CLONE_PARENT_SETTID) &&
        !user_wr_ok(parent_tid, sizeof(uint32_t))) return (uint32_t)-EFAULT;
    if (child_tid && (flags & (LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_CHILD_CLEARTID)) &&
        !user_wr_ok(child_tid, sizeof(uint32_t))) return (uint32_t)-EFAULT;

    int tls_entry = -1;
    uint32_t tls_base = 0, tls_limit = 0, tls_flags = 0;
    if (flags & LINUX_CLONE_SETTLS) {
        int rc = linux_clone_tls(tls_ptr, &tls_entry, &tls_base, &tls_limit, &tls_flags);
        if (rc < 0) return (uint32_t)rc;
    }

    task_preempt_disable();
    int tid = task_spawn_user_thread(regs, child_stack, p, p->pgdir,
                                     (flags & LINUX_CLONE_PARENT) ? parent->thread_group : p->pid,
                                     tls_entry, tls_base, tls_limit, tls_flags,
                                     (flags & LINUX_CLONE_CHILD_CLEARTID) ? child_tid : 0);
    if (tid < 0) {
        task_preempt_enable();
        if (tls_entry >= 0) gdt_tls_release(tls_entry);
        return (uint32_t)-EAGAIN;
    }
    if ((flags & LINUX_CLONE_CHILD_SETTID) && child_tid)
        *(uint32_t *)(uintptr_t)child_tid = (uint32_t)tid;
    if ((flags & LINUX_CLONE_PARENT_SETTID) && parent_tid)
        *(uint32_t *)(uintptr_t)parent_tid = (uint32_t)tid;
    task_preempt_enable();
    return (uint32_t)tid;
}

static uint32_t linux_futex(uint32_t uaddr, uint32_t op, uint32_t value,
                            uint32_t timeout_or_val2, uint32_t uaddr2, uint32_t val3)
{
    (void)uaddr2;
    uint32_t command = op & ~(0x80u | 0x100u); /* private and realtime flags */
    if ((uaddr & 3u) || !user_range_ok(uaddr, sizeof(uint32_t)))
        return (uint32_t)-EFAULT;

    switch (command) {
        case 1:                 /* FUTEX_WAKE */
            return (uint32_t)task_futex_wake(uaddr, value);
        case 0:                 /* FUTEX_WAIT */
        case 9: {               /* FUTEX_WAIT_BITSET */
            uint64_t deadline = 0;
            if (command == 9 && val3 == 0) return (uint32_t)-EINVAL;
            if (timeout_or_val2) {
                linux_timespec32_t ts;
                if (!user_range_ok(timeout_or_val2, sizeof(ts))) return (uint32_t)-EFAULT;
                memcpy(&ts, (const void *)(uintptr_t)timeout_or_val2, sizeof(ts));
                if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
                    return (uint32_t)-EINVAL;
                uint64_t ms = (uint64_t)(uint32_t)ts.sec * 1000u +
                              ((uint32_t)ts.nsec + 999999u) / 1000000u;
                if (command == 0) deadline = debugmon_uptime_ms64() + ms;
                else deadline = ms; /* absolute CLOCK_MONOTONIC deadline */
            }
            int rc = task_futex_wait(uaddr, value, deadline);
            if (rc < 0) return (uint32_t)-EAGAIN;
            if (rc > 0) return (uint32_t)-ETIMEDOUT;
            return 0;
        }
        default:
            return (uint32_t)-ENOSYS;
    }
}

/* Linux i386 syscall namespace for ELF processes; the existing .t ABI keeps
 * its tOS extension numbers and behaviour in syscall_handler() below. */
static uint32_t linux_syscall_handler(uint32_t number, uint32_t a, uint32_t b,
                                      uint32_t c, uint32_t d, uint32_t e, uint32_t f,
                                      registers_t *regs)
{
    switch (number) {
        case SYS_EXIT:
            if (task_current() && task_current()->user_thread)
                proc_thread_exit((int)a);
            proc_exit((int)a);

        case 252: /* exit_group */
            proc_exit((int)a);

        /* a signal to itself (abort() raises SIGABRT): no handlers run yet, so the
         * default action, ending the program with 128 + the signal */
        case LINUX_SYS_TGKILL:
        case LINUX_SYS_TKILL: {
            int sig = (int)(number == LINUX_SYS_TGKILL ? c : b);
            if (sig < 0 || sig > 64) return (uint32_t)-EINVAL;
            if (sig == 0) return 0;
            if (sig == 6 && regs) {
                /* abort(): the return addresses on the stack say who gave up */
                serial_write("linux: SIGABRT, stack:\n");
                uint32_t sp = regs->useresp;
                char row[112], h[12];
                for (uint32_t w = 0; w < 160 && sp + w * 4 < 0xC0000000u; w += 8) {
                    strcpy(row, "  abort ");
                    for (uint32_t k = w; k < w + 8; k++) {
                        uint32_t v = ((const uint32_t *)(uintptr_t)sp)[k];
                        for (int sh = 28; sh >= 0; sh -= 4) { h[(28 - sh) / 4] = "0123456789ABCDEF"[(v >> sh) & 0xF]; }
                        h[8] = ' '; h[9] = 0;
                        strcat(row, h);
                    }
                    strcat(row, "\n");
                    serial_write(row);
                }
                syscall_dump_recent();
            }
            proc_exit(128 + sig);
        }

        case LINUX_SYS_IOCTL:
            return linux_ioctl((int)a, b, c);

        case LINUX_SYS_UGETRLIMIT:
            return linux_rlimit((int)a, 0, b, 0);
        case LINUX_SYS_PRLIMIT64:
            if (a && a != task_get_pid()) return (uint32_t)-ESRCH;
            return linux_rlimit((int)b, c, d, 1);

        case LINUX_SYS_CLOCK_GETRES:
        case LINUX_SYS_CLOCK_GETRES64:
            if (b) {
                /* the clocks tick in milliseconds */
                uint32_t res64[4] = { 0, 0, 1000000, 0 };
                uint32_t res32[2] = { 0, 1000000 };
                uint32_t size = number == LINUX_SYS_CLOCK_GETRES64 ? 16u : 8u;
                if (!user_wr_ok(b, size)) return (uint32_t)-EFAULT;
                memcpy((void *)(uintptr_t)b, number == LINUX_SYS_CLOCK_GETRES64 ? (void *)res64 : (void *)res32, size);
            }
            return 0;

        case 242: /* sched_getaffinity: one CPU (glibc counts processors from it, V8 needs that count) */
            if (a && a != task_get_pid()) return (uint32_t)-ESRCH;
            if (b < 4) return (uint32_t)-EINVAL;
            if (!user_wr_ok(c, 4)) return (uint32_t)-EFAULT;
            { uint32_t one = 1; memcpy((void *)(uintptr_t)c, &one, 4); }
            return 4;
        case 241: /* sched_setaffinity: accepted, there is only the one CPU */
            if (a && a != task_get_pid()) return (uint32_t)-ESRCH;
            return 0;

        case LINUX_SYS_ARCH_PRCTL:              /* x86-64's call; i386 programs only probe for it */
            return (uint32_t)-EINVAL;

        case SYS_FORK:
            return (uint32_t)-ENOSYS;

        case LINUX_SYS_CLONE:
            return linux_clone(regs, a, b, c, d, e);

        case SYS_READ: {
            int fd = (int)a, count = (int)c;
            char *buf = (char *)(uintptr_t)b;
            if (count <= 0) return 0;
            if (!user_wr_ok(b, (uint32_t)count)) return (uint32_t)-EFAULT;
            if (fd == 0) {
                for (int i = 0; i < count; i++) {
                    char ch = keyboard_getchar_ring3();
                    terminal_putchar(ch);
                    buf[i] = ch;
                    if (ch == '\n') return (uint32_t)(i + 1);
                }
                return (uint32_t)count;
            }
            proc_t *p = proc_current();
            if (p && fd >= 3 && fd - 3 < PROC_MAX_FDS && p->fd[fd - 3] == PROC_FD_URANDOM) {
                csprng_fill((uint8_t *)buf, count);
                return (uint32_t)count;
            }
            if (p && fd >= 3 && fd - 3 < PROC_MAX_FDS && p->fd[fd - 3] == PROC_FD_NULL) return 0;
            {
                proc_eventfd_t *ev = eventfd_of(p, fd);
                if (ev) return eventfd_read(p, fd, ev, b, (uint32_t)count);
                int sk = sock_of(p, fd);
                if (sk >= 0) return sock_recv_on(p, fd, sk, b, (uint32_t)count, 0, 0, 0);
            }
            if (proc_pipe_endpoint(p, fd, 0) >= 0)
                return linux_pipe_read(p, fd, b, (uint32_t)count);
            int vfd = proc_vfd(proc_current(), fd);
            if (vfd < 0) return (uint32_t)-EBADF;
            int n = vfs_read(vfd, buf, (uint32_t)count);
            return n < 0 ? (uint32_t)-EIO : (uint32_t)n;
        }

        case SYS_WRITE: {
            int fd = (int)a, count = (int)c;
            const char *buf = (const char *)(uintptr_t)b;
            if (count <= 0) return 0;
            if (!user_range_ok(b, (uint32_t)count)) return (uint32_t)-EFAULT;
            if (fd == 1 || fd == 2) {
                for (int i = 0; i < count; i++) {
                    terminal_putchar(buf[i]);
                }
                return (uint32_t)count;
            }
            proc_t *p = proc_current();
            if (p && fd >= 3 && fd - 3 < PROC_MAX_FDS &&
                (p->fd[fd - 3] == PROC_FD_NULL || p->fd[fd - 3] == PROC_FD_URANDOM))
                return (uint32_t)count;
            {
                proc_eventfd_t *ev = eventfd_of(p, fd);
                if (ev) return eventfd_write(ev, b, (uint32_t)count);
                int sk = sock_of(p, fd);
                if (sk >= 0) return sock_send_on(p, fd, sk, b, (uint32_t)count, 0, 0, 0);
            }
            if (proc_pipe_endpoint(p, fd, 0) >= 0)
                return linux_pipe_write(p, fd, b, (uint32_t)count);
            int vfd = proc_vfd(proc_current(), fd);
            if (vfd < 0) return (uint32_t)-EBADF;
            int n = vfs_write(vfd, buf, (uint32_t)count);
            return n < 0 ? (uint32_t)-EIO : (uint32_t)n;
        }

        case LINUX_SYS_WRITEV: {
            int fd = (int)a;
            int iovcnt = (int)c;
            if (iovcnt < 0 || iovcnt > 1024) return (uint32_t)-EINVAL;
            if (iovcnt == 0) return 0;
            uint32_t iov_bytes = (uint32_t)iovcnt * sizeof(linux_iovec32_t);
            if (!user_range_ok(b, iov_bytes)) return (uint32_t)-EFAULT;
            const linux_iovec32_t *iov = (const linux_iovec32_t *)(uintptr_t)b;
            uint32_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (iov[i].len > UINT32_MAX - total) return (uint32_t)-EINVAL;
                total += iov[i].len;
                if (iov[i].len && !user_range_ok(iov[i].base, iov[i].len))
                    return (uint32_t)-EFAULT;
            }
            proc_t *wp = proc_current();
            int wsk = sock_of(wp, fd);
            if (wsk >= 0) {                     /* one send: a TLS record split in segments is slow */
                uint8_t *tmp = malloc(total ? total : 1);
                if (!tmp) return (uint32_t)-ENOMEM;
                uint32_t off = 0;
                for (int i = 0; i < iovcnt; i++) { memcpy(tmp + off, (const void *)(uintptr_t)iov[i].base, iov[i].len); off += iov[i].len; }
                uint32_t r = sock_send_k(wp, fd, wsk, tmp, total, 0, 0, 0);
                free(tmp);
                return r;
            }
            if (fd > 2 && proc_vfd(wp, fd) < 0) {   /* pipes, eventfds, /dev/null: write() knows them */
                uint32_t done = 0;
                for (int i = 0; i < iovcnt; i++) {
                    if (!iov[i].len) continue;
                    uint32_t r = linux_syscall_handler(SYS_WRITE, (uint32_t)fd, iov[i].base, iov[i].len, 0, 0, 0, regs);
                    if ((int32_t)r < 0) return done ? done : r;
                    done += r;
                    if (r < iov[i].len) break;
                }
                return done;
            }
            uint32_t written = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (fd == 1 || fd == 2) {
                    const char *buf = (const char *)(uintptr_t)iov[i].base;
                    for (uint32_t j = 0; j < iov[i].len; j++) {
                        terminal_putchar(buf[j]);
                    }
                    written += iov[i].len;
                } else {
                    int vfd = proc_vfd(proc_current(), fd);
                    if (vfd < 0) return written ? written : (uint32_t)-EBADF;
                    int n = vfs_write(vfd, (const void *)(uintptr_t)iov[i].base, iov[i].len);
                    if (n < 0) return written ? written : (uint32_t)-EIO;
                    written += (uint32_t)n;
                    if ((uint32_t)n < iov[i].len) break;
                }
            }
            return written;
        }

        case 145: { /* readv */
            int iovcnt = (int)c;
            if (iovcnt < 0 || iovcnt > 1024) return (uint32_t)-EINVAL;
            if (!user_range_ok(b, (uint32_t)iovcnt * 8u)) return (uint32_t)-EFAULT;
            const uint32_t *iov = (const uint32_t *)(uintptr_t)b;
            uint32_t done = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (!iov[2 * i + 1]) continue;
                uint32_t r = linux_syscall_handler(SYS_READ, a, iov[2 * i], iov[2 * i + 1], 0, 0, 0, regs);
                if ((int32_t)r < 0) return done ? done : r;
                done += r;
                if (r < iov[2 * i + 1]) break;
            }
            return done;
        }

        case SYS_OPEN:
            return linux_open_path(a, b, c, -100);
        case LINUX_SYS_OPENAT:
            return linux_open_path(b, c, d, (int32_t)a);

        case SYS_CLOSE: {
            proc_t *p = proc_current();
            int fd = (int)a;
            if (p && fd >= 3 && fd - 3 < PROC_MAX_FDS && p->fd[fd - 3] != -1) {
                proc_fd_close(p, fd - 3);
                return 0;
            }
            return (uint32_t)-EBADF;
        }

        case SYS_WAITPID: {
            proc_t *child = proc_by_pid(a);
            if (!child || child->ppid != task_get_pid()) return (uint32_t)-ECHILD;
            if (b && !user_wr_ok(b, sizeof(uint32_t))) return (uint32_t)-EFAULT;
            int code = proc_wait(a);
            if (b) *(uint32_t *)(uintptr_t)b = ((uint32_t)(code & 0xFF)) << 8;
            return a;
        }

        case SYS_SPAWN: /* TOS extension, useful to bootstrap a child before fork/exec */
            return (uint32_t)syscall_spawn_user(a, b, c);

        case SYS_EXECVE:
            return (uint32_t)syscall_exec_user(a, b, c);

        case SYS_CHDIR:
            if (!user_cstr_ok(a)) return (uint32_t)-EFAULT;
            {
                vfs_entry_t entry;
                if (vfs_stat((const char *)(uintptr_t)a, &entry) != 0 || !entry.is_dir)
                    return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
                if (vfs_access((const char *)(uintptr_t)a, VFS_ACC_X) != 0)
                    return (uint32_t)-EACCES;
            }
            vfs_chdir((const char *)(uintptr_t)a);
            return 0;

        case LINUX_SYS_BRK:
            return linux_sys_brk(a);

        case SYS_LSEEK: {
            int vfd = proc_vfd(proc_current(), (int)a);
            if (vfd < 0) return (uint32_t)-EBADF;
            int offset = (int)b;
            if (offset < 0) return (uint32_t)-EINVAL;
            int r = vfs_lseek(vfd, (uint32_t)offset, (int)c);
            return r < 0 ? (uint32_t)-EINVAL : (uint32_t)r;
        }

        case SYS_GETPID:
            return proc_current() ? proc_current()->pid : task_get_pid();
        case LINUX_SYS_ACCESS:
            if (!user_cstr_ok(a) || (b & ~7u)) return (uint32_t)-EFAULT;
            if (vfs_access((const char *)(uintptr_t)a, (int)b) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;

        case LINUX_SYS_GETCWD: {
            if (!b || !user_wr_ok(a, b)) return (uint32_t)-EFAULT;
            int len = vfs_getcwd((char *)(uintptr_t)a, b);
            return len < 0 ? (uint32_t)-ERANGE : (uint32_t)(len + 1);
        }

        case LINUX_SYS_FCNTL:
        case LINUX_SYS_FCNTL64: {
            proc_t *p = proc_current();
            int fd = (int)a, cmd = (int)b;
            if (fd >= 0 && fd <= 2) {
                if (cmd == LINUX_F_GETFD) return 0;
                if (cmd == LINUX_F_SETFD) return c & ~LINUX_FD_CLOEXEC ? (uint32_t)-EINVAL : 0;
                if (cmd == LINUX_F_GETFL) return fd == 0 ? 0 : 1;
                if (cmd == LINUX_F_SETFL) return 0;
                return (uint32_t)-EINVAL;
            }
            if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS || p->fd[fd - 3] == -1)
                return (uint32_t)-EBADF;
            int slot = fd - 3;
            switch (cmd) {
                case LINUX_F_GETFD: return p->fd_flags[slot];
                case LINUX_F_SETFD:
                    if (c & ~LINUX_FD_CLOEXEC) return (uint32_t)-EINVAL;
                    p->fd_flags[slot] = c;
                    return 0;
                case LINUX_F_GETFL: return p->fd_open_flags[slot];
                case LINUX_F_SETFL:
                    p->fd_open_flags[slot] = (p->fd_open_flags[slot] & ~0x00000F00u) |
                                               (c & 0x00000F00u);
                    return 0;
                default:
                    return (uint32_t)-ENOSYS;
            }
        }

        case LINUX_SYS_READLINK:
        case LINUX_SYS_READLINKAT: {
            uint32_t path_ptr = number == LINUX_SYS_READLINK ? a : b;
            uint32_t buf_ptr = number == LINUX_SYS_READLINK ? b : c;
            uint32_t size = number == LINUX_SYS_READLINK ? c : d;
            if (number == LINUX_SYS_READLINKAT && (int32_t)a != -100)
                return (uint32_t)-EBADF;
            if (!user_cstr_ok(path_ptr)) return (uint32_t)-EFAULT;
            if (size == 0) return 0;
            if (!user_wr_ok(buf_ptr, size)) return (uint32_t)-EFAULT;
            int n = vfs_readlink((const char *)(uintptr_t)path_ptr, (char *)(uintptr_t)buf_ptr, size);
            if (n >= 0) return (uint32_t)n;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-EINVAL;
        }

        case LINUX_SYS_SYMLINK:
            if (!user_cstr_ok(a) || !user_cstr_ok(b)) return (uint32_t)-EFAULT;
            if (vfs_symlink((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-EEXIST;

        case LINUX_SYS_UNLINK: {
            vfs_entry_t entry;
            if (!user_cstr_ok(a)) return (uint32_t)-EFAULT;
            if (vfs_stat((const char *)(uintptr_t)a, &entry) != 0)
                return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
            if (entry.is_dir) return (uint32_t)-EISDIR;
            if (vfs_unlink((const char *)(uintptr_t)a) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-EPERM;
        }

        case 60: { /* umask */
            proc_t *up = proc_current();
            if (!up) return 022;
            uint32_t old = up->umask_set ? up->umask : 022u;
            up->umask = a & 0777u;
            up->umask_set = 1;
            return old;
        }

        case 40: { /* rmdir */
            vfs_entry_t entry;
            if (!user_cstr_ok(a)) return (uint32_t)-EFAULT;
            if (vfs_stat((const char *)(uintptr_t)a, &entry) != 0)
                return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
            if (!entry.is_dir) return (uint32_t)-ENOTDIR;
            if (vfs_unlink((const char *)(uintptr_t)a) == 0) return 0;
            if (!vfs_path_has_mount((const char *)(uintptr_t)a) && ramfs_rmdir(vfs_abspath((const char *)(uintptr_t)a)) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-39;        /* ENOTEMPTY */
        }

        case 9: /* link: no hard links here; EPERM is what a filesystem without them says
                 * (npm's cache then moves the file instead) */
            return (uint32_t)-EPERM;

        case 118: case 148:                     /* fsync, fdatasync: writes are already done */
            return 0;

        /* ownership: tOS keeps its own owners and permissions; a Linux program changing them
         * (npm sets the owner it already has) is let through */
        case 16: case 95: case 182: case 198: case 207: case 212: case 298:
            return 0;

        /* times: accepted; the files keep the times tOS gave them. The file has to be there,
         * though (touch creates it only when this says it isn't). */
        case 30: case 271: case 299: case 320: case 412: {
            uint32_t path = (number == 30 || number == 271) ? a : b;
            vfs_entry_t entry;
            if (!path) return 0;                         /* utimensat on an open fd */
            if (!user_cstr_ok(path)) return (uint32_t)-EFAULT;
            if (vfs_stat((const char *)(uintptr_t)path, &entry) != 0)
                return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
            return 0;
        }

        case LINUX_SYS_MKDIR: {
            vfs_entry_t entry;
            if (!user_cstr_ok(a)) return (uint32_t)-EFAULT;
            /* "already there" has to be EEXIST: mkdir -p (npm's mkdirp) climbs to the parent on
             * ENOENT, and at / that never ends */
            if (vfs_stat((const char *)(uintptr_t)a, &entry) == 0) return (uint32_t)-EEXIST;
            if (vfs_mkdir((const char *)(uintptr_t)a, b & 0777u) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
        }

        case LINUX_SYS_RENAME: {
            char old_path[VFS_NAME_LEN], new_path[VFS_NAME_LEN];
            if (!user_cstr_ok(a) || !user_cstr_ok(b)) return (uint32_t)-EFAULT;
            strncpy(old_path, (const char *)(uintptr_t)a, sizeof(old_path) - 1);
            old_path[sizeof(old_path) - 1] = 0;
            strncpy(new_path, (const char *)(uintptr_t)b, sizeof(new_path) - 1);
            new_path[sizeof(new_path) - 1] = 0;
            if (vfs_rename(old_path, new_path) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
        }

        case LINUX_SYS_CHMOD:
            if (!user_cstr_ok(a)) return (uint32_t)-EFAULT;
            if (vfs_chmod((const char *)(uintptr_t)a, b & 07777u) == 0) return 0;
            return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-EPERM;

        case LINUX_SYS_DUP:
        case LINUX_SYS_DUP2:
            return (uint32_t)-ENOSYS;
        case SYS_GETUID:
        case LINUX_SYS_GETUID32: {
            uint32_t uid = 0, gid = 0;
            task_get_cred(&uid, &gid);
            return uid;
        }
        case SYS_GETGID:
        case LINUX_SYS_GETGID32: {
            uint32_t uid = 0, gid = 0;
            task_get_cred(&uid, &gid);
            return gid;
        }
        case LINUX_SYS_GETEUID:
        case LINUX_SYS_GETEUID32: {
            uint32_t uid = 0, gid = 0;
            task_get_cred(&uid, &gid);
            return uid;
        }
        case LINUX_SYS_GETEGID:
        case LINUX_SYS_GETEGID32: {
            uint32_t uid = 0, gid = 0;
            task_get_cred(&uid, &gid);
            return gid;
        }
        case LINUX_SYS_GETPPID: {
            proc_t *p = proc_current();
            return p ? p->ppid : 0;
        }
        case LINUX_SYS_GETTID:
            return task_get_pid();
        case LINUX_SYS_SET_TID_ADDRESS:
            if (a && !user_wr_ok(a, sizeof(uint32_t))) return (uint32_t)-EFAULT;
            if (task_current()) task_current()->clear_tid = a;
            return task_get_pid();
        case LINUX_SYS_SET_ROBUST_LIST:
            return 0;
        case LINUX_SYS_SET_THREAD_AREA:
            return linux_set_thread_area(a);
        case LINUX_SYS_GET_THREAD_AREA:
            return linux_get_thread_area(a);
        case LINUX_SYS_FUTEX:
            return linux_futex(a, b, c, d, e, f);
        case SYS_KILL:
            if (b != 9 && b != 15) return (uint32_t)-EINVAL;
            return proc_kill(a, 128 + (int)b) == 0 ? 0 : (uint32_t)-ESRCH;
        case SYS_ISATTY:
            return a <= 2 ? 1 : 0;

        case LINUX_SYS_EPOLL_CREATE:
        case LINUX_SYS_EPOLL_CREATE1: {
            proc_t *p = proc_current();
            int flags = number == LINUX_SYS_EPOLL_CREATE1 ? (int)a : 0;
            if (!p) return (uint32_t)-ESRCH;
            if ((number == LINUX_SYS_EPOLL_CREATE && (int32_t)a <= 0) ||
                (flags & ~0x80000)) return (uint32_t)-EINVAL;
            for (int i = 0; i < PROC_MAX_FDS; i++) {
                if (p->fd[i] != -1) continue;
                p->fd[i] = PROC_FD_EPOLL;
                p->fd_flags[i] = (flags & 0x80000) ? LINUX_FD_CLOEXEC : 0;
                p->fd_open_flags[i] = flags;
                p->fdpath[i][0] = 0;
                return (uint32_t)(i + 3);
            }
            return (uint32_t)-EMFILE;
        }

        case LINUX_SYS_PIPE:
            return linux_pipe_create(a, 0);
        case LINUX_SYS_PIPE2:
            return linux_pipe_create(a, b);

        case 323: /* eventfd */
            return linux_eventfd(a, 0);
        case 328: /* eventfd2 */
            return linux_eventfd(a, b);

        case 168: /* poll */
            return linux_poll(a, b, (int)c);

        case LINUX_SYS_EPOLL_CTL:
            return linux_epoll_ctl((int)a, (int)b, (int)c, d);

        case LINUX_SYS_EPOLL_WAIT:
            return linux_epoll_wait((int)a, b, (int)c, (int)d);
        case LINUX_SYS_EPOLL_PWAIT:              /* the signal mask is not used: no signals are delivered */
            return linux_epoll_wait((int)a, b, (int)c, (int)d);

        case LINUX_SYS_UNAME: {
            linux_utsname_t info;
            if (!user_wr_ok(a, sizeof(info))) return (uint32_t)-EFAULT;
            memset(&info, 0, sizeof(info));
            strcpy(info.sysname, "Linux");
            const char *hostname = auth_hostname();
            strncpy(info.nodename, hostname ? hostname : "tOS", sizeof(info.nodename) - 1);
            /* Static glibc parses the release as a Linux kernel version and
             * refuses startup when it is below the libc build minimum. */
            strcpy(info.release, "3.2.0-tOS");
            strcpy(info.version, "tOS Linux-i386 ABI");
            strcpy(info.machine, "i686");
            strcpy(info.domainname, "localdomain");
            memcpy((void *)(uintptr_t)a, &info, sizeof(info));
            return 0;
        }

        case LINUX_SYS_STAT64:
        case LINUX_SYS_LSTAT64: {
            linux_stat64_t st;
            vfs_entry_t entry;
            if (!user_cstr_ok(a) || !user_wr_ok(b, sizeof(st))) return (uint32_t)-EFAULT;
            if (!strcmp((const char *)(uintptr_t)a, "/etc/resolv.conf") || !strcmp((const char *)(uintptr_t)a, "/etc/hosts")) write_net_files();
            if (vfs_stat((const char *)(uintptr_t)a, &entry) != 0) return (uint32_t)-ENOENT;
            linux_stat64_fill(&st, &entry);
            memcpy((void *)(uintptr_t)b, &st, sizeof(st));
            return 0;
        }

        case LINUX_SYS_FSTATAT64: {
            int32_t dirfd = (int32_t)a;
            linux_stat64_t st;
            vfs_entry_t entry;
            if (!user_cstr_ok(b) || !user_wr_ok(c, sizeof(st))) return (uint32_t)-EFAULT;
            const char *path = (const char *)(uintptr_t)b;
            if (path[0] == 0 && (d & 0x1000u)) { /* AT_EMPTY_PATH: glibc's fstat */
                int r = linux_fd_stat(dirfd, &st);
                if (r < 0) return (uint32_t)r;
            } else {
                if (path[0] != '/' && dirfd != -100) return (uint32_t)-EBADF;
                if (!strcmp(path, "/etc/resolv.conf") || !strcmp(path, "/etc/hosts")) write_net_files();
                if (vfs_stat(path, &entry) != 0)
                    return vfs_denied() ? (uint32_t)-EACCES : (uint32_t)-ENOENT;
                linux_stat64_fill(&st, &entry);
            }
            memcpy((void *)(uintptr_t)c, &st, sizeof(st));
            return 0;
        }

        case LINUX_SYS_GETDENTS64:
            return linux_getdents64(a, b, c);

        case LINUX_SYS_FSTAT64: {
            linux_stat64_t st;
            if (!user_wr_ok(b, sizeof(st))) return (uint32_t)-EFAULT;
            int r = linux_fd_stat((int)a, &st);
            if (r < 0) return (uint32_t)r;
            memcpy((void *)(uintptr_t)b, &st, sizeof(st));
            return 0;
        }

        case 102: { /* socketcall(call, args) */
            if (a < 1 || a > 20) return (uint32_t)-EINVAL;
            if (!user_range_ok(b, 24)) return (uint32_t)-EFAULT;
            uint32_t v[6];
            memcpy(v, (const void *)(uintptr_t)b, sizeof(v));
            return linux_sockcall((int)a, v);
        }
        /* the same calls, one syscall each (Linux 4.3 and later) */
        case 359: case 360: case 361: case 362: case 363: case 364: case 365: case 366:
        case 367: case 368: case 369: case 370: case 371: case 372: case 373: case 345: {
            static const uint8_t call_of[15] = { 1, 8, 2, 3, 4, 18, 15, 14, 6, 7, 11, 16, 12, 17, 13 };
            uint32_t v[6] = { a, b, c, d, e, f };
            return linux_sockcall(number == 345 ? 20 : call_of[number - 359], v);
        }

        case 172: { /* prctl: the thread's name (PR_SET_NAME, PR_GET_NAME), nothing else */
            proc_t *pp = proc_current();
            if (!pp) return (uint32_t)-ESRCH;
            if (a == 15) {
                if (!user_range_ok(b, 1)) return (uint32_t)-EFAULT;
                int k = 0;
                for (; k < 15 && k < (int)sizeof(pp->name) - 1 && ((const char *)(uintptr_t)b)[k]; k++) pp->name[k] = ((const char *)(uintptr_t)b)[k];
                pp->name[k] = 0;
                return 0;
            }
            if (a == 16) {
                if (!user_wr_ok(b, 16)) return (uint32_t)-EFAULT;
                memset((void *)(uintptr_t)b, 0, 16);
                strncpy((char *)(uintptr_t)b, pp->name, 15);
                return 0;
            }
            return (uint32_t)-EINVAL;
        }

        case 219: /* madvise: advice only; the pages stay as they are */
            if (a & 0xFFFu) return (uint32_t)-EINVAL;
            return 0;

        case 180:   /* pread64 */
        case 181: { /* pwrite64: at an offset, leaving the file position where it was */
            int vfd = proc_vfd(proc_current(), (int)a);
            if (vfd < 0) return (uint32_t)-EBADF;
            if (e || (int32_t)d < 0) return (uint32_t)-EINVAL;   /* offsets past 2 GB do not exist here */
            if (!c) return 0;
            if (number == 180 ? !user_wr_ok(b, c) : !user_range_ok(b, c)) return (uint32_t)-EFAULT;
            int cur = vfs_lseek(vfd, 0, 1);
            if (cur < 0 || vfs_lseek(vfd, d, 0) < 0) return (uint32_t)-ESPIPE;
            int n = number == 180 ? vfs_read(vfd, (void *)(uintptr_t)b, c) : vfs_write(vfd, (const void *)(uintptr_t)b, c);
            vfs_lseek(vfd, (uint32_t)cur, 0);
            return n < 0 ? (uint32_t)-EIO : (uint32_t)n;
        }

        case LINUX_SYS_LLSEEK: {
            int vfd = proc_vfd(proc_current(), (int)a);
            if (vfd < 0) return (uint32_t)-EBADF;
            uint64_t offset = ((uint64_t)b << 32) | c;
            if (offset > UINT32_MAX) return (uint32_t)-EOVERFLOW;
            if (!user_wr_ok(d, sizeof(uint64_t))) return (uint32_t)-EFAULT;
            int r = vfs_lseek(vfd, (uint32_t)offset, (int)e);
            if (r < 0) return (uint32_t)-EINVAL;
            *(uint64_t *)(uintptr_t)d = (uint32_t)r;
            return 0;
        }

        case LINUX_SYS_CLOCK_GETTIME:
        case LINUX_SYS_CLOCK_GETTIME64: {
            int64_t sec;
            int32_t nsec;
            if (linux_clock_now(a, &sec, &nsec) != 0) return (uint32_t)-EINVAL;
            if (number == LINUX_SYS_CLOCK_GETTIME64) {
                linux_timespec64_t ts = { sec, nsec };
                if (!user_wr_ok(b, sizeof(ts))) return (uint32_t)-EFAULT;
                memcpy((void *)(uintptr_t)b, &ts, sizeof(ts));
            } else {
                linux_timespec32_t ts;
                if (sec > INT32_MAX) return (uint32_t)-EOVERFLOW;
                ts.sec = (int32_t)sec;
                ts.nsec = nsec;
                if (!user_wr_ok(b, sizeof(ts))) return (uint32_t)-EFAULT;
                memcpy((void *)(uintptr_t)b, &ts, sizeof(ts));
            }
            return 0;
        }

        case LINUX_SYS_GETTIMEOFDAY: {
            if (a) {
                int64_t sec;
                int32_t nsec;
                if (linux_clock_now(0, &sec, &nsec) != 0 || sec > INT32_MAX)
                    return (uint32_t)-EOVERFLOW;
                linux_timeval32_t tv = { (int32_t)sec, nsec / 1000 };
                if (!user_wr_ok(a, sizeof(tv))) return (uint32_t)-EFAULT;
                memcpy((void *)(uintptr_t)a, &tv, sizeof(tv));
            }
            if (b) {
                uint32_t timezone[2] = { 0, 0 };
                if (!user_wr_ok(b, sizeof(timezone))) return (uint32_t)-EFAULT;
                memcpy((void *)(uintptr_t)b, timezone, sizeof(timezone));
            }
            return 0;
        }

        case LINUX_SYS_NANOSLEEP:
            return linux_nanosleep(a, b, 0);
        case LINUX_SYS_NANOSLEEP64:              /* clock_nanosleep_time64(clock, flags, req, rem) */
            return linux_nanosleep_on(a, (b & 1u) != 0, c, d, 1);
        case 267:                                /* clock_nanosleep(clock, flags, req, rem) */
            return linux_nanosleep_on(a, (b & 1u) != 0, c, d, 0);

        case LINUX_SYS_RT_SIGPROCMASK: {
            if (d != 8 || (a > 2)) return (uint32_t)-EINVAL;
            if (b && !user_range_ok(b, d)) return (uint32_t)-EFAULT;
            if (c) {
                if (!user_wr_ok(c, d)) return (uint32_t)-EFAULT;
                memset((void *)(uintptr_t)c, 0, d);
            }
            return 0; /* user signal delivery is not implemented yet */
        }

        case LINUX_SYS_RT_SIGACTION: {
            if (a == 0 || a > 64 || d != 8) return (uint32_t)-EINVAL;
            if (b && !user_range_ok(b, sizeof(linux_sigaction32_t))) return (uint32_t)-EFAULT;
            if (c) {
                if (!user_wr_ok(c, sizeof(linux_sigaction32_t))) return (uint32_t)-EFAULT;
                linux_sigaction32_t old_action;
                memset(&old_action, 0, sizeof(old_action));
                memcpy((void *)(uintptr_t)c, &old_action, sizeof(old_action));
            }
            return 0; /* accept dispositions; process signal delivery remains unsupported */
        }

        case LINUX_SYS_SIGALTSTACK:
            if (a && !user_range_ok(a, 12)) return (uint32_t)-EFAULT;
            if (b) {
                if (!user_wr_ok(b, 12)) return (uint32_t)-EFAULT;
                memset((void *)(uintptr_t)b, 0, 12);
            }
            return 0;

        case LINUX_SYS_GETRANDOM:
            if (c & ~3u) return (uint32_t)-EINVAL;
            if (b > 1024u * 1024u) return (uint32_t)-EINVAL;
            if (b == 0) return 0;
            if (!user_wr_ok(a, b)) return (uint32_t)-EFAULT;
            csprng_fill((uint8_t *)(uintptr_t)a, (int)b);
            return b;

        case SYS_MMAP2:
            return syscall_mmap2(a, b, c, d, e, f);
        case SYS_MUNMAP:
            return syscall_munmap(a, b);
        case SYS_MPROTECT:
            return syscall_mprotect(a, b, c);
        default:
            return (uint32_t)-ENOSYS;
    }
}

/* the last Linux syscalls and what they returned, written out when a program crashes */
#define RECENT 48
static uint32_t recent_nr[RECENT], recent_ret[RECENT];
static uint32_t recent_n;

void syscall_dump_recent(void)
{
    char line[64], h[12];
    uint32_t start = recent_n > RECENT ? recent_n - RECENT : 0;
    for (uint32_t i = start; i < recent_n; i++) {
        uint32_t k = i % RECENT;
        strcpy(line, "  sys ");
        sprintf_u32(h, recent_nr[k]); strcat(line, h);
        strcat(line, " -> ");
        if ((int32_t)recent_ret[k] < 0 && (int32_t)recent_ret[k] > -4096) { strcat(line, "-"); sprintf_u32(h, (uint32_t)-(int32_t)recent_ret[k]); }
        else sprintf_u32(h, recent_ret[k]);
        strcat(line, h);
        strcat(line, "\n");
        serial_write(line);
    }
}

void syscall_proc_cleanup(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        proc_fd_close(p, i);
    }
    if (p->gfx) { p->gfx = 0; gfx_leave_if_active(); }
    if (p->win) { wm_script_close(p->win, p->win_prev); p->win = 0; p->win_prev = 0; }
}

static void syscall_stub(registers_t *regs)
{
    uint32_t result = syscall_handler(regs->eax, regs->ebx, regs->ecx, regs->edx,
                                      regs->esi, regs->edi, regs->ebp, regs);
    regs->eax = result;
}

uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d, uint32_t e, uint32_t f, registers_t *regs)
{
    proc_t *current_proc = proc_current();
    if (current_proc && current_proc->linux_abi) {
        cstr_too_long = 0;
        uint32_t r = linux_syscall_handler(syscall, a, b, c, d, e, f, regs);
        if (r == (uint32_t)-EFAULT && cstr_too_long) r = (uint32_t)-ENAMETOOLONG;
        recent_nr[recent_n % RECENT] = syscall;
        recent_ret[recent_n % RECENT] = r;
        recent_n++;
        if (r == (uint32_t)-ENOSYS) {
            /* the first three of each, with the sub-call (socketcall's first argument) and where from */
            static uint8_t seen[512];
            if (syscall < 512 && seen[syscall] < 3) {
                seen[syscall]++;
                char line[96] = "linux: no syscall ", h[12];
                sprintf_u32(h, syscall); strcat(line, h);
                strcat(line, " a="); sprintf_u32(h, a); strcat(line, h);
                strcat(line, " eip="); sprintf_u32(h, regs ? regs->eip : 0); strcat(line, h);
                strcat(line, "\n");
                serial_write(line);
                if (syscall == 102 && regs && seen[syscall] == 1) {
                    /* who keeps asking for sockets: the stack's return addresses */
                    uint32_t sp = regs->useresp;
                    for (uint32_t w = 0; w < 64 && sp + w * 4 < 0xC0000000u; w += 8) {
                        char row[96] = "  stack ";
                        for (uint32_t k = w; k < w + 8; k++) {
                            sprintf_u32(h, ((const uint32_t *)(uintptr_t)sp)[k]);
                            strcat(row, h); strcat(row, " ");
                        }
                        strcat(row, "\n");
                        serial_write(row);
                    }
                }
            }
        }
        return r;
    }

    (void)d;
    (void)e;
    (void)f;
    (void)regs;

    switch (syscall) {
        case SYS_EXIT:
            /* the process's files and any graphics mode are released by proc_exit() */
            proc_exit((int)a);

        case SYS_FORK:
            return -1;

        case SYS_READ: {
            int fd = (int)a;
            char *buf = (char *)(uintptr_t)b;
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
            const char *buf = (const char *)(uintptr_t)b;
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
            strncpy(path, (const char *)(uintptr_t)a, sizeof(path) - 1);
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
            int pid = syscall_spawn_user(a, b, c);
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
            if (addr < p->brk_base) addr = p->brk_base;
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

        case SYS_MMAP2:
            return syscall_mmap2(a, b, c, d, e, f);

        case SYS_MUNMAP:
            return syscall_munmap(a, b);

        case SYS_MPROTECT:
            return syscall_mprotect(a, b, c);

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
            struct tos_stat *st = (struct tos_stat *)(uintptr_t)b;
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
            strncpy(title, (const char *)(uintptr_t)a, sizeof(title) - 1);
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
            ((int *)(uintptr_t)a)[0] = x;
            ((int *)(uintptr_t)a)[1] = y;
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
            strncpy(app, (const char *)(uintptr_t)a, sizeof(app) - 1);
            app[sizeof(app) - 1] = 0;
            return (uint32_t)tos_open_app(app);
        }

        case SYS_NET_RESOLVE: {
            const char *host = (const char *)(uintptr_t)a;
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
            void *data = (void *)(uintptr_t)a;
            int len = (int)b;
            if (len <= 0 || !user_range_ok(a, (uint32_t)len)) return -1;
            int rc = tcp_send(data, len);
            return (rc == 0) ? len : -1;
        }

        case SYS_NET_RECV: {
            uint8_t *buf = (uint8_t *)(uintptr_t)a;
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
            flags = irq_save();
            if (bochs_set_mode(&gfx_dev, width, height, 32) != 0) {
                irq_restore(flags);
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
            irq_restore(flags);
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
            struct gfx_blit_args *args = (struct gfx_blit_args *)(uintptr_t)a;
            if (args->x < 0 || args->y < 0 || args->w <= 0 || args->h <= 0) return -1;
            if (args->x + args->w > gfx_dev.width || args->y + args->h > gfx_dev.height) return -1;
            /* args->pixels is a second, independent pointer read out of
             * ring3-controlled memory -- validating the struct itself
             * says nothing about where this points. Without this check
             * a ring3 program could point it at arbitrary kernel memory
             * and have this syscall copy it straight into the
             * framebuffer, an arbitrary kernel-read primitive. */
            if (!user_range_ok(args->pixels, (uint32_t)args->w * (uint32_t)args->h * 4))
                return -1;
            uint32_t *fb = (uint32_t *)P2V(gfx_dev.lfb);
            for (int row = 0; row < args->h; row++) {
                memcpy(fb + (args->y + row) * gfx_dev.width + args->x,
                       (const uint32_t *)UP(args->pixels) + row * args->w,
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
            const uint8_t *buf = (const uint8_t *)(uintptr_t)a;
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
            struct inflate_args *args = (struct inflate_args *)(uintptr_t)a;
            if (!user_range_ok(args->src, args->src_len)) return (uint32_t)-1;
            if (!user_wr_ok(args->out, args->out_cap)) return (uint32_t)-1;
            if (!user_wr_ok(args->out_len, sizeof(uint32_t))) return (uint32_t)-1;
            uint32_t out_len = 0;
            int rc = inflate_raw_buffer(UP(args->src), args->src_len, UP(args->out), args->out_cap, &out_len);
            if (rc != 0) return (uint32_t)-1;
            *(uint32_t *)UP(args->out_len) = out_len;
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
            uint8_t *buf = (uint8_t *)(uintptr_t)a;
            int len = (int)b;
            for (int i = 0; i < len; i++) buf[i] = prng_syscall_byte();
            return 0;
        }

        case SYS_CRYPTO_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hash_args))) return (uint32_t)-1;
            struct crypto_hash_args *args = (struct crypto_hash_args *)(uintptr_t)a;
            if (!user_range_ok(args->data, args->len)) return (uint32_t)-1;
            if (!user_wr_ok(args->out, 32)) return (uint32_t)-1;
            sha256_hash(UP(args->data), args->len, UP(args->out));
            return 0;
        }

        case SYS_CRYPTO_HMAC_SHA256: {
            if (!user_range_ok(a, sizeof(struct crypto_hmac_args))) return (uint32_t)-1;
            struct crypto_hmac_args *args = (struct crypto_hmac_args *)(uintptr_t)a;
            if (!user_range_ok(args->key, args->klen)) return (uint32_t)-1;
            if (!user_range_ok(args->msg, args->mlen)) return (uint32_t)-1;
            if (!user_wr_ok(args->out, 32)) return (uint32_t)-1;
            hmac_sha256(UP(args->key), args->klen, UP(args->msg), args->mlen, UP(args->out));
            return 0;
        }

        case SYS_CRYPTO_AES128_CTR: {
            if (!user_range_ok(a, sizeof(struct crypto_aesctr_args))) return (uint32_t)-1;
            struct crypto_aesctr_args *args = (struct crypto_aesctr_args *)(uintptr_t)a;
            if (!user_range_ok(args->key16, 16)) return (uint32_t)-1;
            if (!user_range_ok(args->iv16, 16)) return (uint32_t)-1;
            if (!user_range_ok(args->in, args->len)) return (uint32_t)-1;
            if (!user_wr_ok(args->out, args->len)) return (uint32_t)-1;
            aes128_ctr_crypt(UP(args->key16), UP(args->iv16), UP(args->in), UP(args->out), args->len);
            return 0;
        }

        case SYS_CRYPTO_MODEXP: {
            if (!user_range_ok(a, sizeof(struct crypto_modexp_args))) return (uint32_t)-1;
            struct crypto_modexp_args *args = (struct crypto_modexp_args *)(uintptr_t)a;
            if (!user_range_ok(args->base256, 256)) return (uint32_t)-1;
            if (!user_range_ok(args->exp, args->exp_len)) return (uint32_t)-1;
            if (!user_range_ok(args->mod256, 256)) return (uint32_t)-1;
            if (!user_wr_ok(args->out256, 256)) return (uint32_t)-1;
            bignum_modexp(UP(args->base256), UP(args->exp), args->exp_len, UP(args->mod256), UP(args->out256));
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
