#include "style.h"
#include "tfire_libc.h"
#include <libcss/libcss.h>
#include <hubbub/types.h>

/*
 * tFire's own style sheet: what HTML elements look like before a page says otherwise. It follows the
 * rendering section of the HTML standard, trimmed to what tFire can draw.
 */
static const char ua_css[] =
    "html, address, blockquote, body, dd, div, dl, dt, fieldset, form, frame, frameset, h1, h2, h3, h4, h5, h6,"
    " noframes, ol, p, ul, center, dir, hr, menu, pre, header, footer, nav, article, aside, section, main,"
    " figure, figcaption, hgroup, details, summary, search, legend, optgroup { display: block }\n"
    "head, script, style, link, meta, title, template, noscript, datalist, param, source, track, area, base,"
    " [hidden], dialog:not([open]), input[type=hidden] { display: none }\n"
    "body { margin: 8px; line-height: 1.35 }\n"
    "p, blockquote, figure, dl, ul, ol, menu, dir, pre { margin-top: 1em; margin-bottom: 1em }\n"
    "blockquote, figure { margin-left: 40px; margin-right: 40px }\n"
    "h1 { font-size: 2em; margin: 0.67em 0; font-weight: bold }\n"
    "h2 { font-size: 1.5em; margin: 0.83em 0; font-weight: bold }\n"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold }\n"
    "h4 { margin: 1.33em 0; font-weight: bold }\n"
    "h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold }\n"
    "h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold }\n"
    "ul, ol, menu, dir { padding-left: 40px }\n"
    "ul ul, ul ol, ol ul, ol ol, ul menu, ol menu { margin-top: 0; margin-bottom: 0 }\n"
    "li { display: list-item }\n"
    "ul { list-style-type: disc } ol { list-style-type: decimal }\n"
    "ul ul { list-style-type: circle } ul ul ul { list-style-type: square }\n"
    "dd { margin-left: 40px }\n"
    "table { display: table; border-spacing: 2px; border-collapse: separate }\n"
    "caption { display: table-caption; text-align: center }\n"
    "thead { display: table-header-group } tbody { display: table-row-group } tfoot { display: table-footer-group }\n"
    "tr { display: table-row } td, th { display: table-cell; padding: 1px } th { font-weight: bold; text-align: center }\n"
    "col { display: table-column } colgroup { display: table-column-group }\n"
    "b, strong { font-weight: bold } i, em, cite, var, dfn, address { font-style: italic }\n"
    "u, ins { text-decoration: underline } s, strike, del { text-decoration: line-through }\n"
    "big { font-size: larger } small, sub, sup { font-size: smaller }\n"
    "pre, xmp, plaintext, listing { white-space: pre } tt, code, kbd, samp, pre { font-family: monospace }\n"
    "center { text-align: center }\n"
    "a:link, a:visited { color: #1a56db; text-decoration: underline }\n"
    "hr { border: 1px inset #c0c0c0; margin: 0.5em auto }\n"
    "img, video, canvas, iframe, embed, object, svg { display: inline-block }\n"
    "input, select, textarea, button { display: inline-block; border: 1px solid #8e8e93; padding: 2px 4px;"
    " background-color: #ffffff; color: #000000 }\n"
    "button, input[type=submit], input[type=button], input[type=reset] { background-color: #e5e5ea; padding: 2px 10px }\n"
    "button { text-align: center; white-space: nowrap }\n"
    "center table { text-align: left }\n"
    "center > table { margin-left: auto; margin-right: auto }\n"
    "textarea { white-space: pre-wrap }\n"
    "mark { background-color: yellow; color: black }\n"
    "nobr { white-space: nowrap }\n"
    "ruby { display: ruby } rt { font-size: 50% }\n";

/* a declaration libcss does not know (grid, border-radius, gap), with the selector and media it came with */
enum { SP_GRID_COLS, SP_GRID_ROWS, SP_GRID_AREAS, SP_GRID_TEMPLATE, SP_GRID_AREA, SP_GRID_COLUMN, SP_GRID_ROW,
       SP_RADIUS, SP_GAP, SP_ROW_GAP, SP_MASK, SP_WMASK, SP_N };
typedef struct {
    char *sel;
    char *key;                 /* the class, id or tag the selector's last part needs (0 = any) */
    uint8_t keykind;           /* 1 class, 2 id, 3 tag */
    uint8_t prop;
    char *value;
    int minw, maxw;            /* the media query's window widths */
    int spec, order;
} side_rule;

typedef struct { char *name, *value; } css_var;

/* custom properties set for some elements only (".pane { --w: 300px }", [data-color-mode=dark] { ... }, style="--w:..."):
 * a rule's selector and the properties it sets */
typedef struct {
    char *sel, *key;
    uint8_t keykind;           /* as side_rule's, and 4: an attribute the element must have */
    int minw, maxw, spec, order;
    css_var *defs;
    int ndefs;
} scoped_rule;

/* a rule that uses var(): kept as written, to be written again for elements whose variables differ */
typedef struct {
    char *text;                /* "selector{declarations}" */
    char *uses;                /* the variables it names, "\n--a\n--b\n" */
    int minw, maxw;
} var_rule;

/* the rules of var_rule written with one set of variables, and a selection context with them after the page's sheets */
typedef struct {
    char *key;
    css_stylesheet *sheet;
    css_select_ctx *ctx;
} var_variant;

/* the variables in force at an element where they differ from the page's: inherited down the tree */
typedef struct venv {
    css_var *flat;             /* sorted by name; the strings belong to the rules (or to own) */
    int nflat;
    char **own;                /* strings this one made (from style="") */
    int nown;
    var_variant *variant;
} venv;

/* where the styling of a tree has got to: the children still to do under each parent on the way down */
typedef struct { tf_node *next; const css_computed_style *parent; int is_root; venv *env; } style_frame;

struct tf_styler {
    css_select_ctx *ctx;
    css_stylesheet *ua;
    css_stylesheet **sheets;
    int nsheets;
    css_unit_ctx unit;
    side_rule *side;
    int nside, side_cap;
    css_var *vars;             /* custom properties (--name) of the page, as :root and html define them */
    int nvars, vars_cap;
    char root_classes[1024];   /* the classes of <html> and <body>: which of the rules for them apply */
    char root_attrs[2048];     /* and their attributes, as "name=value" lines */
    style_frame *stk;
    int sp, scap;
    scoped_rule *scoped;
    int nscoped, scoped_cap;
    var_rule *vrules;
    int nvrules, vrules_cap;
    var_variant **variants;    /* (kept from one styling to the next while the sheets stay the same) */
    int nvariants;
    venv **envs;               /* this styling's */
    int nenvs, envs_cap;
    int gen, variants_gen;     /* gen: bumped with each sheet */
    tf_node *hover, *focus;    /* the element under the pointer, the one with the keyboard */
    char **hover_keys;         /* what rules with :hover are about (".class", "#id", "tag", "*"): whether a move matters */
    int nhover_keys;
};

static void envs_clear(tf_styler *st);
static void variants_clear(tf_styler *st);
static venv *cur_env;                              /* the variables var() takes first, while an element is styled */

static tf_styler *current_styler;                  /* (for style="" attributes, which are styled without one) */

/* ------------------------------------------------------------------ the selection handler: libcss asks, the tree answers */

static int name_is(tf_node *n, lwc_string *name)
{
    bool m = false;
    return n && n->type == TF_ELEM && n->name && lwc_string_caseless_isequal(n->name, name, &m) == lwc_error_ok && m;
}

static css_error h_node_name(void *pw, void *node, css_qname *q)
{
    (void)pw;
    q->ns = NULL;
    q->name = lwc_string_ref(((tf_node *)node)->name);
    return CSS_OK;
}

static css_error h_node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *n)
{
    (void)pw;
    tf_node *e = node;
    for (int i = 0; i < e->nclasses; i++) lwc_string_ref(e->classes[i]);     /* libcss lets go of each */
    *classes = e->nclasses ? e->classes : NULL;
    *n = (uint32_t)e->nclasses;
    return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
    (void)pw;
    tf_node *e = node;
    *id = e->id ? lwc_string_ref(e->id) : NULL;
    return CSS_OK;
}

static tf_node *elem_parent(tf_node *n)
{
    tf_node *p = n->parent;
    return p && p->type == TF_ELEM ? p : 0;
}

static tf_node *elem_prev(tf_node *n)
{
    for (tf_node *p = n->prev; p; p = p->prev) if (p->type == TF_ELEM) return p;
    return 0;
}

static css_error h_named_ancestor(void *pw, void *node, const css_qname *q, void **anc)
{
    (void)pw;
    *anc = NULL;
    for (tf_node *p = elem_parent(node); p; p = elem_parent(p)) if (name_is(p, q->name)) { *anc = p; break; }
    return CSS_OK;
}

static css_error h_named_parent(void *pw, void *node, const css_qname *q, void **parent)
{
    (void)pw;
    tf_node *p = elem_parent(node);
    *parent = name_is(p, q->name) ? p : NULL;
    return CSS_OK;
}

static css_error h_named_sibling(void *pw, void *node, const css_qname *q, void **sib)
{
    (void)pw;
    tf_node *p = elem_prev(node);
    *sib = name_is(p, q->name) ? p : NULL;
    return CSS_OK;
}

static css_error h_named_generic_sibling(void *pw, void *node, const css_qname *q, void **sib)
{
    (void)pw;
    *sib = NULL;
    for (tf_node *p = elem_prev(node); p; p = elem_prev(p)) if (name_is(p, q->name)) { *sib = p; break; }
    return CSS_OK;
}

static css_error h_parent(void *pw, void *node, void **parent) { (void)pw; *parent = elem_parent(node); return CSS_OK; }
static css_error h_sibling(void *pw, void *node, void **sib) { (void)pw; *sib = elem_prev(node); return CSS_OK; }

static css_error h_has_name(void *pw, void *node, const css_qname *q, bool *m)
{
    (void)pw;
    *m = name_is(node, q->name);
    return CSS_OK;
}

static css_error h_has_class(void *pw, void *node, lwc_string *name, bool *m)
{
    (void)pw;
    tf_node *e = node;
    *m = false;
    for (int i = 0; i < e->nclasses && !*m; i++) (void)lwc_string_isequal(e->classes[i], name, m);
    return CSS_OK;
}

static css_error h_has_id(void *pw, void *node, lwc_string *name, bool *m)
{
    (void)pw;
    tf_node *e = node;
    *m = false;
    if (e->id) (void)lwc_string_isequal(e->id, name, m);
    return CSS_OK;
}

static const char *attr_q(tf_node *e, const css_qname *q)
{
    char nm[64];
    size_t l = lwc_string_length(q->name);
    if (l >= sizeof(nm)) return 0;
    for (size_t i = 0; i < l; i++) nm[i] = (char)tolower((unsigned char)lwc_string_data(q->name)[i]);
    nm[l] = 0;
    return dom_attr(e, nm);
}

static css_error h_has_attr(void *pw, void *node, const css_qname *q, bool *m)
{
    (void)pw;
    *m = attr_q(node, q) != 0;
    return CSS_OK;
}

/* the attribute selectors: kind 0 =, 1 |=, 2 ~=, 3 ^=, 4 $=, 5 *= */
static int attr_test(const char *have, lwc_string *want, int kind)
{
    if (!have) return 0;
    const char *v = lwc_string_data(want);
    size_t vl = lwc_string_length(want), hl = strlen(have);
    switch (kind) {
    case 0: return hl == vl && !strncasecmp(have, v, vl);
    case 1: return (hl == vl && !strncasecmp(have, v, vl)) || (hl > vl && !strncasecmp(have, v, vl) && have[vl] == '-');
    case 2:
        while (*have) {
            while (*have == ' ' || *have == '\t' || *have == '\n') have++;
            const char *s = have;
            while (*have && *have != ' ' && *have != '\t' && *have != '\n') have++;
            if ((size_t)(have - s) == vl && vl && !memcmp(s, v, vl)) return 1;
        }
        return 0;
    case 3: return vl && hl >= vl && !memcmp(have, v, vl);
    case 4: return vl && hl >= vl && !memcmp(have + hl - vl, v, vl);
    default:
        for (size_t i = 0; vl && i + vl <= hl; i++) if (!memcmp(have + i, v, vl)) return 1;
        return 0;
    }
}

#define ATTR_HANDLER(fn, kind) \
    static css_error fn(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m) \
    { (void)pw; *m = attr_test(attr_q(node, q), v, kind); return CSS_OK; }
ATTR_HANDLER(h_attr_eq, 0)
ATTR_HANDLER(h_attr_dash, 1)
ATTR_HANDLER(h_attr_inc, 2)
ATTR_HANDLER(h_attr_pre, 3)
ATTR_HANDLER(h_attr_suf, 4)
ATTR_HANDLER(h_attr_sub, 5)

static css_error h_is_root(void *pw, void *node, bool *m)
{
    (void)pw;
    tf_node *e = node;
    *m = e->parent && e->parent->type == TF_DOC;
    return CSS_OK;
}

static css_error h_count_siblings(void *pw, void *node, bool same_name, bool after, int32_t *count)
{
    (void)pw;
    tf_node *e = node;
    int32_t c = 0;
    for (tf_node *s = after ? e->next : e->prev; s; s = after ? s->next : s->prev) {
        if (s->type != TF_ELEM) continue;
        if (same_name && !name_is(s, e->name)) continue;
        c++;
    }
    *count = c;
    return CSS_OK;
}

static css_error h_is_empty(void *pw, void *node, bool *m)
{
    (void)pw;
    *m = true;
    for (tf_node *c = ((tf_node *)node)->first; c; c = c->next)
        if (c->type == TF_ELEM || (c->type == TF_TEXT && c->text_len)) { *m = false; break; }
    return CSS_OK;
}

static css_error h_is_link(void *pw, void *node, bool *m)
{
    (void)pw;
    tf_node *e = node;
    *m = (dom_is(e, "a") || dom_is(e, "area")) && dom_attr(e, "href");
    return CSS_OK;
}

static css_error h_false(void *pw, void *node, bool *m) { (void)pw; (void)node; *m = false; return CSS_OK; }
static css_error h_is_hover(void *pw, void *node, bool *m)
{
    tf_styler *s = pw;
    *m = s && s->hover && dom_contains(node, s->hover);
    return CSS_OK;
}
static css_error h_is_focus(void *pw, void *node, bool *m)
{
    tf_styler *s = pw;
    *m = s && s->focus && node == s->focus;
    return CSS_OK;
}

static css_error h_is_enabled(void *pw, void *node, bool *m)
{
    (void)pw;
    tf_node *e = node;
    *m = (dom_is(e, "input") || dom_is(e, "button") || dom_is(e, "select") || dom_is(e, "textarea")) && !dom_attr(e, "disabled");
    return CSS_OK;
}

static css_error h_is_disabled(void *pw, void *node, bool *m) { (void)pw; *m = dom_attr(node, "disabled") != 0; return CSS_OK; }
static css_error h_is_checked(void *pw, void *node, bool *m) { (void)pw; *m = ((tf_node *)node)->checked || dom_attr(node, "checked"); return CSS_OK; }

static css_error h_is_lang(void *pw, void *node, lwc_string *lang, bool *m)
{
    (void)pw;
    *m = false;
    for (tf_node *e = node; e && e->type == TF_ELEM; e = e->parent) {
        const char *l = dom_attr(e, "lang");
        if (l) { *m = attr_test(l, lang, 1); break; }
    }
    return CSS_OK;
}

static int hexv(char c);

