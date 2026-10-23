#include "layout.h"
#include "anim.h"
#include "tfire_libc.h"

/*
 * Layout (see layout.h). Everything is laid out at document coordinates straight into the display list, except
 * what has to be measured before it can be placed (inline-blocks, flex and grid items, absolutely positioned
 * boxes): those are laid out into a list of their own at (0, 0) and copied in where they go.
 *
 * Floats live in the formatting context that holds them (a list of rectangles); line boxes take their left and
 * right edges from it, boxes that are formatting contexts of their own keep clear of it.
 */

typedef struct { int x, y, w, h; uint8_t right; } flt;

/* an absolutely positioned box waiting for its containing block to be done */
typedef struct { tf_node *n, *cb; int sx, sy, avail; } absbox;

typedef struct {
    tf_layout *L;
    const tf_env *env;
    tf_doc *d;
    int hidden;                    /* inside visibility:hidden: laid out but not drawn */
    int vw, vh;
    flt *fl;                       /* the floats of the formatting contexts open now */
    int nfl, flcap;
    int bfc;                       /* where the innermost formatting context's floats start in fl */
    absbox *ab;
    int nab, abcap;
} ctx;

static tf_style default_style = {
    .display = TD_BLOCK, .position = TP_STATIC, .white_space = TWS_NORMAL, .text_align = TA_LEFT, .list_style = TL_DISC,
    .visible = 1, .flex_row = 1, .font_px = 16, .line_px = 21,
    .width = { 0, 0, 1 }, .height = { 0, 0, 1 }, .max_width = { 0, 0, 1 }, .min_width = { 0, 0, 1 },
    .offs = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } },
    .min_height = { 0, 0, 1 }, .max_height = { 0, 0, 1 }, .flex_basis = { 0, 0, 1 },
    .flex_shrink = 100, .align_items = TAL_STRETCH, .opacity = 255,
};

static tf_style *sty(tf_node *n)
{
    while (n && (n->type != TF_ELEM || !n->style)) n = n->parent;
    return n ? n->style : 0;
}

static tf_style *style_of(tf_node *n)
{
    tf_style *s = sty(n);
    return s ? s : &default_style;
}

static int len_of(tf_len l, int base, int dflt)
{
    if (l.autov) return dflt;
    if (l.pct) return (int)((int64_t)base * l.v / 10000);
    return l.v;
}

/* ------------------------------------------------------------------ the display list */

static int add_item(ctx *c, int kind, int x, int y, int w, int h, uint32_t color)
{
    tf_layout *L = c->L;
    if (c->hidden) return -1;
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 1024;
        tf_item *ni = realloc(L->items, (size_t)nc * sizeof(tf_item));
        if (!ni) return -1;
        L->items = ni;
        L->cap = nc;
    }
    tf_item *it = &L->items[L->n];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->color = color;
    it->alpha = 255;
    it->ow = (uint16_t)(w > 0 && w < 65535 ? w : 0);
    it->oh = (uint16_t)(h > 0 && h < 65535 ? h : 0);
    if (y + h > L->doc_h) L->doc_h = y + h;
    return L->n++;
}

static uint32_t pool_add(tf_layout *L, const char *s, int n)
{
    if (L->pool_len + (uint32_t)n + 1 > L->pool_cap) {
        uint32_t nc = L->pool_cap ? L->pool_cap * 2 : 16384;
        while (nc < L->pool_len + (uint32_t)n + 1) nc *= 2;
        char *np = realloc(L->pool, nc);
        if (!np) return 0;
        L->pool = np;
        L->pool_cap = nc;
    }
    uint32_t off = L->pool_len;
    memcpy(L->pool + off, s, (size_t)n);
    L->pool[off + (uint32_t)n] = 0;
    L->pool_len += (uint32_t)n + 1;
    return off;
}

void layout_clear(tf_layout *L)
{
    free(L->items);
    free(L->pool);
    memset(L, 0, sizeof(*L));
}

static tf_node *link_of(tf_node *n)
{
    for (; n; n = n->parent)
        if (n->type == TF_ELEM && (dom_is(n, "a") || dom_is(n, "area")) && dom_attr(n, "href")) return n;
    return 0;
}

/* the boxes of a subtree laid out somewhere else moved by (dx, dy) */
static void shift_nodes(tf_node *top, int dx, int dy)
{
    if (!dx && !dy) return;
    for (tf_node *n = top; n; n = dom_next(n, top)) if (n->laid) { n->lx += dx; n->ly += dy; }
}

/* a list laid out at (0, 0) copied in at (x, y) */
static void box_place(ctx *c, tf_layout *b, int x, int y)
{
    uint32_t shift = c->L->pool_len;
    if (b->pool_len) pool_add(c->L, b->pool, (int)b->pool_len - 1);
    int was = c->hidden;
    c->hidden = 0;
    for (int j = 0; j < b->n; j++) {
        tf_item *src = &b->items[j];
        int k = add_item(c, src->kind, src->x + x, src->y + y, src->w, src->h, src->color);
        if (k < 0) continue;
        tf_item *it = &c->L->items[k];
        it->font = src->font; it->deco = src->deco; it->len = src->len; it->node = src->node;
        it->radius = src->radius; it->alpha = src->alpha; it->px = src->px; it->rot = src->rot; it->ow = src->ow; it->oh = src->oh;
        it->text = src->kind == IT_SHADOW ? src->text : src->text + shift;   /* (a shadow keeps its box there, not text) */
    }
    c->hidden = was;
    if (b->doc_h + y > c->L->doc_h) c->L->doc_h = b->doc_h + y;
}

/* ------------------------------------------------------------------ floats */

static int is_blocky(const tf_style *s)
{
    return s->display == TD_BLOCK || s->display == TD_LIST_ITEM || s->display == TD_TABLE || s->display == TD_FLEX ||
           s->display == TD_GRID || s->display == TD_ROW || s->display == TD_ROW_GROUP || s->display == TD_CAPTION ||
           s->display == TD_CELL;
}

static int out_of_flow(const tf_style *s) { return s->position == TP_ABSOLUTE || s->position == TP_FIXED; }
static int has_xf(const tf_style *s) { return s->nxf || s->ind[0].kind || s->ind[1].kind || s->ind[2].kind; }

/* a box that is a formatting context of its own: floats stay inside it, and it keeps clear of those outside */
static int new_bfc(const tf_style *s)
{
    return s->floated || s->clip || s->display == TD_TABLE || s->display == TD_FLEX || s->display == TD_GRID ||
           s->display == TD_INLINE_BLOCK || s->display == TD_INLINE_FLEX || s->display == TD_CELL || out_of_flow(s);
}

/* the part of [x0, x0 + w) that floats leave free between y and y + h */
static void float_band(ctx *c, int y, int h, int x0, int w, int *lx, int *lw)
{
    int l = x0, r = x0 + w;
    if (h < 1) h = 1;
    for (int i = c->bfc; i < c->nfl; i++) {
        flt *f = &c->fl[i];
        if (f->y >= y + h || f->y + f->h <= y) continue;
        if (f->x + f->w <= x0 || f->x >= x0 + w) continue;
        if (!f->right) { if (f->x + f->w > l) l = f->x + f->w; }
        else if (f->x < r) r = f->x;
    }
    *lx = l;
    *lw = r > l ? r - l : 0;
}

/* the nearest bottom of a float that is beside y (to move below when there is no room); -1 = none */
static int float_next_bottom(ctx *c, int y, int x0, int w)
{
    int best = -1;
    for (int i = c->bfc; i < c->nfl; i++) {
        flt *f = &c->fl[i];
        if (f->y + f->h <= y || f->y > y) continue;
        if (f->x + f->w <= x0 || f->x >= x0 + w) continue;
        if (best < 0 || f->y + f->h < best) best = f->y + f->h;
    }
    return best;
}

static int float_clear(ctx *c, int y, int which)
{
    for (int i = c->bfc; i < c->nfl; i++) {
        flt *f = &c->fl[i];
        if ((which & 1) && !f->right && f->y + f->h > y) y = f->y + f->h;
        if ((which & 2) && f->right && f->y + f->h > y) y = f->y + f->h;
    }
    return y;
}

static int floats_bottom(ctx *c, int from)
{
    int b = 0;
    for (int i = from; i < c->nfl; i++) if (c->fl[i].y + c->fl[i].h > b) b = c->fl[i].y + c->fl[i].h;
    return b;
}

static void float_add(ctx *c, int x, int y, int w, int h, int right)
{
    if (c->nfl == c->flcap) {
        int nc = c->flcap ? c->flcap * 2 : 32;
        flt *nf = realloc(c->fl, (size_t)nc * sizeof(flt));
        if (!nf) return;
        c->fl = nf;
        c->flcap = nc;
    }
    c->fl[c->nfl++] = (flt){ x, y, w, h, (uint8_t)right };
}

/* ------------------------------------------------------------------ intrinsic widths (shrink-to-fit) */

typedef struct { int line, max, min, run; } intr;   /* run: the unbreakable (nowrap) text going on now */

static int field_size(ctx *c, tf_node *n, int *w, int *h, char *label, int cap);
static int is_ws_text(tf_node *t);

static void svg_size(tf_node *k, int avail, int *w, int *h);

static void intrinsic_walk(ctx *c, tf_node *n, intr *r, int depth)
{
    if (depth > 60) return;
    for (tf_node *k = n->first; k; k = k->next) {
        if (k->type == TF_TEXT) {
            tf_style *s = style_of(n);
            int f = c->env->font_of(s->font_px, s->bold);
            const char *t = k->text;
            int i = 0, len = k->text_len, sp = c->env->text_w(f, " ", 1);
            int nowrap = s->white_space == TWS_NOWRAP || s->white_space == TWS_PRE;
            while (i < len) {
                while (i < len && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r')) {
                    if (t[i] == '\n' && (s->white_space == TWS_PRE || s->white_space == TWS_PRE_WRAP || s->white_space == TWS_PRE_LINE)) {
                        if (r->line > r->max) r->max = r->line;
                        r->line = 0;
                        r->run = 0;
                    } else {
                        if (r->line) r->line += sp;
                        if (nowrap) { if (r->run) r->run += sp; }
                        else r->run = 0;                    /* a break may go here */
                    }
                    i++;
                }
                int j = i;
                while (j < len && t[j] != ' ' && t[j] != '\n' && t[j] != '\t' && t[j] != '\r') j++;
                if (j > i) {
                    int ww = c->env->text_w(f, t + i, j - i);
                    if (ww > r->min) r->min = ww;
                    r->line += ww;
                    if (nowrap) { r->run += ww; if (r->run > r->min) r->min = r->run; }
                    else r->run = 0;
                }
                i = j;
            }
            if (r->line > r->max) r->max = r->line;
            continue;
        }
        if (k->type != TF_ELEM || !k->style) continue;
        tf_style *s = k->style;
        if (s->display == TD_NONE || out_of_flow(s)) continue;
        int extra = s->padding[1] + s->padding[3] + s->border[1] + s->border[3] +
                    len_of(s->margin[1], 0, 0) + len_of(s->margin[3], 0, 0);
        if (dom_is(k, "br")) { if (r->line > r->max) r->max = r->line; r->line = 0; continue; }
        if (dom_is(k, "svg")) {
            int w, h;
            svg_size(k, 100000, &w, &h);
            w += extra;
            if (w > r->min) r->min = w;
            r->line += w;
            if (r->line > r->max) r->max = r->line;
            continue;
        }
        if (dom_is(k, "img")) {
            int w = 0, h = 0;
            if (!s->width.autov && !s->width.pct) w = s->width.v;
            else if (c->env->image_size(k, &w, &h) != 0) { const char *a = dom_attr(k, "width"); w = a ? atoi(a) : 0; }
            w += extra;
            if (w > r->min) r->min = w;
            r->line += w;
            if (r->line > r->max) r->max = r->line;
            continue;
        }
        if (dom_is(k, "iframe")) {
            const char *aw = dom_attr(k, "width");
            int w = !s->width.autov && !s->width.pct ? s->width.v : aw && !strchr(aw, '%') ? atoi(aw) : 300;
            w += extra;
            if (w > r->min) r->min = w;
            r->line += w;
            if (r->line > r->max) r->max = r->line;
            continue;
        }
        if (dom_is(k, "input") || dom_is(k, "select") || dom_is(k, "textarea")) {
            int w, h;
            char lab[8];
            field_size(c, k, &w, &h, lab, sizeof(lab));
            w += extra;
            if (w > r->min) r->min = w;
            r->line += w;
            if (r->line > r->max) r->max = r->line;
            continue;
        }
        if (is_blocky(s) || s->display == TD_INLINE_BLOCK || s->display == TD_INLINE_FLEX) {
            intr in = { 0, 0, 0, 0 };
            if (!s->width.autov && !s->width.pct) {
                in.max = in.min = s->width.v;
                if (s->border_box) { in.max -= extra - len_of(s->margin[1], 0, 0) - len_of(s->margin[3], 0, 0); in.min = in.max; }
            } else {
                int row = (s->display == TD_FLEX || s->display == TD_INLINE_FLEX) && s->flex_dir < 2;
                if (row) {
                    /* a flex row: its items side by side */
                    for (tf_node *q = k->first; q; q = q->next) {
                        if (q->type == TF_TEXT && !is_ws_text(q)) {
                            /* text straight in the flex row: an item of its own */
                            tf_node tmp, tq = *q;
                            memset(&tmp, 0, sizeof(tmp));
                            tmp.type = TF_DOC;
                            tq.next = tq.prev = 0;
                            tq.parent = k;
                            tmp.first = &tq;
                            intr qi = { 0, 0, 0, 0 };
                            intrinsic_walk(c, &tmp, &qi, depth + 1);
                            if (qi.line > qi.max) qi.max = qi.line;
                            in.max += qi.max + s->gap;
                            in.min += qi.min;
                            continue;
                        }
                        if (q->type != TF_ELEM || !q->style || q->style->display == TD_NONE || out_of_flow(q->style)) continue;
                        tf_node tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        tmp.type = TF_DOC;
                        tmp.first = q;
                        tf_node *nx = q->next;
                        q->next = 0;
                        intr qi = { 0, 0, 0, 0 };
                        intrinsic_walk(c, &tmp, &qi, depth + 1);
                        q->next = nx;
                        if (qi.line > qi.max) qi.max = qi.line;
                        in.max += qi.max + s->gap;
                        in.min += qi.min;
                    }
                } else intrinsic_walk(c, k, &in, depth + 1);
            }
            if (!s->max_width.autov && !s->max_width.pct && in.max > s->max_width.v) in.max = s->max_width.v;
            if (in.line > in.max) in.max = in.line;
            in.max += extra;
            in.min += extra;
            if (in.min > r->min) r->min = in.min;
            if (is_blocky(s)) {
                if (r->line > r->max) r->max = r->line;
                r->line = 0;
                if (in.max > r->max) r->max = in.max;
            } else {
                r->line += in.max;
                if (r->line > r->max) r->max = r->line;
            }
            continue;
        }
        intrinsic_walk(c, k, r, depth + 1);            /* an inline element: its contents flow on */
    }
}

