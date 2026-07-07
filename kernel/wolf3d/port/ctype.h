/* Wraps kernel/lib/ctype.h in extern "C" -- see string.h in this same
 * directory for why. */
#ifndef WOLF3D_CTYPE_COMPAT_H
#define WOLF3D_CTYPE_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <ctype.h>

#ifdef __cplusplus
}
#endif

#endif
