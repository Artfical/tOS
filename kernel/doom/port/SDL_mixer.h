#ifndef DOOM_PORT_SDL_MIXER_H
#define DOOM_PORT_SDL_MIXER_H
/* kernel/doom/i_sound.c #includes <SDL_mixer.h> whenever FEATURE_SOUND
 * is defined, but (checked before enabling FEATURE_SOUND for tOS)
 * never actually calls any Mix_*() function or references any
 * SDL_mixer type in that file -- the include is unconditional dead
 * weight for this particular vendored source file. tOS has no SDL and
 * needs no real SDL_mixer functionality (kernel/doom/port/
 * i_sound_tos.c drives tOS's own audio.c mixer directly instead), so
 * this stub just needs to exist to satisfy the #include. */
#endif