static void intrinsic(ctx *c, tf_node *n, int *min, int *max)
{
    intr r = { 0, 0, 0, 0 };
    tf_style *s = style_of(n);
    int extra = s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
    if (dom_is(n, "svg")) {
        int w, h;
        svg_size(n, 100000, &w, &h);
        *min = *max = w + extra;
        return;
    }
    if (!s->width.autov && !s->width.pct) {
        *min = *max = s->border_box ? s->width.v : s->width.v + extra;
        return;
    }
    intrinsic_walk(c, n, &r, 0);
    if (r.line > r.max) r.max = r.line;
    if (!s->max_width.autov && !s->max_width.pct && r.max > s->max_width.v) r.max = s->max_width.v;
    *min = r.min + extra;
    *max = r.max + extra;
}

/* ------------------------------------------------------------------ inline content: lines */

enum { P_WORD, P_ATOM, P_BOX, P_BR };

typedef struct {
    uint8_t kind, font, deco, px;
    int w, h, asc;                 /* asc: how far above the baseline it reaches */
    int space;                     /* width of the space after it (0 = none) */
    int line_px;
    uint32_t color, text;
    uint16_t len;
    tf_node *link, *node;          /* link: P_WORD, the element the text is in; P_ATOM, the link around it */
    tf_layout *box;                /* P_BOX: an inline-block laid out on its own, at (0, 0) */
    tf_node *boxnode;
    int item_kind;
    uint8_t valign;
} piece;

typedef struct {
    ctx *c;
    int x0, w, y;                  /* the column */
    int lx, lw;                    /* this line's part of it, between the floats */
    int started;
    int align, indent;
    piece *p;
    int np, cap;
    int line_w;
    int pending_space;             /* white space seen and not yet put down */
    int pending_space_w;
    int lines;
    int line_h_guess;
} lines_t;

static void line_begin(lines_t *ln)
{
    ctx *c = ln->c;
    for (int guard = 0; guard < 200; guard++) {
        float_band(c, ln->y, ln->line_h_guess, ln->x0, ln->w, &ln->lx, &ln->lw);
        if (ln->lw >= 40 || ln->lw == ln->w) break;
        int nb = float_next_bottom(c, ln->y, ln->x0, ln->w);
        if (nb < 0 || nb <= ln->y) break;
        ln->y = nb;                                     /* too narrow beside the floats: below them */
    }
    ln->started = 1;
}

static void flush_line(lines_t *ln, int forced)
{
    ctx *c = ln->c;
    if (!ln->np) {
        if (forced) { ln->y += ln->line_h_guess; ln->lines++; ln->started = 0; }
        return;
    }
    if (!ln->started) line_begin(ln);
    ln->p[ln->np - 1].space = 0;                       /* not the space after the last piece */
    int used = 0, asc = 0, desc = 0;
    for (int i = 0; i < ln->np; i++) {
        piece *p = &ln->p[i];
        used += p->w + p->space;
        int a = p->asc, d = p->h - p->asc;
        if (p->kind == P_WORD) {                       /* half-leading above and below */
            int lead = (p->line_px - p->h) / 2;
            a += lead;
            d += p->line_px - p->h - lead;
        } else if (p->valign == TV_MIDDLE) { a = p->h / 2 + 5; d = p->h - a; }
        if (a > asc) asc = a;
        if (d > desc) desc = d;
    }
    int x = ln->lx + (ln->lines == 0 ? ln->indent : 0);
    int room = ln->lw - (ln->lines == 0 ? ln->indent : 0);
    if (ln->align == TA_RIGHT && used < room) x += room - used;
    else if (ln->align == TA_CENTER && used < room) x += (room - used) / 2;
    int base = ln->y + asc;
    for (int i = 0; i < ln->np; i++) {
        piece *p = &ln->p[i];
        int top = base - p->asc;
        if (p->kind != P_WORD && p->valign == TV_MIDDLE) top = base - 5 - p->h / 2;
        if (p->kind == P_WORD) {
            int k = add_item(c, IT_TEXT, x, top, p->w + p->space, p->h, p->color);
            if (k >= 0) {
                tf_item *it = &c->L->items[k];
                it->font = p->font; it->deco = p->deco; it->text = p->text; it->len = p->len; it->node = p->link; it->px = p->px;
            }
        } else if (p->kind == P_ATOM) {
            int k = add_item(c, p->item_kind, x, top, p->w, p->h, p->color);
            if (k >= 0) {
                tf_item *it = &c->L->items[k];
                it->text = p->text; it->len = p->len;
                it->node = p->node;
                it->font = p->font;
                if (p->item_kind == IT_IMAGE) it->deco = p->link ? 1 : 0;
            }
            if (p->node) { p->node->lx = x; p->node->ly = top; p->node->lw = p->w; p->node->lh = p->h; p->node->laid = 1; }
        } else if (p->kind == P_BOX && p->box) {
            box_place(c, p->box, x, top);
            if (p->boxnode) shift_nodes(p->boxnode, x, top);
            layout_clear(p->box);
            free(p->box);
            p->box = 0;
        }
        x += p->w + p->space;
    }
    ln->y += asc + desc;
    ln->np = 0;
    ln->line_w = 0;
    ln->lines++;
    ln->started = 0;
}

static piece *new_piece(lines_t *ln)
{
    if (ln->np == ln->cap) {
        int nc = ln->cap ? ln->cap * 2 : 64;
        piece *np = realloc(ln->p, (size_t)nc * sizeof(piece));
        if (!np) return 0;
        ln->p = np;
        ln->cap = nc;
    }
    piece *p = &ln->p[ln->np];
    memset(p, 0, sizeof(*p));
    return p;
}

static int line_room(lines_t *ln)
{
    if (!ln->started) line_begin(ln);
    return ln->lw - (ln->lines == 0 ? ln->indent : 0);
}

/* puts a piece w wide on the line, starting a new line first if it does not fit */
static piece *place(lines_t *ln, int w, int can_wrap)
{
    if (ln->pending_space && ln->np) {
        ln->p[ln->np - 1].space = ln->pending_space_w;
        ln->line_w += ln->pending_space_w;
    }
    ln->pending_space = 0;
    int room = line_room(ln);
    if (can_wrap && ln->np && ln->line_w + w > room) {
        ln->line_w -= ln->p[ln->np - 1].space;
        flush_line(ln, 0);
        room = line_room(ln);
    }
    piece *p = new_piece(ln);
    if (!p) return 0;
    ln->np++;
    ln->line_w += w;
    p->w = w;
    return p;
}

static void put_word(lines_t *ln, tf_node *textnode, const tf_style *s, const char *w, int n, int can_wrap)
{
    ctx *c = ln->c;
    int f = c->env->font_of(s->font_px, s->bold);
    char up[256];
    if (s->upper && n < (int)sizeof(up)) {
        for (int i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)w[i]);
        w = up;
    }
    int ww = c->env->text_w(f, w, n);
    int room = line_room(ln);
    if (ww > room && can_wrap && n > 1 && room > 20) {
        /* wider than the whole line (a long address): broken wherever the line is full */
        int a = 0;
        while (a < n) {
            int b = a + 1;
            while (b < n && ((unsigned char)w[b] & 0xC0) == 0x80) b++;
            for (;;) {
                int nb = b;
                if (nb >= n) break;
                nb++;
                while (nb < n && ((unsigned char)w[nb] & 0xC0) == 0x80) nb++;
                if (c->env->text_w(f, w + a, nb - a) > room - ln->line_w && b > a + 1) break;
                b = nb;
            }
            put_word(ln, textnode, s, w + a, b - a, b - a < n);   /* (a piece as long as the word: not broken again) */
            a = b;
        }
        return;
    }
    piece *p = place(ln, ww, can_wrap);
    if (!p) return;
    p->kind = P_WORD;
    p->font = (uint8_t)f;
    p->h = c->env->font_h(f);
    p->asc = c->env->font_ascent(f);
    p->line_px = s->line_px > p->h ? s->line_px : p->h;
    if (s->valign == TV_SUPER) p->asc += p->h / 3;
    else if (s->valign == TV_SUB) p->asc -= p->h / 4;
    p->color = s->color;
    p->deco = (uint8_t)((s->underline ? 1 : 0) | (s->strike ? 2 : 0) | (s->italic ? 4 : 0) | (s->bold & 1 ? 8 : 0));
    p->px = (uint8_t)(s->font_px > 255 ? 255 : s->font_px);
    p->text = pool_add(c->L, w, n);
    p->len = (uint16_t)n;
    p->link = textnode->type == TF_TEXT ? textnode->parent : textnode;     /* what a click on it reaches first */
}

static void put_text(lines_t *ln, tf_node *t)
{
    tf_style *s = style_of(t->parent);
    ctx *c = ln->c;
    int f = c->env->font_of(s->font_px, s->bold);
    int spw = c->env->text_w(f, " ", 1);
    const char *p = t->text;
    int n = t->text_len, i = 0;
    int pre = s->white_space == TWS_PRE || s->white_space == TWS_PRE_WRAP;
    int wrap = s->white_space != TWS_NOWRAP && s->white_space != TWS_PRE;
    int keep_nl = pre || s->white_space == TWS_PRE_LINE;
    if (s->line_px > ln->line_h_guess) ln->line_h_guess = s->line_px;
    while (i < n) {
        char ch = p[i];
        if (ch == '\n' && keep_nl) { flush_line(ln, 1); ln->pending_space = 0; i++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
            if (pre) {                                  /* every space counts */
                int cnt = ch == '\t' ? 8 - (ln->line_w / (spw ? spw : 1)) % 8 : 1;
                if (ch == '\r') { i++; continue; }
                if (ln->np) { ln->p[ln->np - 1].space += spw * cnt; ln->line_w += spw * cnt; }
                else { piece *q = place(ln, spw * cnt, 0); if (q) { q->kind = P_ATOM; q->item_kind = IT_HLINE; q->h = 0; q->asc = 0; q->color = 0xFFFFFFFFu; } }
            } else if (ln->np || ln->pending_space) {
                ln->pending_space = 1;
                ln->pending_space_w = spw;
            }
            i++;
            continue;
        }
        int j = i;
        while (j < n && p[j] != ' ' && p[j] != '\t' && p[j] != '\n' && p[j] != '\r' && p[j] != '\f') j++;
        put_word(ln, t, s, p + i, j - i, wrap);
        i = j;
    }
}

static int layout_block(ctx *c, tf_node *n, int x, int y, int avail, int *mb_out);
static void place_float(ctx *c, tf_node *k, int y, int x0, int w);
static void defer_abs(ctx *c, tf_node *k, int sx, int sy, int avail);

