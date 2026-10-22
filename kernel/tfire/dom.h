#ifndef TFIRE_DOM_H
#define TFIRE_DOM_H

/*
 * tFire's document tree.
 *
 * libhubbub (NetSurf's HTML5 parser) builds it through the tree handler in dom.c; libcss matches selectors
 * against it through the handler in style.c; the JavaScript engine (js.c) reads and changes it. Nodes are
 * never freed one by one while the page lives: they stay on the document's list and go with the page, so a
 * node removed from the tree by a script cannot leave a dangling pointer anywhere.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <libwapcaplet/libwapcaplet.h>

enum { TF_DOC = 1, TF_ELEM, TF_TEXT, TF_COMMENT, TF_DOCTYPE, TF_FRAG };

typedef struct tf_attr {
    lwc_string *name;          /* lower case */
    char *value;
} tf_attr;

struct tf_style;
struct css_select_results;

typedef struct tf_node {
    int type;
    struct tf_node *parent, *first, *last, *prev, *next;
    struct tf_node *all_next;              /* every node of the document, for freeing */
    lwc_string *name;                      /* element: tag name, lower case */
    int ns;                                /* hubbub namespace (HTML, SVG, MathML) */
    tf_attr *attrs;
    int nattrs;
    lwc_string *id;                        /* the id attribute, interned (or 0) */
    lwc_string **classes;                  /* the class attribute split, interned */
    int nclasses;
    char *text;                            /* text and comments */
    int text_len;
    void *css_data;                        /* libcss's own data for the node */
    struct css_select_results *sel;        /* its computed style (from libcss) */
    struct tf_style *style;                /* that style in the form layout uses */
    char *inline_css;                      /* style="" as last parsed (to see when a script changed it) */
    void *inline_sheet;                    /* that, as a libcss style sheet */
    void *js_obj;                          /* the JavaScript object standing for it (a heap pointer) */
    void *anim;                            /* what moves on it (anim.c), or 0 */
    char *value;                           /* form fields: what is typed in */
    int checked;
    int lx, ly, lw, lh;                    /* where layout put it (document coordinates) */
    int img;                               /* <img>: its picture in the page's list, plus one (0 = none yet) */
    int mimg;                              /* its mask-image (an icon), the same way */
    uint8_t started;                       /* <script>: run (or on its way); <link>/<style>: its sheet is in */
    uint8_t laid;                          /* layout gave it a box this time */
} tf_node;

typedef struct tf_doc {
    tf_node *root;                         /* the document node */
    tf_node *all;                          /* every node made for it */
    int quirks;
    int changed;                           /* the tree changed: style and layout again */
    char title[128];
} tf_doc;

/* charset: what the server said (0 = what the page's <meta> says) */
tf_doc *dom_parse(const char *html, uint32_t len, const char *charset);
void dom_find_title(tf_doc *d);                   /* d->title from its <title> again */
void dom_free(tf_doc *d);

tf_node *dom_new_element(tf_doc *d, const char *name);
tf_node *dom_new_text(tf_doc *d, const char *text, int len);
tf_node *dom_new_node(tf_doc *d, int type);                      /* a comment or a document fragment */
tf_node *dom_clone(tf_doc *d, tf_node *n, int deep);
char *dom_outer_html(tf_node *node);             /* malloc'd */
int dom_contains(const tf_node *a, const tf_node *b);           /* b is a or below it */
int dom_connected(const tf_doc *d, const tf_node *n);             /* in the document */
void dom_append(tf_node *parent, tf_node *child);
void dom_insert_before(tf_node *parent, tf_node *child, tf_node *ref);   /* a fragment: its children go in */
void dom_remove(tf_node *child);
/* parses an HTML fragment (innerHTML) and puts what it makes in place of node's children */
void dom_set_inner_html(tf_doc *d, tf_node *node, const char *html, int len);
char *dom_inner_html(tf_node *node);             /* malloc'd */
char *dom_text_content(tf_node *node);           /* malloc'd */
void dom_set_text_content(tf_doc *d, tf_node *node, const char *text);

const char *dom_attr(const tf_node *n, const char *name);    /* 0 = not there */
void dom_set_attr(tf_doc *d, tf_node *n, const char *name, const char *value);
void dom_remove_attr(tf_doc *d, tf_node *n, const char *name);
int dom_is(const tf_node *n, const char *tag);                 /* an element with this tag name */
tf_node *dom_find(tf_node *from, const char *tag);              /* first such element below from */
tf_node *dom_by_id(tf_doc *d, const char *id);
tf_node *dom_next(tf_node *n, tf_node *top);                    /* depth-first order, staying below top */

/* selectors for querySelector: tag, #id, .class, [attr], [attr=v], descendant and child combinators, lists;
 * :hover and :focus as these say (style_set_state sets them) */
extern tf_node *dom_hover, *dom_focus;
int dom_matches(tf_node *n, const char *selector);
tf_node *dom_query(tf_node *top, const char *selector);
int dom_query_all(tf_node *top, const char *selector, tf_node **out, int max);

#endif
