#include "tsharp.h"
#include "terminal.h"
#include "keyboard.h"
#include "ramfs.h"
#include "string.h"
#include "memory.h"
#include "stdlib.h"
#include "tos_api.h"

#define TS_MAX_VARS 256
#define TS_VAR_NAME 64
#define TS_VAR_VAL 256
#define TS_MAX_LINES 4096
#define TS_LINE_LEN 512
#define TS_MAX_ARGS 32
#define TS_CALL_DEPTH 256
#define TS_LOOP_MAX 10000000

typedef struct { char name[TS_VAR_NAME]; char val[TS_VAR_VAL]; } ts_var_t;

static ts_var_t ts_vars[TS_MAX_VARS];
static int ts_var_count;
static int ts_return_flag;
static char ts_return_val[TS_VAR_VAL];
static int ts_break_flag;
static int ts_continue_flag;

static char *ts_strdup(const char *s)
{
    int len = strlen(s);
    char *d = malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

static void ts_normalize(char *dst, const char *src)
{
    while (*src) {
        unsigned char c = (unsigned char)*src;
        if (c >= 0x80) {
            unsigned char c2 = (unsigned char)*(src+1);
                 if (c == 0xC4 && c2 == 0xB1) { *dst++ = 'i'; src += 2; }
            else if (c == 0xC4 && c2 == 0xB0) { *dst++ = 'I'; src += 2; }
            else if (c == 0xC3 && c2 == 0xBC) { *dst++ = 'u'; src += 2; }
            else if (c == 0xC3 && c2 == 0x9C) { *dst++ = 'U'; src += 2; }
            else if (c == 0xC3 && c2 == 0xB6) { *dst++ = 'o'; src += 2; }
            else if (c == 0xC3 && c2 == 0x96) { *dst++ = 'O'; src += 2; }
            else if (c == 0xC3 && c2 == 0xA7) { *dst++ = 'c'; src += 2; }
            else if (c == 0xC3 && c2 == 0x87) { *dst++ = 'C'; src += 2; }
            else if (c == 0xC5 && c2 == 0x9F) { *dst++ = 's'; src += 2; }
            else if (c == 0xC5 && c2 == 0x9E) { *dst++ = 'S'; src += 2; }
            else if (c == 0xC4 && c2 == 0x9F) { *dst++ = 'g'; src += 2; }
            else if (c == 0xC4 && c2 == 0x9E) { *dst++ = 'G'; src += 2; }
            else { *dst++ = *src++; }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void ts_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

static void ts_var_set(const char *name, const char *val)
{
    for (int i = 0; i < ts_var_count; i++) {
        if (strcmp(ts_vars[i].name, name) == 0) {
            strncpy(ts_vars[i].val, val, TS_VAR_VAL - 1);
            return;
        }
    }
    if (ts_var_count < TS_MAX_VARS) {
        strncpy(ts_vars[ts_var_count].name, name, TS_VAR_NAME - 1);
        strncpy(ts_vars[ts_var_count].val, val, TS_VAR_VAL - 1);
        ts_var_count++;
    }
}

static const char *ts_var_get(const char *name)
{
    for (int i = 0; i < ts_var_count; i++) {
        if (strcmp(ts_vars[i].name, name) == 0)
            return ts_vars[i].val;
    }
    return NULL;
}

static char *ts_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    int nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

static int ts_is_number(const char *s)
{
    if (!s || !*s) return 0;
    char *p = (char *)s;
    if (*p == '-') p++;
    if (!*p) return 0;
    int has_dot = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') { p++; continue; }
        if (*p == '.' && !has_dot) { has_dot = 1; p++; continue; }
        return 0;
    }
    return 1;
}

static double ts_atof(const char *s)
{
    if (!s || !*s) return 0;
    double res = 0, frac = 0, div = 1;
    int sign = 1, i = 0;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') i++;
    while (s[i] >= '0' && s[i] <= '9')
        res = res * 10 + (s[i++] - '0');
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10 + (s[i++] - '0');
            div *= 10;
        }
    }
    return sign * (res + frac / div);
}

static void ts_itoa(int val, char *buf)
{
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[32]; int i = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void ts_ftoa(double val, char *buf)
{
    int int_part = (int)val;
    double frac = val - int_part;
    if (frac < 0) frac = -frac;
    ts_itoa(int_part, buf);
    char *p = buf + strlen(buf);
    if (frac > 0.000001) {
        *p++ = '.';
        for (int i = 0; i < 6; i++) {
            frac *= 10;
            int d = (int)frac;
            *p++ = '0' + d;
            frac -= d;
            if (frac < 0.000001) break;
        }
        *p = '\0';
    }
}

static int ts_tokenize_expr(const char *expr, char **tokens, int max)
{
    int n = 0;
    int i = 0;
    int len = strlen(expr);
    while (i < len && n < max) {
        while (i < len && expr[i] == ' ') i++;
        if (i >= len) break;
        if (expr[i] == '"' || expr[i] == '\'') {
            char q = expr[i];
            int start = i;
            i++;
            while (i < len && expr[i] != q) i++;
            if (i < len) i++;
            int tlen = i - start;
            tokens[n] = malloc(tlen + 1);
            memcpy(tokens[n], expr + start, tlen);
            tokens[n][tlen] = '\0';
            n++;
        } else if (strchr("+-*/%()=<>!&|^,", expr[i])) {
            int start = i;
            if ((expr[i] == '=' || expr[i] == '!' || expr[i] == '<' || expr[i] == '>') &&
                i + 1 < len && expr[i+1] == '=') i++;
            else if ((expr[i] == '&' && i + 1 < len && expr[i+1] == '&') ||
                     (expr[i] == '|' && i + 1 < len && expr[i+1] == '|')) i++;
            else if (expr[i] == '*' && i + 1 < len && expr[i+1] == '*') i++;
            i++;
            int tlen = i - start;
            tokens[n] = malloc(tlen + 1);
            memcpy(tokens[n], expr + start, tlen);
            tokens[n][tlen] = '\0';
            n++;
        } else {
            int start = i;
            while (i < len && expr[i] != ' ' && !strchr("+-*/%()=<>!&|^,", expr[i])) i++;
            int tlen = i - start;
            tokens[n] = malloc(tlen + 1);
            memcpy(tokens[n], expr + start, tlen);
            tokens[n][tlen] = '\0';
            n++;
        }
    }
    return n;
}

static void ts_free_tokens(char **tokens, int n)
{
    for (int i = 0; i < n; i++) free(tokens[i]);
}

static int ts_prec(const char *op)
{
    if (!op) return 0;
    if (strcmp(op, "||") == 0 || strcmp(op, "or") == 0) return 1;
    if (strcmp(op, "&&") == 0 || strcmp(op, "and") == 0) return 2;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) return 3;
    if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) return 4;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) return 5;
    if (strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) return 6;
    if (strcmp(op, "**") == 0) return 7;
    if (strcmp(op, "not") == 0 || strcmp(op, "!") == 0) return 8;
    return 0;
}

