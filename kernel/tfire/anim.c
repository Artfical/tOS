#include "anim.h"
#include "tfire_libc.h"

/*
 * Transitions, CSS animations and element.animate() (see anim.h).
 *
 * Every element that moves (or may: it has transition-* or animation-*) gets a record: its base values as the
 * sheets last said, and the runs on it — transitions (one property, from a value to another), CSS animations
 * and script animations (keyframes, each a list of values per property). On a frame each run gives its values
 * for the moment, later runs over earlier ones (transitions, then CSS animations, then scripts', as CSS says),
 * and what nothing gives any more goes back to its base value.
 *
 * Time is in milliseconds; progress and easing in floats (this is soft-float code, like the rest of the engine).
 */

/* ------------------------------------------------------------------ a value of a property */

typedef struct {
    int32_t n;                 /* opacity x1000, font size and radius in px */
    uint32_t c;                /* colours: AARRGGBB */
    tf_len l;                  /* lengths */
    uint8_t nx;                /* transforms */
    tf_xop x[TF_MAXX];
} aval;

enum { K_NUM, K_COLOR, K_LEN, K_XF };

static const struct { const char *name; uint8_t kind; } props[AP_N] = {
    { "opacity", K_NUM }, { "color", K_COLOR }, { "background-color", K_COLOR }, { "border-color", K_COLOR },
    { "transform", K_XF }, { "translate", K_XF }, { "rotate", K_XF }, { "scale", K_XF },
    { "width", K_LEN }, { "height", K_LEN }, { "left", K_LEN }, { "top", K_LEN }, { "right", K_LEN }, { "bottom", K_LEN },
    { "margin-left", K_LEN }, { "margin-top", K_LEN }, { "font-size", K_NUM }, { "border-radius", K_NUM }, { "max-height", K_LEN },
};

int anim_prop_id(const char *name, int len)
{
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t' || name[len - 1] == '\n' || name[len - 1] == '\r')) len--;
    while (len > 0 && (*name == ' ' || *name == '\t' || *name == '\n' || *name == '\r')) { name++; len--; }
    if (len > 8 && !strncasecmp(name, "-webkit-", 8)) { name += 8; len -= 8; }
    if (len == 3 && !strncasecmp(name, "all", 3)) return AP_ALL;
    if (len == 10 && !strncasecmp(name, "background", 10)) return AP_BG;
    for (int i = 0; i < AP_N; i++) if ((int)strlen(props[i].name) == len && !strncasecmp(name, props[i].name, (size_t)len)) return i;
    return -1;
}

/* ------------------------------------------------------------------ reading what the sheets write */

static int list_items(const char *v, const char **is, const char **ie, int max);
static const char *word_end(const char *p, const char *e);

static int ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static const char *skip(const char *p, const char *e) { while (p < e && ws(*p)) p++; return p; }

/* a number, and where it ends */
static int number(const char **pp, const char *e, float *out)
{
    const char *p = skip(*pp, e);
    float sign = 1, v = 0, div = 0;
    int digits = 0;
    if (p < e && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1; p++; }
    for (; p < e; p++) {
        if (*p >= '0' && *p <= '9') {
            digits++;
            if (div) { v += (float)(*p - '0') / div; div *= 10; } else v = v * 10 + (float)(*p - '0');
        } else if (*p == '.' && !div) div = 10;
        else break;
    }
    if (!digits) return 0;
    if (p < e && (*p == 'e' || *p == 'E') && p + 1 < e && ((p[1] >= '0' && p[1] <= '9') || p[1] == '-' || p[1] == '+')) {
        p++;
        int es = 1, ex = 0;
        if (*p == '-' || *p == '+') { if (*p == '-') es = -1; p++; }
        while (p < e && *p >= '0' && *p <= '9') ex = ex * 10 + (*p++ - '0');
        while (ex-- > 0) v = es > 0 ? v * 10 : v / 10;
    }
    *out = sign * v;
    *pp = p;
    return 1;
}

static int unit_is(const char *p, const char *e, const char *u)
{
    size_t l = strlen(u);
    if ((size_t)(e - p) < l || strncasecmp(p, u, l)) return 0;
    p += l;
    return p >= e || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'));
}

static int viewport_w = 1024, viewport_h = 768;      /* (for vw and vh: set by anim_restyled from the root's box) */

/* a length: px, em, rem, %, vw, vh, or 0, or auto */
int anim_parse_len(const char *v, const char *e, int font_px, tf_len *out)
{
    const char *p = skip(v, e);
    memset(out, 0, sizeof(*out));
    if (unit_is(p, e, "auto") || unit_is(p, e, "none")) { out->autov = 1; return 1; }
    float f;
    if (!number(&p, e, &f)) return 0;
    if (p < e && *p == '%') { out->pct = 1; out->v = (int)(f * 100); return 1; }
    if (unit_is(p, e, "em")) f *= (float)font_px;
    else if (unit_is(p, e, "rem")) f *= 16;
    else if (unit_is(p, e, "vw")) f = f * (float)viewport_w / 100;
    else if (unit_is(p, e, "vh")) f = f * (float)viewport_h / 100;
    else if (unit_is(p, e, "pt")) f = f * 4 / 3;
    out->v = (int)(f < 0 ? f - 0.5f : f + 0.5f);
    return 1;
}

/* an angle, in degrees x100 */
static int angle(const char **pp, const char *e, int *out)
{
    float f;
    if (!number(pp, e, &f)) return 0;
    const char *p = *pp;
    if (unit_is(p, e, "turn")) { f *= 360; p += 4; }
    else if (unit_is(p, e, "rad")) { f = f * 57.29578f; p += 3; }
    else if (unit_is(p, e, "grad")) { f = f * 0.9f; p += 4; }
    else if (unit_is(p, e, "deg")) p += 3;
    *pp = p;
    *out = (int)(f * 100);
    return 1;
}

/* the arguments of a function, split at commas (or white space): up to max */
static int args(const char *p, const char *e, const char **as, const char **ae, int max)
{
    int n = 0;
    p = skip(p, e);
    int commas = memchr(p, ',', (size_t)(e - p)) != 0;
    while (p < e && n < max) {
        const char *q = p;
        int depth = 0;
        while (q < e && (depth || (commas ? *q != ',' : !ws(*q)))) { if (*q == '(') depth++; else if (*q == ')') depth--; q++; }
        const char *qe = q;
        while (qe > p && ws(qe[-1])) qe--;
        if (qe > p) { as[n] = p; ae[n] = qe; n++; }
        p = skip(q < e ? q + 1 : q, e);
    }
    return n;
}

static float fabs_(float x) { return x < 0 ? -x : x; }

