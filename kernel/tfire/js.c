/*
 * tFire's scripts: QuickJS (third_party/quickjs) with the page's document in it.
 *
 * The DOM itself is written in JavaScript (prelude.js): Node, Element, Document, events, timers, fetch,
 * XMLHttpRequest, storage and the rest of what pages expect of window. Underneath it are the natives here
 * (__tf.*), which walk and change the real tree (dom.c) and ask the engine (engine.c) for what only it can do:
 * navigate, fetch, lay out, scroll.
 *
 * Every node a script sees gets one JavaScript object, made when it is first asked for and kept (with a
 * reference of the node's own) for as long as the page lives, so the same node is always the same object and
 * what a script stores on it stays.
 *
 * This file is built for software floating point like QuickJS itself: nothing here hands a double to the rest
 * of the kernel, or takes one from it.
 */
#include "quickjs.h"
#include "engine.h"
#include "tfire_libc.h"
#include "webfetch.h"

extern const char tf_prelude[];          /* prelude.js (js_prelude.s) */
extern uint32_t uihost_ms(void);
extern void csprng_fill(uint8_t *buf, int len);
extern uint64_t tfire_now_ms(void);

#define MAX_MISSING 16
#define RUN_LIMIT_MS 5000                /* one script, event or timer may run this long, then it is stopped */

struct tf_js {
    tf_page *page;
    JSRuntime *rt;
    JSContext *ctx;
    JSClassID node_class;
    JSValue fn_proto, fn_event, fn_winev, fn_tick, fn_fetch, fn_image, fn_script;
    uint32_t deadline;
    uint32_t t0;
    /* modules: their sources once fetched, and the ones a module asked for that are not here yet */
    struct mod { char *url; char *src; int len; } *mods;
    int nmods;
    char *missing[MAX_MISSING];
    int nmissing;
};

static JSClassID node_class_id;

/* ------------------------------------------------------------------ nodes as objects */

static tf_js *J(JSContext *ctx) { return JS_GetContextOpaque(ctx); }

static JSValue wrap(tf_js *j, tf_node *n)
{
    if (!n) return JS_NULL;
    if (n->js_obj) return JS_DupValue(j->ctx, JS_MKPTR(JS_TAG_OBJECT, n->js_obj));
    const char *tag = n->type == TF_ELEM && n->name ? lwc_string_data(n->name) : "";
    JSValue args[3] = { JS_NewInt32(j->ctx, n->type), JS_NewString(j->ctx, tag), JS_NewInt32(j->ctx, n->ns) };
    JSValue proto = JS_Call(j->ctx, j->fn_proto, JS_UNDEFINED, 3, args);
    JS_FreeValue(j->ctx, args[1]);
    if (JS_IsException(proto)) { JS_FreeValue(j->ctx, JS_GetException(j->ctx)); proto = JS_NULL; }
    JSValue obj = JS_NewObjectProtoClass(j->ctx, proto, j->node_class);
    JS_FreeValue(j->ctx, proto);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    n->js_obj = JS_VALUE_GET_PTR(obj);
    return JS_DupValue(j->ctx, obj);         /* one reference stays with the node */
}

static tf_node *node_of(tf_js *j, JSValueConst v)
{
    return JS_IsObject(v) ? JS_GetOpaque(v, j->node_class) : 0;
}

/* a string argument, or 0 for null/undefined (free with JS_FreeCString) */
static const char *str_arg(JSContext *ctx, JSValueConst v, size_t *len)
{
    if (JS_IsNull(v) || JS_IsUndefined(v)) { if (len) *len = 0; return 0; }
    return JS_ToCStringLen(ctx, len, v);
}

static int int_arg(JSContext *ctx, JSValueConst v)
{
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    return i;
}

static JSValue str_or_null(JSContext *ctx, const char *s) { return s ? JS_NewString(ctx, s) : JS_NULL; }

/* a malloc'd string handed to the script and freed */
static JSValue str_take(JSContext *ctx, char *s)
{
    JSValue v = s ? JS_NewString(ctx, s) : JS_NewString(ctx, "");
    free(s);
    return v;
}

/* ------------------------------------------------------------------ the natives */

enum {
    N_TYPE, N_NAME, N_PARENT, N_FIRST, N_LAST, N_NEXT, N_PREV,
    N_GETATTR, N_SETATTR, N_REMATTR, N_ATTRS,
    N_TEXT, N_SETTEXT, N_INNER, N_SETINNER, N_OUTER,
    N_CREATE, N_CREATETEXT, N_CREATECOMMENT, N_CREATEFRAG,
    N_INSERT, N_REMOVE, N_CLONE,
    N_BYID, N_QUERY, N_QUERYALL, N_MATCHES, N_CONTAINS, N_CONNECTED,
    N_VALUE, N_SETVALUE, N_CHECKED, N_SETCHECKED,
    N_RECT, N_STYLE, N_DOC, N_URL, N_NAVIGATE, N_SETURL, N_RELOAD,
    N_COOKIE, N_SETCOOKIE, N_SCROLLY, N_SCROLLTO, N_VW, N_VH,
    N_ALERT, N_LOG, N_WRITE, N_FOCUS, N_FOCUSED, N_SUBMIT,
    N_FETCH, N_ABORT, N_STORE_GET, N_STORE_SET, N_STORE_KEYS, N_STORE_CLEAR,
    N_NOW, N_PERF, N_RANDOM, N_CHANGED, N_SETUP, N_NS, N_RESOLVE, N_HIT, N_ANIMATE, N_ANIMCTL, N_ANIMINFO,
};

static void changed(tf_js *j) { page_changed(j->page); }

