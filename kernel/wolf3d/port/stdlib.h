/* Wraps kernel/lib/stdlib.h in extern "C" -- see string.h in this
 * same directory for why. getenv()/system()/atof() are declared but
 * NOT defined again here: kernel/doom/port/doom_compat.c already
 * implements all three as plain (C-linkage) global symbols, and both
 * kernel/doom/'s and kernel/wolf3d/'s object files link into the same
 * final kernel/tOS.elf, so re-defining any of them here would be a
 * duplicate-symbol error -- this just declares the matching
 * prototypes so kernel/wolf3d/'s callers can see them, and lets the
 * linker resolve to DOOM's existing implementations. Not the
 * cleanest layering (wolf3d ending up dependent on doom's compat
 * file), but pragmatic given how small and genuinely OS-agnostic
 * those three functions are. atexit() is new -- nothing else in tOS
 * has needed real libc atexit() semantics before now -- so it's
 * implemented for real in wolf_compat.cpp. */
#ifndef WOLF3D_STDLIB_COMPAT_H
#define WOLF3D_STDLIB_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <stdlib.h>

char *getenv(const char *name);
int system(const char *command);
double atof(const char *s);
int atexit(void (*func)(void));

#ifdef __cplusplus
}
#endif

#endif
