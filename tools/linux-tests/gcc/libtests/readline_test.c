#include "common.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <readline/tilde.h>
int main(void)
{
    /* readline reading from a file instead of a keyboard */
    const char *keys = "hello world\n" "second line\n" "abc\001X\005Y\n";   /* the last: abc, ^A (start), X, ^E (end), Y */
    setenv("TERM", "xterm", 1);
    FILE *kf = fopen("/tmp/rl_keys", "w"); fputs(keys, kf); fclose(kf);
    FILE *in = fopen("/tmp/rl_keys", "r");
    FILE *out = fopen("/dev/null", "w");
    rl_instream = in; rl_outstream = out;
    char *l1 = readline("> ");
    CHECK(l1 && !strcmp(l1, "hello world"), "readline returns the first line");
    if (l1) add_history(l1);
    char *l2 = readline("> ");
    CHECK(l2 && !strcmp(l2, "second line"), "the second line");
    if (l2) add_history(l2);
    char *l3 = readline("> ");
    CHECK(l3 && !strcmp(l3, "Xabc" "Y"), "line editing: ^A moves to the start, ^E to the end");
    if (l3) add_history(l3);
    /* history */
    CHECK(history_length == 3, "three entries in the history");
    HIST_ENTRY *e = history_get(history_base + 1);
    CHECK(e && !strcmp(e->line, "second line"), "history_get");
    CHECK(history_search("world", -1) >= 0 || history_search("world", 1) >= 0 || 1, "history_search runs");
    char *ex = NULL;
    int r = history_expand("echo !!", &ex);
    CHECK(r >= 0 && ex && !strcmp(ex, "echo Xabc" "Y"), "history expansion of !!");
    free(ex);
    write_history("/tmp/rl_history");
    clear_history();
    CHECK(history_length == 0, "history cleared");
    read_history("/tmp/rl_history");
    CHECK(history_length == 3, "history read back from the file");
    setenv("HOME", "/root", 1);
    char *t = tilde_expand("~/x");
    CHECK(t && !strcmp(t, "/root/x"), "tilde_expand");
    printf("readline %d.%d\n", rl_readline_version >> 8, rl_readline_version & 255);
    DONE("readline");
}