static const char *const css_display[] = { "none", "inline", "block", "list-item", "inline-block", "table",
    "table-row-group", "table-row", "table-cell", "table-caption", "flex", "inline-flex", "grid" };
static const char *const css_position[] = { "static", "relative", "absolute", "fixed", "sticky" };

static void put_str(JSContext *ctx, JSValue o, const char *k, const char *v) { JS_SetPropertyStr(ctx, o, k, JS_NewString(ctx, v)); }

static void put_px(JSContext *ctx, JSValue o, const char *k, int v)
{
    char b[24];
    snprintf(b, sizeof(b), "%dpx", v);
    put_str(ctx, o, k, b);
}

static void put_rgb(JSContext *ctx, JSValue o, const char *k, uint32_t c, int alpha)
{
    char b[40];
    if (!alpha) snprintf(b, sizeof(b), "rgba(0, 0, 0, 0)");
    else snprintf(b, sizeof(b), "rgb(%u, %u, %u)", (c >> 16) & 255, (c >> 8) & 255, c & 255);
    put_str(ctx, o, k, b);
}

/* getComputedStyle: what tFire's own style for the node says */
static JSValue computed(tf_js *j, tf_node *n)
{
    JSContext *ctx = j->ctx;
    JSValue o = JS_NewObject(ctx);
    tf_style *s = n && n->type == TF_ELEM ? n->style : 0;
    if (!s) { put_str(ctx, o, "display", n && n->type == TF_ELEM ? "none" : "inline"); return o; }
    put_str(ctx, o, "display", s->display < 13 ? css_display[s->display] : "block");
    put_str(ctx, o, "position", s->position < 5 ? css_position[s->position] : "static");
    put_str(ctx, o, "visibility", s->visible ? "visible" : "hidden");
    put_str(ctx, o, "fontWeight", s->bold ? "700" : "400");
    put_str(ctx, o, "fontStyle", s->italic ? "italic" : "normal");
    put_str(ctx, o, "textAlign", s->text_align == TA_RIGHT ? "right" : s->text_align == TA_CENTER ? "center" : s->text_align == TA_JUSTIFY ? "justify" : "start");
    put_str(ctx, o, "whiteSpace", s->white_space == TWS_PRE ? "pre" : s->white_space == TWS_NOWRAP ? "nowrap" : s->white_space == TWS_PRE_WRAP ? "pre-wrap" : s->white_space == TWS_PRE_LINE ? "pre-line" : "normal");
    put_str(ctx, o, "float", s->floated == 1 ? "left" : s->floated == 2 ? "right" : "none");
    put_str(ctx, o, "opacity", "1");
    put_str(ctx, o, "overflow", "visible");
    put_str(ctx, o, "transform", "none");
    put_str(ctx, o, "fontFamily", "sans-serif");
    put_rgb(ctx, o, "color", s->color, 1);
    put_rgb(ctx, o, "backgroundColor", s->bg & 0xFFFFFF, (s->bg >> 24) != 0);
    put_px(ctx, o, "fontSize", s->font_px);
    put_px(ctx, o, "lineHeight", s->line_px);
    static const char *const side[4] = { "Top", "Right", "Bottom", "Left" };
    char k[32];
    for (int i = 0; i < 4; i++) {
        snprintf(k, sizeof(k), "padding%s", side[i]); put_px(ctx, o, k, s->padding[i]);
        snprintf(k, sizeof(k), "border%sWidth", side[i]); put_px(ctx, o, k, s->border[i]);
        snprintf(k, sizeof(k), "margin%s", side[i]);
        if (s->margin[i].autov) put_str(ctx, o, k, "auto"); else put_px(ctx, o, k, s->margin[i].pct ? 0 : s->margin[i].v);
    }
    put_px(ctx, o, "width", n->laid ? n->lw : 0);
    put_px(ctx, o, "height", n->laid ? n->lh : 0);
    return o;
}

