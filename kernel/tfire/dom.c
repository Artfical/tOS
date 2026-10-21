#include "dom.h"
#include <hubbub/parser.h>
#include <hubbub/tree.h>
#include "tfire_libc.h"
#include "style.h"

/* ------------------------------------------------------------------ nodes */

static tf_node *node_new(tf_doc *d, int type)
{
    tf_node *n = calloc(1, sizeof(*n));
    if (!n) return 0;
    n->type = type;
    n->all_next = d->all;
    d->all = n;
    return n;
}

static lwc_string *intern_lower(const char *s, size_t len)
{
    char buf[64];
    char *b = len < sizeof(buf) ? buf : malloc(len + 1);
    if (!b) return 0;
    for (size_t i = 0; i < len; i++) b[i] = (char)tolower((unsigned char)s[i]);
    b[len] = 0;
    lwc_string *r = 0;
    lwc_intern_string(b, len, &r);
    if (b != buf) free(b);
    return r;
}

static int lwc_is(lwc_string *s, const char *z)
{
    size_t l = strlen(z);
    return s && lwc_string_length(s) == l && !memcmp(lwc_string_data(s), z, l);
}

int dom_is(const tf_node *n, const char *tag) { return n && n->type == TF_ELEM && lwc_is(n->name, tag); }

/* id and class are kept interned, for libcss (it asks for them for every selector it tries) */
static void cache_attr(tf_node *n, const tf_attr *a)
{
    if (lwc_is(a->name, "id")) {
        if (n->id) lwc_string_unref(n->id);
        n->id = 0;
        if (a->value && *a->value) lwc_intern_string(a->value, strlen(a->value), &n->id);
    } else if (lwc_is(a->name, "class")) {
        for (int i = 0; i < n->nclasses; i++) lwc_string_unref(n->classes[i]);
        free(n->classes);
        n->classes = 0;
        n->nclasses = 0;
        const char *p = a->value ? a->value : "";
        int cap = 0;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
            const char *s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') p++;
            if (p == s) break;
            if (n->nclasses == cap) {
                cap = cap ? cap * 2 : 4;
                lwc_string **nc = realloc(n->classes, (size_t)cap * sizeof(*nc));
                if (!nc) break;
                n->classes = nc;
            }
            lwc_string *c = 0;
            if (lwc_intern_string(s, (size_t)(p - s), &c) == lwc_error_ok) n->classes[n->nclasses++] = c;
        }
    }
}

const char *dom_attr(const tf_node *n, const char *name)
{
    if (!n || n->type != TF_ELEM) return 0;
    for (int i = 0; i < n->nattrs; i++)
        if (lwc_is(n->attrs[i].name, name)) return n->attrs[i].value ? n->attrs[i].value : "";
    return 0;
}

static void set_attr_len(tf_node *n, const char *name, size_t nl, const char *value, size_t vl, int replace)
{
    lwc_string *nm = intern_lower(name, nl);
    if (!nm) return;
    for (int i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].name == nm || lwc_is(n->attrs[i].name, lwc_string_data(nm))) {
            lwc_string_unref(nm);
            if (!replace) return;
            free(n->attrs[i].value);
            n->attrs[i].value = strndup_(value, vl);
            cache_attr(n, &n->attrs[i]);
            return;
        }
    }
    tf_attr *na = realloc(n->attrs, (size_t)(n->nattrs + 1) * sizeof(*na));
    if (!na) { lwc_string_unref(nm); return; }
    n->attrs = na;
    na[n->nattrs].name = nm;
    na[n->nattrs].value = strndup_(value, vl);
    cache_attr(n, &na[n->nattrs]);
    n->nattrs++;
}

void dom_set_attr(tf_doc *d, tf_node *n, const char *name, const char *value)
{
    if (!n || n->type != TF_ELEM) return;
    set_attr_len(n, name, strlen(name), value ? value : "", value ? strlen(value) : 0, 1);
    if (d) d->changed = 1;
}

