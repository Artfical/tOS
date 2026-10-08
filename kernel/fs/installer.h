#ifndef INSTALLER_H
#define INSTALLER_H

#include "ata.h"
#include "tfsk.h"

int installer_check_installed(void);
int installer_run(void);
int installer_setup_accounts(void);   /* computer name, root password, first user (needs /etc to be writable) */

#endif
