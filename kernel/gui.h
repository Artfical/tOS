#ifndef GUI_H
#define GUI_H

#include <stdint.h>

#define GUI_TITLE_ROW 0
#define GUI_TERM_ROW 1
#define GUI_TERM_HEIGHT 23

void gui_init(void);
void gui_draw_titlebar(void);
void gui_update_mouse(void);
void gui_poll(void);

#endif
