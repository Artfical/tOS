#ifndef WM_H
#define WM_H

/* Actions a window can be sent from the top File/Edit/... menu bar, claimed
 * one-shot via wm_get_menu_action() from the target window's own task. */
enum { WM_ACTION_NONE = 0, WM_ACTION_NEW, WM_ACTION_OPEN, WM_ACTION_SAVE };

void wm_init(void);
void wm_poll(void);
void wm_run(void);
int wm_current_task_has_focus(void);
int wm_get_content_click(int *x, int *y);
int wm_get_content_mouse(int *x, int *y, int *buttons);
int wm_get_menu_action(void);
void wm_open_notepad_file(const char *path);
void wm_open_viewer_file(const char *path);

#endif
