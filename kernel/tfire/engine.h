#ifndef TFIRE_ENGINE_H
#define TFIRE_ENGINE_H

/*
 * tFire's engine: one web page, from its address to a display list.
 *
 * The page is fetched, parsed into a document (dom.c), its style sheets and scripts fetched in document order,
 * the sheets cascaded (style.c), the scripts run (js.c, QuickJS), the result laid out (layout.c) and its
 * pictures fetched after. While it is open its timers, its own fetches (fetch, XMLHttpRequest) and whatever
 * its scripts change keep it going: page_tick does a little of all that each time the desktop asks.
 *
 * Fetches go one at a time (tOS has one TCP connection), in this order: the page, its style sheets and
 * scripts, what its scripts ask for, its pictures.
 */

#include <stdint.h>
#include "dom.h"
#include "style.h"
#include "layout.h"

typedef struct tf_page tf_page;

/* what the engine needs from the window it is drawn in */
typedef struct {
    tf_env env;                                /* fonts, for layout */
    void (*alert)(void *ctx, const char *msg); /* alert(): shown, never waited for */
    void *ctx;
} tf_host;

/* a request: GET unless method says otherwise; body with its content type */
typedef struct {
    char url[1024];
    char method[8];
    char ctype[80];
    char *body;                                /* malloc'd (or 0) */
    int body_len;
    int replace;                               /* not a new entry in the history (location.replace) */
    int download;                              /* save what comes, whatever it is (<a download>) */
    char dl_name[128];                         /* the name <a download="..."> gives it */
} tf_request;

/* a decoded picture */
typedef struct {
    uint8_t *rgba;
    int w, h;                                  /* its pixels */
    int dw, dh;                                /* the size it is shown at when nothing says otherwise */
} tf_picture;

tf_page *page_open(const tf_request *req, const tf_host *host);
/* a page made from HTML already in hand (tFire's own pages), at url */
tf_page *page_open_html(const char *url, const char *html, int len, const tf_host *host);
void page_close(tf_page *p);

/* does what is due: fetches, scripts, timers, style and layout for a viewport w x h.
 * Returns 1 when what is shown changed. */
int page_tick(tf_page *p, int w, int h);
/* 1 while the page itself or what it needs to be shown is still coming (the loading bar) */
int page_loading(tf_page *p);
/* 1 once the answer to the page's own request is here (a page to show, an error, or a download) */
int page_fetched(tf_page *p);
/* The page was a file to save rather than show (a type tFire cannot show, Content-Disposition: attachment, or
 * <a download>): 1, its name, its bytes (data, len) and the buffer they are in (raw: the caller frees it with
 * the kernel's free). Once. */
int page_take_download(tf_page *p, char *name, int cap, void **raw, const uint8_t **data, uint32_t *len);

const char *page_url(tf_page *p);
const char *page_title(tf_page *p);
tf_layout *page_layout(tf_page *p);            /* the display list (document coordinates); never 0 */
tf_picture *page_picture(tf_page *p, tf_node *img);   /* an <img>'s picture, or 0 while there is none */
tf_picture *page_mask(tf_page *p, tf_node *n);        /* an element's mask-image (an icon), or 0 */
tf_node *page_focus(tf_page *p);               /* the form field being typed in, or 0 */
int page_caret_on(tf_page *p);

/* the window asks: a press at (x, y) in document coordinates; a key (UI_KEY_* or a character) */
void page_click(tf_page *p, int x, int y);
int page_hover(tf_page *p, int x, int y);          /* the pointer over (x, y) of the page; x < 0: it left. 1 = something changed */
const char *page_link_at(tf_page *p, int x, int y);
/* 1 = the page took the key (a field, or a script that wanted it); 0 = the window may scroll with it */
int page_key(tf_page *p, int k);
void page_scrolled(tf_page *p, int y);         /* the window scrolled: window.scrollY */
/* where the page wants the window scrolled (location.hash, scrollTo); -1 = nowhere */
int page_take_scroll(tf_page *p);
/* the page wants another address shown (a link, a form, location): 1 and the request in *out */
int page_take_navigation(tf_page *p, tf_request *out);
/* history.pushState/replaceState changed the address without loading: 1 and the new one in out */
int page_take_url_change(tf_page *p, char *out, int cap);