/* a colour attribute (bgcolor="#f60", color=white): 0 = not one */
static int attr_color(const char *v, css_color *out)
{
    static const struct { const char *n; uint32_t c; } named[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "navy", 0x000080 },
        { "maroon", 0x800000 }, { "purple", 0x800080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
        { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "orange", 0xFFA500 }, { "lightgrey", 0xD3D3D3 }, { "lightgray", 0xD3D3D3 },
    };
    if (!v) return 0;
    while (*v == ' ') v++;
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) if (!strcasecmp(v, named[i].n)) { *out = 0xFF000000u | named[i].c; return 1; }
    if (*v == '#') v++;
    int n = 0;
    while (hexv(v[n]) >= 0) n++;
    uint32_t c;
    if (n == 6) c = (uint32_t)strtoul(v, 0, 16);
    else if (n == 3) { int r = hexv(v[0]), g = hexv(v[1]), b = hexv(v[2]); c = (uint32_t)((r * 17) << 16 | (g * 17) << 8 | b * 17); }
    else return 0;
    *out = 0xFF000000u | c;
    return 1;
}

/* width="85%", width=120 */
static int attr_len(const char *v, css_hint_length *l)
{
    if (!v || !(*v >= '0' && *v <= '9')) return 0;
    int n = atoi(v);
    l->value = INTTOFIX(n);
    l->unit = strchr(v, '%') ? CSS_UNIT_PCT : CSS_UNIT_PX;
    return 1;
}

/* what old HTML attributes ask for (bgcolor, align, valign, width, cellpadding...), under every style sheet */
static css_error h_hints(void *pw, void *node, uint32_t *n, css_hint **hints)
{
    (void)pw;
    static css_hint h[12];
    tf_node *e = node;
    int k = 0;
    const char *v;
    int cell = dom_is(e, "td") || dom_is(e, "th"), table = dom_is(e, "table");
    if ((cell || table || dom_is(e, "tr") || dom_is(e, "body") || dom_is(e, "thead") || dom_is(e, "tbody")) &&
        attr_color(dom_attr(e, "bgcolor"), &h[k].data.color)) { h[k].prop = CSS_PROP_BACKGROUND_COLOR; h[k].status = CSS_BACKGROUND_COLOR_COLOR; k++; }
    if (dom_is(e, "font") && attr_color(dom_attr(e, "color"), &h[k].data.color)) { h[k].prop = CSS_PROP_COLOR; h[k].status = CSS_COLOR_COLOR; k++; }
    if (dom_is(e, "body") && attr_color(dom_attr(e, "text"), &h[k].data.color)) { h[k].prop = CSS_PROP_COLOR; h[k].status = CSS_COLOR_COLOR; k++; }
    if ((cell || table || dom_is(e, "col") || dom_is(e, "hr")) && attr_len(dom_attr(e, "width"), &h[k].data.length)) { h[k].prop = CSS_PROP_WIDTH; h[k].status = CSS_WIDTH_SET; k++; }
    if ((cell || table || dom_is(e, "tr")) && attr_len(dom_attr(e, "height"), &h[k].data.length)) { h[k].prop = CSS_PROP_HEIGHT; h[k].status = CSS_HEIGHT_SET; k++; }
    if ((cell || dom_is(e, "tr") || dom_is(e, "div") || dom_is(e, "p") || dom_is(e, "thead") || dom_is(e, "tbody") ||
         (lwc_string_length(e->name) == 2 && lwc_string_data(e->name)[0] == 'h' && lwc_string_data(e->name)[1] >= '1' && lwc_string_data(e->name)[1] <= '6')) &&
        (v = dom_attr(e, "align"))) {
        int st = !strcasecmp(v, "center") || !strcasecmp(v, "middle") ? CSS_TEXT_ALIGN_CENTER : !strcasecmp(v, "right") ? CSS_TEXT_ALIGN_RIGHT :
                 !strcasecmp(v, "left") ? CSS_TEXT_ALIGN_LEFT : !strcasecmp(v, "justify") ? CSS_TEXT_ALIGN_JUSTIFY : -1;
        if (st >= 0) { h[k].prop = CSS_PROP_TEXT_ALIGN; h[k].status = (uint8_t)st; k++; }
    }
    if ((cell || dom_is(e, "tr")) && (v = dom_attr(e, "valign"))) {
        int st = !strcasecmp(v, "top") ? CSS_VERTICAL_ALIGN_TOP : !strcasecmp(v, "middle") ? CSS_VERTICAL_ALIGN_MIDDLE :
                 !strcasecmp(v, "bottom") ? CSS_VERTICAL_ALIGN_BOTTOM : !strcasecmp(v, "baseline") ? CSS_VERTICAL_ALIGN_BASELINE : -1;
        if (st >= 0) { h[k].prop = CSS_PROP_VERTICAL_ALIGN; h[k].status = (uint8_t)st; k++; }
    }
    if (table && (v = dom_attr(e, "cellspacing")) && *v >= '0' && *v <= '9') {
        h[k].prop = CSS_PROP_BORDER_SPACING; h[k].status = CSS_BORDER_SPACING_SET;
        h[k].data.position.h.value = h[k].data.position.v.value = INTTOFIX(atoi(v));
        h[k].data.position.h.unit = h[k].data.position.v.unit = CSS_UNIT_PX;
        k++;
    }
    if (cell) {
        tf_node *t = e->parent;
        while (t && !dom_is(t, "table")) t = t->parent;
        if (t && (v = dom_attr(t, "cellpadding")) && *v >= '0' && *v <= '9') {
            static const uint32_t pp[4] = { CSS_PROP_PADDING_TOP, CSS_PROP_PADDING_RIGHT, CSS_PROP_PADDING_BOTTOM, CSS_PROP_PADDING_LEFT };
            for (int i = 0; i < 4; i++) { h[k].prop = pp[i]; h[k].status = CSS_PADDING_SET; h[k].data.length.value = INTTOFIX(atoi(v)); h[k].data.length.unit = CSS_UNIT_PX; k++; }
        }
    }
    *n = (uint32_t)k;
    *hints = k ? h : NULL;
    return CSS_OK;
}

static css_error h_ua_default(void *pw, uint32_t prop, css_hint *hint)
{
    (void)pw;
    if (prop == CSS_PROP_COLOR) { hint->data.color = 0xFF000000; hint->status = CSS_COLOR_COLOR; }
    else if (prop == CSS_PROP_FONT_FAMILY) { hint->data.strings = NULL; hint->status = CSS_FONT_FAMILY_SANS_SERIF; }
    else if (prop == CSS_PROP_QUOTES) { hint->data.strings = NULL; hint->status = CSS_QUOTES_NONE; }
    else if (prop == CSS_PROP_VOICE_FAMILY) { hint->data.strings = NULL; hint->status = 0; }
    else return CSS_INVALID;
    return CSS_OK;
}

static css_error h_set_data(void *pw, void *node, void *data) { (void)pw; ((tf_node *)node)->css_data = data; return CSS_OK; }
static css_error h_get_data(void *pw, void *node, void **data) { (void)pw; *data = ((tf_node *)node)->css_data; return CSS_OK; }

static css_select_handler handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    h_node_name, h_node_classes, h_node_id,
    h_named_ancestor, h_named_parent, h_named_sibling, h_named_generic_sibling,
    h_parent, h_sibling,
    h_has_name, h_has_class, h_has_id, h_has_attr,
    h_attr_eq, h_attr_dash, h_attr_inc, h_attr_pre, h_attr_suf, h_attr_sub,
    h_is_root, h_count_siblings, h_is_empty,
    h_is_link, h_false, h_is_hover, h_false, h_is_focus,        /* link, visited, hover, active, focus */
    h_is_enabled, h_is_disabled, h_is_checked,
    h_false, h_is_lang,                                          /* target, lang */
    h_hints, h_ua_default, h_set_data, h_get_data,
};

/* ------------------------------------------------------------------ style sheets */

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);        /* (pictures in style sheets are not drawn yet) */
    return CSS_OK;
}

static css_stylesheet *make_sheet(const char *css, size_t len, const char *url, int inline_style)
{
    css_stylesheet_params p;
    memset(&p, 0, sizeof(p));
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = url ? url : "about:blank";
    p.title = NULL;
    p.allow_quirks = false;
    p.inline_style = inline_style;
    p.resolve = resolve_url;
    css_stylesheet *s;
    if (css_stylesheet_create(&p, &s) != CSS_OK) return 0;
    css_error e = css_stylesheet_append_data(s, (const uint8_t *)css, len);
    if (e != CSS_OK && e != CSS_NEEDDATA) { css_stylesheet_destroy(s); return 0; }
    css_stylesheet_data_done(s);
    return s;
}


/* ------------------------------------------------------------------ what libcss does not know */

static const char *const side_names[SP_N] = { "grid-template-columns", "grid-template-rows", "grid-template-areas",
    "grid-template", "grid-area", "grid-column", "grid-row", "border-radius", "gap", "row-gap", "mask-image", "-webkit-mask-image" };

static int css_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static const char *skip_ws_comments(const char *p, const char *e)
{
    for (;;) {
        while (p < e && css_ws(*p)) p++;
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            p = p + 2 < e ? p + 2 : e;
            continue;
        }
        return p;
    }
}

/* the end of what starts at p (a block or a statement): the matching '}' or ';' at depth 0 */
static const char *skip_to(const char *p, const char *e, char stop)
{
    int depth = 0;
    char q = 0;
    for (; p < e; p++) {
        char c = *p;
        if (q) { if (c == '\\') p++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '/' && p + 1 < e && p[1] == '*') { p += 2; while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++; p++; continue; }
        if (c == stop && depth == 0) return p;
        if (c == '{' || c == '(' || c == '[') depth++;
        else if (c == '}' || c == ')' || c == ']') { depth--; if (depth < 0) return p; }
    }
    return e;
}

static int media_len(const char *v, int font)
{
    int n = 0, frac = 0, div = 1;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') { if (div < 1000) { frac = frac * 10 + (*v - '0'); div *= 10; } v++; } }
    int scale = (!strncmp(v, "em", 2) || !strncmp(v, "rem", 3)) ? font : 1;
    return n * scale + frac * scale / div;
}

/* "screen and (min-width: 1120px)" -> the widths it is for; 0 = never (print) */
static int media_query(const char *p, const char *e, int *minw, int *maxw)
{
    char q[512];
    int n = (int)(e - p) < 511 ? (int)(e - p) : 511;
    for (int i = 0; i < n; i++) q[i] = (char)tolower((unsigned char)p[i]);
    q[n] = 0;
    if (strstr(q, "print") && !strstr(q, "screen") && !strstr(q, "all")) return 0;
    if (strstr(q, "not ")) return 0;
    if (strstr(q, "prefers-color-scheme") && strstr(q, "dark")) return 0;
    char *m;
    if ((m = strstr(q, "min-width"))) { m = strchr(m, ':'); if (m) { int w = media_len(m + 1, 16); if (w > *minw) *minw = w; } }
    if ((m = strstr(q, "max-width"))) { m = strchr(m, ':'); if (m) { int w = media_len(m + 1, 16); if (w < *maxw) *maxw = w; } }
    return 1;
}

static int specificity(const char *s)
{
    int sp = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '#') sp += 100;
        else if (*c == '.' || *c == '[') sp += 10;
        else if (*c == ':' && c[1] != ':') sp += 10;
        else if ((*c >= 'a' && *c <= 'z') && (c == s || c[-1] == ' ' || c[-1] == '>' || c[-1] == '+' || c[-1] == '~')) sp += 1;
    }
    return sp;
}

static void side_add(tf_styler *st, const char *sel, int sl, int prop, const char *val, int vl, int minw, int maxw)
{
    while (sl && css_ws(sel[sl - 1])) sl--;
    while (sl && css_ws(*sel)) { sel++; sl--; }
    while (vl && css_ws(val[vl - 1])) vl--;
    while (vl && css_ws(*val)) { val++; vl--; }
    if (vl > 10 && !strncmp(val + vl - 10, "!important", 10)) { vl -= 10; while (vl && css_ws(val[vl - 1])) vl--; }
    if (sl <= 0 || vl <= 0) return;
    /* rules for hover, focus and the like are left out: tFire cannot tell those states anyway */
    for (const char *c = memchr(sel, ':', (size_t)sl); c && c < sel + sl; c = memchr(c + 1, ':', (size_t)(sel + sl - c - 1)))
        if (strncmp(c, ":root", 5) && strncmp(c, ":not(", 5) && strncmp(c, ":first-child", 12) && strncmp(c, ":last-child", 11)) return;
    if (st->nside == st->side_cap) {
        int nc = st->side_cap ? st->side_cap * 2 : 64;
        side_rule *nr = realloc(st->side, (size_t)nc * sizeof(side_rule));
        if (!nr) return;
        st->side = nr;
        st->side_cap = nc;
    }
    side_rule *r = &st->side[st->nside];
    memset(r, 0, sizeof(*r));
    r->sel = strndup_(sel, (size_t)sl);
    r->value = strndup_(val, (size_t)vl);
    if (!r->sel || !r->value) { free(r->sel); free(r->value); return; }
    r->prop = (uint8_t)prop;
    r->minw = minw;
    r->maxw = maxw;
    r->spec = specificity(r->sel);
    r->order = st->nside;
#ifdef TFIRE_HOST
    if (getenv("TFSIDE")) fprintf(stderr, "side %s { %s: %s } media %d..%d\n", r->sel, side_names[prop], r->value, minw, maxw);
#endif
    /* the last compound's class, id or tag: a quick test before matching the whole selector */
    const char *last = r->sel + strlen(r->sel);
    const char *c = last;
    while (c > r->sel && c[-1] != ' ' && c[-1] != '>' && c[-1] != '+' && c[-1] != '~') c--;
    const char *k = 0;
    for (const char *q = c; q < last; q++) if (*q == '#') { k = q; r->keykind = 2; break; }
    if (!k) for (const char *q = c; q < last; q++) if (*q == '.') { k = q; r->keykind = 1; break; }
    if (k) {
        k++;
        const char *ke = k;
        while (ke < last && *ke != '.' && *ke != '#' && *ke != '[' && *ke != ':') ke++;
        r->key = strndup_(k, (size_t)(ke - k));
    } else if (c < last && *c >= 'a' && *c <= 'z') {
        const char *ke = c;
        while (ke < last && ((*ke >= 'a' && *ke <= 'z') || (*ke >= '0' && *ke <= '9'))) ke++;
        r->key = strndup_(c, (size_t)(ke - c));
        r->keykind = 3;
    }
    st->nside++;
}

static void side_block(tf_styler *st, const char *p, const char *e, int minw, int maxw)
{
    for (;;) {
        p = skip_ws_comments(p, e);
        if (p >= e) return;
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *a = p + 1;
            const char *ae = a;
            while (ae < e && (*ae == '-' || (*ae >= 'a' && *ae <= 'z') || (*ae >= 'A' && *ae <= 'Z'))) ae++;
            const char *brace = skip_to(ae, e, '{');
            const char *semi = skip_to(ae, brace, ';');
            if (semi < brace) { p = semi + 1; continue; }            /* @import, @charset... */
            if (brace >= e) return;
            const char *close = skip_to(brace + 1, e, '}');
            int alen = (int)(ae - a);
            if (alen == 5 && !strncasecmp(a, "media", 5)) {
                int mn = minw, mx = maxw;
                if (media_query(ae, brace, &mn, &mx)) side_block(st, brace + 1, close, mn, mx);
            } else if ((alen == 8 && !strncasecmp(a, "supports", 8)) || (alen == 5 && !strncasecmp(a, "layer", 5)) ||
                       (alen == 9 && !strncasecmp(a, "container", 9))) {
                side_block(st, brace + 1, close, minw, maxw);
            }
            p = close + 1;
            continue;
        }
        const char *brace = skip_to(p, e, '{');
        if (brace >= e) return;
        const char *close = skip_to(brace + 1, e, '}');
        /* the declarations libcss leaves out */
        for (const char *d = brace + 1; d < close;) {
            d = skip_ws_comments(d, close);
            const char *colon = skip_to(d, close, ':');
            const char *end = skip_to(d, close, ';');
            if (colon < end) {
                int nl = (int)(colon - d);
                while (nl && css_ws(d[nl - 1])) nl--;
                for (int pi = 0; pi < SP_N; pi++) {
                    if ((int)strlen(side_names[pi]) != nl || strncasecmp(d, side_names[pi], (size_t)nl)) continue;
                    /* each selector of the list by itself */
                    const char *sel = p;
                    while (sel < brace) {
                        const char *comma = skip_to(sel, brace, ',');
                        side_add(st, sel, (int)(comma - sel), pi, colon + 1, (int)(end - colon - 1), minw, maxw);
                        sel = comma + 1;
                    }
                }
            }
            d = end + 1;
        }
        p = close + 1;
    }
}

