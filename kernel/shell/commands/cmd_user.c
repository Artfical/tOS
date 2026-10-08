#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "memory.h"
#include "auth.h"
#include "perm.h"
#include "vfs.h"
#include "ramfs.h"
#include "fsbridge.h"
#include "shell.h"
#include "scheduler.h"
#include "debugmon.h"

/* ---------- small helpers ---------- */

static void put_num(uint32_t n)
{
    char buf[12];
    sprintf_u32(buf, n);
    terminal_writestring(buf);
}

static void print_user_id(uint32_t uid)
{
    put_num(uid);
    terminal_putchar('(');
    terminal_writestring(auth_name_of(uid));
    terminal_putchar(')');
}

static void print_group_id(uint32_t gid)
{
    put_num(gid);
    terminal_putchar('(');
    terminal_writestring(auth_group_name_of(gid));
    terminal_putchar(')');
}

static int parse_uint(const char *s, uint32_t *out)
{
    if (!*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

/* user name or number -> uid */
static int resolve_user(const char *s, uint32_t *uid, uint32_t *gid)
{
    auth_user_t u;
    if (auth_lookup(s, &u) == 0) { *uid = u.uid; if (gid) *gid = u.gid; return 0; }
    if (parse_uint(s, uid) == 0) {
        if (gid) *gid = auth_lookup_uid(*uid, &u) == 0 ? u.gid : *uid;
        return 0;
    }
    return -1;
}

static int resolve_group(const char *s, uint32_t *gid)
{
    if (auth_group_lookup(s, gid) == 0) return 0;
    return parse_uint(s, gid);
}

void report_fs_failure(const char *cmd, const char *path)
{
    terminal_writestring(cmd);
    terminal_writestring(": ");
    terminal_writestring(path);
    terminal_writestring(vfs_denied() ? ": Permission denied\n" : ": Failed\n");
}

/* ---------- identity ---------- */

void cmd_whoami(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring(auth_name_of(auth_uid()));
    terminal_putchar('\n');
}

void cmd_id(int argc, char **args)
{
    uint32_t uid = auth_uid(), gid = auth_gid();
    if (argc > 1 && resolve_user(args[1], &uid, &gid) != 0) {
        terminal_writestring("id: ");
        terminal_writestring(args[1]);
        terminal_writestring(": no such user\n");
        return;
    }
    terminal_writestring("uid="); print_user_id(uid);
    terminal_writestring(" gid="); print_group_id(gid);
    terminal_writestring(" groups=");
    print_group_id(gid);
    static const char *extra[] = { AUTH_SUDO_GROUP, "users", 0 };
    for (int i = 0; extra[i]; i++) {
        uint32_t g;
        if (auth_group_lookup(extra[i], &g) == 0 && g != gid && auth_in_gid(uid, g)) {
            terminal_putchar(',');
            print_group_id(g);
        }
    }
    terminal_putchar('\n');
}

void cmd_groups(int argc, char **args)
{
    uint32_t uid = auth_uid(), gid = auth_gid();
    if (argc > 1 && resolve_user(args[1], &uid, &gid) != 0) {
        terminal_writestring("groups: ");
        terminal_writestring(args[1]);
        terminal_writestring(": no such user\n");
        return;
    }
    terminal_writestring(auth_group_name_of(gid));
    static const char *extra[] = { AUTH_SUDO_GROUP, "users", 0 };
    for (int i = 0; extra[i]; i++) {
        uint32_t g;
        if (auth_group_lookup(extra[i], &g) == 0 && g != gid && auth_in_gid(uid, g)) {
            terminal_putchar(' ');
            terminal_writestring(extra[i]);
        }
    }
    terminal_putchar('\n');
}

void cmd_hostname(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(auth_hostname());
        terminal_putchar('\n');
        return;
    }
    if (!auth_is_root()) {
        terminal_writestring("hostname: you must be root to change the host name\n");
        return;
    }
    auth_set_hostname(args[1]);
}

/* ---------- password prompts ---------- */

static int ask_current_password(const char *user, const char *prompt_prefix)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        char prompt[96], pw[64];
        strcpy(prompt, prompt_prefix);
        strcat(prompt, user);
        strcat(prompt, ": ");
        if (auth_prompt_password(prompt, pw, sizeof(pw)) < 0) return -1;
        int ok = auth_check_password(user, pw) == 0;
        memset(pw, 0, sizeof(pw));
        if (ok) return 0;
        terminal_writestring("Sorry, try again.\n");
    }
    return -1;
}

