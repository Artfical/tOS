#ifndef TOS_WOLF3D_SDL_MIXER_H
#define TOS_WOLF3D_SDL_MIXER_H
/* tOS compatibility shim for the small slice of SDL_mixer's API
 * kernel/wolf3d/id_sd.cpp actually uses -- a classic multichannel
 * sample mixer with stereo panning, plus a "post mix" hook the engine
 * uses to inject its own AdLib/PC speaker software synthesis
 * (SDL_PCMixCallback/SDL_IMFMusicPlayer, both defined in id_sd.cpp
 * itself, not part of real SDL_mixer). tOS's own kernel/audio/audio.c
 * only supports one active playback buffer at a time (see its
 * audio_submit()), so this backing implementation
 * (kernel/wolf3d/port/sdl_shim.cpp) can't offer true multichannel
 * mixing or the post-mix hook either -- see the README's Wolfenstein
 * 3D section for exactly what that means in practice. */
#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int allocated;
    Uint8 *abuf;
    Uint32 alen;
    Uint8 volume;
} Mix_Chunk;

#define AUDIO_S16SYS 0x8010
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x00000001
/* Real SDL_mixer's default MIX_CHANNELS -- the engine sizes its own
 * per-channel sound-position tracking array off this constant, so it
 * needs to match whatever Mix_ReserveChannels()/Mix_GroupChannels()
 * calls actually request (2 reserved + the rest grouped, see
 * id_sd.cpp's SD_Startup()), even though tOS's single-buffer
 * audio.c can't really honor more than one of them at a time. */
#define MIX_CHANNELS 8
#define MIX_MAX_VOLUME 128

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize,
                         const char *device, int allowed_changes);
void Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
int Mix_ReserveChannels(int num);
int Mix_GroupChannels(int from, int to, int tag);
int Mix_GroupAvailable(int tag);
int Mix_GroupOldest(int tag);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_HaltChannel(int channel);
int Mix_SetPanning(int channel, Uint8 left, Uint8 right);
void Mix_ChannelFinished(void (*channel_finished)(int channel));
void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);
void Mix_SetPostMix(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);
const char *Mix_GetError(void);

#ifdef __cplusplus
}
#endif

#endif