static const char *memmem_(const char *h, size_t hl, const char *n, size_t nl)
{
    for (size_t i = 0; i + nl <= hl; i++) if (!memcmp(h + i, n, nl)) return h + i;
    return 0;
}

static int has_class(tf_node *n, const char *c)
{
    size_t l = strlen(c);
    for (int i = 0; i < n->nclasses; i++)
        if (lwc_string_length(n->classes[i]) == l && !memcmp(lwc_string_data(n->classes[i]), c, l)) return 1;
    return 0;
}

/* the side declarations that apply to n, into its style */
static void side_apply(tf_styler *st, tf_node *n, tf_style *t, int vw)
{
    const side_rule *best[SP_N] = { 0 };
    for (int i = 0; i < st->nside; i++) {
        const side_rule *r = &st->side[i];
        if (vw < r->minw || vw > r->maxw) continue;
        if (r->keykind == 1 && !has_class(n, r->key)) continue;
        if (r->keykind == 2 && (!n->id || strcmp(lwc_string_data(n->id), r->key))) continue;
        if (r->keykind == 3 && !dom_is(n, r->key)) continue;
        const side_rule *b = best[r->prop];
        if (b && (b->spec > r->spec || (b->spec == r->spec && b->order > r->order))) continue;
        if (!dom_matches(n, r->sel)) continue;
        best[r->prop] = r;
    }
    t->grid_cols = best[SP_GRID_COLS] ? best[SP_GRID_COLS]->value : 0;
    t->grid_rows = best[SP_GRID_ROWS] ? best[SP_GRID_ROWS]->value : 0;
    t->grid_areas = best[SP_GRID_AREAS] ? best[SP_GRID_AREAS]->value : 0;
    t->grid_area = best[SP_GRID_AREA] ? best[SP_GRID_AREA]->value : 0;
    t->grid_col = best[SP_GRID_COLUMN] ? best[SP_GRID_COLUMN]->value : 0;
    t->grid_row = best[SP_GRID_ROW] ? best[SP_GRID_ROW]->value : 0;
    /* grid-template: rows / columns (whichever was written later wins over the longhands) */
    const side_rule *gt = best[SP_GRID_TEMPLATE];
    if (gt) {
        const char *slash = gt->value;
        int depth = 0;
        for (; *slash; slash++) { if (*slash == '(') depth++; else if (*slash == ')') depth--; else if (*slash == '/' && !depth) break; }
        if (*slash && (!best[SP_GRID_COLS] || best[SP_GRID_COLS]->spec < gt->spec ||
                       (best[SP_GRID_COLS]->spec == gt->spec && best[SP_GRID_COLS]->order < gt->order)))
            t->grid_cols = slash + 1;
        if (!best[SP_GRID_ROWS] || best[SP_GRID_ROWS]->order < gt->order) t->grid_rows = gt->value;
        if (strchr(gt->value, '\'') || strchr(gt->value, '"')) {
            if (!best[SP_GRID_AREAS] || best[SP_GRID_AREAS]->order < gt->order) t->grid_areas = gt->value;
        }
    }
    if (best[SP_RADIUS]) {
        const char *v = best[SP_RADIUS]->value;
        int r = media_len(v, t->font_px);
        if (strchr(v, '%')) r = 999;                           /* (a circle or a pill: as round as it gets) */
        t->radius = r;
    }
    if (best[SP_GAP] && !t->gap) { t->row_gap = t->gap = media_len(best[SP_GAP]->value, t->font_px); }
    if (best[SP_ROW_GAP]) t->row_gap = media_len(best[SP_ROW_GAP]->value, t->font_px);
    /* an icon drawn as a mask over its background colour */
    const side_rule *mk = best[SP_MASK] ? best[SP_MASK] : best[SP_WMASK];
    t->mask = mk && strstr(mk->value, "url(") ? mk->value : 0;
}

tf_styler *style_new(void)
{
    tf_styler *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    if (css_select_ctx_create(&s->ctx) != CSS_OK) { free(s); return 0; }
    s->ua = make_sheet(ua_css, sizeof(ua_css) - 1, "tfire:ua", 0);
    if (s->ua) css_select_ctx_append_sheet(s->ctx, s->ua, CSS_ORIGIN_UA, NULL);
    s->unit.viewport_width = INTTOFIX(1024);
    s->unit.viewport_height = INTTOFIX(768);
    s->unit.font_size_default = INTTOFIX(16);
    s->unit.font_size_minimum = INTTOFIX(9);
    s->unit.device_dpi = INTTOFIX(96);
    return s;
}

void style_free(tf_styler *s)
{
    if (!s) return;
    for (int i = 0; i < s->nside; i++) { free(s->side[i].sel); free(s->side[i].key); free(s->side[i].value); }
    free(s->side);
    for (int i = 0; i < s->nvars; i++) { free(s->vars[i].name); free(s->vars[i].value); }
    free(s->vars);
    for (int i = 0; i < s->nscoped; i++) {
        for (int j = 0; j < s->scoped[i].ndefs; j++) { free(s->scoped[i].defs[j].name); free(s->scoped[i].defs[j].value); }
        free(s->scoped[i].defs); free(s->scoped[i].sel); free(s->scoped[i].key);
    }
    free(s->scoped);
    for (int i = 0; i < s->nvrules; i++) { free(s->vrules[i].text); free(s->vrules[i].uses); }
    free(s->vrules);
    envs_clear(s);
    free(s->envs);
    for (int i = 0; i < s->nhover_keys; i++) free(s->hover_keys[i]);
    free(s->hover_keys);
    variants_clear(s);
    if (current_styler == s) current_styler = 0;
    free(s->stk);
    css_select_ctx_destroy(s->ctx);
    for (int i = 0; i < s->nsheets; i++) css_stylesheet_destroy(s->sheets[i]);
    free(s->sheets);
    if (s->ua) css_stylesheet_destroy(s->ua);
    free(s);
}

/* @supports blocks, which libcss leaves out: kept (their rules as if they stood by themselves) when tFire takes the
 * condition to hold, dropped when it says "not" or asks for what tFire has not got */
static int supports_ok(const char *p, const char *e)
{
    char q[256];
    int n = (int)(e - p) < 255 ? (int)(e - p) : 255;
    for (int i = 0; i < n; i++) q[i] = (char)tolower((unsigned char)p[i]);
    q[n] = 0;
    if (strstr(q, "not ") || strstr(q, "not(")) return 0;
    if (strstr(q, "selector(") || strstr(q, "backdrop") || strstr(q, "subgrid") || strstr(q, "container") || strstr(q, "anchor")) return 0;
    return 1;
}

static char *unwrap_supports(const char *css, uint32_t len, uint32_t *out_len);

typedef struct { char *s; size_t n, cap; } obuf;

static void ob_put(obuf *o, const char *p, size_t n)
{
    if (o->n + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 4096;
        while (nc < o->n + n + 1) nc *= 2;
        char *ns = realloc(o->s, nc);
        if (!ns) return;
        o->s = ns;
        o->cap = nc;
    }
    memcpy(o->s + o->n, p, n);
    o->n += n;
    o->s[o->n] = 0;
}


/* ------------------------------------------------------------------ media queries level 4 ranges, which libcss does not know */

/* "(width >= 1012px)" -> "(min-width:1012px)", "(width<=1011.98px)" -> "(max-width:1011.98px)",
 * "(600px <= width < 900px)" -> "(min-width:600px) and (max-width:899.99px)"; the same for height */
static int range_one(const char *p, const char *e, obuf *o)
{
    char t[160];
    int n = 0;
    for (const char *q = p; q < e && n < 159; q++) if (!css_ws(*q)) t[n++] = *q;
    t[n] = 0;
    const char *feat = strstr(t, "width") ? "width" : strstr(t, "height") ? "height" : 0;
    if (!feat || (!strchr(t, '<') && !strchr(t, '>'))) return 0;
    /* split at the operators: a op feat [op b] */
    char *f = strstr(t, feat);
    size_t fl = strlen(feat);
    char left[64] = "", right[64] = "", lop[3] = "", rop[3] = "";
    if (f > t) {                                      /* value op feature */
        char *opp = f;
        while (opp > t && (opp[-1] == '<' || opp[-1] == '>' || opp[-1] == '=')) opp--;
        snprintf(lop, sizeof(lop), "%.*s", (int)(f - opp), opp);
        snprintf(left, sizeof(left), "%.*s", (int)(opp - t), t);
    }
    char *after = f + fl;
    if (*after) {
        char *v = after;
        while (*v == '<' || *v == '>' || *v == '=') v++;
        snprintf(rop, sizeof(rop), "%.*s", (int)(v - after), after);
        snprintf(right, sizeof(right), "%s", v);
    }
    int any = 0;
    char buf[200];
    /* "a < width": width > a; "a <= width": width >= a; "a > width": width < a */
    if (left[0] && lop[0]) {
        int ge = lop[0] == '<';                       /* a < width -> min */
        int strict = !strchr(lop, '=');
        snprintf(buf, sizeof(buf), "(%s-%s:%s%s)", ge ? "min" : "max", feat, left, strict ? "" : "");
        ob_put(o, buf, strlen(buf));
        any = 1;
    }
    if (right[0] && rop[0]) {
        int max = rop[0] == '<';                      /* width < b -> max */
        if (any) ob_put(o, " and ", 5);
        snprintf(buf, sizeof(buf), "(%s-%s:%s)", max ? "max" : "min", feat, right);
        ob_put(o, buf, strlen(buf));
        any = 1;
    }
    return any;
}

static void rewrite_ranges_prelude(const char *p, const char *e, obuf *o)
{
    while (p < e) {
        if (*p == '(') {
            const char *c = skip_to(p + 1, e, ')');
            int lt = 0;
            for (const char *q = p; q < c; q++) if (*q == '<' || *q == '>') lt = 1;
            if (lt && range_one(p + 1, c, o)) { p = c < e ? c + 1 : e; continue; }
            ob_put(o, p, (size_t)((c < e ? c + 1 : e) - p));
            p = c < e ? c + 1 : e;
            continue;
        }
        ob_put(o, p, 1);
        p++;
    }
}

static void rewrite_ranges_block(const char *p, const char *e, obuf *o)
{
    while (p < e) {
        const char *at = 0;
        for (const char *q = p; q + 6 < e; q++) {
            if (*q == '"' || *q == '\'') { char qc = *q++; while (q < e && *q != qc) { if (*q == '\\') q++; q++; } continue; }
            if (*q == '@' && !strncasecmp(q, "@media", 6)) { at = q; break; }
        }
        if (!at) { ob_put(o, p, (size_t)(e - p)); return; }
        const char *brace = skip_to(at + 6, e, '{');
        ob_put(o, p, (size_t)(at + 6 - p));
        rewrite_ranges_prelude(at + 6, brace, o);
        p = brace;
    }
}

static char *rewrite_ranges(const char *css, uint32_t len, uint32_t *out_len)
{
    int has = 0;
    for (const char *q = memmem_(css, len, "@media", 6); q && !has; q = memmem_(q + 6, (size_t)(css + len - q - 6), "@media", 6)) {
        const char *b = skip_to(q + 6, css + len, '{');
        for (const char *c = q; c < b; c++) if (*c == '<' || *c == '>') { has = 1; break; }
    }
    if (!has) return 0;
    obuf o = { 0, 0, 0 };
    rewrite_ranges_block(css, css + len, &o);
    *out_len = (uint32_t)o.n;
    return o.s;
}
/* ------------------------------------------------------------------ :is() and :where(), which libcss does not know */

/* the selector list s (n bytes) with every :is(a, b) / :where(a, b) written out as the selectors it stands for
 * (".x :is(.a,.b) p" -> ".x .a p, .x .b p"), appended to o; 0 = it had none */
static int expand_is_one(const char *s, size_t n, obuf *o, int *count, int depth)
{
    /* the first :is( or :where( */
    const char *f = 0;
    size_t fl = 0;
    for (size_t i = 0; i + 4 < n; i++) {
        if (s[i] != ':') continue;
        if (!strncmp(s + i, ":is(", 4)) { f = s + i; fl = 4; break; }
        if (i + 7 < n && !strncmp(s + i, ":where(", 7)) { f = s + i; fl = 7; break; }
        if (i + 9 < n && !strncmp(s + i, ":matches(", 9)) { f = s + i; fl = 9; break; }
        if (i + 13 < n && !strncmp(s + i, ":-webkit-any(", 13)) { f = s + i; fl = 13; break; }
    }
    if (!f || depth > 4) {
        if (*count >= 64) return 1;
        if (o->n) ob_put(o, ",", 1);
        ob_put(o, s, n);
        (*count)++;
        return 0;
    }
    const char *a = f + fl, *e = s + n;
    const char *close = skip_to(a, e, ')');
    /* "a:is(...)" with nothing before it in the compound means "*:is(...)": no prefix is fine either way */
    while (a < close && *count < 64) {
        const char *comma = skip_to(a, close, ',');
        obuf one = { 0, 0, 0 };
        ob_put(&one, s, (size_t)(f - s));
        const char *x = a, *xe = comma;
        while (x < xe && css_ws(*x)) x++;
        while (xe > x && css_ws(xe[-1])) xe--;
        ob_put(&one, x, (size_t)(xe - x));
        if (close < e) ob_put(&one, close + 1, (size_t)(e - close - 1));
        if (one.s) expand_is_one(one.s, one.n, o, count, depth + 1);
        free(one.s);
        a = comma + 1;
    }
    return 1;
}

static void expand_is(const char *sel, size_t n, obuf *o)
{
    int count = 0;
    obuf list = { 0, 0, 0 };
    const char *p = sel, *e = sel + n;
    while (p < e) {
        const char *comma = skip_to(p, e, ',');
        const char *x = p, *xe = comma;
        while (x < xe && css_ws(*x)) x++;
        while (xe > x && css_ws(xe[-1])) xe--;
        if (xe > x) expand_is_one(x, (size_t)(xe - x), &list, &count, 0);
        p = comma + 1;
    }
    if (list.s) ob_put(o, list.s, list.n);
    free(list.s);
}

/* every rule's selector list with :is() and :where() written out (inside @media too) */
/* pseudo-classes and -elements libcss knows; a selector with another one would take its whole rule down with it */
static int pseudo_known(const char *p, size_t n, int element)
{
    static const char *const cls[] = { "first-child", "link", "visited", "hover", "active", "focus", "lang", "root", "nth-child",
        "nth-last-child", "nth-of-type", "nth-last-of-type", "last-child", "first-of-type", "last-of-type", "only-child",
        "only-of-type", "empty", "target", "enabled", "disabled", "checked", "not", "first-line", "first-letter", "before", "after", 0 };
    static const char *const els[] = { "first-line", "first-letter", "before", "after", 0 };
    for (const char *const *k = element ? els : cls; *k; k++) if (strlen(*k) == n && !strncasecmp(*k, p, n)) return 1;
    return 0;
}

/* a selector list with what libcss cannot read made readable: newer pseudo-classes as the nearest old one, and the
 * selectors it still cannot read left out (rather than the whole rule) */
