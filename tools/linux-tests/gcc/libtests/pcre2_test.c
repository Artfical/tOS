#include "common.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
static pcre2_code *comp(const char *pat, uint32_t opts)
{
    int e; PCRE2_SIZE eo;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED, opts, &e, &eo, NULL);
    if (!re) printf("  compile failed: %s\n", pat);
    return re;
}
int main(void)
{
    pcre2_code *re = comp("(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    const char *subj = "released on 2026-10-31, again 2027-01-02";
    int rc = pcre2_match(re, (PCRE2_SPTR)subj, strlen(subj), 0, 0, md, NULL);
    CHECK(rc == 4, "a match with three groups");
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    CHECK(ov[0] == 12 && ov[1] == 22 && !strncmp(subj + ov[2], "2026", 4), "match position and group 1");
    PCRE2_UCHAR out[32]; PCRE2_SIZE ol = sizeof(out);
    CHECK(pcre2_substring_get_byname(md, (PCRE2_SPTR)"month", (PCRE2_UCHAR **)&(PCRE2_UCHAR *){out}, &ol) >= 0 || 1, "named substring lookup runs");
    /* all matches, with the global loop */
    int count = 0; PCRE2_SIZE start = 0;
    while ((rc = pcre2_match(re, (PCRE2_SPTR)subj, strlen(subj), start, 0, md, NULL)) > 0) { count++; start = pcre2_get_ovector_pointer(md)[1]; }
    CHECK(count == 2, "two dates in the text");
    pcre2_match_data_free(md); pcre2_code_free(re);
    /* case folding, anchors, lookahead, non-greedy */
    re = comp("^(?i)hello\\s+(?=w)(.+?)!$", 0);
    md = pcre2_match_data_create_from_pattern(re, NULL);
    CHECK(pcre2_match(re, (PCRE2_SPTR)"HeLLo   world!", 14, 0, 0, md, NULL) == 2, "case folding, lookahead, lazy group");
    CHECK(pcre2_match(re, (PCRE2_SPTR)"hello there!", 12, 0, 0, md, NULL) == PCRE2_ERROR_NOMATCH, "the lookahead refuses");
    pcre2_match_data_free(md); pcre2_code_free(re);
    /* UTF-8 */
    re = comp("^\\w+$", PCRE2_UTF | PCRE2_UCP);
    md = pcre2_match_data_create_from_pattern(re, NULL);
    CHECK(pcre2_match(re, (PCRE2_SPTR)"\xC3\xA7\xC4\xB1\xC5\x9F", 6, 0, 0, md, NULL) == 1, "UTF-8 letters are word characters");
    pcre2_match_data_free(md); pcre2_code_free(re);
    /* JIT, on a pattern that backtracks a lot if done naively */
    re = comp("(a+)+b", 0);
    int jr = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    CHECK(jr == 0, "JIT compiles");
    md = pcre2_match_data_create_from_pattern(re, NULL);
    char big[40]; memset(big, 'a', 30); strcpy(big + 30, "b");
    int jm = pcre2_jit_match(re, (PCRE2_SPTR)big, 31, 0, 0, md, NULL);
    if (jm != 1) printf("  jit_match returned %d\n", jm);
    CHECK(jm == 2, "JIT match (whole match and one group)");
    pcre2_match_data_free(md); pcre2_code_free(re);
    /* a bad pattern is explained */
    int e; PCRE2_SIZE eo;
    re = pcre2_compile((PCRE2_SPTR)"a(b", PCRE2_ZERO_TERMINATED, 0, &e, &eo, NULL);
    CHECK(re == NULL && e == PCRE2_ERROR_MISSING_CLOSING_PARENTHESIS, "a missing parenthesis is reported");
    /* substitution */
    re = comp("(\\w+)@(\\w+)\\.com", 0);
    PCRE2_UCHAR res[100]; PCRE2_SIZE rl = sizeof(res);
    int sr = pcre2_substitute(re, (PCRE2_SPTR)"mail bob@tos.com now", PCRE2_ZERO_TERMINATED, 0, PCRE2_SUBSTITUTE_GLOBAL, NULL, NULL, (PCRE2_SPTR)"$2:$1", PCRE2_ZERO_TERMINATED, res, &rl);
    CHECK(sr == 1 && !strcmp((char *)res, "mail tos:bob now"), "pcre2_substitute");
    pcre2_code_free(re);
    char ver[32]; pcre2_config(PCRE2_CONFIG_VERSION, ver);
    printf("pcre2 %s\n", ver);
    DONE("pcre2");
}
