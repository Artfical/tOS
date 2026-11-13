/* The DRM device (/dev/dri/card0): the calls a KMS program makes with dumb buffers, raw ioctls (no libdrm) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define CHECK(c, what) do { if (!(c)) { printf("drmtest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("drmtest: ok %s\n", what); } while (0)
struct version { int major, minor, patch; size_t nl; char *n; size_t dl; char *d; size_t cl; char *c; };
struct cap { uint64_t c, v; };
struct res { uint64_t fbs, crtcs, conns, encs; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct mode { uint32_t clock; uint16_t hd, hss, hse, ht, hsk, vd, vss, vse, vt, vsc; uint32_t vr, fl, ty; char name[32]; };
struct crtc { uint64_t conns; uint32_t nconn, id, fb, x, y, gamma, valid; struct mode m; };
struct conn { uint64_t encs, modes, props, pvals; uint32_t nmodes, nprops, nencs, enc, id, type, tid, connection, mmw, mmh, sub, pad; };
struct create { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; };
struct mapd { uint32_t handle, pad; uint64_t offset; };
struct fbcmd2 { uint32_t id, w, h, fmt, flags, handles[4], pitches[4], offsets[4]; uint64_t mod[4]; };
struct flip { uint32_t crtc, fb, flags, res; uint64_t ud; };
struct plres { uint64_t p; uint32_t n, pad; };
#define IOW(nr, t) _IOWR('d', nr, t)

int main(void)
{
    int fails = 0;
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0 && errno == ENODEV) { printf("drmtest: skipped (no desktop to show the window in)\n"); return 0; }
    CHECK(fd >= 0, "open /dev/dri/card0");
    if (fd < 0) { printf("drmtest: %d FAILED\n", fails); return 1; }
    char name[32] = "";
    struct version v; memset(&v, 0, sizeof(v)); v.nl = 31; v.n = name;
    CHECK(ioctl(fd, IOW(0x00, struct version), &v) == 0 && !strcmp(name, "tos"), "VERSION (driver tos)");
    struct cap c = { 1, 0 };
    CHECK(ioctl(fd, IOW(0x0c, struct cap), &c) == 0 && c.v == 1, "GET_CAP DUMB_BUFFER");
    struct cap at = { 3, 1 };
    CHECK(ioctl(fd, _IOW('d', 0x0d, struct cap), &at) != 0, "SET_CLIENT_CAP atomic is refused");
    struct cap up = { 2, 1 };
    CHECK(ioctl(fd, _IOW('d', 0x0d, struct cap), &up) == 0, "SET_CLIENT_CAP universal planes");
    uint32_t crtcs[4], conns[4], encs[4];
    struct res r; memset(&r, 0, sizeof(r));
    CHECK(ioctl(fd, IOW(0xA0, struct res), &r) == 0 && r.ncrtc == 1 && r.nconn == 1 && r.nenc == 1, "GETRESOURCES counts");
    r.crtcs = (uintptr_t)crtcs; r.conns = (uintptr_t)conns; r.encs = (uintptr_t)encs; r.ncrtc = r.nconn = r.nenc = 4;
    CHECK(ioctl(fd, IOW(0xA0, struct res), &r) == 0 && crtcs[0] && conns[0] && encs[0], "GETRESOURCES ids");
    struct mode modes[4];
    struct conn cn; memset(&cn, 0, sizeof(cn)); cn.id = conns[0];
    CHECK(ioctl(fd, IOW(0xA7, struct conn), &cn) == 0 && cn.nmodes == 1 && cn.connection == 1, "GETCONNECTOR connected, one mode");
    cn.modes = (uintptr_t)modes; cn.nmodes = 4; cn.encs = (uintptr_t)encs; cn.nencs = 4;
    CHECK(ioctl(fd, IOW(0xA7, struct conn), &cn) == 0 && modes[0].hd == 800 && modes[0].vd == 560 && modes[0].vr == 60, "GETCONNECTOR mode 800x560@60");
    uint32_t w = modes[0].hd, h = modes[0].vd;
    struct create cr; memset(&cr, 0, sizeof(cr)); cr.w = w; cr.h = h; cr.bpp = 32;
    CHECK(ioctl(fd, IOW(0xB2, struct create), &cr) == 0 && cr.handle && cr.pitch >= w * 4, "CREATE_DUMB 800x560");
    struct mapd md; memset(&md, 0, sizeof(md)); md.handle = cr.handle;
    CHECK(ioctl(fd, IOW(0xB3, struct mapd), &md) == 0, "MAP_DUMB");
    uint8_t *px = mmap(0, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    CHECK(px != MAP_FAILED, "mmap the buffer");
    if (px != MAP_FAILED) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) ((uint32_t *)(px + y * cr.pitch))[x] = ((x * 255 / w) << 16) | ((y * 255 / h) << 8) | 0x40;
        CHECK(((uint32_t *)(px + 10 * cr.pitch))[10] != 0, "the buffer holds what was drawn");
    }
    struct fbcmd2 fb; memset(&fb, 0, sizeof(fb)); fb.w = w; fb.h = h; fb.fmt = 0x34325258; fb.handles[0] = cr.handle; fb.pitches[0] = cr.pitch;
    CHECK(ioctl(fd, IOW(0xB8, struct fbcmd2), &fb) == 0 && fb.id, "ADDFB2 XRGB8888");
    struct crtc sc; memset(&sc, 0, sizeof(sc)); sc.conns = (uintptr_t)conns; sc.nconn = 1; sc.id = crtcs[0]; sc.fb = fb.id; sc.valid = 1; sc.m = modes[0];
    CHECK(ioctl(fd, IOW(0xA2, struct crtc), &sc) == 0, "SETCRTC");
    struct crtc gc; memset(&gc, 0, sizeof(gc)); gc.id = crtcs[0];
    CHECK(ioctl(fd, IOW(0xA1, struct crtc), &gc) == 0 && gc.fb == fb.id && gc.valid, "GETCRTC shows the buffer");
    struct flip fl = { crtcs[0], fb.id, 1, 0, 0x1234 };
    CHECK(ioctl(fd, IOW(0xB0, struct flip), &fl) == 0, "PAGE_FLIP with an event");
    struct pollfd pf = { fd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "poll: the flip is complete");
    uint32_t ev[8];
    int n = (int)read(fd, ev, sizeof(ev));
    uint64_t ud; if (n >= 16) memcpy(&ud, (char *)ev + 8, 8); else ud = 0;
    CHECK(n == 32 && ev[0] == 2 && ud == 0x1234, "read: DRM_EVENT_FLIP_COMPLETE with the user data");
    struct plres pr; memset(&pr, 0, sizeof(pr));
    CHECK(ioctl(fd, IOW(0xB5, struct plres), &pr) == 0 && pr.n == 1, "GETPLANERESOURCES: one plane");
    CHECK(ioctl(fd, _IOWR('d', 0xAF, uint32_t), &fb.id) == 0, "RMFB");
    CHECK(ioctl(fd, _IOWR('d', 0xB4, uint32_t), &cr.handle) == 0, "DESTROY_DUMB");
    sleep(1);
    close(fd);
    printf(fails ? "drmtest: %d FAILED\n" : "drmtest: all passed\n", fails);
    return fails != 0;
}
