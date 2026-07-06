/* tOS sound backend for doomgeneric -- not part of id Software/
 * doomgeneric's source. Implements the sound_module_t interface
 * kernel/doom/i_sound.c expects (see its FEATURE_SOUND-gated
 * DG_sound_module slot) by driving tOS's own kernel/audio/audio.c
 * mixer directly instead of SDL_mixer.
 *
 * Known limitations, all a direct consequence of tOS's audio.c only
 * supporting one active buffer at a time (see its audio_busy()/
 * audio_submit()):
 * - No real multichannel mixing. DOOM plays several sound effects at
 *   once (footsteps, gunfire, monster noises); here, starting a new
 *   one always cuts off whatever was already playing.
 * - audio_submit() briefly busy-waits for the previous sound to
 *   finish before starting a new one if called while already playing
 *   (see its own comment) -- so trigger-happy scenes can introduce a
 *   short stall rather than overlapping cleanly.
 * - audio_submit() truncates anything over AUDIO_DMA_SIZE (4096)
 *   bytes, so only roughly the first ~185ms (at the fixed 22050Hz
 *   output rate) of a longer sound effect plays.
 * - Music is not implemented (DG_music_module below always reports
 *   init failure) -- that needs MUS-to-something playable, an
 *   entirely separate synthesis job, out of scope here.
 */
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "audio.h"
#include <stdio.h>
#include <stdint.h>

/* DMX sound lump format (the only kind DOOM's own WADs contain):
 * a 3-field, 8-byte header --
 *   u16 format (always 3)
 *   u16 sample rate, Hz (typically 11025 for DOOM's stock sounds)
 *   u32 sample count
 * followed by that many 8-bit unsigned PCM samples -- which happens
 * to already match audio.c's AUDIO_OUT_BITS/AUDIO_OUT_CH (8-bit
 * unsigned mono) exactly, so the only real conversion needed is
 * resampling from the lump's rate to AUDIO_OUT_RATE. */
static uint8_t tos_snd_buf[AUDIO_DMA_SIZE];

static boolean tos_sfx_init(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    if (!audio_available()) audio_init();
    return audio_available();
}

static void tos_sfx_shutdown(void)
{
}

static int tos_sfx_getsfxlumpnum(sfxinfo_t *sfxinfo)
{
    char name[9];
    sprintf(name, "ds%s", sfxinfo->name);
    return W_CheckNumForName(name);
}

static void tos_sfx_update(void)
{
}

static void tos_sfx_updateparams(int channel, int vol, int sep)
{
    (void)channel; (void)vol; (void)sep;
}

static int tos_sfx_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    (void)vol; (void)sep;

    if (sfxinfo->lumpnum < 0) return -1;
    int len = W_LumpLength(sfxinfo->lumpnum);
    if (len < 9) return -1;

    const uint8_t *data = (const uint8_t *)W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    uint32_t rate = (uint32_t)(data[2] | (data[3] << 8));
    uint32_t samples = (uint32_t)(data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24));
    if (rate == 0 || samples == 0 || samples > (uint32_t)len - 8) return -1;
    const uint8_t *pcm = data + 8;

    /* Nearest-neighbor resample from the lump's native rate to
     * AUDIO_OUT_RATE -- more than good enough for short sound
     * effects, and audio_submit() would truncate a fancier
     * resampler's output at AUDIO_DMA_SIZE anyway. */
    uint32_t out_len = (uint32_t)((uint64_t)samples * AUDIO_OUT_RATE / rate);
    if (out_len > AUDIO_DMA_SIZE) out_len = AUDIO_DMA_SIZE;
    for (uint32_t i = 0; i < out_len; i++) {
        uint32_t src = (uint32_t)((uint64_t)i * rate / AUDIO_OUT_RATE);
        if (src >= samples) src = samples - 1;
        tos_snd_buf[i] = pcm[src];
    }

    audio_submit(tos_snd_buf, out_len);
    return channel;
}

static void tos_sfx_stop(int channel)
{
    (void)channel;
    audio_stop();
}

static boolean tos_sfx_isplaying(int channel)
{
    (void)channel;
    return audio_busy() != 0;
}

static snddevice_t tos_sound_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    tos_sound_devices,
    1, /* number of devices in the list above */
    tos_sfx_init,
    tos_sfx_shutdown,
    tos_sfx_getsfxlumpnum,
    tos_sfx_update,
    tos_sfx_updateparams,
    tos_sfx_start,
    tos_sfx_stop,
    tos_sfx_isplaying,
    NULL, /* CacheSounds -- nothing to precache, lumps are read on demand */
};

/* Music needs MUS-to-something-playable synthesis, an entirely
 * separate job from digitized sound effects above -- not implemented,
 * so this module always fails to initialize and DOOM just runs
 * without music, same as it already does with silence hardware. */
static boolean tos_mus_init(void) { return false; }
static void tos_mus_shutdown(void) {}
static void tos_mus_setvolume(int volume) { (void)volume; }
static void tos_mus_pause(void) {}
static void tos_mus_resume(void) {}
static void *tos_mus_register(void *data, int len) { (void)data; (void)len; return NULL; }
static void tos_mus_unregister(void *handle) { (void)handle; }
static void tos_mus_play(void *handle, boolean looping) { (void)handle; (void)looping; }
static void tos_mus_stop(void) {}
static boolean tos_mus_isplaying(void) { return false; }
static void tos_mus_poll(void) {}

music_module_t DG_music_module = {
    tos_sound_devices,
    1,
    tos_mus_init,
    tos_mus_shutdown,
    tos_mus_setvolume,
    tos_mus_pause,
    tos_mus_resume,
    tos_mus_register,
    tos_mus_unregister,
    tos_mus_play,
    tos_mus_stop,
    tos_mus_isplaying,
    tos_mus_poll,
};
