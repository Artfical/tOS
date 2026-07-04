#ifndef WM_H
#define WM_H

#include <stdint.h>

/* Actions a window can be sent from the top File/Edit/... menu bar, claimed
 * one-shot via wm_get_menu_action() from the target window's own task. */
enum { WM_ACTION_NONE = 0, WM_ACTION_NEW, WM_ACTION_OPEN, WM_ACTION_SAVE };

void wm_init(void);
void wm_poll(void);
void wm_run(void);
int wm_current_task_has_focus(void);
int wm_get_content_click(int *x, int *y);
int wm_get_content_mouse(int *x, int *y, int *buttons);
int wm_get_content_wheel(void);
int wm_get_menu_action(void);
void wm_open_notepad_file(const char *path);
void wm_open_viewer_file(const char *path);
void wm_open_pdfviewer_file(const char *path);
void wm_set_wallpaper_from_cells(const uint8_t *cells, int rows, int cols);
int wm_set_wallpaper_file(const char *path);
int wm_kill_task_window(uint32_t pid);
int wm_open_app(const char *name);
void *wm_script_open(const char *title, void **out_prev_userdata);
void wm_script_close(void *handle, void *prev_userdata);

#endif
