#ifndef SYSDIRS_H
#define SYSDIRS_H

/* The top-level layout, owned by root: /system (the system's programs), /etc, /home, /root, /tmp.
 * With an installed disk these are folders of that disk made visible at the root (bind mounts);
 * without one they are plain directories in memory. */
void sysdirs_setup(int have_disk);

#endif
