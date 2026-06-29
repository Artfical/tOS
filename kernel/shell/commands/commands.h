#ifndef COMMANDS_H
#define COMMANDS_H

void cmd_help(int argc, char **args);
void cmd_man(int argc, char **args);
void cmd_echo(int argc, char **args);
void cmd_clear(int argc, char **args);
void cmd_pwd(int argc, char **args);
void cmd_ls(int argc, char **args);
void cmd_cd(int argc, char **args);
void cmd_mkdir(int argc, char **args);
void cmd_rmdir(int argc, char **args);
void cmd_rm(int argc, char **args);
void cmd_touch(int argc, char **args);
void cmd_cat(int argc, char **args);
void cmd_mv(int argc, char **args);
void cmd_cp(int argc, char **args);
void cmd_edit(int argc, char **args);
void cmd_exec(int argc, char **args);
void cmd_reboot(int argc, char **args);
void cmd_shutdown(int argc, char **args);
void cmd_version(int argc, char **args);
void cmd_about(int argc, char **args);
void cmd_uname(int argc, char **args);
void cmd_ping(int argc, char **args);
void cmd_wget(int argc, char **args);
void cmd_tsharp(int argc, char **args);
void cmd_python(int argc, char **args);

void cmd_head(int argc, char **args);
void cmd_tail(int argc, char **args);
void cmd_wc(int argc, char **args);
void cmd_sort(int argc, char **args);
void cmd_grep(int argc, char **args);
void cmd_find(int argc, char **args);
void cmd_rev(int argc, char **args);
void cmd_uniq(int argc, char **args);
void cmd_date(int argc, char **args);
void cmd_whoami(int argc, char **args);
void cmd_hostname(int argc, char **args);
void cmd_cal(int argc, char **args);
void cmd_yes(int argc, char **args);
void cmd_seq(int argc, char **args);
void cmd_sleep(int argc, char **args);
void cmd_df(int argc, char **args);
void cmd_free(int argc, char **args);
void cmd_dmesg(int argc, char **args);
void cmd_basename(int argc, char **args);
void cmd_dirname(int argc, char **args);
void cmd_which(int argc, char **args);
void cmd_env(int argc, char **args);

void cmd_uptime(int argc, char **args);
void cmd_ps(int argc, char **args);
void cmd_kill(int argc, char **args);
void cmd_chmod(int argc, char **args);
void cmd_hexdump(int argc, char **args);
void cmd_tee(int argc, char **args);
void cmd_alias(int argc, char **args);
void cmd_history(int argc, char **args);
void cmd_unalias(int argc, char **args);
void cmd_font(int argc, char **args);
void cmd_htop(int argc, char **args);
void cmd_disk(int argc, char **args);

#endif
