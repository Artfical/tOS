/* The terminal a Linux program finds on descriptors 0, 1 and 2: termios, the line discipline
 * (cooked lines with editing, or raw bytes), the window size, ^C, and what the keyboard sends as
 * xterm key sequences. The screen side is vt.c; the state lives in the terminal window's surface
 * (terminal.h), so each window has a terminal of its own. Without a window (the text screen, a
 * self-test) a program gets a private one with the same defaults. */
#include "tty.h"
#include "terminal.h"
#include "keyboard.h"
#include "proc.h"
#include "scheduler.h"
#include "errno.h"
#include "string.h"
#include "serial.h"
#include "debugmon.h"

/* termios flags (Linux) */
#define IGNCR  0x080
#define ICRNL  0x100
#define INLCR  0x040
#define ISIG   0x01
#define ICANON 0x02
#define ECHO   0x08
#define ECHOE  0x10
#define ECHOK  0x20
#define ECHONL 0x40
#define ECHOCTL 0x200
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6, VSUSP = 10, VEOL = 11, VWERASE = 14 };

static term_surface_t direct_surface;
static int direct_ready;

static term_surface_t *surface(void)
{
    term_surface_t *s = terminal_current_surface();
    if (s) return s;
    if (!direct_ready) { terminal_surface_init(&direct_surface); direct_ready = 1; }
    return &direct_surface;
}

/* the first time a Linux program uses the terminal: it speaks UTF-8 and wants xterm's key sequences */
static void attach(term_surface_t *s)
{
    if (s != &direct_surface && !s->xterm_keys) {
        s->utf8 = 1;
        s->xterm_keys = 1;
        keyboard_set_xterm(1);
    }
}

void tty_detach(void *surf)
{
    term_surface_t *s = surf;
    if (!s) return;
    vt_reset_modes(s);
    keyboard_set_xterm(0);
}

/* ---------------------------------------------------------------- input */

static void inq_put(tty_t *t, uint8_t c)
{
    uint16_t next = (uint16_t)((t->inq_head + 1) % TTY_INQ);
    if (next == t->inq_tail) return;                      /* full: the byte is lost, as a real tty's is */
    t->inq[t->inq_head] = c;
    t->inq_head = next;
}

static int inq_len(const tty_t *t) { return (t->inq_head - t->inq_tail + TTY_INQ) % TTY_INQ; }

static void echo_byte(term_surface_t *s, uint8_t c)
{
    if (c < 0x20 && c != '\n' && c != '\t' && (s->tty.c_lflag & ECHOCTL)) {
        terminal_putchar('^');
        terminal_putchar((char)(c + 64));
    } else {
        terminal_putchar((char)c);
    }
}

static void erase_echo(term_surface_t *s, int n)
{
    if (!(s->tty.c_lflag & ECHO)) return;
    for (int i = 0; i < n; i++) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
}

static void signal_group(term_surface_t *s, int sig)
{
    if (s == &direct_surface) { proc_t *p = proc_current(); if (p) proc_signal(p, sig); return; }
    proc_signal_surface(s, sig);
}

/* one byte from the keyboard through the line discipline */
static void discipline(term_surface_t *s, uint8_t c, int is_key_seq)
{
    tty_t *t = &s->tty;
    if (is_key_seq) {                                     /* an arrow, F-key...: only a program reading raw can use it */
        if (!(t->c_lflag & ICANON)) inq_put(t, c);
        return;
    }
    if (c == '\r') {
        if (t->c_iflag & IGNCR) return;
        if (t->c_iflag & ICRNL) c = '\n';
    } else if (c == '\n' && (t->c_iflag & INLCR)) c = '\r';

    if (t->c_lflag & ISIG) {
        int sig = c == t->c_cc[VINTR] ? 2 : c == t->c_cc[VQUIT] ? 3 : 0;
        if (sig && c) {
            t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0;
            if (t->c_lflag & ECHO) { echo_byte(s, c); terminal_putchar('\n'); }
            signal_group(s, sig);
            return;
        }
        if (c == t->c_cc[VSUSP] && c) return;             /* stopping is not supported */
    }
    if (!(t->c_lflag & ICANON)) {
        inq_put(t, c);
        if (t->c_lflag & ECHO) echo_byte(s, c);
        return;
    }
    /* canonical: edit the line until it is complete */
    if (c == t->c_cc[VERASE] && c) {
        if (t->line_len) { t->line_len--; erase_echo(s, 1); }
        return;
    }
    if (c == t->c_cc[VKILL] && c) { erase_echo(s, t->line_len); t->line_len = 0; return; }
    if (c == t->c_cc[VWERASE] && c) {
        int n = 0;
        while (t->line_len && t->line[t->line_len - 1] == ' ') { t->line_len--; n++; }
        while (t->line_len && t->line[t->line_len - 1] != ' ') { t->line_len--; n++; }
        erase_echo(s, n);
        return;
    }
    if (c == t->c_cc[VEOF] && c) {
        if (!t->line_len) t->eof_pending = 1;
        else { for (int i = 0; i < t->line_len; i++) inq_put(t, t->line[i]); t->line_len = 0; t->line_done = 1; }
        return;
    }
    if (c == '\n' || (c == t->c_cc[VEOL] && c)) {
        if (t->line_len < sizeof(t->line)) t->line[t->line_len++] = c;
        if (t->c_lflag & (ECHO | ECHONL)) terminal_putchar('\n');
        for (int i = 0; i < t->line_len; i++) inq_put(t, t->line[i]);
        t->line_len = 0;
        t->line_done = 1;
        return;
    }
    if (t->line_len < sizeof(t->line) - 1) {
        t->line[t->line_len++] = c;
        if (t->c_lflag & ECHO) echo_byte(s, c);
    }
}