/* sin and cos of degrees (a series after bringing the angle near 0), sqrt and atan2 (enough for transforms) */
static float sin_deg(float d)
{
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    if (d > 90) d = 180 - d;
    if (d < -90) d = -180 - d;
    float x = d * 0.01745329f, x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float cos_deg(float d) { return sin_deg(d + 90); }
static float sqrt_(float x)
{
    if (x <= 0) return 0;
    float r = x > 1 ? x : 1;
    for (int i = 0; i < 20; i++) r = (r + x / r) / 2;
    return r;
}
static float atan2_deg(float y, float x)
{
    if (x == 0 && y == 0) return 0;
    float ax = fabs_(x), ay = fabs_(y);
    float a = ax > ay ? ay / ax : ax / ay;
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;   /* atan on [0, 1] */
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    if (y < 0) r = -r;
    return r * 57.29578f;
}

/* matrix(a, b, c, d, e, f): as a translate, a rotate and a scale (skew is lost) */
static int from_matrix(float m[6], tf_xop *out, int max)
{
    int n = 0;
    float sx = sqrt_(m[0] * m[0] + m[1] * m[1]);
    float det = m[0] * m[3] - m[1] * m[2];
    float sy = sx ? det / sx : 0;
    float rot = atan2_deg(m[1], m[0]);
    if ((m[4] || m[5]) && n < max) out[n++] = (tf_xop){ XT_TRANSLATE, 0, (int)m[4], (int)m[5] };
    if (rot && n < max) out[n++] = (tf_xop){ XT_ROTATE, 0, (int)(rot * 100), 0 };
    if ((fabs_(sx - 1) > 0.001f || fabs_(sy - 1) > 0.001f) && n < max) out[n++] = (tf_xop){ XT_SCALE, 0, (int)(sx * 1000), (int)(sy * 1000) };
    return n;
}

int anim_parse_transform(const char *v, const char *e, int font_px, tf_xop *out, int max)
{
    int n = 0;
    const char *p = skip(v, e);
    if (unit_is(p, e, "none")) return 0;
    while (p < e && n < max) {
        p = skip(p, e);
        const char *name = p;
        while (p < e && *p != '(' && !ws(*p)) p++;
        if (p >= e || *p != '(') break;
        int nl = (int)(p - name);
        const char *open = p + 1, *close = open;
        int depth = 0;
        while (close < e && (depth || *close != ')')) { if (*close == '(') depth++; else if (*close == ')') depth--; close++; }
        const char *as[6], *ae[6];
        int na = args(open, close, as, ae, 6);
        p = close < e ? close + 1 : close;
#define IS(s) (nl == (int)sizeof(s) - 1 && !strncasecmp(name, s, sizeof(s) - 1))
        tf_len l1, l2;
        if ((IS("translate") || IS("translate3d")) && na >= 1) {
            if (!anim_parse_len(as[0], ae[0], font_px, &l1)) continue;
            if (na < 2 || !anim_parse_len(as[1], ae[1], font_px, &l2)) memset(&l2, 0, sizeof(l2));
            out[n++] = (tf_xop){ XT_TRANSLATE, (uint8_t)((l1.pct ? 1 : 0) | (l2.pct ? 2 : 0)), l1.v, l2.v };
        } else if (IS("translateX") && na >= 1) {
            if (anim_parse_len(as[0], ae[0], font_px, &l1)) out[n++] = (tf_xop){ XT_TRANSLATE, (uint8_t)(l1.pct ? 1 : 0), l1.v, 0 };
        } else if (IS("translateY") && na >= 1) {
            if (anim_parse_len(as[0], ae[0], font_px, &l1)) out[n++] = (tf_xop){ XT_TRANSLATE, (uint8_t)(l1.pct ? 2 : 0), 0, l1.v };
        } else if ((IS("scale") || IS("scale3d")) && na >= 1) {
            float a, b;
            const char *q = as[0];
            if (!number(&q, ae[0], &a)) continue;
            if (q < ae[0] && *q == '%') a /= 100;
            b = a;
            if (na >= 2) { q = as[1]; if (number(&q, ae[1], &b) && q < ae[1] && *q == '%') b /= 100; }
            out[n++] = (tf_xop){ XT_SCALE, 0, (int)(a * 1000), (int)(b * 1000) };
        } else if ((IS("scaleX") || IS("scaleY")) && na >= 1) {
            float a;
            const char *q = as[0];
            if (!number(&q, ae[0], &a)) continue;
            int k = (int)(a * 1000);
            out[n++] = (tf_xop){ XT_SCALE, 0, IS("scaleX") ? k : 1000, IS("scaleX") ? 1000 : k };
        } else if ((IS("rotate") || IS("rotateZ")) && na >= 1) {
            int d;
            const char *q = as[0];
            if (angle(&q, ae[0], &d)) out[n++] = (tf_xop){ XT_ROTATE, 0, d, 0 };
        } else if (IS("matrix") && na == 6) {
            float m[6];
            int ok = 1;
            for (int i = 0; i < 6 && ok; i++) { const char *q = as[i]; ok = number(&q, ae[i], &m[i]); }
            if (ok) n += from_matrix(m, out + n, max - n);
        }
        /* skew, perspective and the 3D ones: not drawn */
#undef IS
    }
    return n;
}

/* the translate, rotate and scale properties: one function each */
static int parse_individual(int prop, const char *v, const char *e, int font_px, tf_xop *out)
{
    const char *p = skip(v, e);
    if (unit_is(p, e, "none")) return 0;
    const char *as[3], *ae[3];
    int na = args(p, e, as, ae, 3);
    if (!na) return 0;
    if (prop == AP_TRANSLATE) {
        tf_len a, b;
        memset(&b, 0, sizeof(b));
        if (!anim_parse_len(as[0], ae[0], font_px, &a)) return 0;
        if (na > 1) anim_parse_len(as[1], ae[1], font_px, &b);
        *out = (tf_xop){ XT_TRANSLATE, (uint8_t)((a.pct ? 1 : 0) | (b.pct ? 2 : 0)), a.v, b.v };
        return 1;
    }
    if (prop == AP_ROTATE) {
        int d;
        const char *q = as[na - 1];                      /* (rotate: x 45deg: the angle comes last) */
        if (!angle(&q, ae[na - 1], &d)) return 0;
        *out = (tf_xop){ XT_ROTATE, 0, d, 0 };
        return 1;
    }
    float a, b;
    const char *q = as[0];
    if (!number(&q, ae[0], &a)) return 0;
    if (q < ae[0] && *q == '%') a /= 100;
    b = a;
    if (na > 1) { q = as[1]; if (number(&q, ae[1], &b) && q < ae[1] && *q == '%') b /= 100; }
    *out = (tf_xop){ XT_SCALE, 0, (int)(a * 1000), (int)(b * 1000) };
    return 1;
}

static int memmem_s(const char *p, const char *e, const char *w)
{
    size_t l = strlen(w);
    for (; p + l <= e; p++) if (!strncasecmp(p, w, l)) return 1;
    return 0;
}

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static const struct { const char *n; uint32_t c; } named[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "purple", 0x800080 }, { "pink", 0xFFC0CB }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF },
    { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "navy", 0x000080 }, { "teal", 0x008080 }, { "maroon", 0x800000 },
    { "olive", 0x808000 }, { "gold", 0xFFD700 }, { "tomato", 0xFF6347 }, { "coral", 0xFF7F50 }, { "crimson", 0xDC143C },
    { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "orchid", 0xDA70D6 }, { "salmon", 0xFA8072 }, { "skyblue", 0x87CEEB },
    { "royalblue", 0x4169E1 }, { "steelblue", 0x4682B4 }, { "dodgerblue", 0x1E90FF }, { "deepskyblue", 0x00BFFF },
    { "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 }, { "limegreen", 0x32CD32 }, { "seagreen", 0x2E8B57 },
    { "forestgreen", 0x228B22 }, { "darkgreen", 0x006400 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "whitesmoke", 0xF5F5F5 }, { "rebeccapurple", 0x663399 },
    { "slateblue", 0x6A5ACD }, { "turquoise", 0x40E0D0 }, { "chocolate", 0xD2691E }, { "brown", 0xA52A2A },
    { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "lavender", 0xE6E6FA }, { "plum", 0xDDA0DD },
    { "tan", 0xD2B48C }, { "wheat", 0xF5DEB3 }, { "mintcream", 0xF5FFFA }, { "aliceblue", 0xF0F8FF }, { "darkorange", 0xFF8C00 },
};

static int clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }

static float hue2rgb(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

/* a colour: #rgb[a], #rrggbb[aa], rgb[a](), hsl[a](), a name, transparent, currentColor (cur) */
static int parse_color(const char *v, const char *e, uint32_t cur, uint32_t *out)
{
    const char *p = skip(v, e);
    while (e > p && ws(e[-1])) e--;
    int n = (int)(e - p);
    if (n <= 0) return 0;
    if (*p == '#') {
        int d[8], k = 0;
        for (const char *q = p + 1; q < e && k < 8; q++) { int h = hexd(*q); if (h < 0) return 0; d[k++] = h; }
        if (k == 3 || k == 4) { *out = (uint32_t)((k == 4 ? d[3] * 17 : 255) << 24 | (d[0] * 17) << 16 | (d[1] * 17) << 8 | d[2] * 17); return 1; }
        if (k == 6 || k == 8) { *out = (uint32_t)((k == 8 ? d[6] * 16 + d[7] : 255) << 24 | (d[0] * 16 + d[1]) << 16 | (d[2] * 16 + d[3]) << 8 | (d[4] * 16 + d[5])); return 1; }
        return 0;
    }
    if (unit_is(p, e, "transparent")) { *out = 0; return 1; }
    if (unit_is(p, e, "currentcolor")) { *out = 0xFF000000u | cur; return 1; }
    const char *open = memchr(p, '(', (size_t)n);
    if (open) {
        int nl = (int)(open - p);
        const char *close = e;
        if (close > open && close[-1] == ')') close--;
        /* rgb(1 2 3 / 0.5) and rgb(1, 2, 3, 0.5) alike: the slash is a separator too */
        char buf[128];
        int bl = 0;
        for (const char *q = open + 1; q < close && bl < 126; q++) buf[bl++] = *q == '/' ? ',' : *q;
        buf[bl] = 0;
        int has_comma = strchr(buf, ',') != 0;
        if (!has_comma) for (int i = 0; i < bl; i++) if (ws(buf[i])) buf[i] = ',';
        const char *as[4], *ae[4];
        int na = args(buf, buf + bl, as, ae, 4);
        float c[4] = { 0, 0, 0, 1 };
        int pct[4] = { 0 };
        for (int i = 0; i < na; i++) {
            const char *q = as[i];
            if (!number(&q, ae[i], &c[i])) return 0;
            if (q < ae[i] && *q == '%') pct[i] = 1;
        }
        if (pct[3]) c[3] /= 100;
        int a = clamp255(c[3] * 255);
        if ((nl == 3 || nl == 4) && !strncasecmp(p, "rgb", 3) && na >= 3) {
            for (int i = 0; i < 3; i++) if (pct[i]) c[i] = c[i] * 255 / 100;
            *out = (uint32_t)a << 24 | (uint32_t)clamp255(c[0]) << 16 | (uint32_t)clamp255(c[1]) << 8 | (uint32_t)clamp255(c[2]);
            return 1;
        }
        if ((nl == 3 || nl == 4) && !strncasecmp(p, "hsl", 3) && na >= 3) {
            float h = c[0] / 360, s = c[1] / 100, l = c[2] / 100;
            h -= (float)(int)h;
            if (h < 0) h += 1;
            float r, g, b;
            if (s == 0) r = g = b = l;
            else {
                float q2 = l < 0.5f ? l * (1 + s) : l + s - l * s, p2 = 2 * l - q2;
                r = hue2rgb(p2, q2, h + 1.0f / 3); g = hue2rgb(p2, q2, h); b = hue2rgb(p2, q2, h - 1.0f / 3);
            }
            *out = (uint32_t)a << 24 | (uint32_t)clamp255(r * 255) << 16 | (uint32_t)clamp255(g * 255) << 8 | (uint32_t)clamp255(b * 255);
            return 1;
        }
        return 0;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if ((int)strlen(named[i].n) == n && !strncasecmp(p, named[i].n, (size_t)n)) { *out = 0xFF000000u | named[i].c; return 1; }
    return 0;
}

int anim_parse_color(const char *v, const char *e, uint32_t cur, uint32_t *out) { return parse_color(v, e, cur, out); }

/* box-shadow: x y [blur [spread]] colour, the first of the list that is not inset */
void anim_set_shadow(tf_style *t, const char *v)
{
    t->shadow_color = 0;
    const char *is[4], *ie[4];
    int n = list_items(v, is, ie, 4);
    for (int i = 0; i < n; i++) {
        int lens[4], nl = 0, inset = 0;
        uint32_t col = 0xFF000000u | t->color;
        int got_col = 0;
        for (const char *p = is[i]; p < ie[i];) {
            p = skip(p, ie[i]);
            const char *we = word_end(p, ie[i]);
            if (we == p) { p++; continue; }
            tf_len l;
            if ((int)(we - p) == 5 && !strncasecmp(p, "inset", 5)) inset = 1;
            else if (!got_col && parse_color(p, we, t->color, &col)) got_col = 1;
            else if (nl < 4 && anim_parse_len(p, we, t->font_px, &l) && !l.pct && !l.autov) lens[nl++] = l.v;
            p = we;
        }
        if (inset || nl < 2 || !(col >> 24)) continue;
        t->shadow_x = (int16_t)lens[0]; t->shadow_y = (int16_t)lens[1];
        t->shadow_blur = (int16_t)(nl > 2 ? lens[2] : 0); t->shadow_spread = (int16_t)(nl > 3 ? lens[3] : 0);
        t->shadow_color = col;
        return;
    }
}

/* linear-gradient([angle | to side], colour [pos], ...): up to 4 stops; other gradients as their first colour */
void anim_set_gradient(tf_style *t, const char *v)
{
    t->ngrad = 0;
    const char *g = v ? strstr(v, "gradient(") : 0;
    if (!g) return;
    int linear = g - v >= 7 && !strncasecmp(g - 7, "linear-", 7);
    const char *open = g + 8, *close = open + 1;
    int depth = 1;
    for (; *close; close++) { if (*close == '(') depth++; else if (*close == ')' && --depth == 0) break; }
    char buf[400];
    int bl = (int)(close - open - 1);
    if (bl <= 0) return;
    if (bl > 399) bl = 399;
    memcpy(buf, open + 1, (size_t)bl);
    buf[bl] = 0;
    const char *is[8], *ie[8];
    int n = list_items(buf, is, ie, 8), first = 0;
    t->grad_angle = 180;
    if (n && linear) {
        const char *p = is[0];
        int a;
        if (!strncasecmp(p, "to ", 3)) {
            int top = memmem_s(p, ie[0], "top"), bot = memmem_s(p, ie[0], "bottom"), lef = memmem_s(p, ie[0], "left"), rig = memmem_s(p, ie[0], "right");
            int ang = bot ? 180 : top ? 0 : 180;
            if (rig) ang = top ? 45 : bot ? 135 : 90;
            if (lef) ang = top ? 315 : bot ? 225 : 270;
            t->grad_angle = (int16_t)ang;
            first = 1;
        } else if (angle(&p, ie[0], &a)) { t->grad_angle = (int16_t)(a / 100); first = 1; }
    } else if (n && !linear) {
        /* radial and conic: their first colour, as a plain background */
        for (int i = 0; i < n; i++) {
            const char *we = word_end(is[i], ie[i]);
            uint32_t c;
            if (parse_color(is[i], we, t->color, &c)) { t->bg = c; return; }
        }
        return;
    }
    int k = 0;
    for (int i = first; i < n && k < 4; i++) {
        const char *we = word_end(is[i], ie[i]);
        uint32_t c;
        if (!parse_color(is[i], we, t->color, &c)) continue;
        tf_len pos;
        int p = -1;
        const char *q = skip(we, ie[i]);
        if (q < ie[i] && anim_parse_len(q, ie[i], t->font_px, &pos) && pos.pct) p = pos.v / 10;
        t->grad_c[k] = c;
        t->grad_p[k] = (int16_t)p;
        k++;
    }
    if (k < 2) { if (k == 1) t->bg = t->grad_c[0]; return; }
    /* stops without a position spread out between the ones that have */
    if (t->grad_p[0] < 0) t->grad_p[0] = 0;
    if (t->grad_p[k - 1] < 0) t->grad_p[k - 1] = 1000;
    for (int i = 1; i < k - 1; i++) if (t->grad_p[i] < 0) t->grad_p[i] = (int16_t)(t->grad_p[i - 1] + (1000 - t->grad_p[i - 1]) / (k - i));
    t->ngrad = (uint8_t)k;
}

/* a value of prop as written, for an element with this style (em, currentColor) */
static int parse_value(int prop, const char *v, const char *e, const tf_style *s, aval *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = skip(v, e);
    while (e > p && ws(e[-1])) e--;
    if (e - p > 10 && !strncasecmp(e - 10, "!important", 10)) { e -= 10; while (e > p && ws(e[-1])) e--; }
    switch (props[prop].kind) {
    case K_NUM: {
        if (prop == AP_FONT_SIZE || prop == AP_RADIUS) {
            tf_len l;
            if (!anim_parse_len(p, e, s->font_px, &l) || l.autov) return 0;
            out->n = l.pct ? (prop == AP_FONT_SIZE ? s->font_px * l.v / 10000 : 999) : l.v;
            return 1;
        }
        float f;
        const char *q = p;
        if (!number(&q, e, &f)) return 0;
        if (q < e && *q == '%') f /= 100;
        out->n = (int)(f * 1000);
        if (out->n < 0) out->n = 0;
        if (out->n > 1000) out->n = 1000;
        return 1;
    }
    case K_COLOR: return parse_color(p, e, s->color, &out->c);
    case K_LEN: return anim_parse_len(p, e, s->font_px, &out->l);
    case K_XF:
        if (prop == AP_TRANSFORM) { out->nx = (uint8_t)anim_parse_transform(p, e, s->font_px, out->x, TF_MAXX); return 1; }
        out->nx = (uint8_t)parse_individual(prop, p, e, s->font_px, out->x);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ timing */

static const char *word_end(const char *p, const char *e)
{
    int depth = 0;
    while (p < e && (depth || (!ws(*p) && *p != ','))) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
    return p;
}

/* a timing function at [p, e): 1 if it is one */
static int parse_timing(const char *p, const char *e, tf_timing *t)
{
    memset(t, 0, sizeof(*t));
    int n = (int)(e - p);
#define KW(s) (n == (int)sizeof(s) - 1 && !strncasecmp(p, s, sizeof(s) - 1))
    if (KW("linear")) { t->kind = TT_LINEAR; return 1; }
    if (KW("ease")) { *t = (tf_timing){ TT_BEZIER, 0, 250, 100, 250, 1000, 0 }; return 1; }
    if (KW("ease-in")) { *t = (tf_timing){ TT_BEZIER, 0, 420, 0, 1000, 1000, 0 }; return 1; }
    if (KW("ease-out")) { *t = (tf_timing){ TT_BEZIER, 0, 0, 0, 580, 1000, 0 }; return 1; }
    if (KW("ease-in-out")) { *t = (tf_timing){ TT_BEZIER, 0, 420, 0, 580, 1000, 0 }; return 1; }
    if (KW("step-start")) { *t = (tf_timing){ TT_STEPS, 1, 0, 0, 0, 0, 1 }; return 1; }
    if (KW("step-end")) { *t = (tf_timing){ TT_STEPS, 0, 0, 0, 0, 0, 1 }; return 1; }
#undef KW
    const char *open = memchr(p, '(', (size_t)n);
    if (!open) return 0;
    const char *close = e;
    if (close > open && close[-1] == ')') close--;
    const char *as[4], *ae[4];
    int na = args(open + 1, close, as, ae, 4);
    int nl = (int)(open - p);
    if (nl == 12 && !strncasecmp(p, "cubic-bezier", 12) && na == 4) {
        float v[4];
        for (int i = 0; i < 4; i++) { const char *q = as[i]; if (!number(&q, ae[i], &v[i])) return 0; }
        *t = (tf_timing){ TT_BEZIER, 0, (int)(v[0] * 1000), (int)(v[1] * 1000), (int)(v[2] * 1000), (int)(v[3] * 1000), 0 };
        return 1;
    }
    if (nl == 5 && !strncasecmp(p, "steps", 5) && na >= 1) {
        float k;
        const char *q = as[0];
        if (!number(&q, ae[0], &k) || k < 1) return 0;
        t->kind = TT_STEPS;
        t->steps = (int)k;
        if (na > 1) {
            int l = (int)(ae[1] - as[1]);
            if ((l == 5 && !strncasecmp(as[1], "start", 5)) || (l == 10 && !strncasecmp(as[1], "jump-start", 10))) t->pos = 1;
            else if (l == 9 && !strncasecmp(as[1], "jump-none", 9)) t->pos = 2;
            else if (l == 9 && !strncasecmp(as[1], "jump-both", 9)) t->pos = 3;
        }
        return 1;
    }
    if (nl == 6 && !strncasecmp(p, "linear", 6)) { t->kind = TT_LINEAR; return 1; }   /* linear(...): as plain linear */
    return 0;
}

static float bez(float a, float b, float t) { float u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; }

/* the eased progress for x in [0, 1] */
static float ease(const tf_timing *t, float x)
{
    if (t->kind == TT_LINEAR) return x;
    if (t->kind == TT_STEPS) {
        int n = t->steps < 1 ? 1 : t->steps;
        int jumps = t->pos == 2 ? n - 1 : t->pos == 3 ? n + 1 : n;
        if (jumps < 1) jumps = 1;
        int step = (int)(x * (float)n);
        if (t->pos == 1 || t->pos == 3) step++;
        if (x >= 1) step = t->pos == 2 ? n - 1 : t->pos == 3 ? n : n;
        if (x >= 1 && (t->pos == 0 || t->pos == 1)) return 1;
        if (step < 0) step = 0;
        if (step > jumps) step = jumps;
        return (float)step / (float)jumps;
    }
    float x1 = (float)t->x1 / 1000, y1 = (float)t->y1 / 1000, x2 = (float)t->x2 / 1000, y2 = (float)t->y2 / 1000;
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    /* the curve's parameter for this x: bisection (x(t) only grows on [0, 1]) */
    float lo = 0, hi = 1, m = x;
    for (int i = 0; i < 24; i++) {
        m = (lo + hi) / 2;
        float bx = bez(x1, x2, m);
        if (fabs_(bx - x) < 0.0001f) break;
        if (bx < x) lo = m; else hi = m;
    }
    return bez(y1, y2, m);
}

/* a time: 0.3s, 300ms */
static int parse_time(const char *p, const char *e, int *ms)
{
    float f;
    const char *q = p;
    if (!number(&q, e, &f)) return 0;
    if (unit_is(q, e, "ms")) *ms = (int)f;
    else if (unit_is(q, e, "s")) *ms = (int)(f * 1000);
    else if (q == e && f == 0) *ms = 0;
    else return 0;
    return 1;
}

/* the items of a comma list */
static int list_items(const char *v, const char **is, const char **ie, int max)
{
    if (!v) return 0;
    const char *e = v + strlen(v);
    int n = 0;
    const char *p = v;
    while (p < e && n < max) {
        const char *q = p;
        int depth = 0;
        while (q < e && (depth || *q != ',')) { if (*q == '(') depth++; else if (*q == ')') depth--; q++; }
        const char *a = skip(p, q), *b = q;
        while (b > a && ws(b[-1])) b--;
        if (b - a > 10 && !strncasecmp(b - 10, "!important", 10)) { b -= 10; while (b > a && ws(b[-1])) b--; }
        is[n] = a; ie[n] = b; n++;
        p = q < e ? q + 1 : q;
    }
    return n;
}

void anim_set_transitions(tf_style *t, const char *sh, const char *props_, const char *durs, const char *tims, const char *dels)
{
    t->ntrans = 0;
    tf_trans tr[4];
    int n = 0;
    const char *is[4], *ie[4];
    int k = list_items(sh, is, ie, 4);
    for (int i = 0; i < k; i++) {
        tf_trans x = { AP_ALL, 0, 0, { TT_BEZIER, 0, 250, 100, 250, 1000, 0 } };
        int times = 0, prop_ok = 1;
        for (const char *p = is[i]; p < ie[i];) {
            p = skip(p, ie[i]);
            const char *we = word_end(p, ie[i]);
            if (we == p) { p++; continue; }
            int ms;
            if (parse_time(p, we, &ms)) { if (times++ == 0) x.dur = ms; else x.delay = ms; }
            else if (!parse_timing(p, we, &x.tf)) {
                int id = anim_prop_id(p, (int)(we - p));
                if ((int)(we - p) == 4 && !strncasecmp(p, "none", 4)) prop_ok = 0;
                else if (id < 0) prop_ok = 0;       /* a property that does not move here */
                else x.prop = (uint8_t)id;
            }
            p = we;
        }
        if (prop_ok && n < 4) tr[n++] = x;
    }
    /* the longhands: lists matched up by position, repeated as needed */
    if (props_ || durs || tims || dels) {
        const char *ps[4], *pe[4];
        int np = props_ ? list_items(props_, ps, pe, 4) : 0;
        if (props_) {
            n = 0;
            for (int i = 0; i < np; i++) {
                int id = anim_prop_id(ps[i], (int)(pe[i] - ps[i]));
                if (id < 0) continue;
                tr[n] = (tf_trans){ (uint8_t)id, 0, 0, { TT_BEZIER, 0, 250, 100, 250, 1000, 0 } };
                tr[n].prop = (uint8_t)id;
                n++;
            }
        }
        const char *vs[4], *ve[4];
        int nv;
        if (durs && (nv = list_items(durs, vs, ve, 4)) > 0) for (int i = 0; i < n; i++) parse_time(vs[i % nv], ve[i % nv], &tr[i].dur);
        if (dels && (nv = list_items(dels, vs, ve, 4)) > 0) for (int i = 0; i < n; i++) parse_time(vs[i % nv], ve[i % nv], &tr[i].delay);
        if (tims && (nv = list_items(tims, vs, ve, 4)) > 0) for (int i = 0; i < n; i++) parse_timing(vs[i % nv], ve[i % nv], &tr[i].tf);
    }
    for (int i = 0; i < n; i++) if (tr[i].dur > 0 || tr[i].delay > 0) t->trans[t->ntrans++] = tr[i];
}

static int kw_in(const char *p, const char *e, const char *const *list, int n)
{
    int l = (int)(e - p);
    for (int i = 0; i < n; i++) if ((int)strlen(list[i]) == l && !strncasecmp(p, list[i], (size_t)l)) return i;
    return -1;
}

static const char *const dirs[] = { "normal", "reverse", "alternate", "alternate-reverse" };
static const char *const fills[] = { "none", "forwards", "backwards", "both" };
static const char *const plays[] = { "running", "paused" };

static int parse_iter(const char *p, const char *e, int *out)
{
    if ((int)(e - p) == 8 && !strncasecmp(p, "infinite", 8)) { *out = -1; return 1; }
    float f;
    const char *q = p;
    if (!number(&q, e, &f) || q != e || f < 0) return 0;
    *out = (int)(f * 1000);
    return 1;
}

void anim_set_animations(tf_style *t, const char *sh, const char *const lh[8], anim_kf_lookup look, void *ctx)
{
    t->nanim = 0;
    tf_animdecl an[3];
    int n = 0;
    const char *is[3], *ie[3];
    int k = list_items(sh, is, ie, 3);
    for (int i = 0; i < k; i++) {
        tf_animdecl x = { -1, AD_NORMAL, AF_NONE, 0, 0, 0, 1000, { TT_BEZIER, 0, 250, 100, 250, 1000, 0 } };
        int times = 0;
        for (const char *p = is[i]; p < ie[i];) {
            p = skip(p, ie[i]);
            const char *we = word_end(p, ie[i]);
            if (we == p) { p++; continue; }
            int ms, v;
            if (parse_time(p, we, &ms)) { if (times++ == 0) x.dur = ms; else x.delay = ms; }
            else if (parse_timing(p, we, &x.tf)) { }
            else if (parse_iter(p, we, &x.iter)) { }
            else if ((v = kw_in(p, we, dirs, 4)) >= 0 && (v || x.dir == 0)) x.dir = (uint8_t)v;
            else if ((v = kw_in(p, we, fills, 4)) > 0) x.fill = (uint8_t)v;
            else if ((v = kw_in(p, we, plays, 2)) >= 0) x.paused = (uint8_t)v;
            else if (x.kf < 0) x.kf = (int16_t)look(ctx, *p == '"' || *p == '\'' ? p + 1 : p, (int)(we - p) - (*p == '"' || *p == '\'' ? 2 : 0));
            p = we;
        }
        an[n++] = x;
    }
    /* longhands: name, duration, timing, delay, iteration count, direction, fill mode, play state */
    if (lh[0]) {
        const char *ns[3], *ne[3];
        int nn = list_items(lh[0], ns, ne, 3);
        n = 0;
        for (int i = 0; i < nn; i++) {
            an[n] = (tf_animdecl){ -1, AD_NORMAL, AF_NONE, 0, 0, 0, 1000, { TT_BEZIER, 0, 250, 100, 250, 1000, 0 } };
            const char *a = ns[i], *b = ne[i];
            if (b - a >= 2 && (*a == '"' || *a == '\'')) { a++; b--; }
            if (!((int)(b - a) == 4 && !strncasecmp(a, "none", 4))) an[n].kf = (int16_t)look(ctx, a, (int)(b - a));
            n++;
        }
    }
    const char *vs[3], *ve[3];
    int nv;
    for (int f = 1; f < 8; f++) {
        if (!lh[f] || (nv = list_items(lh[f], vs, ve, 3)) <= 0) continue;
        for (int i = 0; i < n; i++) {
            const char *a = vs[i % nv], *b = ve[i % nv];
            int v;
            switch (f) {
            case 1: parse_time(a, b, &an[i].dur); break;
            case 2: parse_timing(a, b, &an[i].tf); break;
            case 3: parse_time(a, b, &an[i].delay); break;
            case 4: parse_iter(a, b, &an[i].iter); break;
            case 5: if ((v = kw_in(a, b, dirs, 4)) >= 0) an[i].dir = (uint8_t)v; break;
            case 6: if ((v = kw_in(a, b, fills, 4)) >= 0) an[i].fill = (uint8_t)v; break;
            case 7: if ((v = kw_in(a, b, plays, 2)) >= 0) an[i].paused = (uint8_t)v; break;
            }
        }
    }
    for (int i = 0; i < n; i++) if (an[i].kf >= 0) t->anim[t->nanim++] = an[i];
}

/* ------------------------------------------------------------------ @keyframes */

static int kf_cmp(const void *a, const void *b) { return ((const tf_kframe *)a)->off - ((const tf_kframe *)b)->off; }

/* the inside of @keyframes name { ... }: "from { ... } 50% { ... } to { ... }" */
tf_keyframes *anim_parse_keyframes(const char *name, int nlen, const char *body, const char *e)
{
    tf_keyframes *k = calloc(1, sizeof(*k));
    if (!k) return 0;
    k->name = strndup_(name, (size_t)nlen);
    int cap = 0;
    const char *p = body;
    while (p < e) {
        p = skip(p, e);
        if (p + 1 < e && p[0] == '/' && p[1] == '*') { const char *c = p + 2; while (c + 1 < e && !(c[0] == '*' && c[1] == '/')) c++; p = c + 2; continue; }
        const char *brace = memchr(p, '{', (size_t)(e - p));
        if (!brace) break;
        const char *close = memchr(brace, '}', (size_t)(e - brace));
        if (!close) close = e;
        /* the declarations, once for all the offsets in front */
        tf_kframe proto;
        memset(&proto, 0, sizeof(proto));
        for (const char *d = brace + 1; d < close;) {
            d = skip(d, close);
            const char *semi = d;
            int depth = 0;
            while (semi < close && (depth || *semi != ';')) { if (*semi == '(') depth++; else if (*semi == ')') depth--; semi++; }
            const char *colon = memchr(d, ':', (size_t)(semi - d));
            if (colon) {
                const char *ne = colon;
                while (ne > d && ws(ne[-1])) ne--;
                int nl = (int)(ne - d);
                if ((nl == 25 && !strncasecmp(d, "animation-timing-function", 25)) ||
                    (nl == 33 && !strncasecmp(d, "-webkit-animation-timing-function", 33))) {
                    const char *v = skip(colon + 1, semi), *ve = semi;
                    while (ve > v && ws(ve[-1])) ve--;
                    proto.has_tf = (uint8_t)parse_timing(v, ve, &proto.tf);
                } else {
                    int id = anim_prop_id(d, nl);
                    if (id >= 0 && id != AP_ALL) {
                        tf_kdecl *nd = realloc(proto.decl, (size_t)(proto.ndecl + 1) * sizeof(tf_kdecl));
                        if (nd) {
                            proto.decl = nd;
                            nd[proto.ndecl].prop = (uint8_t)id;
                            nd[proto.ndecl].value = strndup_(colon + 1, (size_t)(semi - colon - 1));
                            proto.ndecl++;
                        }
                    }
                }
            }
            d = semi + 1;
        }
        int used = 0;
        for (const char *s = p; s < brace;) {
            s = skip(s, brace);
            const char *se = s;
            while (se < brace && *se != ',') se++;
            const char *te = se;
            while (te > s && ws(te[-1])) te--;
            int off = -1;
            float f;
            const char *q = s;
            if ((int)(te - s) == 4 && !strncasecmp(s, "from", 4)) off = 0;
            else if ((int)(te - s) == 2 && !strncasecmp(s, "to", 2)) off = 1000;
            else if (number(&q, te, &f)) off = (int)(f * 10);
            if (off >= 0 && off <= 1000) {
                if (k->nf == cap) {
                    int nc = cap ? cap * 2 : 4;
                    tf_kframe *nf = realloc(k->f, (size_t)nc * sizeof(tf_kframe));
                    if (!nf) break;
                    k->f = nf;
                    cap = nc;
                }
                tf_kframe *fr = &k->f[k->nf++];
                *fr = proto;
                fr->off = off;
                if (used) {                             /* (each frame owns its declarations) */
                    fr->decl = malloc((size_t)(proto.ndecl ? proto.ndecl : 1) * sizeof(tf_kdecl));
                    if (!fr->decl) { fr->ndecl = 0; }
                    for (int i = 0; fr->decl && i < proto.ndecl; i++) { fr->decl[i].prop = proto.decl[i].prop; fr->decl[i].value = strdup(proto.decl[i].value ? proto.decl[i].value : ""); }
                }
                used = 1;
            }
            s = se + 1;
        }
        if (!used) { for (int i = 0; i < proto.ndecl; i++) free(proto.decl[i].value); free(proto.decl); }
        p = close + 1;
    }
    if (k->nf > 1) qsort(k->f, (size_t)k->nf, sizeof(tf_kframe), kf_cmp);
    return k;
}

void anim_free_keyframes(tf_keyframes *k)
{
    if (!k) return;
    for (int i = 0; i < k->nf; i++) {
        for (int j = 0; j < k->f[i].ndecl; j++) free(k->f[i].decl[j].value);
        free(k->f[i].decl);
    }
    free(k->f);
    free(k->name);
    free(k);
}

/* ------------------------------------------------------------------ values in and out of a style */

static void get_value(const tf_style *s, int prop, aval *v)
{
    memset(v, 0, sizeof(*v));
    switch (prop) {
    case AP_OPACITY: v->n = s->opacity * 1000 / 255; break;
    case AP_COLOR: v->c = 0xFF000000u | s->color; break;
    case AP_BG: v->c = s->bg; break;
    case AP_BORDER_COLOR: v->c = s->border_color[0] == 0xFFFFFFFFu ? 0 : 0xFF000000u | s->border_color[0]; break;
    case AP_TRANSFORM: v->nx = s->nxf; memcpy(v->x, s->xf, sizeof(v->x)); break;
    case AP_TRANSLATE: case AP_ROTATE: case AP_SCALE: {
        const tf_xop *o = &s->ind[prop - AP_TRANSLATE];
        if (o->kind != XT_NONE) { v->nx = 1; v->x[0] = *o; }
        break;
    }
    case AP_WIDTH: v->l = s->width; break;
    case AP_HEIGHT: v->l = s->height; break;
    case AP_LEFT: v->l = s->offs[3]; break;
    case AP_TOP: v->l = s->offs[0]; break;
    case AP_RIGHT: v->l = s->offs[1]; break;
    case AP_BOTTOM: v->l = s->offs[2]; break;
    case AP_MARGIN_LEFT: v->l = s->margin[3]; break;
    case AP_MARGIN_TOP: v->l = s->margin[0]; break;
    case AP_FONT_SIZE: v->n = s->font_px; break;
    case AP_RADIUS: v->n = s->radius; break;
    case AP_MAX_HEIGHT: v->l = s->max_height; break;
    }
}

static void set_value(tf_style *s, int prop, const aval *v)
{
    switch (prop) {
    case AP_OPACITY: { int o = v->n * 255 / 1000; s->opacity = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o); break; }
    case AP_COLOR: s->color = v->c & 0xFFFFFF; break;
    case AP_BG: s->bg = v->c; break;
    case AP_BORDER_COLOR:
        for (int i = 0; i < 4; i++) s->border_color[i] = (v->c >> 24) ? (v->c & 0xFFFFFF) : 0xFFFFFFFFu;
        break;
    case AP_TRANSFORM: s->nxf = v->nx; memcpy(s->xf, v->x, sizeof(s->xf)); break;
    case AP_TRANSLATE: case AP_ROTATE: case AP_SCALE:
        if (v->nx) s->ind[prop - AP_TRANSLATE] = v->x[0]; else memset(&s->ind[prop - AP_TRANSLATE], 0, sizeof(tf_xop));
        break;
    case AP_WIDTH: s->width = v->l; break;
    case AP_HEIGHT: s->height = v->l; break;
    case AP_LEFT: s->offs[3] = v->l; break;
    case AP_TOP: s->offs[0] = v->l; break;
    case AP_RIGHT: s->offs[1] = v->l; break;
    case AP_BOTTOM: s->offs[2] = v->l; break;
    case AP_MARGIN_LEFT: s->margin[3] = v->l; break;
    case AP_MARGIN_TOP: s->margin[0] = v->l; break;
    case AP_FONT_SIZE:
        if (v->n > 0 && s->font_px > 0) { s->line_px = s->line_px * v->n / s->font_px; s->font_px = v->n; }
        break;
    case AP_RADIUS: s->radius = v->n < 0 ? 0 : v->n; break;
    case AP_MAX_HEIGHT: s->max_height = v->l; break;
    }
}

static int same_value(int prop, const aval *a, const aval *b)
{
    switch (props[prop].kind) {
    case K_NUM: return a->n == b->n;
    case K_COLOR: return a->c == b->c || (!(a->c >> 24) && !(b->c >> 24));
    case K_LEN: return a->l.v == b->l.v && a->l.pct == b->l.pct && a->l.autov == b->l.autov;
    case K_XF: return a->nx == b->nx && !memcmp(a->x, b->x, sizeof(tf_xop) * a->nx);
    }
    return 1;
}

static int mix_i(int a, int b, float t) { float v = (float)a + (float)(b - a) * t; return (int)(v < 0 ? v - 0.5f : v + 0.5f); }

static uint32_t mix_color(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    /* from or to transparent: the colour stays, only how much of it shows changes */
    if (!(a >> 24)) a = b & 0xFFFFFF;
    if (!(b >> 24)) b = a & 0xFFFFFF;
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) r |= (uint32_t)mix_i((int)((a >> sh) & 255), (int)((b >> sh) & 255), t) << sh;
    return r;
}

static tf_xop identity(uint8_t kind) { return (tf_xop){ kind, 0, kind == XT_SCALE ? 1000 : 0, kind == XT_SCALE ? 1000 : 0 }; }

/* a list of transform functions as translate, rotate, scale (px only: 0 if it has percentages) */
static int decompose(const aval *v, float *tx, float *ty, float *rot, float *sx, float *sy)
{
    float m[6] = { 1, 0, 0, 1, 0, 0 };
    for (int i = 0; i < v->nx; i++) {
        const tf_xop *o = &v->x[i];
        float n[6] = { 1, 0, 0, 1, 0, 0 };
        if (o->kind == XT_TRANSLATE) { if (o->pct) return 0; n[4] = (float)o->a; n[5] = (float)o->b; }
        else if (o->kind == XT_SCALE) { n[0] = (float)o->a / 1000; n[3] = (float)o->b / 1000; }
        else if (o->kind == XT_ROTATE) { float d = (float)o->a / 100; n[0] = cos_deg(d); n[1] = sin_deg(d); n[2] = -n[1]; n[3] = n[0]; }
        float r[6] = { m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
                       m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5] };
        memcpy(m, r, sizeof(m));
    }
    *tx = m[4]; *ty = m[5];
    *sx = sqrt_(m[0] * m[0] + m[1] * m[1]);
    *sy = *sx ? (m[0] * m[3] - m[1] * m[2]) / *sx : 0;
    *rot = atan2_deg(m[1], m[0]);
    return 1;
}

static void mix(int prop, const aval *a, const aval *b, float t, aval *out)
{
    memset(out, 0, sizeof(*out));
    switch (props[prop].kind) {
    case K_NUM: out->n = mix_i(a->n, b->n, t); if (prop == AP_OPACITY) { if (out->n < 0) out->n = 0; if (out->n > 1000) out->n = 1000; } break;
    case K_COLOR: out->c = mix_color(a->c, b->c, t); break;
    case K_LEN:
        if (a->l.autov || b->l.autov || a->l.pct != b->l.pct) out->l = t < 0.5f ? a->l : b->l;   /* (no way between those) */
        else { out->l = a->l; out->l.v = mix_i(a->l.v, b->l.v, t); }
        break;
    case K_XF: {
        aval A = *a, B = *b;
        if (!A.nx) { A.nx = B.nx; for (int i = 0; i < B.nx; i++) A.x[i] = identity(B.x[i].kind); }
        if (!B.nx) { B.nx = A.nx; for (int i = 0; i < A.nx; i++) B.x[i] = identity(A.x[i].kind); }
        int same = A.nx == B.nx;
        for (int i = 0; same && i < A.nx; i++) same = A.x[i].kind == B.x[i].kind && A.x[i].pct == B.x[i].pct;
        if (same) {
            out->nx = A.nx;
            for (int i = 0; i < A.nx; i++) {
                out->x[i] = A.x[i];
                out->x[i].a = mix_i(A.x[i].a, B.x[i].a, t);
                out->x[i].b = mix_i(A.x[i].b, B.x[i].b, t);
            }
            break;
        }
        float t1, u1, r1, s1, z1, t2, u2, r2, s2, z2;
        if (!decompose(&A, &t1, &u1, &r1, &s1, &z1) || !decompose(&B, &t2, &u2, &r2, &s2, &z2)) { *out = t < 0.5f ? A : B; break; }
        out->nx = 3;
        out->x[0] = (tf_xop){ XT_TRANSLATE, 0, (int)(t1 + (t2 - t1) * t), (int)(u1 + (u2 - u1) * t) };
        out->x[1] = (tf_xop){ XT_ROTATE, 0, (int)((r1 + (r2 - r1) * t) * 100), 0 };
        out->x[2] = (tf_xop){ XT_SCALE, 0, (int)((s1 + (s2 - s1) * t) * 1000), (int)((z1 + (z2 - z1) * t) * 1000) };
        break;
    }
    }
}

/* ------------------------------------------------------------------ matrices (layout.c) */

static void apply_op(float m[6], const tf_xop *o, int bw, int bh)
{
    float n[6] = { 1, 0, 0, 1, 0, 0 };
    if (o->kind == XT_TRANSLATE) {
        n[4] = (o->pct & 1) ? (float)o->a * (float)bw / 10000 : (float)o->a;
        n[5] = (o->pct & 2) ? (float)o->b * (float)bh / 10000 : (float)o->b;
    } else if (o->kind == XT_SCALE) { n[0] = (float)o->a / 1000; n[3] = (float)o->b / 1000; }
    else if (o->kind == XT_ROTATE) { float d = (float)o->a / 100; n[0] = cos_deg(d); n[1] = sin_deg(d); n[2] = -n[1]; n[3] = n[0]; }
    else return;
    float r[6] = { m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
                   m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5] };
    memcpy(m, r, sizeof(r));
}

