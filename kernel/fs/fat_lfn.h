#ifndef FAT_LFN_H
#define FAT_LFN_H

/* VFAT long file name helpers shared by the FAT16 and FAT32 drivers. Pure
 * functions on 32-byte directory entries: no disk access in here. */

#include <stdint.h>
#include "string.h"

#define FATL_MAX 255

typedef struct {
    uint16_t name[FATL_MAX + 14];
    int next;        /* sequence number expected next (counting down to 1) */
    uint8_t csum;
    int active;
} fatl_state_t;

static inline void fatl_reset(fatl_state_t *l) { l->active = 0; }

static inline uint8_t fatl_checksum(const uint8_t *n11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + n11[i]);
    return sum;
}

/* Feed an LFN fragment (attr 0x0F, first byte not 0xE5), in directory order. */
static inline void fatl_feed(fatl_state_t *l, const uint8_t *e)
{
    uint8_t seq = e[0];
    int n = seq & 0x3F;
    if (n < 1 || n > 20) { l->active = 0; return; }
    if (seq & 0x40) {
        memset(l->name, 0xFF, sizeof(l->name));
        l->active = 1;
        l->csum = e[13];
        l->next = n;
    } else if (!l->active || n != l->next || e[13] != l->csum) {
        l->active = 0;
        return;
    }
    static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int i = 0; i < 13; i++)
        l->name[(n - 1) * 13 + i] = (uint16_t)(e[offs[i]] | (e[offs[i] + 1] << 8));
    l->next = n - 1;
}

/* After an LFN run, the short entry follows. Returns the long name length
 * (UTF-16 units) when the run is complete and matches the short entry,
 * else 0. */
static inline int fatl_complete(const fatl_state_t *l, const uint8_t *short_e, uint16_t *out, int cap)
{
    if (!l->active || l->next != 0) return 0;
    if (fatl_checksum(short_e) != l->csum) return 0;
    int len = 0;
    while (len < FATL_MAX && l->name[len] != 0x0000 && l->name[len] != 0xFFFF) len++;
    if (len == 0 || len > cap) return 0;
    memcpy(out, l->name, (size_t)len * 2);
    return len;
}

static inline int fatl_utf8_to_utf16(const char *s, int slen, uint16_t *out, int cap)
{
    int n = 0, i = 0;
    while (i < slen) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else { cp = '?'; extra = 0; }
        for (int k = 1; k <= extra; k++) {
            if (i + k >= slen || ((uint8_t)s[i + k] & 0xC0) != 0x80) { cp = '?'; extra = k - 1; break; }
            cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        }
        i += 1 + extra;
        if (n >= cap) return -1;
        out[n++] = (uint16_t)cp;
    }
    return n;
}

static inline int fatl_utf16_to_utf8(const uint16_t *in, int ilen, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < ilen; i++) {
        uint16_t c = in[i];
        if (c < 0x80) {
            if (n + 1 >= cap) break;
            out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 >= cap) break;
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (n + 3 >= cap) break;
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

/* Case folding for lookups: ASCII, Latin-1 letters and the Turkish dotted/
 * dotless I pair fold to lowercase; everything else compares exactly. */
static inline uint16_t fatl_fold(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);
    if (c == 0x130) return 'i';       /* İ */
    if (c == 0x131) return 'i';       /* ı */
    if (c >= 0x391 && c <= 0x3A9) return (uint16_t)(c + 32);   /* Greek capitals */
    if (c >= 0x410 && c <= 0x42F) return (uint16_t)(c + 32);   /* Cyrillic capitals */
    return c;
}

static inline int fatl_equal(const uint16_t *a, int al, const uint16_t *b, int bl)
{
    if (al != bl) return 0;
    for (int i = 0; i < al; i++)
        if (fatl_fold(a[i]) != fatl_fold(b[i])) return 0;
    return 1;
}

