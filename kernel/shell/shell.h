#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_run(void);
int  shell_alias_set(const char *name, const char *value);
int  shell_alias_unset(const char *name);
void shell_alias_list(void);
void shell_history_show(void);

#endif
