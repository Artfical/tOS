/* No real math.h exists elsewhere in this freestanding kernel to wrap
 * (only kernel/micropython/ports/tos/math_stubs.c's float functions,
 * used ad hoc without a shared header) -- DOOM's generic source only
 * ever needs the two declared below (see kernel/doom sources: one atan()
 * call in r_main.c, one fabs() call in v_video.c), implemented in
 * doom_compat.c by delegating to math_stubs.c's already-tested float
 * versions rather than duplicating trig/abs logic. */
#ifndef DOOM_MATH_H
#define DOOM_MATH_H

double atan(double x);
double fabs(double x);

#endif