int anim_matrix(const tf_style *s, int bw, int bh, float m[6])
{
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;
    int any = 0;
    for (int i = 0; i < 3; i++) if (s->ind[i].kind != XT_NONE) { apply_op(m, &s->ind[i], bw, bh); any = 1; }
    for (int i = 0; i < s->nxf && i < TF_MAXX; i++) { apply_op(m, &s->xf[i], bw, bh); any = 1; }
    if (!any) return 0;
    return !(m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0);
}

void anim_matrix_parts(const float m[6], float *sx, float *sy, float *rot)
{
    *sx = sqrt_(m[0] * m[0] + m[1] * m[1]);
    *sy = *sx ? (m[0] * m[3] - m[1] * m[2]) / *sx : 0;
    *rot = atan2_deg(m[1], m[0]);
}

/* ------------------------------------------------------------------ runs */

enum { R_TRANS, R_CSS, R_JS };

/* one property's keyframes, read for the element */
typedef struct { uint8_t prop, has_tf; int off; tf_timing tf; aval v; } kval;

typedef struct {
    uint8_t kind, prop;        /* prop: R_TRANS */
    aval from, to;             /* R_TRANS */
    kval *kv;                  /* R_CSS, R_JS: the frames' values, by property then offset */
    int nkv;
    uint32_t mask;             /* the properties it sets */
    tf_keyframes *own;         /* R_JS: its keyframes */
    char name[64];             /* R_CSS: the animation's name; R_TRANS: the property's */
    int id;                    /* R_JS */
    int slot;                  /* R_CSS: which of animation-name's */
    uint32_t start;
    int dur, delay, iter;
    uint8_t dir, fill, paused, rev;
    uint32_t hold;             /* paused: its time then */
    tf_timing tf;
    int started, iter_seen, ended;
    int cur_iter;
} run;