void dom_remove_attr(tf_doc *d, tf_node *n, const char *name)
{
    if (!n || n->type != TF_ELEM) return;
    for (int i = 0; i < n->nattrs; i++) {
        if (!lwc_is(n->attrs[i].name, name)) continue;
        tf_attr gone = n->attrs[i];
        memmove(&n->attrs[i], &n->attrs[i + 1], (size_t)(n->nattrs - i - 1) * sizeof(tf_attr));
        n->nattrs--;
        tf_attr empty = { gone.name, 0 };
        cache_attr(n, &empty);
        lwc_string_unref(gone.name);
        free(gone.value);
        if (d) d->changed = 1;
        return;
    }
}

tf_node *dom_new_element(tf_doc *d, const char *name)
{
    tf_node *n = node_new(d, TF_ELEM);
    if (!n) return 0;
    n->name = intern_lower(name, strlen(name));
    n->ns = HUBBUB_NS_HTML;
    return n;
}

tf_node *dom_new_text(tf_doc *d, const char *text, int len)
{
    tf_node *n = node_new(d, TF_TEXT);
    if (!n) return 0;
    n->text = strndup_(text, (size_t)len);
    n->text_len = n->text ? len : 0;
    return n;
}

void dom_remove(tf_node *c)
{
    tf_node *p = c->parent;
    if (!p) return;
    if (c->prev) c->prev->next = c->next; else p->first = c->next;
    if (c->next) c->next->prev = c->prev; else p->last = c->prev;
    c->parent = c->prev = c->next = 0;
}

void dom_insert_before(tf_node *p, tf_node *c, tf_node *ref)
{
    if (!p || !c || c == p) return;
    if (c->type == TF_FRAG) {
        if (ref && ref->parent != p) ref = 0;
        while (c->first) dom_insert_before(p, c->first, ref);
        return;
    }
    for (tf_node *a = p; a; a = a->parent) if (a == c) return;     /* not into itself */
    if (c->parent) dom_remove(c);
    if (ref && ref->parent != p) ref = 0;
    c->parent = p;
    if (!ref) {
        c->prev = p->last;
        c->next = 0;
        if (p->last) p->last->next = c; else p->first = c;
        p->last = c;
    } else {
        c->next = ref;
        c->prev = ref->prev;
        if (ref->prev) ref->prev->next = c; else p->first = c;
        ref->prev = c;
    }
}

void dom_append(tf_node *p, tf_node *c) { dom_insert_before(p, c, 0); }

tf_node *dom_next(tf_node *n, tf_node *top)
{
    if (n->first) return n->first;
    while (n && n != top) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return 0;
}

tf_node *dom_find(tf_node *from, const char *tag)
{
    for (tf_node *n = from; n; n = dom_next(n, from)) if (dom_is(n, tag)) return n;
    return 0;
}

tf_node *dom_by_id(tf_doc *d, const char *id)
{
    size_t l = strlen(id);
    for (tf_node *n = d->root; n; n = dom_next(n, d->root))
        if (n->type == TF_ELEM && n->id && lwc_string_length(n->id) == l && !memcmp(lwc_string_data(n->id), id, l)) return n;
    return 0;
}

/* ------------------------------------------------------------------ the parser's tree handler */

typedef struct { tf_doc *doc; } build_ctx;

