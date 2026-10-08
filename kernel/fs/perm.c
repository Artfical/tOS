#include "perm.h"
#include "auth.h"
#include "string.h"
#include "terminal.h"

static uint32_t umask_bits = 022;

uint32_t perm_umask(void) { return umask_bits; }
void perm_set_umask(uint32_t mask) { umask_bits = mask & 0777; }

int perm_mode_allows(uint32_t mode, uint32_t fuid, uint32_t fgid, int want)
{
    uint32_t uid = auth_uid();
    if (uid == 0) return 1;
    uint32_t bits;
    if (uid == fuid) bits = (mode >> 6) & 7;
    else if (auth_gid() == fgid || auth_in_gid(uid, fgid)) bits = (mode >> 3) & 7;
    else bits = mode & 7;
    return (bits & (uint32_t)want) == (uint32_t)want;
}

static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == '/');
}

int perm_is_system_path(const char *abs_path)
{
    return under(abs_path, "/system") || under(abs_path, "/mnt/system");
}

/* Removing anything of the system needs root and, on top, the password: even a
 * root shell has to say "yes, really" once per command. */
int perm_guard_system(const char *abs_path, const char *action)
{
    if (!perm_is_system_path(abs_path)) return 0;
    if (auth_uid() != 0) return -1;
    char what[160];
    strcpy(what, action);
    strcat(what, " ");
    strncpy(what + strlen(what), abs_path, 80);
    what[sizeof(what) - 1] = 0;
    return auth_confirm_privileged(what);
}