static void sel_fix(const char *p, size_t n, obuf *o)
{
    const char *e = p + n;
    int kept = 0;
    while (p < e) {
        const char *comma = skip_to(p, e, ',');
        obuf one = { 0, 0, 0 };
        int ok = 1;
        for (const char *q = p; q < comma && ok;) {
            if (*q != ':') {
                const char *r = q;
                while (r < comma && *r != ':') {
                    if (*r == '\\' && r + 1 < comma) { r += 2; continue; }      /* .md\:flex: an escaped colon */
                    if (*r == '[') r = skip_to(r + 1, comma, ']');
                    if (r < comma) r++;
                }
                ob_put(&one, q, (size_t)(r - q));
                q = r;
                continue;
            }
            int el = q + 1 < comma && q[1] == ':';
            const char *nm = q + (el ? 2 : 1), *ne = nm;
            while (ne < comma && (*ne == '-' || (*ne >= 'a' && *ne <= 'z') || (*ne >= 'A' && *ne <= 'Z') || (*ne >= '0' && *ne <= '9'))) ne++;
            size_t nl = (size_t)(ne - nm);
            const char *rep = 0;
            if (!el && ((nl == 12 && !strncasecmp(nm, "focus-within", 12)) || (nl == 13 && !strncasecmp(nm, "focus-visible", 13)))) rep = ":focus";
            else if (!el && nl == 8 && !strncasecmp(nm, "any-link", 8)) rep = ":link";
            else if (!el && nl == 7 && !strncasecmp(nm, "defined", 7)) rep = "";
            if (rep) { ob_put(&one, rep, strlen(rep)); q = ne; continue; }
            if (!pseudo_known(nm, nl, el)) { ok = 0; break; }
            const char *end = ne;
            if (end < comma && *end == '(') {
                /* inside :not( ) the same again */
                const char *cl = skip_to(end + 1, comma, ')');
                ob_put(&one, q, (size_t)(end + 1 - q));
                if (nl == 3 && !strncasecmp(nm, "not", 3)) {
                    obuf in = { 0, 0, 0 };
                    sel_fix(end + 1, (size_t)(cl - end - 1), &in);
                    if (!in.s || !strcmp(in.s, "tfire-none")) ok = 0;
                    else ob_put(&one, in.s, in.n);
                    free(in.s);
                } else ob_put(&one, end + 1, (size_t)(cl - end - 1));
                ob_put(&one, ")", 1);
                q = cl < comma ? cl + 1 : comma;
                continue;
            }
            ob_put(&one, q, (size_t)(end - q));
            q = end;
        }
        if (ok && one.s) {
            if (kept) ob_put(o, ",", 1);
            ob_put(o, one.s, one.n);
            kept++;
        }
        free(one.s);
        p = comma < e ? comma + 1 : e;
    }
    if (!kept) ob_put(o, "tfire-none", 10);
}

/* the values of a shorthand: up to 4, split at top-level spaces */
static int split_vals(const char *v, const char *e, const char **vs, const char **ve, int max)
{
    int n = 0;
    while (v < e && n < max) {
        while (v < e && css_ws(*v)) v++;
        if (v >= e) break;
        const char *s = v;
        int depth = 0;
        while (v < e && (depth || !css_ws(*v))) { if (*v == '(') depth++; else if (*v == ')') depth--; v++; }
        vs[n] = s; ve[n] = v; n++;
    }
    return n;
}

static void put_decl(obuf *o, const char *name, const char *v, const char *e, const char *imp)
{
    ob_put(o, name, strlen(name));
    ob_put(o, ":", 1);
    ob_put(o, v, (size_t)(e - v));
    if (imp) ob_put(o, imp, strlen(imp));
    ob_put(o, ";", 1);
}

/* logical properties (padding-inline, inset, inline-size...), which libcss does not know, as the physical ones of a
 * left-to-right, top-to-bottom page */
static void logical_block(const char *p, const char *e, obuf *o)
{
    static const char *const one[][2] = {
        { "padding-inline-start", "padding-left" }, { "padding-inline-end", "padding-right" },
        { "padding-block-start", "padding-top" }, { "padding-block-end", "padding-bottom" },
        { "margin-inline-start", "margin-left" }, { "margin-inline-end", "margin-right" },
        { "margin-block-start", "margin-top" }, { "margin-block-end", "margin-bottom" },
        { "inset-inline-start", "left" }, { "inset-inline-end", "right" }, { "inset-block-start", "top" }, { "inset-block-end", "bottom" },
        { "border-inline-start", "border-left" }, { "border-inline-end", "border-right" },
        { "border-block-start", "border-top" }, { "border-block-end", "border-bottom" },
        { "border-inline-start-width", "border-left-width" }, { "border-inline-end-width", "border-right-width" },
        { "border-block-start-width", "border-top-width" }, { "border-block-end-width", "border-bottom-width" },
        { "border-inline-start-color", "border-left-color" }, { "border-inline-end-color", "border-right-color" },
        { "border-block-start-color", "border-top-color" }, { "border-block-end-color", "border-bottom-color" },
        { "border-inline-start-style", "border-left-style" }, { "border-inline-end-style", "border-right-style" },
        { "border-block-start-style", "border-top-style" }, { "border-block-end-style", "border-bottom-style" },
        { "inline-size", "width" }, { "block-size", "height" }, { "min-inline-size", "min-width" }, { "max-inline-size", "max-width" },
        { "min-block-size", "min-height" }, { "max-block-size", "max-height" },
        { "border-start-start-radius", "border-top-left-radius" }, { "border-start-end-radius", "border-top-right-radius" },
        { "border-end-start-radius", "border-bottom-left-radius" }, { "border-end-end-radius", "border-bottom-right-radius" },
    };
    /* name, first side, second side (two values: start end) */
    static const char *const two[][3] = {
        { "padding-inline", "padding-left", "padding-right" }, { "padding-block", "padding-top", "padding-bottom" },
        { "margin-inline", "margin-left", "margin-right" }, { "margin-block", "margin-top", "margin-bottom" },
        { "inset-inline", "left", "right" }, { "inset-block", "top", "bottom" },
        { "border-inline", "border-left", "border-right" }, { "border-block", "border-top", "border-bottom" },
        { "border-inline-width", "border-left-width", "border-right-width" }, { "border-block-width", "border-top-width", "border-bottom-width" },
        { "border-inline-color", "border-left-color", "border-right-color" }, { "border-block-color", "border-top-color", "border-bottom-color" },
        { "border-inline-style", "border-left-style", "border-right-style" }, { "border-block-style", "border-top-style", "border-bottom-style" },
    };
    while (p < e) {
        const char *end = skip_to(p, e, ';');
        const char *d = skip_ws_comments(p, end);
        const char *colon = skip_to(d, end, ':');
        int done = 0;
        if (colon < end && d < colon && (*d == 'p' || *d == 'm' || *d == 'i' || *d == 'b' || *d == 'm')) {
            size_t nl = (size_t)(colon - d);
            while (nl && css_ws(d[nl - 1])) nl--;
            const char *v = colon + 1, *ve = end;
            while (ve > v && css_ws(ve[-1])) ve--;
            const char *imp = 0;
            if (ve - v > 10 && !strncmp(ve - 10, "!important", 10)) { imp = "!important"; ve -= 10; while (ve > v && css_ws(ve[-1])) ve--; }
            for (size_t k = 0; k < sizeof(one) / sizeof(one[0]) && !done; k++)
                if (strlen(one[k][0]) == nl && !strncmp(d, one[k][0], nl)) { put_decl(o, one[k][1], v, ve, imp); done = 1; }
            for (size_t k = 0; k < sizeof(two) / sizeof(two[0]) && !done; k++)
                if (strlen(two[k][0]) == nl && !strncmp(d, two[k][0], nl)) {
                    const char *vs[2], *vx[2];
                    int n = !strncmp(d, "border-inline", 13) && nl == 13 ? 0 : !strncmp(d, "border-block", 12) && nl == 12 ? 0 : split_vals(v, ve, vs, vx, 2);
                    if (n == 0) { put_decl(o, two[k][1], v, ve, imp); put_decl(o, two[k][2], v, ve, imp); }
                    else { put_decl(o, two[k][1], vs[0], vx[0], imp); put_decl(o, two[k][2], vs[n - 1], vx[n - 1], imp); }
                    done = 1;
                }
            if (!done && nl == 5 && !strncmp(d, "inset", 5)) {
                const char *vs[4], *vx[4];
                int n = split_vals(v, ve, vs, vx, 4);
                if (n > 0) {
                    int t = 0, r = n > 1 ? 1 : 0, b = n > 2 ? 2 : 0, l = n > 3 ? 3 : r;
                    put_decl(o, "top", vs[t], vx[t], imp); put_decl(o, "right", vs[r], vx[r], imp);
                    put_decl(o, "bottom", vs[b], vx[b], imp); put_decl(o, "left", vs[l], vx[l], imp);
                    done = 1;
                }
            }
        }
        if (!done) { ob_put(o, p, (size_t)(end - p)); if (end < e) ob_put(o, ";", 1); }
        p = end < e ? end + 1 : e;
    }
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* #rgba and #rrggbbaa, which libcss does not read, as rgba() */
static void hex_alpha(const char *p, const char *e, obuf *o)
{
    const char *run = p;
    for (const char *q = p; q < e; q++) {
        if (*q != '#' || (q > p && !(css_ws(q[-1]) || q[-1] == ':' || q[-1] == ',' || q[-1] == '('))) continue;
        int n = 0;
        while (q + 1 + n < e && hexv(q[1 + n]) >= 0) n++;
        const char *after = q + 1 + n;
        if ((n != 4 && n != 8) || (after < e && ((*after >= 'g' && *after <= 'z') || (*after >= 'G' && *after <= 'Z') || *after == '-' || *after == '_'))) continue;
        int r, g, b, a;
        const char *h = q + 1;
        if (n == 4) { r = hexv(h[0]) * 17; g = hexv(h[1]) * 17; b = hexv(h[2]) * 17; a = hexv(h[3]) * 17; }
        else { r = hexv(h[0]) * 16 + hexv(h[1]); g = hexv(h[2]) * 16 + hexv(h[3]); b = hexv(h[4]) * 16 + hexv(h[5]); a = hexv(h[6]) * 16 + hexv(h[7]); }
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "rgba(%d,%d,%d,%d.%03d)", r, g, b, a / 255, (a % 255) * 1000 / 255);
        ob_put(o, run, (size_t)(q - run));
        ob_put(o, tmp, strlen(tmp));
        run = after;
        q = after - 1;
    }
    ob_put(o, run, (size_t)(e - run));
}

static int has_hex_alpha(const char *p, const char *e)
{
    for (const char *q = p; q < e; q++) {
        if (*q != '#') continue;
        int n = 0;
        while (q + 1 + n < e && hexv(q[1 + n]) >= 0) n++;
        if (n == 4 || n == 8) return 1;
    }
    return 0;
}

static int has_logical(const char *p, const char *e)
{
    return memmem_(p, (size_t)(e - p), "-inline", 7) || memmem_(p, (size_t)(e - p), "-block", 6) ||
           memmem_(p, (size_t)(e - p), "inset", 5) || memmem_(p, (size_t)(e - p), "-size", 5) ||
           memmem_(p, (size_t)(e - p), "-start-", 7) || memmem_(p, (size_t)(e - p), "-end-", 5);
}

static void rewrite_is_block(const char *p, const char *e, obuf *o)
{
    while (p < e) {
        const char *q = skip_ws_comments(p, e);
        ob_put(o, p, (size_t)(q - p));
        p = q;
        if (p >= e) break;
        if (*p == '}') { ob_put(o, "}", 1); p++; continue; }
        if (*p == '@') {
            const char *brace = skip_to(p, e, '{'), *semi = skip_to(p, brace, ';');
            if (semi < brace) { ob_put(o, p, (size_t)(semi + 1 - p)); p = semi + 1; continue; }
            if (brace >= e) { ob_put(o, p, (size_t)(e - p)); return; }
            const char *close = skip_to(brace + 1, e, '}');
            ob_put(o, p, (size_t)(brace + 1 - p));
            if (!strncasecmp(p, "@media", 6)) rewrite_is_block(brace + 1, close, o);
            else ob_put(o, brace + 1, (size_t)(close - brace - 1));
            ob_put(o, "}", 1);
            p = close < e ? close + 1 : e;
            continue;
        }
        const char *brace = skip_to(p, e, '{');
        if (brace >= e) { ob_put(o, p, (size_t)(e - p)); return; }
        const char *close = skip_to(brace + 1, e, '}');
        int has = 0;
        for (const char *c = p; c + 4 < brace && !has; c++)
            if (*c == ':' && (!strncmp(c, ":is(", 4) || !strncmp(c, ":where(", 7) || !strncmp(c, ":matches(", 9) || !strncmp(c, ":-webkit-any(", 13))) has = 1;
        if (has) {
            obuf x = { 0, 0, 0 };
            expand_is(p, (size_t)(brace - p), &x);
            if (x.s) sel_fix(x.s, x.n, o);
            free(x.s);
        } else if (memchr(p, ':', (size_t)(brace - p))) sel_fix(p, (size_t)(brace - p), o);
        else ob_put(o, p, (size_t)(brace - p));
        int lg = close < e && has_logical(brace, close), hx = close < e && has_hex_alpha(brace, close);
        if (lg || hx) {
            ob_put(o, "{", 1);
            obuf t = { 0, 0, 0 };
            if (lg) logical_block(brace + 1, close, &t); else ob_put(&t, brace + 1, (size_t)(close - brace - 1));
            if (hx && t.s) hex_alpha(t.s, t.s + t.n, o); else if (t.s) ob_put(o, t.s, t.n);
            free(t.s);
            ob_put(o, "}", 1);
        } else ob_put(o, brace, (size_t)((close < e ? close + 1 : e) - brace));
        p = close < e ? close + 1 : e;
    }
}

static char *rewrite_is(const char *css, uint32_t len, uint32_t *out_len)
{
    if (!memchr(css, ':', len)) return 0;
    obuf o = { 0, 0, 0 };
    rewrite_is_block(css, css + len, &o);
    *out_len = (uint32_t)o.n;
    return o.s;
}

static char *unwrap_supports(const char *css, uint32_t len, uint32_t *out_len)
{
    if (!memmem_(css, len, "@supports", 9) && !memmem_(css, len, "@layer", 6)) return 0;
    char *o = malloc(len + 1);
    if (!o) return 0;
    uint32_t n = 0;
    const char *p = css, *e = css + len;
    while (p < e) {
        if (*p == '@' && e - p > 6 && !strncasecmp(p, "@layer", 6) && (p[6] == ' ' || p[6] == '{' || p[6] == '\n')) {
            /* cascade layers: "@layer a, b;" says nothing tFire needs; "@layer a { rules }" is its rules (the
             * layers' order among themselves is not kept) */
            const char *brace = skip_to(p + 6, e, '{'), *semi = skip_to(p + 6, brace, ';');
            if (semi < brace) { p = semi + 1; continue; }
            if (brace >= e) break;
            const char *close = skip_to(brace + 1, e, '}');
            uint32_t il;
            char *inner = unwrap_supports(brace + 1, (uint32_t)(close - brace - 1), &il);
            if (inner) { memcpy(o + n, inner, il); n += il; free(inner); }
            else { memcpy(o + n, brace + 1, (size_t)(close - brace - 1)); n += (uint32_t)(close - brace - 1); }
            p = close < e ? close + 1 : e;
            continue;
        }
        if (*p == '@' && e - p > 9 && !strncasecmp(p, "@supports", 9)) {
            const char *brace = skip_to(p + 9, e, '{');
            if (brace >= e) break;
            const char *close = skip_to(brace + 1, e, '}');
            if (supports_ok(p + 9, brace)) {
                /* the inside, unwrapped (nested @supports in it too) */
                uint32_t il;
                char *inner = unwrap_supports(brace + 1, (uint32_t)(close - brace - 1), &il);
                if (inner) { memcpy(o + n, inner, il); n += il; free(inner); }
                else { memcpy(o + n, brace + 1, (size_t)(close - brace - 1)); n += (uint32_t)(close - brace - 1); }
            }
            p = close < e ? close + 1 : e;
            continue;
        }
        if (*p == '/' && p + 1 < e && p[1] == '*') {
            const char *q = p + 2;
            while (q + 1 < e && !(q[0] == '*' && q[1] == '/')) q++;
            q = q + 2 <= e ? q + 2 : e;
            memcpy(o + n, p, (size_t)(q - p));
            n += (uint32_t)(q - p);
            p = q;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char qc = *p;
            const char *q = p + 1;
            while (q < e && *q != qc) { if (*q == '\\') q++; q++; }
            q = q < e ? q + 1 : e;
            memcpy(o + n, p, (size_t)(q - p));
            n += (uint32_t)(q - p);
            p = q;
            continue;
        }
        o[n++] = *p++;
    }
    o[n] = 0;
    *out_len = n;
    return o;
}