static hubbub_error t_comment(void *ctx, const hubbub_string *data, void **result)
{
    tf_node *n = node_new(((build_ctx *)ctx)->doc, TF_COMMENT);
    if (!n) return HUBBUB_NOMEM;
    n->text = strndup_((const char *)data->ptr, data->len);
    n->text_len = (int)data->len;
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error t_doctype(void *ctx, const hubbub_doctype *dt, void **result)
{
    (void)dt;
    tf_node *n = node_new(((build_ctx *)ctx)->doc, TF_DOCTYPE);
    if (!n) return HUBBUB_NOMEM;
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error t_element(void *ctx, const hubbub_tag *tag, void **result)
{
    tf_node *n = node_new(((build_ctx *)ctx)->doc, TF_ELEM);
    if (!n) return HUBBUB_NOMEM;
    n->name = intern_lower((const char *)tag->name.ptr, tag->name.len);
    n->ns = tag->ns;
    for (uint32_t i = 0; i < tag->n_attributes; i++) {
        const hubbub_attribute *a = &tag->attributes[i];
        set_attr_len(n, (const char *)a->name.ptr, a->name.len, (const char *)a->value.ptr, a->value.len, 0);
    }
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error t_text(void *ctx, const hubbub_string *data, void **result)
{
    tf_node *n = dom_new_text(((build_ctx *)ctx)->doc, (const char *)data->ptr, (int)data->len);
    if (!n) return HUBBUB_NOMEM;
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error t_ref(void *ctx, void *node) { (void)ctx; (void)node; return HUBBUB_OK; }

/* text next to text is one text node, as the DOM has it */
static tf_node *merge_text(tf_node *into, tf_node *t)
{
    char *m = realloc(into->text, (size_t)into->text_len + (size_t)t->text_len + 1);
    if (!m) return into;
    memcpy(m + into->text_len, t->text, (size_t)t->text_len);
    into->text_len += t->text_len;
    m[into->text_len] = 0;
    into->text = m;
    return into;
}

static hubbub_error t_append(void *ctx, void *parent, void *child, void **result)
{
    (void)ctx;
    tf_node *p = parent, *c = child;
    if (c->type == TF_TEXT && p->last && p->last->type == TF_TEXT) { *result = merge_text(p->last, c); return HUBBUB_OK; }
    dom_append(p, c);
    *result = c;
    return HUBBUB_OK;
}

static hubbub_error t_insert_before(void *ctx, void *parent, void *child, void *ref, void **result)
{
    (void)ctx;
    tf_node *p = parent, *c = child, *r = ref;
    if (c->type == TF_TEXT && r && r->prev && r->prev->type == TF_TEXT) { *result = merge_text(r->prev, c); return HUBBUB_OK; }
    dom_insert_before(p, c, r);
    *result = c;
    return HUBBUB_OK;
}

static hubbub_error t_remove(void *ctx, void *parent, void *child, void **result)
{
    (void)ctx; (void)parent;
    dom_remove(child);
    *result = child;
    return HUBBUB_OK;
}

tf_node *dom_new_node(tf_doc *d, int type) { return node_new(d, type); }

static tf_node *clone(tf_doc *d, tf_node *src, int deep)
{
    tf_node *n = node_new(d, src->type);
    if (!n) return 0;
    if (src->name) n->name = lwc_string_ref(src->name);
    n->ns = src->ns;
    for (int i = 0; i < src->nattrs; i++)
        set_attr_len(n, lwc_string_data(src->attrs[i].name), lwc_string_length(src->attrs[i].name),
                     src->attrs[i].value ? src->attrs[i].value : "", src->attrs[i].value ? strlen(src->attrs[i].value) : 0, 1);
    if (src->text) { n->text = strndup_(src->text, (size_t)src->text_len); n->text_len = src->text_len; }
    if (deep)
        for (tf_node *c = src->first; c; c = c->next) {
            tf_node *cc = clone(d, c, 1);
            if (cc) dom_append(n, cc);
        }
    return n;
}

tf_node *dom_clone(tf_doc *d, tf_node *n, int deep)
{
    tf_node *c = clone(d, n, deep);
    if (c && n->value) c->value = strdup(n->value);
    if (c) c->checked = n->checked;
    return c;
}

int dom_contains(const tf_node *a, const tf_node *b)
{
    for (; b; b = b->parent) if (a == b) return 1;
    return 0;
}

int dom_connected(const tf_doc *d, const tf_node *n)
{
    while (n->parent) n = n->parent;
    return n == d->root;
}

static hubbub_error t_clone(void *ctx, void *node, bool deep, void **result)
{
    tf_node *n = clone(((build_ctx *)ctx)->doc, node, deep);
    if (!n) return HUBBUB_NOMEM;
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error t_reparent(void *ctx, void *node, void *new_parent)
{
    (void)ctx;
    tf_node *n = node;
    while (n->first) dom_append(new_parent, n->first);
    return HUBBUB_OK;
}

static hubbub_error t_parent(void *ctx, void *node, bool element_only, void **result)
{
    (void)ctx;
    tf_node *p = ((tf_node *)node)->parent;
    *result = (p && (!element_only || p->type == TF_ELEM)) ? p : 0;
    return HUBBUB_OK;
}

static hubbub_error t_has_children(void *ctx, void *node, bool *result)
{
    (void)ctx;
    *result = ((tf_node *)node)->first != 0;
    return HUBBUB_OK;
}

static hubbub_error t_form(void *ctx, void *form, void *node) { (void)ctx; (void)form; (void)node; return HUBBUB_OK; }

static hubbub_error t_add_attrs(void *ctx, void *node, const hubbub_attribute *a, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++)
        set_attr_len(node, (const char *)a[i].name.ptr, a[i].name.len, (const char *)a[i].value.ptr, a[i].value.len, 0);
    return HUBBUB_OK;
}

static hubbub_error t_quirks(void *ctx, hubbub_quirks_mode mode)
{
    ((build_ctx *)ctx)->doc->quirks = mode != HUBBUB_QUIRKS_MODE_NONE;
    return HUBBUB_OK;
}

static hubbub_error t_encoding(void *ctx, const char *enc) { (void)ctx; (void)enc; return HUBBUB_OK; }
static hubbub_error t_script(void *ctx, void *script) { (void)ctx; (void)script; return HUBBUB_OK; }

static int run_parser(tf_doc *d, tf_node *docnode, const char *html, uint32_t len, const char *charset)
{
    build_ctx bc = { d };
    hubbub_tree_handler th = {
        t_comment, t_doctype, t_element, t_text, t_ref, t_ref, t_append, t_insert_before, t_remove, t_clone,
        t_reparent, t_parent, t_has_children, t_form, t_add_attrs, t_quirks, t_encoding, t_script, &bc,
    };
    hubbub_parser *p;
    /* the charset the server named, else the one the page names in a <meta> */
    if (hubbub_parser_create(charset && *charset ? charset : 0, true, &p) != HUBBUB_OK &&
        hubbub_parser_create("UTF-8", true, &p) != HUBBUB_OK) return -1;
    hubbub_parser_optparams o;
    o.tree_handler = &th;
    hubbub_parser_setopt(p, HUBBUB_PARSER_TREE_HANDLER, &o);
    o.document_node = docnode;
    hubbub_parser_setopt(p, HUBBUB_PARSER_DOCUMENT_NODE, &o);
    o.enable_scripting = true;                         /* <noscript> is not shown: scripts run */
    hubbub_parser_setopt(p, HUBBUB_PARSER_ENABLE_SCRIPTING, &o);
    hubbub_parser_parse_chunk(p, (const uint8_t *)html, len);
    hubbub_parser_completed(p);
    hubbub_parser_destroy(p);
    return 0;
}

void dom_find_title(tf_doc *d)
{
    tf_node *t = dom_find(d->root, "title");
    d->title[0] = 0;
    if (!t) return;
    char *s = dom_text_content(t);
    if (!s) return;
    int o = 0, sp = 1;
    for (char *c = s; *c && o < (int)sizeof(d->title) - 1; c++) {
        int w = *c == ' ' || *c == '\n' || *c == '\t' || *c == '\r';
        if (w) { if (!sp) d->title[o++] = ' '; sp = 1; }
        else { d->title[o++] = *c; sp = 0; }
    }
    while (o && d->title[o - 1] == ' ') o--;
    d->title[o] = 0;
    free(s);
}

tf_doc *dom_parse(const char *html, uint32_t len, const char *charset)
{
    tf_doc *d = calloc(1, sizeof(*d));
    if (!d) return 0;
    d->root = node_new(d, TF_DOC);
    if (!d->root || run_parser(d, d->root, html, len, charset) != 0) { dom_free(d); return 0; }
    dom_find_title(d);
    return d;
}

void dom_free(tf_doc *d)
{
    if (!d) return;
    tf_node *n = d->all;
    while (n) {
        tf_node *nx = n->all_next;
        if (n->name) lwc_string_unref(n->name);
        if (n->id) lwc_string_unref(n->id);
        for (int i = 0; i < n->nclasses; i++) lwc_string_unref(n->classes[i]);
        free(n->classes);
        for (int i = 0; i < n->nattrs; i++) { lwc_string_unref(n->attrs[i].name); free(n->attrs[i].value); }
        free(n->attrs);
        free(n->text);
        free(n->inline_css);
        free(n->value);
        style_node_free(n);
        free(n);
        n = nx;
    }
    free(d);
}

/* ------------------------------------------------------------------ text and markup */

typedef struct { char *s; size_t len, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *ns = realloc(b->s, nc);
        if (!ns) return;
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void text_of(tf_node *n, sbuf *b)
{
    for (tf_node *c = n->first; c; c = c->next) {
        if (c->type == TF_TEXT) sb_put(b, c->text, (size_t)c->text_len);
        else if (c->type == TF_ELEM) text_of(c, b);
    }
}

char *dom_text_content(tf_node *n)
{
    sbuf b = { 0, 0, 0 };
    if (n->type == TF_TEXT || n->type == TF_COMMENT) sb_put(&b, n->text ? n->text : "", (size_t)n->text_len);
    else text_of(n, &b);
    if (!b.s) b.s = strndup_("", 0);
    return b.s;
}

void dom_set_text_content(tf_doc *d, tf_node *n, const char *text)
{
    if (n->type == TF_TEXT) {
        free(n->text);
        n->text = strndup_(text, strlen(text));
        n->text_len = n->text ? (int)strlen(text) : 0;
    } else {
        while (n->first) dom_remove(n->first);
        if (*text) { tf_node *t = dom_new_text(d, text, (int)strlen(text)); if (t) dom_append(n, t); }
    }
    d->changed = 1;
}

static void escape(sbuf *b, const char *s, size_t n, int attr)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_puts(b, "&amp;");
        else if (c == '<') sb_puts(b, "&lt;");
        else if (c == '>') sb_puts(b, "&gt;");
        else if (c == '"' && attr) sb_puts(b, "&quot;");
        else sb_put(b, &c, 1);
    }
}

static int is_void(const tf_node *n)
{
    static const char *const v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                     "param", "source", "track", "wbr", 0 };
    for (int i = 0; v[i]; i++) if (dom_is(n, v[i])) return 1;
    return 0;
}

static void markup(tf_node *n, sbuf *b)
{
    for (tf_node *c = n->first; c; c = c->next) {
        if (c->type == TF_TEXT) {
            if (dom_is(n, "script") || dom_is(n, "style")) sb_put(b, c->text, (size_t)c->text_len);
            else escape(b, c->text, (size_t)c->text_len, 0);
        } else if (c->type == TF_COMMENT) {
            sb_puts(b, "<!--");
            sb_put(b, c->text, (size_t)c->text_len);
            sb_puts(b, "-->");
        } else if (c->type == TF_ELEM) {
            sb_puts(b, "<");
            sb_put(b, lwc_string_data(c->name), lwc_string_length(c->name));
            for (int i = 0; i < c->nattrs; i++) {
                sb_puts(b, " ");
                sb_put(b, lwc_string_data(c->attrs[i].name), lwc_string_length(c->attrs[i].name));
                sb_puts(b, "=\"");
                if (c->attrs[i].value) escape(b, c->attrs[i].value, strlen(c->attrs[i].value), 1);
                sb_puts(b, "\"");
            }
            sb_puts(b, ">");
            if (is_void(c)) continue;
            markup(c, b);
            sb_puts(b, "</");
            sb_put(b, lwc_string_data(c->name), lwc_string_length(c->name));
            sb_puts(b, ">");
        }
    }
}

char *dom_outer_html(tf_node *n)
{
    sbuf b = { 0, 0, 0 };
    if (n->type != TF_ELEM) { char *t = n->type == TF_TEXT ? dom_text_content(n) : strndup_("", 0); return t; }
    tf_node tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.type = TF_FRAG;
    tmp.first = tmp.last = n;
    tf_node *nx = n->next;
    n->next = 0;
    markup(&tmp, &b);
    n->next = nx;
    if (!b.s) b.s = strndup_("", 0);
    return b.s;
}

char *dom_inner_html(tf_node *n)
{
    sbuf b = { 0, 0, 0 };
    markup(n, &b);
    if (!b.s) b.s = strndup_("", 0);
    return b.s;
}

void dom_set_inner_html(tf_doc *d, tf_node *node, const char *html, int len)
{
    while (node->first) dom_remove(node->first);
    /* the fragment is parsed as the body of a document of its own, whose body's children move in */
    tf_node *tmp = node_new(d, TF_DOC);
    if (!tmp) return;
    run_parser(d, tmp, html, (uint32_t)len, "UTF-8");
    tf_node *body = dom_find(tmp, "body");
    tf_node *head = dom_find(tmp, "head");
    if (head) while (head->first) dom_append(node, head->first);   /* (a <style> or <script> goes to the head) */
    if (body) while (body->first) dom_append(node, body->first);
    d->changed = 1;
}

/* ------------------------------------------------------------------ selectors (for scripts) */

/* one compound selector, e.g. div.note#x[data-a=b] */
static int match_compound(tf_node *n, const char *s, const char *e)
{
    if (!n || n->type != TF_ELEM) return 0;
    const char *p = s;
    while (p < e) {
        if (*p == '*') { p++; continue; }
        if (*p == '#' || *p == '.') {
            char kind = *p++;
            const char *q = p;
            while (q < e && *q != '.' && *q != '#' && *q != '[' && *q != ':') q++;
            size_t l = (size_t)(q - p);
            if (kind == '#') {
                if (!n->id || lwc_string_length(n->id) != l || memcmp(lwc_string_data(n->id), p, l)) return 0;
            } else {
                int ok = 0;
                for (int i = 0; i < n->nclasses && !ok; i++)
                    ok = lwc_string_length(n->classes[i]) == l && !memcmp(lwc_string_data(n->classes[i]), p, l);
                if (!ok) return 0;
            }
            p = q;
        } else if (*p == '[') {
            const char *q = ++p;
            while (q < e && *q != ']' && *q != '=' && *q != '~' && *q != '^' && *q != '$' && *q != '*' && *q != '|') q++;
            char name[64];
            size_t nl = (size_t)(q - p) < sizeof(name) - 1 ? (size_t)(q - p) : sizeof(name) - 1;
            for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)p[i]);
            name[nl] = 0;
            const char *have = dom_attr(n, name);
            if (!have) return 0;
            if (q < e && *q != ']') {
                char op = *q == '=' ? '=' : *q++;
                if (q < e && *q == '=') q++;
                char quote = (q < e && (*q == '"' || *q == '\'')) ? *q++ : 0;
                const char *v = q;
                while (q < e && (quote ? *q != quote : *q != ']')) q++;
                size_t vl = (size_t)(q - v), hl = strlen(have);
                int ok = 0;
                if (op == '=') ok = hl == vl && !memcmp(have, v, vl);
                else if (op == '^') ok = hl >= vl && !memcmp(have, v, vl);
                else if (op == '$') ok = hl >= vl && !memcmp(have + hl - vl, v, vl);
                else if (op == '*') { for (size_t i = 0; i + vl <= hl && !ok; i++) ok = !memcmp(have + i, v, vl); }
                else if (op == '~') {
                    const char *h = have;
                    while (*h && !ok) {
                        while (*h == ' ') h++;
                        const char *ws = h;
                        while (*h && *h != ' ') h++;
                        ok = (size_t)(h - ws) == vl && !memcmp(ws, v, vl);
                    }
                } else if (op == '|') ok = (hl == vl && !memcmp(have, v, vl)) || (hl > vl && !memcmp(have, v, vl) && have[vl] == '-');
                if (!ok) return 0;
                if (quote && q < e) q++;
            }
            while (q < e && *q != ']') q++;
            p = q < e ? q + 1 : q;
        } else if (*p == ':') {
            const char *q = ++p;
            while (q < e && *q != '.' && *q != '#' && *q != '[' && *q != ':') q++;
            if (q - p == 11 && !memcmp(p, "first-child", 11)) { tf_node *s2 = n->prev; while (s2 && s2->type != TF_ELEM) s2 = s2->prev; if (s2) return 0; }
            else if (q - p == 10 && !memcmp(p, "last-child", 10)) { tf_node *s2 = n->next; while (s2 && s2->type != TF_ELEM) s2 = s2->next; if (s2) return 0; }
            p = q;                                      /* other pseudo-classes: not checked */
        } else {
            const char *q = p;
            while (q < e && *q != '.' && *q != '#' && *q != '[' && *q != ':') q++;
            size_t l = (size_t)(q - p);
            if (lwc_string_length(n->name) != l || strncasecmp(lwc_string_data(n->name), p, l)) return 0;
            p = q;
        }
    }
    return 1;
}

