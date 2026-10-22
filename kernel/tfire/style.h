#ifndef TFIRE_STYLE_H
#define TFIRE_STYLE_H

/*
 * Styles: libcss (NetSurf's CSS engine) selects and cascades every element's style from tFire's default style
 * sheet, the page's own (<style>, <link rel=stylesheet>) and style="" attributes; the result is turned into a
 * tf_style, which is all layout needs to know.
 */

#include "dom.h"

enum { TD_NONE, TD_INLINE, TD_BLOCK, TD_LIST_ITEM, TD_INLINE_BLOCK, TD_TABLE, TD_ROW_GROUP, TD_ROW, TD_CELL,
       TD_CAPTION, TD_FLEX, TD_INLINE_FLEX, TD_GRID };
enum { TWS_NORMAL, TWS_PRE, TWS_NOWRAP, TWS_PRE_WRAP, TWS_PRE_LINE };
enum { TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY };
enum { TP_STATIC, TP_RELATIVE, TP_ABSOLUTE, TP_FIXED, TP_STICKY };
enum { TL_NONE, TL_DISC, TL_CIRCLE, TL_SQUARE, TL_DECIMAL };

/* a length that may be auto or a percentage of the containing block */
typedef struct { int v; uint8_t pct, autov; } tf_len;     /* pct: v is a percentage times 100 */

/* ------------------------------------------------------------------ transforms, transitions, animations */

/* one transform function: translate (a, b in px, or percent x100 of the box itself where pct says: bit 1 a,
 * bit 2 b), scale (x1000), rotate (a: degrees x100) */
enum { XT_NONE, XT_TRANSLATE, XT_SCALE, XT_ROTATE };
typedef struct { uint8_t kind, pct; int a, b; } tf_xop;
#define TF_MAXX 4

/* a timing function: linear, cubic-bezier (x1000; ease and the rest are ones of these) or steps */
enum { TT_LINEAR, TT_BEZIER, TT_STEPS };
typedef struct { uint8_t kind, pos; int x1, y1, x2, y2; int steps; } tf_timing;   /* pos: steps' 0 end, 1 start, 2 none, 3 both */

/* what can move (anim.c): the properties transitions and animations change */
enum { AP_OPACITY, AP_COLOR, AP_BG, AP_BORDER_COLOR, AP_TRANSFORM, AP_TRANSLATE, AP_ROTATE, AP_SCALE, AP_WIDTH, AP_HEIGHT,
       AP_LEFT, AP_TOP, AP_RIGHT, AP_BOTTOM, AP_MARGIN_LEFT, AP_MARGIN_TOP, AP_FONT_SIZE, AP_RADIUS, AP_MAX_HEIGHT, AP_N,
       AP_ALL = 255 };

typedef struct { uint8_t prop; int dur, delay; tf_timing tf; } tf_trans;          /* transition-*: ms */
enum { AD_NORMAL, AD_REVERSE, AD_ALTERNATE, AD_ALT_REVERSE };
enum { AF_NONE = 0, AF_FORWARDS = 1, AF_BACKWARDS = 2, AF_BOTH = 3 };
typedef struct { int16_t kf; uint8_t dir, fill, paused; int dur, delay, iter; tf_timing tf; } tf_animdecl;  /* kf: style_keyframes(), -1 none;
                                                                                                           iter x1000, -1 infinite */
/* @keyframes: its frames (offset x1000), each with the declarations it makes, as written */
typedef struct { uint8_t prop; char *value; } tf_kdecl;
typedef struct { int off; uint8_t has_tf; tf_timing tf; int ndecl; tf_kdecl *decl; } tf_kframe;
typedef struct { char *name; int nf; tf_kframe *f; } tf_keyframes;

