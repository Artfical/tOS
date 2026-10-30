#include "pty.h"
#include "terminal.h"
#include "proc.h"
#include "scheduler.h"
#include "errno.h"
#include "string.h"
#include "debugmon.h"

/* the line discipline here is the one tty.c has for the terminal windows, working on a pair's own state */

#define IGNCR  0x080
#define ICRNL  0x100
#define INLCR  0x040
#define ISIG   0x01
#define ICANON 0x02
#define ECHO   0x08
#define ECHOE  0x10
#define ECHONL 0x40
#define ECHOCTL 0x200
#define OPOST  0x01
#define ONLCR  0x04
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6, VSUSP = 10, VEOL = 11, VWERASE = 14 };

#define OUTQ 16384

typedef struct {
    int used, locked;
    int mrefs, srefs;
    int slave_seen;                      /* the slave has been opened once (a master read waits until then) */
    tty_t t;
    uint8_t out[OUTQ];                   /* from the slave's program to the master */
    uint16_t out_head, out_tail;
} pty_t;

static pty_t ptys[PTY_MAX];

static pty_t *get(int n) { return n >= 0 && n < PTY_MAX && ptys[n].used ? &ptys[n] : 0; }

static int out_len(const pty_t *p) { return (p->out_head - p->out_tail + OUTQ) % OUTQ; }
static int out_space(const pty_t *p) { return OUTQ - 1 - out_len(p); }

static void out_put(pty_t *p, uint8_t c)
{
    uint16_t next = (uint16_t)((p->out_head + 1) % OUTQ);
    if (next == p->out_tail) return;
    p->out[p->out_head] = c;
    p->out_head = next;
}

/* output processing: \n becomes \r\n */
static void out_processed(pty_t *p, uint8_t c)
{
    if (c == '\n' && (p->t.c_oflag & OPOST) && (p->t.c_oflag & ONLCR)) out_put(p, '\r');
    out_put(p, c);
}

static void inq_put(tty_t *t, uint8_t c)
{
    uint16_t next = (uint16_t)((t->inq_head + 1) % TTY_INQ);
    if (next == t->inq_tail) return;
    t->inq[t->inq_head] = c;
    t->inq_head = next;
}

static int inq_len(const tty_t *t) { return (t->inq_head - t->inq_tail + TTY_INQ) % TTY_INQ; }

static void echo_byte(pty_t *p, uint8_t c)
{
    if (c < 0x20 && c != '\n' && c != '\t' && (p->t.c_lflag & ECHOCTL)) {
        out_put(p, '^');
        out_put(p, (uint8_t)(c + 64));
    } else {
        out_processed(p, c);
    }
}

static void erase_echo(pty_t *p, int n)
{
    if (!(p->t.c_lflag & ECHO)) return;
    for (int i = 0; i < n; i++) { out_put(p, '\b'); out_put(p, ' '); out_put(p, '\b'); }
}

/* a signal for every program that has the slave open */
static void signal_slave(int n, int sig)
{
    proc_signal_pty(n, sig);
}

