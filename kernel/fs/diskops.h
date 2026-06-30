#ifndef DISKOPS_H
#define DISKOPS_H

/* Shared disk mount/unmount/format backend used by both the `disk` shell
   command and the graphical Disk Utility app, so the two front-ends can't
   drift out of sync on what filesystem types are supported. On failure,
   err is filled with a human-readable message (if err and err_len > 0). */

int diskops_mount(const char *name, const char *mount_point, const char *fstype, char *err, int err_len);
int diskops_umount(const char *mount_point, char *err, int err_len);
int diskops_format(const char *name, const char *fstype, char *err, int err_len);

/* Probes a block device against every supported on-disk filesystem format
   (read-only, no VFS mount performed) and returns the first one whose
   signature checks out, or NULL if none match. Lets mount UIs skip asking
   the user for a filesystem type up front. */
const char *diskops_detect(const char *name);

extern const char *diskops_fstypes[];
extern const int diskops_fstypes_count;

#endif
