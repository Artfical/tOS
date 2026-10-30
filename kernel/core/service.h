#ifndef SERVICE_H
#define SERVICE_H

#include <stdint.h>

/* Services: programs that run in the background (an SSH server...), started by hand or when tOS starts.
 *
 * A package declares its services in tpkg.services ("name|command line|description", %PKG% is the
 * package's folder); tpkg writes them to /system/services.tmbl ("name|command|description|package").
 * /system/services.on lists the ones that start at boot. A service's output goes to a log of its own
 * (service log <name>), not to any window. */

#define SERVICE_MAX 16

typedef struct {
    char name[24];
    char desc[80];
    char package[40];
    int enabled;                 /* starts when tOS starts */
    int running;
} service_info_t;

int  service_list(service_info_t *out, int max);       /* what is installed; the count */
int  service_start(const char *name);                  /* 0 = started, else a negative number (service_strerror) */
int  service_stop(const char *name);
int  service_enable(const char *name, int on);         /* start at boot, or not */
int  service_register(const char *line, const char *package_dir);   /* tpkg: one tpkg.services line */
void service_unregister_package(const char *package);  /* tpkg remove: stops, disables and forgets them */
const char *service_strerror(int err);
int  service_log(const char *name, char *out, int cap);   /* the last lines of its output */
void service_boot_task(void);                          /* a kernel task: starts the enabled services once the network is up */

#endif