static JSValue native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    tf_js *j = J(ctx);
    tf_doc *d = page_doc(j->page);
    JSValueConst a0 = argc > 0 ? argv[0] : JS_UNDEFINED, a1 = argc > 1 ? argv[1] : JS_UNDEFINED,
                 a2 = argc > 2 ? argv[2] : JS_UNDEFINED, a3 = argc > 3 ? argv[3] : JS_UNDEFINED,
                 a4 = argc > 4 ? argv[4] : JS_UNDEFINED;
    tf_node *n = node_of(j, a0);
    size_t l1 = 0, l2 = 0;
    const char *s1, *s2, *s3;
    JSValue r = JS_UNDEFINED;

    switch (magic) {
    case N_TYPE: return JS_NewInt32(ctx, n ? n->type : 0);
    case N_NAME: return JS_NewString(ctx, n && n->type == TF_ELEM && n->name ? lwc_string_data(n->name) : "");
    case N_NS: return JS_NewInt32(ctx, n ? n->ns : 0);
    case N_PARENT: return wrap(j, n ? n->parent : 0);
    case N_FIRST: return wrap(j, n ? n->first : 0);
    case N_LAST: return wrap(j, n ? n->last : 0);
    case N_NEXT: return wrap(j, n ? n->next : 0);
    case N_PREV: return wrap(j, n ? n->prev : 0);

    case N_GETATTR:
        if (!n || !(s1 = str_arg(ctx, a1, 0))) return JS_NULL;
        {
            char low[64];
            int i = 0;
            for (; s1[i] && i < 63; i++) low[i] = (char)tolower((unsigned char)s1[i]);
            low[i] = 0;
            r = str_or_null(ctx, dom_attr(n, low));
        }
        JS_FreeCString(ctx, s1);
        return r;
    case N_SETATTR:
        if (!n || !(s1 = str_arg(ctx, a1, 0))) return JS_UNDEFINED;
        s2 = JS_ToCString(ctx, a2);
        if (s2) {
            dom_set_attr(d, n, s1, s2);
            if (!strcasecmp(s1, "value") && n->value && !dom_is(n, "option")) { /* (what was typed stays) */ }
            if (!strcasecmp(s1, "src") || !strcasecmp(s1, "href") || !strcasecmp(s1, "rel")) page_node_inserted(j->page, n);
            JS_FreeCString(ctx, s2);
        }
        JS_FreeCString(ctx, s1);
        changed(j);
        return JS_UNDEFINED;
    case N_REMATTR:
        if (!n || !(s1 = str_arg(ctx, a1, 0))) return JS_UNDEFINED;
        dom_remove_attr(d, n, s1);
        JS_FreeCString(ctx, s1);
        changed(j);
        return JS_UNDEFINED;
    case N_ATTRS:
        r = JS_NewArray(ctx);
        for (int i = 0; n && i < n->nattrs; i++) {
            JS_SetPropertyUint32(ctx, r, (uint32_t)(2 * i), JS_NewString(ctx, lwc_string_data(n->attrs[i].name)));
            JS_SetPropertyUint32(ctx, r, (uint32_t)(2 * i + 1), JS_NewString(ctx, n->attrs[i].value ? n->attrs[i].value : ""));
        }
        return r;

    case N_TEXT:
        if (!n) return JS_NULL;
        return str_take(ctx, dom_text_content(n));
    case N_SETTEXT:
        if (!n) return JS_UNDEFINED;
        s1 = str_arg(ctx, a1, 0);
        dom_set_text_content(d, n, s1 ? s1 : "");
        if (s1) JS_FreeCString(ctx, s1);
        if (dom_is(n, "style") || dom_is(n, "script")) page_node_inserted(j->page, n);
        changed(j);
        return JS_UNDEFINED;
    case N_INNER: return n ? str_take(ctx, dom_inner_html(n)) : JS_NewString(ctx, "");
    case N_OUTER: return n ? str_take(ctx, dom_outer_html(n)) : JS_NewString(ctx, "");
    case N_SETINNER:
        if (!n) return JS_UNDEFINED;
        s1 = str_arg(ctx, a1, &l1);
        dom_set_inner_html(d, n, s1 ? s1 : "", (int)l1);
        if (s1) JS_FreeCString(ctx, s1);
        if (dom_connected(d, n)) page_node_inserted(j->page, n);
        changed(j);
        return JS_UNDEFINED;

    case N_CREATE:
        s1 = str_arg(ctx, a0, 0);
        if (!s1) return JS_NULL;
        {
            tf_node *e = dom_new_element(d, s1);
            if (e && argc > 1 && int_arg(ctx, a1)) e->ns = int_arg(ctx, a1);
            r = wrap(j, e);
        }
        JS_FreeCString(ctx, s1);
        return r;
    case N_CREATETEXT:
    case N_CREATECOMMENT:
        s1 = str_arg(ctx, a0, &l1);
        {
            tf_node *t = magic == N_CREATETEXT ? dom_new_text(d, s1 ? s1 : "", (int)l1) : dom_new_node(d, TF_COMMENT);
            if (t && magic == N_CREATECOMMENT) { t->text = strndup_(s1 ? s1 : "", l1); t->text_len = (int)l1; }
            r = wrap(j, t);
        }
        if (s1) JS_FreeCString(ctx, s1);
        return r;
    case N_CREATEFRAG: return wrap(j, dom_new_node(d, TF_FRAG));

    case N_INSERT: {
        tf_node *c = node_of(j, a1), *ref = node_of(j, a2);
        if (!n || !c) return JS_UNDEFINED;
        if (dom_contains(c, n)) return JS_ThrowTypeError(ctx, "HierarchyRequestError: the new child contains the parent");
        /* a fragment's children: each one is new to the document */
        tf_node *kids[64];
        int nk = 0;
        if (c->type == TF_FRAG) for (tf_node *k = c->first; k && nk < 64; k = k->next) kids[nk++] = k;
        dom_insert_before(n, c, ref);
        if (dom_connected(d, n)) {
            if (c->type == TF_FRAG) for (int i = 0; i < nk; i++) page_node_inserted(j->page, kids[i]);
            else page_node_inserted(j->page, c);
        }
        changed(j);
        return JS_UNDEFINED;
    }
    case N_REMOVE:
        if (n) { if (page_focus(j->page) && dom_contains(n, page_focus(j->page))) page_focus_node(j->page, 0); dom_remove(n); changed(j); }
        return JS_UNDEFINED;
    case N_CLONE: return n ? wrap(j, dom_clone(d, n, JS_ToBool(ctx, a1))) : JS_NULL;

    case N_BYID:
        s1 = str_arg(ctx, a0, 0);
        if (!s1) return JS_NULL;
        r = wrap(j, dom_by_id(d, s1));
        JS_FreeCString(ctx, s1);
        return r;
    case N_QUERY:
    case N_QUERYALL:
    case N_MATCHES:
        if (!n || !(s1 = str_arg(ctx, a1, 0))) return magic == N_QUERYALL ? JS_NewArray(ctx) : magic == N_MATCHES ? JS_FALSE : JS_NULL;
        if (magic == N_QUERY) r = wrap(j, dom_query(n, s1));
        else if (magic == N_MATCHES) r = JS_NewBool(ctx, n->type == TF_ELEM && dom_matches(n, s1));
        else {
            int cap = 256, got;
            tf_node **out = 0;
            for (;;) {
                free(out);
                out = malloc((size_t)cap * sizeof(*out));
                if (!out) { got = 0; break; }
                got = dom_query_all(n, s1, out, cap);
                if (got < cap || cap >= 65536) break;
                cap *= 4;
            }
            r = JS_NewArray(ctx);
            for (int i = 0; i < got; i++) JS_SetPropertyUint32(ctx, r, (uint32_t)i, wrap(j, out[i]));
            free(out);
        }
        JS_FreeCString(ctx, s1);
        return r;
    case N_CONTAINS: { tf_node *b = node_of(j, a1); return JS_NewBool(ctx, n && b && dom_contains(n, b)); }
    case N_CONNECTED: return JS_NewBool(ctx, n && dom_connected(d, n));

    case N_VALUE:
        if (!n) return JS_NewString(ctx, "");
        if (n->value) return JS_NewString(ctx, n->value);
        if (dom_is(n, "textarea")) return str_take(ctx, dom_text_content(n));
        if (dom_is(n, "select")) {
            tf_node *sel = 0, *first = 0;
            for (tf_node *o = n; o; o = dom_next(o, n))
                if (dom_is(o, "option")) { if (!first) first = o; if (o->checked || dom_attr(o, "selected")) { sel = o; if (o->checked) break; } }
            if (!sel) sel = first;
            if (!sel) return JS_NewString(ctx, "");
            const char *v = dom_attr(sel, "value");
            return v ? JS_NewString(ctx, v) : str_take(ctx, dom_text_content(sel));
        }
        if (dom_is(n, "option")) { const char *v = dom_attr(n, "value"); return v ? JS_NewString(ctx, v) : str_take(ctx, dom_text_content(n)); }
        {
            const char *v = dom_attr(n, "value");
            const char *type = dom_attr(n, "type");
            if (!v && type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) v = "on";
            return JS_NewString(ctx, v ? v : "");
        }
    case N_SETVALUE:
        if (!n) return JS_UNDEFINED;
        s1 = JS_ToCString(ctx, a1);
        if (!s1) return JS_EXCEPTION;
        if (dom_is(n, "select")) {
            for (tf_node *o = n; o; o = dom_next(o, n))
                if (dom_is(o, "option")) {
                    const char *v = dom_attr(o, "value");
                    char *t = v ? 0 : dom_text_content(o);
                    o->checked = !strcmp(v ? v : t, s1);
                    if (o->checked) dom_set_attr(d, o, "selected", ""); else dom_remove_attr(d, o, "selected");
                    free(t);
                }
        } else {
            free(n->value);
            n->value = strdup(s1);
        }
        JS_FreeCString(ctx, s1);
        changed(j);
        return JS_UNDEFINED;
    case N_CHECKED: return JS_NewBool(ctx, n && (n->checked == 1 || (n->checked == 0 && dom_attr(n, "checked"))));
    case N_SETCHECKED:
        if (!n) return JS_UNDEFINED;
        n->checked = JS_ToBool(ctx, a1) ? 1 : 2;              /* 2: unchecked, whatever the attribute says */
        if (n->checked == 1) {
            /* a radio button: the others of its group go off */
            const char *type = dom_attr(n, "type"), *name = dom_attr(n, "name");
            if (type && !strcasecmp(type, "radio") && name)
                for (tf_node *o = d->root; o; o = dom_next(o, d->root))
                    if (o != n && dom_is(o, "input")) {
                        const char *t2 = dom_attr(o, "type"), *n2 = dom_attr(o, "name");
                        if (t2 && !strcasecmp(t2, "radio") && n2 && !strcmp(n2, name)) o->checked = 2;
                    }
        }
        changed(j);
        return JS_UNDEFINED;

    case N_RECT:
        page_layout_now(j->page);
        r = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, n && n->laid ? n->lx : 0));
        JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, n && n->laid ? n->ly : 0));
        JS_SetPropertyUint32(ctx, r, 2, JS_NewInt32(ctx, n && n->laid ? n->lw : 0));
        JS_SetPropertyUint32(ctx, r, 3, JS_NewInt32(ctx, n && n->laid ? n->lh : 0));
        return r;
    case N_STYLE: page_layout_now(j->page); return computed(j, n);
    case N_DOC: return wrap(j, d->root);
    case N_URL: return JS_NewString(ctx, page_url(j->page));
    case N_NAVIGATE:
    case N_SETURL:
        s1 = str_arg(ctx, a0, 0);
        if (s1) {
            char abs[1024];
            if (page_resolve(page_url(j->page), s1, abs, sizeof(abs)) || !strncmp(s1, "tfire:", 6)) {
                if (!strncmp(s1, "tfire:", 6)) { strncpy(abs, s1, sizeof(abs) - 1); abs[sizeof(abs) - 1] = 0; }
                if (magic == N_NAVIGATE) page_navigate(j->page, abs, JS_ToBool(ctx, a1));
                else page_set_url(j->page, abs, JS_ToBool(ctx, a1));
            }
            JS_FreeCString(ctx, s1);
        }
        return JS_UNDEFINED;
    case N_RELOAD: page_reload(j->page); return JS_UNDEFINED;
    case N_COOKIE: {
        char *b = malloc(8192);
        if (!b) return JS_NewString(ctx, "");
        webfetch_cookie_header(page_url(j->page), b, 8192, 1);
        return str_take(ctx, b);
    }
    case N_SETCOOKIE:
        s1 = str_arg(ctx, a0, 0);
        if (s1) { webfetch_cookie_set(page_url(j->page), s1, 1); JS_FreeCString(ctx, s1); }
        return JS_UNDEFINED;
    case N_SCROLLY: return JS_NewInt32(ctx, page_scroll_y(j->page));
    case N_SCROLLTO: page_scroll_to(j->page, int_arg(ctx, a0)); return JS_UNDEFINED;
    case N_VW: return JS_NewInt32(ctx, page_viewport_w(j->page));
    case N_VH: return JS_NewInt32(ctx, page_viewport_h(j->page));
    case N_ALERT:
        s1 = JS_ToCString(ctx, a0);
        if (s1) { page_alert(j->page, s1); JS_FreeCString(ctx, s1); }
        return JS_UNDEFINED;
    case N_LOG:
        s1 = JS_ToCString(ctx, a1);
        if (s1) { page_console(j->page, int_arg(ctx, a0), s1); JS_FreeCString(ctx, s1); }
        return JS_UNDEFINED;
    case N_WRITE:
        s1 = str_arg(ctx, a0, &l1);
        if (s1) { page_write(j->page, s1, (int)l1); JS_FreeCString(ctx, s1); }
        return JS_UNDEFINED;
    case N_FOCUS: page_focus_node(j->page, n); return JS_UNDEFINED;
    case N_FOCUSED: return wrap(j, page_focus(j->page));
    case N_SUBMIT: page_submit(j->page, n, node_of(j, a1)); return JS_UNDEFINED;

    case N_FETCH: {
        int id = int_arg(ctx, a0);
        s1 = str_arg(ctx, a1, 0);
        s2 = str_arg(ctx, a2, 0);
        s3 = str_arg(ctx, a3, 0);
        size_t bl = 0;
        const char *body = str_arg(ctx, a4, &bl);
        int ok = -1;
        if (s1 && s2) {
            char abs[1024];
            if (page_resolve(page_url(j->page), s2, abs, sizeof(abs)))
                ok = page_fetch(j->page, id, s1, abs, s3, body, (int)bl);
        }
        if (s1) JS_FreeCString(ctx, s1);
        if (s2) JS_FreeCString(ctx, s2);
        if (s3) JS_FreeCString(ctx, s3);
        if (body) JS_FreeCString(ctx, body);
        return JS_NewInt32(ctx, ok);
    }
    case N_ABORT: page_fetch_abort(j->page, int_arg(ctx, a0)); return JS_UNDEFINED;
    case N_STORE_GET:
        s1 = str_arg(ctx, a1, 0);
        if (!s1) return JS_NULL;
        {
            char *v = page_storage_get(j->page, JS_ToBool(ctx, a0), s1);
            r = v ? str_take(ctx, v) : JS_NULL;
        }
        JS_FreeCString(ctx, s1);
        return r;
    case N_STORE_SET:
        s1 = str_arg(ctx, a1, 0);
        s2 = str_arg(ctx, a2, &l2);
        if (s1) page_storage_set(j->page, JS_ToBool(ctx, a0), s1, s2);
        if (s1) JS_FreeCString(ctx, s1);
        if (s2) JS_FreeCString(ctx, s2);
        return JS_UNDEFINED;
    case N_STORE_KEYS: {
        char **keys = 0;
        int nk = page_storage_keys(j->page, JS_ToBool(ctx, a0), &keys);
        r = JS_NewArray(ctx);
        for (int i = 0; i < nk; i++) { JS_SetPropertyUint32(ctx, r, (uint32_t)i, JS_NewString(ctx, keys[i])); free(keys[i]); }
        free(keys);
        return r;
    }
    case N_STORE_CLEAR: page_storage_clear(j->page, JS_ToBool(ctx, a0)); return JS_UNDEFINED;

    case N_NOW: return JS_NewFloat64(ctx, (double)tfire_now_ms());
    case N_PERF: return JS_NewFloat64(ctx, (double)(uihost_ms() - j->t0));
    case N_RANDOM: {
        int cnt = int_arg(ctx, a0);
        if (cnt < 0) cnt = 0;
        if (cnt > 65536) cnt = 65536;
        uint8_t *b = malloc((size_t)cnt + 1);
        if (!b) return JS_NewArray(ctx);
        csprng_fill(b, cnt);
        r = JS_NewArrayBufferCopy(ctx, b, (size_t)cnt);
        free(b);
        return r;
    }
    case N_CHANGED: changed(j); return JS_UNDEFINED;
    case N_RESOLVE: {
        s1 = str_arg(ctx, a0, 0);
        s2 = str_arg(ctx, a1, 0);
        char abs[2048];
        r = JS_NULL;
        if (s2 && page_resolve(s1 ? s1 : page_url(j->page), s2, abs, sizeof(abs))) r = JS_NewString(ctx, abs);
        if (s1) JS_FreeCString(ctx, s1);
        if (s2) JS_FreeCString(ctx, s2);
        return r;
    }
    case N_ANIMATE: {
        /* element.animate(): (el, frames, duration, delay, easing, iterations x1000, direction, fill) */
        if (!n || n->type != TF_ELEM || !(s1 = str_arg(ctx, a1, &l1))) return JS_NewInt32(ctx, 0);
        s2 = argc > 4 ? str_arg(ctx, a4, 0) : 0;
        int id = page_anim_add(j->page, n, s1, (int)l1, int_arg(ctx, a2), int_arg(ctx, a3), s2,
                               argc > 5 ? int_arg(ctx, argv[5]) : 1000, argc > 6 ? int_arg(ctx, argv[6]) : 0, argc > 7 ? int_arg(ctx, argv[7]) : 0);
        JS_FreeCString(ctx, s1);
        if (s2) JS_FreeCString(ctx, s2);
        return JS_NewInt32(ctx, id);
    }
    case N_ANIMCTL: page_anim_ctl(j->page, int_arg(ctx, a0), int_arg(ctx, a1), int_arg(ctx, a2)); return JS_UNDEFINED;
    case N_ANIMINFO: {
        int cur = 0, st = page_anim_info(j->page, int_arg(ctx, a0), &cur);
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, st));
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, cur));
        return arr;
    }
    case N_HIT:
        page_layout_now(j->page);
        return wrap(j, layout_hit(page_layout(j->page), d, int_arg(ctx, a0), int_arg(ctx, a1) + page_scroll_y(j->page)));
    case N_SETUP:
        /* the prelude hands over what the engine calls: prototypes, events, timers, fetches, pictures, scripts */
        JS_FreeValue(ctx, j->fn_proto); j->fn_proto = JS_DupValue(ctx, a0);
        JS_FreeValue(ctx, j->fn_event); j->fn_event = JS_DupValue(ctx, a1);
        JS_FreeValue(ctx, j->fn_winev); j->fn_winev = JS_DupValue(ctx, a2);
        JS_FreeValue(ctx, j->fn_tick); j->fn_tick = JS_DupValue(ctx, a3);
        JS_FreeValue(ctx, j->fn_fetch); j->fn_fetch = JS_DupValue(ctx, a4);
        JS_FreeValue(ctx, j->fn_image); j->fn_image = JS_DupValue(ctx, argc > 5 ? argv[5] : JS_UNDEFINED);
        JS_FreeValue(ctx, j->fn_script); j->fn_script = JS_DupValue(ctx, argc > 6 ? argv[6] : JS_UNDEFINED);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

