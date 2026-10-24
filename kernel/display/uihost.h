#ifndef UIHOST_H
#define UIHOST_H

#include <stdint.h>

/*
 * What a desktop gives its native apps (uiapp.h). tOS implements it in wm_gfx.c over its VFS,
 * scheduler and network stack; ArmtOS in desktop.c over the SD card's FAT partition. Anything a
 * platform cannot do returns -1 / 0 and the app says so.
 */

typedef struct { char name[64]; uint32_t size; int is_dir; } uihost_entry_t;
typedef struct { uint32_t pid; char name[32]; int state; uint32_t cpu; } uihost_task_t;
typedef struct {
    int up;
    uint32_t ip, gateway, dns, netmask;
    uint8_t mac[6];
    char driver[32];
    uint32_t rx_bytes, tx_bytes, rx_packets, tx_packets;
} uihost_net_t;

uint32_t uihost_ms(void);
void uihost_time(int *year, int *month, int *day, int *hour, int *min, int *sec);

/* files: paths are absolute ("/" is the root the platform has) */
const char *uihost_home(void);
int  uihost_list(const char *dir, uihost_entry_t *out, int max);    /* count, or -1 */
int  uihost_read(const char *path, uint8_t **data, uint32_t *len);  /* malloc'd; 0 = ok */
int  uihost_write(const char *path, const void *data, uint32_t len);
int  uihost_mkdir(const char *path);
int  uihost_delete(const char *path);
int  uihost_rename(const char *from, const char *to);
int  uihost_exists(const char *path);                               /* 0 no, 1 a file, 2 a folder */

/* Dragging files out of an app (from its drag callback, the button held): the desktop takes over, draws
 * them under the pointer and drops them where the button comes up -- the desktop, the Trash, an app in
 * the Dock, or a window whose app takes files (uiapp_class_t.drop). paths: one per line. */
void uihost_drag_files(const char *paths);
int  uihost_keys_held(void);                                        /* 1 Shift, 2 Ctrl, 4 Alt/Option */
/* files moved (a drop, a delete): a number that changes then, for apps that show folders to look again */
unsigned uihost_files_gen(void);
void uihost_files_touch(void);

/* other apps and the desktop itself */
void uihost_open(const char *app_id, const char *path);
int  uihost_set_wallpaper(const char *path);         /* PNG or JPEG; 0 = ok */

/* the system */
int  uihost_tasks(uihost_task_t *out, int max);
int  uihost_kill(uint32_t pid);
void uihost_memory(uint32_t *total_kb, uint32_t *used_kb);
void uihost_net(uihost_net_t *n);
int  uihost_net_start(void);
const char *uihost_system_name(void);
const char *uihost_hostname(void);

/* desktop settings: "dark", "dock_magnify", "mouse_speed", "wallpaper" (0-3), "width", "height";
 * set also takes "resolution" (an index into 1024x768, 1280x720, 1366x768, 1600x900, 1920x1080) */
int  uihost_get(const char *key);
void uihost_set(const char *key, int value);

/* accounts */
typedef struct { char name[32]; char full[48]; uint32_t uid; int admin; int current; } uihost_user_t;
int  uihost_users(uihost_user_t *out, int max);
int  uihost_add_user(const char *name, const char *password, const char *full, int admin);   /* needs authorisation */
int  uihost_set_password(const char *name, const char *password);                          /* needs authorisation */

/* Asks the person at the desktop for their password (the password dialog), without blocking:
 * poll uihost_authorized() -- 0 while asking, 1 granted, -1 refused or cancelled. A grant lasts
 * for one privileged call. */
void uihost_authorize(const char *reason);
int  uihost_authorized(void);


/* Notifications: a banner at the top right that stays in Notification Centre. app is the app's
 * id (its icon is used) or a name such as "System"; icon is an image name ("doc48_disk",
 * "icon48_paint") or NULL; act1/act2 are button labels or NULL (with buttons the banner waits
 * for an answer). Returns an id for uihost_notify_result: 0 waiting, 1 or 2 that button,
 * 3 the notification itself was clicked, -1 closed, -2 unknown. Do Not Disturb ("dnd" in
 * uihost_get/uihost_set) keeps banners away but the Centre still collects. */
int  uihost_notify(const char *app, const char *icon, const char *title, const char *body, const char *act1, const char *act2);
int  uihost_notify_result(int id);