static int ts_is_op(const char *s)
{
    return strcmp(s, "+") == 0 || strcmp(s, "-") == 0 || strcmp(s, "*") == 0 ||
           strcmp(s, "/") == 0 || strcmp(s, "%") == 0 || strcmp(s, "**") == 0 ||
           strcmp(s, "==") == 0 || strcmp(s, "!=") == 0 ||
           strcmp(s, "<") == 0 || strcmp(s, ">") == 0 || strcmp(s, "<=") == 0 || strcmp(s, ">=") == 0 ||
           strcmp(s, "&&") == 0 || strcmp(s, "||") == 0 ||
           strcmp(s, "and") == 0 || strcmp(s, "or") == 0 || strcmp(s, "not") == 0 || strcmp(s, "!") == 0;
}

static double ts_to_num(const char *s)
{
    if (!s || !*s) return 0;
    if ((s[0] == '"' || s[0] == '\'') && strlen(s) >= 2) return 0;
    if (ts_is_number(s)) return ts_atof(s);
    return 0;
}

static char **ts_shunting_yard(char **tokens, int n, int *rpn_count)
{
    char **out = malloc(sizeof(char *) * n);
    char **stack = malloc(sizeof(char *) * n);
    int out_i = 0, st_i = 0;

    for (int i = 0; i < n; i++) {
        char *t = tokens[i];
        if (t[0] == '"' || t[0] == '\'' || ts_is_number(t) || 
            (t[0] != '"' && t[0] != '\'' && !ts_is_op(t) && strcmp(t, "(") != 0 && strcmp(t, ")") != 0 && strcmp(t, ",") != 0)) {
            out[out_i++] = ts_strdup(t);
        } else if (strcmp(t, "(") == 0) {
            stack[st_i++] = ts_strdup(t);
        } else if (strcmp(t, ")") == 0) {
            while (st_i > 0 && strcmp(stack[st_i - 1], "(") != 0)
                out[out_i++] = stack[--st_i];
            if (st_i > 0) free(stack[--st_i]);
        } else if (ts_is_op(t)) {
            while (st_i > 0 && ts_is_op(stack[st_i - 1]) && 
                   ts_prec(stack[st_i - 1]) >= ts_prec(t) &&
                   strcmp(stack[st_i - 1], "(") != 0)
                out[out_i++] = stack[--st_i];
            stack[st_i++] = ts_strdup(t);
        }
    }
    while (st_i > 0)
        out[out_i++] = stack[--st_i];

    *rpn_count = out_i;
    
    char **result = malloc(sizeof(char *) * out_i);
    for (int i = 0; i < out_i; i++) {
        result[i] = ts_strdup(out[i]);
        free(out[i]);
    }
    free(out);
    free(stack);
    return result;
}

