#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Fixed hardware output format via SB16 DMA */
#define AUDIO_OUT_RATE  22050
#define AUDIO_OUT_BITS  8
#define AUDIO_OUT_CH    1

/* DMA chunk size: ~186ms at 22050 Hz (must be power-of-2 ≤ 65536 for DMA) */
#define AUDIO_DMA_SIZE  4096

void  audio_init(void);
int   audio_available(void);            /* 1 if SB16 present */
void  audio_set_volume(uint8_t vol);    /* 0-100 */

/* Submit one DMA chunk of 8-bit unsigned PCM, mono, at AUDIO_OUT_RATE.
   len ≤ AUDIO_DMA_SIZE.  Returns 0 on success, -1 if not available. */
int   audio_submit(const uint8_t *pcm, uint32_t len);

int   audio_busy(void);   /* 1 while DMA transfer in progress */
void  audio_stop(void);

#endif