/* form fields: their size, and the text shown in them */
static int field_size(ctx *c, tf_node *n, int *w, int *h, char *label, int cap)
{
    tf_style *s = style_of(n);
    int f = c->env->font_of(s->font_px, s->bold);
    int fh = c->env->font_h(f);
    const char *type = dom_attr(n, "type");
    label[0] = 0;
    int kind = 0;                                       /* 0 text field, 1 button, 2 check box, 3 select */
    if (dom_is(n, "button")) kind = 1;
    else if (dom_is(n, "select")) kind = 3;
    else if (dom_is(n, "input") && type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "image"))) kind = 1;
    else if (dom_is(n, "input") && type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) kind = 2;
    if (kind == 1) {
        const char *v = dom_attr(n, "value");
        char *tc = 0;
        if (dom_is(n, "button")) v = tc = dom_text_content(n);
        if (!v || !*v) v = dom_is(n, "button") ? "" : type && !strcasecmp(type, "reset") ? "Reset" : "Submit";
        int k = 0, sp = 1;
        for (const char *q = v; *q && k < cap - 1; q++) {
            int ws = *q == ' ' || *q == '\n' || *q == '\t' || *q == '\r';
            if (ws) { if (!sp) label[k++] = ' '; sp = 1; } else { label[k++] = *q; sp = 0; }
        }
        while (k && label[k - 1] == ' ') k--;
        label[k] = 0;
        free(tc);
        *w = c->env->text_w(f, label, k);
        *h = fh;
    } else if (kind == 2) {
        *w = *h = 13;
        if (n->checked == 1 || (n->checked == 0 && dom_attr(n, "checked"))) strcpy(label, "x");
    } else if (kind == 3) {
        int best = 40;
        tf_node *sel = 0;
        for (tf_node *o = n; o; o = dom_next(o, n))
            if (dom_is(o, "option")) {
                char *t = dom_text_content(o);
                if (t) { int tw = c->env->text_w(f, t, (int)strlen(t)); if (tw > best) best = tw; free(t); }
                if (!sel || o->checked == 1 || (o->checked == 0 && dom_attr(o, "selected"))) sel = o;
            }
        if (sel) { char *t = dom_text_content(sel); if (t) { strncpy(label, t, (size_t)cap - 1); label[cap - 1] = 0; free(t); } }
        *w = best + 18;
        *h = fh;
    } else {
        int size = 20;
        const char *sz = dom_attr(n, dom_is(n, "textarea") ? "cols" : "size");
        if (sz && atoi(sz) > 0) size = atoi(sz);
        if (size > 80) size = 80;
        *w = size * c->env->text_w(f, "n", 1);
        int rows = 1;
        if (dom_is(n, "textarea")) { const char *r = dom_attr(n, "rows"); rows = r && atoi(r) > 0 ? atoi(r) : 2; if (rows > 20) rows = 20; }
        *h = fh * rows;
        const char *v = n->value ? n->value : dom_attr(n, "value");
        if (dom_is(n, "textarea") && !n->value) { char *t = dom_text_content(n); if (t) { strncpy(label, t, (size_t)cap - 1); label[cap - 1] = 0; free(t); } }
        else if (v && *v) {
            if (type && !strcasecmp(type, "password")) { int k = 0; for (; v[k] && k < cap - 1; k++) label[k] = '*'; label[k] = 0; }
            else { strncpy(label, v, (size_t)cap - 1); label[cap - 1] = 0; }
        }
    }
    int pad = s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
    if (!s->width.autov && !s->width.pct && s->width.v > 0) *w = s->border_box ? s->width.v - pad : s->width.v;
    if (!s->height.autov && !s->height.pct && s->height.v > 0 && kind != 2) {
        int padv = s->padding[0] + s->padding[2] + s->border[0] + s->border[2];
        *h = s->border_box ? s->height.v - padv : s->height.v;
    }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
    return kind;
}

static void inline_walk(lines_t *ln, tf_node *n);

/* an <svg>'s size: CSS, else its width and height attributes, else its viewBox, else 24 x 24 (an icon) */
static void svg_size(tf_node *k, int avail, int *w, int *h)
{
    tf_style *s = k->style;
    int vw = 0, vh = 0;
    const char *vb = dom_attr(k, "viewBox");
    if (!vb) vb = dom_attr(k, "viewbox");
    if (vb) {
        int a[4] = { 0, 0, 0, 0 }, n = 0;
        const char *p = vb;
        while (*p && n < 4) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            a[n++] = atoi(p);
            while (*p && *p != ' ' && *p != ',') p++;
        }
        if (n == 4) { vw = a[2]; vh = a[3]; }
    }
    const char *aw = dom_attr(k, "width"), *ah = dom_attr(k, "height");
    int ww = aw && !strchr(aw, '%') ? atoi(aw) : 0, hh = ah && !strchr(ah, '%') ? atoi(ah) : 0;
    if (aw && strstr(aw, "em")) ww = atoi(aw) * s->font_px;
    if (ah && strstr(ah, "em")) hh = atoi(ah) * s->font_px;
    if (!s->width.autov) ww = len_of(s->width, avail, ww);
    if (!s->height.autov && !s->height.pct) hh = s->height.v;
    if (!ww && !hh) { ww = vw ? vw : 24; hh = vh ? vh : 24; }
    else if (!ww) ww = vh ? (int)((int64_t)hh * vw / vh) : hh;
    else if (!hh) hh = vw ? (int)((int64_t)ww * vh / vw) : ww;
    if (ww > avail && avail > 0) { hh = (int)((int64_t)hh * avail / ww); ww = avail; }
    *w = ww;
    *h = hh;
}

/* an inline-block (or inline-flex, or a button with markup in it) laid out by itself and placed like a word */
static void put_box(lines_t *ln, tf_node *k)
{
    ctx *c = ln->c;
    tf_style *s = k->style;
    int mh = len_of(s->margin[1], ln->w, 0) + len_of(s->margin[3], ln->w, 0);
    int mn, mx;
    intrinsic(c, k, &mn, &mx);
    int frame = s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
    int w = !s->width.autov ? len_of(s->width, ln->w, mx) + (s->border_box ? 0 : frame) : mx;
    if (w > ln->w) w = ln->w;
    if (w <= 0) return;
    tf_layout *box = calloc(1, sizeof(tf_layout));
    if (!box) return;
    tf_layout *saved = c->L;
    int sbfc = c->bfc, snfl = c->nfl;
    c->L = box;
    c->bfc = c->nfl;
    int mb = 0;
    tf_len sw = s->width;
    /* laid out at exactly the width found */
    s->width.autov = 0; s->width.pct = 0; s->width.v = s->border_box ? w : w - frame;
    int h = layout_block(c, k, 0, 0, w + mh, &mb);
    s->width = sw;
    c->L = saved;
    c->nfl = snfl;
    c->bfc = sbfc;
    if (box->n == 0 && h <= 0) { layout_clear(box); free(box); return; }
    piece *p = place(ln, w + mh, 1);
    if (!p) { layout_clear(box); free(box); return; }
    p->kind = P_BOX; p->box = box; p->boxnode = k; p->h = h + mb; p->asc = h + mb;
    p->valign = s->valign;
    /* a box with text in it sits on the line's baseline by its own last line, roughly */
    if (s->valign == TV_BASELINE && h > 0) {
        int f = c->env->font_of(s->font_px, s->bold);
        p->asc = h + mb - (s->padding[2] + s->border[2]) - c->env->font_h(f) / 4;
        if (p->asc < 0) p->asc = h + mb;
    }
}

/* an atom on the line: a picture, a form field, an inline-block */
static void put_element(lines_t *ln, tf_node *k)
{
    ctx *c = ln->c;
    tf_style *s = k->style;
    int mh = len_of(s->margin[1], ln->w, 0) + len_of(s->margin[3], ln->w, 0);
    if (dom_is(k, "svg")) {
        int w, h;
        svg_size(k, ln->w, &w, &h);
        if (w <= 0 || h <= 0) return;
        piece *p = place(ln, w + mh, 1);
        if (!p) return;
        p->kind = P_ATOM; p->item_kind = IT_IMAGE; p->h = h; p->asc = h; p->node = k; p->link = link_of(k);
        p->valign = s->valign;
        if (s->valign == TV_TEXT_BOTTOM || s->valign == TV_BASELINE) p->asc = h - h / 8;     /* (icons sit a little low) */
        return;
    }
    if (dom_is(k, "img")) {
        int w = 0, h = 0, have = c->env->image_size(k, &w, &h) == 0 && w > 0 && h > 0;
        int cw = !s->width.autov ? len_of(s->width, ln->w, 0) : 0, ch = !s->height.autov && !s->height.pct ? s->height.v : 0;
        if (!cw) { const char *a = dom_attr(k, "width"); if (a) cw = atoi(a); }
        if (!ch) { const char *a = dom_attr(k, "height"); if (a) ch = atoi(a); }
        if (cw && ch) { w = cw; h = ch; }
        else if (cw && have) { h = h * cw / (w ? w : 1); w = cw; }
        else if (ch && have) { w = w * ch / (h ? h : 1); h = ch; }
        else if (!have) {
            /* not there (yet, or a kind we can't decode): its description, if it has one */
            const char *alt = dom_attr(k, "alt");
            if (alt && *alt) {
                tf_style as = *style_of(k);
                as.color = 0x6B7280;
                char buf[160];
                int l = 0;
                for (const char *q = alt; *q && l < 150; q++) buf[l++] = *q == '\n' ? ' ' : *q;
                put_word(ln, k, &as, buf, l, 1);
            }
            return;
        }
        if (!s->max_width.autov) { int m = len_of(s->max_width, ln->w, w); if (w > m && m > 0) { h = (int)((int64_t)h * m / w); w = m; } }
        if (w > ln->w) { h = (int)((int64_t)h * ln->w / w); w = ln->w; }
        if (w <= 0 || h <= 0) return;
        piece *p = place(ln, w + mh, 1);
        if (!p) return;
        p->kind = P_ATOM; p->item_kind = IT_IMAGE; p->h = h; p->asc = h; p->node = k; p->link = link_of(k);
        p->valign = s->valign;
        return;
    }
    if (dom_is(k, "iframe")) {
        /* a page in the page: as big as it says (300 x 150 when it does not) */
        int w = !s->width.autov ? len_of(s->width, ln->w, 300) : 0, h = !s->height.autov && !s->height.pct ? s->height.v : 0;
        if (!w) { const char *a = dom_attr(k, "width"); w = a && !strchr(a, '%') ? atoi(a) : a ? ln->w * atoi(a) / 100 : 300; }
        if (!h) { const char *a = dom_attr(k, "height"); h = a && !strchr(a, '%') ? atoi(a) : 150; }
        if (s->border_box) { w -= s->border[1] + s->border[3]; h -= s->border[0] + s->border[2]; }
        if (w > ln->w) w = ln->w;
        if (w <= 0 || h <= 0) return;
        piece *p = place(ln, w + mh, 1);
        if (!p) return;
        p->kind = P_ATOM; p->item_kind = IT_FRAME; p->h = h; p->asc = h; p->node = k;
        p->valign = s->valign;
        return;
    }
    if (dom_is(k, "iframe")) {
        /* a page in the page: as big as it says (300 x 150 when it does not) */
        int w = !s->width.autov ? len_of(s->width, ln->w, 300) : 0, h = !s->height.autov && !s->height.pct ? s->height.v : 0;
        if (!w) { const char *a = dom_attr(k, "width"); w = a && !strchr(a, '%') ? atoi(a) : a ? ln->w * atoi(a) / 100 : 300; }
        if (!h) { const char *a = dom_attr(k, "height"); h = a && !strchr(a, '%') ? atoi(a) : 150; }
        if (s->border_box) { w -= s->border[1] + s->border[3]; h -= s->border[0] + s->border[2]; }
        if (w > ln->w) w = ln->w;
        if (w <= 0 || h <= 0) return;
        piece *p = place(ln, w + mh, 1);
        if (!p) return;
        p->kind = P_ATOM; p->item_kind = IT_FRAME; p->h = h; p->asc = h; p->node = k;
        p->valign = s->valign;
        return;
    }
    if (dom_is(k, "input") || dom_is(k, "select") || dom_is(k, "textarea")) {
        int w, h;
        char label[200];
        int kind = field_size(c, k, &w, &h, label, sizeof(label));
        int pad = kind == 2 ? 0 : s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
        int padv = kind == 2 ? 0 : s->padding[0] + s->padding[2] + s->border[0] + s->border[2];
        piece *p = place(ln, w + pad + mh, 1);
        if (!p) return;
        tf_style *ts = style_of(k);
        p->kind = P_ATOM; p->item_kind = IT_FIELD; p->h = h + padv; p->asc = p->h - 2; p->node = k;
        p->font = (uint8_t)c->env->font_of(ts->font_px, ts->bold);
        p->text = pool_add(c->L, label, (int)strlen(label));
        p->len = (uint16_t)strlen(label);
        p->color = (uint32_t)kind;
        p->w = w + pad;
        p->valign = s->valign;
        return;
    }
    put_box(ln, k);
}

