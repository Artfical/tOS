#ifndef TFIRE_LAYOUT_H
#define TFIRE_LAYOUT_H

/*
 * Layout: the styled tree becomes a list of things to draw at document coordinates (a display list).
 *
 * Block boxes stack, with margins, borders, padding, backgrounds, widths and auto margins (centring); inline
 * content is broken into lines of words, pictures and form fields, aligned as text-align says, and flows around
 * floats; lists get their markers, tables their columns; flex containers and grids place their items (grid from
 * grid-template-columns and -areas); relative, absolute and fixed boxes go where their offsets say.
 */

#include "dom.h"
#include "style.h"

enum { IT_RECT, IT_TEXT, IT_IMAGE, IT_HLINE, IT_FIELD };

typedef struct tf_item {
    uint8_t kind;
    uint8_t font;                  /* IT_TEXT: which font (tf_env.font_of) */
    uint8_t deco;                  /* IT_TEXT: 1 underline, 2 line-through, 4 italic; IT_IMAGE: 1 a link, 2 a mask (icon) */
    uint8_t radius;                /* IT_RECT: rounded corners */
    uint8_t alpha;                 /* 255 = opaque */
    int x, y, w, h;
    uint32_t color;
    uint32_t text;                 /* IT_TEXT, IT_FIELD: offset of its text in the layout's pool */
    uint16_t len;
    tf_node *node;                 /* what a click on it reaches: the element a word is in, a form field, a picture */
} tf_item;

/* what layout needs from whoever draws */
typedef struct tf_env {
    int (*font_of)(int px, int bold);                       /* a font for this size and weight */
    int (*text_w)(int font, const char *s, int len);
    int (*font_h)(int font);                                /* its height (ascent + descent) */
    int (*font_ascent)(int font);
    /* a picture's size, if it is known (0 = not yet, or nothing to show) */
    int (*image_size)(tf_node *img, int *w, int *h);
} tf_env;

typedef struct tf_layout {
    tf_item *items;
    int n, cap;
    char *pool;
    uint32_t pool_len, pool_cap;
    int doc_w, doc_h;
    uint32_t page_bg;
} tf_layout;

void layout_run(tf_layout *L, tf_doc *d, int width, int height, const tf_env *env);
void layout_clear(tf_layout *L);
/* the element under (x, y) in document coordinates: the innermost one with text or a picture or a field there,
 * or else the innermost block box there (0 = none) */
tf_node *layout_hit(tf_layout *L, tf_doc *d, int x, int y);

#endif