static char *ts_eval_rpn(char **rpn, int n)
{
    char **val_stack = malloc(sizeof(char *) * n);
    int vs_i = 0;

    for (int i = 0; i < n; i++) {
        char *t = rpn[i];
        if (t[0] == '"' || t[0] == '\'' || ts_is_number(t) ||
            (t[0] != '"' && t[0] != '\'' && !ts_is_op(t))) {
            char *v = ts_strdup(t);
            if (v[0] != '"' && v[0] != '\'') {
                const char *gv = ts_var_get(v);
                if (gv) {
                    free(v);
                    v = ts_strdup(gv);
                }
            }
            val_stack[vs_i++] = v;
        } else if (ts_is_op(t)) {
            if (strcmp(t, "not") == 0 || strcmp(t, "!") == 0) {
                char *a = val_stack[--vs_i];
                double da = ts_to_num(a);
                char buf[64]; ts_ftoa((da == 0) ? 1 : 0, buf);
                free(a);
                val_stack[vs_i++] = ts_strdup(buf);
            } else {
                char *b = val_stack[--vs_i];
                char *a = val_stack[--vs_i];
                double da = ts_to_num(a), db = ts_to_num(b);
                double r = 0;
                int cmp;
                if (strcmp(t, "+") == 0) { r = da + db; }
                else if (strcmp(t, "-") == 0) { r = da - db; }
                else if (strcmp(t, "*") == 0) { r = da * db; }
                else if (strcmp(t, "/") == 0) { 
                    if (db == 0) { terminal_writestring("tsharp: divide by zero\n"); r = 0; }
                    else r = da / db;
                }
                else if (strcmp(t, "%") == 0) { 
                    if (db == 0) { terminal_writestring("tsharp: mod by zero\n"); r = 0; }
                    else r = (int)da % (int)db;
                }
                else if (strcmp(t, "**") == 0) { 
                    r = 1;
                    for (int j = 0; j < (int)db; j++) r *= da;
                }
                else if (strcmp(t, "==") == 0) { 
                    if (ts_is_number(a) && ts_is_number(b)) cmp = da == db;
                    else cmp = strcmp(a, b) == 0;
                    r = cmp ? 1 : 0;
                }
                else if (strcmp(t, "!=") == 0) {
                    if (ts_is_number(a) && ts_is_number(b)) cmp = da != db;
                    else cmp = strcmp(a, b) != 0;
                    r = cmp ? 1 : 0;
                }
                else if (strcmp(t, "<") == 0) { r = da < db ? 1 : 0; }
                else if (strcmp(t, ">") == 0) { r = da > db ? 1 : 0; }
                else if (strcmp(t, "<=") == 0) { r = da <= db ? 1 : 0; }
                else if (strcmp(t, ">=") == 0) { r = da >= db ? 1 : 0; }
                else if (strcmp(t, "&&") == 0 || strcmp(t, "and") == 0) { r = (da != 0 && db != 0) ? 1 : 0; }
                else if (strcmp(t, "||") == 0 || strcmp(t, "or") == 0) { r = (da != 0 || db != 0) ? 1 : 0; }
                char buf[64]; ts_ftoa(r, buf);
                free(a); free(b);
                val_stack[vs_i++] = ts_strdup(buf);
            }
        }
    }

    char *res = ts_strdup(vs_i > 0 ? val_stack[0] : "");
    for (int i = 0; i < vs_i; i++) free(val_stack[i]);
    free(val_stack);
    return res;
}

static char *ts_eval_expr(const char *expr)
{
    char buf[TS_LINE_LEN];
    ts_normalize(buf, expr);
    ts_trim(buf);
    
    if (!buf[0]) return ts_strdup("");
    if (buf[0] == '"' || buf[0] == '\'') {
        int len = strlen(buf);
        if (len >= 2 && buf[len-1] == buf[0]) {
            buf[len-1] = '\0';
            return ts_strdup(buf + 1);
        }
        return ts_strdup(buf);
    }

    const char *gv = ts_var_get(buf);
    if (gv) return ts_strdup(gv);

    char *tok[64];
    int n = ts_tokenize_expr(buf, tok, 64);
    if (n == 0) return ts_strdup(buf);

    int rpn_n;
    char **rpn = ts_shunting_yard(tok, n, &rpn_n);
    ts_free_tokens(tok, n);

    char *result = ts_eval_rpn(rpn, rpn_n);
    for (int i = 0; i < rpn_n; i++) free(rpn[i]);
    free(rpn);
    return result;
}