void cmd_passwd(int argc, char **args)
{
    const char *user = argc > 1 ? args[1] : auth_name_of(auth_uid());
    auth_user_t u;
    if (auth_lookup(user, &u) != 0) {
        terminal_writestring("passwd: user '");
        terminal_writestring(user);
        terminal_writestring("' does not exist\n");
        return;
    }
    if (!auth_is_root() && u.uid != auth_uid()) {
        terminal_writestring("passwd: you may not change the password of ");
        terminal_writestring(user);
        terminal_putchar('\n');
        return;
    }
    if (!auth_is_root() && auth_user_has_password(user) && ask_current_password(user, "(current) password for ") != 0) {
        terminal_writestring("passwd: Authentication failure\n");
        return;
    }
    terminal_writestring("Changing password for ");
    terminal_writestring(user);
    terminal_writestring(".\n");
    char a[64], b[64];
    if (auth_prompt_password("New password: ", a, sizeof(a)) < 0) return;
    if (a[0] == 0) { terminal_writestring("passwd: password cannot be empty\n"); return; }
    if (auth_prompt_password("Retype new password: ", b, sizeof(b)) < 0) return;
    if (strcmp(a, b) != 0) {
        terminal_writestring("Sorry, passwords do not match.\npasswd: password unchanged\n");
    } else if (auth_set_password(user, a) != 0) {
        terminal_writestring("passwd: could not update the account files\n");
    } else {
        terminal_writestring("passwd: password updated successfully\n");
    }
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
}

/* ---------- accounts ---------- */

void cmd_useradd(int argc, char **args)
{
    if (!auth_is_root()) { terminal_writestring("useradd: permission denied (try sudo)\n"); return; }
    const char *name = 0;
    int admin = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(args[i], "-G") == 0 && i + 1 < argc) {
            if (strcmp(args[i + 1], AUTH_SUDO_GROUP) == 0 || strcmp(args[i + 1], "wheel") == 0) admin = 1;
            i++;
        } else if (strcmp(args[i], "-m") == 0) {
            /* the home directory is always created */
        } else if (args[i][0] != '-') {
            name = args[i];
        }
    }
    if (!name) {
        terminal_writestring("usage: useradd [-G sudo] <name>\n");
        return;
    }
    if (auth_lookup(name, 0) == 0) { terminal_writestring("useradd: user already exists\n"); return; }
    char a[64], b[64];
    if (auth_prompt_password("New password: ", a, sizeof(a)) < 0) return;
    if (auth_prompt_password("Retype new password: ", b, sizeof(b)) < 0) return;
    if (a[0] == 0 || strcmp(a, b) != 0) {
        terminal_writestring("useradd: passwords empty or not matching, no account created\n");
        return;
    }
    uint32_t uid;
    if (auth_add_user(name, a, name, admin, &uid) != 0) {
        terminal_writestring("useradd: could not create the account (bad name or no room)\n");
    } else {
        terminal_writestring("useradd: created user ");
        terminal_writestring(name);
        terminal_writestring(" uid=");
        put_num(uid);
        terminal_putchar('\n');
    }
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
}

void cmd_userdel(int argc, char **args)
{
    if (!auth_is_root()) { terminal_writestring("userdel: permission denied (try sudo)\n"); return; }
    if (argc < 2) { terminal_writestring("usage: userdel <name>\n"); return; }
    if (auth_del_user(args[1]) != 0) terminal_writestring("userdel: no such user (or it cannot be removed)\n");
}

/* ---------- file ownership and permissions ---------- */

/* "755" or "u+rwx,go-w" applied to the current mode */
static int parse_mode(const char *spec, uint32_t cur, uint32_t *out)
{
    int octal = *spec != 0;
    for (const char *p = spec; *p; p++) if (*p < '0' || *p > '7') octal = 0;
    if (octal) {
        uint32_t v = 0;
        for (const char *p = spec; *p; p++) v = (v << 3) | (uint32_t)(*p - '0');
        if (v > 07777) return -1;
        *out = v;
        return 0;
    }
    uint32_t mode = cur & 07777;
    const char *p = spec;
    while (*p) {
        uint32_t who = 0;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            if (*p == 'u') who |= 0700; else if (*p == 'g') who |= 070; else if (*p == 'o') who |= 07; else who |= 0777;
            p++;
        }
        if (!who) who = 0777 & ~perm_umask();
        if (*p != '+' && *p != '-' && *p != '=') return -1;
        char op = *p++;
        uint32_t bits = 0;
        while (*p && *p != ',') {
            if (*p == 'r') bits |= 0444;
            else if (*p == 'w') bits |= 0222;
            else if (*p == 'x') bits |= 0111;
            else if (*p == 'X') bits |= (cur & 040000) ? 0111 : 0;
            else if (*p == 's') bits |= 06000;
            else if (*p == 't') bits |= 01000;
            else return -1;
            p++;
        }
        uint32_t m = bits & (who | 07000);
        if (op == '+') mode |= m;
        else if (op == '-') mode &= ~m;
        else mode = (mode & ~(who | 07000)) | m;
        if (*p == ',') p++;
    }
    *out = mode;
    return 0;
}