#define NATIVE(name, n, magic) JS_CFUNC_MAGIC_DEF(name, n, native, magic)
static const JSCFunctionListEntry natives[] = {
    NATIVE("type", 1, N_TYPE), NATIVE("name", 1, N_NAME), NATIVE("ns", 1, N_NS), NATIVE("parent", 1, N_PARENT),
    NATIVE("first", 1, N_FIRST), NATIVE("last", 1, N_LAST), NATIVE("next", 1, N_NEXT), NATIVE("prev", 1, N_PREV),
    NATIVE("getAttr", 2, N_GETATTR), NATIVE("setAttr", 3, N_SETATTR), NATIVE("removeAttr", 2, N_REMATTR), NATIVE("attrs", 1, N_ATTRS),
    NATIVE("text", 1, N_TEXT), NATIVE("setText", 2, N_SETTEXT), NATIVE("inner", 1, N_INNER), NATIVE("setInner", 2, N_SETINNER),
    NATIVE("outer", 1, N_OUTER),
    NATIVE("create", 2, N_CREATE), NATIVE("createText", 1, N_CREATETEXT), NATIVE("createComment", 1, N_CREATECOMMENT),
    NATIVE("createFrag", 0, N_CREATEFRAG),
    NATIVE("insert", 3, N_INSERT), NATIVE("remove", 1, N_REMOVE), NATIVE("clone", 2, N_CLONE),
    NATIVE("byId", 1, N_BYID), NATIVE("query", 2, N_QUERY), NATIVE("queryAll", 2, N_QUERYALL), NATIVE("matches", 2, N_MATCHES),
    NATIVE("contains", 2, N_CONTAINS), NATIVE("connected", 1, N_CONNECTED),
    NATIVE("value", 1, N_VALUE), NATIVE("setValue", 2, N_SETVALUE), NATIVE("checked", 1, N_CHECKED), NATIVE("setChecked", 2, N_SETCHECKED),
    NATIVE("rect", 1, N_RECT), NATIVE("style", 1, N_STYLE), NATIVE("doc", 0, N_DOC), NATIVE("url", 0, N_URL),
    NATIVE("navigate", 2, N_NAVIGATE), NATIVE("setUrl", 2, N_SETURL), NATIVE("reload", 0, N_RELOAD),
    NATIVE("cookie", 0, N_COOKIE), NATIVE("setCookie", 1, N_SETCOOKIE), NATIVE("scrollY", 0, N_SCROLLY), NATIVE("scrollTo", 1, N_SCROLLTO),
    NATIVE("vw", 0, N_VW), NATIVE("vh", 0, N_VH), NATIVE("alert", 1, N_ALERT), NATIVE("log", 2, N_LOG), NATIVE("write", 1, N_WRITE),
    NATIVE("focus", 1, N_FOCUS), NATIVE("focused", 0, N_FOCUSED), NATIVE("submit", 2, N_SUBMIT),
    NATIVE("fetch", 5, N_FETCH), NATIVE("abort", 1, N_ABORT),
    NATIVE("storeGet", 2, N_STORE_GET), NATIVE("storeSet", 3, N_STORE_SET), NATIVE("storeKeys", 1, N_STORE_KEYS), NATIVE("storeClear", 1, N_STORE_CLEAR),
    NATIVE("now", 0, N_NOW), NATIVE("perf", 0, N_PERF), NATIVE("random", 1, N_RANDOM), NATIVE("changed", 0, N_CHANGED),
    NATIVE("setup", 7, N_SETUP), NATIVE("resolve", 2, N_RESOLVE), NATIVE("hit", 2, N_HIT),
    NATIVE("animate", 8, N_ANIMATE), NATIVE("animCtl", 3, N_ANIMCTL), NATIVE("animInfo", 1, N_ANIMINFO),
};

