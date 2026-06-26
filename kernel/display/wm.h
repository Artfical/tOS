#ifndef WM_H
#define WM_H

void wm_init(void);
void wm_poll(void);
void wm_run(void);
int wm_current_task_has_focus(void);
int wm_get_content_click(int *x, int *y);

#endif