typedef struct {
    tf_node *n;
    aval base[AP_N];
    aval last[AP_N];           /* what the runs wrote last time */
    run *r;
    int nr, rcap;
    uint32_t touched;          /* what the runs wrote last time */
    int gen;
} arec;

typedef struct { tf_node *n; char type[24]; char name[64]; int elapsed, id; } aevent;

struct tf_anims {
    arec **recs;
    int nrecs, cap;
    aevent *ev;
    int nev, evcap, evhead;
    int next_id;
    int gen;
    int restyled;              /* the styles were computed again: every value moving is written again */
};

tf_anims *anim_new(void) { tf_anims *a = calloc(1, sizeof(*a)); if (a) a->next_id = 1; return a; }

static void run_free(run *r) { free(r->kv); anim_free_keyframes(r->own); r->kv = 0; r->own = 0; }

void anim_free(tf_anims *a)
{
    if (!a) return;
    for (int i = 0; i < a->nrecs; i++) {
        arec *r = a->recs[i];
        for (int j = 0; j < r->nr; j++) run_free(&r->r[j]);
        free(r->r);
        if (r->n && r->n->anim == r) r->n->anim = 0;
        free(r);
    }
    free(a->recs);
    free(a->ev);
    free(a);
}

static void event(tf_anims *a, tf_node *n, const char *type, const char *name, int elapsed, int id)
{
    if (a->nev - a->evhead > 256) return;
    if (a->evhead && a->evhead == a->nev) a->evhead = a->nev = 0;
    if (a->nev == a->evcap) {
        int nc = a->evcap ? a->evcap * 2 : 16;
        aevent *ne = realloc(a->ev, (size_t)nc * sizeof(aevent));
        if (!ne) return;
        a->ev = ne;
        a->evcap = nc;
    }
    aevent *e = &a->ev[a->nev++];
    e->n = n;
    strncpy(e->type, type, sizeof(e->type) - 1); e->type[sizeof(e->type) - 1] = 0;
    strncpy(e->name, name ? name : "", sizeof(e->name) - 1); e->name[sizeof(e->name) - 1] = 0;
    e->elapsed = elapsed;
    e->id = id;
}

