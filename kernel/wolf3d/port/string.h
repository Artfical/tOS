/* Wraps kernel/lib/string.h (found via #include_next once this
 * directory's earlier -I position is skipped past) in extern "C" --
 * needed because kernel/lib's own headers aren't C++-aware, and
 * without this, g++ would give every declared function C++ (mangled)
 * linkage while the actual kernel/lib .c implementations are compiled
 * as plain C (unmangled), an unresolved-symbol link failure waiting
 * to happen for every single one of them. Also adds the handful of
 * extra string functions kernel/wolf3d's source calls that tOS's
 * libc doesn't happen to provide -- see wolf_compat.cpp. */
#ifndef WOLF3D_STRING_COMPAT_H
#define WOLF3D_STRING_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <string.h>

char *strdup(const char *s);

#ifdef __cplusplus
}
#endif

#endif
