/* Filled, flat-shaded, z-buffered software rasterizer -- a rotating
 * lit cube, drawn triangle-by-triangle into an offscreen buffer with
 * per-pixel z-testing, then blitted whole to the real Bochs/VBE linear
 * framebuffer (see bochs.h/vgatest/DOOM for the same hardware path).
 *
 * sinf/cosf/sqrtf aren't pulled in from anywhere else on purpose: the
 * only existing float math lives behind kernel/doom/port's compat
 * headers (scoped to DOOM_CFLAGS's include path) or MicroPython's own
 * internal math_stubs.c, neither meant to be a general kernel-wide
 * <math.h>. Small self-contained approximations here avoid dragging
 * either of those in just for this one file.
 */
#include "render3d.h"
#include "bochs.h"
#include "vga.h"
#include "terminal.h"
#include "keyboard.h"
#include "debugmon.h"
#include "memory.h"

#define R3D_W 320
#define R3D_H 200

typedef struct { float x, y, z; } vec3;

static float r3d_fabs(float x) { return x < 0.0f ? -x : x; }
static float r3d_fmin(float a, float b) { return a < b ? a : b; }
static float r3d_fmax(float a, float b) { return a > b ? a : b; }

static float r3d_sin(float x)
{
    const float PI = 3.14159265f;
    while (x > PI) x -= 2.0f * PI;
    while (x < -PI) x += 2.0f * PI;
    float x2 = x * x, x3 = x2 * x, x5 = x3 * x2, x7 = x5 * x2;
    return x - x3 / 6.0f + x5 / 120.0f - x7 / 5040.0f;
}

static float r3d_cos(float x) { return r3d_sin(x + 1.5707963f); }

static float r3d_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x;
    for (int i = 0; i < 12; i++) r = 0.5f * (r + x / r);
    return r;
}

static vec3 vsub(vec3 a, vec3 b) { vec3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static vec3 vcross(vec3 a, vec3 b)
{
    vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
static float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 vnorm(vec3 a)
{
    float len = r3d_sqrt(vdot(a, a));
    if (len < 0.0001f) return a;
    float inv = 1.0f / len;
    vec3 r = { a.x * inv, a.y * inv, a.z * inv };
    return r;
}

static const vec3 cube_verts[8] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1,1},  {1,-1,1},  {1,1,1},  {-1,1,1},
};

typedef struct { int a, b, c; uint32_t color; } face_t;

/* Each face wound counter-clockwise as seen from outside the cube. */
static const face_t cube_faces[12] = {
    {0,3,2, 0xCC2222}, {0,2,1, 0xCC2222}, /* back  z=-1, red */
    {4,5,6, 0x22CC22}, {4,6,7, 0x22CC22}, /* front z=+1, green */
    {0,4,7, 0x2222CC}, {0,7,3, 0x2222CC}, /* left  x=-1, blue */
    {1,2,6, 0xCCCC22}, {1,6,5, 0xCCCC22}, /* right x=+1, yellow */
    {0,1,5, 0xCC22CC}, {0,5,4, 0xCC22CC}, /* bottom y=-1, magenta */
    {3,7,6, 0x22CCCC}, {3,6,2, 0x22CCCC}, /* top    y=+1, cyan */
};

