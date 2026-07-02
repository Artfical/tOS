#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define AUDIO_OUT_RATE  22050
#define AUDIO_OUT_BITS  8
#define AUDIO_OUT_CH    1
#define AUDIO_DMA_SIZE  4096

void  audio_init(void);
int   audio_available(void);
void  audio_set_volume(uint8_t vol);
int   audio_submit(const uint8_t *pcm, uint32_t len);
int   audio_busy(void);
void  audio_stop(void);

/* Returns a short string describing the active backend: "SB16", "AC97", or "" */
const char *audio_backend_name(void);

/* Macro for compile-time use is not possible (runtime); use the function above.
   This empty define silences the toolbar label at compile time if needed. */
#define AUDIO_BACKEND_NAME "Audio OK"

#endif
