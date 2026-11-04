#include "../uiapp.h"
#include "../uihost.h"
#include "../fbwin.h"
#include "string.h"
#include "memory.h"
#include "paging.h"

/* Linux programs' window: a bitmap of shared pages (800 x 560, 32 bits, one page per row) the program draws in through
 * /dev/fb0, and an evdev device that tells it about the pointer and the keys. */

extern int wm_gfx_open_native(const char *id, const char *arg);
extern void uihost_close_window(void *app);

typedef struct { uint16_t type, code; int32_t value; } ev_t;
#define EVQ 512

typedef struct {
    uint32_t pg[TFBW_H];
    ev_t q[2][EVQ];                                   /* 0: the pointer (event0), 1: the keyboard (event1) */
    int head[2], tail[2];
    uint32_t owner;
    uiapp_t *app;
    int closed;
    uint32_t last_ms;
    int mx, my;
} fbw_t;

static fbw_t *cur;                                    /* there is one such window at a time */

/* ---- the events ---- */
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

static int cur_dev;                                   /* the queue push() fills */
static void push(fbw_t *f, int type, int code, int value)
{
    int d = cur_dev;
    int next = (f->head[d] + 1) % EVQ;
    if (next == f->tail[d]) return;                   /* full: the oldest stay, the newest are lost (nobody reads) */
    f->q[d][f->head[d]].type = (uint16_t)type; f->q[d][f->head[d]].code = (uint16_t)code; f->q[d][f->head[d]].value = value;
    f->head[d] = next;
}
static void sync(fbw_t *f) { push(f, EV_SYN, 0, 0); }

static void pointer(fbw_t *f, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= TFBW_W) x = TFBW_W - 1;
    if (y >= TFBW_H) y = TFBW_H - 1;
    if (x != f->mx) { push(f, EV_ABS, 0, x); f->mx = x; }
    if (y != f->my) { push(f, EV_ABS, 1, y); f->my = y; }
}

static void fb_press(uiapp_t *a, int x, int y, int button)
{
    fbw_t *f = a->st;
    cur_dev = 0;
    pointer(f, x, y);
    push(f, EV_KEY, button == 2 ? BTN_RIGHT : BTN_LEFT, 1);
    sync(f);
}
static void fb_drag(uiapp_t *a, int x, int y) { fbw_t *f = a->st; cur_dev = 0; pointer(f, x, y); sync(f); }
static void fb_release(uiapp_t *a, int x, int y)
{
    fbw_t *f = a->st;
    cur_dev = 0;
    pointer(f, x, y);
    push(f, EV_KEY, BTN_LEFT, 0);
    push(f, EV_KEY, BTN_RIGHT, 0);
    sync(f);
}
static void fb_hover(uiapp_t *a, int x, int y)
{
    fbw_t *f = a->st;
    cur_dev = 0;
    if (x < 0 || y < 0) return;
    pointer(f, x, y);
    sync(f);
}
static void fb_wheel(uiapp_t *a, int delta)
{
    fbw_t *f = a->st;
    cur_dev = 0;
    push(f, EV_REL, 8, delta);                        /* REL_WHEEL */
    sync(f);
}

/* ASCII to Linux key codes (US layout): the code, and whether Shift is held for it */
static int key_of(int ch, int *shift)
{
    static const char row1[] = "1234567890";          /* KEY_1.. = 2.. */
    static const char row2[] = "qwertyuiop";          /* 16.. */
    static const char row3[] = "asdfghjkl";           /* 30.. */
    static const char row4[] = "zxcvbnm";             /* 44.. */
    static const char sym_un[] = "-=[];'`\\,./";
    static const int sym_code[] = { 12, 13, 26, 27, 39, 40, 41, 43, 51, 52, 53 };
    static const char sym_sh[] = "_+{}:\"~|<>?";
    static const char num_sh[] = "!@#$%^&*()";
    *shift = 0;
    if (ch >= 'A' && ch <= 'Z') { *shift = 1; ch += 32; }
    for (int i = 0; i < 10; i++) { if (ch == row1[i]) return 2 + i; if (ch == num_sh[i]) { *shift = 1; return 2 + i; } }
    for (int i = 0; i < 10; i++) if (ch == row2[i]) return 16 + i;
    for (int i = 0; i < 9; i++) if (ch == row3[i]) return 30 + i;
    for (int i = 0; i < 7; i++) if (ch == row4[i]) return 44 + i;
    for (int i = 0; i < 11; i++) { if (ch == sym_un[i]) return sym_code[i]; if (ch == sym_sh[i]) { *shift = 1; return sym_code[i]; } }
    switch (ch) {
    case ' ': return 57;
    case '\n': case '\r': return 28;
    case '\t': return 15;
    case 8: case 127: return 14;
    case 27: return 1;
    case UI_KEY_UP: return 103;
    case UI_KEY_DOWN: return 108;
    case UI_KEY_LEFT: return 105;
    case UI_KEY_RIGHT: return 106;
    }
    return 0;
}

