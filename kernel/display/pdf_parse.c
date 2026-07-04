#include "pdf_parse.h"
#include "memory.h"
#include "string.h"
#include "png.h"

#define PDF_MAX_OBJS        4096
#define PDF_MAX_ARR_ITEMS   64
#define PDF_DECOMPRESS_CAP  (1024U * 1024U)

typedef struct {
    int num;
    const uint8_t *body_s, *body_e;
} pdf_obj_entry_t;

typedef struct {
    pdf_obj_entry_t *arr;
    int count;
    int cap;
} pdf_objtab_t;

static void set_err(char *err, int cap, const char *msg)
{
    int n = 0;
    while (msg[n] && n < cap - 1) { err[n] = msg[n]; n++; }
    err[n] = 0;
}

static int is_pdf_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0; }
static int is_pdf_delim(uint8_t c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
static int is_pdf_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static const uint8_t *skip_ws(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        if (is_pdf_ws(*p)) { p++; continue; }
        if (*p == '%') { while (p < end && *p != '\n' && *p != '\r') p++; continue; }
        break;
    }
    return p;
}

/* Advances past one PDF "value" token starting at p (name, literal
 * string, hex string, dict, array, number, keyword, or a whole "N G R"
 * indirect reference collapsed into a single token). Used both to walk
 * dictionary key/value pairs and to tokenize content streams. */
static const uint8_t *skip_value(const uint8_t *p, const uint8_t *end)
{
    p = skip_ws(p, end);
    if (p >= end) return p;

    if (*p == '/') {
        p++;
        while (p < end && !is_pdf_ws(*p) && !is_pdf_delim(*p)) p++;
        return p;
    }
    if (*p == '(') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        return p;
    }
    if (*p == '<') {
        if (p + 1 < end && p[1] == '<') {
            p += 2;
            int depth = 1;
            while (p < end && depth > 0) {
                if (p + 1 < end && p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
                if (p + 1 < end && p[0] == '>' && p[1] == '>') { depth--; p += 2; continue; }
                p++;
            }
            return p;
        }
        p++;
        while (p < end && *p != '>') p++;
        if (p < end) p++;
        return p;
    }
    if (*p == '[') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '[') { depth++; p++; }
            else if (*p == ']') { depth--; p++; }
            else if (*p == '(') {
                p++;
                int d2 = 1;
                while (p < end && d2 > 0) {
                    if (*p == '\\' && p + 1 < end) { p += 2; continue; }
                    if (*p == '(') d2++;
                    else if (*p == ')') d2--;
                    p++;
                }
            } else if (*p == '<' && p + 1 < end && p[1] == '<') {
                p += 2;
                int d2 = 1;
                while (p < end && d2 > 0) {
                    if (p + 1 < end && p[0] == '<' && p[1] == '<') { d2++; p += 2; continue; }
                    if (p + 1 < end && p[0] == '>' && p[1] == '>') { d2--; p += 2; continue; }
                    p++;
                }
            } else p++;
        }
        return p;
    }
    if (*p == '+' || *p == '-' || *p == '.' || is_pdf_digit(*p)) {
        const uint8_t *num1_start = p;
        while (p < end && (*p == '+' || *p == '-' || *p == '.' || is_pdf_digit(*p))) p++;
        const uint8_t *after_num1 = p;
        int is_int1 = 1;
        for (const uint8_t *q = num1_start; q < after_num1; q++) if (*q == '.') is_int1 = 0;
        if (is_int1) {
            const uint8_t *q = skip_ws(after_num1, end);
            if (q < end && is_pdf_digit(*q)) {
                while (q < end && is_pdf_digit(*q)) q++;
                const uint8_t *r = skip_ws(q, end);
                if (r < end && *r == 'R' && (r + 1 >= end || is_pdf_ws(r[1]) || is_pdf_delim(r[1]))) {
                    return r + 1;
                }
            }
        }
        return after_num1;
    }

    const uint8_t *kstart = p;
    while (p < end && !is_pdf_ws(*p) && !is_pdf_delim(*p)) p++;
    if (p == kstart) p++;
    return p;
}

