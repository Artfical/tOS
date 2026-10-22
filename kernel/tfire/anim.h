#ifndef TFIRE_ANIM_H
#define TFIRE_ANIM_H

/*
 * Things that move: CSS transitions, CSS animations (@keyframes) and the Web Animations API (element.animate).
 *
 * style.c computes every element's style as the sheets say (its base values); after each styling anim_restyled
 * compares them with the last ones and starts the transitions the page asks for, starts and ends animations as
 * animation-name comes and goes. anim_apply then writes the values of the moment into the styles, on every
 * frame while something moves, and layout draws them (transforms and opacity included).
 */

#include "style.h"

typedef struct tf_anims tf_anims;

tf_anims *anim_new(void);
void anim_free(tf_anims *a);
void anim_restyled(tf_anims *a, tf_doc *d, tf_styler *st, uint32_t now);
/* the values for now, into the styles: bit 1 = something changed (lay out again), bit 2 = still moving */
int anim_apply(tf_anims *a, uint32_t now);
/* the events that are due (animationstart, transitionend...), one at a time: 1 while there are */
int anim_take_event(tf_anims *a, tf_node **node, char *type, int tcap, char *name, int ncap, int *elapsed_ms, int *id);

/* element.animate(): frames as "offset%{declarations}..." (offset in percent); iter x1000 (-1 infinite).
 * Returns its id. */
int anim_js_add(tf_anims *a, tf_node *n, const char *frames, int len, int dur, int delay, const char *easing,
                int iter, int dir, int fill, uint32_t now);
enum { AJ_PLAY, AJ_PAUSE, AJ_CANCEL, AJ_FINISH, AJ_REVERSE, AJ_SEEK };
void anim_js_ctl(tf_anims *a, int id, int op, int value, uint32_t now);
/* its state (0 idle, 1 running, 2 paused, 3 finished) and its current time in ms */
int anim_js_info(tf_anims *a, int id, int *current, uint32_t now);

/* for layout.c: the transform of a box bw x bh (the translate, rotate and scale properties, then transform), as
 * a matrix a b c d e f about the box's origin; 0 when there is none. And its scale and turn. */
int anim_matrix(const tf_style *s, int bw, int bh, float m[6]);
void anim_matrix_parts(const float m[6], float *sx, float *sy, float *rot_deg);

/* for style.c: what the sheets write */
int anim_prop_id(const char *name, int len);                 /* AP_*, AP_ALL for "all", -1 = not one that moves */
int anim_parse_transform(const char *v, const char *e, int font_px, tf_xop *out, int max);   /* functions found */
int anim_parse_len(const char *v, const char *e, int font_px, tf_len *out);
int anim_parse_color(const char *v, const char *e, uint32_t current, uint32_t *argb);
/* box-shadow and linear-gradient(), into the style */
void anim_set_shadow(tf_style *t, const char *v);
void anim_set_gradient(tf_style *t, const char *v);
void anim_set_transitions(tf_style *t, const char *shorthand, const char *props, const char *durs,
                          const char *timings, const char *delays);
/* name -> keyframes index, for animation-name */
typedef int (*anim_kf_lookup)(void *ctx, const char *name, int len);
void anim_set_animations(tf_style *t, const char *shorthand, const char *const longhands[8], anim_kf_lookup look, void *ctx);
tf_keyframes *anim_parse_keyframes(const char *name, int nlen, const char *body, const char *e);
void anim_free_keyframes(tf_keyframes *k);

#endif