/* the key sequences xterm sends; the keyboard left (0xFF, key, modifiers) in its queue */
static int key_sequence(term_surface_t *s, uint8_t key, uint8_t mod, uint8_t *out)
{
    int n = 0;
    char fin = 0;
    int tilde = 0;
    int app = s->app_cursor;
    switch (key) {
        case XKEY_UP: fin = 'A'; break;
        case XKEY_DOWN: fin = 'B'; break;
        case XKEY_RIGHT: fin = 'C'; break;
        case XKEY_LEFT: fin = 'D'; break;
        case XKEY_HOME: fin = 'H'; break;
        case XKEY_END: fin = 'F'; break;
        case XKEY_INS: tilde = 2; break;
        case XKEY_DEL: tilde = 3; break;
        case XKEY_PGUP: tilde = 5; break;
        case XKEY_PGDN: tilde = 6; break;
        default:
            if (key >= XKEY_F1 && key <= XKEY_F4) { fin = (char)('P' + key - XKEY_F1); app = 1; break; }   /* SS3 P..S */
            if (key >= XKEY_F5 && key <= XKEY_F12) {
                static const uint8_t f[8] = { 15, 17, 18, 19, 20, 21, 23, 24 };
                tilde = f[key - XKEY_F5];
                break;
            }
            return 0;
    }
    out[n++] = 0x1B;
    if (fin) {
        if (mod) { out[n++] = '['; out[n++] = '1'; out[n++] = ';'; out[n++] = (uint8_t)('1' + mod); }
        else out[n++] = app ? 'O' : '[';
        out[n++] = (uint8_t)fin;
    } else {
        out[n++] = '[';
        if (tilde >= 10) out[n++] = (uint8_t)('0' + tilde / 10);
        out[n++] = (uint8_t)('0' + tilde % 10);
        if (mod) { out[n++] = ';'; out[n++] = (uint8_t)('1' + mod); }
        out[n++] = '~';
    }
    return n;
}

/* everything the keyboard and the terminal itself have for the program so far */
static void pump(term_surface_t *s)
{
    uint8_t c;
    while (s->reply_len) {                                /* answers to its queries (cursor position...) */
        uint8_t n = s->reply_len;
        s->reply_len = 0;
        for (int i = 0; i < n; i++) inq_put(&s->tty, s->reply[i]);
    }
    if (s == &direct_surface) return;                     /* no window: no keyboard of its own */
    while (keyboard_tty_poll(&c)) {
        if (c == 0xFF) {
            uint8_t key = 0, mod = 0, seq[8];
            keyboard_tty_poll(&key);
            keyboard_tty_poll(&mod);
            int n = key_sequence(s, key, mod, seq);
            for (int i = 0; i < n; i++) discipline(s, seq[i], 1);
            continue;
        }
        if (c == '\b') c = 0x7F;                          /* a keyboard's Backspace is DEL to a terminal */
        if (c == '\n') c = '\r';                          /* and Enter is CR (ICRNL turns it back in cooked mode) */
        discipline(s, c, 0);
    }
}

static int signal_waiting(void)
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    return p && t && (p->sig_pending & ~t->sig_blocked);
}

/* bytes a read could return now; canonical mode only counts completed lines */
static int readable(term_surface_t *s)
{
    tty_t *t = &s->tty;
    if (t->c_lflag & ICANON) return (t->line_done && inq_len(t)) ? inq_len(t) : (t->eof_pending ? 1 : 0);
    return inq_len(t);
}

int tty_ready(void)
{
    term_surface_t *s = surface();
    attach(s);
    pump(s);
    return readable(s) != 0;
}