/* Walks the (inner) bytes of a dictionary looking for "/key", returning
 * the byte span of its value. Depth-correct because it always jumps by
 * a full skip_value() rather than counting brackets loosely. */
static int dict_get(const uint8_t *body, const uint8_t *end, const char *key,
                     const uint8_t **val_s, const uint8_t **val_e)
{
    const uint8_t *p = body;
    int klen = (int)strlen(key);
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) break;
        if (*p != '/') { p = skip_value(p, end); continue; }
        const uint8_t *name_s = p + 1;
        const uint8_t *q = name_s;
        while (q < end && !is_pdf_ws(*q) && !is_pdf_delim(*q)) q++;
        int nlen = (int)(q - name_s);
        const uint8_t *vs = skip_ws(q, end);
        const uint8_t *ve = skip_value(vs, end);
        if (nlen == klen && memcmp(name_s, key, (size_t)klen) == 0) {
            *val_s = vs; *val_e = ve;
            return 1;
        }
        p = ve;
    }
    return 0;
}

static int find_dict_span(const uint8_t *buf, const uint8_t *end, const uint8_t **inner_s, const uint8_t **inner_e)
{
    const uint8_t *p = buf;
    while (p + 1 < end) {
        if (p[0] == '<' && p[1] == '<') {
            const uint8_t *vend = skip_value(p, end);
            *inner_s = p + 2;
            *inner_e = (vend >= p + 4) ? vend - 2 : p + 2;
            return 1;
        }
        p++;
    }
    return 0;
}

static int find_array_span(const uint8_t *buf, const uint8_t *end, const uint8_t **inner_s, const uint8_t **inner_e)
{
    const uint8_t *p = skip_ws(buf, end);
    if (p >= end || *p != '[') return 0;
    const uint8_t *vend = skip_value(p, end);
    *inner_s = p + 1;
    *inner_e = (vend > p + 1) ? vend - 1 : p + 1;
    return 1;
}

static int array_items(const uint8_t *s, const uint8_t *e, const uint8_t **item_s, const uint8_t **item_e, int max_items)
{
    int n = 0;
    const uint8_t *p = s;
    while (p < e && n < max_items) {
        p = skip_ws(p, e);
        if (p >= e) break;
        const uint8_t *vs = p;
        const uint8_t *ve = skip_value(p, e);
        item_s[n] = vs; item_e[n] = ve; n++;
        p = ve;
    }
    return n;
}

