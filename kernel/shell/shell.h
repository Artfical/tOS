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

#endif
