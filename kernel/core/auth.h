#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>

/*
 * Users, groups and credentials.
 *
 * Accounts live in /etc/passwd, /etc/shadow and /etc/group (the usual formats).
 * Every task carries the uid/gid it runs as: the kernel and everything it
 * starts before a login run as root (uid 0), a login session runs as the user
 * who logged in, and `sudo` runs one command as another user. File access is
 * checked against these credentials by the VFS and ramfs.
 */

#define AUTH_NAME_MAX   32
#define AUTH_ROOT_UID   0
#define AUTH_SUDO_GROUP "sudo"
#define AUTH_SUDO_GID   27
#define AUTH_USERS_GID  100
#define AUTH_FIRST_UID  1000

typedef struct {
    char name[AUTH_NAME_MAX];
    uint32_t uid, gid;
    char gecos[48];
    char home[64];
    char shell[24];
} auth_user_t;

typedef struct { uint32_t uid, gid; } auth_saved_t;

void auth_init(void);                       /* loads the account files (a root account always exists) */
int  auth_have_accounts(void);              /* 1 when /etc/shadow holds a real login (an installed system) */

uint32_t auth_uid(void);                    /* credentials of the running task */
uint32_t auth_gid(void);
int  auth_is_root(void);
void auth_set_session(uint32_t uid, uint32_t gid);           /* becomes this user (login, su) */
void auth_session(uint32_t *uid, uint32_t *gid);             /* who is logged in on this console */
void auth_become(uint32_t uid, uint32_t gid, auth_saved_t *saved);   /* temporary switch (sudo, kernel work) */
void auth_restore(const auth_saved_t *saved);

int  auth_lookup(const char *name, auth_user_t *out);        /* 0 = found */
int  auth_lookup_uid(uint32_t uid, auth_user_t *out);
const char *auth_name_of(uint32_t uid);                      /* "1234" when unknown; static buffer */
const char *auth_group_name_of(uint32_t gid);
int  auth_group_lookup(const char *name, uint32_t *gid);
int  auth_user_in_group(uint32_t uid, const char *group);
int  auth_can_sudo(uint32_t uid);
int  auth_in_gid(uint32_t uid, uint32_t gid);              /* primary or supplementary group */
void auth_mark_confirmed(void);                            /* a successful sudo counts as the password confirmation */
void sprintf_u32(char *out, uint32_t v);

int  auth_check_password(const char *name, const char *password);   /* 0 = right */
int  auth_user_has_password(const char *name);
int  auth_add_user(const char *name, const char *password, const char *gecos, int admin, uint32_t *uid_out);
int  auth_del_user(const char *name);
int  auth_set_password(const char *name, const char *password);
int  auth_set_root_password(const char *password);
int  auth_save(void);                       /* writes the three files, shadow readable by root only */

/* terminal prompts */
int  auth_read_line(const char *prompt, char *out, int max, int echo);
int  auth_prompt_password(const char *prompt, char *out, int max);

/* One password confirmation per shell command: asking for it twice in one
 * `rm -r /system/x` would be silly, so the answer is remembered until the
 * next command line starts. */
void auth_command_begin(void);
int  auth_confirm_privileged(const char *what);     /* 0 = confirmed */

const char *auth_hostname(void);
void auth_set_hostname(const char *name);

#endif