static int parse_ref(const uint8_t *vs, const uint8_t *ve, int *out_num)
{
    const uint8_t *p = skip_ws(vs, ve);
    if (p >= ve || !is_pdf_digit(*p)) return 0;
    int num = 0;
    while (p < ve && is_pdf_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
    p = skip_ws(p, ve);
    if (p >= ve || !is_pdf_digit(*p)) return 0;
    while (p < ve && is_pdf_digit(*p)) p++;
    p = skip_ws(p, ve);
    if (p >= ve || *p != 'R') return 0;
    p++;
    p = skip_ws(p, ve);
    if (p != ve) return 0;
    *out_num = num;
    return 1;
}

static int find_obj(pdf_objtab_t *tab, int num, const uint8_t **body_s, const uint8_t **body_e)
{
    for (int i = 0; i < tab->count; i++) {
        if (tab->arr[i].num == num) { *body_s = tab->arr[i].body_s; *body_e = tab->arr[i].body_e; return 1; }
    }
    return 0;
}

static int resolve_dict_span(pdf_objtab_t *tab, const uint8_t *vs, const uint8_t *ve, int depth,
                              const uint8_t **inner_s, const uint8_t **inner_e)
{
    if (depth > 8) return 0;
    int refnum;
    if (parse_ref(vs, ve, &refnum)) {
        const uint8_t *bs, *be;
        if (!find_obj(tab, refnum, &bs, &be)) return 0;
        return resolve_dict_span(tab, bs, be, depth + 1, inner_s, inner_e);
    }
    return find_dict_span(vs, ve, inner_s, inner_e);
}

static int resolve_array_span(pdf_objtab_t *tab, const uint8_t *vs, const uint8_t *ve, int depth,
                               const uint8_t **arr_s, const uint8_t **arr_e)
{
    if (depth > 8) return 0;
    int refnum;
    if (parse_ref(vs, ve, &refnum)) {
        const uint8_t *bs, *be;
        if (!find_obj(tab, refnum, &bs, &be)) return 0;
        return resolve_array_span(tab, bs, be, depth + 1, arr_s, arr_e);
    }
    return find_array_span(vs, ve, arr_s, arr_e);
}

static int resolve_number(pdf_objtab_t *tab, const uint8_t *vs, const uint8_t *ve, int depth, double *out)
{
    if (depth > 8) return 0;
    int refnum;
    if (parse_ref(vs, ve, &refnum)) {
        const uint8_t *bs, *be;
        if (!find_obj(tab, refnum, &bs, &be)) return 0;
        return resolve_number(tab, bs, be, depth + 1, out);
    }
    const uint8_t *p = skip_ws(vs, ve);
    int neg = 0;
    double val = 0;
    int any = 0;
    if (p < ve && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
    while (p < ve && is_pdf_digit(*p)) { val = val * 10 + (*p - '0'); p++; any = 1; }
    if (p < ve && *p == '.') {
        p++;
        double frac = 0.1;
        while (p < ve && is_pdf_digit(*p)) { val += (*p - '0') * frac; frac *= 0.1; p++; any = 1; }
    }
    if (!any) return 0;
    *out = neg ? -val : val;
    return 1;
}

static int try_parse_objdef(const uint8_t *data, uint32_t size, uint32_t i, int *out_num, uint32_t *out_bs, uint32_t *out_be)
{
    uint32_t p = i;
    if (!is_pdf_digit(data[p])) return 0;
    uint32_t num = 0;
    while (p < size && is_pdf_digit(data[p])) { num = num * 10 + (data[p] - '0'); p++; }
    if (p >= size || !is_pdf_ws(data[p])) return 0;
    while (p < size && is_pdf_ws(data[p])) p++;
    if (p >= size || !is_pdf_digit(data[p])) return 0;
    while (p < size && is_pdf_digit(data[p])) p++;
    if (p >= size || !is_pdf_ws(data[p])) return 0;
    while (p < size && is_pdf_ws(data[p])) p++;
    if (p + 3 > size || data[p] != 'o' || data[p + 1] != 'b' || data[p + 2] != 'j') return 0;
    p += 3;
    uint32_t body_s = p;
    uint32_t e = body_s;
    while (e + 6 <= size && !(data[e] == 'e' && data[e + 1] == 'n' && data[e + 2] == 'd' && data[e + 3] == 'o' && data[e + 4] == 'b' && data[e + 5] == 'j')) e++;
    if (e + 6 > size) return 0;
    *out_num = (int)num; *out_bs = body_s; *out_be = e;
    return 1;
}

static void scan_objects(const uint8_t *data, uint32_t size, pdf_objtab_t *tab)
{
    uint32_t i = 0;
    while (i < size) {
        int num; uint32_t bs, be;
        if (try_parse_objdef(data, size, i, &num, &bs, &be)) {
            int found = 0;
            for (int j = 0; j < tab->count; j++) {
                if (tab->arr[j].num == num) { tab->arr[j].body_s = data + bs; tab->arr[j].body_e = data + be; found = 1; break; }
            }
            if (!found && tab->count < tab->cap) {
                tab->arr[tab->count].num = num;
                tab->arr[tab->count].body_s = data + bs;
                tab->arr[tab->count].body_e = data + be;
                tab->count++;
            }
            i = be;
            continue;
        }
        i++;
    }
}

static int decode_literal_string(const uint8_t *s, const uint8_t *e, char *out, int out_cap)
{
    int n = 0;
    while (s < e && n < out_cap - 1) {
        if (*s == '\\' && s + 1 < e) {
            s++;
            switch (*s) {
                case 'n': out[n++] = '\n'; s++; break;
                case 'r': out[n++] = '\r'; s++; break;
                case 't': out[n++] = '\t'; s++; break;
                case '(': out[n++] = '('; s++; break;
                case ')': out[n++] = ')'; s++; break;
                case '\\': out[n++] = '\\'; s++; break;
                default:
                    if (*s >= '0' && *s <= '7') {
                        int v = 0, cnt = 0;
                        while (s < e && *s >= '0' && *s <= '7' && cnt < 3) { v = v * 8 + (*s - '0'); s++; cnt++; }
                        out[n++] = (char)v;
                    } else { out[n++] = (char)*s; s++; }
                    break;
            }
        } else { out[n++] = (char)*s; s++; }
    }
    return n;
}

static int decode_hex_string(const uint8_t *s, const uint8_t *e, char *out, int out_cap)
{
    int n = 0, hi = -1;
    for (const uint8_t *p = s; p < e && n < out_cap - 1; p++) {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { out[n++] = (char)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0 && n < out_cap - 1) out[n++] = (char)(hi << 4);
    return n;
}

static void add_run(pdf_page_t *pg, double x, double y, double size, const char *text, int len)
{
    if (pg->run_count >= PDF_MAX_RUNS_PER_PAGE) return;
    if (len <= 0) return;
    if (len > PDF_MAX_RUN_LEN - 1) len = PDF_MAX_RUN_LEN - 1;
    pdf_run_t *r = &pg->runs[pg->run_count++];
    r->x = x; r->y = y; r->size = size;
    memcpy(r->text, text, (size_t)len);
    r->text[len] = 0;
    r->len = len;
}

typedef struct {
    double tlm_x, tlm_y, tm_x, tm_y, font_size, leading;
} pdf_text_state_t;

#define TOK_IS(kstart, klen, s) ((klen) == (int)(sizeof(s) - 1) && memcmp((kstart), (s), (klen)) == 0)

static void tokenize_content(const uint8_t *data, uint32_t len, pdf_page_t *pg)
{
    const uint8_t *p = data, *end = data + len;
    pdf_text_state_t ts = { 0, 0, 0, 0, 12.0, 0.0 };
    double nums[8];
    int nn = 0;
    char pend_text[PDF_MAX_RUN_LEN];
    int pend_len = 0, have_pend = 0;

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) break;

        if (*p == '/') { p = skip_value(p, end); continue; }

        if (*p == '(') {
            const uint8_t *vs = p + 1;
            const uint8_t *ve = skip_value(p, end);
            const uint8_t *content_end = (ve > p) ? ve - 1 : vs;
            pend_len = decode_literal_string(vs, content_end, pend_text, (int)sizeof(pend_text));
            have_pend = 1;
            p = ve;
            continue;
        }
        if (*p == '<' && !(p + 1 < end && p[1] == '<')) {
            const uint8_t *vs = p + 1;
            const uint8_t *ve = skip_value(p, end);
            const uint8_t *content_end = (ve > p) ? ve - 1 : vs;
            pend_len = decode_hex_string(vs, content_end, pend_text, (int)sizeof(pend_text));
            have_pend = 1;
            p = ve;
            continue;
        }
        if (*p == '[') {
            const uint8_t *arr_s = p, *arr_e = p;
            find_array_span(p, end, &arr_s, &arr_e);
            const uint8_t *ve = skip_value(p, end);
            pend_len = 0;
            const uint8_t *q = arr_s;
            while (q < arr_e) {
                q = skip_ws(q, arr_e);
                if (q >= arr_e) break;
                if (*q == '(') {
                    const uint8_t *ss = q + 1;
                    const uint8_t *se = skip_value(q, arr_e);
                    const uint8_t *sc = (se > q) ? se - 1 : ss;
                    if (pend_len < (int)sizeof(pend_text) - 1)
                        pend_len += decode_literal_string(ss, sc, pend_text + pend_len, (int)sizeof(pend_text) - pend_len);
                    q = se;
                } else {
                    q = skip_value(q, arr_e);
                }
            }
            have_pend = 1;
            p = ve;
            continue;
        }
        if (*p == '+' || *p == '-' || *p == '.' || is_pdf_digit(*p)) {
            const uint8_t *vs = p;
            const uint8_t *ve = skip_value(p, end);
            const uint8_t *q = vs;
            int neg = 0;
            double val = 0;
            if (q < ve && (*q == '-' || *q == '+')) { neg = (*q == '-'); q++; }
            while (q < ve && is_pdf_digit(*q)) { val = val * 10 + (*q - '0'); q++; }
            if (q < ve && *q == '.') {
                q++;
                double frac = 0.1;
                while (q < ve && is_pdf_digit(*q)) { val += (*q - '0') * frac; frac *= 0.1; q++; }
            }
            if (neg) val = -val;
            if (nn < 8) nums[nn++] = val;
            p = ve;
            continue;
        }

        const uint8_t *kstart = p;
        while (p < end && !is_pdf_ws(*p) && !is_pdf_delim(*p)) p++;
        if (p == kstart) { p++; continue; }
        int klen = (int)(p - kstart);

        if (TOK_IS(kstart, klen, "BT")) {
            ts.tlm_x = 0; ts.tlm_y = 0; ts.tm_x = 0; ts.tm_y = 0;
        } else if (TOK_IS(kstart, klen, "Tf")) {
            if (nn >= 1) ts.font_size = nums[nn - 1];
        } else if (TOK_IS(kstart, klen, "Td")) {
            if (nn >= 2) { ts.tlm_x += nums[nn - 2]; ts.tlm_y += nums[nn - 1]; ts.tm_x = ts.tlm_x; ts.tm_y = ts.tlm_y; }
        } else if (TOK_IS(kstart, klen, "TD")) {
            if (nn >= 2) { ts.leading = -nums[nn - 1]; ts.tlm_x += nums[nn - 2]; ts.tlm_y += nums[nn - 1]; ts.tm_x = ts.tlm_x; ts.tm_y = ts.tlm_y; }
        } else if (TOK_IS(kstart, klen, "Tm")) {
            if (nn >= 6) { ts.tlm_x = nums[nn - 2]; ts.tlm_y = nums[nn - 1]; ts.tm_x = ts.tlm_x; ts.tm_y = ts.tlm_y; }
        } else if (TOK_IS(kstart, klen, "T*")) {
            ts.tlm_y -= ts.leading; ts.tm_x = ts.tlm_x; ts.tm_y = ts.tlm_y;
        } else if (TOK_IS(kstart, klen, "TL")) {
            if (nn >= 1) ts.leading = nums[nn - 1];
        } else if (TOK_IS(kstart, klen, "Tj") || TOK_IS(kstart, klen, "TJ")) {
            if (have_pend) { add_run(pg, ts.tm_x, ts.tm_y, ts.font_size, pend_text, pend_len); ts.tm_x += pend_len * ts.font_size * 0.5; }
        } else if (TOK_IS(kstart, klen, "'") || TOK_IS(kstart, klen, "\"")) {
            ts.tlm_y -= ts.leading; ts.tm_x = ts.tlm_x; ts.tm_y = ts.tlm_y;
            if (have_pend) { add_run(pg, ts.tm_x, ts.tm_y, ts.font_size, pend_text, pend_len); ts.tm_x += pend_len * ts.font_size * 0.5; }
        }

        nn = 0; have_pend = 0;
    }
}

static void process_content_stream_ref(pdf_objtab_t *tab, const uint8_t *vs, const uint8_t *ve, pdf_page_t *pg)
{
    int refnum;
    const uint8_t *body_s, *body_e;
    if (parse_ref(vs, ve, &refnum)) {
        if (!find_obj(tab, refnum, &body_s, &body_e)) return;
    } else {
        body_s = vs; body_e = ve;
    }

    const uint8_t *dict_is = NULL, *dict_ie = NULL;
    find_dict_span(body_s, body_e, &dict_is, &dict_ie);

    const uint8_t *p = body_s;
    const uint8_t *stream_kw = NULL;
    while (p + 6 <= body_e) {
        if (p[0] == 's' && p[1] == 't' && p[2] == 'r' && p[3] == 'e' && p[4] == 'a' && p[5] == 'm') { stream_kw = p; break; }
        p++;
    }
    if (!stream_kw) return;

    const uint8_t *raw_s = stream_kw + 6;
    if (raw_s < body_e && *raw_s == '\r') raw_s++;
    if (raw_s < body_e && *raw_s == '\n') raw_s++;

    const uint8_t *endp = raw_s;
    while (endp + 9 <= body_e && memcmp(endp, "endstream", 9) != 0) endp++;
    if (endp + 9 > body_e) return;

    const uint8_t *raw_e = endp;
    if (raw_e > raw_s && raw_e[-1] == '\n') raw_e--;
    if (raw_e > raw_s && raw_e[-1] == '\r') raw_e--;

    int is_flate = 0;
    if (dict_is) {
        const uint8_t *fs, *fe;
        if (dict_get(dict_is, dict_ie, "Filter", &fs, &fe)) {
            if (fe - fs >= 12 && fs[0] == '/' && memcmp(fs, "/FlateDecode", 12) == 0) is_flate = 1;
        }
    }

    if (!is_flate) {
        tokenize_content(raw_s, (uint32_t)(raw_e - raw_s), pg);
        return;
    }

    if (raw_e - raw_s < 2) return;
    uint32_t deflate_len = (uint32_t)(raw_e - raw_s) - 2;
    const uint8_t *deflate_data = raw_s + 2;
    uint32_t cap = deflate_len * 8U > 8192U ? deflate_len * 8U : 8192U;
    if (cap > PDF_DECOMPRESS_CAP) cap = PDF_DECOMPRESS_CAP;
    uint8_t *outbuf = (uint8_t *)malloc(cap);
    if (!outbuf) return;
    uint32_t outlen = 0;
    if (inflate_raw_buffer(deflate_data, deflate_len, outbuf, cap, &outlen) == 0) {
        tokenize_content(outbuf, outlen, pg);
    }
    free(outbuf);
}

static void parse_page_contents(pdf_objtab_t *tab, const uint8_t *vs, const uint8_t *ve, pdf_page_t *pg)
{
    const uint8_t *p = skip_ws(vs, ve);
    if (p < ve && *p == '[') {
        const uint8_t *arr_s, *arr_e;
        if (find_array_span(p, ve, &arr_s, &arr_e)) {
            const uint8_t *is[PDF_MAX_ARR_ITEMS], *ie[PDF_MAX_ARR_ITEMS];
            int n = array_items(arr_s, arr_e, is, ie, PDF_MAX_ARR_ITEMS);
            for (int i = 0; i < n; i++) process_content_stream_ref(tab, is[i], ie[i], pg);
        }
        return;
    }
    process_content_stream_ref(tab, vs, ve, pg);
}

static void collect_pages(pdf_objtab_t *tab, const uint8_t *dict_s, const uint8_t *dict_e,
                           const double inh_mb[4], pdf_doc_t *doc, int depth)
{
    if (depth > 8 || doc->page_count >= PDF_MAX_PAGES) return;

    double mb[4] = { inh_mb[0], inh_mb[1], inh_mb[2], inh_mb[3] };
    const uint8_t *mbs, *mbe;
    if (dict_get(dict_s, dict_e, "MediaBox", &mbs, &mbe)) {
        const uint8_t *arr_s, *arr_e;
        if (resolve_array_span(tab, mbs, mbe, 0, &arr_s, &arr_e)) {
            const uint8_t *is[4], *ie[4];
            if (array_items(arr_s, arr_e, is, ie, 4) == 4) {
                for (int i = 0; i < 4; i++) resolve_number(tab, is[i], ie[i], 0, &mb[i]);
            }
        }
    }

    const uint8_t *ks, *ke;
    if (dict_get(dict_s, dict_e, "Kids", &ks, &ke)) {
        const uint8_t *arr_s, *arr_e;
        if (resolve_array_span(tab, ks, ke, 0, &arr_s, &arr_e)) {
            const uint8_t *is[PDF_MAX_ARR_ITEMS], *ie[PDF_MAX_ARR_ITEMS];
            int n = array_items(arr_s, arr_e, is, ie, PDF_MAX_ARR_ITEMS);
            for (int i = 0; i < n && doc->page_count < PDF_MAX_PAGES; i++) {
                const uint8_t *kd_s, *kd_e;
                if (resolve_dict_span(tab, is[i], ie[i], 0, &kd_s, &kd_e))
                    collect_pages(tab, kd_s, kd_e, mb, doc, depth + 1);
            }
        }
        return;
    }

    pdf_page_t *pg = &doc->pages[doc->page_count++];
    pg->mbx0 = mb[0]; pg->mby0 = mb[1]; pg->mbx1 = mb[2]; pg->mby1 = mb[3];
    pg->run_count = 0;

    const uint8_t *cs, *ce;
    if (dict_get(dict_s, dict_e, "Contents", &cs, &ce)) parse_page_contents(tab, cs, ce, pg);
}

int pdf_parse(const uint8_t *data, uint32_t size, pdf_doc_t *doc, char *err, int err_cap)
{
    memset(doc, 0, sizeof(*doc));

    pdf_objtab_t tab;
    tab.cap = PDF_MAX_OBJS;
    tab.count = 0;
    tab.arr = (pdf_obj_entry_t *)malloc(sizeof(pdf_obj_entry_t) * (uint32_t)tab.cap);
    if (!tab.arr) { set_err(err, err_cap, "Out of memory."); return -1; }

    scan_objects(data, size, &tab);
    if (tab.count == 0) { set_err(err, err_cap, "Not a valid PDF (no objects found)."); free(tab.arr); return -1; }

    const uint8_t *cat_is = NULL, *cat_ie = NULL;
    for (int i = 0; i < tab.count && !cat_is; i++) {
        const uint8_t *is, *ie;
        if (!find_dict_span(tab.arr[i].body_s, tab.arr[i].body_e, &is, &ie)) continue;
        const uint8_t *ts, *te;
        if (dict_get(is, ie, "Type", &ts, &te) && te - ts == 8 && memcmp(ts, "/Catalog", 8) == 0) { cat_is = is; cat_ie = ie; }
    }

    const uint8_t *pages_is = NULL, *pages_ie = NULL;
    if (cat_is) {
        const uint8_t *ps, *pe;
        if (dict_get(cat_is, cat_ie, "Pages", &ps, &pe)) resolve_dict_span(&tab, ps, pe, 0, &pages_is, &pages_ie);
    }
    if (!pages_is) {
        for (int i = 0; i < tab.count && !pages_is; i++) {
            const uint8_t *is, *ie;
            if (!find_dict_span(tab.arr[i].body_s, tab.arr[i].body_e, &is, &ie)) continue;
            const uint8_t *ts, *te;
            if (dict_get(is, ie, "Type", &ts, &te) && te - ts == 6 && memcmp(ts, "/Pages", 6) == 0) { pages_is = is; pages_ie = ie; }
        }
    }
    if (!pages_is) { set_err(err, err_cap, "Not a valid PDF (no page tree found)."); free(tab.arr); return -1; }

    double default_mb[4] = { 0, 0, 612, 792 };
    collect_pages(&tab, pages_is, pages_ie, default_mb, doc, 0);

    free(tab.arr);

    if (doc->page_count == 0) { set_err(err, err_cap, "PDF has no pages."); return -1; }
    err[0] = 0;
    return 0;
}

void pdf_free(pdf_doc_t *doc)
{
    (void)doc; /* fixed-size struct, nothing to release */
}