int anim_take_event(tf_anims *a, tf_node **node, char *type, int tcap, char *name, int ncap, int *elapsed_ms, int *id)
{
    if (!a || a->evhead >= a->nev) return 0;
    aevent *e = &a->ev[a->evhead++];
    *node = e->n;
    strncpy(type, e->type, (size_t)tcap - 1); type[tcap - 1] = 0;
    strncpy(name, e->name, (size_t)ncap - 1); name[ncap - 1] = 0;
    *elapsed_ms = e->elapsed;
    *id = e->id;
    return 1;
}

static arec *rec_for(tf_anims *a, tf_node *n)
{
    if (n->anim) return n->anim;
    if (a->nrecs == a->cap) {
        int nc = a->cap ? a->cap * 2 : 32;
        arec **nr = realloc(a->recs, (size_t)nc * sizeof(arec *));
        if (!nr) return 0;
        a->recs = nr;
        a->cap = nc;
    }
    arec *r = calloc(1, sizeof(*r));
    if (!r) return 0;
    r->n = n;
    for (int p = 0; p < AP_N; p++) get_value(n->style, p, &r->base[p]);
    a->recs[a->nrecs++] = r;
    n->anim = r;
    return r;
}

static run *add_run(arec *r)
{
    if (r->nr == r->rcap) {
        int nc = r->rcap ? r->rcap * 2 : 4;
        run *nn = realloc(r->r, (size_t)nc * sizeof(run));
        if (!nn) return 0;
        r->r = nn;
        r->rcap = nc;
    }
    run *x = &r->r[r->nr++];
    memset(x, 0, sizeof(*x));
    return x;
}