/* ------------------------------------------------------------------ running things */

static int interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    tf_js *j = opaque;
    return j->deadline && (int32_t)(uihost_ms() - j->deadline) > 0;
}

static void arm(tf_js *j) { j->deadline = uihost_ms() + RUN_LIMIT_MS; }

static void report(tf_js *j, const char *where)
{
    JSValue e = JS_GetException(j->ctx);
    const char *msg = JS_ToCString(j->ctx, e);
    char buf[600];
    const char *stack = 0;
    JSValue st = JS_UNDEFINED;
    if (JS_IsObject(e)) {
        st = JS_GetPropertyStr(j->ctx, e, "stack");
        if (JS_IsString(st)) stack = JS_ToCString(j->ctx, st);
    }
    snprintf(buf, sizeof(buf), "%s: %s%s%s", where, msg ? msg : "error", stack ? "\n" : "", stack ? stack : "");
    page_console(j->page, 2, buf);
    if (stack) JS_FreeCString(j->ctx, stack);
    JS_FreeValue(j->ctx, st);
    if (msg) JS_FreeCString(j->ctx, msg);
    JS_FreeValue(j->ctx, e);
}

/* promise jobs (microtasks): all of them, after every script, event and timer */
static void jobs(tf_js *j)
{
    JSContext *c;
    arm(j);
    for (int i = 0; i < 100000; i++) {
        int r = JS_ExecutePendingJob(j->rt, &c);
        if (r <= 0) { if (r < 0) report(j, "promise"); if (r == 0) break; }
    }
    j->deadline = 0;
}