static void fb_key(uiapp_t *a, int ch)
{
    fbw_t *f = a->st;
    cur_dev = 1;
    int shift, code = key_of(ch, &shift);
    if (!code) return;
    if (ch < 32 && ch != 8 && ch != 9 && ch != 10 && ch != 13 && ch != 27) {      /* Ctrl-letter: control held */
        push(f, EV_KEY, 29, 1); push(f, EV_KEY, 30 + 0, 0); push(f, EV_KEY, 29, 0);
        sync(f);
        return;
    }
    if (shift) push(f, EV_KEY, 42, 1);
    push(f, EV_KEY, code, 1); sync(f);
    push(f, EV_KEY, code, 0);
    if (shift) push(f, EV_KEY, 42, 0);
    sync(f);
}

/* ---- the window ---- */
static void fb_init(uiapp_t *a, const char *arg)
{
    (void)arg;
    fbw_t *f = a->st;
    f->app = a;
    for (int y = 0; y < TFBW_H; y++) {
        uint32_t ph = alloc_physical_page();
        if (!ph) break;
        memset(P2V(ph), 0, 4096);
        f->pg[y] = ph;
    }
    strncpy(a->title, "Linux program", sizeof(a->title) - 1);
    f->mx = f->my = -1;
    cur = f;
}

static void fb_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    fbw_t *f = a->st;
    gfx_fill(s, x, y, w, h, 0x000000);
    int c0 = x < s->cx0 ? s->cx0 - x : 0;                              /* the columns of the bitmap that are inside the clip */
    int c1 = TFBW_W < w ? TFBW_W : w;
    if (x + c1 > s->cx1) c1 = s->cx1 - x;
    if (c1 <= c0) return;
    for (int row = 0; row < TFBW_H && row < h; row++) {
        if (!f->pg[row]) continue;
        int dy = y + row;
        if (dy < s->cy0 || dy >= s->cy1) continue;
        memcpy(&s->px[dy * s->w + x + c0], (const uint32_t *)P2V(f->pg[row]) + c0, (size_t)(c1 - c0) * 4);   /* a row at a time */
    }
}

static int fb_tick(uiapp_t *a)
{
    fbw_t *f = a->st;
    uint32_t now = uihost_ms();
    if (now - f->last_ms < 66) return 0;
    f->last_ms = now;
    return 1;                                         /* the program draws whenever it likes: repaint ~15 times a second */
}

static void fb_close(uiapp_t *a)
{
    fbw_t *f = a->st;
    f->closed = 1;
    for (int y = 0; y < TFBW_H; y++) if (f->pg[y]) { free_physical_page(f->pg[y]); f->pg[y] = 0; }   /* the program's mappings keep theirs */
    if (cur == f) cur = 0;
}

const uiapp_class_t app_fbwin = {
    "fbwin", "Linux program", "terminal", TFBW_W, TFBW_H, 0, (int)sizeof(fbw_t),
    fb_init, fb_draw, fb_press, fb_drag, fb_release, fb_key, fb_wheel, fb_tick, fb_close,
    0, 0, 0, fb_hover, 0,
};

/* ---- for the system calls ---- */
int fbwin_open(void)
{
    if (cur && !cur->closed) return 0;
    if (wm_gfx_open_native("fbwin", 0) != 0) return -1;
    return cur ? 0 : -1;
}
int fbwin_active(void) { return cur && !cur->closed; }
uint32_t fbwin_page(int row) { return cur && row >= 0 && row < TFBW_H ? cur->pg[row] : 0; }
void fbwin_set_owner(uint32_t pid) { if (cur) cur->owner = pid; }
void fbwin_owner_gone(uint32_t pid)
{
    if (cur && !cur->closed && cur->owner == pid && cur->app) uihost_close_window(cur->app);
}

