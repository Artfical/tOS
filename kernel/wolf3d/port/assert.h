/* __assert_fail() already exists as a linked-in global symbol
 * (kernel/micropython/ports/tos/math_stubs.c needed the same thing,
 * kernel/doom/port/assert.h reuses it too) -- just declaring it here
 * avoids pulling in another port's whole include directory for one
 * function. */
#ifndef WOLF3D_ASSERT_H
#define WOLF3D_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else

#ifdef __cplusplus
extern "C" {
#endif
void __assert_fail(const char *expr, const char *file, int line);
#ifdef __cplusplus
}
#endif

#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