void render3d_run(void)
{
    /* Same restore concern as cmd_vgatest -- see its comment for why
     * this snapshot has to happen before anything touches VBE. */
    vga_init();

    bochs_device_t bochs;
    if (bochs_init(&bochs) != 0 || !bochs.lfb) {
        terminal_writestring("3d: no Bochs/VBE-capable display adapter found\n");
        return;
    }
    bochs_set_mode(&bochs, R3D_W, R3D_H, 32);

    /* Heap-allocated rather than static: ~500KB of extra BSS pushed
     * the kernel image's memory footprint far enough to collide with
     * where GRUB had already placed the initrd module, corrupting it
     * during boot ("ramfs: importing initrd" crashing into a reboot
     * loop, before this command could ever even run) -- see
     * memory.c's memory_init(mem_upper, reserved_end) for the
     * (unrelated) heap-vs-initrd overlap bug this project already hit
     * once before. Heap allocation here happens long after boot has
     * finished, so it can't repeat that collision. */
    float *zbuf = (float *)malloc(sizeof(float) * R3D_W * R3D_H);
    uint32_t *frame = (uint32_t *)malloc(sizeof(uint32_t) * R3D_W * R3D_H);
    if (!zbuf || !frame) {
        terminal_writestring("3d: out of memory\n");
        if (zbuf) free(zbuf);
        if (frame) free(frame);
        bochs_disable();
        vga_set_mode(VGA_MODE_TEXT);
        terminal_set_force_direct(0);
        terminal_clear();
        return;
    }

    vec3 light_dir = vnorm((vec3){0.4f, 0.6f, -1.0f});
    uint32_t start_ms = debugmon_uptime_ms();

    int quit = 0;
    for (;;) {
        /* keyboard_getchar()'s ASCII table maps the Escape scancode to
         * 0 (it's not a printable character), so it can never surface
         * 27 there -- the raw press/release queue (see keyboard.c's
         * process_scancode(), same one doomgeneric_tos.c uses) is the
         * one that explicitly special-cases it. */
        uint8_t rk; int pressed;
        while (keyboard_get_raw_event(&rk, &pressed)) {
            if (rk == 27 && pressed) quit = 1;
        }
        /* Ctrl+C, same as DOOM's fullscreen exit -- comes through the
         * regular ASCII queue (ctrl_pressed folds 'c' down to 0x03),
         * not the raw one above. */
        if (keyboard_data_available() && keyboard_getchar() == 3) quit = 1;
        if (quit) break;

        float t = (float)(debugmon_uptime_ms() - start_ms) / 1000.0f;
        float ax = t * 0.9f, ay = t * 1.3f;
        float sx = r3d_sin(ax), cx = r3d_cos(ax);
        float sy = r3d_sin(ay), cy = r3d_cos(ay);

        for (int i = 0; i < R3D_W * R3D_H; i++) { zbuf[i] = 1e9f; frame[i] = 0x101018; }

        vec3 xf[8];
        for (int i = 0; i < 8; i++) {
            vec3 v = cube_verts[i];
            float x1 = v.x * cy + v.z * sy;
            float z1 = -v.x * sy + v.z * cy;
            float y2 = v.y * cx - z1 * sx;
            float z2 = v.y * sx + z1 * cx;
            xf[i] = (vec3){ x1, y2, z2 + 4.5f }; /* push in front of the camera */
        }

        const float focal = 160.0f;
        for (int f = 0; f < 12; f++) {
            face_t face = cube_faces[f];
            vec3 a = xf[face.a], b = xf[face.b], c = xf[face.c];
            vec3 normal = vnorm(vcross(vsub(b, a), vsub(c, a)));

            /* Camera sits at the origin looking down +z -- a face is
             * front-facing if its normal points back toward the
             * camera, i.e. away from the scene along its own depth. */
            vec3 to_cam = vnorm((vec3){ -a.x, -a.y, -a.z });
            if (vdot(normal, to_cam) <= 0.0f) continue;

            float px[3], py[3], pz[3];
            vec3 verts[3] = { a, b, c };
            for (int k = 0; k < 3; k++) {
                px[k] = R3D_W / 2.0f + verts[k].x * focal / verts[k].z;
                py[k] = R3D_H / 2.0f - verts[k].y * focal / verts[k].z;
                pz[k] = verts[k].z;
            }

            float intensity = vdot(normal, light_dir);
            intensity = r3d_fmax(0.15f, r3d_fmin(1.0f, intensity));
            uint8_t r = (uint8_t)(((face.color >> 16) & 0xFF) * intensity);
            uint8_t g = (uint8_t)(((face.color >> 8) & 0xFF) * intensity);
            uint8_t bl = (uint8_t)((face.color & 0xFF) * intensity);
            uint32_t shaded = ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;

            int minx = (int)r3d_fmax(0.0f, r3d_fmin(r3d_fmin(px[0], px[1]), px[2]));
            int maxx = (int)r3d_fmin((float)(R3D_W - 1), r3d_fmax(r3d_fmax(px[0], px[1]), px[2]));
            int miny = (int)r3d_fmax(0.0f, r3d_fmin(r3d_fmin(py[0], py[1]), py[2]));
            int maxy = (int)r3d_fmin((float)(R3D_H - 1), r3d_fmax(r3d_fmax(py[0], py[1]), py[2]));

            float denom = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
            if (r3d_fabs(denom) < 1e-6f) continue;

            for (int y = miny; y <= maxy; y++) {
                for (int x = minx; x <= maxx; x++) {
                    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                    float w0 = ((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / denom;
                    float w1 = ((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / denom;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                    float z = w0 * pz[0] + w1 * pz[1] + w2 * pz[2];
                    int idx = y * R3D_W + x;
                    if (z < zbuf[idx]) {
                        zbuf[idx] = z;
                        frame[idx] = shaded;
                    }
                }
            }
        }

        volatile uint32_t *lfb = (volatile uint32_t *)bochs.lfb;
        for (int i = 0; i < R3D_W * R3D_H; i++) lfb[i] = frame[i];
    }

    free(zbuf);
    free(frame);

    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
}
