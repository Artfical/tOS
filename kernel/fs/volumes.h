#ifndef VOLUMES_H
#define VOLUMES_H

#include <stdint.h>

/* Removable disks: mounted at /Volumes/<label> when plugged in, ejected before unplugging. */

#define VOLUMES_MAX 4

enum { VOLUMES_NONE, VOLUMES_ADDED, VOLUMES_REMOVED, VOLUMES_UNSAFE };

typedef struct {
    char name[32];          /* the volume label */
    char path[64];          /* /Volumes/<name> */
    uint64_t bytes;
    int removable;          /* a USB disk, offered for ejecting in the top bar */
} volume_info_t;

/* looks for disks plugged in or pulled out; returns what happened (VOLUMES_UNSAFE: a mounted
 * disk was pulled out without being ejected), with the disk's name in volumes_last_name() */
int  volumes_poll(void);
const char *volumes_last_name(void);
int  volumes_list(volume_info_t *out, int max);
int  volumes_eject(const char *path);       /* 0 = safe to unplug; -1 = in use or unknown; -2 = Disk Utility is busy with it */

/* Disk Utility: any block device by its name (ata1, usb0) */
int  volumes_mount(const char *bdname);                  /* at /Volumes/<label> */
int  volumes_unmount(const char *bdname);
const char *volumes_name_of(const char *bdname);         /* the mounted volume's name, or 0 */
void volumes_label_of(const char *bdname, const char *fs, char *out);   /* label read from the disk, "" when none */
/* a long operation on a disk holds it; mounting, unmounting and ejecting it are refused (-2) meanwhile */
int  volumes_busy(const char *bdname);
int  volumes_claim(const char *bdname);                  /* -1 = already busy */
void volumes_release(const char *bdname);
int  volumes_erase(const char *bdname, const char *fstype, const char *label, char *err, int err_len);

#endif
