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

#define DRM_MAXROWS 768                               /* a dumb buffer: up to 1024 x 768 (a page per row) */
#define DRM_NGEM 6
#define DRM_NFB 6
#define DRM_NEV 8
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
    /* /dev/dri/card0: dumb buffers (a page per row, like the window's own), what is shown, and flip-complete events */
    struct { int used; uint32_t w, h, npg; uint32_t pg[DRM_MAXROWS]; } gem[DRM_NGEM];
    struct { int used; uint32_t id, handle, w, h; } fbs[DRM_NFB];
    uint32_t next_fb, scan_fb, crtc_on;
    struct { uint64_t user_data; uint32_t sec, usec, seq; } evq[DRM_NEV];
    int ev_n;
    uint32_t seq;
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
    f->next_fb = 100;                                 /* DRM framebuffer ids (0 is no framebuffer) */
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
    int sg = -1;                                       /* a DRM program's buffer on the screen (SETCRTC, PAGE_FLIP) replaces the window's pixels */
    if (f->crtc_on)
        for (int i = 0; i < DRM_NFB; i++)
            if (f->fbs[i].used && f->fbs[i].id == f->scan_fb && f->gem[f->fbs[i].handle - 1].used) sg = (int)f->fbs[i].handle - 1;
    for (int row = 0; row < TFBW_H && row < h; row++) {
        uint32_t src = sg >= 0 ? ((uint32_t)row < f->gem[sg].h ? f->gem[sg].pg[row] : 0) : f->pg[row];
        if (!src) continue;
        int dy = y + row;
        if (dy < s->cy0 || dy >= s->cy1) continue;
        memcpy(&s->px[dy * s->w + x + c0], (const uint32_t *)P2V(src) + c0, (size_t)(c1 - c0) * 4);   /* a row at a time */
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
    for (int g = 0; g < DRM_NGEM; g++) if (f->gem[g].used) { for (uint32_t r = 0; r < f->gem[g].npg; r++) free_physical_page(f->gem[g].pg[r]); f->gem[g].used = 0; }
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

/* ===================================================================== /dev/dri/card0
 * A minimal DRM/KMS device over the window: one virtual connector with one mode (800 x 560), one encoder, one CRTC, one
 * primary plane, and dumb buffers (software rendering: a program draws in them through mmap and shows one with SETCRTC
 * or PAGE_FLIP). There is no GPU, no GEM execbuffer and no atomic API: a client that asks for the atomic capability is
 * refused and uses the legacy calls. The ids and numbers are the ones of Linux's drm.h and drm_mode.h. */
#define DRM_CRTC 31
#define DRM_CONN 32
#define DRM_ENC 33
#define DRM_PLANE 34
#define DRM_OFF_SHIFT 28                              /* the mmap offset of a buffer: handle << 28 */
#define FOURCC_XR24 0x34325258u
#define FOURCC_AR24 0x34325241u

static void put_u32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static void drm_mode(uint8_t *m)                      /* struct drm_mode_modeinfo (68 bytes): 800x560 at 60 Hz */
{
    memset(m, 0, 68);
    uint32_t clock = 36000, vref = 60, flags = 5, type = 72;     /* PHSYNC | PVSYNC; PREFERRED | DRIVER */
    uint16_t t[10] = { TFBW_W, 832, 912, 1000, 0, TFBW_H, 563, 568, 600, 0 };
    memcpy(m, &clock, 4); memcpy(m + 4, t, 20); memcpy(m + 24, &vref, 4); memcpy(m + 28, &flags, 4); memcpy(m + 32, &type, 4);
    memcpy(m + 36, "800x560", 8);
}
static int drm_fb_find(fbw_t *f, uint32_t id) { for (int i = 0; i < DRM_NFB; i++) if (f->fbs[i].used && f->fbs[i].id == id) return i; return -1; }
static void drm_gem_free(fbw_t *f, uint32_t h)
{
    if (h < 1 || h > DRM_NGEM || !f->gem[h - 1].used) return;
    for (uint32_t r = 0; r < f->gem[h - 1].npg; r++) free_physical_page(f->gem[h - 1].pg[r]);
    f->gem[h - 1].used = 0;
    for (int i = 0; i < DRM_NFB; i++) if (f->fbs[i].used && f->fbs[i].handle == h) { if (f->fbs[i].id == f->scan_fb) f->crtc_on = 0; f->fbs[i].used = 0; }
}
static int drm_addfb(fbw_t *f, uint32_t handle, uint32_t w, uint32_t h, uint32_t *id)
{
    if (handle < 1 || handle > DRM_NGEM || !f->gem[handle - 1].used) return -2;
    for (int i = 0; i < DRM_NFB; i++)
        if (!f->fbs[i].used) {
            f->fbs[i].used = 1; f->fbs[i].id = f->next_fb++; f->fbs[i].handle = handle; f->fbs[i].w = w; f->fbs[i].h = h;
            *id = f->fbs[i].id;
            return 0;
        }
    return -12;
}
static void drm_queue_event(fbw_t *f, uint64_t user_data)
{
    uint32_t ms = uihost_ms();
    if (f->ev_n >= DRM_NEV) return;
    f->evq[f->ev_n].user_data = user_data; f->evq[f->ev_n].sec = ms / 1000; f->evq[f->ev_n].usec = (ms % 1000) * 1000; f->evq[f->ev_n].seq = ++f->seq;
    f->ev_n++;
}

int fbwin_drm_ready(void) { return cur && cur->ev_n > 0; }
int fbwin_drm_read(uint8_t *out, int max_bytes)       /* whole 32-byte events (DRM_EVENT_FLIP_COMPLETE): bytes, 0 when none */
{
    if (!cur) return 0;
    int n = 0;
    while (cur->ev_n > 0 && n + 32 <= max_bytes) {
        uint32_t hdr[2] = { 2, 32 };
        memcpy(out + n, hdr, 8); memcpy(out + n + 8, &cur->evq[0].user_data, 8);
        memcpy(out + n + 16, &cur->evq[0].sec, 4); memcpy(out + n + 20, &cur->evq[0].usec, 4);
        memcpy(out + n + 24, &cur->evq[0].seq, 4); { uint32_t c = DRM_CRTC; memcpy(out + n + 28, &c, 4); }
        for (int i = 1; i < cur->ev_n; i++) cur->evq[i - 1] = cur->evq[i];
        cur->ev_n--;
        n += 32;
    }
    return n;
}

/* the physical page of a dumb buffer for mmap: off = (handle << 28) + byte offset; 0 = none */
uint32_t fbwin_drm_page(uint64_t off)
{
    if (!cur) return 0;
    uint32_t h = (uint32_t)(off >> DRM_OFF_SHIFT), pg = (uint32_t)((off & ((1ull << DRM_OFF_SHIFT) - 1)) / 4096);
    if (h < 1 || h > DRM_NGEM || !cur->gem[h - 1].used || pg >= cur->gem[h - 1].npg) return 0;
    return cur->gem[h - 1].pg[pg];
}

#define ARR(ptr, n, sz) ((ptr) && wr_ok((ptr), (uint64_t)(n) * (sz)))
int fbwin_ioctl_drm(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t))
{
    if (((req >> 8) & 0xFF) != 'd') return 1;
    uint32_t nr = req & 0xFF, size = (req >> 16) & 0x3FFF;
    fbw_t *f = cur;
    if (!f || f->closed) return -19;
    if (size && !wr_ok(arg, size)) return -14;
    uint8_t *a = (uint8_t *)(uintptr_t)arg;
    switch (nr) {
    case 0x00: {                                      /* DRM_IOCTL_VERSION: struct drm_version */
        static const char name[] = "tos", date[] = "20261113", desc[] = "tOS window (dumb buffers)";
        int32_t v[3] = { 1, 0, 0 };
        memcpy(a, v, 12);
        const char *s[3] = { name, date, desc };
        for (int i = 0; i < 3; i++) {
            uint64_t len, ptr;
            memcpy(&len, a + 16 + i * 16, 8); memcpy(&ptr, a + 24 + i * 16, 8);
            uint64_t sl = strlen(s[i]);
            if (ptr && len) { uint64_t n = sl < len ? sl : len; if (!wr_ok(ptr, n)) return -14; memcpy((void *)(uintptr_t)ptr, s[i], (size_t)n); }
            memcpy(a + 16 + i * 16, &sl, 8);
        }
        return 0;
    }
    case 0x01: { uint64_t len; memcpy(&len, a, 8); uint64_t z = 0; memcpy(a, &z, 8); (void)len; return 0; }      /* GET_UNIQUE: none */
    case 0x0c: {                                      /* GET_CAP: u64 capability, u64 value */
        uint64_t cap, val = 0;
        memcpy(&cap, a, 8);
        switch (cap) {
        case 1: val = 1; break;                       /* DUMB_BUFFER */
        case 3: val = 24; break;                      /* DUMB_PREFERRED_DEPTH */
        case 6: val = 1; break;                       /* TIMESTAMP_MONOTONIC */
        case 8: case 9: val = 64; break;              /* CURSOR_WIDTH, CURSOR_HEIGHT */
        }
        memcpy(a + 8, &val, 8);
        return 0;
    }
    case 0x0d: { uint64_t cap; memcpy(&cap, a, 8); return cap == 2 ? 0 : -22; }                                  /* SET_CLIENT_CAP: universal planes yes, atomic no */
    case 0x1e: case 0x1f: return 0;                                                           /* SET_MASTER, DROP_MASTER */
    case 0x09: { uint32_t h; memcpy(&h, a, 4); drm_gem_free(f, h); return 0; }                                   /* GEM_CLOSE */
    case 0x3a: {                                      /* WAIT_VBLANK: struct drm_wait_vblank (24 bytes): the reply at once */
        uint32_t ms = uihost_ms();
        uint32_t type = 0; memcpy(&type, a, 4);
        memcpy(a, &type, 4); memcpy(a + 4, &(uint32_t){ f->seq }, 4);
        uint64_t sec = ms / 1000, usec = (uint64_t)(ms % 1000) * 1000;
        memcpy(a + 8, &sec, 8); memcpy(a + 16, &usec, 8);
        return 0;
    }
    case 0xA0: {                                      /* MODE_GETRESOURCES: struct drm_mode_card_res (64 bytes) */
        uint64_t pf, pc, pn, pe;
        memcpy(&pf, a, 8); memcpy(&pc, a + 8, 8); memcpy(&pn, a + 16, 8); memcpy(&pe, a + 24, 8);
        uint32_t cf, cc, cn, ce, nfb = 0, ids[DRM_NFB];
        memcpy(&cf, a + 32, 4); memcpy(&cc, a + 36, 4); memcpy(&cn, a + 40, 4); memcpy(&ce, a + 44, 4);
        for (int i = 0; i < DRM_NFB; i++) if (f->fbs[i].used) ids[nfb++] = f->fbs[i].id;
        if (cf >= nfb && nfb && ARR(pf, nfb, 4)) memcpy((void *)(uintptr_t)pf, ids, nfb * 4);
        if (cc >= 1 && ARR(pc, 1, 4)) put_u32(pc, DRM_CRTC);
        if (cn >= 1 && ARR(pn, 1, 4)) put_u32(pn, DRM_CONN);
        if (ce >= 1 && ARR(pe, 1, 4)) put_u32(pe, DRM_ENC);
        uint32_t o[8] = { nfb, 1, 1, 1, 320, 1024, 200, 768 };           /* counts; min/max width and height */
        memcpy(a + 32, o, 32);
        return 0;
    }
    case 0xA1: case 0xA2: {                           /* GETCRTC / SETCRTC: struct drm_mode_crtc (104 bytes) */
        uint32_t crtc, fb;
        memcpy(&crtc, a + 12, 4); memcpy(&fb, a + 16, 4);
        if (crtc != DRM_CRTC) return -2;
        if (nr == 0xA2) {
            uint32_t valid;
            memcpy(&valid, a + 32, 4);
            if (!valid || !fb) { f->crtc_on = 0; return 0; }
            int i = drm_fb_find(f, fb);
            if (i < 0) return -2;
            f->scan_fb = fb; f->crtc_on = 1;
            return 0;
        }
        uint8_t o[104];
        memset(o, 0, sizeof(o));
        memcpy(o + 12, &(uint32_t){ DRM_CRTC }, 4);
        memcpy(o + 16, &(uint32_t){ f->crtc_on ? f->scan_fb : 0 }, 4);
        memcpy(o + 32, &(uint32_t){ f->crtc_on }, 4);
        if (f->crtc_on) drm_mode(o + 36);
        memcpy(a, o, 104);
        return 0;
    }
    case 0xA6: {                                      /* GETENCODER: struct drm_mode_get_encoder (20 bytes) */
        uint32_t id; memcpy(&id, a, 4);
        if (id != DRM_ENC) return -2;
        uint32_t o[5] = { DRM_ENC, 5, DRM_CRTC, 1, 0 };                  /* type VIRTUAL */
        memcpy(a, o, 20);
        return 0;
    }
    case 0xA7: {                                      /* GETCONNECTOR: struct drm_mode_get_connector (80 bytes) */
        uint32_t id; memcpy(&id, a + 48, 4);
        if (id != DRM_CONN) return -2;
        uint64_t pe, pm; uint32_t cm, ce;
        memcpy(&pe, a, 8); memcpy(&pm, a + 8, 8); memcpy(&cm, a + 32, 4); memcpy(&ce, a + 40, 4);
        if (pm && cm >= 1) { uint8_t m[68]; drm_mode(m); if (!wr_ok(pm, 68)) return -14; memcpy((void *)(uintptr_t)pm, m, 68); }
        if (pe && ce >= 1 && wr_ok(pe, 4)) put_u32(pe, DRM_ENC);
        uint32_t o[4] = { 1, 0, 1, DRM_ENC };                            /* count_modes, count_props, count_encoders, encoder_id */
        memcpy(a + 32, o, 16);
        uint32_t q[8] = { DRM_CONN, 15, 1, 1, 211, 148, 1, 0 };          /* id, VIRTUAL, type id, connected, mm, subpixel unknown */
        memcpy(a + 48, q, 32);
        return 0;
    }
    case 0xAA: {                                      /* GETPROPERTY: struct drm_mode_get_property (64 bytes): only the plane's "type" (id 1), an enum */
        uint32_t id; memcpy(&id, a + 16, 4);
        if (id != 1) return -2;
        uint64_t pe; uint32_t ce;
        memcpy(&pe, a + 8, 8); memcpy(&ce, a + 60, 4);
        if (ce >= 3 && pe && wr_ok(pe, 120)) {
            static const char *const nm[3] = { "Overlay", "Primary", "Cursor" };
            uint8_t *o = (uint8_t *)(uintptr_t)pe;
            memset(o, 0, 120);
            for (uint64_t v = 0; v < 3; v++) { memcpy(o + v * 40, &v, 8); strcpy((char *)o + v * 40 + 8, nm[v]); }
        }
        uint32_t fl = (1u << 2) | (1u << 3);                              /* IMMUTABLE | ENUM */
        memcpy(a + 20, &fl, 4);
        memset(a + 24, 0, 32); memcpy(a + 24, "type", 5);
        memcpy(a + 56, &(uint32_t){ 0 }, 4); memcpy(a + 60, &(uint32_t){ 3 }, 4);
        return 0;
    }
    case 0xB5: {                                      /* GETPLANERESOURCES: struct drm_mode_get_plane_res (16 bytes) */
        uint64_t p; uint32_t c;
        memcpy(&p, a, 8); memcpy(&c, a + 8, 4);
        if (c >= 1 && ARR(p, 1, 4)) put_u32(p, DRM_PLANE);
        put_u32(arg + 8, 1);
        return 0;
    }
    case 0xB6: {                                      /* GETPLANE: struct drm_mode_get_plane (32 bytes) */
        uint32_t id; memcpy(&id, a, 4);
        if (id != DRM_PLANE) return -2;
        uint64_t p; uint32_t c;
        memcpy(&p, a + 24, 8); memcpy(&c, a + 20, 4);
        if (c >= 2 && ARR(p, 2, 4)) { put_u32(p, FOURCC_XR24); put_u32(p + 4, FOURCC_AR24); }
        uint32_t o[6] = { DRM_PLANE, f->crtc_on ? DRM_CRTC : 0, f->crtc_on ? f->scan_fb : 0, 1, 0, 2 };
        memcpy(a, o, 24);
        return 0;
    }
    case 0xB7: {                                      /* SETPLANE: struct drm_mode_set_plane: show the framebuffer */
        uint32_t fb; memcpy(&fb, a + 8, 4);
        if (fb && drm_fb_find(f, fb) >= 0) { f->scan_fb = fb; f->crtc_on = 1; } else f->crtc_on = 0;
        return 0;
    }
    case 0xB9: {                                      /* OBJ_GETPROPERTIES: struct drm_mode_obj_get_properties (32 bytes) */
        uint64_t pp, pv; uint32_t cnt, id, type;
        memcpy(&pp, a, 8); memcpy(&pv, a + 8, 8); memcpy(&cnt, a + 16, 4); memcpy(&id, a + 20, 4); memcpy(&type, a + 24, 4);
        uint32_t n = (type == 0xeeeeeeeeu && id == DRM_PLANE) ? 1 : 0;   /* the plane has one: "type" (id 1) = Primary (1) */
        if (n && cnt >= 1 && ARR(pp, 1, 4) && ARR(pv, 1, 8)) { put_u32(pp, 1); uint64_t v = 1; memcpy((void *)(uintptr_t)pv, &v, 8); }
        put_u32(arg + 16, n);
        return 0;
    }
    case 0xB2: {                                      /* CREATE_DUMB: struct drm_mode_create_dumb (32 bytes) */
        uint32_t h, w, bpp;
        memcpy(&h, a, 4); memcpy(&w, a + 4, 4); memcpy(&bpp, a + 8, 4);
        if (!w || !h || w > 1024 || h > DRM_MAXROWS || bpp != 32) return -22;
        for (uint32_t g = 0; g < DRM_NGEM; g++)
            if (!f->gem[g].used) {
                uint32_t i;
                for (i = 0; i < h; i++) {
                    uint32_t ph = alloc_physical_page();
                    if (!ph) break;
                    memset(P2V(ph), 0, 4096);
                    f->gem[g].pg[i] = ph;
                }
                if (i < h) { while (i) free_physical_page(f->gem[g].pg[--i]); return -12; }
                f->gem[g].used = 1; f->gem[g].w = w; f->gem[g].h = h; f->gem[g].npg = h;
                uint32_t hp[2] = { g + 1, 4096 };                         /* handle, pitch: one page a row */
                uint64_t sz = (uint64_t)h * 4096;
                memcpy(a + 16, hp, 8); memcpy(a + 24, &sz, 8);
                return 0;
            }
        return -12;
    }
    case 0xB3: {                                      /* MAP_DUMB: struct drm_mode_map_dumb (16 bytes): the offset to mmap */
        uint32_t h; memcpy(&h, a, 4);
        if (h < 1 || h > DRM_NGEM || !f->gem[h - 1].used) return -2;
        uint64_t off = (uint64_t)h << DRM_OFF_SHIFT;
        memcpy(a + 8, &off, 8);
        return 0;
    }
    case 0xB4: { uint32_t h; memcpy(&h, a, 4); if (h < 1 || h > DRM_NGEM || !f->gem[h - 1].used) return -2; drm_gem_free(f, h); return 0; }   /* DESTROY_DUMB */
    case 0xAE: {                                      /* ADDFB: struct drm_mode_fb_cmd (28 bytes) */
        uint32_t w, h, handle, id;
        memcpy(&w, a + 4, 4); memcpy(&h, a + 8, 4); memcpy(&handle, a + 24, 4);
        int r = drm_addfb(f, handle, w, h, &id);
        if (r) return r;
        memcpy(a, &id, 4);
        return 0;
    }
    case 0xB8: {                                      /* ADDFB2: struct drm_mode_fb_cmd2 (104 bytes) */
        uint32_t w, h, fmt, handle, id;
        memcpy(&w, a + 4, 4); memcpy(&h, a + 8, 4); memcpy(&fmt, a + 12, 4); memcpy(&handle, a + 20, 4);
        if (fmt != FOURCC_XR24 && fmt != FOURCC_AR24) return -22;
        int r = drm_addfb(f, handle, w, h, &id);
        if (r) return r;
        memcpy(a, &id, 4);
        return 0;
    }
    case 0xAF: {                                      /* RMFB */
        uint32_t id; memcpy(&id, a, 4);
        int i = drm_fb_find(f, id);
        if (i < 0) return -2;
        if (f->fbs[i].id == f->scan_fb) f->crtc_on = 0;
        f->fbs[i].used = 0;
        return 0;
    }
    case 0xB0: {                                      /* PAGE_FLIP: struct drm_mode_crtc_page_flip (24 bytes) */
        uint32_t crtc, fb, flags; uint64_t ud;
        memcpy(&crtc, a, 4); memcpy(&fb, a + 4, 4); memcpy(&flags, a + 8, 4); memcpy(&ud, a + 16, 8);
        if (crtc != DRM_CRTC || drm_fb_find(f, fb) < 0) return -22;
        f->scan_fb = fb; f->crtc_on = 1;
        if (flags & 1) drm_queue_event(f, ud);        /* DRM_MODE_PAGE_FLIP_EVENT */
        return 0;
    }
    case 0xB1: return 0;                              /* DIRTYFB: the window is redrawn all the time */
    case 0xA3: case 0xA4: case 0xA5: case 0xBB: return -22;      /* CURSOR, GAMMA: no hardware cursor */
    }
    return -25;
}