static void inline_walk(lines_t *ln, tf_node *n)
{
    for (tf_node *k = n->first; k; k = k->next) {
        if (k->type == TF_TEXT) { put_text(ln, k); continue; }
        if (k->type != TF_ELEM || !k->style) continue;
        tf_style *s = k->style;
        if (s->display == TD_NONE) continue;
        if (out_of_flow(s)) {
            if (!ln->started) line_begin(ln);
            defer_abs(ln->c, k, ln->lx + ln->line_w, ln->y, ln->w);
            continue;
        }
        if (s->floated) {
            /* a float among the words: at the top of this line if it is empty, else below it */
            place_float(ln->c, k, ln->np ? ln->y + ln->line_h_guess : ln->y, ln->x0, ln->w);
            if (!ln->np) ln->started = 0;
            continue;
        }
        if (dom_is(k, "br")) { flush_line(ln, 1); ln->pending_space = 0; continue; }
        if (dom_is(k, "img") || dom_is(k, "svg") || dom_is(k, "iframe") || dom_is(k, "input") || dom_is(k, "button") || dom_is(k, "select") || dom_is(k, "textarea") ||
            s->display == TD_INLINE_BLOCK || s->display == TD_INLINE_FLEX) {
            int hid = !s->visible || s->opacity == 0;
            if (hid) ln->c->hidden++;
            put_element(ln, k);
            if (hid) ln->c->hidden--;
            continue;
        }
        if (is_blocky(s)) {                             /* a block inside an inline: its own lines */
            flush_line(ln, 0);
            int mb = 0;
            int h = layout_block(ln->c, k, ln->x0, ln->y, ln->w, &mb);
            ln->y += h + mb;
            ln->pending_space = 0;
            ln->started = 0;
            continue;
        }
        int hid = !s->visible;
        if (hid) ln->c->hidden++;
        /* an inline element's own padding and borders (a badge, a button-styled link): room on the line */
        int pl = s->padding[3] + s->border[3], pr = s->padding[1] + s->border[1];
        int ml = len_of(s->margin[3], 0, 0), mr = len_of(s->margin[1], 0, 0);
        int start = ln->np, y0 = ln->lines;
        if (pl + ml > 0) { piece *q = place(ln, pl + ml, 1); if (q) { q->kind = P_ATOM; q->item_kind = IT_HLINE; q->color = 0xFFFFFFFFu; } }
        inline_walk(ln, k);
        if (pr + mr > 0) { piece *q = place(ln, pr + mr, 0); if (q) { q->kind = P_ATOM; q->item_kind = IT_HLINE; q->color = 0xFFFFFFFFu; } }
        (void)start; (void)y0;
        if (hid) ln->c->hidden--;
    }
}

/* ------------------------------------------------------------------ blocks */