static void drop_run(arec *r, int i)
{
    run_free(&r->r[i]);
    memmove(&r->r[i], &r->r[i + 1], (size_t)(r->nr - i - 1) * sizeof(run));
    r->nr--;
}

static int kv_cmp(const void *a, const void *b)
{
    const kval *x = a, *y = b;
    return x->prop != y->prop ? x->prop - y->prop : x->off - y->off;
}

/* the keyframes' values read for the element n (its font size, its colour) */
static void read_frames(run *x, const tf_keyframes *k, const tf_style *s)
{
    int total = 0;
    for (int i = 0; i < k->nf; i++) total += k->f[i].ndecl;
    x->kv = calloc((size_t)(total ? total : 1), sizeof(kval));
    x->nkv = 0;
    x->mask = 0;
    if (!x->kv) return;
    for (int i = 0; i < k->nf; i++) {
        const tf_kframe *f = &k->f[i];
        for (int j = 0; j < f->ndecl; j++) {
            kval *v = &x->kv[x->nkv];
            const char *val = f->decl[j].value ? f->decl[j].value : "";
            if (!parse_value(f->decl[j].prop, val, val + strlen(val), s, &v->v)) continue;
            v->prop = f->decl[j].prop;
            v->off = f->off;
            v->has_tf = f->has_tf;
            v->tf = f->tf;
            x->mask |= 1u << v->prop;
            x->nkv++;
        }
    }
    qsort(x->kv, (size_t)x->nkv, sizeof(kval), kv_cmp);
}

/* a keyframe run's value of prop at progress p (0..1000 of an iteration): base where no frame says */
static void sample(const run *x, int prop, float p, const aval *base, aval *out)
{
    int a = -1, b = -1;
    for (int i = 0; i < x->nkv; i++) {
        if (x->kv[i].prop != prop) continue;
        if ((float)x->kv[i].off <= p * 1000) a = i;
        else if (b < 0) b = i;
    }
    const aval *va = a >= 0 ? &x->kv[a].v : base, *vb = b >= 0 ? &x->kv[b].v : base;
    float oa = a >= 0 ? (float)x->kv[a].off / 1000 : 0, ob = b >= 0 ? (float)x->kv[b].off / 1000 : 1;
    if (a >= 0 && b < 0 && x->kv[a].off == 1000) { *out = *va; return; }
    float t = ob > oa ? (p - oa) / (ob - oa) : 1;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const tf_timing *tf = a >= 0 && x->kv[a].has_tf ? &x->kv[a].tf : &x->tf;
    mix(prop, va, vb, ease(tf, t), out);
}

/* a keyframe run at now: its progress in its iteration (0..1, direction done), or -1 when it shows nothing.
 * Events on the way. */
static float progress(tf_anims *a, arec *r, run *x, uint32_t now, int *moving)
{
    int el = x->paused ? (int)x->hold : (int)(now - x->start);
    int dur = x->dur > 0 ? x->dur : 0;
    int64_t active = x->iter < 0 ? ((int64_t)1 << 40) : (int64_t)dur * x->iter / 1000;
    int64_t t = (int64_t)el - x->delay;
    if (x->rev) t = active - t;
    const char *nm = x->kind == R_CSS ? x->name : "";
    if (!x->paused && !x->ended) *moving = 1;
    if (t < 0) {
        if (!x->rev) return (x->fill & AF_BACKWARDS) ? ((x->dir == AD_REVERSE || x->dir == AD_ALT_REVERSE) ? 1 : 0) : -1;
        t = 0;
    }
    if (!x->started) {
        x->started = 1;
        if (x->kind == R_CSS) event(a, r->n, "animationstart", nm, 0, 0);
    }
    if (t >= active || (x->rev && t <= 0 && el > 0)) {
        if (!x->ended) {
            x->ended = 1;
            if (x->kind == R_CSS) event(a, r->n, "animationend", nm, (int)active, 0);
            else event(a, r->n, "__waapi", "finish", 0, x->id);
        }
        if (x->rev) return (x->fill & AF_BACKWARDS) ? 0 : -1;
        if (!(x->fill & AF_FORWARDS) || x->iter < 0) return -1;
        int whole = x->iter / 1000, frac = x->iter % 1000;
        int it = frac ? whole : whole - 1;
        float p = frac ? (float)frac / 1000 : 1;
        if (it < 0) { it = 0; p = 0; }
        int back = x->dir == AD_REVERSE || (x->dir == AD_ALTERNATE && (it & 1)) || (x->dir == AD_ALT_REVERSE && !(it & 1));
        return back ? 1 - p : p;
    }
    x->ended = 0;
    if (!dur) return 1;
    int it = (int)(t / dur);
    float p = (float)(t % dur) / (float)dur;
    if (it > x->iter_seen && x->kind == R_CSS) event(a, r->n, "animationiteration", nm, (int)t, 0);
    x->iter_seen = it;
    x->cur_iter = it;
    int back = x->dir == AD_REVERSE || (x->dir == AD_ALTERNATE && (it & 1)) || (x->dir == AD_ALT_REVERSE && !(it & 1));
    return back ? 1 - p : p;
}

/* a transition's value at now; 0 once it is over (and gone) */
static int trans_value(tf_anims *a, arec *r, run *x, uint32_t now, aval *out, int *moving)
{
    int t = (int)(now - x->start) - x->delay;
    if (t >= 0 && !x->started) { x->started = 1; event(a, r->n, "transitionstart", x->name, 0, 0); }
    if (t >= x->dur) { event(a, r->n, "transitionend", x->name, x->dur, 0); return 0; }
    *moving = 1;
    if (t < 0) { *out = x->from; return 1; }
    mix(x->prop, &x->from, &x->to, ease(&x->tf, (float)t / (float)x->dur), out);
    return 1;
}

/* what a record's runs make of its properties now, into vals (mask: which) */
static uint32_t values(tf_anims *a, arec *r, uint32_t now, aval *vals, int *moving)
{
    uint32_t mask = 0;
    for (int pass = R_TRANS; pass <= R_JS; pass++) {
        for (int i = 0; i < r->nr; i++) {
            run *x = &r->r[i];
            if (x->kind != pass) continue;
            if (x->kind == R_TRANS) {
                if (trans_value(a, r, x, now, &vals[x->prop], moving)) mask |= 1u << x->prop;
                else { drop_run(r, i); i--; }
                continue;
            }
            float p = progress(a, r, x, now, moving);
            if (p < 0) continue;
            for (int prop = 0; prop < AP_N; prop++) {
                if (!(x->mask & (1u << prop))) continue;
                aval under = (mask & (1u << prop)) ? vals[prop] : r->base[prop];
                sample(x, prop, p, &under, &vals[prop]);
                mask |= 1u << prop;
            }
        }
    }
    return mask;
}

static const tf_trans *trans_for(const tf_style *s, int prop)
{
    const tf_trans *best = 0;
    for (int i = 0; i < s->ntrans; i++) if (s->trans[i].prop == prop || s->trans[i].prop == AP_ALL) best = &s->trans[i];
    return best;
}