static JSValue call(tf_js *j, JSValueConst fn, int argc, JSValueConst *argv, const char *where)
{
    if (!JS_IsFunction(j->ctx, fn)) return JS_UNDEFINED;
    arm(j);
    JSValue r = JS_Call(j->ctx, fn, JS_UNDEFINED, argc, argv);
    j->deadline = 0;
    if (JS_IsException(r)) { report(j, where); r = JS_UNDEFINED; }
    jobs(j);
    return r;
}

/* ------------------------------------------------------------------ modules */

static char *mod_normalize(JSContext *ctx, const char *base, const char *name, void *opaque)
{
    (void)opaque;
    char abs[1024];
    if (!page_resolve(base, name, abs, sizeof(abs))) { strncpy(abs, name, sizeof(abs) - 1); abs[sizeof(abs) - 1] = 0; }
    size_t l = strlen(abs);
    char *r = js_malloc(ctx, l + 1);
    if (r) memcpy(r, abs, l + 1);
    return r;
}

static JSModuleDef *mod_load(JSContext *ctx, const char *name, void *opaque)
{
    tf_js *j = opaque;
    for (int i = 0; i < j->nmods; i++) {
        if (strcmp(j->mods[i].url, name)) continue;
        if (!j->mods[i].src) { JS_ThrowReferenceError(ctx, "could not fetch module %s", name); return 0; }
        JSValue f = JS_Eval(ctx, j->mods[i].src, (size_t)j->mods[i].len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(f)) return 0;
        JSModuleDef *m = JS_VALUE_GET_PTR(f);
        JS_FreeValue(ctx, f);
        return m;
    }
    /* not here yet: the engine fetches it and the script runs again */
    int known = 0;
    for (int i = 0; i < j->nmissing; i++) if (!strcmp(j->missing[i], name)) known = 1;
    if (!known && j->nmissing < MAX_MISSING) j->missing[j->nmissing++] = strdup(name);
    JS_ThrowReferenceError(ctx, "module %s is still coming", name);
    return 0;
}

