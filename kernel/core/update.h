#ifndef UPDATE_H
#define UPDATE_H

#include <stdint.h>

/*
 * Software Update. update.artfical.com publishes every build of tOS (build = the commit count);
 * an installed system asks it what is newer than its own build, downloads the new kernel and
 * initrd into /system/update, checks their SHA-256, and marks them ready. While restarting, the
 * boot screen says "Updating" and the files move to /system/boot/<build>/; from then on the
 * kernel that boots (the CD's) hands over to the newest one on the disk. A build that fails to
 * reach the desktop twice is passed over, so an interrupted or broken update still leaves a
 * working system, and nothing outside /system is ever touched.
 */

enum { UPD_IDLE, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_READY, UPD_ERROR, UPD_LIVE };

#define UPD_MAX_NOTES 40
#define UPD_MAX_SHOTS 8

typedef struct {
    unsigned build;
    char icon[20], app[32], text[200], url[96];
} update_note_t;

typedef struct {
    unsigned build;
    char path[64], caption[80], app[32];
    uint8_t *rgba;                      /* decoded once downloaded, NULL until then */
    int w, h;
    int frames, frame_ms;               /* a video: `frames` pictures stacked in rgba, shown frame_ms each */
} update_shot_t;

typedef struct {
    int state;
    unsigned current, latest;
    int behind, far;
    char commit[16], date[16], title[96], err[96];
    uint32_t ksize, isize;
    char ksha[65], isha[65], kpath[64], ipath[64];
    int nnotes, nshots;
    update_note_t notes[UPD_MAX_NOTES];
    update_shot_t shots[UPD_MAX_SHOTS];
    uint32_t got, total, speed;         /* download progress, bytes and bytes per second */
} update_info_t;

const update_info_t *update_info(void);
void update_check_async(void);          /* asks update.artfical.com, on a task of its own */
void update_download_async(void);       /* fetches, verifies and stages the update */
int  update_ready(void);                /* an update waits for the restart */

/* boot: install a staged update (the boot screen shows it), then start the newest kernel on the disk */
int  update_apply_at_boot(void);
void update_boot_chain(void);
void update_session_ok(void);           /* the desktop is up: the kernel that booted works */

#endif
