/* wl_menu.cpp's CheckForEpisodes()/config-dir creation is the only
 * thing here that needs anything from sys/stat.h -- a single
 * best-effort mkdir(path, mode) call. Declared, not defined: kernel/
 * doom/port/sys/stat.h's doom_compat.c already implements a matching
 * mkdir(const char*, unsigned) as a plain (C-linkage) global symbol,
 * and kernel/doom/'s and kernel/wolf3d/'s object files link into the
 * same final kernel/tOS.elf, so this just declares the prototype and
 * lets the linker resolve to DOOM's existing implementation (see
 * kernel/wolf3d/port/stdlib.h for the same reasoning applied to
 * getenv()/system()/atof()). */
#ifndef WOLF3D_SYS_STAT_H
#define WOLF3D_SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

int mkdir(const char *path, unsigned mode);

/* wl_menu.cpp's CheckForEpisodes() only ever checks stat()'s return
 * value (whether the config directory exists at all), never any
 * field of the struct itself -- so this is a placeholder shape,
 * backed by kernel/fs/vfs.h's vfs_stat() (see wolf_compat.cpp). */
struct stat { int st_mode; };
int stat(const char *path, struct stat *buf);

#ifdef __cplusplus
}
#endif

#endif
