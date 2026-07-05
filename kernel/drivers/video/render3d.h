#ifndef RENDER3D_H
#define RENDER3D_H

/* Filled, flat-shaded, z-buffered software rasterizer demo -- rotating
 * lit cube rendered through the real Bochs/VBE pixel framebuffer (see
 * bochs.h), the same one DOOM uses. Runs until Escape is pressed. */
void render3d_run(void);

#endif