static int is_ws_text(tf_node *t)
{
    for (int i = 0; i < t->text_len; i++) {
        char ch = t->text[i];
        if (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r' && ch != '\f') return 0;
    }
    return 1;
}

static int child_is_inline(tf_node *k)
{
    if (k->type == TF_TEXT) return 1;
    if (k->type != TF_ELEM || !k->style) return 0;
    tf_style *s = k->style;
    if (s->display == TD_NONE) return 0;
    if (out_of_flow(s)) return 0;
    if (s->floated) return 0;
    return !is_blocky(s);
}

/* the children of a block container, from (x, y) in a column w wide; returns their height */
static int flow(ctx *c, tf_node *n, int x, int y, int w)
{
    tf_style *s = style_of(n);
    int cy = y, pending_mb = 0, first = 1;
    tf_node *k = n->first;
    while (k) {
        if (child_is_inline(k)) {
            /* a run of inline content: its lines */
            tf_node *start = k;
            int content = 0;
            while (k && (child_is_inline(k) || (k->type != TF_ELEM && k->type != TF_TEXT) ||
                         (k->type == TF_ELEM && k->style && k->style->display == TD_NONE))) {
                if (k->type == TF_ELEM || (k->type == TF_TEXT && !is_ws_text(k))) content = 1;
                k = k->next;
            }
            if (!content) continue;
            lines_t ln;
            memset(&ln, 0, sizeof(ln));
            ln.c = c; ln.x0 = x; ln.w = w; ln.y = cy + pending_mb; ln.align = s->text_align; ln.indent = s->text_indent;
            ln.line_h_guess = s->line_px;
            for (tf_node *q = start; q != k; q = q->next) {
                if (q->type == TF_TEXT) put_text(&ln, q);
                else if (q->type == TF_ELEM && q->style && q->style->display != TD_NONE) {
                    tf_node tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    tmp.type = TF_DOC;
                    tf_node *nx = q->next;
                    tmp.first = q;
                    q->next = 0;
                    inline_walk(&ln, &tmp);
                    q->next = nx;
                }
            }
            flush_line(&ln, 0);
            free(ln.p);
            if (ln.y > cy + pending_mb) { cy = ln.y; pending_mb = 0; first = 0; }
            continue;
        }
        if (k->type == TF_ELEM && k->style && k->style->display != TD_NONE) {
            tf_style *ks = k->style;
            if (out_of_flow(ks)) { defer_abs(c, k, x, cy + pending_mb, w); k = k->next; continue; }
            if (ks->floated) { place_float(c, k, cy + pending_mb, x, w); k = k->next; continue; }
            int mt = len_of(ks->margin[0], w, 0);
            int gap = (first ? mt : (pending_mb > mt ? pending_mb : mt));
            int top = cy + gap - mt;
            if (ks->clear) {
                int cl = float_clear(c, top + mt, ks->clear);
                if (cl > top + mt) { top = cl - mt; }
            }
            int bx = x, bw = w;
            if (new_bfc(ks) && c->nfl > c->bfc) {
                /* a formatting context of its own keeps clear of the floats beside it */
                int lx, lw, mn, mx;
                intrinsic(c, k, &mn, &mx);
                for (int guard = 0; guard < 100; guard++) {
                    float_band(c, top + mt, 20, x, w, &lx, &lw);
                    if (lw >= mn || lw == w) break;
                    int nb = float_next_bottom(c, top + mt, x, w);
                    if (nb < 0) break;
                    top = nb - mt;
                }
                bx = lx;
                bw = lw;
            }
            int mb = 0;
            int h = layout_block(c, k, bx, top, bw, &mb);
            cy = top + h;
            pending_mb = mb;
            first = 0;
        }
        k = k->next;
    }
    return cy - y + pending_mb;
}

/* a float: put beside what came before, as far left (or right) as there is room at y or below */
static void place_float(ctx *c, tf_node *k, int y, int x0, int w)
{
    tf_style *s = k->style;
    int mn, mx;
    intrinsic(c, k, &mn, &mx);
    int ml = len_of(s->margin[3], w, 0), mr = len_of(s->margin[1], w, 0);
    int mt = len_of(s->margin[0], w, 0), mb0 = len_of(s->margin[2], w, 0);
    int frame = s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
    int bw = !s->width.autov ? len_of(s->width, w, mx) + (s->border_box ? 0 : frame) : (mx < w - ml - mr ? mx : w - ml - mr);
    if (!s->max_width.autov) { int m = len_of(s->max_width, w, bw) + (s->border_box ? 0 : frame); if (bw > m) bw = m; }
    if (bw < 0) bw = 0;
    int tw = bw + ml + mr;
    if (s->clear) y = float_clear(c, y, s->clear);
    int lx = x0, lw = w;
    for (int guard = 0; guard < 200; guard++) {
        float_band(c, y, 1, x0, w, &lx, &lw);
        if (lw >= tw || lw == w) break;
        int nb = float_next_bottom(c, y, x0, w);
        if (nb < 0 || nb <= y) break;
        y = nb;
    }
    int fx = s->floated == 2 ? lx + lw - tw : lx;
    if (fx < x0) fx = x0;
    int sbfc = c->bfc, snfl = c->nfl;
    c->bfc = c->nfl;
    tf_len sw = s->width;
    s->width.autov = 0; s->width.pct = 0; s->width.v = s->border_box ? bw : bw - frame;
    int mb = 0;
    int floated = s->floated;
    s->floated = 0;                                     /* (laid out as the block it is, at the place found) */
    int h = layout_block(c, k, fx, y, tw, &mb);
    s->floated = (uint8_t)floated;
    s->width = sw;
    c->nfl = snfl;
    c->bfc = sbfc;
    (void)mt; (void)mb0;
    float_add(c, fx, y, tw, h + mb, floated == 2);
}

static int layout_table(ctx *c, tf_node *t, int x, int y, int w);
static int layout_flex(ctx *c, tf_node *n, int x, int y, int w);
static int layout_grid(ctx *c, tf_node *n, int x, int y, int w);
static void abs_resolve(ctx *c, tf_node *cb, int x, int y, int w, int h);

static void border_items(ctx *c, const tf_style *s, int x, int y, int w, int h)
{
    if (s->radius > 0 && s->border[0] && s->border[0] == s->border[1] && s->border[0] == s->border[2] && s->border[0] == s->border[3] &&
        s->border_color[0] != 0xFFFFFFFFu) {
        /* one rounded outline (drawn as a ring: the colour, then the inside again by the background) */
        int k = add_item(c, IT_RECT, x, y, w, h, s->border_color[0]);
        if (k >= 0) { c->L->items[k].radius = (uint8_t)(s->radius > 255 ? 255 : s->radius); c->L->items[k].deco = (uint8_t)s->border[0]; c->L->items[k].font = 1; }
        return;
    }
    if (s->border[0] && s->border_color[0] != 0xFFFFFFFFu) add_item(c, IT_RECT, x, y, w, s->border[0], s->border_color[0]);
    if (s->border[2] && s->border_color[2] != 0xFFFFFFFFu) add_item(c, IT_RECT, x, y + h - s->border[2], w, s->border[2], s->border_color[2]);
    if (s->border[3] && s->border_color[3] != 0xFFFFFFFFu) add_item(c, IT_RECT, x, y, s->border[3], h, s->border_color[3]);
    if (s->border[1] && s->border_color[1] != 0xFFFFFFFFu) add_item(c, IT_RECT, x + w - s->border[1], y, s->border[1], h, s->border_color[1]);
}

/* ------------------------------------------------------------------ transforms and opacity */

/* what a box's transform and opacity do to what was drawn for it (items from first on) and to the boxes in it:
 * moved, scaled, turned about its transform-origin; made see-through */
static void effects(ctx *c, tf_node *n, const tf_style *s, int first, int bx, int by, int bw, int bh)
{
    tf_layout *L = c->L;
    if (s->opacity < 255 && s->opacity > 0)
        for (int i = first; i < L->n; i++) L->items[i].alpha = (uint8_t)(L->items[i].alpha * s->opacity / 255);
    float m[6];
    if (!has_xf(s) || !anim_matrix(s, bw, bh, m)) return;
    float ox = (float)(bx + len_of(s->origin[0], bw, bw / 2)), oy = (float)(by + len_of(s->origin[1], bh, bh / 2));
    float sx, sy, rot;
    anim_matrix_parts(m, &sx, &sy, &rot);
    if (sx < 0) sx = -sx;
    if (sy < 0) sy = -sy;
    int r10 = (int)(rot * 10);
    for (int i = first; i < L->n; i++) {
        tf_item *it = &L->items[i];
        float cx = (float)it->x + (float)it->w / 2 - ox, cy = (float)it->y + (float)it->h / 2 - oy;
        float nx = m[0] * cx + m[2] * cy + m[4] + ox, ny = m[1] * cx + m[3] * cy + m[5] + oy;
        int w = (int)((float)it->w * sx + 0.5f), h = (int)((float)it->h * sy + 0.5f);
        if (it->radius) { int r = (int)((float)it->radius * (sx < sy ? sx : sy)); it->radius = (uint8_t)(r > 255 ? 255 : r); }
        it->x = (int)(nx - (float)w / 2 + (nx < 0 ? -0.5f : 0.5f));
        it->y = (int)(ny - (float)h / 2 + (ny < 0 ? -0.5f : 0.5f));
        it->w = w;
        it->h = h;
        int nr = it->rot + r10;
        while (nr > 1800) nr -= 3600;
        while (nr < -1800) nr += 3600;
        it->rot = (int16_t)nr;
        if (it->y + it->h > L->doc_h) L->doc_h = it->y + it->h;
    }
    /* the boxes (for clicks and scripts): where they went, roughly (the box around a turned one) */
    for (tf_node *k = n; k; k = dom_next(k, n)) {
        if (!k->laid) continue;
        float cx = (float)k->lx + (float)k->lw / 2 - ox, cy = (float)k->ly + (float)k->lh / 2 - oy;
        float nx = m[0] * cx + m[2] * cy + m[4] + ox, ny = m[1] * cx + m[3] * cy + m[5] + oy;
        int w = (int)((float)k->lw * sx), h = (int)((float)k->lh * sy);
        k->lx = (int)nx - w / 2; k->ly = (int)ny - h / 2; k->lw = w; k->lh = h;
    }
}

static int list_number(tf_node *li)
{
    int n = 1;
    tf_node *ol = li->parent;
    const char *st = ol ? dom_attr(ol, "start") : 0;
    if (st) n = atoi(st);
    for (tf_node *p = ol ? ol->first : 0; p && p != li; p = p->next)
        if (p->type == TF_ELEM && p->style && p->style->display == TD_LIST_ITEM) n++;
    return n;
}

/* a block box at (x, y) in a column avail wide: draws its background and borders, lays out what is in it.
 * Returns its height without the bottom margin, which goes to *mb_out (for collapsing with the next one). */
static int layout_block(ctx *c, tf_node *n, int x, int y, int avail, int *mb_out)
{
    tf_style *s = style_of(n);
    int hid = !s->visible;
    int mt = len_of(s->margin[0], avail, 0), mb = len_of(s->margin[2], avail, 0);
    int ml = len_of(s->margin[3], avail, 0), mr = len_of(s->margin[1], avail, 0);
    int bt = s->border[0], br = s->border[1], bb = s->border[2], bl = s->border[3];
    int pt = s->padding[0], pr = s->padding[1], pb = s->padding[2], pl = s->padding[3];
    int frame = bl + br + pl + pr;
    int w;
    int is_img = dom_is(n, "img"), is_svg = dom_is(n, "svg"), is_frame = dom_is(n, "iframe");
    int iw = 0, ih = 0;
    if (is_svg) { svg_size(n, avail, &iw, &ih); is_img = 1; }
    else if (is_frame) {
        const char *aw = dom_attr(n, "width"), *ah = dom_attr(n, "height");
        iw = !s->width.autov ? len_of(s->width, avail, 300) : aw && !strchr(aw, '%') ? atoi(aw) : aw ? avail * atoi(aw) / 100 : 300;
        ih = !s->height.autov && !s->height.pct ? s->height.v : ah && !strchr(ah, '%') ? atoi(ah) : 150;
        if (iw > avail) iw = avail;
        is_img = 1;
    }
    else if (is_img) {
        /* a picture as a block: its own size, or what the page says (keeping its proportions) */
        int have = c->env->image_size(n, &iw, &ih) == 0 && iw > 0 && ih > 0;
        const char *aw = dom_attr(n, "width"), *ah = dom_attr(n, "height");
        if (!have) { iw = aw ? atoi(aw) : 0; ih = ah ? atoi(ah) : 0; }
        if (!s->width.autov && !s->height.autov && !s->height.pct) { int ww = len_of(s->width, avail, iw); ih = s->height.v; iw = ww; }
        else if (!s->width.autov) { int ww = len_of(s->width, avail, iw); ih = iw ? (int)((int64_t)ih * ww / iw) : ih; iw = ww; }
        else if (!s->height.autov && !s->height.pct) { int hh = s->height.v; iw = ih ? (int)((int64_t)iw * hh / ih) : iw; ih = hh; }
        if (!s->max_width.autov) { int m = len_of(s->max_width, avail, iw); if (iw > m && iw) { ih = (int)((int64_t)ih * m / iw); iw = m; } }
        if (iw > avail && iw) { ih = (int)((int64_t)ih * avail / iw); iw = avail; }
    }
    if (is_img) w = iw - (s->border_box ? frame : 0);
    else if (!s->width.autov) { w = len_of(s->width, avail, 0); if (s->border_box) w -= frame; }
    else if (s->display == TD_TABLE || s->floated) {          /* shrink to fit */
        int mn, mx;
        intrinsic(c, n, &mn, &mx);
        w = mx - frame;
        if (w > avail - ml - mr - frame) w = avail - ml - mr - frame;
    } else w = avail - ml - mr - frame;
    if (!s->max_width.autov) { int mw = len_of(s->max_width, avail, w + frame) - (s->border_box ? frame : 0); if (w > mw) w = mw; }
    if (!s->min_width.autov) { int mw = len_of(s->min_width, avail, 0) - (s->border_box ? frame : 0); if (w < mw) w = mw; }
    if (w < 0) w = 0;
    /* the "visually hidden" pattern (a 1 px box that hides what overflows): not drawn */
    if (s->clip && (w <= 2 || (!s->height.autov && !s->height.pct && s->height.v <= 2))) hid = 1;
    if (s->opacity == 0) hid = 1;                       /* (fully transparent: there, but not seen) */
    if (hid) c->hidden++;
    /* auto side margins centre a box narrower than its column */
    int rest = avail - w - frame;
    if (s->margin[3].autov && s->margin[1].autov && rest > 0) ml = rest / 2;
    else if (s->margin[3].autov && rest > 0) ml = rest - mr;
    int bx = x + ml, by = y + mt;
    int first_item = c->L->n;
    int bg = -1, shadow = -1;
    if (s->shadow_color >> 24) {
        /* a box-shadow under it (its height when the box's is known, below) */
        int sp = s->shadow_spread;
        shadow = add_item(c, IT_SHADOW, bx + s->shadow_x - sp, by + s->shadow_y - sp, w + frame + 2 * sp, 0, s->shadow_color & 0xFFFFFF);
        if (shadow >= 0) {
            tf_item *it = &c->L->items[shadow];
            it->alpha = (uint8_t)(s->shadow_color >> 24);
            it->deco = (uint8_t)(s->shadow_blur > 255 ? 255 : s->shadow_blur < 0 ? 0 : s->shadow_blur);
            it->radius = (uint8_t)(s->radius > 255 ? 255 : s->radius);
            /* where the box itself is, from the shadow's corner: no shadow is drawn under it */
            it->text = (uint32_t)(uint16_t)(int16_t)(sp - s->shadow_x) | (uint32_t)(uint16_t)(int16_t)(sp - s->shadow_y) << 16;
            it->len = (uint16_t)(sp < 0 ? 0 : sp);
        }
    }
    if (s->ngrad >= 2 && !s->mask) {
        tf_grad g;
        memset(&g, 0, sizeof(g));
        g.angle = s->grad_angle;
        g.n = s->ngrad;
        for (int i = 0; i < s->ngrad && i < 4; i++) { g.pos[i] = s->grad_p[i]; g.color[i] = s->grad_c[i]; }
        bg = add_item(c, IT_GRAD, bx, by, w + frame, 0, s->grad_c[0] & 0xFFFFFF);
        if (bg >= 0) {
            tf_item *it = &c->L->items[bg];
            it->text = pool_add(c->L, (const char *)&g, (int)sizeof(g));
            it->len = (uint16_t)sizeof(g);
            it->radius = (uint8_t)(s->radius > 255 ? 255 : s->radius);
        }
    } else if ((s->bg >> 24) && s->mask) {
        /* an icon: the mask's picture, painted in the background colour */
        bg = add_item(c, IT_IMAGE, bx, by, w + frame, 0, s->bg & 0xFFFFFF);
        if (bg >= 0) { c->L->items[bg].deco = 2; c->L->items[bg].node = n; }
    } else if (s->bg >> 24) {
        bg = add_item(c, IT_RECT, bx, by, w + frame, 0, s->bg & 0xFFFFFF);
        if (bg >= 0 && s->radius) c->L->items[bg].radius = (uint8_t)(s->radius > 255 ? 255 : s->radius);
        if (bg >= 0) c->L->items[bg].alpha = (uint8_t)(s->bg >> 24);
    }
    int cx = bx + bl + pl, cy = by + bt + pt;

    if (s->display == TD_LIST_ITEM && s->list_style != TL_NONE) {
        int f = c->env->font_of(s->font_px, s->bold);
        char m[16];
        int ml2;
        if (s->list_style == TL_DECIMAL) {
            int v = list_number(n), k = 0;
            char r[12];
            if (v < 0) v = 0;
            do { r[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 10);
            ml2 = 0;
            while (k) m[ml2++] = r[--k];
            m[ml2++] = '.';
        } else {
            const char *b = s->list_style == TL_CIRCLE ? "\xE2\x97\xA6" : s->list_style == TL_SQUARE ? "\xE2\x96\xAA" : "\xE2\x80\xA2";
            ml2 = (int)strlen(b);
            memcpy(m, b, (size_t)ml2);
        }
        int mw = c->env->text_w(f, m, ml2);
        int lead = (s->line_px - c->env->font_h(f)) / 2;
        int k = add_item(c, IT_TEXT, cx - mw - 8, cy + (lead > 0 ? lead : 0), mw, c->env->font_h(f), s->color);
        if (k >= 0) { c->L->items[k].font = (uint8_t)f; c->L->items[k].text = pool_add(c->L, m, ml2); c->L->items[k].len = (uint16_t)ml2; }
    }

    int own_bfc = new_bfc(s) || n->parent == 0 || (n->parent && n->parent->type == TF_DOC);
    int sbfc = c->bfc, snfl = c->nfl;
    if (own_bfc) c->bfc = c->nfl;
    int inner;
    if (is_img) {
        inner = ih;
        if (iw > 0 && ih > 0) {
            int k = add_item(c, is_frame ? IT_FRAME : IT_IMAGE, cx, cy, w, ih, 0);
            if (k >= 0) { c->L->items[k].node = n; c->L->items[k].deco = !is_frame && link_of(n) ? 1 : 0; }
        }
    } else if (s->display == TD_TABLE) inner = layout_table(c, n, cx, cy, w);
    else if (s->display == TD_FLEX || s->display == TD_INLINE_FLEX) inner = layout_flex(c, n, cx, cy, w);
    else if (s->display == TD_GRID && s->grid_cols) inner = layout_grid(c, n, cx, cy, w);
    else inner = flow(c, n, cx, cy, w);
    if (own_bfc) {
        int fb = floats_bottom(c, c->bfc);               /* a formatting context holds its floats */
        if (fb - cy > inner) inner = fb - cy;
        c->nfl = snfl;
        c->bfc = sbfc;
    }
    if (!s->height.autov && !s->height.pct) {
        int hv = s->height.v - (s->border_box ? bt + bb + pt + pb : 0);
        if (hv >= 0) inner = hv;
    }
    if (!s->min_height.autov && !s->min_height.pct) { int m = s->min_height.v - (s->border_box ? bt + bb + pt + pb : 0); if (inner < m) inner = m; }
    int h = bt + pt + inner + pb + bb;
    if (bg >= 0) c->L->items[bg].h = h;
    if (shadow >= 0) c->L->items[shadow].h = h + 2 * s->shadow_spread;
    border_items(c, s, bx, by, w + frame, h);
    n->lx = bx; n->ly = by; n->lw = w + frame; n->lh = h; n->laid = 1;
    if (bx + w + frame > c->L->doc_w) c->L->doc_w = bx + w + frame;
    /* the absolutely positioned boxes this one is the containing block of */
    if (s->position != TP_STATIC || has_xf(s)) abs_resolve(c, n, bx + bl, by + bt, w + pl + pr, h - bt - bb);
    /* relative: moved where its offsets say, after its place was taken */
    if (s->position == TP_RELATIVE) {
        int dx = !s->offs[3].autov ? len_of(s->offs[3], avail, 0) : !s->offs[1].autov ? -len_of(s->offs[1], avail, 0) : 0;
        int dy = !s->offs[0].autov ? len_of(s->offs[0], avail, 0) : !s->offs[2].autov ? -len_of(s->offs[2], avail, 0) : 0;
        if (dx || dy) {
            for (int i = first_item; i < c->L->n; i++) { c->L->items[i].x += dx; c->L->items[i].y += dy; }
            shift_nodes(n, dx, dy);
        }
    }
    effects(c, n, s, first_item, n->lx, n->ly, n->lw, n->lh);
    if (hid) c->hidden--;
    *mb_out = mb;
    return mt + h;
}

/* ------------------------------------------------------------------ absolute and fixed boxes */

static tf_node *containing_block(tf_node *n, int fixed)
{
    for (tf_node *a = n->parent; a && a->type == TF_ELEM; a = a->parent)
        if (a->style && ((!fixed && a->style->position != TP_STATIC) || has_xf(a->style))) return a;   /* (a transform holds fixed ones too) */
    return 0;
}

static void defer_abs(ctx *c, tf_node *k, int sx, int sy, int avail)
{
    if (c->nab == c->abcap) {
        int nc = c->abcap ? c->abcap * 2 : 32;
        absbox *na = realloc(c->ab, (size_t)nc * sizeof(absbox));
        if (!na) return;
        c->ab = na;
        c->abcap = nc;
    }
    absbox *a = &c->ab[c->nab++];
    a->n = k;
    a->cb = containing_block(k, k->style->position == TP_FIXED);
    a->sx = sx; a->sy = sy; a->avail = avail;
}

static void abs_layout(ctx *c, absbox *a, int cbx, int cby, int cbw, int cbh)
{
    tf_node *k = a->n;
    tf_style *s = k->style;
    int frame = s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
    int ml = len_of(s->margin[3], cbw, 0), mr = len_of(s->margin[1], cbw, 0);
    int L0 = !s->offs[3].autov, R0 = !s->offs[1].autov, T0 = !s->offs[0].autov, B0 = !s->offs[2].autov;
    int left = len_of(s->offs[3], cbw, 0), right = len_of(s->offs[1], cbw, 0);
    int top = len_of(s->offs[0], cbh, 0), bottom = len_of(s->offs[2], cbh, 0);
    int bw;
    if (!s->width.autov) bw = len_of(s->width, cbw, 0) + (s->border_box ? 0 : frame);
    else if (L0 && R0) bw = cbw - left - right - ml - mr;
    else {
        int mn, mx;
        intrinsic(c, k, &mn, &mx);
        int room = cbw - (L0 ? left : 0) - (R0 ? right : 0) - ml - mr;
        bw = mx < room ? mx : room;
        if (bw < mn && mn < cbw) bw = mn;
    }
    if (!s->max_width.autov) { int m = len_of(s->max_width, cbw, bw) + (s->border_box ? 0 : frame); if (bw > m) bw = m; }
    if (bw < 0) bw = 0;
    tf_layout *box = calloc(1, sizeof(tf_layout));
    if (!box) return;
    tf_layout *saved = c->L;
    int sbfc = c->bfc, snfl = c->nfl;
    c->L = box;
    c->bfc = c->nfl;
    tf_len sw = s->width;
    s->width.autov = 0; s->width.pct = 0; s->width.v = s->border_box ? bw : bw - frame;
    uint8_t pos = s->position;
    s->position = TP_RELATIVE;                          /* (still the containing block of what is inside it) */
    tf_len so[4];
    memcpy(so, s->offs, sizeof(so));
    for (int i = 0; i < 4; i++) s->offs[i].autov = 1;
    int mb = 0;
    int h = layout_block(c, k, 0, 0, bw + ml + mr, &mb);
    memcpy(s->offs, so, sizeof(so));
    s->position = pos;
    s->width = sw;
    c->L = saved;
    c->nfl = snfl;
    c->bfc = sbfc;
    int x = L0 ? cbx + left : R0 ? cbx + cbw - right - bw - ml - mr : a->sx;
    int y = T0 ? cby + top : B0 ? cby + cbh - bottom - h - mb : a->sy;
    if (!s->height.autov && !s->height.pct && T0 && B0) { /* (both: the height given wins) */ }
    box_place(c, box, x, y);
    shift_nodes(k, x, y);
    layout_clear(box);
    free(box);
}

static void abs_resolve(ctx *c, tf_node *cb, int x, int y, int w, int h)
{
    for (int i = 0; i < c->nab; i++) {
        if (c->ab[i].cb != cb || !c->ab[i].n) continue;
        absbox a = c->ab[i];
        c->ab[i].n = 0;                                 /* (taken: laying it out may add more) */
        abs_layout(c, &a, x, y, w, h);
    }
}

/* ------------------------------------------------------------------ tables */

#define MAXCOL 64

static int layout_table(ctx *c, tf_node *t, int x, int y, int w)
{
    /* the rows, in order: directly in the table or in its row groups */
    tf_node *rows[1024];
    int nr = 0;
    int cy = y;
    for (tf_node *k = t->first; k; k = k->next) {
        if (k->type != TF_ELEM || !k->style || k->style->display == TD_NONE) continue;
        if (k->style->display == TD_ROW && nr < 1024) rows[nr++] = k;
        else if (k->style->display == TD_ROW_GROUP) {
            for (tf_node *r = k->first; r; r = r->next)
                if (r->type == TF_ELEM && r->style && r->style->display == TD_ROW && nr < 1024) rows[nr++] = r;
        } else if (k->style->display == TD_CAPTION) {
            int mb = 0;
            cy += layout_block(c, k, x, cy, w, &mb) + mb;
        }
    }
    int cmin[MAXCOL] = { 0 }, cmax[MAXCOL] = { 0 }, cfix[MAXCOL] = { 0 }, ncol = 0;
    for (int r = 0; r < nr; r++) {
        int col = 0;
        for (tf_node *cell = rows[r]->first; cell; cell = cell->next) {
            if (cell->type != TF_ELEM || !cell->style || cell->style->display == TD_NONE) continue;
            const char *cs = dom_attr(cell, "colspan");
            int span = cs && atoi(cs) > 1 ? atoi(cs) : 1;
            if (col >= MAXCOL) break;
            if (span == 1) {
                int mn, mx;
                intrinsic(c, cell, &mn, &mx);
                if (mn > cmin[col]) cmin[col] = mn;
                if (mx > cmax[col]) cmax[col] = mx;
                if (!cell->style->width.autov && !cell->style->width.pct && cell->style->width.v > cfix[col]) cfix[col] = cell->style->width.v;
            }
            col += span;
        }
        if (col > ncol) ncol = col > MAXCOL ? MAXCOL : col;
    }
    if (!ncol) return cy - y;
    tf_style *ts = style_of(t);
    int spacing = ts->bspacing >= 0 ? ts->bspacing : 2;
    if (dom_attr(t, "cellspacing")) spacing = atoi(dom_attr(t, "cellspacing"));
    if (ts->collapse) spacing = 0;                        /* (neighbouring borders are drawn over each other, below) */
    int avail = w - spacing * (ncol + 1);
    int smin = 0, smax = 0, cw[MAXCOL];
    for (int i = 0; i < ncol; i++) {
        if (cfix[i] > cmin[i]) cmin[i] = cfix[i];
        if (cmax[i] < cmin[i]) cmax[i] = cmin[i];
        smin += cmin[i]; smax += cmax[i];
    }
    int fill = !ts->width.autov;                          /* a table with a width uses all of it */
    for (int i = 0; i < ncol; i++) {
        if (smax <= avail && !fill) cw[i] = cmax[i];
        else if (smax <= avail) cw[i] = smax ? cmax[i] + (avail - smax) * cmax[i] / smax : avail / ncol;
        else if (smin >= avail) cw[i] = cmin[i];           /* too wide: as narrow as each can be (the table sticks out) */
        else cw[i] = cmin[i] + (smax - smin ? (int)((int64_t)(avail - smin) * (cmax[i] - cmin[i]) / (smax - smin)) : 0);
        if (cw[i] < 0) cw[i] = 0;
    }
    cy += spacing;
    for (int r = 0; r < nr; r++) {
        tf_style *rs = style_of(rows[r]);
        int rowbg = (rs->bg >> 24) ? add_item(c, IT_RECT, x, cy, w, 0, rs->bg & 0xFFFFFF) : -1;
        int col = 0, cx = x + spacing, rowh = 0;
        int cells_bg[MAXCOL], cells_n = 0;
        int cell_start[MAXCOL], cell_end[MAXCOL], cell_h[MAXCOL];
        uint8_t cell_va[MAXCOL];
        tf_node *cell_node[MAXCOL];
        for (tf_node *cell = rows[r]->first; cell; cell = cell->next) {
            if (cell->type != TF_ELEM || !cell->style || cell->style->display == TD_NONE) continue;
            if (col >= ncol) break;
            const char *cs = dom_attr(cell, "colspan");
            int span = cs && atoi(cs) > 1 ? atoi(cs) : 1;
            int ww = 0;
            for (int i = col; i < col + span && i < ncol; i++) ww += cw[i] + (i > col ? spacing : 0);
            int mb = 0;
            int before = c->L->n;
            tf_len sw = cell->style->width;
            cell->style->width.autov = 1;
            int h = layout_block(c, cell, cx, cy, ww, &mb);
            cell->style->width = sw;
            if (cells_n < MAXCOL) {
                cells_bg[cells_n] = (cell->style->bg >> 24) && c->L->n > before ? before : -1;
                cell_start[cells_n] = before;
                cell_end[cells_n] = c->L->n;
                cell_h[cells_n] = h + mb;
                cell_va[cells_n] = cell->style->valign;
                cell_node[cells_n] = cell;
                cells_n++;
            }
            if (h + mb > rowh) rowh = h + mb;
            cx += ww + spacing;
            if (ts->collapse) cx -= cell->style->border[1];   /* its right border is the next one's left */
            col += span;
        }
        for (int i = 0; i < cells_n; i++) {
            tf_style *cs = cell_node[i]->style;
            if (cells_bg[i] >= 0) c->L->items[cells_bg[i]].h = rowh;
            /* the cell's borders were drawn as tall as its content: they are the last items it made (top, bottom,
             * left, right, those it has); made as tall as the row */
            int nb = 0, which[4], k = 0;
            for (int q = 0; q < 4; q++) which[q] = 0;
            if (cs->radius > 0 && cs->border[0] && cs->border[0] == cs->border[1] && cs->border[0] == cs->border[2] && cs->border[0] == cs->border[3] &&
                cs->border_color[0] != 0xFFFFFFFFu) nb = -1;
            else {
                static const int order[4] = { 0, 2, 3, 1 };      /* (border_items: top, bottom, left, right) */
                for (int q = 0; q < 4; q++) if (cs->border[order[q]] && cs->border_color[order[q]] != 0xFFFFFFFFu) { which[nb++] = order[q]; }
            }
            int first_border = cell_end[i] - (nb < 0 ? 1 : nb);
            if (first_border < cell_start[i]) first_border = cell_end[i];
            int celly = cell_node[i]->ly;
            if (nb < 0 && first_border < cell_end[i]) c->L->items[first_border].h = rowh;
            for (int q = 0; q < nb && first_border + q < cell_end[i]; q++) {
                tf_item *it = &c->L->items[first_border + q];
                if (which[q] == 2) it->y = celly + rowh - cs->border[2];
                else if (which[q] == 1 || which[q] == 3) it->h = rowh;
            }
            cell_node[i]->lh = rowh;
            (void)k;
            /* vertical-align: middle and bottom move what is in it */
            int off = cell_va[i] == TV_MIDDLE ? (rowh - cell_h[i]) / 2 : cell_va[i] == TV_BOTTOM ? rowh - cell_h[i] : 0;
            for (int j = cell_start[i]; j < first_border && off > 0; j++) if (j != cells_bg[i]) c->L->items[j].y += off;
        }
        if (rowbg >= 0) c->L->items[rowbg].h = rowh;
        cy += rowh + spacing;
        if (ts->collapse && cells_n) cy -= cell_node[0]->style->border[2];   /* its bottom border is the next row's top */
    }
    return cy - y;
}

/* ------------------------------------------------------------------ flex */

typedef struct { tf_node *n; int basis, minw, size, h, mb, grow, shrink, order; tf_layout *box; } fitem;

/* an item laid out by itself, w wide (0 = as wide as its content wants, up to max) */
static tf_layout *measure(ctx *c, tf_node *k, int w, int *h, int *mb)
{
    tf_layout *box = calloc(1, sizeof(tf_layout));
    if (!box) return 0;
    tf_layout *saved = c->L;
    int sbfc = c->bfc, snfl = c->nfl;
    c->L = box;
    c->bfc = c->nfl;
    *mb = 0;
    *h = layout_block(c, k, 0, 0, w, mb);
    c->L = saved;
    c->nfl = snfl;
    c->bfc = sbfc;
    return box;
}

static int fitem_cmp_order(const fitem *a, const fitem *b) { return a->order - b->order; }

static int layout_flex(ctx *c, tf_node *n, int x, int y, int w)
{
    tf_style *s = style_of(n);
    int ni = 0, cap = 0;
    fitem *it = 0;
    /* text straight in a flex container: an anonymous block item holding it (a copy of the text node, alone) */
    tf_node anon[8], anon_text[8];
    tf_style anon_style[8];
    int nanon = 0;
    for (tf_node *k = n->first; k; k = k->next) {
        if (k->type == TF_TEXT && !is_ws_text(k) && nanon < 8) {
            tf_node *a = &anon[nanon], *t = &anon_text[nanon];
            tf_style *as = &anon_style[nanon];
            nanon++;
            *t = *k;
            t->next = t->prev = 0;
            t->parent = n;                      /* (its words take the container's font and colour) */
            memset(a, 0, sizeof(*a));
            a->type = TF_ELEM;
            a->name = n->name;
            a->parent = n;
            a->first = a->last = t;
            *as = *s;
            as->display = TD_BLOCK;
            as->bg = 0;
            memset(as->margin, 0, sizeof(as->margin));
            for (int q = 0; q < 4; q++) { as->margin[q].autov = 0; as->padding[q] = 0; as->border[q] = 0; }
            as->width.autov = as->height.autov = as->max_width.autov = as->min_width.autov = 1;
            as->min_height.autov = as->max_height.autov = as->flex_basis.autov = 1;
            as->position = TP_STATIC;
            as->floated = 0;
            as->flex_grow = 0;
            as->flex_shrink = 100;
            as->align_self = TAL_AUTO;
            as->mask = 0;
            as->radius = 0;
            a->style = as;
            if (ni == cap) { cap = cap ? cap * 2 : 16; fitem *na = realloc(it, (size_t)cap * sizeof(fitem)); if (!na) break; it = na; }
            fitem *f = &it[ni++];
            memset(f, 0, sizeof(*f));
            f->n = a;
            f->shrink = 100;
            continue;
        }
        if (k->type != TF_ELEM || !k->style || k->style->display == TD_NONE) continue;
        if (out_of_flow(k->style)) { defer_abs(c, k, x, y, w); continue; }
        if (ni == cap) { cap = cap ? cap * 2 : 16; fitem *na = realloc(it, (size_t)cap * sizeof(fitem)); if (!na) break; it = na; }
        fitem *f = &it[ni++];
        memset(f, 0, sizeof(*f));
        f->n = k;
        f->grow = k->style->flex_grow;
        f->shrink = k->style->flex_shrink;
        f->order = k->style->order;
    }
    /* order (a stable insertion sort) */
    for (int i = 1; i < ni; i++) {
        fitem t = it[i];
        int j = i - 1;
        while (j >= 0 && fitem_cmp_order(&it[j], &t) > 0) { it[j + 1] = it[j]; j--; }
        it[j + 1] = t;
    }
    int row = s->flex_dir < 2;
    if (s->flex_dir == 1 || s->flex_dir == 3)
        for (int i = 0; i < ni / 2; i++) { fitem t = it[i]; it[i] = it[ni - 1 - i]; it[ni - 1 - i] = t; }
    int gap = s->gap, rgap = s->row_gap ? s->row_gap : s->gap;
    int cy = y;
    if (!row) {
        /* a column: items stacked, each as wide as the container (stretch) or as its content */
        for (int i = 0; i < ni; i++) {
            fitem *f = &it[i];
            tf_style *ks = f->n->style;
            int al = ks->align_self != TAL_AUTO ? ks->align_self : s->align_items;
            int iw = w;
            if (al != TAL_STRETCH && ks->width.autov) {
                int mn, mx;
                intrinsic(c, f->n, &mn, &mx);
                iw = mx + len_of(ks->margin[1], w, 0) + len_of(ks->margin[3], w, 0);
                if (iw > w) iw = w;
            }
            int h, mb;
            tf_layout *box = measure(c, f->n, iw, &h, &mb);
            if (!box) continue;
            int ix = x + (al == TAL_CENTER ? (w - iw) / 2 : al == TAL_END ? w - iw : 0);
            box_place(c, box, ix, cy);
            shift_nodes(f->n, ix, cy);
            layout_clear(box);
            free(box);
            cy += h + mb + (i + 1 < ni ? rgap : 0);
        }
        free(it);
        return cy - y;
    }
    /* a row: sizes from the bases, grown or shrunk to fit, in lines if it wraps */
    for (int i = 0; i < ni; i++) {
        fitem *f = &it[i];
        tf_style *ks = f->n->style;
        int mn, mx;
        intrinsic(c, f->n, &mn, &mx);
        int mh = len_of(ks->margin[1], w, 0) + len_of(ks->margin[3], w, 0);
        int frame = ks->padding[1] + ks->padding[3] + ks->border[1] + ks->border[3];
        int b;
        if (!ks->flex_basis.autov && !(ks->flex_basis.v == 0 && !ks->flex_basis.pct && f->grow == 0))
            b = len_of(ks->flex_basis, w, mx) + (ks->border_box ? 0 : frame);
        else if (!ks->width.autov) b = len_of(ks->width, w, mx) + (ks->border_box ? 0 : frame);
        else b = mx;
        if (!ks->max_width.autov) { int m = len_of(ks->max_width, w, b) + (ks->border_box ? 0 : frame); if (b > m) b = m; }
        if (!ks->min_width.autov) { int m = len_of(ks->min_width, w, 0) + (ks->border_box ? 0 : frame); if (b < m) b = m; if (mn < m) mn = m; }
        f->basis = b + mh;
        f->minw = (ks->width.autov && ks->flex_basis.autov ? mn : (b < mn ? b : mn)) + mh;
        if (f->minw > w) f->minw = w;
    }
    int i = 0;
    while (i < ni) {
        int j = i, sum = 0;
        while (j < ni) {
            int add = it[j].basis + (j > i ? gap : 0);
            if (s->flex_wrap && j > i && sum + add > w) break;
            sum += add;
            j++;
        }
        int free_space = w - sum;
        int tg = 0, ts = 0;
        for (int k = i; k < j; k++) { tg += it[k].grow; ts += it[k].shrink * it[k].basis / 100; }
        for (int k = i; k < j; k++) {
            fitem *f = &it[k];
            f->size = f->basis;
            if (free_space > 0 && tg > 0) f->size += (int)((int64_t)free_space * f->grow / tg);
            else if (free_space < 0 && ts > 0) {
                f->size += (int)((int64_t)free_space * (f->shrink * f->basis / 100) / ts);
                if (f->size < f->minw) f->size = f->minw;
            }
            if (f->size < 0) f->size = 0;
        }
        /* each laid out at its size; the line as tall as the tallest */
        int lineh = 0, used = 0;
        for (int k = i; k < j; k++) {
            fitem *f = &it[k];
            tf_style *ks = f->n->style;
            tf_len sw = ks->width;
            int mh = len_of(ks->margin[1], w, 0) + len_of(ks->margin[3], w, 0);
            int frame = ks->padding[1] + ks->padding[3] + ks->border[1] + ks->border[3];
            ks->width.autov = 0; ks->width.pct = 0; ks->width.v = f->size - mh - (ks->border_box ? 0 : frame);
            if (ks->width.v < 0) ks->width.v = 0;
            tf_len smax = ks->max_width;
            ks->max_width.autov = 1;
            f->box = measure(c, f->n, f->size, &f->h, &f->mb);
            ks->width = sw;
            ks->max_width = smax;
            if (f->h + f->mb > lineh) lineh = f->h + f->mb;
            used += f->size + (k > i ? gap : 0);
        }
        if (i == 0 && j == ni) {
            /* one line in a container with a height of its own: the line is that tall (for centring) */
            int ch = -1;
            if (!s->height.autov && !s->height.pct) ch = s->height.v - (s->border_box ? s->padding[0] + s->padding[2] + s->border[0] + s->border[2] : 0);
            if (!s->min_height.autov && !s->min_height.pct) {
                int m = s->min_height.v - (s->border_box ? s->padding[0] + s->padding[2] + s->border[0] + s->border[2] : 0);
                if (m > ch) ch = m;
            }
            if (ch > lineh) lineh = ch;
        }
        int left = w - used, cx = x, between = 0;
        if (left > 0 && tg == 0) {
            if (s->justify == TJ_END) cx += left;
            else if (s->justify == TJ_CENTER) cx += left / 2;
            else if (s->justify == TJ_BETWEEN && j - i > 1) between = left / (j - i - 1);
            else if (s->justify == TJ_AROUND) { between = left / (j - i); cx += between / 2; }
            else if (s->justify == TJ_EVENLY) { between = left / (j - i + 1); cx += between; }
        }
        for (int k = i; k < j; k++) {
            fitem *f = &it[k];
            if (!f->box) continue;
            tf_style *ks = f->n->style;
            int al = ks->align_self != TAL_AUTO ? ks->align_self : s->align_items;
            int iy = cy;
            if (al == TAL_CENTER) iy += (lineh - f->h - f->mb) / 2;
            else if (al == TAL_END) iy += lineh - f->h - f->mb;
            else if (al == TAL_STRETCH && f->box->n && f->box->items[0].kind == IT_RECT && f->box->items[0].y <= 0 + len_of(ks->margin[0], w, 0) &&
                     f->box->items[0].h == f->h - len_of(ks->margin[0], w, 0) && ks->height.autov)
                f->box->items[0].h = lineh - f->mb - len_of(ks->margin[0], w, 0);     /* its background as tall as the line */
            box_place(c, f->box, cx, iy);
            shift_nodes(f->n, cx, iy);
            layout_clear(f->box);
            free(f->box);
            f->box = 0;
            cx += f->size + gap + between;
        }
        cy += lineh;
        i = j;
        if (i < ni) cy += rgap;
    }
    free(it);
    return cy - y;
}

/* ------------------------------------------------------------------ grid */

#define GMAX 24

typedef struct { int kind; int v; int minv; } track;    /* kind 0 px, 1 fr (v = fr x 100), 2 auto */

static const char *gskip(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static int glen(const char *p, const char *e, int font, int base)
{
    int v = 0, frac = 0, div = 1, neg = 0;
    if (p < e && *p == '-') { neg = 1; p++; }
    while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (p < e && *p == '.') { p++; while (p < e && *p >= '0' && *p <= '9') { if (div < 10000) { frac = frac * 10 + (*p - '0'); div *= 10; } p++; } }
    int r;
    if (p + 3 <= e && !strncmp(p, "rem", 3)) r = v * 16 + frac * 16 / div;
    else if (p + 2 <= e && !strncmp(p, "em", 2)) r = v * font + frac * font / div;
    else if (p < e && *p == '%') r = (int)((int64_t)base * (v * div + frac) / (100 * div));
    else if (p + 2 <= e && !strncmp(p, "vw", 2)) r = 0;
    else r = v;
    return neg ? -r : r;
}

/* one track size ("12rem", "1fr", "minmax(0,1fr)", "auto", "min-content") */
static track gtrack(const char *p, const char *e, int font, int base)
{
    track t = { 2, 0, 0 };
    while (p < e && (*p == ' ')) p++;
    while (e > p && e[-1] == ' ') e--;
    if (e - p > 7 && !strncmp(p, "minmax(", 7)) {
        const char *a = p + 7, *comma = a;
        int depth = 0;
        while (comma < e && (*comma != ',' || depth)) { if (*comma == '(') depth++; if (*comma == ')') depth--; comma++; }
        const char *b = comma + 1, *be = e;
        if (be > b && be[-1] == ')') be--;
        track lo = gtrack(a, comma, font, base), hi = gtrack(b, be, font, base);
        t = hi;
        t.minv = lo.kind == 0 ? lo.v : 0;
        return t;
    }
    if (e - p > 13 && !strncmp(p, "fit-content(", 12)) { t.kind = 2; t.minv = 0; t.v = glen(p + 12, e, font, base); return t; }
    if (e > p && e[-1] == 'r' && e - p >= 2 && e[-2] == 'f') {
        int v = 0, frac = 0, div = 1;
        const char *q = p;
        while (q < e && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
        if (q < e && *q == '.') { q++; while (q < e && *q >= '0' && *q <= '9') { if (div < 100) { frac = frac * 10 + (*q - '0'); div *= 10; } q++; } }
        t.kind = 1;
        t.v = v * 100 + frac * 100 / div;
        if (!t.v) t.v = 100;
        return t;
    }
    if ((*p >= '0' && *p <= '9') || *p == '.') { t.kind = 0; t.v = glen(p, e, font, base); return t; }
    return t;                                           /* auto, min-content, max-content */
}

/* "a b repeat(3, 1fr)" -> tracks; returns how many */
static int gtracks(const char *v, track *out, int max, int font, int base)
{
    int n = 0;
    const char *p = v;
    while (*p && n < max) {
        p = gskip(p);
        if (!*p || *p == '/' || *p == ';') break;
        if (*p == '[') { while (*p && *p != ']') p++; if (*p) p++; continue; }           /* line names */
        if (*p == '\'' || *p == '"') { char q = *p++; while (*p && *p != q) p++; if (*p) p++; continue; }
        const char *s = p;
        int depth = 0;
        while (*p && (depth || (*p != ' ' && *p != '\t' && *p != '\n' && *p != '/'))) { if (*p == '(') depth++; if (*p == ')') depth--; p++; }
        if (p - s > 7 && !strncmp(s, "repeat(", 7)) {
            const char *a = s + 7;
            int cnt = atoi(a);
            if (cnt <= 0) cnt = 1;
            if (cnt > max) cnt = max;
            const char *comma = strchr(a, ',');
            if (!comma || comma >= p) continue;
            char inner[256];
            int il = (int)(p - 1 - (comma + 1));
            if (il < 0) il = 0;
            if (il > 255) il = 255;
            memcpy(inner, comma + 1, (size_t)il);
            inner[il] = 0;
            track tmp[GMAX];
            int tn = gtracks(inner, tmp, GMAX, font, base);
            for (int r = 0; r < cnt && n < max; r++) for (int k = 0; k < tn && n < max; k++) out[n++] = tmp[k];
            continue;
        }
        out[n++] = gtrack(s, p, font, base);
    }
    return n;
}

typedef struct { char name[32]; int r0, c0, r1, c1; } garea;

/* grid-template-areas: "'a a' 'b c'" -> the named rectangles, and the grid's size */
static int gareas(const char *v, garea *out, int max, int *rows, int *cols)
{
    int n = 0, r = 0;
    *rows = *cols = 0;
    for (const char *p = v ? v : ""; *p; p++) {
        if (*p != '\'' && *p != '"') continue;
        char q = *p++;
        int col = 0;
        while (*p && *p != q) {
            while (*p == ' ') p++;
            if (*p == q || !*p) break;
            char name[32];
            int l = 0;
            while (*p && *p != ' ' && *p != q) { if (l < 31) name[l++] = *p; p++; }
            name[l] = 0;
            if (strcmp(name, ".")) {
                int k;
                for (k = 0; k < n; k++) if (!strcmp(out[k].name, name)) break;
                if (k == n && n < max) { strcpy(out[n].name, name); out[n].r0 = r; out[n].c0 = col; out[n].r1 = r + 1; out[n].c1 = col + 1; n++; }
                else if (k < n) { if (col + 1 > out[k].c1) out[k].c1 = col + 1; if (r + 1 > out[k].r1) out[k].r1 = r + 1; }
            }
            col++;
        }
        if (col > *cols) *cols = col;
        r++;
        if (!*p) break;
    }
    *rows = r;
    return n;
}

/* "2 / 4", "span 2", "3", "1 / -1" -> [start, end) lines, 0-based; -1 = auto */
static void gline(const char *v, int ncols, int *a, int *b)
{
    *a = -1; *b = -1;
    if (!v) return;
    const char *slash = strchr(v, '/');
    const char *p = gskip(v);
    int span1 = 0;
    if (!strncmp(p, "span", 4)) { span1 = atoi(p + 4); }
    else if ((*p >= '0' && *p <= '9') || *p == '-') { int x = atoi(p); *a = x < 0 ? ncols + 1 + x : x - 1; }
    if (slash) {
        const char *q = gskip(slash + 1);
        if (!strncmp(q, "span", 4)) { int sp = atoi(q + 4); if (*a >= 0) *b = *a + (sp > 0 ? sp : 1); else span1 = sp; }
        else { int x = atoi(q); *b = x < 0 ? ncols + 1 + x : x - 1; }
    }
    if (*a >= 0 && *b < 0) *b = *a + (span1 > 0 ? span1 : 1);
    if (*a < 0 && span1 > 0) *b = -1 - span1;          /* a span with no start: encoded for auto placement */
}

static int layout_grid(ctx *c, tf_node *n, int x, int y, int w)
{
    tf_style *s = style_of(n);
    track cols[GMAX];
    int nc = gtracks(s->grid_cols, cols, GMAX, s->font_px, w);
    garea areas[GMAX];
    int arows = 0, acols = 0;
    int na = gareas(s->grid_areas, areas, GMAX, &arows, &acols);
    if (acols > nc) { for (int i = nc; i < acols && i < GMAX; i++) cols[i] = (track){ 1, 100, 0 }; nc = acols < GMAX ? acols : GMAX; }
    if (nc <= 0) return flow(c, n, x, y, w);
    int gap = s->gap, rgap = s->row_gap;
    /* the items and where they go */
    typedef struct { tf_node *n; int r0, c0, r1, c1, h, mb; tf_layout *box; } gitem;
    gitem items[256];
    int ni = 0;
    uint8_t used[64][GMAX];
    memset(used, 0, sizeof(used));
    int nrows = arows;
    for (tf_node *k = n->first; k && ni < 256; k = k->next) {
        if (k->type != TF_ELEM || !k->style || k->style->display == TD_NONE) continue;
        if (out_of_flow(k->style)) { defer_abs(c, k, x, y, w); continue; }
        gitem *g = &items[ni];
        memset(g, 0, sizeof(*g));
        g->n = k;
        g->r0 = g->c0 = -1;
        tf_style *ks = k->style;
        if (ks->grid_area && !strchr(ks->grid_area, '/')) {
            char nm[32];
            int l = 0;
            for (const char *q = gskip(ks->grid_area); *q && *q != ' ' && l < 31; q++) nm[l++] = *q;
            nm[l] = 0;
            for (int a = 0; a < na; a++) if (!strcmp(areas[a].name, nm)) { g->r0 = areas[a].r0; g->c0 = areas[a].c0; g->r1 = areas[a].r1; g->c1 = areas[a].c1; }
        }
        if (g->c0 < 0) {
            int a, b;
            gline(ks->grid_col, nc, &a, &b);
            if (a >= 0) { g->c0 = a; g->c1 = b > a ? b : a + 1; }
            int ra, rb;
            gline(ks->grid_row, 64, &ra, &rb);
            if (ra >= 0) { g->r0 = ra; g->r1 = rb > ra ? rb : ra + 1; }
            int span = b < -1 ? -1 - b : 1;
            if (g->c0 < 0 || g->r0 < 0) {
                /* auto placement: the next free cells, row by row */
                int placed = 0;
                for (int r = 0; r < 64 && !placed; r++) {
                    if (g->r0 >= 0 && r != g->r0) continue;
                    for (int cc = 0; cc + (g->c0 >= 0 ? 1 : span) <= nc && !placed; cc++) {
                        if (g->c0 >= 0 && cc != g->c0) continue;
                        int c0 = g->c0 >= 0 ? g->c0 : cc, c1 = g->c0 >= 0 ? g->c1 : cc + span;
                        int ok = 1;
                        for (int q = c0; q < c1 && q < GMAX; q++) if (used[r][q]) ok = 0;
                        if (!ok) continue;
                        g->r0 = r; g->r1 = r + 1; g->c0 = c0; g->c1 = c1;
                        placed = 1;
                    }
                }
                if (!placed) { g->r0 = nrows; g->r1 = nrows + 1; g->c0 = 0; g->c1 = 1; }
            }
        }
        if (g->c1 > nc) g->c1 = nc;
        if (g->c0 >= nc) { g->c0 = nc - 1; g->c1 = nc; }
        if (g->r1 > 64) g->r1 = 64;
        if (g->r0 >= 64) continue;
        for (int r = g->r0; r < g->r1; r++) for (int q = g->c0; q < g->c1; q++) used[r][q] = 1;
        if (g->r1 > nrows) nrows = g->r1;
        ni++;
    }
    /* the columns: fixed, then auto (their items' widths), then fr (what is left) */
    int cw[GMAX], fixed = gap * (nc - 1), frs = 0;
    for (int i = 0; i < nc; i++) {
        cw[i] = 0;
        if (cols[i].kind == 0) cw[i] = cols[i].v;
        else if (cols[i].kind == 2) {
            int best = cols[i].v > 0 ? 0 : 0;
            for (int k = 0; k < ni; k++) {
                if (items[k].c0 != i || items[k].c1 != i + 1) continue;
                int mn, mx;
                intrinsic(c, items[k].n, &mn, &mx);
                tf_style *ks = items[k].n->style;
                mx += len_of(ks->margin[1], w, 0) + len_of(ks->margin[3], w, 0);
                if (cols[i].v > 0 && mx > cols[i].v) mx = cols[i].v;
                if (mx > best) best = mx;
            }
            cw[i] = best;
        } else frs += cols[i].v;
        if (cols[i].kind != 1) fixed += cw[i];
    }
    int left = w - fixed;
    if (left < 0) {
        /* too wide: the tracks that may shrink (minmax(small, big)) give way */
        int give = 0;
        for (int i = 0; i < nc; i++) if (cols[i].kind == 0 && cols[i].minv < cw[i]) give += cw[i] - cols[i].minv;
        for (int i = 0; i < nc && give > 0; i++)
            if (cols[i].kind == 0 && cols[i].minv < cw[i]) {
                int cut = (int)((int64_t)(-left) * (cw[i] - cols[i].minv) / give);
                if (cut > cw[i] - cols[i].minv) cut = cw[i] - cols[i].minv;
                cw[i] -= cut;
            }
        left = 0;
    }
    for (int i = 0; i < nc; i++)
        if (cols[i].kind == 1) {
            cw[i] = frs ? (int)((int64_t)(left > 0 ? left : 0) * cols[i].v / frs) : 0;
            if (cw[i] < cols[i].minv) cw[i] = cols[i].minv;
        }
    int cx[GMAX + 1];
    cx[0] = x;
    for (int i = 0; i < nc; i++) cx[i + 1] = cx[i] + cw[i] + gap;
    /* each item laid out in its cells; the rows as tall as what is in them */
    int rh[64];
    memset(rh, 0, sizeof(rh));
    for (int k = 0; k < ni; k++) {
        gitem *g = &items[k];
        int iw = cx[g->c1] - cx[g->c0] - gap;
        if (iw < 0) iw = 0;
        g->box = measure(c, g->n, iw, &g->h, &g->mb);
        if (g->r1 == g->r0 + 1 && g->h + g->mb > rh[g->r0]) rh[g->r0] = g->h + g->mb;
    }
    for (int k = 0; k < ni; k++) {
        gitem *g = &items[k];
        if (g->r1 - g->r0 <= 1) continue;
        int have = 0;
        for (int r = g->r0; r < g->r1; r++) have += rh[r] + (r > g->r0 ? rgap : 0);
        if (g->h + g->mb > have) rh[g->r1 - 1] += g->h + g->mb - have;
    }
    int ry[65];
    ry[0] = y;
    for (int r = 0; r < nrows; r++) ry[r + 1] = ry[r] + rh[r] + (rh[r] ? rgap : 0);
    for (int k = 0; k < ni; k++) {
        gitem *g = &items[k];
        if (!g->box) continue;
        box_place(c, g->box, cx[g->c0], ry[g->r0]);
        shift_nodes(g->n, cx[g->c0], ry[g->r0]);
        layout_clear(g->box);
        free(g->box);
    }
    int h = ry[nrows] - y;
    if (nrows && rh[nrows - 1]) h -= rgap;
    return h > 0 ? h : 0;
}

/* ------------------------------------------------------------------ the page */

void layout_run(tf_layout *L, tf_doc *d, int width, int height, const tf_env *env)
{
    free(L->items);
    free(L->pool);
    memset(L, 0, sizeof(*L));
    L->page_bg = 0xFFFFFF;
    ctx c;
    memset(&c, 0, sizeof(c));
    c.L = L; c.env = env; c.d = d; c.vw = width; c.vh = height > 0 ? height : 600;
    for (tf_node *n = d->all; n; n = n->all_next) n->laid = 0;
    tf_node *html = 0;
    for (tf_node *k = d->root->first; k; k = k->next) if (dom_is(k, "html")) html = k;
    if (!html || !html->style) return;
    /* the page's background is the root's, or else the body's (CSS 2.1, 14.2) */
    if (html->style->bg >> 24) L->page_bg = html->style->bg & 0xFFFFFF;
    else {
        tf_node *body = dom_find(html, "body");
        if (body && body->style && (body->style->bg >> 24)) {
            L->page_bg = body->style->bg & 0xFFFFFF;
            body->style->bg = 0;                          /* (it is drawn as the page, not as a box) */
        }
    }
    int mb = 0;
    int h = layout_block(&c, html, 0, 0, width, &mb);
    if (h + mb > L->doc_h) L->doc_h = h + mb;
    /* what is positioned against the window (no positioned ancestor) */
    abs_resolve(&c, 0, 0, 0, width, c.vh);
    L->doc_w = width;
    free(c.fl);
    free(c.ab);
}

static tf_node *box_hit(tf_node *n, int x, int y)
{
    tf_node *best = 0;
    for (tf_node *k = n->first; k; k = k->next) {
        if (k->type != TF_ELEM) continue;
        if (k->laid && x >= k->lx && x < k->lx + k->lw && y >= k->ly && y < k->ly + k->lh) {
            tf_node *in = box_hit(k, x, y);
            best = in ? in : k;                       /* (later siblings are drawn over earlier ones) */
        } else if (!k->laid || (k->style && k->style->display == TD_INLINE)) {
            tf_node *in = box_hit(k, x, y);           /* inline boxes have none of their own: what is in them may */
            if (in) best = in;
        }
    }
    return best;
}

tf_node *layout_hit(tf_layout *L, tf_doc *d, int x, int y)
{
    for (int i = L->n - 1; i >= 0; i--) {
        tf_item *it = &L->items[i];
        if (!it->node || it->kind == IT_RECT || it->kind == IT_SHADOW || it->kind == IT_GRAD) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y - 2 && y < it->y + it->h + 2) return it->node;
    }
    return d ? box_hit(d->root, x, y) : 0;
}