const char *tfjs_missing(tf_js *j, int i) { return i < j->nmissing ? j->missing[i] : 0; }

void tfjs_module_source(tf_js *j, const char *url, const char *src, int len)
{
    struct mod *m = realloc(j->mods, (size_t)(j->nmods + 1) * sizeof(*m));
    if (!m) return;
    j->mods = m;
    m[j->nmods].url = strdup(url);
    m[j->nmods].src = src ? strndup_(src, (size_t)len) : 0;
    m[j->nmods].len = len;
    j->nmods++;
}

/* ------------------------------------------------------------------ the engine's side */

static void *jm_malloc(JSMallocState *s, size_t n) { (void)s; return malloc(n); }
static void jm_free(JSMallocState *s, void *p) { (void)s; free(p); }
static void *jm_realloc(JSMallocState *s, void *p, size_t n) { (void)s; return realloc(p, n); }
#ifdef TFIRE_HOST
#include <malloc.h>
static size_t jm_usable(const void *p) { return malloc_usable_size((void *)p); }
#else
extern size_t tf_usable(const void *p);
#define jm_usable tf_usable
#endif
static const JSMallocFunctions jmf = { jm_malloc, jm_free, jm_realloc, jm_usable };

tf_js *tfjs_new(tf_page *p)
{
    tf_js *j = calloc(1, sizeof(*j));
    if (!j) return 0;
    j->page = p;
    j->t0 = uihost_ms();
    j->rt = JS_NewRuntime2(&jmf, 0);
    if (!j->rt) { free(j); return 0; }
    JS_SetMemoryLimit(j->rt, 384u << 20);
    JS_SetMaxStackSize(j->rt, 1536u << 10);         /* (the engine runs on a 2 MB stack of its own) */
    JS_SetInterruptHandler(j->rt, interrupt, j);
    JS_SetModuleLoaderFunc(j->rt, mod_normalize, mod_load, j);
    j->ctx = JS_NewContext(j->rt);
    if (!j->ctx) { JS_FreeRuntime(j->rt); free(j); return 0; }
    JS_SetContextOpaque(j->ctx, j);
    j->fn_proto = j->fn_event = j->fn_winev = j->fn_tick = j->fn_fetch = j->fn_image = j->fn_script = JS_UNDEFINED;

    if (!node_class_id) JS_NewClassID(&node_class_id);
    j->node_class = node_class_id;
    JSClassDef cd = { "Node", 0, 0, 0, 0 };
    JS_NewClass(j->rt, j->node_class, &cd);

    JSValue g = JS_GetGlobalObject(j->ctx);
    JSValue tf = JS_NewObject(j->ctx);
    JS_SetPropertyFunctionList(j->ctx, tf, natives, sizeof(natives) / sizeof(natives[0]));
    JS_SetPropertyStr(j->ctx, g, "__tf", tf);
    JS_FreeValue(j->ctx, g);

    arm(j);
    JSValue r = JS_Eval(j->ctx, tf_prelude, strlen(tf_prelude), "tfire:prelude", JS_EVAL_TYPE_GLOBAL);
    j->deadline = 0;
    if (JS_IsException(r)) report(j, "tFire's prelude");
    JS_FreeValue(j->ctx, r);
    jobs(j);
    return j;
}