/* The 8.3 name of a short entry as UTF-16, honouring the NT lowercase bits. */
static inline int fatl_short_to_utf16(const uint8_t *e, uint16_t *out)
{
    int n = 0;
    int base_lower = (e[12] & 0x08) != 0, ext_lower = (e[12] & 0x10) != 0;
    int bl = 8;
    while (bl > 0 && e[bl - 1] == ' ') bl--;
    for (int i = 0; i < bl; i++) {
        uint8_t c = e[i];
        if (i == 0 && c == 0x05) c = 0xE5;
        if (base_lower && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        out[n++] = c;
    }
    int el = 3;
    while (el > 0 && e[8 + el - 1] == ' ') el--;
    if (el > 0) {
        out[n++] = '.';
        for (int i = 0; i < el; i++) {
            uint8_t c = e[8 + i];
            if (ext_lower && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
            out[n++] = c;
        }
    }
    return n;
}

static inline int fatl_short_char_ok(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '`': case '!': case '(': case ')': case '{': case '}': case '^': case '#': case '&':
        return 1;
    }
    return 0;
}

/* Can the name be stored as a plain 8.3 entry (with NT case bits) and no
 * long-name entries? On success fills out11 and the NT flag byte. */
static inline int fatl_fits_83(const uint16_t *n, int len, uint8_t *out11, uint8_t *ntflags)
{
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) if (n[i] == '.') { dot = i; break; }
    int bl = dot < 0 ? len : dot;
    int el = dot < 0 ? 0 : len - dot - 1;
    if (bl < 1 || bl > 8 || el > 3 || (dot >= 0 && el == 0)) return 0;
    int b_up = 0, b_lo = 0, e_up = 0, e_lo = 0;
    for (int i = 0; i < len; i++) {
        if (i == dot) continue;
        if (!fatl_short_char_ok(n[i])) return 0;
        int in_ext = dot >= 0 && i > dot;
        if (n[i] >= 'A' && n[i] <= 'Z') { if (in_ext) e_up = 1; else b_up = 1; }
        if (n[i] >= 'a' && n[i] <= 'z') { if (in_ext) e_lo = 1; else b_lo = 1; }
    }
    if ((b_up && b_lo) || (e_up && e_lo)) return 0;
    memset(out11, ' ', 11);
    for (int i = 0; i < bl; i++) { uint16_t c = n[i]; out11[i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c); }
    for (int i = 0; i < el; i++) { uint16_t c = n[dot + 1 + i]; out11[8 + i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c); }
    *ntflags = (uint8_t)((b_lo ? 0x08 : 0) | (e_lo ? 0x10 : 0));
    if (out11[0] == 0xE5) out11[0] = 0x05;
    return 1;
}

/* Generates a "BASE~N.EXT" alias for a name that needs long-name entries. */
static inline void fatl_make_alias(const uint16_t *n, int len, int tail, uint8_t *out11)
{
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) if (n[i] == '.') { dot = i; break; }
    uint8_t base[16], ext[4];
    int bn = 0, en = 0;
    int bend = dot < 0 ? len : dot;
    for (int i = 0; i < bend && bn < 8; i++) {
        uint16_t c = n[i];
        if (c == ' ' || c == '.') continue;
        if (!fatl_short_char_ok(c)) c = '_';
        if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
        base[bn++] = (uint8_t)c;
    }
    if (dot >= 0) {
        for (int i = dot + 1; i < len && en < 3; i++) {
            uint16_t c = n[i];
            if (c == ' ') continue;
            if (!fatl_short_char_ok(c)) c = '_';
            if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
            ext[en++] = (uint8_t)c;
        }
    }
    if (bn == 0) base[bn++] = '_';
    char num[12];
    int nl = 0;
    int t = tail;
    char rev[12];
    int rl = 0;
    do { rev[rl++] = (char)('0' + t % 10); t /= 10; } while (t > 0 && rl < 8);
    num[nl++] = '~';
    while (rl > 0) num[nl++] = rev[--rl];
    int keep = 8 - nl;
    if (bn > keep) bn = keep;
    memset(out11, ' ', 11);
    for (int i = 0; i < bn; i++) out11[i] = base[i];
    for (int i = 0; i < nl; i++) out11[bn + i] = (uint8_t)num[i];
    for (int i = 0; i < en; i++) out11[8 + i] = ext[i];
}

/* Builds the long-name entries (highest sequence first) for a name. */
static inline int fatl_build_entries(const uint16_t *n, int len, uint8_t csum, uint8_t out[][32])
{
    int count = (len + 12) / 13;
    static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = 0; k < count; k++) {
        int seq = count - k;
        uint8_t *e = out[k];
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = csum;
        for (int i = 0; i < 13; i++) {
            int idx = (seq - 1) * 13 + i;
            uint16_t c;
            if (idx < len) c = n[idx];
            else if (idx == len) c = 0x0000;
            else c = 0xFFFF;
            e[offs[i]] = (uint8_t)(c & 0xFF);
            e[offs[i] + 1] = (uint8_t)(c >> 8);
        }
    }
    return count;
}

#endif
