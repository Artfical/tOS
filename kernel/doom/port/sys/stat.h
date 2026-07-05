/* Stub: m_misc.c's M_MakeDirectory() is the only thing in DOOM's
 * generic source that needs anything from here -- a single
 * best-effort mkdir(path, mode) call (see doom_compat.c). Everything
 * else DOOM does is regular buffered file I/O through
 * kernel/lib/stdio.h's fopen/fread/etc, which needs nothing from
 * sys/stat.h at all. */
#ifndef DOOM_SYS_STAT_H
#define DOOM_SYS_STAT_H

int mkdir(const char *path, unsigned mode);

#endif
