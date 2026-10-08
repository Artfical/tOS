#ifndef PERM_H
#define PERM_H

#include <stdint.h>

/* file permission model shared by the VFS and the shell */

#define PERM_STICKY 01000

/* may the current user do `want` (4 read, 2 write, 1 execute/search) to something with this owner and mode? root may do everything */
int  perm_mode_allows(uint32_t mode, uint32_t fuid, uint32_t fgid, int want);

uint32_t perm_umask(void);
void perm_set_umask(uint32_t mask);

/* paths that belong to the operating system itself: deleting or moving away anything below needs the account password */
int  perm_is_system_path(const char *abs_path);
int  perm_guard_system(const char *abs_path, const char *action);      /* 0 = allowed */

#endif
