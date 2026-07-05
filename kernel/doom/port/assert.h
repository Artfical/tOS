/* Only sha1.c includes this. __assert_fail() itself already exists as
 * a linked-in global symbol (kernel/micropython/ports/tos/math_stubs.c
 * needed the same thing for its own porting job) -- just declaring it
 * here avoids pulling in that whole port's include directory for one
 * function. */
#ifndef DOOM_ASSERT_H
#define DOOM_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
void __assert_fail(const char *expr, const char *file, int line);
#endif

#endif
