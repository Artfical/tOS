/* Stub: id_ca.cpp includes this unconditionally on non-_WIN32 builds
 * but never actually calls readv()/writev() -- everything else here
 * goes through open()/read()/write() (see fcntl.h/wolf_compat.cpp) or
 * kernel/lib/stdio.h's fopen/fread/etc. Empty on purpose. */
#ifndef WOLF3D_SYS_UIO_H
#define WOLF3D_SYS_UIO_H
#endif
