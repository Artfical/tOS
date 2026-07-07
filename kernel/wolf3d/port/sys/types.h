/* mode_t (for sys/stat.h's mkdir()) doesn't need a real definition --
 * ssize_t does, for open()/read()/write()'s declarations in fcntl.h.
 */
#ifndef WOLF3D_SYS_TYPES_H
#define WOLF3D_SYS_TYPES_H

typedef int ssize_t;

#endif
