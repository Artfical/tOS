#include "gui.h"
#include "wm.h"

static int gui_active = 0;

int gui_is_active(void)
{
    return gui_active;
}

void gui_init(void)
{
    wm_init();
    gui_active = 1;
}

void gui_poll(void)
{
    if (gui_active) wm_poll();
}