typedef struct tf_style {
    uint8_t display, position, white_space, text_align, list_style, visible, flex_row, flex_wrap, floated;
    uint8_t bold, italic, underline, strike, upper;   /* bold: 1 bold, 2 monospace (both bits as font_of gets them) */
    uint32_t color;                      /* 0xRRGGBB */
    uint32_t bg;                         /* 0xAARRGGBB, AA = 0: none */
    int font_px, line_px;
    tf_len margin[4];                    /* top right bottom left */
    int padding[4], border[4];
    uint32_t border_color[4];
    tf_len width, height, max_width, min_width;
    int text_indent;
    int gap, row_gap;
    /* positioning, floats, boxes */
    uint8_t clear;                       /* 1 left, 2 right, 3 both */
    uint8_t clip;                        /* overflow other than visible: a formatting context of its own */
    uint8_t border_box;                  /* box-sizing: border-box */
    uint8_t valign;                      /* TV_* */
    tf_len offs[4];                      /* top right bottom left (auto when not given) */
    tf_len min_height, max_height;
    /* flex */
    uint8_t flex_dir;                    /* 0 row, 1 row-reverse, 2 column, 3 column-reverse */
    uint8_t justify;                     /* TJ_* */
    uint8_t align_items, align_self;     /* TAL_* (align_self 0 = as the container says) */
    int flex_grow, flex_shrink;          /* times 100 */
    tf_len flex_basis;
    int order;
    int radius;                          /* border-radius (px) */
    uint8_t collapse;                    /* tables: border-collapse: collapse */
    int bspacing;                        /* tables: border-spacing (horizontal; -1 = not given) */
    uint8_t opacity;                     /* 0..255 */
    /* what libcss does not know (style.c's own small cascade): grid, as written */
    const char *grid_cols, *grid_rows, *grid_areas, *grid_area, *grid_col, *grid_row;
    const char *mask;                    /* mask-image: url(...), as written: an icon painted in the background colour */
    /* box-shadow (the first one, not inset; colour 0 = none) and a linear-gradient background (up to 4 stops) */
    uint32_t shadow_color;               /* 0xAARRGGBB */
    int16_t shadow_x, shadow_y, shadow_blur, shadow_spread;
    uint8_t ngrad;
    int16_t grad_angle;                  /* degrees, CSS's: 180 = to bottom */
    uint32_t grad_c[4];                  /* 0xAARRGGBB */
    int16_t grad_p[4];                   /* positions x1000 */
    /* transform (functions in order), the translate, rotate and scale properties (kind XT_NONE: not given), transform-origin */
    uint8_t nxf;
    tf_xop xf[TF_MAXX];
    tf_xop ind[3];
    tf_len origin[2];
    /* transition-* and animation-* */
    uint8_t ntrans, nanim;
    tf_trans trans[4];
    tf_animdecl anim[3];
} tf_style;

enum { TV_BASELINE, TV_TOP, TV_MIDDLE, TV_BOTTOM, TV_SUB, TV_SUPER, TV_TEXT_TOP, TV_TEXT_BOTTOM };
enum { TJ_START, TJ_END, TJ_CENTER, TJ_BETWEEN, TJ_AROUND, TJ_EVENLY };
enum { TAL_AUTO, TAL_STRETCH, TAL_START, TAL_END, TAL_CENTER, TAL_BASELINE };

typedef struct tf_styler tf_styler;

/* a set of style sheets for one page, starting with tFire's own */
tf_styler *style_new(void);
void style_free(tf_styler *s);
/* adds a page style sheet (in document order); base resolves the url()s in it */
void style_add_sheet(tf_styler *s, const char *css, uint32_t len, const char *base);
/* the classes of the page's <html> and <body>, before its sheets are added: which custom properties apply */
void style_set_root_classes(tf_styler *s, const char *classes);
void style_set_state(tf_styler *s, tf_node *hover, tf_node *focus);   /* for :hover and :focus */
int style_hover_matters(tf_styler *s, tf_node *n);
/* and their attributes, as "name=value\n" lines ([data-theme=light] rules set custom properties too) */
void style_set_root_attrs(tf_styler *s, const char *attrs);
/* (re)computes every element's style; viewport_w for vw/vh and media queries */
void style_compute(tf_styler *s, tf_doc *d, int viewport_w, int viewport_h);
/* the same, a slice at a time: begin, then step (at most max_elements each time) until it says 1 = done.
 * The tree must not change in between (the engine finishes the styling first when it is about to). */
void style_compute_begin(tf_styler *s, tf_doc *d, int viewport_w, int viewport_h);
int style_compute_step(tf_styler *s, int max_elements);
int style_computing(tf_styler *s);
void style_node_free(tf_node *n);
/* the page's @keyframes rule number i (tf_animdecl.kf), or 0 */
const tf_keyframes *style_keyframes(tf_styler *s, int i);
int style_uses_transitions(tf_styler *s);             /* any transition-* in the page's sheets (or style="") */

#endif