/* ------------------------------------------------------------------ custom properties and calc(), which libcss does not know */

static css_var *var_find(tf_styler *st, const char *name, size_t nl)
{
    for (int i = st->nvars - 1; i >= 0; i--)
        if (strlen(st->vars[i].name) == nl && !memcmp(st->vars[i].name, name, nl)) return &st->vars[i];
    return 0;
}

static void var_set(tf_styler *st, const char *name, size_t nl, const char *val, size_t vl)
{
    while (vl && css_ws(val[vl - 1])) vl--;
    while (vl && css_ws(*val)) { val++; vl--; }
    if (vl > 10 && !strncmp(val + vl - 10, "!important", 10)) { vl -= 10; while (vl && css_ws(val[vl - 1])) vl--; }
    css_var *v = var_find(st, name, nl);
    if (v) { free(v->value); v->value = strndup_(val, vl); return; }
    if (st->nvars == st->vars_cap) {
        int nc = st->vars_cap ? st->vars_cap * 2 : 128;
        css_var *nv = realloc(st->vars, (size_t)nc * sizeof(css_var));
        if (!nv) return;
        st->vars = nv;
        st->vars_cap = nc;
    }
    st->vars[st->nvars].name = strndup_(name, nl);
    st->vars[st->nvars].value = strndup_(val, vl);
    st->nvars++;
}

/* one compound of a selector ("html.a.b", ":root", "body.c") that the page's <html> or <body> matches */
static int root_compound(tf_styler *st, const char *p, const char *e)
{
    while (p < e && css_ws(*p)) p++;
    while (e > p && css_ws(e[-1])) e--;
    if (p >= e) return 0;
    const char *q = p;
    if (e - q >= 5 && !strncasecmp(q, ":root", 5)) q += 5;
    else if (e - q >= 4 && !strncasecmp(q, "html", 4)) q += 4;
    else if (e - q >= 4 && !strncasecmp(q, "body", 4)) q += 4;
    else if (*q == '*') q++;
    else if (*q != '.' && *q != '[') return 0;
    while (q < e) {
        if (*q == '.') {
            const char *c = ++q;
            while (q < e && *q != '.' && *q != ':' && *q != '[') q++;
            /* the class must be on <html> or <body> */
            size_t l = (size_t)(q - c);
            int found = 0;
            for (const char *r = st->root_classes; *r && !found;) {
                while (*r == ' ') r++;
                const char *rs = r;
                while (*r && *r != ' ') r++;
                if ((size_t)(r - rs) == l && !memcmp(rs, c, l)) found = 1;
            }
            if (!found) return 0;
        } else if (*q == ':' && e - q >= 5 && !strncasecmp(q, ":root", 5)) q += 5;
        else if (*q == '[') {
            /* [name], [name=value], [name="value"]: an attribute <html> or <body> has */
            const char *a = ++q;
            while (q < e && *q != ']' && *q != '=') q++;
            size_t al = (size_t)(q - a);
            char want[128] = "";
            int has_val = 0;
            if (q < e && *q == '=') {
                q++;
                char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
                int k = 0;
                while (q < e && (quote ? *q != quote : *q != ']') && k < 127) want[k++] = *q++;
                want[k] = 0;
                has_val = 1;
                if (quote && q < e) q++;
            }
            while (q < e && *q != ']') q++;
            if (q < e) q++;
            int found = 0;
            for (const char *r = st->root_attrs; *r && !found;) {
                const char *eq = strchr(r, '='), *nl = strchr(r, '\n');
                if (!eq || !nl) break;
                if ((size_t)(eq - r) == al && !strncasecmp(r, a, al) &&
                    (!has_val || ((size_t)(nl - eq - 1) == strlen(want) && !strncmp(eq + 1, want, strlen(want))))) found = 1;
                r = nl + 1;
            }
            if (!found) return 0;
        }
        else return 0;                                  /* anything else (descendants, pseudo-classes): not the root */
    }
    return 1;
}

/* a rule for the whole page: where custom properties are set */
static int root_selector(tf_styler *st, const char *p, const char *e)
{
    while (p < e) {
        const char *comma = skip_to(p, e, ',');
        if (root_compound(st, p, comma)) return 1;
        p = comma + 1;
    }
    return 0;
}

void style_set_root_attrs(tf_styler *s, const char *attrs)
{
    strncpy(s->root_attrs, attrs ? attrs : "", sizeof(s->root_attrs) - 1);
    s->root_attrs[sizeof(s->root_attrs) - 1] = 0;
}

void style_set_root_classes(tf_styler *s, const char *classes)
{
    strncpy(s->root_classes, classes ? classes : "", sizeof(s->root_classes) - 1);
    s->root_classes[sizeof(s->root_classes) - 1] = 0;
}

static void vars_collect(tf_styler *st, const char *p, const char *e)
{
    for (;;) {
        p = skip_ws_comments(p, e);
        if (p >= e) return;
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *a = p + 1, *ae = a;
            while (ae < e && (*ae == '-' || (*ae >= 'a' && *ae <= 'z') || (*ae >= 'A' && *ae <= 'Z'))) ae++;
            const char *brace = skip_to(ae, e, '{'), *semi = skip_to(ae, brace, ';');
            if (semi < brace) { p = semi + 1; continue; }
            if (brace >= e) return;
            const char *close = skip_to(brace + 1, e, '}');
            int mn = 0, mx = 1 << 30;
            char cond[200];
            int cl = (int)(brace - ae) < 199 ? (int)(brace - ae) : 199;
            for (int i = 0; i < cl; i++) cond[i] = (char)tolower((unsigned char)ae[i]);
            cond[cl] = 0;
            if (((ae - a == 5 && !strncasecmp(a, "media", 5)) && media_query(ae, brace, &mn, &mx) && !strstr(cond, "dark") && mn <= 1024) ||
                (ae - a == 5 && !strncasecmp(a, "layer", 5)))
                vars_collect(st, brace + 1, close);
            p = close + 1;
            continue;
        }
        const char *brace = skip_to(p, e, '{');
        if (brace >= e) return;
        const char *close = skip_to(brace + 1, e, '}');
        int root = root_selector(st, p, brace);
        char selq[256];
        int sl = (int)(brace - p) < 255 ? (int)(brace - p) : 255;
        for (int i = 0; i < sl; i++) selq[i] = (char)tolower((unsigned char)p[i]);
        selq[sl] = 0;
        int themed = strstr(selq, "dark") || strstr(selq, "night") || strstr(selq, "high_contrast") || strstr(selq, "high-contrast") ||
                     strstr(selq, "colorblind") || strstr(selq, "tritanopia") || strstr(selq, "dimmed");
        if (root || !themed)
            for (const char *d = brace + 1; d < close;) {
                d = skip_ws_comments(d, close);
                const char *colon = skip_to(d, close, ':'), *end = skip_to(d, close, ';');
                if (colon < end && d + 2 < colon && d[0] == '-' && d[1] == '-') {
                    int nl = (int)(colon - d);
                    while (nl && css_ws(d[nl - 1])) nl--;
                    /* the page-wide value; a component's own (".card { --x: ... }") only stands in for one the page
                     * does not set at all */
                    if (root) var_set(st, d, (size_t)nl, colon + 1, (size_t)(end - colon - 1));
                    else if (!var_find(st, d, (size_t)nl)) var_set(st, d, (size_t)nl, colon + 1, (size_t)(end - colon - 1));
                }
                d = end + 1;
            }
        p = close + 1;
    }
}

static void subst(tf_styler *st, const char *p, const char *e, obuf *o, int depth);

static int name_cmp(const char *a, size_t al, const char *b)
{
    size_t bl = strlen(b);
    int c = memcmp(a, b, al < bl ? al : bl);
    return c ? c : (al < bl ? -1 : al > bl ? 1 : 0);
}

