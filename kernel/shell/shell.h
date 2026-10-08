#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_run(void);
void shell_run_windowed(const char *initial_cmd);
const char **shell_builtin_names(void);
int  shell_alias_set(const char *name, const char *value);
int  shell_alias_unset(const char *name);
void shell_alias_list(void);
void shell_history_show(void);
void shell_exec_capture(const char *cmd, char *out, int max);
void shell_dispatch(int argc, char **args);   /* run one already-split command line */
void shell_subshell(void);                    /* a nested interactive shell until `exit` (su, sudo -i) */
void shell_request_exit(void);                /* `exit` / `logout` */
void shell_login(void);                       /* the login prompt (live sessions go straight in as root) */

#endif