/* one byte the master wrote, through the line discipline to the program */
static void discipline(int n, pty_t *p, uint8_t c)
{
    tty_t *t = &p->t;
    if (c == '\r') {
        if (t->c_iflag & IGNCR) return;
        if (t->c_iflag & ICRNL) c = '\n';
    } else if (c == '\n' && (t->c_iflag & INLCR)) c = '\r';

    if (t->c_lflag & ISIG) {
        int sig = c == t->c_cc[VINTR] ? 2 : c == t->c_cc[VQUIT] ? 3 : 0;
        if (sig && c) {
            t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0;
            if (t->c_lflag & ECHO) { echo_byte(p, c); out_processed(p, '\n'); }
            signal_slave(n, sig);
            return;
        }
        if (c == t->c_cc[VSUSP] && c) return;
    }
    if (!(t->c_lflag & ICANON)) {
        inq_put(t, c);
        if (t->c_lflag & ECHO) echo_byte(p, c);
        return;
    }
    if (c == t->c_cc[VERASE] && c) {
        if (t->line_len) { t->line_len--; erase_echo(p, 1); }
        return;
    }
    if (c == t->c_cc[VKILL] && c) { erase_echo(p, t->line_len); t->line_len = 0; return; }
    if (c == t->c_cc[VWERASE] && c) {
        int k = 0;
        while (t->line_len && t->line[t->line_len - 1] == ' ') { t->line_len--; k++; }
        while (t->line_len && t->line[t->line_len - 1] != ' ') { t->line_len--; k++; }
        erase_echo(p, k);
        return;
    }
    if (c == t->c_cc[VEOF] && c) {
        if (!t->line_len) t->eof_pending = 1;
        else { for (int i = 0; i < t->line_len; i++) inq_put(t, t->line[i]); t->line_len = 0; t->line_done = 1; }
        return;
    }
    if (c == '\n' || (c == t->c_cc[VEOL] && c)) {
        if (t->line_len < sizeof(t->line)) t->line[t->line_len++] = c;
        if (t->c_lflag & (ECHO | ECHONL)) out_processed(p, '\n');
        for (int i = 0; i < t->line_len; i++) inq_put(t, t->line[i]);
        t->line_len = 0;
        t->line_done = 1;
        return;
    }
    if (t->line_len < sizeof(t->line) - 1) {
        t->line[t->line_len++] = c;
        if (t->c_lflag & ECHO) echo_byte(p, c);
    }
}

static int readable(pty_t *p)
{
    tty_t *t = &p->t;
    if (t->c_lflag & ICANON) return (t->line_done && inq_len(t)) ? inq_len(t) : (t->eof_pending ? 1 : 0);
    return inq_len(t);
}

static int signal_waiting(void)
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    return p && t && (p->sig_pending & ~t->sig_blocked);
}

int pty_new(void)
{
    for (int i = 0; i < PTY_MAX; i++) {
        if (ptys[i].used) continue;
        pty_t *p = &ptys[i];
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->locked = 1;
        p->mrefs = 1;
        tty_t *t = &p->t;
        t->c_iflag = 0x0502; t->c_oflag = 0x0005; t->c_cflag = 0x04BF; t->c_lflag = 0x8A3B;
        t->c_cc[0] = 3; t->c_cc[1] = 0x1C; t->c_cc[2] = 0x7F; t->c_cc[3] = 0x15; t->c_cc[4] = 4;
        t->c_cc[6] = 1; t->c_cc[8] = 0x11; t->c_cc[9] = 0x13; t->c_cc[10] = 0x1A; t->c_cc[12] = 0x12;
        t->c_cc[13] = 0x0F; t->c_cc[14] = 0x17; t->c_cc[15] = 0x16;
        t->ws_row = 24; t->ws_col = 80;
        return i;
    }
    return -ENOSPC;
}

int pty_exists(int n) { return get(n) != 0; }

int pty_slave_open(int n)
{
    pty_t *p = get(n);
    if (!p) return -ENOENT;
    if (p->locked) return -EIO;
    p->srefs++;
    p->slave_seen = 1;
    return 0;
}

void pty_ref(int n, int slave)
{
    pty_t *p = get(n);
    if (!p) return;
    if (slave) p->srefs++; else p->mrefs++;
}

void pty_close(int n, int slave)
{
    pty_t *p = get(n);
    if (!p) return;
    if (slave) { if (p->srefs > 0) p->srefs--; }
    else {
        if (p->mrefs > 0) p->mrefs--;
        if (p->mrefs == 0 && p->srefs > 0) signal_slave(n, 1);        /* the line dropped: SIGHUP */
    }
    if (p->mrefs == 0 && p->srefs == 0) p->used = 0;
}