/* read(0): returns bytes, 0 at end of file (^D on an empty line), or a negative errno */
int tty_read(uint8_t *buf, uint32_t count, int nonblock)
{
    term_surface_t *s = surface();
    tty_t *t = &s->tty;
    attach(s);
    if (!t->fg_pid) { proc_t *p = proc_current(); if (p) t->fg_pid = p->pid; }
    uint64_t start = debugmon_uptime_ms64();
    int got_first = 0;
    uint64_t first_at = 0;
    for (;;) {
        pump(s);
        int avail = readable(s);
        if (t->c_lflag & ICANON) {
            if (avail) break;
        } else {
            uint32_t vmin = t->c_cc[VMIN], vtime = t->c_cc[VTIME];
            if (vmin > count) vmin = count;
            if (avail && !got_first) { got_first = 1; first_at = debugmon_uptime_ms64(); }
            if (avail >= (int)vmin && (avail || vmin)) { if (vmin || !vtime) break; }
            if (!vmin && !vtime) break;                   /* polling: whatever is there, maybe nothing */
            if (vtime) {
                uint64_t now = debugmon_uptime_ms64();
                if (vmin == 0 && now - start >= vtime * 100u) break;           /* the first byte did not come */
                if (vmin && got_first && now - first_at >= vtime * 100u) break;  /* the bytes after it stopped */
            }
        }
        if (nonblock) return -EAGAIN;
        if (signal_waiting()) return -EINTR;
        task_sleep(10);
    }
    if ((t->c_lflag & ICANON) && !inq_len(t)) {           /* ^D on an empty line */
        t->eof_pending = 0;
        return 0;
    }
    uint32_t n = 0;
    while (n < count && t->inq_tail != t->inq_head) {
        buf[n++] = t->inq[t->inq_tail];
        t->inq_tail = (uint16_t)((t->inq_tail + 1) % TTY_INQ);
        if ((t->c_lflag & ICANON) && buf[n - 1] == '\n') break;     /* one line per read */
    }
    if ((t->c_lflag & ICANON) && !inq_len(t)) t->line_done = 0;
    return (int)n;
}

/* ---------------------------------------------------------------- output */

void tty_note_output(void)
{
    attach(surface());
}

/* ---------------------------------------------------------------- ioctl */

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429

/* what Linux's struct termios holds on x86: four flag words, the line discipline, 19 control characters */
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[TTY_NCC];
} __attribute__((packed)) k_termios_t;

/* 0 = done, a negative errno, or 1 when the request is not a terminal one */
int tty_ioctl(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t), int (*rd_ok)(uint64_t, uint64_t))
{
    term_surface_t *s = surface();
    tty_t *t = &s->tty;
    switch (req) {
        case TCGETS: {
            if (!wr_ok(arg, sizeof(k_termios_t))) return -EFAULT;
            k_termios_t k;
            k.c_iflag = t->c_iflag; k.c_oflag = t->c_oflag; k.c_cflag = t->c_cflag; k.c_lflag = t->c_lflag;
            k.c_line = 0;
            memcpy(k.c_cc, t->c_cc, TTY_NCC);
            memcpy((void *)(uintptr_t)arg, &k, sizeof(k));
            attach(s);
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
            attach(s);
            return 0;
        }
        case TIOCGWINSZ: {
            if (!wr_ok(arg, 8)) return -EFAULT;
            uint16_t ws[4] = { t->ws_row, t->ws_col, (uint16_t)(t->ws_col * 9), (uint16_t)(t->ws_row * 18) };
            memcpy((void *)(uintptr_t)arg, ws, 8);
            return 0;
        }
        case TIOCSWINSZ: {
            if (!rd_ok(arg, 8)) return -EFAULT;
            return 0;                                      /* the window decides its size */
        }
        case FIONREAD: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            pump(s);
            int n = readable(s);
            memcpy((void *)(uintptr_t)arg, &n, 4);
            return 0;
        }
        case TIOCGPGRP: case TIOCGSID: {
            if (!wr_ok(arg, 4)) return -EFAULT;
            proc_t *p = proc_current();
            uint32_t g = t->fg_pid ? t->fg_pid : (p ? p->pid : 1);
            memcpy((void *)(uintptr_t)arg, &g, 4);
            return 0;
        }
        case TIOCSPGRP: {
            if (!rd_ok(arg, 4)) return -EFAULT;
            memcpy(&t->fg_pid, (const void *)(uintptr_t)arg, 4);
            return 0;
        }
        case TIOCSCTTY: case TIOCNOTTY: case TCSBRK: case TCXONC: case TIOCOUTQ:
            return 0;
        case TCFLSH:
            if (arg == 0 || arg == 2) { t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0; }
            return 0;
        default:
            return 1;
    }
}