void cmd_chmod(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: chmod <mode> <file>...\n");
        terminal_writestring("  mode: octal (644, 755, 1777) or symbolic (u+x, go-w, a=r)\n");
        return;
    }
    for (int i = 2; i < argc; i++) {
        vfs_entry_t e;
        if (vfs_stat(args[i], &e) != 0) {
            terminal_writestring("chmod: ");
            terminal_writestring(args[i]);
            terminal_writestring(": No such file\n");
            continue;
        }
        uint32_t mode;
        if (parse_mode(args[1], e.mode | (e.is_dir ? 040000 : 0), &mode) != 0) {
            terminal_writestring("chmod: invalid mode '");
            terminal_writestring(args[1]);
            terminal_writestring("'\n");
            return;
        }
        if (vfs_chmod(args[i], mode) != 0) {
            terminal_writestring("chmod: changing permissions of '");
            terminal_writestring(args[i]);
            terminal_writestring(vfs_chmod_supported(args[i]) ? "': Operation not permitted\n" : "': not supported on this file system\n");
        }
    }
}

static int apply_owner(const char *cmd, const char *spec, int only_group, int argc, char **args, int first)
{
    char buf[80];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    uint32_t uid = VFS_KEEP, gid = VFS_KEEP;
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0;
    const char *user = only_group ? "" : buf;
    const char *group = only_group ? buf : (colon ? colon + 1 : "");
    if (user[0] && resolve_user(user, &uid, 0) != 0) {
        terminal_writestring(cmd); terminal_writestring(": invalid user: '"); terminal_writestring(user); terminal_writestring("'\n");
        return -1;
    }
    if (group[0] && resolve_group(group, &gid) != 0) {
        terminal_writestring(cmd); terminal_writestring(": invalid group: '"); terminal_writestring(group); terminal_writestring("'\n");
        return -1;
    }
    for (int i = first; i < argc; i++) {
        if (vfs_chown(args[i], uid, gid) != 0) {
            terminal_writestring(cmd);
            terminal_writestring(": changing ownership of '");
            terminal_writestring(args[i]);
            terminal_writestring(vfs_chmod_supported(args[i]) ? "': Operation not permitted\n" : "': not supported on this file system\n");
        }
    }
    return 0;
}

void cmd_chown(int argc, char **args)
{
    if (argc < 3) { terminal_writestring("usage: chown <user>[:<group>] <file>...\n"); return; }
    apply_owner("chown", args[1], 0, argc, args, 2);
}

void cmd_chgrp(int argc, char **args)
{
    if (argc < 3) { terminal_writestring("usage: chgrp <group> <file>...\n"); return; }
    apply_owner("chgrp", args[1], 1, argc, args, 2);
}

void cmd_umask(int argc, char **args)
{
    if (argc > 1) {
        uint32_t v = 0;
        for (const char *p = args[1]; *p; p++) {
            if (*p < '0' || *p > '7') { terminal_writestring("umask: invalid mode\n"); return; }
            v = (v << 3) | (uint32_t)(*p - '0');
        }
        perm_set_umask(v);
        return;
    }
    uint32_t m = perm_umask();
    char s[5] = { '0', (char)('0' + ((m >> 6) & 7)), (char)('0' + ((m >> 3) & 7)), (char)('0' + (m & 7)), 0 };
    terminal_writestring(s);
    terminal_putchar('\n');
}

/* drwxr-xr-x */
void format_mode(uint32_t mode, int is_dir, char *out)
{
    uint32_t type = mode & 0170000;
    if (type == 0) { type = is_dir ? 0040000 : 0100000; if ((mode & 0777) == 0) mode |= is_dir ? 0755 : 0644; }
    out[0] = type == 0040000 ? 'd' : type == 0120000 ? 'l' : '-';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    if (mode & 04000) out[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000) out[6] = (mode & 010) ? 's' : 'S';
    if (mode & 01000) out[9] = (mode & 01) ? 't' : 'T';
    out[10] = 0;
}

void ls_long_line(const vfs_entry_t *e)
{
    char m[12];
    format_mode(e->mode, e->is_dir, m);
    terminal_writestring(m);
    terminal_putchar(' ');
    const char *u = auth_name_of(e->uid);
    const char *g = auth_group_name_of(e->gid);
    terminal_writestring(u);
    for (int i = (int)strlen(u); i < 8; i++) terminal_putchar(' ');
    terminal_putchar(' ');
    terminal_writestring(g);
    for (int i = (int)strlen(g); i < 8; i++) terminal_putchar(' ');
    char num[12];
    sprintf_u32(num, e->size);
    for (int i = (int)strlen(num); i < 8; i++) terminal_putchar(' ');
    terminal_writestring(num);
    terminal_putchar(' ');
    terminal_writestring(e->name);
    if (e->is_dir) terminal_putchar('/');
    terminal_putchar('\n');
}