static css_var *env_find(venv *env, const char *name, size_t nl)
{
    int lo = 0, hi = env->nflat - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = name_cmp(name, nl, env->flat[mid].name);
        if (!c) return &env->flat[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

/* var(--name, fallback) at p ("var(" already seen at p): its value into o; returns where it ends */
static const char *do_var(tf_styler *st, const char *p, const char *e, obuf *o, int depth)
{
    const char *a = p + 4;
    const char *close = skip_to(a, e, ')');
    while (a < close && css_ws(*a)) a++;
    const char *ne = a;
    while (ne < close && *ne != ',' && !css_ws(*ne)) ne++;
    const char *comma = ne;
    while (comma < close && *comma != ',') comma++;
    css_var *v = cur_env ? env_find(cur_env, a, (size_t)(ne - a)) : 0;
    if (!v) v = var_find(st, a, (size_t)(ne - a));
    if (v && depth < 8) subst(st, v->value, v->value + strlen(v->value), o, depth + 1);
    else if (comma < close) subst(st, comma + 1, close, o, depth + 1);
    else ob_put(o, "initial", 7);
    return close < e ? close + 1 : e;
}

/* one calc() term in px (rem, em as 16 px); 0 = not a length this can do */
static int calc_term(const char *p, const char *e, double *px, int *pct)
{
    while (p < e && css_ws(*p)) p++;
    while (e > p && css_ws(e[-1])) e--;
    if (p >= e) return 0;
    char num[48];
    int n = 0;
    while (p < e && n < 47 && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+')) num[n++] = *p++;
    num[n] = 0;
    if (!n) return 0;
    double v = strtod(num, 0);
    size_t ul = (size_t)(e - p);
    *pct = 0;
    if (!ul || (ul == 2 && !strncmp(p, "px", 2))) *px = v;
    else if (ul == 3 && !strncmp(p, "rem", 3)) *px = v * 16;
    else if (ul == 2 && !strncmp(p, "em", 2)) *px = v * 16;
    else if (ul == 1 && *p == '%') { *px = v; *pct = 1; }
    else if (ul == 2 && !strncmp(p, "vw", 2)) *px = v * 10.24;
    else if (ul == 2 && !strncmp(p, "vh", 2)) *px = v * 7.68;
    else return 0;
    return 1;
}

/* calc(a + b - c), max(a, b), min(a, b) over lengths -> "Npx" (or "N%" when it is a percentage alone) */
static int calc_eval(const char *fn, const char *p, const char *e, char *out)
{
    double acc = 0;
    int have = 0, pct_only = 1, any_pct = 0;
    if (!strcmp(fn, "max") || !strcmp(fn, "min") || !strcmp(fn, "clamp")) {
        double best = 0;
        int first = 1;
        const char *a = p;
        int k = 0;
        while (a < e) {
            const char *c = skip_to(a, e, ',');
            char tmp[64];
            double v;
            if (!calc_eval("calc", a, c, tmp)) return 0;
            v = strtod(tmp, 0);
            if (!strcmp(fn, "clamp")) { if (k == 1) best = v; }
            else if (first || (!strcmp(fn, "max") ? v > best : v < best)) best = v;
            first = 0;
            k++;
            a = c + 1;
        }
        snprintf(out, 64, "%dpx", (int)(best + (best < 0 ? -0.5 : 0.5)));
        return 1;
    }
    /* a + b - c: terms separated by + or - with spaces around them */
    const char *t = p;
    int sign = 1;
    for (const char *q = p; q <= e; q++) {
        int end = q == e;
        if (!end && !((*q == '+' || *q == '-') && q > p && css_ws(q[-1]) && q + 1 < e && css_ws(q[1]))) {
            if (*q == '(') { q = skip_to(q + 1, e, ')'); }
            continue;
        }
        double v;
        int pc;
        char inner[64];
        const char *ts = t;
        while (ts < q && css_ws(*ts)) ts++;
        if (q - ts > 5 && !strncmp(ts, "calc(", 5)) {
            const char *ie = skip_to(ts + 5, q, ')');
            if (!calc_eval("calc", ts + 5, ie, inner)) return 0;
            v = strtod(inner, 0);
            pc = strchr(inner, '%') != 0;
        } else if (q - ts > 1 && *ts == '(') {
            const char *ie = skip_to(ts + 1, q, ')');
            if (!calc_eval("calc", ts + 1, ie, inner)) return 0;
            v = strtod(inner, 0);
            pc = strchr(inner, '%') != 0;
        } else {
            /* a product or quotient with a plain number: 2 * 8px, 100% / 3 */
            const char *star = 0, *slash = 0;
            for (const char *z = ts; z < q; z++) { if (*z == '*') star = z; if (*z == '/') slash = z; }
            if (star || slash) {
                const char *op = star ? star : slash;
                double a1, b1;
                int pa, pb;
                if (!calc_term(ts, op, &a1, &pa) || !calc_term(op + 1, q, &b1, &pb)) return 0;
                if (star) { v = a1 * b1; pc = pa || pb; }
                else { if (b1 == 0) return 0; v = a1 / b1; pc = pa; }
            } else if (!calc_term(ts, q, &v, &pc)) return 0;
        }
        if (pc) any_pct = 1; else pct_only = 0;
        /* a percentage mixed with lengths cannot be worked out here: the percentage is kept */
        if (!any_pct || pc || !have) acc += sign * v;
        have = 1;
        if (!end) sign = *q == '-' ? -1 : 1;
        t = q + 1;
    }
    if (!have) return 0;
    if (any_pct && pct_only) snprintf(out, 64, "%d%%", (int)(acc + 0.5));
    else if (any_pct) {
        /* recompute: only the percentage terms */
        snprintf(out, 64, "%d%%", (int)(acc + 0.5));
    } else snprintf(out, 64, "%dpx", (int)(acc + (acc < 0 ? -0.5 : 0.5)));
    return 1;
}

static void subst(tf_styler *st, const char *p, const char *e, obuf *o, int depth)
{
    const char *run = p;
    while (p < e) {
        char c = *p;
        if (c == '"' || c == '\'') {
            const char *q = p + 1;
            while (q < e && *q != c) { if (*q == '\\') q++; q++; }
            p = q < e ? q + 1 : e;
            continue;
        }
        if (c == '/' && p + 1 < e && p[1] == '*') {
            ob_put(o, run, (size_t)(p - run));
            const char *q = p + 2;
            while (q + 1 < e && !(q[0] == '*' && q[1] == '/')) q++;
            p = run = q + 2 <= e ? q + 2 : e;
            continue;
        }
        if (c == 'v' && e - p > 4 && !strncmp(p, "var(", 4) && (p == run || !((p[-1] >= 'a' && p[-1] <= 'z') || p[-1] == '-'))) {
            ob_put(o, run, (size_t)(p - run));
            p = run = do_var(st, p, e, o, depth);
            continue;
        }
        const char *fn = 0;
        int fl = 0;
        if (e - p > 5 && !strncmp(p, "calc(", 5)) { fn = "calc"; fl = 5; }
        else if (e - p > 4 && !strncmp(p, "max(", 4)) { fn = "max"; fl = 4; }
        else if (e - p > 4 && !strncmp(p, "min(", 4)) { fn = "min"; fl = 4; }
        else if (e - p > 6 && !strncmp(p, "clamp(", 6)) { fn = "clamp"; fl = 6; }
        if (fn && (p == run || !((p[-1] >= 'a' && p[-1] <= 'z') || p[-1] == '-'))) {
            const char *close = skip_to(p + fl, e, ')');
            /* its inside with variables put in first */
            obuf in = { 0, 0, 0 };
            subst(st, p + fl, close, &in, depth + 1);
            char res[64];
            ob_put(o, run, (size_t)(p - run));
            if (in.s && calc_eval(fn, in.s, in.s + in.n, res)) ob_put(o, res, strlen(res));
            else { ob_put(o, p, (size_t)fl); if (in.s) ob_put(o, in.s, in.n); ob_put(o, ")", 1); }
            free(in.s);
            p = run = close < e ? close + 1 : e;
            continue;
        }
        p++;
    }
    ob_put(o, run, (size_t)(p - run));
}

/* the style sheet with custom properties and what calc() can work out put in */
static char *resolve_vars(tf_styler *st, const char *css, uint32_t len, uint32_t *out_len)
{
    vars_collect(st, css, css + len);
    if (!memmem_(css, len, "var(", 4) && !memmem_(css, len, "calc(", 5) && !memmem_(css, len, "max(", 4) && !memmem_(css, len, "min(", 4)) return 0;
    obuf o = { 0, 0, 0 };
    subst(st, css, css + len, &o, 0);
    *out_len = (uint32_t)o.n;
    return o.s;
}


/* ------------------------------------------------------------------ custom properties of some elements only */

/* a selector the matcher (dom_matches) can do and tFire can tell: no states (:hover...), no pseudo-elements */
static int scoped_sel_ok(const char *sel, int sl)
{
    for (int i = 0; i < sl; i++) {
        if (sel[i] != ':') continue;
        if (sl - i >= 5 && !strncmp(sel + i, ":root", 5)) continue;
        if (sl - i >= 12 && !strncmp(sel + i, ":first-child", 12)) continue;
        if (sl - i >= 11 && !strncmp(sel + i, ":last-child", 11)) continue;
        return 0;
    }
    return 1;
}

static void scoped_add(tf_styler *st, const char *sel, int sl, const char *body, const char *be, int minw, int maxw)
{
    while (sl && css_ws(sel[sl - 1])) sl--;
    while (sl && css_ws(*sel)) { sel++; sl--; }
    if (sl <= 0 || !scoped_sel_ok(sel, sl) || root_compound(st, sel, sel + sl)) return;
    if (st->nscoped == st->scoped_cap) {
        int nc = st->scoped_cap ? st->scoped_cap * 2 : 256;
        scoped_rule *nr = realloc(st->scoped, (size_t)nc * sizeof(*nr));
        if (!nr) return;
        st->scoped = nr;
        st->scoped_cap = nc;
    }
    scoped_rule *r = &st->scoped[st->nscoped];
    memset(r, 0, sizeof(*r));
    r->sel = strndup_(sel, (size_t)sl);
    if (!r->sel) return;
    int cap = 0;
    for (const char *d = body; d < be;) {
        d = skip_ws_comments(d, be);
        const char *colon = skip_to(d, be, ':'), *end = skip_to(d, be, ';');
        if (colon < end && d + 2 < colon && d[0] == '-' && d[1] == '-') {
            int nl = (int)(colon - d);
            while (nl && css_ws(d[nl - 1])) nl--;
            const char *v = colon + 1;
            size_t vl = (size_t)(end - v);
            while (vl && css_ws(v[vl - 1])) vl--;
            while (vl && css_ws(*v)) { v++; vl--; }
            if (vl > 10 && !strncmp(v + vl - 10, "!important", 10)) { vl -= 10; while (vl && css_ws(v[vl - 1])) vl--; }
            if (r->ndefs == cap) {
                cap = cap ? cap * 2 : 8;
                css_var *nd = realloc(r->defs, (size_t)cap * sizeof(css_var));
                if (!nd) break;
                r->defs = nd;
            }
            r->defs[r->ndefs].name = strndup_(d, (size_t)nl);
            r->defs[r->ndefs].value = strndup_(v, vl);
            r->ndefs++;
        }
        d = end + 1;
    }
    if (!r->ndefs) { free(r->sel); free(r->defs); return; }
    r->minw = minw;
    r->maxw = maxw;
    r->spec = specificity(r->sel);
    r->order = st->nscoped;
    /* the last compound's id, class, attribute or tag: a quick test before the whole selector */
    const char *last = r->sel + strlen(r->sel), *c = last;
    while (c > r->sel && c[-1] != ' ' && c[-1] != '>' && c[-1] != '+' && c[-1] != '~') c--;
    const char *k = 0;
    for (const char *q = c; q < last; q++) if (*q == '#') { k = q; r->keykind = 2; break; }
    if (!k) for (const char *q = c; q < last; q++) if (*q == '.') { k = q; r->keykind = 1; break; }
    if (k) {
        k++;
        const char *ke = k;
        while (ke < last && *ke != '.' && *ke != '#' && *ke != '[' && *ke != ':') ke++;
        r->key = strndup_(k, (size_t)(ke - k));
    } else if (*c == '[') {
        const char *ke = c + 1;
        while (ke < last && *ke != ']' && *ke != '=' && *ke != '~' && *ke != '^' && *ke != '$' && *ke != '*' && *ke != '|') ke++;
        r->key = strndup_(c + 1, (size_t)(ke - c - 1));
        for (char *z = r->key; z && *z; z++) *z = (char)tolower((unsigned char)*z);
        r->keykind = 4;
    } else if (c < last && *c >= 'a' && *c <= 'z') {
        const char *ke = c;
        while (ke < last && ((*ke >= 'a' && *ke <= 'z') || (*ke >= '0' && *ke <= '9'))) ke++;
        r->key = strndup_(c, (size_t)(ke - c));
        r->keykind = 3;
    }
    st->nscoped++;
}

static void vrule_add(tf_styler *st, const char *p, const char *close, int minw, int maxw)
{
    if (st->nvrules == st->vrules_cap) {
        int nc = st->vrules_cap ? st->vrules_cap * 2 : 512;
        var_rule *nr = realloc(st->vrules, (size_t)nc * sizeof(*nr));
        if (!nr) return;
        st->vrules = nr;
        st->vrules_cap = nc;
    }
    obuf u = { 0, 0, 0 };
    ob_put(&u, "\n", 1);
    for (const char *q = p; q + 4 < close; q++) {
        if (strncmp(q, "var(", 4)) continue;
        const char *a = q + 4;
        while (a < close && css_ws(*a)) a++;
        const char *ne = a;
        while (ne < close && *ne != ',' && *ne != ')' && !css_ws(*ne)) ne++;
        if (ne > a) { ob_put(&u, a, (size_t)(ne - a)); ob_put(&u, "\n", 1); }
    }
    /* the selector and only the declarations with var() in them (the others are in the page's sheet already, and
     * written again later they would win over rules that come after them) */
    const char *brace = skip_to(p, close, '{');
    obuf t = { 0, 0, 0 };
    ob_put(&t, p, (size_t)(brace + 1 - p));
    for (const char *d = brace + 1; d < close;) {
        const char *end = skip_to(d, close, ';');
        const char *ds = skip_ws_comments(d, end);
        if (memmem_(d, (size_t)(end - d), "var(", 4) && !(end - ds > 2 && ds[0] == '-' && ds[1] == '-')) { ob_put(&t, d, (size_t)(end - d)); ob_put(&t, ";", 1); }
        d = end + 1;
    }
    ob_put(&t, "}", 1);
    var_rule *r = &st->vrules[st->nvrules];
    r->text = t.s;
    r->uses = u.s;
    r->minw = minw;
    r->maxw = maxw;
    if (r->text && r->uses) st->nvrules++;
    else { free(r->text); free(r->uses); }
}

static void scoped_collect(tf_styler *st, const char *p, const char *e, int minw, int maxw)
{
    for (;;) {
        p = skip_ws_comments(p, e);
        if (p >= e) return;
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *a = p + 1, *ae = a;
            while (ae < e && (*ae == '-' || (*ae >= 'a' && *ae <= 'z') || (*ae >= 'A' && *ae <= 'Z'))) ae++;
            const char *brace = skip_to(ae, e, '{'), *semi = skip_to(ae, brace, ';');
            if (semi < brace) { p = semi + 1; continue; }
            if (brace >= e) return;
            const char *close = skip_to(brace + 1, e, '}');
            int mn = minw, mx = maxw;
            if (ae - a == 5 && !strncasecmp(a, "media", 5) && media_query(ae, brace, &mn, &mx)) scoped_collect(st, brace + 1, close, mn, mx);
            p = close + 1;
            continue;
        }
        const char *brace = skip_to(p, e, '{');
        if (brace >= e) return;
        const char *close = skip_to(brace + 1, e, '}');
        if (memmem_(brace, (size_t)(close - brace), "--", 2)) {
            const char *sel = p;
            while (sel < brace) {
                const char *comma = skip_to(sel, brace, ',');
                scoped_add(st, sel, (int)(comma - sel), brace + 1, close, minw, maxw);
                sel = comma + 1;
            }
        }
        if (memmem_(brace, (size_t)(close - brace), "var(", 4)) vrule_add(st, p, close, minw, maxw);
        p = close + 1;
    }
}

/* a set of variable names (the dirty ones of a variant): open addressing on a hash of the name */
typedef struct { const char **slot; uint32_t mask; int n; } name_set;

static uint32_t name_hash(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h;
}

static int set_has(const name_set *s, const char *p, size_t n)
{
    for (uint32_t i = name_hash(p, n) & s->mask;; i = (i + 1) & s->mask) {
        const char *k = s->slot[i];
        if (!k) return 0;
        if (strlen(k) == n && !memcmp(k, p, n)) return 1;
    }
}

static int set_add(name_set *s, const char *name)
{
    if ((uint32_t)(s->n + 1) * 2 > s->mask + 1) {
        uint32_t nm = s->mask ? s->mask * 2 + 1 : 255;
        const char **ns = calloc(nm + 1, sizeof(char *));
        if (!ns) return 0;
        for (uint32_t i = 0; s->mask && i <= s->mask; i++) {
            if (!s->slot[i]) continue;
            uint32_t j = name_hash(s->slot[i], strlen(s->slot[i])) & nm;
            while (ns[j]) j = (j + 1) & nm;
            ns[j] = s->slot[i];
        }
        free(s->slot);
        s->slot = ns;
        s->mask = nm;
    }
    size_t n = strlen(name);
    uint32_t i = name_hash(name, n) & s->mask;
    while (s->slot[i]) { if (!strcmp(s->slot[i], name)) return 0; i = (i + 1) & s->mask; }
    s->slot[i] = name;
    s->n++;
    return 1;
}

/* does a "\n--a\n--b\n" list name any of the set? */
static int uses_any(const char *uses, const name_set *set)
{
    for (const char *u = uses; *u;) {
        while (*u == '\n') u++;
        const char *ue = u;
        while (*ue && *ue != '\n') ue++;
        if (ue > u && set_has(set, u, (size_t)(ue - u))) return 1;
        u = ue;
    }
    return 0;
}

/* does a value name (in var()) any of the set? */
static int value_uses_any(const char *val, const name_set *set)
{
    for (const char *q = val; (q = strstr(q, "var(")); q += 4) {
        const char *a = q + 4, *ne = a;
        while (css_ws(*a)) a++;
        ne = a;
        while (*ne && *ne != ',' && *ne != ')' && !css_ws(*ne)) ne++;
        if (ne > a && set_has(set, a, (size_t)(ne - a))) return 1;
    }
    return 0;
}

static void variants_clear(tf_styler *st)
{
    for (int i = 0; i < st->nvariants; i++) {
        var_variant *v = st->variants[i];
        if (v->ctx) css_select_ctx_destroy(v->ctx);
        if (v->sheet) css_stylesheet_destroy(v->sheet);
        free(v->key);
        free(v);
    }
    free(st->variants);
    st->variants = 0;
    st->nvariants = 0;
}

/* the variant for env: the rules whose values its variables change, written with them */
static var_variant *variant_for(tf_styler *st, venv *env)
{
    if (st->variants_gen != st->gen) { variants_clear(st); st->variants_gen = st->gen; }
    obuf k = { 0, 0, 0 };
    for (int i = 0; i < env->nflat; i++) {
        ob_put(&k, env->flat[i].name, strlen(env->flat[i].name));
        ob_put(&k, ":", 1);
        ob_put(&k, env->flat[i].value, strlen(env->flat[i].value));
        ob_put(&k, ";", 1);
    }
    if (!k.s) return 0;
    for (int i = 0; i < st->nvariants; i++) if (!strcmp(st->variants[i]->key, k.s)) { free(k.s); return st->variants[i]; }
    var_variant *v = calloc(1, sizeof(*v));
    var_variant **nv = realloc(st->variants, (size_t)(st->nvariants + 1) * sizeof(*nv));
    if (!v || !nv) { free(v); free(k.s); if (nv) st->variants = nv; return 0; }
    st->variants = nv;
    st->variants[st->nvariants++] = v;
    v->key = k.s;
    /* the names whose values differ: env's own, and the page's variables made of them */
    name_set dirty = { 0, 0, 0 };
    for (int i = 0; i < env->nflat; i++) set_add(&dirty, env->flat[i].name);
    for (int changed = 1, round = 0; changed && round < 6 && dirty.slot; round++) {
        changed = 0;
        for (int i = 0; i < st->nvars; i++) {
            const char *val = st->vars[i].value;
            if (!strstr(val, "var(") || set_has(&dirty, st->vars[i].name, strlen(st->vars[i].name))) continue;
            if (value_uses_any(val, &dirty) && set_add(&dirty, st->vars[i].name)) changed = 1;
        }
    }
    if (!dirty.slot) return v;
    obuf css = { 0, 0, 0 };
    venv *was = cur_env;
    cur_env = env;
    for (int i = 0; i < st->nvrules; i++) {
        var_rule *r = &st->vrules[i];
        if (!uses_any(r->uses, &dirty)) continue;
        char mq[96];
        int wrap = r->minw > 0 || r->maxw < (1 << 30);
        if (wrap) {
            if (r->maxw < (1 << 30)) snprintf(mq, sizeof(mq), "@media (min-width:%dpx) and (max-width:%dpx){", r->minw, r->maxw);
            else snprintf(mq, sizeof(mq), "@media (min-width:%dpx){", r->minw);
            ob_put(&css, mq, strlen(mq));
        }
        subst(st, r->text, r->text + strlen(r->text), &css, 0);
        if (wrap) ob_put(&css, "}", 1);
        ob_put(&css, "\n", 1);
    }
    cur_env = was;
    free(dirty.slot);
    if (!css.s) return v;
    v->sheet = make_sheet(css.s, css.n, "tfire:vars", 0);
    free(css.s);
    if (!v->sheet || css_select_ctx_create(&v->ctx) != CSS_OK) { v->ctx = 0; return v; }
    if (st->ua) css_select_ctx_append_sheet(v->ctx, st->ua, CSS_ORIGIN_UA, NULL);
    for (int i = 0; i < st->nsheets; i++) css_select_ctx_append_sheet(v->ctx, st->sheets[i], CSS_ORIGIN_AUTHOR, NULL);
    css_select_ctx_append_sheet(v->ctx, v->sheet, CSS_ORIGIN_AUTHOR, NULL);
    return v;
}

static int has_class(tf_node *n, const char *c);

/* the variables in force at n: its parent's, with what the rules matching n and its style="" set */
static venv *env_for(tf_styler *st, tf_node *n, venv *parent)
{
    if (!st->nscoped && !strstr(dom_attr(n, "style") ? dom_attr(n, "style") : "", "--")) return parent;
    int vw = FIXTOINT(st->unit.viewport_width);
    /* the matching rules, in cascade order (specificity, then order) */
    int hits[64], nh = 0;
    for (int i = 0; i < st->nscoped && nh < 64; i++) {
        const scoped_rule *r = &st->scoped[i];
        if (vw < r->minw || vw > r->maxw) continue;
        if (r->keykind == 1 && !has_class(n, r->key)) continue;
        if (r->keykind == 2 && (!n->id || strcmp(lwc_string_data(n->id), r->key))) continue;
        if (r->keykind == 3 && !dom_is(n, r->key)) continue;
        if (r->keykind == 4 && !dom_attr(n, r->key)) continue;
        if (!dom_matches(n, r->sel)) continue;
        int j = nh++;
        while (j > 0) {
            const scoped_rule *b = &st->scoped[hits[j - 1]];
            if (b->spec < r->spec || (b->spec == r->spec && b->order < r->order)) break;
            hits[j] = hits[j - 1];
            j--;
        }
        hits[j] = i;
    }
    const char *inl = dom_attr(n, "style");
    int inl_has = inl && strstr(inl, "--");
    if (!nh && !inl_has) return parent;
    /* the new values, later ones over earlier ones */
    css_var *own = 0;
    int nown = 0, cap = 0;
    char **made = 0;
    int nmade = 0;
    #define OWN_SET(NM, VL) do { \
        int f_ = -1; for (int q_ = 0; q_ < nown; q_++) if (!strcmp(own[q_].name, (NM))) { f_ = q_; break; } \
        if (f_ < 0) { if (nown == cap) { cap = cap ? cap * 2 : 16; css_var *z_ = realloc(own, (size_t)cap * sizeof(css_var)); if (!z_) break; own = z_; } f_ = nown++; own[f_].name = (NM); } \
        own[f_].value = (VL); } while (0)
    for (int h = 0; h < nh; h++) {
        const scoped_rule *r = &st->scoped[hits[h]];
        for (int d = 0; d < r->ndefs; d++) OWN_SET(r->defs[d].name, r->defs[d].value);
    }
    if (inl_has) {
        made = malloc(64 * sizeof(char *));
        for (const char *d = inl, *e = inl + strlen(inl); made && d < e && nmade < 62;) {
            d = skip_ws_comments(d, e);
            const char *colon = skip_to(d, e, ':'), *end = skip_to(d, e, ';');
            if (colon < end && d + 2 < colon && d[0] == '-' && d[1] == '-') {
                int nl = (int)(colon - d);
                while (nl && css_ws(d[nl - 1])) nl--;
                const char *v = colon + 1;
                size_t vl = (size_t)(end - v);
                while (vl && css_ws(v[vl - 1])) vl--;
                while (vl && css_ws(*v)) { v++; vl--; }
                char *nm = strndup_(d, (size_t)nl), *vv = strndup_(v, vl);
                if (nm && vv) { made[nmade++] = nm; made[nmade++] = vv; OWN_SET(nm, vv); }
                else { free(nm); free(vv); }
            }
            d = end + 1;
        }
    }
    #undef OWN_SET
    /* merged with the parent's: sorted, and only where it differs from the page's own value */
    int pn = parent ? parent->nflat : 0;
    css_var *flat = malloc((size_t)(pn + nown + 1) * sizeof(css_var));
    int nf = 0, changed = 0;
    if (flat) {
        for (int i = 0; i < pn; i++) flat[nf++] = parent->flat[i];
        for (int i = 0; i < nown; i++) {
            css_var *g = var_find(st, own[i].name, strlen(own[i].name));
            css_var *pf = parent ? env_find(parent, own[i].name, strlen(own[i].name)) : 0;
            const char *before = pf ? pf->value : g ? g->value : 0;
            if (before && !strcmp(before, own[i].value)) continue;      /* no change */
            changed = 1;
            int k = 0;
            for (; k < nf; k++) if (!strcmp(flat[k].name, own[i].name)) break;
            if (k < nf) {
                if (g && !strcmp(g->value, own[i].value)) { memmove(flat + k, flat + k + 1, (size_t)(nf - k - 1) * sizeof(css_var)); nf--; }
                else flat[k].value = own[i].value;
                continue;
            }
            if (g && !strcmp(g->value, own[i].value)) continue;
            int at = 0;
            while (at < nf && strcmp(flat[at].name, own[i].name) < 0) at++;
            memmove(flat + at + 1, flat + at, (size_t)(nf - at) * sizeof(css_var));
            flat[at] = own[i];
            nf++;
        }
    }
    free(own);
    if (!flat || !changed) {
        free(flat);
        for (int i = 0; i < nmade; i++) free(made[i]);
        free(made);
        return parent;
    }
    venv *env = calloc(1, sizeof(*env));
    if (!env || (st->nenvs == st->envs_cap && !(st->envs_cap = st->envs_cap ? st->envs_cap * 2 : 64,
                                                (st->envs = realloc(st->envs, (size_t)st->envs_cap * sizeof(venv *)))))) {
        free(env); free(flat);
        for (int i = 0; i < nmade; i++) free(made[i]);
        free(made);
        return parent;
    }
    st->envs[st->nenvs++] = env;
    env->flat = flat;
    env->nflat = nf;
    env->own = made;
    env->nown = nmade;
    if (nf) env->variant = variant_for(st, env);
    return env;
}

static void envs_clear(tf_styler *st)
{
    for (int i = 0; i < st->nenvs; i++) {
        venv *e = st->envs[i];
        free(e->flat);
        for (int j = 0; j < e->nown; j++) free(e->own[j]);
        free(e->own);
        free(e);
    }
    st->nenvs = 0;
}

/* for each :hover in the sheet, what its compound names, so a pointer move over nothing it names costs no restyle */
static void hover_keys(tf_styler *st, const char *css, const char *e)
{
    for (const char *h = css; (h = memmem_(h, (size_t)(e - h), ":hover", 6)); h += 6) {
        const char *c = h;
        while (c > css && !css_ws(c[-1]) && c[-1] != '>' && c[-1] != '+' && c[-1] != '~' && c[-1] != ',' && c[-1] != '{' && c[-1] != '}' && c[-1] != '(') c--;
        char key[80];
        const char *k = 0;
        for (const char *q = c; q < h; q++) if (*q == '#') { k = q; break; }
        if (!k) for (const char *q = c; q < h; q++) if (*q == '.') { k = q; break; }
        if (!k && c < h && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z'))) k = c;
        if (k) {
            const char *ke = k + 1;
            while (ke < h && *ke != '.' && *ke != '#' && *ke != '[' && *ke != ':') ke++;
            size_t l = (size_t)(ke - k) < sizeof(key) - 1 ? (size_t)(ke - k) : sizeof(key) - 1;
            memcpy(key, k, l);
            key[l] = 0;
        } else strcpy(key, "*");
        int have = 0;
        for (int i = 0; i < st->nhover_keys && !have; i++) have = !strcmp(st->hover_keys[i], key);
        if (have || st->nhover_keys >= 2048) continue;
        char **nk = realloc(st->hover_keys, (size_t)(st->nhover_keys + 1) * sizeof(char *));
        if (!nk) return;
        st->hover_keys = nk;
        if ((nk[st->nhover_keys] = strdup(key))) st->nhover_keys++;
    }
}

/* does a rule with :hover name n or one of its ancestors? (then the pointer coming or going changes styles) */
int style_hover_matters(tf_styler *s, tf_node *n)
{
    if (!s) return 0;
    for (; n; n = n->parent) {
        if (n->type != TF_ELEM) continue;
        for (int i = 0; i < s->nhover_keys; i++) {
            const char *k = s->hover_keys[i];
            if (k[0] == '*') return 1;
            if (k[0] == '.' ? has_class(n, k + 1) : k[0] == '#' ? (n->id && !strcmp(lwc_string_data(n->id), k + 1)) : dom_is(n, k)) return 1;
        }
    }
    return 0;
}

void style_set_state(tf_styler *s, tf_node *hover, tf_node *focus)
{
    if (!s) return;
    s->hover = hover;
    s->focus = focus;
}

void style_add_sheet(tf_styler *s, const char *css, uint32_t len, const char *base)
{
    uint32_t ul, vl;
    uint32_t rl;
    char *rr = rewrite_ranges(css, len, &rl);
    if (rr) { css = rr; len = rl; }
    char *un = unwrap_supports(css, len, &ul);
    if (un) { css = un; len = ul; }
    uint32_t il;
    char *ir = rewrite_is(css, len, &il);
    if (ir) { css = ir; len = il; }
    char *vr = resolve_vars(s, css, len, &vl);
    scoped_collect(s, css, css + len, 0, 1 << 30);
    s->gen++;
    if (vr) { css = vr; len = vl; }
    side_block(s, css, css + len, 0, 1 << 30);
    hover_keys(s, css, css + len);
    css_stylesheet *sh = make_sheet(css, len, base, 0);
    free(rr);
    free(un);
    free(vr);
    free(ir);
    if (!sh) return;
    css_stylesheet **ns = realloc(s->sheets, (size_t)(s->nsheets + 1) * sizeof(*ns));
    if (!ns) { css_stylesheet_destroy(sh); return; }
    s->sheets = ns;
    s->sheets[s->nsheets++] = sh;
    css_select_ctx_append_sheet(s->ctx, sh, CSS_ORIGIN_AUTHOR, NULL);
}

void style_node_free(tf_node *n)
{
    if (n->sel) { css_select_results_destroy(n->sel); n->sel = 0; }
    if (n->inline_sheet) { css_stylesheet_destroy(n->inline_sheet); n->inline_sheet = 0; }
    if (n->css_data) { css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, 0, n, NULL, n->css_data); n->css_data = 0; }
    free(n->style);
    n->style = 0;
}

/* ------------------------------------------------------------------ computed style -> tf_style */

static int px(const css_computed_style *cs, const css_unit_ctx *u, css_fixed v, css_unit unit)
{
    if (unit == CSS_UNIT_PX) return FIXTOINT(v);
    return FIXTOINT(css_unit_len2css_px(cs, u, v, unit));
}

static void length(const css_computed_style *cs, const css_unit_ctx *u, uint8_t type, int set, int autov,
                   css_fixed v, css_unit unit, tf_len *out)
{
    out->v = 0; out->pct = 0; out->autov = 0;
    if (type == autov || type != set) { out->autov = 1; return; }
    if (unit == CSS_UNIT_PCT) { out->pct = 1; out->v = FIXTOINT(FMUL(v, INTTOFIX(100))); return; }
    out->v = px(cs, u, v, unit);
}

static uint32_t rgb(css_color c) { return c & 0xFFFFFF; }

/* a see-through colour as it looks over white (most pages' ground: tFire draws no blending) */
static uint32_t over_white(css_color c)
{
    uint32_t a = c >> 24;
    if (a >= 255) return c & 0xFFFFFF;
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r = (r * a + 255 * (255 - a)) / 255; g = (g * a + 255 * (255 - a)) / 255; b = (b * a + 255 * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

static void convert(const css_computed_style *cs, const css_unit_ctx *u, tf_style *t, int is_root)
{
    css_fixed v;
    css_unit unit;
    css_color c;
    memset(t, 0, sizeof(*t));

    switch (css_computed_display(cs, is_root)) {
    case CSS_DISPLAY_NONE: t->display = TD_NONE; break;
    case CSS_DISPLAY_BLOCK: case CSS_DISPLAY_RUN_IN: t->display = TD_BLOCK; break;
    case CSS_DISPLAY_LIST_ITEM: t->display = TD_LIST_ITEM; break;
    case CSS_DISPLAY_INLINE_BLOCK: case CSS_DISPLAY_INLINE_TABLE: t->display = TD_INLINE_BLOCK; break;
    case CSS_DISPLAY_TABLE: t->display = TD_TABLE; break;
    case CSS_DISPLAY_TABLE_ROW_GROUP: case CSS_DISPLAY_TABLE_HEADER_GROUP: case CSS_DISPLAY_TABLE_FOOTER_GROUP:
        t->display = TD_ROW_GROUP; break;
    case CSS_DISPLAY_TABLE_ROW: t->display = TD_ROW; break;
    case CSS_DISPLAY_TABLE_CELL: t->display = TD_CELL; break;
    case CSS_DISPLAY_TABLE_CAPTION: t->display = TD_CAPTION; break;
    case CSS_DISPLAY_TABLE_COLUMN: case CSS_DISPLAY_TABLE_COLUMN_GROUP: t->display = TD_NONE; break;
    case CSS_DISPLAY_FLEX: t->display = TD_FLEX; break;
    case CSS_DISPLAY_INLINE_FLEX: t->display = TD_INLINE_FLEX; break;
    case CSS_DISPLAY_GRID: case CSS_DISPLAY_INLINE_GRID: t->display = TD_GRID; break;
    default: t->display = TD_INLINE; break;
    }
    switch (css_computed_position(cs)) {
    case CSS_POSITION_RELATIVE: t->position = TP_RELATIVE; break;
    case CSS_POSITION_ABSOLUTE: t->position = TP_ABSOLUTE; break;
    case CSS_POSITION_FIXED: t->position = TP_FIXED; break;
    case CSS_POSITION_STICKY: t->position = TP_STICKY; break;
    default: t->position = TP_STATIC; break;
    }
    uint8_t fl = css_computed_float(cs);
    t->floated = fl == CSS_FLOAT_LEFT ? 1 : fl == CSS_FLOAT_RIGHT ? 2 : 0;
    switch (css_computed_white_space(cs)) {
    case CSS_WHITE_SPACE_PRE: t->white_space = TWS_PRE; break;
    case CSS_WHITE_SPACE_NOWRAP: t->white_space = TWS_NOWRAP; break;
    case CSS_WHITE_SPACE_PRE_WRAP: t->white_space = TWS_PRE_WRAP; break;
    case CSS_WHITE_SPACE_PRE_LINE: t->white_space = TWS_PRE_LINE; break;
    default: t->white_space = TWS_NORMAL; break;
    }
    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_RIGHT: case CSS_TEXT_ALIGN_LIBCSS_RIGHT: t->text_align = TA_RIGHT; break;
    case CSS_TEXT_ALIGN_CENTER: case CSS_TEXT_ALIGN_LIBCSS_CENTER: t->text_align = TA_CENTER; break;
    case CSS_TEXT_ALIGN_JUSTIFY: t->text_align = TA_JUSTIFY; break;
    default: t->text_align = TA_LEFT; break;
    }
    switch (css_computed_list_style_type(cs)) {
    case CSS_LIST_STYLE_TYPE_NONE: t->list_style = TL_NONE; break;
    case CSS_LIST_STYLE_TYPE_DISC: t->list_style = TL_DISC; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL: t->list_style = TL_DECIMAL; break;
    case CSS_LIST_STYLE_TYPE_CIRCLE: t->list_style = TL_CIRCLE; break;
    case CSS_LIST_STYLE_TYPE_SQUARE: t->list_style = TL_SQUARE; break;
    default: t->list_style = TL_DECIMAL; break;                /* roman, alpha, ...: numbers */
    }
    t->visible = css_computed_visibility(cs) != CSS_VISIBILITY_HIDDEN && css_computed_visibility(cs) != CSS_VISIBILITY_COLLAPSE;
    uint8_t fd = css_computed_flex_direction(cs);
    t->flex_row = fd != CSS_FLEX_DIRECTION_COLUMN && fd != CSS_FLEX_DIRECTION_COLUMN_REVERSE;
    t->flex_wrap = css_computed_flex_wrap(cs) != CSS_FLEX_WRAP_NOWRAP;

    uint8_t fw = css_computed_font_weight(cs);
    t->bold = fw == CSS_FONT_WEIGHT_BOLD || fw == CSS_FONT_WEIGHT_BOLDER || fw >= CSS_FONT_WEIGHT_600;
    uint8_t fs = css_computed_font_style(cs);
    t->italic = fs == CSS_FONT_STYLE_ITALIC || fs == CSS_FONT_STYLE_OBLIQUE;
    uint8_t td = css_computed_text_decoration(cs);
    t->underline = (td & CSS_TEXT_DECORATION_UNDERLINE) != 0;
    t->strike = (td & CSS_TEXT_DECORATION_LINE_THROUGH) != 0;
    t->upper = css_computed_text_transform(cs) == CSS_TEXT_TRANSFORM_UPPERCASE;

    css_computed_color(cs, &c);
    t->color = rgb(c);
    if (css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR && (c >> 24) > 6) t->bg = 0xFF000000u | over_white(c);

    css_computed_font_size(cs, &v, &unit);
    t->font_px = px(cs, u, v, unit);
    if (t->font_px < 6) t->font_px = 6;
    if (t->font_px > 96) t->font_px = 96;
    switch (css_computed_line_height(cs, &v, &unit)) {
    case CSS_LINE_HEIGHT_NUMBER: t->line_px = FIXTOINT(FMUL(v, INTTOFIX(t->font_px))); break;
    case CSS_LINE_HEIGHT_DIMENSION:
        t->line_px = unit == CSS_UNIT_PCT ? FIXTOINT(FMUL(v, INTTOFIX(t->font_px))) / 100 : px(cs, u, v, unit);
        break;
    default: t->line_px = t->font_px * 135 / 100; break;
    }
    if (t->line_px < t->font_px) t->line_px = t->font_px;

    uint8_t ty;
    ty = css_computed_margin_top(cs, &v, &unit);    length(cs, u, ty, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, unit, &t->margin[0]);
    ty = css_computed_margin_right(cs, &v, &unit);  length(cs, u, ty, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, unit, &t->margin[1]);
    ty = css_computed_margin_bottom(cs, &v, &unit); length(cs, u, ty, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, unit, &t->margin[2]);
    ty = css_computed_margin_left(cs, &v, &unit);   length(cs, u, ty, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, unit, &t->margin[3]);
    tf_len pl;
    css_computed_padding_top(cs, &v, &unit);    length(cs, u, 1, 1, 2, v, unit, &pl); t->padding[0] = pl.pct ? 0 : pl.v;
    css_computed_padding_right(cs, &v, &unit);  length(cs, u, 1, 1, 2, v, unit, &pl); t->padding[1] = pl.pct ? 0 : pl.v;
    css_computed_padding_bottom(cs, &v, &unit); length(cs, u, 1, 1, 2, v, unit, &pl); t->padding[2] = pl.pct ? 0 : pl.v;
    css_computed_padding_left(cs, &v, &unit);   length(cs, u, 1, 1, 2, v, unit, &pl); t->padding[3] = pl.pct ? 0 : pl.v;

    uint8_t (*bw[4])(const css_computed_style *, css_fixed *, css_unit *) = {
        css_computed_border_top_width, css_computed_border_right_width, css_computed_border_bottom_width, css_computed_border_left_width };
    uint8_t (*bs[4])(const css_computed_style *) = {
        css_computed_border_top_style, css_computed_border_right_style, css_computed_border_bottom_style, css_computed_border_left_style };
    uint8_t (*bc[4])(const css_computed_style *, css_color *) = {
        css_computed_border_top_color, css_computed_border_right_color, css_computed_border_bottom_color, css_computed_border_left_color };
    for (int i = 0; i < 4; i++) {
        uint8_t st = bs[i](cs);
        if (st == CSS_BORDER_STYLE_NONE || st == CSS_BORDER_STYLE_HIDDEN) continue;
        bw[i](cs, &v, &unit);
        t->border[i] = px(cs, u, v, unit);
        if (t->border[i] > 20) t->border[i] = 20;
        bc[i](cs, &c);
        t->border_color[i] = (c >> 24) > 6 ? over_white(c) : 0xFFFFFFFFu;     /* transparent: not drawn */
    }

    ty = css_computed_width(cs, &v, &unit);     length(cs, u, ty, CSS_WIDTH_SET, CSS_WIDTH_AUTO, v, unit, &t->width);
    ty = css_computed_height(cs, &v, &unit);    length(cs, u, ty, CSS_HEIGHT_SET, CSS_HEIGHT_AUTO, v, unit, &t->height);
    ty = css_computed_max_width(cs, &v, &unit); length(cs, u, ty, CSS_MAX_WIDTH_SET, CSS_MAX_WIDTH_NONE, v, unit, &t->max_width);
    ty = css_computed_min_width(cs, &v, &unit); length(cs, u, ty, CSS_MIN_WIDTH_SET, 0xFF, v, unit, &t->min_width);
    if (css_computed_text_indent(cs, &v, &unit) && unit != CSS_UNIT_PCT) t->text_indent = px(cs, u, v, unit);
    if (css_computed_column_gap(cs, &v, &unit) == CSS_COLUMN_GAP_SET && unit != CSS_UNIT_PCT) t->gap = px(cs, u, v, unit);

    switch (css_computed_clear(cs)) {
    case CSS_CLEAR_LEFT: t->clear = 1; break;
    case CSS_CLEAR_RIGHT: t->clear = 2; break;
    case CSS_CLEAR_BOTH: t->clear = 3; break;
    default: t->clear = 0; break;
    }
    uint8_t ox = css_computed_overflow_x(cs), oy = css_computed_overflow_y(cs);
    t->clip = (ox != CSS_OVERFLOW_VISIBLE && ox != CSS_OVERFLOW_INHERIT) || (oy != CSS_OVERFLOW_VISIBLE && oy != CSS_OVERFLOW_INHERIT);
    t->border_box = css_computed_box_sizing(cs) == CSS_BOX_SIZING_BORDER_BOX;
    switch (css_computed_vertical_align(cs, &v, &unit)) {
    case CSS_VERTICAL_ALIGN_TOP: t->valign = TV_TOP; break;
    case CSS_VERTICAL_ALIGN_MIDDLE: t->valign = TV_MIDDLE; break;
    case CSS_VERTICAL_ALIGN_BOTTOM: t->valign = TV_BOTTOM; break;
    case CSS_VERTICAL_ALIGN_SUB: t->valign = TV_SUB; break;
    case CSS_VERTICAL_ALIGN_SUPER: t->valign = TV_SUPER; break;
    case CSS_VERTICAL_ALIGN_TEXT_TOP: t->valign = TV_TEXT_TOP; break;
    case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: t->valign = TV_TEXT_BOTTOM; break;
    default: t->valign = TV_BASELINE; break;
    }
    ty = css_computed_top(cs, &v, &unit);    length(cs, u, ty, CSS_TOP_SET, CSS_TOP_AUTO, v, unit, &t->offs[0]);
    ty = css_computed_right(cs, &v, &unit);  length(cs, u, ty, CSS_RIGHT_SET, CSS_RIGHT_AUTO, v, unit, &t->offs[1]);
    ty = css_computed_bottom(cs, &v, &unit); length(cs, u, ty, CSS_BOTTOM_SET, CSS_BOTTOM_AUTO, v, unit, &t->offs[2]);
    ty = css_computed_left(cs, &v, &unit);   length(cs, u, ty, CSS_LEFT_SET, CSS_LEFT_AUTO, v, unit, &t->offs[3]);
    ty = css_computed_min_height(cs, &v, &unit); length(cs, u, ty, CSS_MIN_HEIGHT_SET, 0xFF, v, unit, &t->min_height);
    ty = css_computed_max_height(cs, &v, &unit); length(cs, u, ty, CSS_MAX_HEIGHT_SET, CSS_MAX_HEIGHT_NONE, v, unit, &t->max_height);

    switch (fd) {
    case CSS_FLEX_DIRECTION_ROW_REVERSE: t->flex_dir = 1; break;
    case CSS_FLEX_DIRECTION_COLUMN: t->flex_dir = 2; break;
    case CSS_FLEX_DIRECTION_COLUMN_REVERSE: t->flex_dir = 3; break;
    default: t->flex_dir = 0; break;
    }
    switch (css_computed_justify_content(cs)) {
    case CSS_JUSTIFY_CONTENT_FLEX_END: t->justify = TJ_END; break;
    case CSS_JUSTIFY_CONTENT_CENTER: t->justify = TJ_CENTER; break;
    case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: t->justify = TJ_BETWEEN; break;
    case CSS_JUSTIFY_CONTENT_SPACE_AROUND: t->justify = TJ_AROUND; break;
    case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: t->justify = TJ_EVENLY; break;
    default: t->justify = TJ_START; break;
    }
    static const uint8_t al[] = { TAL_AUTO, TAL_STRETCH, TAL_START, TAL_END, TAL_CENTER, TAL_BASELINE };
    uint8_t ai = css_computed_align_items(cs);
    t->align_items = ai < 6 ? al[ai] : TAL_STRETCH;
    if (t->align_items == TAL_AUTO) t->align_items = TAL_STRETCH;
    uint8_t as = css_computed_align_self(cs);
    t->align_self = as < 6 ? al[as] : TAL_AUTO;
    if (css_computed_flex_grow(cs, &v) == CSS_FLEX_GROW_SET) t->flex_grow = FIXTOINT(FMUL(v, INTTOFIX(100)));
    t->flex_shrink = 100;
    if (css_computed_flex_shrink(cs, &v) == CSS_FLEX_SHRINK_SET) t->flex_shrink = FIXTOINT(FMUL(v, INTTOFIX(100)));
    ty = css_computed_flex_basis(cs, &v, &unit);
    length(cs, u, ty, CSS_FLEX_BASIS_SET, CSS_FLEX_BASIS_AUTO, v, unit, &t->flex_basis);
    int32_t ord = 0;
    if (css_computed_order(cs, &ord) == CSS_ORDER_SET) t->order = ord;
    t->collapse = css_computed_border_collapse(cs) == CSS_BORDER_COLLAPSE_COLLAPSE;
    {
        css_fixed hl, vl;
        css_unit hu, vu;
        t->bspacing = css_computed_border_spacing(cs, &hl, &hu, &vl, &vu) == CSS_BORDER_SPACING_SET ? px(cs, u, hl, hu) : -1;
    }
    t->opacity = 255;
    if (css_computed_opacity(cs, &v) == CSS_OPACITY_SET) { int o = FIXTOINT(FMUL(v, INTTOFIX(255))); t->opacity = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o); }
}

/* style="" is turned into a style sheet of its own, again only when it changed */
static css_stylesheet *inline_sheet(tf_node *n)
{
    const char *css = dom_attr(n, "style");
    if (!css || !*css) {
        if (n->inline_sheet) { css_stylesheet_destroy(n->inline_sheet); n->inline_sheet = 0; }
        free(n->inline_css);
        n->inline_css = 0;
        return 0;
    }
    if (n->inline_css && !strcmp(n->inline_css, css) && !(cur_env && strstr(css, "var("))) return n->inline_sheet;
    if (n->inline_sheet) css_stylesheet_destroy(n->inline_sheet);
    free(n->inline_css);
    n->inline_css = strdup(css);
    uint32_t vl;
    char *vr = current_styler && (strstr(css, "var(") || strstr(css, "calc(")) ? resolve_vars(current_styler, css, (uint32_t)strlen(css), &vl) : 0;
    const char *src = vr ? vr : css;
    size_t sl = vr ? vl : strlen(css);
    obuf lg = { 0, 0, 0 };
    if (has_logical(src, src + sl)) { logical_block(src, src + sl, &lg); if (lg.s) { src = lg.s; sl = lg.n; } }
    obuf hx = { 0, 0, 0 };
    if (has_hex_alpha(src, src + sl)) { hex_alpha(src, src + sl, &hx); if (hx.s) { src = hx.s; sl = hx.n; } }
    n->inline_sheet = make_sheet(src, sl, 0, 1);
    free(lg.s);
    free(hx.s);
    free(vr);
    return n->inline_sheet;
}

/* one element's style, from its parent's; returns its computed style for its children (0 = nothing below is shown) */
static const css_computed_style *style_one(tf_styler *s, tf_node *c, const css_computed_style *parent, int is_root, venv **envp)
{
    int svg_root = dom_is(c, "svg");
    if (c->ns != HUBBUB_NS_HTML && c->ns != 0 && !svg_root) {   /* inside SVG and MathML: drawn as a picture, or not */
        if (!c->style) c->style = calloc(1, sizeof(tf_style));
        if (c->style) { memset(c->style, 0, sizeof(tf_style)); c->style->display = TD_NONE; }
        return 0;
    }
    if (c->sel) { css_select_results_destroy(c->sel); c->sel = 0; }
    /* libcss's own data on the node (for sharing styles between siblings) may be from before the tree changed */
    if (c->css_data) { css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, 0, c, NULL, c->css_data); c->css_data = 0; }
    css_media media;
    memset(&media, 0, sizeof(media));
    media.type = CSS_MEDIA_SCREEN;
    media.width = s->unit.viewport_width;
    media.height = s->unit.viewport_height;
    css_select_results *r = 0;
    venv *env = env_for(s, c, *envp);
    *envp = env;
    cur_env = env;
    css_stylesheet *isheet = inline_sheet(c);
    css_error err = css_select_style(env && env->variant && env->variant->ctx ? env->variant->ctx : s->ctx, c, &s->unit, &media, isheet, &handler, s, &r);
    cur_env = 0;
    if (err != CSS_OK || !r) return 0;
    css_computed_style *mine = r->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (parent && mine) {
        css_computed_style *composed;
        if (css_computed_style_compose(parent, mine, &s->unit, &composed) == CSS_OK) {
            css_computed_style_destroy(mine);
            r->styles[CSS_PSEUDO_ELEMENT_NONE] = mine = composed;
        }
    }
    c->sel = r;
    if (!mine) return 0;
    if (is_root) s->unit.root_style = mine;
    if (!c->style) c->style = malloc(sizeof(tf_style));
    if (c->style) { convert(mine, &s->unit, c->style, is_root); side_apply(s, c, c->style, FIXTOINT(s->unit.viewport_width)); }
    if (c->style && c->style->clip && (c->style->position == TP_ABSOLUTE || c->style->position == TP_FIXED) &&
        !c->style->width.autov && !c->style->width.pct && c->style->width.v <= 1 &&
        !c->style->height.autov && !c->style->height.pct && c->style->height.v <= 1)
        c->style->display = TD_NONE;          /* "visually hidden" (.sr-only): there for screen readers, not drawn */
    if (c->style && c->style->display == TD_NONE) return 0;             /* nothing below is shown either */
    if (svg_root) {
        /* an <svg> in the page: a picture (engine.c draws it), inline-block unless the page says block */
        if (c->style && (c->style->display == TD_INLINE)) c->style->display = TD_INLINE_BLOCK;
        for (tf_node *k = c->first; k; k = dom_next(k, c)) if (k->type == TF_ELEM && k->style) k->style->display = TD_NONE;
        return 0;
    }
    return mine;
}