int fbwin_ev_ready(int dev) { return cur && cur->head[dev] != cur->tail[dev]; }

int fbwin_ev_read(int dev, uint8_t *out, int max_bytes)
{
    if (!cur) return 0;
    int n = 0;
    uint32_t ms = uihost_ms();
    while (cur->head[dev] != cur->tail[dev] && n + 24 <= max_bytes) {
        ev_t *e = &cur->q[dev][cur->tail[dev]];
        uint64_t sec = ms / 1000, usec = (uint64_t)(ms % 1000) * 1000;
        memcpy(out + n, &sec, 8); memcpy(out + n + 8, &usec, 8);
        memcpy(out + n + 16, &e->type, 2); memcpy(out + n + 18, &e->code, 2); memcpy(out + n + 20, &e->value, 4);
        n += 24;
        cur->tail[dev] = (cur->tail[dev] + 1) % EVQ;
    }
    return n;
}

/* struct fb_var_screeninfo (160 bytes) and fb_fix_screeninfo */
int fbwin_ioctl_fb(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t))
{
    switch (req) {
    case 0x4600: case 0x4601: {                       /* FBIOGET_VSCREENINFO, FBIOPUT_VSCREENINFO */
        if (!wr_ok(arg, 160)) return -14;
        uint32_t v[40];
        memset(v, 0, sizeof(v));
        v[0] = TFBW_W; v[1] = TFBW_H; v[2] = TFBW_STRIDE / 4; v[3] = TFBW_H;     /* xres, yres, virtual */
        v[6] = 32;                                    /* bits per pixel */
        v[8] = 16; v[9] = 8;                          /* red: offset 16, 8 bits */
        v[11] = 8; v[12] = 8;                         /* green */
        v[14] = 0; v[15] = 8;                         /* blue */
        v[17] = 24; v[18] = 0;                        /* transp */
        v[24] = 0xFFFFFFFFu; v[25] = 0xFFFFFFFFu;     /* height, width in mm: unknown */
        memcpy((void *)(uintptr_t)arg, v, 160);
        return 0;
    }
    case 0x4602: {                                    /* FBIOGET_FSCREENINFO */
        if (!wr_ok(arg, 80)) return -14;
        uint8_t b[80];
        memset(b, 0, sizeof(b));
        memcpy(b, "tOS window", 11);
        uint32_t smem_len = TFBW_STRIDE * TFBW_H;
        memcpy(b + 24, &smem_len, 4);                 /* smem_len (smem_start at 16 is 0: it is not a physical range) */
        uint32_t visual = 2;                          /* TRUECOLOR */
        memcpy(b + 32, &visual, 4);
        uint32_t ll = TFBW_STRIDE;
        memcpy(b + 48, &ll, 4);                       /* line_length */
        memcpy((void *)(uintptr_t)arg, b, 80);
        return 0;
    }
    case 0x4606: case 0x4611: case 0x4620: return 0;  /* FBIOPAN_DISPLAY, FBIOBLANK, FBIOPUT_CON2FBMAP */
    case 0x4604: case 0x4605: return 0;               /* FBIOGETCMAP / PUTCMAP */
    }
    return 1;
}

int fbwin_ioctl_vt(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t))
{
    switch (req) {
    case 0x5600: if (!wr_ok(arg, 4)) return -14; { int v = 1; memcpy((void *)(uintptr_t)arg, &v, 4); } return 0;                 /* VT_OPENQRY: vt 1 is free */
    case 0x5601: if (!wr_ok(arg, 8)) return -14; memset((void *)(uintptr_t)arg, 0, 8); return 0;                                    /* VT_GETMODE */
    case 0x5603: if (!wr_ok(arg, 6)) return -14; { uint16_t st[3] = { 1, 0, 2 }; memcpy((void *)(uintptr_t)arg, st, 6); } return 0;  /* VT_GETSTATE: vt 1 is active */
    case 0x5602: case 0x5605: case 0x5606: case 0x5607: case 0x4B3A: case 0x4B45: case 0x4B46: case 0x4B4E: case 0x4B4F: case 0x5604: case 0x560B: return 0;
    case 0x4B3B: if (!wr_ok(arg, 4)) return -14; { int v = 0; memcpy((void *)(uintptr_t)arg, &v, 4); } return 0;                 /* KDGETMODE: text */
    case 0x4B44: if (!wr_ok(arg, 4)) return -14; { int v = 3; memcpy((void *)(uintptr_t)arg, &v, 4); } return 0;                 /* KDGKBMODE: unicode */
    case 0x4B33: if (!wr_ok(arg, 1)) return -14; *(uint8_t *)(uintptr_t)arg = 2; return 0;                                          /* KDGKBTYPE: a 101-key keyboard */
    }
    return 1;
}

