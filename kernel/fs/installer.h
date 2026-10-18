#ifndef INSTALLER_H
#define INSTALLER_H

#include "ata.h"
#include "tfsk.h"

int installer_check_installed(void);
int installer_run(void);
int installer_setup_accounts(void);   /* computer name, root password, first user (needs /etc to be writable) */
/* the graphical installer: erase and fill `disk`, then make the account (progress may be NULL) */
int installer_install(ata_device_t *disk, void (*progress)(int pct, const char *what));
int installer_finish(ata_device_t *disk, const char *host, const char *user, const char *full, const char *password,
                     const char *keyboard, int picture, void (*progress)(int pct, const char *what));

#endif