void cmd_stat(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: stat <file>\n"); return; }
    vfs_entry_t e;
    if (vfs_stat(args[1], &e) != 0) {
        terminal_writestring("stat: ");
        terminal_writestring(args[1]);
        terminal_writestring(": No such file\n");
        return;
    }
    char m[12];
    format_mode(e.mode, e.is_dir, m);
    terminal_writestring("  File: "); terminal_writestring(args[1]); terminal_putchar('\n');
    terminal_writestring("  Size: "); put_num(e.size); terminal_writestring(e.is_dir ? "   directory\n" : "   regular file\n");
    terminal_writestring("Access: ("); put_num((e.mode & 07777) / 512 % 8); put_num((e.mode & 0777) / 64 % 8);
    put_num((e.mode & 0777) / 8 % 8); put_num(e.mode & 7);
    terminal_writestring("/"); terminal_writestring(m); terminal_writestring(")  Uid: "); print_user_id(e.uid);
    terminal_writestring("   Gid: "); print_group_id(e.gid); terminal_putchar('\n');
}

/* ---------- su, sudo, login, logout ---------- */

static uint32_t sudo_cache_until_ms;

void cmd_exit(int argc, char **args)
{
    (void)argc; (void)args;
    shell_request_exit();
}

void cmd_su(int argc, char **args)
{
    const char *who = "root";
    int login_shell = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(args[i], "-") == 0 || strcmp(args[i], "-l") == 0) login_shell = 1;
        else who = args[i];
    }
    auth_user_t target;
    if (auth_lookup(who, &target) != 0) {
        terminal_writestring("su: user ");
        terminal_writestring(who);
        terminal_writestring(" does not exist\n");
        return;
    }
    if (!auth_is_root() && auth_user_has_password(who) && ask_current_password(who, "Password for ") != 0) {
        terminal_writestring("su: Authentication failure\n");
        return;
    }
    auth_saved_t saved;
    auth_become(target.uid, target.gid, &saved);
    char old_cwd[VFS_NAME_LEN];
    strcpy(old_cwd, ramfs_getcwd());
    if (login_shell) ramfs_chdir(target.home);
    shell_subshell();
    ramfs_chdir(old_cwd);
    auth_restore(&saved);
}

static int sudo_authenticate(void)
{
    uint32_t caller = auth_uid();
    if (caller == 0) { auth_mark_confirmed(); return 0; }
    if (!auth_can_sudo(caller)) {
        terminal_writestring(auth_name_of(caller));
        terminal_writestring(" is not in the sudoers file.  This incident will be reported.\n");
        return -1;
    }
    uint32_t now = debugmon_uptime_ms();
    if (sudo_cache_until_ms && now < sudo_cache_until_ms) { auth_mark_confirmed(); return 0; }
    if (ask_current_password(auth_name_of(caller), "[sudo] password for ") != 0) {
        terminal_writestring("sudo: 3 incorrect password attempts\n");
        return -1;
    }
    sudo_cache_until_ms = now + 5 * 60 * 1000;
    auth_mark_confirmed();
    return 0;
}

void cmd_sudo(int argc, char **args)
{
    const char *as = "root";
    int i = 1, interactive = 0;
    for (; i < argc && args[i][0] == '-'; i++) {
        if (strcmp(args[i], "-u") == 0 && i + 1 < argc) as = args[++i];
        else if (strcmp(args[i], "-i") == 0 || strcmp(args[i], "-s") == 0) interactive = 1;
        else if (strcmp(args[i], "-k") == 0 || strcmp(args[i], "-K") == 0) { sudo_cache_until_ms = 0; return; }
        else if (strcmp(args[i], "-v") == 0) { sudo_authenticate(); return; }
        else { terminal_writestring("sudo: unknown option\n"); return; }
    }
    if (i >= argc && !interactive) {
        terminal_writestring("usage: sudo [-u user] [-i] <command> [args...]\n");
        return;
    }
    auth_user_t target;
    if (auth_lookup(as, &target) != 0) {
        terminal_writestring("sudo: unknown user: ");
        terminal_writestring(as);
        terminal_putchar('\n');
        return;
    }
    if (sudo_authenticate() != 0) return;
    auth_saved_t saved;
    auth_become(target.uid, target.gid, &saved);
    if (i >= argc) {
        char old_cwd[VFS_NAME_LEN];
        strcpy(old_cwd, ramfs_getcwd());
        ramfs_chdir(target.home);
        shell_subshell();
        ramfs_chdir(old_cwd);
    } else {
        shell_dispatch(argc - i, args + i);
    }
    auth_restore(&saved);
}
