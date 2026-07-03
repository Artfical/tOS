#include "sound.h"
#include "pcspkr.h"

static int enabled = 1;

void sound_set_enabled(int on) { enabled = on ? 1 : 0; }
int  sound_get_enabled(void)   { return enabled; }

void sound_click(void)
{
    if (!enabled) return;
    pcspkr_beep(1800, 15);
}

void sound_open(void)
{
    if (!enabled) return;
    pcspkr_beep(660, 30);
    pcspkr_beep(990, 40);
}

void sound_close(void)
{
    if (!enabled) return;
    pcspkr_beep(880, 30);
    pcspkr_beep(550, 40);
}

void sound_error(void)
{
    if (!enabled) return;
    pcspkr_beep(220, 60);
    pcspkr_beep(220, 60);
}