/* a complex selector [s, e): compounds joined by ' ' or '>', matched right to left */
static int match_complex(tf_node *n, const char *s, const char *e)
{
    while (e > s && e[-1] == ' ') e--;
    if (e <= s) return 0;
    const char *c = e;
    while (c > s && c[-1] != ' ' && c[-1] != '>' && c[-1] != '+' && c[-1] != '~') c--;
    if (!match_compound(n, c, e)) return 0;
    const char *rest = c;
    while (rest > s && rest[-1] == ' ') rest--;
    if (rest <= s) return 1;
    char comb = rest[-1];
    if (comb == '>' || comb == '+' || comb == '~') {
        rest--;
        while (rest > s && rest[-1] == ' ') rest--;
    } else comb = ' ';
    if (comb == '>') return n->parent && match_complex(n->parent, s, rest);
    if (comb == '+') { tf_node *p = n->prev; while (p && p->type != TF_ELEM) p = p->prev; return p && match_complex(p, s, rest); }
    if (comb == '~') { for (tf_node *p = n->prev; p; p = p->prev) if (p->type == TF_ELEM && match_complex(p, s, rest)) return 1; return 0; }
    for (tf_node *a = n->parent; a && a->type == TF_ELEM; a = a->parent) if (match_complex(a, s, rest)) return 1;
    return 0;
}

int dom_matches(tf_node *n, const char *sel)
{
    const char *s = sel;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        const char *e = s;
        int depth = 0;
        while (*e && (depth || *e != ',')) { if (*e == '[' || *e == '(') depth++; if (*e == ']' || *e == ')') depth--; e++; }
        if (e > s && match_complex(n, s, e)) return 1;
        s = e;
    }
    return 0;
}

tf_node *dom_query(tf_node *top, const char *sel)
{
    for (tf_node *n = top->first ? dom_next(top, top) : 0; n; n = dom_next(n, top))
        if (n->type == TF_ELEM && dom_matches(n, sel)) return n;
    return 0;
}

int dom_query_all(tf_node *top, const char *sel, tf_node **out, int max)
{
    int k = 0;
    for (tf_node *n = top->first ? dom_next(top, top) : 0; n && k < max; n = dom_next(n, top))
        if (n->type == TF_ELEM && dom_matches(n, sel)) out[k++] = n;
    return k;
}