static int ts_eval_condition(const char *expr)
{
    char *r = ts_eval_expr(expr);
    int res = 0;
    if (ts_is_number(r)) {
        double v = ts_to_num(r);
        res = (v < 0 ? -v : v) > 0.0001;
    } else res = (strcmp(r, "dogru") == 0 || strcmp(r, "True") == 0 || strcmp(r, "true") == 0);
    free(r);
    return res;
}

static void ts_exec_lines(char **lines, int start, int end, int *next_line);
static void ts_call_func(const char *name, int argc, char **args, char *result);

typedef struct {
    char name[TS_VAR_NAME];
    char params[TS_MAX_ARGS][TS_VAR_NAME];
    int param_count;
    char **body;
    int body_count;
    int defined;
} ts_func_t;

static ts_func_t ts_functions[TS_MAX_VARS];
static int ts_func_count;

/* tOS scripting API builtins (see tos_api.h): one function per OS
 * capability, available from T# the same way a user-defined function
 * would be, but checked first so a script can't accidentally shadow
 * one by defining a same-named function (the user-defined lookup below
 * would just never be reached for these names). */
static int ts_call_builtin(const char *norm_name, int argc, char **args, char *result)
{
    if (strcmp(norm_name, "calistir") == 0) {
        tos_exec(argc > 0 ? args[0] : "", result, TS_VAR_VAL);
    } else if (strcmp(norm_name, "dosyaoku") == 0) {
        if (tos_read(argc > 0 ? args[0] : "", result, TS_VAR_VAL) < 0) result[0] = 0;
    } else if (strcmp(norm_name, "dosyayaz") == 0) {
        const char *data = argc > 1 ? args[1] : "";
        int ok = (argc > 0) && tos_write(args[0], data, (int)strlen(data)) == 0;
        strcpy(result, ok ? "1" : "0");
    } else if (strcmp(norm_name, "klasoryap") == 0) {
        strcpy(result, (argc > 0 && tos_mkdir(args[0]) == 0) ? "1" : "0");
    } else if (strcmp(norm_name, "dosyasil") == 0) {
        strcpy(result, (argc > 0 && tos_delete(args[0]) == 0) ? "1" : "0");
    } else if (strcmp(norm_name, "dosyavarmi") == 0) {
        strcpy(result, (argc > 0 && tos_exists(args[0])) ? "1" : "0");
    } else if (strcmp(norm_name, "listele") == 0) {
        if (argc == 0 || tos_list(args[0], result, TS_VAR_VAL) < 0) result[0] = 0;
    } else if (strcmp(norm_name, "uygulamaac") == 0) {
        strcpy(result, (argc > 0 && tos_open_app(args[0]) == 0) ? "1" : "0");
    } else if (strcmp(norm_name, "surecler") == 0) {
        tos_ps(result, TS_VAR_VAL);
    } else if (strcmp(norm_name, "sureldur") == 0) {
        strcpy(result, (argc > 0 && tos_kill((uint32_t)atoi(args[0])) == 0) ? "1" : "0");
    } else if (strcmp(norm_name, "calismasuresi") == 0) {
        ts_itoa((int)tos_uptime(), result);
    } else {
        return 0;
    }
    return 1;
}

static void ts_call_func(const char *name, int argc, char **args, char *result)
{
    char norm_name[TS_VAR_NAME];
    ts_normalize(norm_name, name);

    if (ts_call_builtin(norm_name, argc, args, result)) return;

    for (int i = 0; i < ts_func_count; i++) {
        if (!ts_functions[i].defined) continue;
        char fn[TS_VAR_NAME];
        ts_normalize(fn, ts_functions[i].name);
        if (strcmp(norm_name, fn) != 0) continue;

        ts_var_t saved_vars[TS_MAX_VARS];
        int saved_count = ts_var_count;
        memcpy(saved_vars, ts_vars, sizeof(ts_var_t) * ts_var_count);

        for (int j = 0; j < ts_functions[i].param_count && j < argc; j++) {
            ts_var_set(ts_functions[i].params[j], args[j]);
        }

        int old_return = ts_return_flag;
        ts_return_flag = 0;

        ts_exec_lines(ts_functions[i].body, 0, ts_functions[i].body_count, NULL);

        if (ts_return_flag && result) {
            strncpy(result, ts_return_val, TS_VAR_VAL - 1);
        }
        ts_return_flag = old_return;

        ts_var_count = saved_count;
        memcpy(ts_vars, saved_vars, sizeof(ts_var_t) * saved_count);
        return;
    }

    char buf[TS_LINE_LEN];
    
    char *p = buf;
    memcpy(p, "tsharp: unknown function '", 26); p += 26;
    while (*name) *p++ = *name++;
    memcpy(p, "'\n", 2); p += 2;
    *p = '\0';
    terminal_writestring(buf);
}

