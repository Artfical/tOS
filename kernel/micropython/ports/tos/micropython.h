#ifndef MICROPYTHON_H
#define MICROPYTHON_H

int micropython_init(void);
int micropython_run_repl(void);
int micropython_run_file(const char *path);
int micropython_run_file_argv(const char *path, int argc, char **argv);

#endif