int fbwin_ioctl_ev(int dev, uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t))
{
    uint32_t dir_type = req >> 8 & 0xFF, nr = req & 0xFF, size = req >> 16 & 0x3FFF;
    if (dir_type != 'E') return 1;
    if (nr == 0x01) { if (!wr_ok(arg, 4)) return -14; uint32_t v = 0x010001; memcpy((void *)(uintptr_t)arg, &v, 4); return 0; }    /* EVIOCGVERSION */
    if (nr == 0x02) { if (!wr_ok(arg, 8)) return -14; uint16_t id[4] = { 6, 0x1234, 0x5678, 1 }; memcpy((void *)(uintptr_t)arg, id, 8); return 0; }   /* EVIOCGID: BUS_VIRTUAL */
    if (nr == 0x06 || nr == 0x07 || nr == 0x08) {     /* EVIOCGNAME, PHYS, UNIQ */
        if (!size || !wr_ok(arg, size)) return -14;
        const char *nm = nr == 0x06 ? (dev ? "tOS window keyboard" : "tOS window pointer") : nr == 0x07 ? (dev ? "tos/input1" : "tos/input0") : "";
        uint32_t l = (uint32_t)strlen(nm) + 1;
        if (l > size) l = size;
        memcpy((void *)(uintptr_t)arg, nm, l);
        return (int)l;
    }
    if (nr == 0x09 || nr == 0x18 || nr == 0x19 || nr == 0x1A || nr == 0x1B) {     /* PROP, KEY, LED, SND, SW states: none set */
        if (!size || !wr_ok(arg, size)) return -14;
        memset((void *)(uintptr_t)arg, 0, size);
        return (int)size;
    }
    if (nr >= 0x20 && nr < 0x40) {                    /* EVIOCGBIT(type): the event types / codes the device has */
        if (!size || !wr_ok(arg, size)) return -14;
        uint8_t *o = (uint8_t *)(uintptr_t)arg;
        memset(o, 0, size);
        uint32_t t = nr - 0x20;
        #define SETBIT(b) do { if ((uint32_t)(b) / 8 < size) o[(b) / 8] |= (uint8_t)(1u << ((b) % 8)); } while (0)
        if (t == 0) { SETBIT(EV_SYN); SETBIT(EV_KEY); if (!dev) { SETBIT(EV_REL); SETBIT(EV_ABS); } }
        else if (t == EV_KEY) { if (dev) { for (int k = 1; k < 120; k++) SETBIT(k); } else { SETBIT(BTN_LEFT); SETBIT(BTN_RIGHT); SETBIT(BTN_MIDDLE); } }
        else if (t == EV_REL && !dev) { SETBIT(8); }
        else if (t == EV_ABS && !dev) { SETBIT(0); SETBIT(1); }
        return (int)size;
    }
    if (nr >= 0x40 && nr < 0x80) {                    /* EVIOCGABS(axis): struct input_absinfo */
        if (!wr_ok(arg, 24)) return -14;
        int32_t ai[6] = { 0, 0, (nr - 0x40) == 0 ? TFBW_W - 1 : TFBW_H - 1, 0, 0, 0 };
        memcpy((void *)(uintptr_t)arg, ai, 24);
        return 0;
    }
    if (nr == 0x90 || nr == 0xa0 || nr == 0x03 || nr == 0x04 || nr == 0x10 || nr == 0x11) return 0;     /* GRAB, CLOCKID, repeat, keymap */
    return -25;
}