static int ts_find_block_end(char **lines, int start, int total)
{
    int depth = 1;
    for (int i = start; i < total; i++) {
        char buf[TS_LINE_LEN];
        ts_normalize(buf, lines[i]);
        ts_trim(buf);
        if (strncmp(buf, "eger ", 5) == 0 || strncmp(buf, "dongu ", 6) == 0 || 
            strncmp(buf, "her ", 4) == 0 || strncmp(buf, "fonksiyon ", 10) == 0) {
            depth++;
        } else if (strcmp(buf, "son") == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return total - 1;
}

static void ts_exec_lines(char **lines, int start, int end, int *next_line)
{
    int i = start;
    while (i < end && !ts_return_flag && !ts_break_flag) {
        if (ts_continue_flag) break;

        char raw[TS_LINE_LEN];
        ts_normalize(raw, lines[i]);
        ts_trim(raw);

        if (!raw[0] || raw[0] == '/' || raw[0] == '#') { i++; continue; }
        if (strcmp(raw, "son") == 0) { i++; break; }
        if (strcmp(raw, "dur") == 0) { ts_break_flag = 1; i++; break; }
        if (strcmp(raw, "devam") == 0) { ts_continue_flag = 1; i++; break; }

        if (strncmp(raw, "dondur", 6) == 0) {
            char tmp[TS_LINE_LEN];
            ts_normalize(tmp, lines[i] + 6);
            ts_trim(tmp);
            if (tmp[0]) {
                char *val = ts_eval_expr(tmp);
                strncpy(ts_return_val, val ? val : "", TS_VAR_VAL - 1);
                free(val);
            } else {
                ts_return_val[0] = '\0';
            }
            ts_return_flag = 1;
            i++; break;
        }
        if (strncmp(raw, "don ", 4) == 0) {
            char tmp[TS_LINE_LEN];
            ts_normalize(tmp, lines[i] + 3);
            ts_trim(tmp);
            char *val = ts_eval_expr(tmp);
            strncpy(ts_return_val, val ? val : "", TS_VAR_VAL - 1);
            free(val);
            ts_return_flag = 1;
            i++; break;
        }

        if (strncmp(raw, "eger ", 5) == 0) {
            int be = ts_find_block_end(lines, i + 1, end);
            char cond[TS_LINE_LEN];
            int ci;
            for (ci = 5; raw[ci] && raw[ci] != ':'; ci++);
            strncpy(cond, raw + 5, ci - 5);
            cond[ci - 5] = '\0';
            ts_trim(cond);

            int if_true = ts_eval_condition(cond);

            int else_start = -1;
            int else_end = -1;
            int depth = 1;
            for (int j = i + 1; j < be; j++) {
                char sbuf[TS_LINE_LEN];
                ts_normalize(sbuf, lines[j]);
                ts_trim(sbuf);
                if (strncmp(sbuf, "eger ", 5) == 0 || strncmp(sbuf, "dongu ", 6) == 0 || 
                    strncmp(sbuf, "her ", 4) == 0 || strncmp(sbuf, "fonksiyon ", 10) == 0) {
                    depth++;
                } else if (strcmp(sbuf, "son") == 0) {
                    depth--;
                } else if (depth == 1 && (strcmp(sbuf, "degilse:") == 0 || strcmp(sbuf, "degilse") == 0)) {
                    else_start = j + 1;
                    else_end = be;
                    be = j;
                    break;
                }
            }

            if (if_true) {
                ts_exec_lines(lines, i + 1, be, NULL);
            } else if (else_start >= 0) {
                ts_exec_lines(lines, else_start, else_end, NULL);
            }
            i = be + 1;
            continue;
        }

        if (strncmp(raw, "dongu ", 6) == 0) {
            int be = ts_find_block_end(lines, i + 1, end);
            char cond[TS_LINE_LEN];
            int ci;
            for (ci = 6; raw[ci] && raw[ci] != ':'; ci++);
            strncpy(cond, raw + 6, ci - 6);
            cond[ci - 6] = '\0';
            ts_trim(cond);

            int loop_count = 0;
            while (ts_eval_condition(cond) && loop_count < TS_LOOP_MAX) {
                loop_count++;
                ts_break_flag = 0;
                ts_continue_flag = 0;
                ts_exec_lines(lines, i + 1, be, NULL);
                if (ts_break_flag) { ts_break_flag = 0; break; }
                if (ts_return_flag) break;
            }
            if (loop_count >= TS_LOOP_MAX)
                terminal_writestring("tsharp: loop limit exceeded\n");
            ts_continue_flag = 0;
            i = be + 1;
            continue;
        }

        if (strncmp(raw, "her ", 4) == 0) {
            int be = ts_find_block_end(lines, i + 1, end);
            char body[TS_LINE_LEN];
            ts_normalize(body, lines[i]);
            int ci;
            for (ci = 4; body[ci] && body[ci] != ':'; ci++);
            body[ci] = '\0';
            
            char *inc = NULL;
            for (int si = 0; body[3 + si]; si++) {
                if (body[3 + si] == ' ' && strncmp(body + 3 + si + 1, "icinde", 6) == 0) {
                    inc = body + 3 + si;
                    break;
                }
            }
            if (!inc) { i++; continue; }
            *inc = '\0';
            char var_name[TS_VAR_NAME];
            strncpy(var_name, body + 3, TS_VAR_NAME - 1);
            ts_trim(var_name);
            char *inc_body = inc;
            while (*inc_body == ' ') inc_body++;
            inc_body += 6;
            ts_trim(inc_body);
            
            char *list_str = ts_eval_expr(inc_body);
            
            if (list_str && list_str[0] == '[') {
                int llen = strlen(list_str);
                char inner[TS_LINE_LEN];
                int inner_len = llen - 2;
                if (inner_len > TS_LINE_LEN - 1) inner_len = TS_LINE_LEN - 1;
                memcpy(inner, list_str + 1, inner_len);
                inner[inner_len] = '\0';
                
                char *item = inner;
                char *next;
                while (item && *item && !ts_break_flag && !ts_return_flag) {
                    while (*item == ' ') item++;
                    next = NULL;
                    for (char *p = item; *p; p++) {
                        if (*p == ',') { *p = '\0'; next = p + 1; break; }
                    }
                    ts_trim(item);
                    if (*item) {
                        char *v = ts_eval_expr(item);
                        ts_var_set(var_name, v ? v : item);
                        free(v);
                        ts_continue_flag = 0;
                        ts_exec_lines(lines, i + 1, be, NULL);
                    }
                    item = next;
                }
            }
            
            free(list_str);
            ts_break_flag = 0;
            ts_continue_flag = 0;
            i = be + 1;
            continue;
        }

        if (strncmp(raw, "fonksiyon ", 10) == 0) {
            int be = ts_find_block_end(lines, i + 1, end);
            char fb[TS_LINE_LEN];
            ts_normalize(fb, lines[i]);
            char *p = fb + 10;
            ts_trim(p);
            if (ts_func_count >= TS_MAX_VARS) { i++; continue; }
            ts_func_t *f = &ts_functions[ts_func_count++];
            memset(f, 0, sizeof(ts_func_t));

            char *lp = strchr(p, '(');
            if (!lp) { i++; continue; }
            *lp = '\0';
            strncpy(f->name, p, TS_VAR_NAME - 1);
            ts_trim(f->name);
            lp++;
            char *rp = lp;
            for (; *rp && *rp != ')'; rp++);
            if (*rp) *rp = '\0';
            char *param = lp;
            char *next_param;
            while (param && *param && f->param_count < TS_MAX_ARGS) {
                while (*param == ' ') param++;
                next_param = NULL;
                for (char *pp = param; *pp; pp++) {
                    if (*pp == ',') { *pp = '\0'; next_param = pp + 1; break; }
                }
                ts_trim(param);
                if (*param) {
                    strncpy(f->params[f->param_count], param, TS_VAR_NAME - 1);
                    f->param_count++;
                }
                param = next_param;
            }

            f->body_count = be - i - 1;
            f->body = malloc(sizeof(char *) * (f->body_count + 1));
            for (int j = 0; j < f->body_count; j++) {
                f->body[j] = ts_strdup(lines[i + 1 + j]);
            }
            f->body[f->body_count] = NULL;
            f->defined = 1;
            ts_var_set(f->name, "1");

            i = be + 1;
            continue;
        }

        if (strncmp(raw, "yazdir", 6) == 0) {
            const char *body = lines[i] + 6;
            while (*body == ' ') body++;
            if (!*body) { terminal_putchar('\n'); i++; continue; }

            char buf[TS_LINE_LEN];
            ts_normalize(buf, body);
            
            char *final = ts_eval_expr(buf);
            
            char *p = final;
            if ((p[0] == '"' || p[0] == '\'') && p[strlen(p)-1] == p[0]) {
                p[strlen(p)-1] = '\0';
                p++;
            }
            terminal_writestring(p);
            terminal_putchar('\n');
            free(final);
            i++; continue;
        }

        if (strncmp(raw, "degisken ", 9) == 0) {
            char buf[TS_LINE_LEN];
            ts_normalize(buf, lines[i] + 9);
            ts_trim(buf);
            if (!*buf) { i++; continue; }

            char name[TS_VAR_NAME];
            char val_str[TS_VAR_VAL] = "";

            char *eq = strchr(buf, '=');
            if (eq) {
                *eq = '\0';
                ts_trim(buf);
                strncpy(name, buf, TS_VAR_NAME - 1);
                eq++;
                ts_trim(eq);
                strncpy(val_str, eq, TS_VAR_VAL - 1);
            } else {
                char *sp = strchr(buf, ' ');
                if (sp) {
                    *sp = '\0';
                    strncpy(name, buf, TS_VAR_NAME - 1);
                    sp++;
                    ts_trim(sp);
                    strncpy(val_str, sp, TS_VAR_VAL - 1);
                } else {
                    strncpy(name, buf, TS_VAR_NAME - 1);
                }
            }

            if (val_str[0]) {
                char *val = ts_eval_expr(val_str);
                ts_var_set(name, val);
                free(val);
            } else {
                ts_var_set(name, "");
            }
            i++; continue;
        }

        if (strncmp(raw, "girdi ", 6) == 0) {
            char buf[TS_LINE_LEN];
            ts_normalize(buf, lines[i] + 6);
            ts_trim(buf);

            char name[TS_VAR_NAME];
            char prompt[TS_LINE_LEN] = "";

            char *sp = strchr(buf, ' ');
            if (sp) {
                *sp = '\0';
                strncpy(name, buf, TS_VAR_NAME - 1);
                sp++;
                ts_trim(sp);
                char *evaled = ts_eval_expr(sp);
                strncpy(prompt, evaled, TS_LINE_LEN - 1);
                free(evaled);
            } else {
                strncpy(name, buf, TS_VAR_NAME - 1);
            }

            terminal_writestring(prompt);
            char input[TS_LINE_LEN];
            keyboard_readline(input, TS_LINE_LEN);
            ts_var_set(name, input);
            i++; continue;
        }

        if (strncmp(raw, "bekle ", 6) == 0) {
            char buf[TS_LINE_LEN];
            ts_normalize(buf, lines[i] + 6);
            ts_trim(buf);
            char *val = ts_eval_expr(buf);
            if (val) {
                double sec = ts_to_num(val);
                if (sec > 0) {
                    for (volatile double t = 0; t < sec * 5000000; t++);
                }
                free(val);
            }
            i++; continue;
        }

        char *eq = strchr(raw, '=');
        if (eq && eq != raw && *(eq-1) != '>' && *(eq-1) != '<' && *(eq-1) != '!') {
            char lh[TS_LINE_LEN];
            int llen = eq - raw;
            if (llen > TS_LINE_LEN - 1) llen = TS_LINE_LEN - 1;
            memcpy(lh, raw, llen); lh[llen] = '\0';
            ts_trim(lh);
            char *rh = ts_strdup(eq + 1);
            ts_trim(rh);
            
            char *op = NULL;
            if (ts_strstr(lh, "+=")) op = "+=";
            else if (ts_strstr(lh, "-=")) op = "-=";
            else if (ts_strstr(lh, "*=")) op = "*=";
            else if (ts_strstr(lh, "/=")) op = "/=";
            
            if (op) {
                char *star = ts_strstr(lh, op);
                *star = '\0';
                ts_trim(lh);
                const char *cv = ts_var_get(lh);
                double curr = cv ? ts_to_num(cv) : 0;
                char *rv = ts_eval_expr(rh);
                double rval = ts_to_num(rv);
                double result = 0;
                if (strcmp(op, "+=") == 0) result = curr + rval;
                else if (strcmp(op, "-=") == 0) result = curr - rval;
                else if (strcmp(op, "*=") == 0) result = curr * rval;
                else if (strcmp(op, "/=") == 0) {
                    if (rval == 0) terminal_writestring("tsharp: divide by zero\n");
                    else result = curr / rval;
                }
                char resb[64]; ts_ftoa(result, resb);
                ts_var_set(lh, resb);
                free(rv);
            } else {
                char *val = ts_eval_expr(rh);
                ts_var_set(lh, val ? val : "");
                free(val);
            }
            free(rh);
            i++; continue;
        }

        char cline[TS_LINE_LEN];
        strncpy(cline, raw, TS_LINE_LEN - 1);
        ts_trim(cline);
        char *lp = strchr(cline, '(');
        if (lp) {
            *lp = '\0';
            char fname[TS_VAR_NAME];
            strncpy(fname, cline, TS_VAR_NAME - 1);
            ts_trim(fname);
            lp++;
            char *rp = lp;
            for (; *rp && *rp != ')'; rp++);
            if (*rp) {
                *rp = '\0';
                char *toks[64]; int tn;
                tn = ts_tokenize_expr(lp, toks, 64);
                char *args[TS_MAX_ARGS]; int ac = 0;
                for (int ti = 0; ti < tn; ti++) {
                    if (strcmp(toks[ti], ",") == 0) continue;
                    if (ac < TS_MAX_ARGS) args[ac++] = ts_eval_expr(toks[ti]);
                }
                char res[TS_VAR_VAL] = "";
                ts_call_func(fname, ac, args, res);
                for (int ai = 0; ai < ac; ai++) free(args[ai]);
                ts_free_tokens(toks, tn);
                if (res[0]) ts_var_set(fname, res);
                i++; continue;
            }
        }

        i++;
    }
    if (next_line) *next_line = i;
}

void tsharp_run_file(const char *path)
{
    if (!ramfs_exists(path) || ramfs_is_dir(path)) {
        terminal_writestring("tsharp: file not found: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }

    uint32_t sz = ramfs_size(path);
    char *data = malloc(sz + 1);
    if (!data) { terminal_writestring("tsharp: out of memory\n"); return; }
    ramfs_read(path, data, sz, 0);
    data[sz] = '\0';

    char **lines = malloc(sizeof(char *) * TS_MAX_LINES);
    int line_count = 0;

    char *s = data;
    while (*s && line_count < TS_MAX_LINES) {
        char *nl = strchr(s, '\n');
        int llen = nl ? (int)(nl - s) : (int)strlen(s);
        lines[line_count] = malloc(llen + 1);
        memcpy(lines[line_count], s, llen);
        lines[line_count][llen] = '\0';
        line_count++;
        if (nl) s = nl + 1; else break;
    }

    ts_var_count = 0;
    ts_func_count = 0;
    ts_return_flag = 0;
    ts_break_flag = 0;
    ts_continue_flag = 0;

    ts_exec_lines(lines, 0, line_count, NULL);

    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(lines);
    free(data);
}

void tsharp_run_interactive(void)
{
    terminal_writestring("TSharp 4.1 Lite (Kernel Mode)\n");
    terminal_writestring("Type 'cik' to exit\n\n");

    ts_var_count = 0;
    ts_func_count = 0;
    ts_return_flag = 0;
    ts_break_flag = 0;
    ts_continue_flag = 0;

    char line[TS_LINE_LEN];
    char **block_lines = NULL;
    int block_count = 0;
    int in_block = 0;
    int block_depth = 0;

    for (;;) {
        if (in_block) {
            terminal_writestring("... ");
        } else {
            terminal_writestring(">>> ");
        }

        keyboard_readline(line, TS_LINE_LEN);

        if (!in_block) {
            char buf[TS_LINE_LEN];
            ts_normalize(buf, line);
            ts_trim(buf);
            if (strcmp(buf, "cik") == 0 || strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0) {
                terminal_writestring("TSharp closed.\n");
                return;
            }
        }

        if (!in_block) {
            char buf[TS_LINE_LEN];
            ts_normalize(buf, line);
            ts_trim(buf);
            if (strncmp(buf, "eger ", 5) == 0 || strncmp(buf, "dongu ", 6) == 0 || 
                strncmp(buf, "her ", 4) == 0 || strncmp(buf, "fonksiyon ", 10) == 0) {
                in_block = 1;
                block_depth = 1;
                block_count = 0;
                block_lines = malloc(sizeof(char *) * TS_MAX_LINES);
                block_lines[block_count++] = ts_strdup(line);
                continue;
            }
        }

        if (in_block) {
            if (block_count < TS_MAX_LINES)
                block_lines[block_count++] = ts_strdup(line);
            char buf[TS_LINE_LEN];
            ts_normalize(buf, line);
            ts_trim(buf);
            if (strncmp(buf, "eger ", 5) == 0 || strncmp(buf, "dongu ", 6) == 0 || 
                strncmp(buf, "her ", 4) == 0 || strncmp(buf, "fonksiyon ", 10) == 0) {
                block_depth++;
            } else if (strcmp(buf, "son") == 0) {
                block_depth--;
                if (block_depth == 0) {
                    in_block = 0;
                    ts_exec_lines(block_lines, 0, block_count, NULL);
                    for (int i = 0; i < block_count; i++) free(block_lines[i]);
                    free(block_lines);
                    block_lines = NULL;
                    block_count = 0;
                }
            }
            continue;
        }

        char buf[TS_LINE_LEN];
        ts_normalize(buf, line);
        ts_trim(buf);
        if (!buf[0] || buf[0] == '/' || buf[0] == '#') continue;
        if (strcmp(buf, "son") == 0) continue;

        char *single[2] = { ts_strdup(line), NULL };
        ts_exec_lines(single, 0, 1, NULL);
        free(single[0]);
    }
}