/* Removable disks: what is mounted (under /Volumes) and ejecting one (0 = safe to unplug,
 * -1 = in use). Unplugging a disk without ejecting it is reported by the desktop itself. */
typedef struct { char name[32]; char path[64]; uint64_t bytes; } uihost_volume_t;
int  uihost_volumes(uihost_volume_t *out, int max);
int  uihost_eject(const char *path);


/* Disk Utility: every disk, with what is on it. size is the device's; total/used are the file
 * system's (used_known says whether they could be read). Mounting puts a volume at
 * /Volumes/<name>. Erasing needs authorisation (uihost_authorize first). */
typedef struct {
    char dev[16];            /* ata0, usb0 */
    char model[40];          /* what the disk calls itself */
    char name[32];           /* the volume's name, "" when nothing is mounted */
    char fs[16];             /* tfsk, fat32, ...; "" when none was recognised */
    char mount[64];
    char connection[16];     /* SATA, USB, PCI-Express */
    uint64_t size, total, used;
    int used_known, mounted, removable, startup;
} uihost_disk_t;
int  uihost_disks(uihost_disk_t *out, int max);
int  uihost_disk_mount(const char *dev);
int  uihost_disk_unmount(const char *dev);
int  uihost_disk_erase(const char *dev, const char *fs, const char *name, char *err, int len);
int  uihost_disk_first_aid(const char *dev, char *report, int len);   /* 0 OK, 1 problems, -1 unreadable */
/* The long ones in the background, so the desktop keeps running: op 1 = erase (authorise first),
 * 2 = First Aid. While a job runs its disk can't be mounted, unmounted or ejected.
 * start: 0 started, -1 busy (this disk, or another job); poll: -2 still running, otherwise the
 * result, with the report copied out. Mount/unmount return -2 for a disk a job is using. */
/* Web pages (tFire), fetched in the background: uihost_fetch starts one (NULL = could not), poll says
 * 0 while it runs, 1 when the page is there, -1 when it failed. uihost_fetch_result gives the body
 * (not 0-terminated), the HTTP status, the content type, the address it ended at after redirects, and
 * the reason when it failed; they stay valid until uihost_fetch_free. */
void *uihost_fetch(const char *url);
int   uihost_fetch_poll(void *h);
const uint8_t *uihost_fetch_result(void *h, uint32_t *len, int *status, const char **ctype, const char **final_url, const char **error);
void  uihost_fetch_free(void *h);

/* A PNG or (baseline) JPEG picture in memory -> RGBA (malloc'd), scaled down to fit max_w x max_h if it is
 * bigger. Pictures too big to decode safely are refused. 0 = ok. */
int   uihost_image_decode(const uint8_t *data, uint32_t len, int max_w, int max_h, uint8_t **rgba, int *w, int *h);

/* closes the window an app (a uiapp_t) is in, like its red button */
void uihost_close_window(void *app);
int  uihost_disk_job(int op, const char *dev, const char *fs, const char *name);
int  uihost_disk_job_poll(char *report, int len);


/* tOS Recovery and the first boot. session: 0 the desktop, 1 tOS Recovery, 2 the login window.
 * Keyboard layouts: 0 U.S., 1 Turkish Q. Installing runs in the background like the disk jobs:
 * start returns 0 when it began; poll gives -2 while it runs (with the percentage and what it
 * is doing), 0 when tOS is installed, -1 when it failed. */
int  uihost_session(void);
int  uihost_keyboard(void);
void uihost_keyboard_set(int layout);
int  uihost_install_start(const char *dev, const char *host, const char *user, const char *full,
                          const char *password, int keyboard, int picture);
int  uihost_install_poll(int *percent, char *status, int len);
void uihost_try_live(void);          /* leave Recovery for the live desktop */
void uihost_restart(void);
void uihost_shutdown(void);


/* Software Update (update.artfical.com). The status is the kernel's update_info_t: see
 * kernel/core/update.h for its fields. Not offered in the live session or in Recovery. */
const void *uihost_update_status(void);      /* an update_info_t */
void uihost_update_check(void);
void uihost_update_download(void);
void uihost_update_restart(void);            /* restart and install */
unsigned uihost_build(void);
const char *uihost_version(void);

#endif