/* the address href means on a page at base; 0 = none (javascript:, mailto:, a bad one) */
int page_resolve(const char *base, const char *href, char *out, int cap);

/* ------------------------------------------------------------------ for js.c (the engine's side of scripts) */

tf_doc *page_doc(tf_page *p);
void page_changed(tf_page *p);                 /* the tree changed: style and layout again */
void page_layout_now(tf_page *p);              /* layout made current before a script reads sizes */
void page_node_inserted(tf_page *p, tf_node *n);   /* a script put n in the document: scripts and sheets in it */
void page_navigate(tf_page *p, const char *url, int replace);
void page_set_url(tf_page *p, const char *url, int replace);   /* history.pushState */
void page_reload(tf_page *p);
void page_scroll_to(tf_page *p, int y);
int page_scroll_y(tf_page *p);
int page_viewport_w(tf_page *p);
int page_viewport_h(tf_page *p);
void page_alert(tf_page *p, const char *msg);
void page_write(tf_page *p, const char *html, int len);    /* document.write */
void page_focus_node(tf_page *p, tf_node *n);  /* element.focus(); 0 = blur */
void page_submit(tf_page *p, tf_node *form, tf_node *submitter);   /* form.submit() (no submit event) */
/* fetch()/XMLHttpRequest: starts one; js_fetch_done is called with id when it ends */
int page_fetch(tf_page *p, int id, const char *method, const char *url, const char *headers, const char *body, int body_len);
void page_fetch_abort(tf_page *p, int id);
/* a picture of url for new Image(): fetched like the page's own (onload follows) */
void page_console(tf_page *p, int level, const char *msg);
/* localStorage and sessionStorage, per origin, for as long as tOS runs (session: per window) */
char *page_storage_get(tf_page *p, int session, const char *key);     /* malloc'd, or 0 */
void page_storage_set(tf_page *p, int session, const char *key, const char *value);   /* value 0 = remove */
int page_storage_keys(tf_page *p, int session, char ***keys);          /* malloc'd array of malloc'd */
void page_storage_clear(tf_page *p, int session);
/* element.animate() (anim.c's anim_js_*) */
int page_anim_add(tf_page *p, tf_node *n, const char *frames, int len, int dur, int delay, const char *easing, int iter, int dir, int fill);
void page_anim_ctl(tf_page *p, int id, int op, int value);
int page_anim_info(tf_page *p, int id, int *current);

/* ------------------------------------------------------------------ js.c: the scripts of a page */

typedef struct tf_js tf_js;
tf_js *tfjs_new(tf_page *p);
void tfjs_free(tf_js *j);
/* runs a script (as the document's), name (its address) for its errors and imports; module: as an ES module.
 * Returns 1 when a module imports others not fetched yet: js_missing lists them, js_module_source gives them,
 * and the script is run again after. */
int tfjs_run(tf_js *j, const char *src, int len, const char *name, tf_node *script, int module);
const char *tfjs_missing(tf_js *j, int i);        /* 0 after the last */
void tfjs_module_source(tf_js *j, const char *url, const char *src, int len);   /* src 0: it could not be fetched */
/* an event at node: type, plus the mouse position or key; returns 1 if a listener called preventDefault */
int tfjs_event(tf_js *j, tf_node *target, const char *type, int x, int y, int key, const char *keyname);
/* an event that only window and document see: "DOMContentLoaded", "load", "resize", "scroll", "popstate"... */
void tfjs_window_event(tf_js *j, const char *type, int on_document);
/* timers and promise jobs that are due; returns ms until the next timer (-1: none) */
int tfjs_tick(tf_js *j);
void tfjs_fetch_done(tf_js *j, int id, int status, const char *url, const char *headers, int hlen,
                   const char *body, int blen, const char *error);
void tfjs_node_gone(tf_js *j, tf_node *n);       /* (the page is closing) */
void tfjs_image_loaded(tf_js *j, tf_node *img, int ok);

#endif