int pty_read(int n, int slave, uint8_t *buf, uint32_t count, int nonblock)
{
    pty_t *p = get(n);
    if (!p) return -EIO;
    if (!slave) {                                                /* the master: what the program wrote */
        for (;;) {
            int len = out_len(p);
            if (len) {
                uint32_t k = 0;
                while (k < count && p->out_tail != p->out_head) {
                    buf[k++] = p->out[p->out_tail];
                    p->out_tail = (uint16_t)((p->out_tail + 1) % OUTQ);
                }
                return (int)k;
            }
            if (p->slave_seen && p->srefs == 0) return -EIO;     /* the program is gone */
            if (nonblock) return -EAGAIN;
            if (signal_waiting()) return -EINTR;
            task_yield();
            p = get(n);
            if (!p) return -EIO;
        }
    }
    tty_t *t = &p->t;                                            /* the slave: what the master wrote */
    uint64_t start = debugmon_uptime_ms64();
    int got_first = 0;
    uint64_t first_at = 0;
    for (;;) {
        int avail = readable(p);
        if (t->c_lflag & ICANON) {
            if (avail) break;
        } else {
            uint32_t vmin = t->c_cc[VMIN], vtime = t->c_cc[VTIME];
            if (vmin > count) vmin = count;
            if (avail && !got_first) { got_first = 1; first_at = debugmon_uptime_ms64(); }
            if (avail >= (int)vmin && (avail || vmin)) { if (vmin || !vtime) break; }
            if (!vmin && !vtime) break;
            if (vtime) {
                uint64_t now = debugmon_uptime_ms64();
                if (vmin == 0 && now - start >= vtime * 100u) break;
                if (vmin && got_first && now - first_at >= vtime * 100u) break;
            }
        }
        if (p->mrefs == 0) return 0;                             /* hung up */
        if (nonblock) return -EAGAIN;
        if (signal_waiting()) return -EINTR;
        task_yield();
        p = get(n);
        if (!p) return 0;
        t = &p->t;
    }
    if ((t->c_lflag & ICANON) && !inq_len(t)) { t->eof_pending = 0; return 0; }
    uint32_t k = 0;
    while (k < count && t->inq_tail != t->inq_head) {
        buf[k++] = t->inq[t->inq_tail];
        t->inq_tail = (uint16_t)((t->inq_tail + 1) % TTY_INQ);
        if ((t->c_lflag & ICANON) && buf[k - 1] == '\n') break;
    }
    if ((t->c_lflag & ICANON) && !inq_len(t)) t->line_done = 0;
    return (int)k;
}

int pty_write(int n, int slave, const uint8_t *buf, uint32_t count, int nonblock)
{
    pty_t *p = get(n);
    if (!p) return -EIO;
    if (!slave) {                                                /* the master types */
        if (p->srefs == 0 && p->slave_seen) return -EIO;
        for (uint32_t i = 0; i < count; i++) {
            if (out_space(p) < 8) {                              /* echo needs room: wait for the reader */
                if (nonblock) return i ? (int)i : -EAGAIN;
                while (out_space(p) < 8) {
                    if (signal_waiting()) return i ? (int)i : -EINTR;
                    task_yield();
                    p = get(n);
                    if (!p) return -EIO;
                }
            }
            discipline(n, p, buf[i]);
        }
        return (int)count;
    }
    uint32_t i = 0;                                              /* the program writes */
    while (i < count) {
        if (p->mrefs == 0) return i ? (int)i : -EIO;
        if (out_space(p) < 2) {
            if (nonblock) return i ? (int)i : -EAGAIN;
            if (signal_waiting()) return i ? (int)i : -EINTR;
            task_yield();
            p = get(n);
            if (!p) return -EIO;
            continue;
        }
        out_processed(p, buf[i++]);
    }
    return (int)count;
}

uint32_t pty_ready(int n, int slave)
{
    pty_t *p = get(n);
    if (!p) return 0x10;
    uint32_t r = 0;
    if (!slave) {
        if (out_len(p)) r |= 0x001;
        if (p->slave_seen && p->srefs == 0) r |= 0x001 | 0x010;
        if (out_space(p) >= 8) r |= 0x004;
        return r;
    }
    if (readable(p)) r |= 0x001;
    if (p->mrefs == 0) r |= 0x001 | 0x010;
    if (out_space(p) >= 2) r |= 0x004;
    return r;
}

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCEXCL   0x540C
#define TIOCNXCL   0x540D
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCPKT    0x5420
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define TIOCGPTLCK 0x80045439

typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[TTY_NCC];
} __attribute__((packed)) k_termios_t;

int pty_ioctl(int n, int slave, uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t), int (*rd_ok)(uint64_t, uint64_t))
{
    (void)slave;
    pty_t *p = get(n);
    if (!p) return -EIO;
    tty_t *t = &p->t;
    switch (req) {
        case TCGETS: {
            if (!wr_ok(arg, sizeof(k_termios_t))) return -EFAULT;
            k_termios_t k;
            k.c_iflag = t->c_iflag; k.c_oflag = t->c_oflag; k.c_cflag = t->c_cflag; k.c_lflag = t->c_lflag;
            k.c_line = 0;
            memcpy(k.c_cc, t->c_cc, TTY_NCC);
            memcpy((void *)(uintptr_t)arg, &k, sizeof(k));
            return 0;
        }
        case TCSETS: case TCSETSW: case TCSETSF: {
            if (!rd_ok(arg, sizeof(k_termios_t))) return -EFAULT;
            k_termios_t k;
            memcpy(&k, (const void *)(uintptr_t)arg, sizeof(k));
            int was_canon = (t->c_lflag & ICANON) != 0;
            t->c_iflag = k.c_iflag; t->c_oflag = k.c_oflag; t->c_cflag = k.c_cflag; t->c_lflag = k.c_lflag;
            memcpy(t->c_cc, k.c_cc, TTY_NCC);
            if (req == TCSETSF || was_canon != ((t->c_lflag & ICANON) != 0)) {
                t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0; t->eof_pending = 0;
            }
            return 0;
        }
        case TIOCGWINSZ: {
            if (!wr_ok(arg, 8)) return -EFAULT;
            uint16_t ws[4] = { t->ws_row, t->ws_col, 0, 0 };
            memcpy((void *)(uintptr_t)arg, ws, 8);
            return 0;
        }
        case TIOCSWINSZ: {
            if (!rd_ok(arg, 8)) return -EFAULT;
            uint16_t ws[4];
            memcpy(ws, (const void *)(uintptr_t)arg, 8);
            if (ws[0] != t->ws_row || ws[1] != t->ws_col) { t->ws_row = ws[0]; t->ws_col = ws[1]; signal_slave(n, 28); }   /* SIGWINCH */
            return 0;
        }
        case FIONREAD: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            int k = slave ? readable(p) : out_len(p);
            memcpy((void *)(uintptr_t)arg, &k, 4);
            return 0;
        }
        case TIOCGPGRP: case TIOCGSID: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            proc_t *pr = proc_current();
            uint32_t g = t->fg_pid ? t->fg_pid : (pr ? pr->pid : 1);
            memcpy((void *)(uintptr_t)arg, &g, 4);
            return 0;
        }
        case TIOCSPGRP: {
            if (!rd_ok(arg, 4)) return -EFAULT;
            memcpy(&t->fg_pid, (const void *)(uintptr_t)arg, 4);
            return 0;
        }
        case TIOCGPTN: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            uint32_t v = (uint32_t)n;
            memcpy((void *)(uintptr_t)arg, &v, 4);
            return 0;
        }
        case TIOCSPTLCK: {
            if (!rd_ok(arg, 4)) return -EFAULT;
            int v;
            memcpy(&v, (const void *)(uintptr_t)arg, 4);
            p->locked = v != 0;
            return 0;
        }
        case TIOCGPTLCK: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            uint32_t v = (uint32_t)p->locked;
            memcpy((void *)(uintptr_t)arg, &v, 4);
            return 0;
        }
        case TIOCSCTTY: case TIOCNOTTY: case TCSBRK: case TCXONC: case TIOCOUTQ: case TIOCEXCL: case TIOCNXCL: case TIOCPKT:
            return 0;
        case TCFLSH:
            if (arg == 0 || arg == 2) { t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0; }
            return 0;
        default:
            return 1;
    }
}