void anim_restyled(tf_anims *a, tf_doc *d, tf_styler *st, uint32_t now)
{
    if (!a || !d || !d->root) return;
    a->gen++;
    a->restyled = 1;
    int uses = style_uses_transitions(st);
    for (tf_node *n = d->root->first; n; n = dom_next(n, d->root)) {
        if (n->type != TF_ELEM || !n->style) continue;
        if (dom_is(n, "html") && n->laid) { viewport_w = n->lw > 0 ? n->lw : viewport_w; }
        tf_style *s = n->style;
        arec *r = n->anim;
        int none = s->display == TD_NONE;
        if (!r && (none || (!s->nanim && !(uses && s->ntrans)))) continue;
        int fresh = !r;
        if (!r && !(r = rec_for(a, n))) continue;
        r->gen = a->gen;
        if (none) {
            /* not shown: what moved on it stops */
            for (int i = r->nr - 1; i >= 0; i--) if (r->r[i].kind != R_JS) drop_run(r, i);
            for (int p = 0; p < AP_N; p++) get_value(s, p, &r->base[p]);
            continue;
        }
        if (!fresh) {
            /* transitions: what changed since the last styling, where the page says it should move */
            for (int p = 0; p < AP_N; p++) {
                aval nb;
                get_value(s, p, &nb);
                if (same_value(p, &r->base[p], &nb)) continue;
                const tf_trans *tr = s->ntrans ? trans_for(s, p) : 0;
                aval from = r->base[p];
                int had = -1;
                for (int i = 0; i < r->nr; i++) if (r->r[i].kind == R_TRANS && r->r[i].prop == p) had = i;
                int short_dur = -1;
                if (had >= 0) {
                    run *o = &r->r[had];
                    int dummy = 0;
                    aval cur;
                    if (trans_value(a, r, o, now, &cur, &dummy)) {
                        /* going back to where it came from: as long as it has come, not the whole way */
                        if (same_value(p, &o->from, &nb) && o->dur > 0) {
                            int t = (int)(now - o->start) - o->delay;
                            short_dur = t < 0 ? 0 : t > o->dur ? o->dur : t;
                        }
                        from = cur;
                        if (!tr) event(a, n, "transitioncancel", props[p].name, 0, 0);
                    }
                    if (had < r->nr && r->r[had].kind == R_TRANS && r->r[had].prop == p) drop_run(r, had);
                }
                if (tr && tr->dur > 0) {
                    run *x = add_run(r);
                    if (!x) continue;
                    x->kind = R_TRANS;
                    x->prop = (uint8_t)p;
                    x->from = from;
                    x->to = nb;
                    x->start = now;
                    x->dur = short_dur > 0 && tr->dur > short_dur ? short_dur : tr->dur;
                    x->delay = tr->delay;
                    x->tf = tr->tf;
                    strncpy(x->name, props[p].name, sizeof(x->name) - 1);
                    event(a, n, "transitionrun", x->name, 0, 0);
                }
            }
        }
        for (int p = 0; p < AP_N; p++) get_value(s, p, &r->base[p]);
        /* CSS animations: started as their names come, ended as they go */
        int keep[8] = { 0 };
        for (int i = 0; i < s->nanim; i++) {
            const tf_keyframes *k = style_keyframes(st, s->anim[i].kf);
            if (!k) continue;
            const tf_animdecl *ad = &s->anim[i];
            run *x = 0;
            for (int j = 0; j < r->nr; j++)
                if (r->r[j].kind == R_CSS && r->r[j].slot == i && !strcmp(r->r[j].name, k->name ? k->name : "")) { x = &r->r[j]; if (j < 8) keep[j] = 1; }
            if (!x) {
                x = add_run(r);
                if (!x) continue;
                x->kind = R_CSS;
                x->slot = i;
                strncpy(x->name, k->name ? k->name : "", sizeof(x->name) - 1);
                x->start = now;
                read_frames(x, k, s);
                if (r->nr - 1 < 8) keep[r->nr - 1] = 1;
            }
            x->dur = ad->dur; x->delay = ad->delay; x->iter = ad->iter; x->dir = ad->dir; x->fill = ad->fill; x->tf = ad->tf;
            if (ad->paused && !x->paused) { x->paused = 1; x->hold = now - x->start; }
            else if (!ad->paused && x->paused) { x->paused = 0; x->start = now - x->hold; }
        }
        for (int j = r->nr - 1; j >= 0; j--) if (r->r[j].kind == R_CSS && (j >= 8 || !keep[j])) drop_run(r, j);
    }
    /* elements gone from the page: their CSS runs end with them */
    for (int i = 0; i < a->nrecs; i++) {
        arec *r = a->recs[i];
        if (r->gen == a->gen) continue;
        for (int j = r->nr - 1; j >= 0; j--) if (r->r[j].kind != R_JS) drop_run(r, j);
        r->touched = 0;
    }
}

int anim_apply(tf_anims *a, uint32_t now)
{
    if (!a) return 0;
    int changed = 0, moving = 0;
    aval vals[AP_N];
    for (int i = 0; i < a->nrecs; i++) {
        arec *r = a->recs[i];
        if (!r->nr && !r->touched) continue;
        tf_style *s = r->n->style;
        if (!s) continue;
        uint32_t mask = values(a, r, now, vals, &moving);
        for (int p = 0; p < AP_N; p++) {
            uint32_t bit = 1u << p;
            if (mask & bit) {
                if (a->restyled || !(r->touched & bit) || !same_value(p, &r->last[p], &vals[p])) {
                    set_value(s, p, &vals[p]);
                    r->last[p] = vals[p];
                    changed = 1;
                }
            } else if (r->touched & bit) {
                set_value(s, p, &r->base[p]);
                changed = 1;
            }
        }
        r->touched = mask;
    }
    a->restyled = 0;
    return (changed ? 1 : 0) | (moving ? 2 : 0);
}

/* ------------------------------------------------------------------ element.animate() */

static run *js_run(tf_anims *a, int id, arec **rec)
{
    for (int i = 0; i < a->nrecs; i++)
        for (int j = 0; j < a->recs[i]->nr; j++)
            if (a->recs[i]->r[j].kind == R_JS && a->recs[i]->r[j].id == id) { if (rec) *rec = a->recs[i]; return &a->recs[i]->r[j]; }
    return 0;
}

int anim_js_add(tf_anims *a, tf_node *n, const char *frames, int len, int dur, int delay, const char *easing,
                int iter, int dir, int fill, uint32_t now)
{
    if (!a || !n || !n->style) return 0;
    arec *r = rec_for(a, n);
    if (!r) return 0;
    run *x = add_run(r);
    if (!x) return 0;
    x->kind = R_JS;
    x->id = a->next_id++;
    x->own = anim_parse_keyframes("", 0, frames, frames + len);
    x->start = now;
    x->dur = dur < 0 ? 0 : dur;
    x->delay = delay;
    x->iter = iter;
    x->dir = (uint8_t)dir;
    x->fill = (uint8_t)fill;
    if (!easing || !parse_timing(easing, easing + strlen(easing), &x->tf)) x->tf.kind = TT_LINEAR;
    if (x->own) read_frames(x, x->own, n->style);
    return x->id;
}

static int64_t js_active(const run *x) { return x->iter < 0 ? ((int64_t)1 << 40) : (int64_t)(x->dur > 0 ? x->dur : 0) * x->iter / 1000; }

void anim_js_ctl(tf_anims *a, int id, int op, int value, uint32_t now)
{
    if (!a) return;
    arec *r = 0;
    run *x = js_run(a, id, &r);
    if (!x) return;
    int el = x->paused ? (int)x->hold : (int)(now - x->start);
    switch (op) {
    case AJ_PLAY:
        if (x->ended) { x->ended = 0; x->started = 0; x->start = now; x->paused = 0; x->iter_seen = 0; break; }
        if (x->paused) { x->paused = 0; x->start = now - x->hold; }
        break;
    case AJ_PAUSE:
        if (!x->paused) { x->paused = 1; x->hold = (uint32_t)el; }
        break;
    case AJ_CANCEL:
        for (int i = 0; i < r->nr; i++) if (&r->r[i] == x) { drop_run(r, i); break; }
        break;
    case AJ_FINISH:
        if (x->iter < 0) break;
        if (x->paused) x->hold = (uint32_t)(js_active(x) + x->delay);
        else x->start = now - (uint32_t)(js_active(x) + x->delay);
        if (x->rev) { if (x->paused) x->hold = (uint32_t)x->delay; else x->start = now - (uint32_t)x->delay; }
        break;
    case AJ_REVERSE: {
        if (x->iter < 0) break;
        int64_t act = js_active(x);
        int64_t t = (int64_t)el - x->delay;
        if (x->ended) t = x->rev ? 0 : act;
        if (t < 0) t = 0;
        if (t > act) t = act;
        x->rev ^= 1;
        x->ended = 0;
        int64_t nel = (act - t) + x->delay;
        if (x->paused) x->hold = (uint32_t)nel; else x->start = now - (uint32_t)nel;
        break;
    }
    case AJ_SEEK:
        x->ended = 0;
        if (x->paused) x->hold = (uint32_t)value; else x->start = now - (uint32_t)value;
        break;
    }
}

int anim_js_info(tf_anims *a, int id, int *current, uint32_t now)
{
    run *x = a ? js_run(a, id, 0) : 0;
    if (!x) { *current = 0; return 0; }
    int el = x->paused ? (int)x->hold : (int)(now - x->start);
    int64_t act = js_active(x);
    int64_t t = el;
    if (x->rev) t = act + x->delay - el;
    if (x->iter >= 0 && t > act + x->delay) t = act + x->delay;
    if (t < 0) t = 0;
    *current = (int)t;
    if (x->ended) return 3;
    return x->paused ? 2 : 1;
}