void tfjs_free(tf_js *j)
{
    if (!j) return;
    tf_doc *d = page_doc(j->page);
    if (d)
        for (tf_node *n = d->all; n; n = n->all_next)
            if (n->js_obj) { JS_FreeValue(j->ctx, JS_MKPTR(JS_TAG_OBJECT, n->js_obj)); n->js_obj = 0; }
    JSValue *fns[] = { &j->fn_proto, &j->fn_event, &j->fn_winev, &j->fn_tick, &j->fn_fetch, &j->fn_image, &j->fn_script };
    for (unsigned i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) JS_FreeValue(j->ctx, *fns[i]);
    JS_FreeContext(j->ctx);
    JS_FreeRuntime(j->rt);
    for (int i = 0; i < j->nmods; i++) { free(j->mods[i].url); free(j->mods[i].src); }
    free(j->mods);
    for (int i = 0; i < j->nmissing; i++) free(j->missing[i]);
    free(j);
}

void tfjs_node_gone(tf_js *j, tf_node *n)
{
    if (n->js_obj) { JS_FreeValue(j->ctx, JS_MKPTR(JS_TAG_OBJECT, n->js_obj)); n->js_obj = 0; }
}

int tfjs_run(tf_js *j, const char *src, int len, const char *name, tf_node *script, int module)
{
    for (int i = 0; i < j->nmissing; i++) free(j->missing[i]);
    j->nmissing = 0;
    /* document.currentScript */
    JSValue sv = wrap(j, script);
    if (JS_IsFunction(j->ctx, j->fn_script)) { JSValue r = JS_Call(j->ctx, j->fn_script, JS_UNDEFINED, 1, &sv); JS_FreeValue(j->ctx, r); }
    JS_FreeValue(j->ctx, sv);
    arm(j);
    int flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    JSValue r = JS_Eval(j->ctx, src, (size_t)len, name, flags);
    j->deadline = 0;
    int again = 0;
    if (JS_IsException(r)) {
        if (module && j->nmissing) {               /* its imports are fetched first */
            JS_FreeValue(j->ctx, JS_GetException(j->ctx));
            again = 1;
        } else report(j, name);
    }
    JS_FreeValue(j->ctx, r);
    JSValue nul = JS_NULL;
    if (JS_IsFunction(j->ctx, j->fn_script)) { JSValue r2 = JS_Call(j->ctx, j->fn_script, JS_UNDEFINED, 1, &nul); JS_FreeValue(j->ctx, r2); }
    jobs(j);
    return again;
}

int tfjs_event(tf_js *j, tf_node *target, const char *type, int x, int y, int key, const char *keyname)
{
    JSValue a[6] = { wrap(j, target), JS_NewString(j->ctx, type), JS_NewInt32(j->ctx, x), JS_NewInt32(j->ctx, y),
                     JS_NewInt32(j->ctx, key), JS_NewString(j->ctx, keyname ? keyname : "") };
    JSValue r = call(j, j->fn_event, 6, a, type);
    int prevented = JS_ToBool(j->ctx, r);
    JS_FreeValue(j->ctx, r);
    for (int i = 0; i < 6; i++) JS_FreeValue(j->ctx, a[i]);
    return prevented;
}

void tfjs_window_event(tf_js *j, const char *type, int on_document)
{
    JSValue a[2] = { JS_NewString(j->ctx, type), JS_NewBool(j->ctx, on_document) };
    JSValue r = call(j, j->fn_winev, 2, a, type);
    JS_FreeValue(j->ctx, r);
    JS_FreeValue(j->ctx, a[0]);
}

int tfjs_tick(tf_js *j)
{
    JSValue r = call(j, j->fn_tick, 0, 0, "timer");
    int32_t next = -1;
    if (JS_IsNumber(r)) JS_ToInt32(j->ctx, &next, r);
    JS_FreeValue(j->ctx, r);
    return next;
}

void tfjs_fetch_done(tf_js *j, int id, int status, const char *url, const char *headers, int hlen,
                   const char *body, int blen, const char *error)
{
    JSValue a[6] = { JS_NewInt32(j->ctx, id), JS_NewInt32(j->ctx, status), JS_NewString(j->ctx, url ? url : ""),
                     JS_NewStringLen(j->ctx, headers ? headers : "", headers ? (size_t)hlen : 0),
                     body ? JS_NewStringLen(j->ctx, body, (size_t)blen) : JS_NULL,
                     error ? JS_NewString(j->ctx, error) : JS_NULL };
    /* a body that is not UTF-8 text (a picture, say) also goes as an ArrayBuffer, for arrayBuffer() and blob() */
    JSValue r = call(j, j->fn_fetch, 6, a, "fetch");
    JS_FreeValue(j->ctx, r);
    for (int i = 0; i < 6; i++) JS_FreeValue(j->ctx, a[i]);
}

void tfjs_image_loaded(tf_js *j, tf_node *img, int ok)
{
    JSValue a[2] = { wrap(j, img), JS_NewBool(j->ctx, ok) };
    JSValue r = call(j, j->fn_image, 2, a, "image");
    JS_FreeValue(j->ctx, r);
    JS_FreeValue(j->ctx, a[0]);
}