/* The tree is walked with a stack of its own, so the work can stop after a number of elements and go on later
 * (style_compute_step): a big page is styled a slice at a time while the desktop keeps running. */
void style_compute_begin(tf_styler *s, tf_doc *d, int vw, int vh)
{
    current_styler = s;
    s->unit.viewport_width = INTTOFIX(vw > 0 ? vw : 1024);
    s->unit.viewport_height = INTTOFIX(vh > 0 ? vh : 768);
    s->unit.root_style = NULL;
    s->sp = 0;
    if (s->scap < 64) {
        style_frame *ns = realloc(s->stk, 64 * sizeof(style_frame));
        if (!ns) return;
        s->stk = ns;
        s->scap = 64;
    }
    envs_clear(s);
    s->stk[s->sp++] = (style_frame){ d->root->first, 0, 1, 0 };
}

int style_compute_step(tf_styler *s, int max_elements)
{
    current_styler = s;
    int done = 0;
    while (s->sp > 0) {
        style_frame *f = &s->stk[s->sp - 1];
        tf_node *c = f->next;
        if (!c) { s->sp--; continue; }
        f->next = c->next;
        if (c->type != TF_ELEM) continue;
        venv *env = f->env;
        const css_computed_style *mine = style_one(s, c, f->parent, f->is_root, &env);
        if (mine && c->first) {
            if (s->sp == s->scap) {
                style_frame *ns = realloc(s->stk, (size_t)s->scap * 2 * sizeof(style_frame));
                if (!ns) continue;
                s->stk = ns;
                s->scap *= 2;
            }
            s->stk[s->sp++] = (style_frame){ c->first, mine, 0, env };
        }
        if (max_elements > 0 && ++done >= max_elements) return s->sp == 0;
    }
    return 1;
}

int style_computing(tf_styler *s) { return s && s->sp > 0; }

void style_compute(tf_styler *s, tf_doc *d, int vw, int vh)
{
    style_compute_begin(s, d, vw, vh);
    style_compute_step(s, 0);
}
