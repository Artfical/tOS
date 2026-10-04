/* NTFS driver: reads and writes real NTFS 1.2/3.x volumes (as created by
 * mkntfs / Windows): update sequence arrays, resident and non-resident data,
 * $INDEX_ALLOCATION B-trees with $UpCase collation, a growable $MFT, and the
 * volume dirty flag / $LogFile clean check that ntfs-3g also performs.
 *
 * Not supported: compressed/encrypted files (reported as errors), files
 * whose attributes spill into an $ATTRIBUTE_LIST (read-only errors),
 * named streams, and volumes whose journal is not clean (mounted read-only
 * so Windows can replay it). */

#include "ntfs.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "io.h"
#include "scheduler.h"

/* ---------- on-disk constants ---------- */

#define NTFS_REC_MFT      0
#define NTFS_REC_MFTMIRR  1
#define NTFS_REC_LOGFILE  2
#define NTFS_REC_VOLUME   3
#define NTFS_REC_ROOT     5
#define NTFS_REC_BITMAP   6
#define NTFS_REC_UPCASE   10
#define NTFS_FIRST_USER_RECORD 27

#define AT_STD_INFO   0x10
#define AT_ATTR_LIST  0x20
#define AT_FILE_NAME  0x30
#define AT_VOL_INFO   0x70
#define AT_DATA       0x80
#define AT_INDEX_ROOT 0x90
#define AT_INDEX_ALLOC 0xA0
#define AT_BITMAP     0xB0
#define AT_END        0xFFFFFFFFu

#define MFT_IN_USE 0x0001
#define MFT_IS_DIR 0x0002

#define IE_SUBNODE 0x01
#define IE_LAST    0x02

#define FN_POSIX 0
#define FN_WIN32 1
#define FN_DOS   2
#define FN_WIN32_DOS 3

#define FA_READONLY 0x00000001
#define FA_HIDDEN   0x00000002
#define FA_SYSTEM   0x00000004
#define FA_ARCHIVE  0x00000020
#define FA_I30_INDEX 0x10000000

#define ATTRF_COMPRESSED 0x0001
#define ATTRF_ENCRYPTED  0x4000
#define ATTRF_SPARSE     0x8000

#define VOLF_DIRTY 0x0001

#define NTFS_MAX_RUNS 1024
#define NTFS_MAX_DIR_ENTRIES 30000

/* ---------- little-endian field access (x86 is little-endian) ---------- */

static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void wr16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr64(void *p, uint64_t v) { memcpy(p, &v, 8); }

static const uint16_t I30_NAME[4] = { '$', 'I', '3', '0' };

/* ---------- wall clock ---------- */

static uint8_t cmos_rd(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

/* 100ns ticks since 1601-01-01, from the CMOS RTC (UTC assumed). */
static uint64_t ntfs_now(void)
{
    uint32_t guard = 0;
    while ((cmos_rd(0x0A) & 0x80) && guard++ < 100000) { }
    uint8_t sec = cmos_rd(0x00), min = cmos_rd(0x02), hour = cmos_rd(0x04);
    uint8_t day = cmos_rd(0x07), mon = cmos_rd(0x08), year = cmos_rd(0x09);
    uint8_t cent = cmos_rd(0x32);
    uint8_t regb = cmos_rd(0x0B);
    if (!(regb & 0x04)) {
        sec = (uint8_t)((sec & 0x0F) + (sec >> 4) * 10);
        min = (uint8_t)((min & 0x0F) + (min >> 4) * 10);
        hour = (uint8_t)(((hour & 0x0F) + ((hour & 0x70) >> 4) * 10) | (hour & 0x80));
        day = (uint8_t)((day & 0x0F) + (day >> 4) * 10);
        mon = (uint8_t)((mon & 0x0F) + (mon >> 4) * 10);
        year = (uint8_t)((year & 0x0F) + (year >> 4) * 10);
        cent = (uint8_t)((cent & 0x0F) + (cent >> 4) * 10);
    }
    if (!(regb & 0x02) && (hour & 0x80)) hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    uint32_t y = (cent ? (uint32_t)cent * 100 : 2000) + year;
    if (y < 1980 || mon < 1 || mon > 12 || day < 1) y = 2024, mon = 1, day = 1;
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint64_t days = 0;
    for (uint32_t yy = 1601; yy < y; yy++)
        days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    days += cum[mon - 1] + (day - 1);
    if (mon > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    uint64_t secs = days * 86400ULL + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
    return secs * 10000000ULL;
}

/* ---------- UTF-8 <-> UTF-16 (BMP only; other planes become '?') ---------- */

static int utf8_to_utf16(const char *s, int slen, uint16_t *out, int cap)
{
    int n = 0;
    int i = 0;
    while (i < slen) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = '?'; extra = 0; }
        for (int k = 1; k <= extra; k++) {
            if (i + k >= slen || ((uint8_t)s[i + k] & 0xC0) != 0x80) { cp = '?'; extra = k - 1; break; }
            cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        }
        i += 1 + extra;
        if (cp > 0xFFFF) cp = '?';
        if (n >= cap) return -1;
        out[n++] = (uint16_t)cp;
    }
    return n;
}

static int utf16_to_utf8(const uint16_t *in, int ilen, char *out, int cap)
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

/* ---------- raw device access ---------- */

static int dev_read(ntfs_t *fs, uint64_t off, uint32_t len, void *buf)
{
    return blockdev_read_bytes(fs->bd, off, len, buf);
}

static int dev_write(ntfs_t *fs, uint64_t off, uint32_t len, const void *buf)
{
    return blockdev_write_bytes(fs->bd, off, len, buf);
}

/* ---------- update sequence (fixup) arrays ---------- */

static int fixup_apply(uint8_t *rec, uint32_t size, const char *magic)
{
    if (memcmp(rec, magic, 4) != 0) return -1;
    uint16_t uo = rd16(rec + 4), uc = rd16(rec + 6);
    if (uc < 2 || (uint32_t)(uc - 1) * 512 != size || (uint32_t)uo + (uint32_t)uc * 2 > size) return -1;
    uint16_t usn = rd16(rec + uo);
    for (uint32_t i = 1; i < uc; i++) {
        uint8_t *p = rec + i * 512 - 2;
        if (rd16(p) != usn) return -1;
        memcpy(p, rec + uo + 2 * i, 2);
    }
    return 0;
}

/* Produces the on-disk form of a logical record in `out` (size bytes). */
static void fixup_protect(const uint8_t *rec, uint32_t size, uint8_t *out)
{
    memcpy(out, rec, size);
    uint16_t uo = rd16(out + 4), uc = rd16(out + 6);
    uint16_t usn = (uint16_t)(rd16(out + uo) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    wr16(out + uo, usn);
    for (uint32_t i = 1; i < uc; i++) {
        uint8_t *p = out + i * 512 - 2;
        memcpy(out + uo + 2 * i, p, 2);
        wr16(p, usn);
    }
}

/* ---------- mapping pairs (runlists) ---------- */

static int rl_decode(const uint8_t *p, const uint8_t *end, ntfs_run_t *runs, int max)
{
    int n = 0;
    uint64_t vcn = 0;
    int64_t lcn = 0;
    while (p < end && *p) {
        uint8_t h = *p++;
        int lb = h & 0x0F, ob = h >> 4;
        if (lb == 0 || lb > 8 || ob > 8 || p + lb + ob > end) return -1;
        uint64_t len = 0;
        for (int i = 0; i < lb; i++) len |= (uint64_t)p[i] << (8 * i);
        p += lb;
        if (n >= max) return -1;
        if (ob) {
            uint64_t off = 0;
            for (int i = 0; i < ob; i++) off |= (uint64_t)p[i] << (8 * i);
            if (ob < 8 && (p[ob - 1] & 0x80)) off |= ~0ULL << (8 * ob);
            p += ob;
            lcn += (int64_t)off;
            runs[n].lcn = lcn;
        } else {
            runs[n].lcn = -1;
        }
        runs[n].vcn = vcn;
        runs[n].len = len;
        vcn += len;
        n++;
    }
    return n;
}

static int nbytes_signed(int64_t v)
{
    for (int n = 1; n < 8; n++) {
        int64_t lim = (int64_t)1 << (8 * n - 1);
        if (v >= -lim && v < lim) return n;
    }
    return 8;
}

/* Encodes runs into mapping pairs plus the terminating zero byte. Returns
 * the byte count, or -1 if it does not fit in cap. */
static int rl_encode(const ntfs_run_t *runs, int n, uint8_t *out, int cap)
{
    int pos = 0;
    int64_t prev = 0;
    for (int i = 0; i < n; i++) {
        int lb = nbytes_signed((int64_t)runs[i].len);
        int ob = 0;
        int64_t delta = 0;
        if (runs[i].lcn >= 0) {
            delta = runs[i].lcn - prev;
            ob = nbytes_signed(delta);
        }
        if (pos + 1 + lb + ob + 1 > cap) return -1;
        out[pos++] = (uint8_t)((ob << 4) | lb);
        for (int k = 0; k < lb; k++) out[pos++] = (uint8_t)(runs[i].len >> (8 * k));
        for (int k = 0; k < ob; k++) out[pos++] = (uint8_t)((uint64_t)delta >> (8 * k));
        if (runs[i].lcn >= 0) prev = runs[i].lcn;
    }
    if (pos + 1 > cap) return -1;
    out[pos++] = 0;
    return pos;
}

/* Finds the run holding vcn. Returns 0 and fills lcn (-1 for a hole) and
 * how many clusters remain in the run from vcn onward. */
static int rl_map(const ntfs_run_t *runs, int n, uint64_t vcn, int64_t *lcn, uint64_t *remain)
{
    for (int i = 0; i < n; i++) {
        if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
            uint64_t d = vcn - runs[i].vcn;
            *lcn = runs[i].lcn < 0 ? -1 : runs[i].lcn + (int64_t)d;
            *remain = runs[i].len - d;
            return 0;
        }
    }
    return -1;
}

static uint64_t rl_total(const ntfs_run_t *runs, int n)
{
    return n ? runs[n - 1].vcn + runs[n - 1].len : 0;
}

/* Reads or writes a byte range of a runlist-described stream. */
static int run_io(ntfs_t *fs, const ntfs_run_t *runs, int n, uint64_t off, void *buf, uint32_t len, int write)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        uint64_t vcn = off / fs->cluster_size;
        uint32_t in = (uint32_t)(off % fs->cluster_size);
        int64_t lcn;
        uint64_t remain;
        if (rl_map(runs, n, vcn, &lcn, &remain) != 0) return -1;
        uint64_t avail = remain * fs->cluster_size - in;
        uint32_t chunk = (uint64_t)len < avail ? len : (uint32_t)avail;
        if (lcn < 0) {
            if (write) return -1;
            memset(p, 0, chunk);
        } else {
            uint64_t doff = (uint64_t)lcn * fs->cluster_size + in;
            if (write ? dev_write(fs, doff, chunk, p) : dev_read(fs, doff, chunk, p)) return -1;
        }
        p += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

/* ---------- MFT record I/O ---------- */

static int mft_read(ntfs_t *fs, uint32_t no, uint8_t *buf)
{
    if ((uint64_t)(no + 1) * fs->mft_record_size > fs->mft_data_size) return -1;
    if (run_io(fs, fs->mft_runs, fs->mft_nruns, (uint64_t)no * fs->mft_record_size, buf, fs->mft_record_size, 0)) return -1;
    return fixup_apply(buf, fs->mft_record_size, "FILE");
}

static int mft_write(ntfs_t *fs, uint32_t no, const uint8_t *buf)
{
    if (!fs->rw) return -1;
    if ((uint64_t)(no + 1) * fs->mft_record_size > fs->mft_data_size) return -1;
    uint8_t *tmp = (uint8_t *)malloc(fs->mft_record_size);
    if (!tmp) return -1;
    fixup_protect(buf, fs->mft_record_size, tmp);
    int rc = run_io(fs, fs->mft_runs, fs->mft_nruns, (uint64_t)no * fs->mft_record_size, tmp, fs->mft_record_size, 1);
    if (rc == 0 && no < fs->mirr_records && fs->mftmirr_lcn)
        rc = dev_write(fs, fs->mftmirr_lcn * fs->cluster_size + (uint64_t)no * fs->mft_record_size, fs->mft_record_size, tmp);
    free(tmp);
    return rc;
}

/* ---------- attributes inside a record ---------- */

static uint8_t *attr_first(uint8_t *rec) { return rec + rd16(rec + 20); }

static int attr_ok(const uint8_t *rec, uint32_t rs, const uint8_t *a)
{
    if (a < rec || a + 8 > rec + rs) return 0;
    if (rd32(a) == AT_END) return 0;
    uint32_t l = rd32(a + 4);
    if (l < 16 || (l & 7) || a + l > rec + rs) return 0;
    return 1;
}

static uint8_t *attr_next(uint8_t *a) { return a + rd32(a + 4); }

static uint8_t *attr_find(uint8_t *rec, uint32_t rs, uint32_t type, const uint16_t *name, int nlen)
{
    for (uint8_t *a = attr_first(rec); attr_ok(rec, rs, a); a = attr_next(a)) {
        uint32_t t = rd32(a);
        if (t > type) break;
        if (t != type) continue;
        if (a[9] != nlen) continue;
        if (nlen && memcmp(a + rd16(a + 10), name, (size_t)nlen * 2) != 0) continue;
        return a;
    }
    return 0;
}

static uint8_t *attr_find_nth(uint8_t *rec, uint32_t rs, uint32_t type, int n)
{
    for (uint8_t *a = attr_first(rec); attr_ok(rec, rs, a); a = attr_next(a)) {
        uint32_t t = rd32(a);
        if (t > type) break;
        if (t == type && n-- == 0) return a;
    }
    return 0;
}

static uint8_t *attr_res_value(uint8_t *a) { return a + rd16(a + 20); }
static uint32_t attr_res_len(const uint8_t *a) { return rd32(a + 16); }

static uint64_t attr_size(const uint8_t *a) { return a[8] ? rd64(a + 48) : rd32(a + 16); }
static uint64_t attr_init(const uint8_t *a) { return a[8] ? rd64(a + 56) : rd32(a + 16); }

static uint32_t rec_used(const uint8_t *rec) { return rd32(rec + 24); }
static uint32_t rec_cap(const uint8_t *rec) { return rd32(rec + 28); }

/* Resizes an attribute in place (value contents untouched), shifting every
 * attribute after it. New space is zeroed. Fails when the record is full. */
static int rec_attr_resize(uint8_t *rec, uint8_t *a, uint32_t new_len)
{
    new_len = (new_len + 7) & ~7u;
    uint32_t old = rd32(a + 4);
    uint32_t used = rec_used(rec);
    if (new_len > old && used + (new_len - old) > rec_cap(rec)) return -1;
    uint8_t *tail = a + old;
    uint32_t tail_len = (uint32_t)((rec + used) - tail);
    memmove(a + new_len, tail, tail_len);
    if (new_len > old) memset(a + old, 0, new_len - old);
    wr32(a + 4, new_len);
    wr32(rec + 24, used - old + new_len);
    return 0;
}

/* Inserts a zeroed attribute of `len` bytes keeping type order; returns it. */
static uint8_t *rec_attr_insert(uint8_t *rec, uint32_t rs, uint32_t type, uint32_t len)
{
    len = (len + 7) & ~7u;
    uint32_t used = rec_used(rec);
    if (used + len > rec_cap(rec)) return 0;
    uint8_t *pos = attr_first(rec);
    while (attr_ok(rec, rs, pos) && rd32(pos) <= type) pos = attr_next(pos);
    memmove(pos + len, pos, (size_t)((rec + used) - pos));
    memset(pos, 0, len);
    wr32(pos, type);
    wr32(pos + 4, len);
    uint16_t inst = rd16(rec + 40);
    wr16(pos + 14, inst);
    wr16(rec + 40, (uint16_t)(inst + 1));
    wr32(rec + 24, used + len);
    return pos;
}

static void rec_attr_remove(uint8_t *rec, uint8_t *a)
{
    uint32_t len = rd32(a + 4);
    uint32_t used = rec_used(rec);
    uint8_t *tail = a + len;
    memmove(a, tail, (size_t)((rec + used) - tail));
    wr32(rec + 24, used - len);
}

/* ---------- cluster bitmap ($Bitmap) ---------- */

static int bm_io(ntfs_t *fs, uint64_t byte_off, void *buf, uint32_t len, int write)
{
    return run_io(fs, fs->bm_runs, fs->bm_nruns, byte_off, buf, len, write);
}

static int bm_mark(ntfs_t *fs, uint64_t lcn, uint64_t count, int value)
{
    uint8_t *win = (uint8_t *)malloc(4096);
    if (!win) return -1;
    while (count > 0) {
        uint64_t b0 = lcn >> 3;
        if (b0 >= fs->bm_data_size) { free(win); return -1; }
        uint32_t wlen = 4096;
        if (b0 + wlen > fs->bm_data_size) wlen = (uint32_t)(fs->bm_data_size - b0);
        if (bm_io(fs, b0, win, wlen, 0)) { free(win); return -1; }
        uint64_t limit = (b0 + wlen) << 3;
        while (count > 0 && lcn < limit) {
            uint8_t m = (uint8_t)(1u << (lcn & 7));
            uint8_t *bp = &win[(lcn >> 3) - b0];
            if (value) *bp |= m; else *bp &= (uint8_t)~m;
            lcn++;
            count--;
        }
        if (bm_io(fs, b0, win, wlen, 1)) { free(win); return -1; }
    }
    free(win);
    return 0;
}

static void cl_free(ntfs_t *fs, uint64_t lcn, uint64_t count)
{
    if (count) bm_mark(fs, lcn, count, 0);
}

/* First-fit allocation of `count` clusters starting the search at hint.
 * Fills out[] with up to maxout runs (vcn left 0). Nothing is marked on
 * failure. */
static int cl_alloc(ntfs_t *fs, uint64_t count, int64_t hint, ntfs_run_t *out, int maxout, int *nout)
{
    uint8_t *win = (uint8_t *)malloc(4096);
    if (!win) return -1;
    uint64_t total = fs->total_clusters;
    uint64_t start0 = (hint > 0 && (uint64_t)hint < total) ? (uint64_t)hint : 0;
    uint64_t need = count;
    int n = 0;
    uint64_t cur_start = 0, cur_len = 0;
    uint64_t wb = ~0ULL, wlen = 0;
    int pass = 0;
    uint64_t c = start0;
    uint64_t stop = total;
    while (need > 0) {
        if (c >= stop) {
            if (cur_len) {
                if (n >= maxout) { free(win); return -1; }
                out[n].lcn = (int64_t)cur_start; out[n].len = cur_len; out[n].vcn = 0; n++;
                cur_len = 0;
            }
            if (pass == 0 && start0 > 0) { pass = 1; c = 0; stop = start0; continue; }
            free(win);
            return -1;
        }
        uint64_t bi = c >> 3;
        if (wb == ~0ULL || bi < wb || bi >= wb + wlen) {
            if (bi >= fs->bm_data_size) { c = stop; continue; }
            wb = bi & ~4095ULL;
            wlen = 4096;
            if (wb + wlen > fs->bm_data_size) wlen = fs->bm_data_size - wb;
            if (bm_io(fs, wb, win, (uint32_t)wlen, 0)) { free(win); return -1; }
        }
        uint8_t byte = win[bi - wb];
        if ((c & 7) == 0 && byte == 0xFF && cur_len == 0 && c + 8 <= stop) { c += 8; continue; }
        if ((byte >> (c & 7)) & 1) {
            if (cur_len) {
                if (n >= maxout) { free(win); return -1; }
                out[n].lcn = (int64_t)cur_start; out[n].len = cur_len; out[n].vcn = 0; n++;
                cur_len = 0;
            }
        } else {
            if (!cur_len) cur_start = c;
            cur_len++;
            need--;
        }
        c++;
    }
    if (cur_len) {
        if (n >= maxout) { free(win); return -1; }
        out[n].lcn = (int64_t)cur_start; out[n].len = cur_len; out[n].vcn = 0; n++;
    }
    free(win);
    for (int i = 0; i < n; i++) {
        if (bm_mark(fs, (uint64_t)out[i].lcn, out[i].len, 1) != 0) {
            for (int k = 0; k < i; k++) cl_free(fs, (uint64_t)out[k].lcn, out[k].len);
            return -1;
        }
    }
    fs->alloc_hint = (uint64_t)out[n - 1].lcn + out[n - 1].len;
    *nout = n;
    return 0;
}

/* ---------- generic attribute data (resident or non-resident) ---------- */

static int attr_runs(ntfs_t *fs, const uint8_t *a, ntfs_run_t **runs_out, int *n_out)
{
    (void)fs;
    if (rd64(a + 16) != 0) return -1; /* extension chunk of an attribute list: unsupported */
    ntfs_run_t *runs = (ntfs_run_t *)malloc(sizeof(ntfs_run_t) * NTFS_MAX_RUNS);
    if (!runs) return -1;
    const uint8_t *end = a + rd32(a + 4);
    int n = rl_decode(a + rd16(a + 32), end, runs, NTFS_MAX_RUNS);
    if (n < 0) { free(runs); return -1; }
    *runs_out = runs;
    *n_out = n;
    return 0;
}

static int attr_blocked(const uint8_t *a)
{
    uint16_t f = rd16(a + 12);
    return (f & (ATTRF_COMPRESSED | ATTRF_ENCRYPTED)) != 0;
}

/* Reads from an attribute honouring initialized_size (zeros past it). */
static int attr_read(ntfs_t *fs, const uint8_t *a, uint64_t off, void *buf, uint32_t len)
{
    uint64_t size = attr_size(a);
    if (off + len > size) return -1;
    if (!a[8]) {
        memcpy(buf, attr_res_value((uint8_t *)a) + off, len);
        return 0;
    }
    if (attr_blocked(a)) return -1;
    uint64_t init = attr_init(a);
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    if (off < init) {
        uint32_t n = (uint64_t)len < init - off ? len : (uint32_t)(init - off);
        ntfs_run_t *runs; int nr;
        if (attr_runs(fs, a, &runs, &nr)) return -1;
        int rc = run_io(fs, runs, nr, off, p, n, 0);
        free(runs);
        if (rc) return -1;
        done = n;
    }
    if (done < len) memset(p + done, 0, len - done);
    return 0;
}

static uint32_t name_bytes(const uint8_t *a) { return (uint32_t)a[9] * 2; }
static uint32_t nonres_mp_off(const uint8_t *a) { return rd16(a + 32); }

/* Rewrites the runlist of a non-resident attribute, resizing it in record. */
static int attr_set_runs(uint8_t *rec, uint8_t *a, const ntfs_run_t *runs, int n)
{
    uint8_t *tmp = (uint8_t *)malloc(NTFS_MAX_RUNS * 20 + 8);
    if (!tmp) return -1;
    int mp = rl_encode(runs, n, tmp, NTFS_MAX_RUNS * 20 + 8);
    if (mp < 0) { free(tmp); return -1; }
    uint32_t mpo = nonres_mp_off(a);
    if (rec_attr_resize(rec, a, mpo + (uint32_t)mp) != 0) { free(tmp); return -1; }
    memcpy(a + mpo, tmp, (size_t)mp);
    free(tmp);
    uint64_t total = rl_total(runs, n);
    wr64(a + 24, total ? total - 1 : (uint64_t)-1);
    return 0;
}

/* Makes sure at least `bytes` are allocated to a non-resident attribute. */
static int attr_grow_alloc(ntfs_t *fs, uint8_t *rec, uint8_t *a, uint64_t bytes)
{
    uint64_t have = rd64(a + 40);
    if (bytes <= have) return 0;
    ntfs_run_t *runs; int nr;
    if (attr_runs(fs, a, &runs, &nr)) return -1;
    uint64_t total = rl_total(runs, nr);
    uint64_t want = (bytes + fs->cluster_size - 1) / fs->cluster_size;
    if (want <= total) { free(runs); return 0; }
    int64_t hint = -1;
    for (int i = nr - 1; i >= 0; i--)
        if (runs[i].lcn >= 0) { hint = runs[i].lcn + (int64_t)runs[i].len; break; }
    ntfs_run_t nw[64];
    int nn = 0;
    if (cl_alloc(fs, want - total, hint, nw, 64, &nn)) { free(runs); return -1; }
    uint64_t vcn = total;
    for (int i = 0; i < nn; i++) {
        if (nr > 0 && runs[nr - 1].lcn >= 0 && vcn == rl_total(runs, nr) &&
            runs[nr - 1].lcn + (int64_t)runs[nr - 1].len == nw[i].lcn) {
            runs[nr - 1].len += nw[i].len;
        } else {
            if (nr >= NTFS_MAX_RUNS) goto fail;
            runs[nr].vcn = vcn; runs[nr].len = nw[i].len; runs[nr].lcn = nw[i].lcn; nr++;
        }
        vcn += nw[i].len;
    }
    if (attr_set_runs(rec, a, runs, nr) != 0) goto fail;
    wr64(a + 40, vcn * fs->cluster_size);
    free(runs);
    return 0;
fail:
    for (int i = 0; i < nn; i++) cl_free(fs, (uint64_t)nw[i].lcn, nw[i].len);
    free(runs);
    return -1;
}

/* Converts a resident attribute to non-resident, keeping its contents. */
static int attr_make_nonres(ntfs_t *fs, uint8_t *rec, uint8_t *a, uint64_t min_bytes)
{
    uint32_t vlen = attr_res_len(a);
    uint8_t *data = 0;
    if (vlen) {
        data = (uint8_t *)malloc(vlen);
        if (!data) return -1;
        memcpy(data, attr_res_value(a), vlen);
    }
    uint32_t nb = name_bytes(a);
    uint8_t namebuf[512];
    if (nb > sizeof(namebuf)) { free(data); return -1; }
    memcpy(namebuf, a + rd16(a + 10), nb);
    uint16_t flags = rd16(a + 12);
    uint64_t bytes = vlen > min_bytes ? vlen : min_bytes;
    uint64_t clusters = (bytes + fs->cluster_size - 1) / fs->cluster_size;
    ntfs_run_t runs[64];
    int nr = 0;
    if (clusters && cl_alloc(fs, clusters, (int64_t)fs->alloc_hint, runs, 64, &nr)) { free(data); return -1; }
    uint64_t vcn = 0;
    for (int i = 0; i < nr; i++) { runs[i].vcn = vcn; vcn += runs[i].len; }

    uint8_t mp[NTFS_MAX_RUNS];
    int mpl = rl_encode(runs, nr, mp, (int)sizeof(mp));
    if (mpl < 0) goto fail;
    uint32_t mpo = (0x40 + nb + 7) & ~7u;
    if (rec_attr_resize(rec, a, mpo + (uint32_t)mpl) != 0) goto fail;
    a[8] = 1;
    wr16(a + 12, flags);
    wr16(a + 10, 0x40);
    memcpy(a + 0x40, namebuf, nb);
    wr64(a + 16, 0);
    wr64(a + 24, clusters ? clusters - 1 : (uint64_t)-1);
    wr16(a + 32, (uint16_t)mpo);
    a[34] = 0;
    memset(a + 35, 0, 5);
    wr64(a + 40, clusters * fs->cluster_size);
    wr64(a + 48, vlen);
    wr64(a + 56, vlen);
    memcpy(a + mpo, mp, (size_t)mpl);

    if (vlen && nr) {
        uint8_t *zero = (uint8_t *)malloc(fs->cluster_size);
        if (!zero) { free(data); return -1; }
        memset(zero, 0, fs->cluster_size);
        run_io(fs, runs, nr, 0, data, vlen, 1);
        free(zero);
    }
    free(data);
    return 0;
fail:
    for (int i = 0; i < nr; i++) cl_free(fs, (uint64_t)runs[i].lcn, runs[i].len);
    free(data);
    return -1;
}

static int zero_range(ntfs_t *fs, const ntfs_run_t *runs, int nr, uint64_t off, uint64_t len)
{
    uint8_t *z = (uint8_t *)malloc(4096);
    if (!z) return -1;
    memset(z, 0, 4096);
    int rc = 0;
    while (len > 0 && rc == 0) {
        uint32_t n = len > 4096 ? 4096 : (uint32_t)len;
        rc = run_io(fs, runs, nr, off, z, n, 1);
        off += n;
        len -= n;
    }
    free(z);
    return rc;
}

/* Writes to an attribute, growing/converting it as needed. The caller must
 * write the record back afterwards. */
static int attr_write(ntfs_t *fs, uint8_t *rec, uint8_t *a, uint64_t off, const void *buf, uint32_t len)
{
    uint64_t end = off + len;
    if (!a[8]) {
        uint32_t vlen = attr_res_len(a);
        uint32_t voff = rd16(a + 20);
        if (end <= vlen) { memcpy(attr_res_value(a) + off, buf, len); return 0; }
        if (end < 0xF00 && rec_attr_resize(rec, a, voff + (uint32_t)end) == 0) {
            uint8_t *v = attr_res_value(a);
            memset(v + vlen, 0, (size_t)(end - vlen));
            wr32(a + 16, (uint32_t)end);
            memcpy(v + off, buf, len);
            return 0;
        }
        if (attr_make_nonres(fs, rec, a, end) != 0) return -1;
    }
    if (attr_blocked(a)) return -1;
    if (attr_grow_alloc(fs, rec, a, end) != 0) return -1;
    ntfs_run_t *runs; int nr;
    if (attr_runs(fs, a, &runs, &nr)) return -1;
    uint64_t init = rd64(a + 56);
    int rc = 0;
    if (off > init) rc = zero_range(fs, runs, nr, init, off - init);
    if (rc == 0 && len) rc = run_io(fs, runs, nr, off, (void *)buf, len, 1);
    free(runs);
    if (rc) return -1;
    if (end > init) wr64(a + 56, end);
    if (end > rd64(a + 48)) wr64(a + 48, end);
    return 0;
}

/* Truncates (or extends with zeros) an attribute to new_size bytes. */
static int attr_truncate(ntfs_t *fs, uint8_t *rec, uint8_t *a, uint64_t new_size)
{
    uint64_t cur = attr_size(a);
    if (new_size == cur) return 0;
    if (!a[8]) {
        uint32_t voff = rd16(a + 20);
        if (new_size < cur) {
            if (rec_attr_resize(rec, a, voff + (uint32_t)new_size) != 0) return -1;
            wr32(a + 16, (uint32_t)new_size);
            return 0;
        }
        uint8_t z = 0;
        return attr_write(fs, rec, a, new_size - 1, &z, 1);
    }
    if (attr_blocked(a)) return -1;
    if (new_size > cur) {
        if (attr_grow_alloc(fs, rec, a, new_size) != 0) return -1;
        wr64(a + 48, new_size);
        return 0;
    }
    ntfs_run_t *runs; int nr;
    if (attr_runs(fs, a, &runs, &nr)) return -1;
    uint64_t keep = (new_size + fs->cluster_size - 1) / fs->cluster_size;
    int out = 0;
    for (int i = 0; i < nr; i++) {
        uint64_t rs0 = runs[i].vcn, re = runs[i].vcn + runs[i].len;
        if (re <= keep) { runs[out++] = runs[i]; continue; }
        if (rs0 >= keep) {
            if (runs[i].lcn >= 0) cl_free(fs, (uint64_t)runs[i].lcn, runs[i].len);
            continue;
        }
        uint64_t k = keep - rs0;
        if (runs[i].lcn >= 0) cl_free(fs, (uint64_t)runs[i].lcn + k, runs[i].len - k);
        runs[i].len = k;
        runs[out++] = runs[i];
    }
    int rc = attr_set_runs(rec, a, runs, out);
    free(runs);
    if (rc) return -1;
    wr64(a + 40, keep * fs->cluster_size);
    wr64(a + 48, new_size);
    if (rd64(a + 56) > new_size) wr64(a + 56, new_size);
    return 0;
}

/* Frees every cluster of a non-resident attribute (used when deleting). */
static void attr_free_clusters(ntfs_t *fs, const uint8_t *a)
{
    if (!a[8]) return;
    ntfs_run_t *runs; int nr;
    if (attr_runs(fs, a, &runs, &nr)) return;
    for (int i = 0; i < nr; i++)
        if (runs[i].lcn >= 0) cl_free(fs, (uint64_t)runs[i].lcn, runs[i].len);
    free(runs);
}

/* ---------- MFT record allocation ---------- */

static void rec_format_empty(ntfs_t *fs, uint8_t *buf, uint32_t no, uint16_t seq)
{
    uint32_t rs = fs->mft_record_size;
    memset(buf, 0, rs);
    memcpy(buf, "FILE", 4);
    uint16_t uo = fs->ntfs_major >= 3 ? 0x30 : 0x2A;
    uint16_t uc = (uint16_t)(rs / 512 + 1);
    wr16(buf + 4, uo);
    wr16(buf + 6, uc);
    wr16(buf + 16, seq);
    uint16_t ao = (uint16_t)((uo + uc * 2 + 7) & ~7);
    wr16(buf + 20, ao);
    wr16(buf + 22, 0);
    wr32(buf + 24, ao + 8u);
    wr32(buf + 28, rs);
    if (fs->ntfs_major >= 3) wr32(buf + 44, no);
    wr32(buf + ao, AT_END);
}

static int mft_load_runs(ntfs_t *fs, const uint8_t *a)
{
    ntfs_run_t *runs; int nr;
    if (attr_runs(fs, a, &runs, &nr)) return -1;
    free(fs->mft_runs);
    fs->mft_runs = runs;
    fs->mft_nruns = nr;
    fs->mft_data_size = rd64(a + 48);
    return 0;
}

/* Grows $MFT by 16 empty records. */
static int mft_extend(ntfs_t *fs)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec0 = (uint8_t *)malloc(rs);
    uint8_t *tmp = (uint8_t *)malloc(rs);
    if (!rec0 || !tmp) { free(rec0); free(tmp); return -1; }
    int rc = -1;
    if (mft_read(fs, 0, rec0)) goto out;
    uint8_t *a = attr_find(rec0, rs, AT_DATA, 0, 0);
    if (!a || !a[8]) goto out;
    uint64_t old_size = attr_size(a);
    uint64_t new_size = old_size + 16ULL * rs;
    if (attr_grow_alloc(fs, rec0, a, new_size)) goto out;
    wr64(a + 48, new_size);
    wr64(a + 56, new_size);
    uint64_t save_size = fs->mft_data_size;
    ntfs_run_t *save_runs = fs->mft_runs;
    int save_n = fs->mft_nruns;
    fs->mft_runs = 0;
    if (mft_load_runs(fs, a)) { fs->mft_runs = save_runs; fs->mft_nruns = save_n; fs->mft_data_size = save_size; goto out; }
    free(save_runs);
    uint32_t first = (uint32_t)(old_size / rs);
    for (uint32_t no = first; no < (uint32_t)(new_size / rs); no++) {
        rec_format_empty(fs, tmp, no, 1);
        if (mft_write(fs, no, tmp)) goto out;
    }
    rc = mft_write(fs, 0, rec0);
out:
    free(rec0);
    free(tmp);
    return rc;
}

/* Reads the $MFT bitmap into a fresh buffer of at least `min_bytes` bytes. */
static uint8_t *mftbm_load(ntfs_t *fs, uint32_t *bytes_out, uint32_t min_bytes)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec0 = (uint8_t *)malloc(rs);
    if (!rec0) return 0;
    if (mft_read(fs, 0, rec0)) { free(rec0); return 0; }
    uint8_t *a = attr_find(rec0, rs, AT_BITMAP, 0, 0);
    if (!a) { free(rec0); return 0; }
    uint32_t have = (uint32_t)attr_size(a);
    uint32_t n = have > min_bytes ? have : min_bytes;
    n = (n + 7) & ~7u;
    uint8_t *bm = (uint8_t *)malloc(n);
    if (!bm) { free(rec0); return 0; }
    memset(bm, 0, n);
    if (have && attr_read(fs, a, 0, bm, have)) { free(bm); free(rec0); return 0; }
    free(rec0);
    *bytes_out = n;
    return bm;
}

static int mftbm_store(ntfs_t *fs, const uint8_t *bm, uint32_t bytes)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec0 = (uint8_t *)malloc(rs);
    if (!rec0) return -1;
    int rc = -1;
    if (mft_read(fs, 0, rec0)) goto out;
    uint8_t *a = attr_find(rec0, rs, AT_BITMAP, 0, 0);
    if (!a) goto out;
    if (attr_write(fs, rec0, a, 0, bm, bytes)) goto out;
    rc = mft_write(fs, 0, rec0);
out:
    free(rec0);
    return rc;
}

static int mft_alloc_record(ntfs_t *fs, uint32_t *out_no)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t nrec = (uint32_t)(fs->mft_data_size / fs->mft_record_size);
        uint32_t bytes;
        uint8_t *bm = mftbm_load(fs, &bytes, (nrec + 7) / 8);
        if (!bm) return -1;
        uint32_t start = fs->mft_hint > NTFS_FIRST_USER_RECORD ? fs->mft_hint : NTFS_FIRST_USER_RECORD;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t lo = pass == 0 ? start : NTFS_FIRST_USER_RECORD;
            uint32_t hi = pass == 0 ? nrec : start;
            for (uint32_t no = lo; no < hi && no < nrec; no++) {
                if (bm[no >> 3] & (1u << (no & 7))) continue;
                bm[no >> 3] |= (uint8_t)(1u << (no & 7));
                int rc = mftbm_store(fs, bm, bytes);
                free(bm);
                if (rc) return -1;
                fs->mft_hint = no + 1;
                *out_no = no;
                return 0;
            }
        }
        free(bm);
        if (attempt == 0 && mft_extend(fs)) return -1;
    }
    return -1;
}

static int mft_release_record(ntfs_t *fs, uint32_t no)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    if (mft_read(fs, no, rec) == 0) {
        uint16_t seq = (uint16_t)(rd16(rec + 16) + 1);
        if (seq == 0) seq = 1;
        rec_format_empty(fs, rec, no, seq);
        mft_write(fs, no, rec);
    }
    free(rec);
    uint32_t bytes;
    uint8_t *bm = mftbm_load(fs, &bytes, 0);
    if (!bm) return -1;
    if ((no >> 3) < bytes) bm[no >> 3] &= (uint8_t)~(1u << (no & 7));
    int rc = mftbm_store(fs, bm, bytes);
    free(bm);
    if (no < fs->mft_hint) fs->mft_hint = no;
    return rc;
}

/* ---------- names and collation ---------- */

static uint16_t up16(ntfs_t *fs, uint16_t c)
{
    return c < fs->upcase_len ? fs->upcase[c] : c;
}

static int collate(ntfs_t *fs, const uint16_t *a, int al, const uint16_t *b, int bl)
{
    int n = al < bl ? al : bl;
    for (int i = 0; i < n; i++) {
        uint16_t ua = up16(fs, a[i]), ub = up16(fs, b[i]);
        if (ua != ub) return ua < ub ? -1 : 1;
    }
    if (al != bl) return al < bl ? -1 : 1;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* Order used by Windows for lookups: case-insensitive only. */
static int collate_ic(ntfs_t *fs, const uint16_t *a, int al, const uint16_t *b, int bl)
{
    int n = al < bl ? al : bl;
    for (int i = 0; i < n; i++) {
        uint16_t ua = up16(fs, a[i]), ub = up16(fs, b[i]);
        if (ua != ub) return ua < ub ? -1 : 1;
    }
    if (al != bl) return al < bl ? -1 : 1;
    return 0;
}

/* $FILE_NAME value helpers (the same bytes are an index entry's key). */
static uint32_t fn_name_len(const uint8_t *fn) { return fn[64]; }

static int fn_collate_mode(ntfs_t *fs, const uint16_t *name, int nlen, const uint8_t *key, int icase)
{
    uint16_t tmp[NTFS_MAX_FILENAME];
    int kl = (int)fn_name_len(key);
    memcpy(tmp, key + 66, (size_t)kl * 2);
    return icase ? collate_ic(fs, name, nlen, tmp, kl) : collate(fs, name, nlen, tmp, kl);
}

static int fn_collate(ntfs_t *fs, const uint16_t *name, int nlen, const uint8_t *key)
{
    return fn_collate_mode(fs, name, nlen, key, 0);
}

/* ---------- directory index ($I30) ---------- */

typedef struct {
    ntfs_t *fs;
    uint32_t no;
    uint8_t *rec;
    uint32_t bs;       /* index block size */
    uint32_t unit;     /* bytes per VCN unit in $INDEX_ALLOCATION */
    int dirty_rec;
} ntfs_idx_t;

#define IDX_ROOT_HDR 16

static uint8_t *ix_root(ntfs_idx_t *ix) { return attr_find(ix->rec, ix->fs->mft_record_size, AT_INDEX_ROOT, I30_NAME, 4); }
static uint8_t *ix_alloc(ntfs_idx_t *ix) { return attr_find(ix->rec, ix->fs->mft_record_size, AT_INDEX_ALLOC, I30_NAME, 4); }
static uint8_t *ix_bmp(ntfs_idx_t *ix) { return attr_find(ix->rec, ix->fs->mft_record_size, AT_BITMAP, I30_NAME, 4); }

static int ix_open(ntfs_t *fs, uint32_t no, ntfs_idx_t *ix)
{
    memset(ix, 0, sizeof(*ix));
    ix->fs = fs;
    ix->no = no;
    ix->rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!ix->rec) return -1;
    if (mft_read(fs, no, ix->rec)) { free(ix->rec); ix->rec = 0; return -1; }
    if (!(rd16(ix->rec + 22) & MFT_IS_DIR)) { free(ix->rec); ix->rec = 0; return -1; }
    uint8_t *r = ix_root(ix);
    if (!r || r[8]) { free(ix->rec); ix->rec = 0; return -1; }
    uint8_t *v = attr_res_value(r);
    if (rd32(v) != AT_FILE_NAME) { free(ix->rec); ix->rec = 0; return -1; }
    ix->bs = rd32(v + 8);
    if (ix->bs < 512 || ix->bs > 65536 || (ix->bs & (ix->bs - 1))) { free(ix->rec); ix->rec = 0; return -1; }
    ix->unit = fs->cluster_size <= ix->bs ? fs->cluster_size : 512;
    return 0;
}

static void ix_close(ntfs_idx_t *ix)
{
    free(ix->rec);
    ix->rec = 0;
}

static int ix_read_block(ntfs_idx_t *ix, uint64_t vcn, uint8_t *buf)
{
    uint8_t *al = ix_alloc(ix);
    if (!al || !al[8]) return -1;
    uint64_t off = vcn * ix->unit;
    if (off + ix->bs > attr_size(al)) return -1;
    if (attr_read(ix->fs, al, off, buf, ix->bs)) return -1;
    if (fixup_apply(buf, ix->bs, "INDX")) return -1;
    if (rd64(buf + 16) != vcn) return -1;
    return 0;
}

static int ix_write_block(ntfs_idx_t *ix, uint64_t vcn, const uint8_t *buf)
{
    uint8_t *al = ix_alloc(ix);
    if (!al) return -1;
    uint8_t *tmp = (uint8_t *)malloc(ix->bs);
    if (!tmp) return -1;
    fixup_protect(buf, ix->bs, tmp);
    ntfs_run_t *runs; int nr;
    int rc = -1;
    if (attr_runs(ix->fs, al, &runs, &nr) == 0) {
        rc = run_io(ix->fs, runs, nr, vcn * ix->unit, tmp, ix->bs, 1);
        free(runs);
    }
    free(tmp);
    return rc;
}

/* Walks a node's entries. `base` points at the INDEX_HEADER. */
static uint8_t *node_entries(uint8_t *hdr) { return hdr + rd32(hdr); }
static uint8_t *node_end(uint8_t *hdr) { return hdr + rd32(hdr + 4); }

static int entry_ok(const uint8_t *e, const uint8_t *end)
{
    if (e + 16 > end) return 0;
    uint16_t len = rd16(e + 8);
    if (len < 16 || (len & 7) || e + len > end) return 0;
    return 1;
}

typedef struct {
    int in_root;
    uint64_t vcn;
    uint32_t off;        /* entry offset inside the node buffer */
} ix_loc_t;

/* Finds a name in the index. On success fills the file reference and the
 * location of the entry. */
static int ix_lookup_mode(ntfs_idx_t *ix, const uint16_t *name, int nlen, uint64_t *mref, ix_loc_t *loc, int icase)
{
    ntfs_t *fs = ix->fs;
    uint8_t *root = ix_root(ix);
    uint8_t *rv = attr_res_value(root);
    uint8_t *hdr = rv + IDX_ROOT_HDR;
    uint8_t *buf = 0;
    uint8_t *node_base = rv;
    int in_root = 1;
    uint64_t vcn = 0;
    for (int depth = 0; depth < 20; depth++) {
        uint8_t *e = node_entries(hdr);
        uint8_t *end = node_end(hdr);
        uint64_t child = 0;
        int descend = 0;
        for (;;) {
            if (!entry_ok(e, end)) { free(buf); return -1; }
            uint16_t flags = rd16(e + 12);
            uint16_t len = rd16(e + 8);
            if (flags & IE_LAST) {
                if (flags & IE_SUBNODE) { child = rd64(e + len - 8); descend = 1; }
                break;
            }
            int c = fn_collate_mode(fs, name, nlen, e + 16, icase);
            if (c == 0) {
                *mref = rd64(e);
                if (loc) { loc->in_root = in_root; loc->vcn = vcn; loc->off = (uint32_t)(e - node_base); }
                free(buf);
                return 0;
            }
            if (c < 0) {
                if (flags & IE_SUBNODE) { child = rd64(e + len - 8); descend = 1; }
                break;
            }
            e += len;
        }
        if (!descend) { free(buf); return -1; }
        if (!buf) { buf = (uint8_t *)malloc(ix->bs); if (!buf) return -1; }
        if (ix_read_block(ix, child, buf)) { free(buf); return -1; }
        in_root = 0;
        vcn = child;
        node_base = buf;
        hdr = buf + 0x18;
    }
    free(buf);
    return -1;
}

/* Exact match first (names that differ only in case can coexist), then the
 * case-insensitive match Windows semantics require. */
static int ix_lookup(ntfs_idx_t *ix, const uint16_t *name, int nlen, uint64_t *mref, ix_loc_t *loc)
{
    if (ix_lookup_mode(ix, name, nlen, mref, loc, 0) == 0) return 0;
    return ix_lookup_mode(ix, name, nlen, mref, loc, 1);
}

typedef struct {
    uint8_t *e;       /* entry bytes, leaf form (no subnode), length multiple of 8 */
    uint32_t len;
    uint64_t sub;     /* subnode VCN for internal-level items, ~0 when none */
} item_t;

#define NO_SUB (~0ULL)

typedef struct {
    item_t *items;
    uint32_t n, cap;
    uint8_t *arena;
    uint32_t arena_len, arena_cap;
} ent_list_t;

static void el_free(ent_list_t *l)
{
    free(l->items);
    free(l->arena);
    memset(l, 0, sizeof(*l));
}

static int el_add(ent_list_t *l, const uint8_t *e, uint32_t len)
{
    if (l->n >= NTFS_MAX_DIR_ENTRIES) return -1;
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 64;
        item_t *ni = (item_t *)malloc(nc * sizeof(item_t));
        if (!ni) return -1;
        if (l->n) memcpy(ni, l->items, l->n * sizeof(item_t));
        free(l->items);
        l->items = ni;
        l->cap = nc;
    }
    if (l->arena_len + len > l->arena_cap) {
        uint32_t nc = l->arena_cap ? l->arena_cap * 2 : 8192;
        while (nc < l->arena_len + len) nc *= 2;
        uint8_t *na = (uint8_t *)malloc(nc);
        if (!na) return -1;
        /* items hold offsets until the list is finished, so a move is safe */
        if (l->arena_len) memcpy(na, l->arena, l->arena_len);
        free(l->arena);
        l->arena = na;
        l->arena_cap = nc;
    }
    memcpy(l->arena + l->arena_len, e, len);
    l->items[l->n].e = (uint8_t *)(uintptr_t)l->arena_len;   /* offset for now */
    l->items[l->n].len = len;
    l->items[l->n].sub = NO_SUB;
    l->arena_len += len;
    l->n++;
    return 0;
}

static void el_finish(ent_list_t *l)
{
    for (uint32_t i = 0; i < l->n; i++)
        l->items[i].e = l->arena + (uintptr_t)l->items[i].e;
}

/* In-order flatten of the whole tree (internal entries are real entries). */
static int ix_collect_node(ntfs_idx_t *ix, uint8_t *hdr, ent_list_t *l, int depth)
{
    if (depth > 20) return -1;
    uint8_t *e = node_entries(hdr);
    uint8_t *end = node_end(hdr);
    for (;;) {
        if (!entry_ok(e, end)) return -1;
        uint16_t flags = rd16(e + 12);
        uint16_t len = rd16(e + 8);
        if (flags & IE_SUBNODE) {
            uint8_t *blk = (uint8_t *)malloc(ix->bs);
            if (!blk) return -1;
            int rc = ix_read_block(ix, rd64(e + len - 8), blk);
            if (rc == 0) rc = ix_collect_node(ix, blk + 0x18, l, depth + 1);
            free(blk);
            if (rc) return -1;
        }
        if (flags & IE_LAST) break;
        uint32_t elen = len - ((flags & IE_SUBNODE) ? 8 : 0);
        uint8_t tmp[16 + 66 + 2 * NTFS_MAX_FILENAME + 8];
        if (elen > sizeof(tmp)) return -1;
        memcpy(tmp, e, elen);
        wr16(tmp + 8, (uint16_t)elen);
        wr16(tmp + 12, (uint16_t)(flags & ~IE_SUBNODE));
        if (el_add(l, tmp, elen)) return -1;
        e += len;
    }
    return 0;
}

static int ix_collect(ntfs_idx_t *ix, ent_list_t *l)
{
    memset(l, 0, sizeof(*l));
    uint8_t *root = ix_root(ix);
    uint8_t *rv = attr_res_value(root);
    if (ix_collect_node(ix, rv + IDX_ROOT_HDR, l, 0)) { el_free(l); return -1; }
    el_finish(l);
    return 0;
}


/* ---------- rebuilding the index tree from a sorted entry list ---------- */

typedef struct {
    uint8_t *buf;
    uint64_t vcn;
} blk_t;

typedef struct {
    ntfs_idx_t *ix;
    blk_t *blocks;
    uint32_t nblocks, cap;
    uint8_t *pool_used;     /* per existing block: was in use before */
    uint32_t pool_n;        /* number of blocks currently allocated in $INDEX_ALLOCATION */
    uint32_t pool_next;     /* next pool block to hand out */
    uint32_t entries_abs;   /* offset of the first entry inside a block */
    uint32_t max_blocks;
} build_t;

static uint32_t blk_entries_abs(uint32_t bs)
{
    uint32_t uc = bs / 512 + 1;
    return (0x28 + uc * 2 + 7) & ~7u;
}

/* Hands out the next free index block (extending $INDEX_ALLOCATION when
 * the existing blocks are used up). Returns its VCN or ~0. */
static uint64_t build_new_vcn(build_t *b)
{
    ntfs_idx_t *ix = b->ix;
    ntfs_t *fs = ix->fs;
    uint32_t idx = b->pool_next++;
    if (idx >= b->pool_n) {
        uint8_t *al = ix_alloc(ix);
        if (!al) return NO_SUB;
        uint64_t want = ((uint64_t)idx + 1) * ix->bs;
        if (attr_grow_alloc(fs, ix->rec, al, want)) return NO_SUB;
        al = ix_alloc(ix);
        wr64(al + 48, want);
        wr64(al + 56, want);
        b->pool_n = idx + 1;
    }
    return (uint64_t)idx * ix->bs / ix->unit;
}

static int build_add_block(build_t *b, uint8_t *buf, uint64_t vcn)
{
    if (b->nblocks == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 16;
        blk_t *nb = (blk_t *)malloc(nc * sizeof(blk_t));
        if (!nb) return -1;
        if (b->nblocks) memcpy(nb, b->blocks, b->nblocks * sizeof(blk_t));
        free(b->blocks);
        b->blocks = nb;
        b->cap = nc;
    }
    b->blocks[b->nblocks].buf = buf;
    b->blocks[b->nblocks].vcn = vcn;
    b->nblocks++;
    return 0;
}

/* Writes entries [i0,i1) plus a LAST entry (pointing to last_sub if not
 * NO_SUB) at p. Returns bytes written. */
static uint32_t emit_entries(uint8_t *p, const item_t *it, uint32_t i0, uint32_t i1, uint64_t last_sub)
{
    uint8_t *start = p;
    for (uint32_t i = i0; i < i1; i++) {
        memcpy(p, it[i].e, it[i].len);
        uint16_t flags = rd16(p + 12);
        uint16_t len = (uint16_t)it[i].len;
        if (it[i].sub != NO_SUB) {
            flags |= IE_SUBNODE;
            len = (uint16_t)(len + 8);
            wr64(p + len - 8, it[i].sub);
        }
        wr16(p + 8, len);
        wr16(p + 12, flags);
        p += len;
    }
    memset(p, 0, 16);
    uint16_t llen = 16;
    uint16_t lflags = IE_LAST;
    if (last_sub != NO_SUB) { llen = 24; lflags |= IE_SUBNODE; wr64(p + 16, last_sub); }
    wr16(p + 8, llen);
    wr16(p + 12, lflags);
    p += llen;
    return (uint32_t)(p - start);
}

static uint32_t item_wire_len(const item_t *it) { return it->len + (it->sub != NO_SUB ? 8 : 0); }

/* Builds one INDX block holding items [i0,i1) and the LAST entry. */
static int build_node_block(build_t *b, const item_t *it, uint32_t i0, uint32_t i1, uint64_t last_sub, uint64_t *vcn_out)
{
    ntfs_idx_t *ix = b->ix;
    uint64_t vcn = build_new_vcn(b);
    if (vcn == NO_SUB) return -1;
    uint8_t *buf = (uint8_t *)malloc(ix->bs);
    if (!buf) return -1;
    memset(buf, 0, ix->bs);
    memcpy(buf, "INDX", 4);
    uint32_t uc = ix->bs / 512 + 1;
    wr16(buf + 4, 0x28);
    wr16(buf + 6, (uint16_t)uc);
    wr64(buf + 16, vcn);
    uint8_t *hdr = buf + 0x18;
    uint32_t eo = b->entries_abs - 0x18;
    uint32_t used = emit_entries(buf + b->entries_abs, it, i0, i1, last_sub);
    wr32(hdr, eo);
    wr32(hdr + 4, eo + used);
    wr32(hdr + 8, ix->bs - 0x18);
    hdr[12] = (last_sub != NO_SUB) ? 1 : 0;
    if (build_add_block(b, buf, vcn)) { free(buf); return -1; }
    *vcn_out = vcn;
    return 0;
}

static void build_free(build_t *b)
{
    for (uint32_t i = 0; i < b->nblocks; i++) free(b->blocks[i].buf);
    free(b->blocks);
    free(b->pool_used);
    b->blocks = 0;
    b->pool_used = 0;
}

/* Packs `items` (all at one level) into nodes; separators go to `up`.
 * leaf=1 means the nodes carry no subnode pointers. final_sub is the
 * pointer the last node's LAST entry gets (internal levels only). */
static int build_level(build_t *b, item_t *items, uint32_t n, int leaf, uint64_t final_sub, item_t *up, uint32_t *nup, uint64_t *last_node_vcn)
{
    uint32_t cap = b->ix->bs - b->entries_abs;
    uint32_t i0 = 0, used = 0;
    *nup = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t need = item_wire_len(&items[i]);
        uint32_t last_len = leaf ? 16 : 24;
        if (used + need + last_len > cap) {
            /* close [i0, i): items[i] becomes the separator */
            if (need + last_len > cap) return -1;
            uint64_t vcn;
            uint64_t lsub = leaf ? NO_SUB : items[i].sub;
            if (build_node_block(b, items, i0, i, lsub, &vcn)) return -1;
            up[*nup] = items[i];
            up[*nup].sub = vcn;
            (*nup)++;
            i0 = i + 1;
            used = 0;
            continue;
        }
        used += need;
    }
    uint64_t vcn;
    if (build_node_block(b, items, i0, n, leaf ? NO_SUB : final_sub, &vcn)) return -1;
    *last_node_vcn = vcn;
    return 0;
}

/* Replaces the INDEX_ROOT attribute value with the given entries. */
static int ix_set_root(ntfs_idx_t *ix, const item_t *it, uint32_t n, uint64_t last_sub, int large)
{
    uint32_t wire = 0;
    for (uint32_t i = 0; i < n; i++) wire += item_wire_len(&it[i]);
    wire += (last_sub != NO_SUB) ? 24 : 16;
    uint8_t *r = ix_root(ix);
    uint32_t voff = rd16(r + 20);
    uint8_t *old = (uint8_t *)malloc(IDX_ROOT_HDR);
    if (!old) return -1;
    memcpy(old, attr_res_value(r), IDX_ROOT_HDR);
    if (rec_attr_resize(ix->rec, r, voff + IDX_ROOT_HDR + 16 + wire)) { free(old); return -1; }
    uint8_t *v = attr_res_value(r);
    memcpy(v, old, IDX_ROOT_HDR);
    free(old);
    wr32(r + 16, IDX_ROOT_HDR + 16 + wire);
    uint8_t *hdr = v + IDX_ROOT_HDR;
    memset(hdr, 0, 16);
    wr32(hdr, 16);
    wr32(hdr + 4, 16 + wire);
    wr32(hdr + 8, 16 + wire);
    hdr[12] = large ? 1 : 0;
    emit_entries(hdr + 16, it, 0, n, last_sub);
    return 0;
}

/* Rewrites the whole directory index from a sorted list of entries. */
static int ix_rebuild(ntfs_idx_t *ix, ent_list_t *l)
{
    ntfs_t *fs = ix->fs;
    build_t b;
    memset(&b, 0, sizeof(b));
    b.ix = ix;
    b.entries_abs = blk_entries_abs(ix->bs);

    uint8_t *al = ix_alloc(ix);
    uint8_t *bm = ix_bmp(ix);
    b.pool_n = al ? (uint32_t)(attr_size(al) / ix->bs) : 0;

    /* how much of the MFT record is available for root entries */
    uint8_t *root = ix_root(ix);
    uint32_t root_now = rd32(root + 4) - rd16(root + 20);
    uint32_t free_rec = rec_cap(ix->rec) - rec_used(ix->rec);
    int32_t rb = (int32_t)(free_rec + root_now) - IDX_ROOT_HDR - 16 - 64;
    uint32_t root_budget = rb < 64 ? 64 : (uint32_t)rb;

    item_t *items = l->items;
    uint32_t n = l->n;
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += items[i].len;

    int rc = -1;
    uint8_t *saved_rec = (uint8_t *)malloc(fs->mft_record_size);
    if (!saved_rec) return -1;
    memcpy(saved_rec, ix->rec, fs->mft_record_size);

    if (total + 16 <= root_budget) {
        if (ix_set_root(ix, items, n, NO_SUB, 0)) goto fail;
        /* everything fits in the root: drop the allocation and its bitmap */
        al = ix_alloc(ix);
        if (al) { attr_free_clusters(fs, al); rec_attr_remove(ix->rec, al); }
        bm = ix_bmp(ix);
        if (bm) { attr_free_clusters(fs, bm); rec_attr_remove(ix->rec, bm); }
        rc = mft_write(fs, ix->no, ix->rec);
        goto done;
    }

    /* the old root still occupies record space: empty it before adding attributes */
    if (ix_set_root(ix, 0, 0, NO_SUB, 0)) goto fail;
    al = ix_alloc(ix);
    bm = ix_bmp(ix);
    {
        uint8_t *r2 = ix_root(ix);
        uint32_t now2 = rd32(r2 + 4) - rd16(r2 + 20);
        uint32_t free2 = rec_cap(ix->rec) - rec_used(ix->rec);
        int32_t rb2 = (int32_t)(free2 + now2) - IDX_ROOT_HDR - 16 - 64;
        root_budget = rb2 < 64 ? 64 : (uint32_t)rb2;
    }

    /* make sure the allocation + bitmap attributes exist */
    if (!al) {
        uint32_t nb = 8;
        al = rec_attr_insert(ix->rec, fs->mft_record_size, AT_INDEX_ALLOC, 0x40 + 8 + 8);
        if (!al) goto fail;
        al[8] = 1;
        al[9] = 4;
        wr16(al + 10, 0x40);
        memcpy(al + 0x40, I30_NAME, 8);
        wr16(al + 32, 0x48);
        wr64(al + 24, (uint64_t)-1);
        al[0x48] = 0;
        (void)nb;
        bm = ix_bmp(ix);
        if (!bm) {
            bm = rec_attr_insert(ix->rec, fs->mft_record_size, AT_BITMAP, 0x18 + 8 + 8);
            if (!bm) goto fail;
            bm[9] = 4;
            wr16(bm + 10, 0x18);
            wr32(bm + 16, 8);
            wr16(bm + 20, 0x20);
            memcpy(bm + 0x18, I30_NAME, 8);
        }
        al = ix_alloc(ix);
    }
    {
        uint8_t *r3 = ix_root(ix);
        uint32_t now3 = rd32(r3 + 4) - rd16(r3 + 20);
        uint32_t free3 = rec_cap(ix->rec) - rec_used(ix->rec);
        int32_t rb3 = (int32_t)(free3 + now3) - IDX_ROOT_HDR - 16 - 64;
        root_budget = rb3 < 64 ? 64 : (uint32_t)rb3;
    }

    item_t *lvl = items;
    uint32_t ln = n;
    int leaf = 1;
    uint64_t final_sub = NO_SUB;
    item_t *owned = 0;
    for (int level = 0; level < 12; level++) {
        uint32_t tot = 0;
        for (uint32_t i = 0; i < ln; i++) tot += item_wire_len(&lvl[i]);
        if (!leaf && tot + 24 <= root_budget) {
            if (ix_set_root(ix, lvl, ln, final_sub, 1)) { free(owned); goto fail; }
            free(owned);
            goto blocks_done;
        }
        item_t *up = (item_t *)malloc((ln + 1) * sizeof(item_t));
        if (!up) { free(owned); goto fail; }
        uint32_t nup;
        uint64_t last_vcn;
        if (build_level(&b, lvl, ln, leaf, final_sub, up, &nup, &last_vcn)) { free(up); free(owned); goto fail; }
        free(owned);
        owned = up;
        lvl = up;
        ln = nup;
        final_sub = last_vcn;
        leaf = 0;
    }
    free(owned);
    goto fail;

blocks_done:
    {
        /* write blocks, then fix the bitmap, then the record */
        al = ix_alloc(ix);
        uint32_t nblk = (uint32_t)(attr_size(al) / ix->bs);
        uint32_t bbytes = ((nblk + 63) / 64) * 8;
        uint8_t *bits = (uint8_t *)malloc(bbytes);
        if (!bits) goto fail;
        memset(bits, 0, bbytes);
        for (uint32_t i = 0; i < b.nblocks; i++) {
            uint32_t bi = (uint32_t)(b.blocks[i].vcn * ix->unit / ix->bs);
            bits[bi >> 3] |= (uint8_t)(1u << (bi & 7));
        }
        for (uint32_t i = 0; i < b.nblocks; i++)
            if (ix_write_block(ix, b.blocks[i].vcn, b.blocks[i].buf)) { free(bits); goto fail; }
        bm = ix_bmp(ix);
        if (attr_write(fs, ix->rec, bm, 0, bits, bbytes)) { free(bits); goto fail; }
        free(bits);
        rc = mft_write(fs, ix->no, ix->rec);
    }
done:
    free(saved_rec);
    build_free(&b);
    return rc;
fail:
    memcpy(ix->rec, saved_rec, fs->mft_record_size);
    free(saved_rec);
    build_free(&b);
    return -1;
}

/* ---------- entry list editing ---------- */

static int el_insert(ent_list_t *l, uint32_t pos, const uint8_t *e, uint32_t len)
{
    if (l->n >= NTFS_MAX_DIR_ENTRIES) return -1;
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 64;
        item_t *ni = (item_t *)malloc(nc * sizeof(item_t));
        if (!ni) return -1;
        if (l->n) memcpy(ni, l->items, l->n * sizeof(item_t));
        free(l->items);
        l->items = ni;
        l->cap = nc;
    }
    if (l->arena_len + len > l->arena_cap) {
        uint32_t nc = l->arena_cap * 2;
        if (nc < l->arena_len + len + 4096) nc = l->arena_len + len + 4096;
        uint8_t *na = (uint8_t *)malloc(nc);
        if (!na) return -1;
        if (l->arena_len) memcpy(na, l->arena, l->arena_len);
        for (uint32_t i = 0; i < l->n; i++) l->items[i].e = na + (l->items[i].e - l->arena);
        free(l->arena);
        l->arena = na;
        l->arena_cap = nc;
    }
    memcpy(l->arena + l->arena_len, e, len);
    memmove(&l->items[pos + 1], &l->items[pos], (l->n - pos) * sizeof(item_t));
    l->items[pos].e = l->arena + l->arena_len;
    l->items[pos].len = len;
    l->items[pos].sub = NO_SUB;
    l->arena_len += len;
    l->n++;
    return 0;
}

static void el_remove(ent_list_t *l, uint32_t pos)
{
    memmove(&l->items[pos], &l->items[pos + 1], (l->n - pos - 1) * sizeof(item_t));
    l->n--;
}

/* Index of the first item greater than name; sets *found if one is equal. */
static uint32_t el_find(ntfs_t *fs, ent_list_t *l, const uint16_t *name, int nlen, int *found)
{
    *found = 0;
    uint32_t lo = 0, hi = l->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = fn_collate(fs, name, nlen, l->items[mid].e + 16);
        if (c == 0) { *found = 1; return mid; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    /* no exact match: entries equal ignoring case are adjacent to lo */
    for (int64_t k = (int64_t)lo - 1; k >= 0; k--) {
        if (fn_collate_mode(fs, name, nlen, l->items[k].e + 16, 1) != 0) break;
        *found = 1;
        return (uint32_t)k;
    }
    for (uint32_t k = lo; k < l->n; k++) {
        if (fn_collate_mode(fs, name, nlen, l->items[k].e + 16, 1) != 0) break;
        *found = 1;
        return k;
    }
    return lo;
}

/* ---------- $FILE_NAME and new records ---------- */

static uint32_t fn_build(uint8_t *out, uint64_t parent_ref, const uint16_t *name, int nlen, uint32_t fattr,
                         uint8_t ntype, uint64_t t, uint64_t alloc, uint64_t size)
{
    uint32_t len = 66 + 2 * (uint32_t)nlen;
    memset(out, 0, len);
    wr64(out, parent_ref);
    wr64(out + 8, t); wr64(out + 16, t); wr64(out + 24, t); wr64(out + 32, t);
    wr64(out + 40, alloc);
    wr64(out + 48, size);
    wr32(out + 56, fattr);
    out[64] = (uint8_t)nlen;
    out[65] = ntype;
    memcpy(out + 66, name, (size_t)nlen * 2);
    return len;
}

static uint32_t entry_build(uint8_t *out, uint64_t mref, const uint8_t *key, uint32_t klen)
{
    uint32_t len = (16 + klen + 7) & ~7u;
    memset(out, 0, len);
    wr64(out, mref);
    wr16(out + 8, (uint16_t)len);
    wr16(out + 10, (uint16_t)klen);
    wr16(out + 12, 0);
    memcpy(out + 16, key, klen);
    return len;
}

static int valid_name(const uint16_t *n, int len)
{
    if (len <= 0 || len > NTFS_MAX_FILENAME) return 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = n[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    if (n[len - 1] == '.' || n[len - 1] == ' ') return 0;
    if (len == 1 && n[0] == '.') return 0;
    if (len == 2 && n[0] == '.' && n[1] == '.') return 0;
    return 1;
}

static uint64_t mref_make(uint32_t no, uint16_t seq) { return (uint64_t)no | ((uint64_t)seq << 48); }

static int rec_has_attrlist(uint8_t *rec, uint32_t rs) { return attr_find(rec, rs, AT_ATTR_LIST, 0, 0) != 0; }

static int rec_build_new(ntfs_t *fs, uint8_t *rec, uint32_t no, uint16_t seq, int is_dir, uint64_t parent_ref,
                         const uint16_t *name, int nlen, uint64_t now, uint32_t secid)
{
    uint32_t rs = fs->mft_record_size;
    rec_format_empty(fs, rec, no, seq);
    wr16(rec + 18, 1);
    wr16(rec + 22, (uint16_t)(MFT_IN_USE | (is_dir ? MFT_IS_DIR : 0)));

    uint32_t silen = fs->ntfs_major >= 3 ? 72 : 48;
    uint8_t *a = rec_attr_insert(rec, rs, AT_STD_INFO, 0x18 + silen);
    if (!a) return -1;
    wr32(a + 16, silen);
    wr16(a + 20, 0x18);
    uint8_t *v = a + 0x18;
    wr64(v, now); wr64(v + 8, now); wr64(v + 16, now); wr64(v + 24, now);
    wr32(v + 32, FA_ARCHIVE);
    if (silen == 72) wr32(v + 52, secid);

    uint32_t fattr = FA_ARCHIVE | (is_dir ? FA_I30_INDEX : 0);
    uint8_t fnbuf[66 + 2 * NTFS_MAX_FILENAME];
    uint32_t fnlen = fn_build(fnbuf, parent_ref, name, nlen, fattr, FN_POSIX, now, 0, 0);
    a = rec_attr_insert(rec, rs, AT_FILE_NAME, 0x18 + fnlen);
    if (!a) return -1;
    wr32(a + 16, fnlen);
    wr16(a + 20, 0x18);
    a[22] = 1;
    memcpy(a + 0x18, fnbuf, fnlen);

    if (!is_dir) {
        a = rec_attr_insert(rec, rs, AT_DATA, 0x18);
        if (!a) return -1;
        wr32(a + 16, 0);
        wr16(a + 20, 0x18);
    } else {
        a = rec_attr_insert(rec, rs, AT_INDEX_ROOT, 0x18 + 8 + IDX_ROOT_HDR + 16 + 16);
        if (!a) return -1;
        a[9] = 4;
        wr16(a + 10, 0x18);
        memcpy(a + 0x18, I30_NAME, 8);
        wr32(a + 16, IDX_ROOT_HDR + 16 + 16);
        wr16(a + 20, 0x20);
        uint8_t *rv = a + 0x20;
        wr32(rv, AT_FILE_NAME);
        wr32(rv + 4, 1);
        wr32(rv + 8, fs->index_block_size);
        rv[12] = (uint8_t)(fs->index_block_size >= fs->cluster_size ? fs->index_block_size / fs->cluster_size : fs->index_block_size / 512);
        uint8_t *hdr = rv + IDX_ROOT_HDR;
        wr32(hdr, 16);
        wr32(hdr + 4, 32);
        wr32(hdr + 8, 32);
        hdr[12] = 0;
        uint8_t *le = hdr + 16;
        wr16(le + 8, 16);
        wr16(le + 12, IE_LAST);
    }
    return 0;
}

/* ---------- path resolution ---------- */

static int dir_lookup(ntfs_t *fs, uint32_t dir_no, const uint16_t *name, int nlen, uint32_t *no, uint16_t *seq)
{
    ntfs_idx_t ix;
    if (ix_open(fs, dir_no, &ix)) return -1;
    uint64_t mref;
    int rc = ix_lookup(&ix, name, nlen, &mref, 0);
    ix_close(&ix);
    if (rc) return -1;
    *no = (uint32_t)(mref & 0xFFFFFFFFFFFFULL);
    if (seq) *seq = (uint16_t)(mref >> 48);
    return 0;
}

static int rec_parent(ntfs_t *fs, uint32_t no, uint32_t *parent)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (mft_read(fs, no, rec) == 0) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, 0);
        if (a && !a[8]) { *parent = (uint32_t)(rd64(attr_res_value(a)) & 0xFFFFFFFFFFFFULL); rc = 0; }
    }
    free(rec);
    return rc;
}

static int split_path(const char *path, char *parent, int psz, char *name, int nsz)
{
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
    int nl = len - (slash + 1);
    if (nl <= 0 || nl >= nsz) return -1;
    memcpy(name, path + slash + 1, (size_t)nl);
    name[nl] = 0;
    int pl = slash < 0 ? 0 : slash;
    if (pl >= psz) return -1;
    memcpy(parent, path, (size_t)pl);
    parent[pl] = 0;
    return 0;
}

static int walk(ntfs_t *fs, const char *path, uint32_t *out)
{
    uint32_t cur = NTFS_REC_ROOT;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            uint32_t par;
            if (cur != NTFS_REC_ROOT && rec_parent(fs, cur, &par) == 0) cur = par;
            continue;
        }
        uint16_t name[NTFS_MAX_FILENAME + 1];
        int nl = utf8_to_utf16(s, len, name, NTFS_MAX_FILENAME);
        if (nl <= 0) return -1;
        uint32_t next;
        if (dir_lookup(fs, cur, name, nl, &next, 0)) return -1;
        cur = next;
    }
    *out = cur;
    return 0;
}

/* ---------- timestamps and size bookkeeping ---------- */

static void std_touch(ntfs_t *fs, uint8_t *rec, uint64_t now, int modify)
{
    uint8_t *a = attr_find(rec, fs->mft_record_size, AT_STD_INFO, 0, 0);
    if (!a || a[8] || attr_res_len(a) < 48) return;
    uint8_t *v = attr_res_value(a);
    if (modify) wr64(v + 8, now);
    wr64(v + 16, now);
    wr64(v + 24, now);
}

/* Rewrites a record's $FILE_NAME copies (sizes/times) and the matching
 * entries in each parent directory index. */
static void sync_names(ntfs_t *fs, uint32_t no)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return;
    if (mft_read(fs, no, rec)) { free(rec); return; }
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    uint8_t *si = attr_find(rec, rs, AT_STD_INFO, 0, 0);
    uint64_t alloc = 0, size = 0;
    if (d) { alloc = d[8] ? rd64(d + 40) : 0; size = attr_size(d); }
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    uint32_t sattr = 0;
    if (si && !si[8] && attr_res_len(si) >= 48) {
        uint8_t *v = attr_res_value(si);
        t0 = rd64(v); t1 = rd64(v + 8); t2 = rd64(v + 16); t3 = rd64(v + 24);
        sattr = rd32(v + 32) & 0xFFFF;
    }
    int is_dir = (rd16(rec + 22) & MFT_IS_DIR) != 0;
    int dirty_rec = 0;
    for (int i = 0;; i++) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, i);
        if (!a) break;
        if (a[8]) continue;
        uint8_t *fn = attr_res_value(a);
        uint8_t key[66 + 2 * NTFS_MAX_FILENAME];
        uint32_t klen = 66 + 2 * fn_name_len(fn);
        wr64(fn + 8, t0); wr64(fn + 16, t1); wr64(fn + 24, t2); wr64(fn + 32, t3);
        if (!is_dir) { wr64(fn + 40, alloc); wr64(fn + 48, size); }
        uint32_t fa = rd32(fn + 56);
        fa = (fa & ~0xFFFFu) | sattr;
        wr32(fn + 56, fa);
        memcpy(key, fn, klen);
        dirty_rec = 1;

        uint32_t parent = (uint32_t)(rd64(fn) & 0xFFFFFFFFFFFFULL);
        ntfs_idx_t ix;
        if (ix_open(fs, parent, &ix) == 0) {
            uint16_t nm[NTFS_MAX_FILENAME];
            memcpy(nm, fn + 66, fn_name_len(fn) * 2);
            uint64_t mref; ix_loc_t loc;
            if (ix_lookup(&ix, nm, (int)fn_name_len(fn), &mref, &loc) == 0 &&
                (uint32_t)(mref & 0xFFFFFFFFFFFFULL) == no) {
                if (loc.in_root) {
                    uint8_t *r = ix_root(&ix);
                    memcpy(attr_res_value(r) + loc.off + 16, key, klen);
                    mft_write(fs, parent, ix.rec);
                } else {
                    uint8_t *blk = (uint8_t *)malloc(ix.bs);
                    if (blk && ix_read_block(&ix, loc.vcn, blk) == 0) {
                        memcpy(blk + loc.off + 16, key, klen);
                        ix_write_block(&ix, loc.vcn, blk);
                    }
                    free(blk);
                }
            }
            ix_close(&ix);
        }
    }
    if (dirty_rec) mft_write(fs, no, rec);
    free(rec);
}

/* ---------- creating, removing and renaming ---------- */

/* Link count = $FILE_NAME attributes that are not DOS-only twins. */
static uint16_t rec_recount_links(uint8_t *rec, uint32_t rs, int *any_names)
{
    uint16_t links = 0;
    *any_names = 0;
    for (int i = 0;; i++) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, i);
        if (!a) break;
        *any_names = 1;
        if (!a[8] && attr_res_value(a)[65] != FN_DOS) links++;
    }
    return links;
}

static int create_node(ntfs_t *fs, uint32_t parent_no, const uint16_t *name, int nlen, int is_dir, uint32_t *out_no)
{
    if (!fs->rw || !valid_name(name, nlen)) return -1;
    uint32_t rs = fs->mft_record_size;
    ntfs_idx_t ix;
    if (ix_open(fs, parent_no, &ix)) return -1;
    if (rec_has_attrlist(ix.rec, rs)) { ix_close(&ix); return -1; }
    uint64_t dummy;
    if (ix_lookup(&ix, name, nlen, &dummy, 0) == 0) { ix_close(&ix); return -1; }

    ent_list_t l;
    if (ix_collect(&ix, &l)) { ix_close(&ix); return -1; }
    int found;
    uint32_t pos = el_find(fs, &l, name, nlen, &found);

    uint32_t no;
    if (mft_alloc_record(fs, &no)) { el_free(&l); ix_close(&ix); return -1; }

    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) { mft_release_record(fs, no); el_free(&l); ix_close(&ix); return -1; }
    uint16_t seq = 1;
    if (mft_read(fs, no, rec) == 0) { seq = rd16(rec + 16); if (seq == 0) seq = 1; }

    uint64_t now = ntfs_now();
    uint32_t secid = 0;
    uint8_t *psi = attr_find(ix.rec, rs, AT_STD_INFO, 0, 0);
    if (psi && !psi[8] && attr_res_len(psi) >= 72) secid = rd32(attr_res_value(psi) + 52);
    uint64_t parent_ref = mref_make(parent_no, rd16(ix.rec + 16));

    int rc = -1;
    if (rec_build_new(fs, rec, no, seq, is_dir, parent_ref, name, nlen, now, secid)) goto fail;
    if (mft_write(fs, no, rec)) goto fail;

    uint8_t *fa = attr_find(rec, rs, AT_FILE_NAME, 0, 0);
    uint8_t ebuf[16 + 66 + 2 * NTFS_MAX_FILENAME + 8];
    uint32_t elen = entry_build(ebuf, mref_make(no, seq), attr_res_value(fa), 66 + 2 * (uint32_t)nlen);
    if (el_insert(&l, pos, ebuf, elen)) goto fail;
    if (ix_rebuild(&ix, &l)) goto fail;
    std_touch(fs, ix.rec, now, 1);
    mft_write(fs, parent_no, ix.rec);
    *out_no = no;
    rc = 0;
fail:
    if (rc) mft_release_record(fs, no);
    free(rec);
    el_free(&l);
    ix_close(&ix);
    return rc;
}

static int dir_is_empty(ntfs_t *fs, uint32_t no)
{
    ntfs_idx_t ix;
    if (ix_open(fs, no, &ix)) return 0;
    ent_list_t l;
    int rc = ix_collect(&ix, &l);
    int empty = rc == 0 && l.n == 0;
    if (rc == 0) el_free(&l);
    ix_close(&ix);
    return empty;
}

/* Frees everything a record owns and releases it. */
static int destroy_record(ntfs_t *fs, uint32_t no)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    if (mft_read(fs, no, rec)) { free(rec); return -1; }
    for (uint8_t *a = attr_first(rec); attr_ok(rec, rs, a); a = attr_next(a))
        attr_free_clusters(fs, a);
    free(rec);
    return mft_release_record(fs, no);
}

static int remove_node(ntfs_t *fs, uint32_t parent_no, const uint16_t *name, int nlen)
{
    if (!fs->rw) return -1;
    uint32_t rs = fs->mft_record_size;
    ntfs_idx_t ix;
    if (ix_open(fs, parent_no, &ix)) return -1;
    if (rec_has_attrlist(ix.rec, rs)) { ix_close(&ix); return -1; }
    ent_list_t l;
    if (ix_collect(&ix, &l)) { ix_close(&ix); return -1; }
    int found;
    uint32_t pos = el_find(fs, &l, name, nlen, &found);
    if (!found) { el_free(&l); ix_close(&ix); return -1; }

    uint64_t mref = rd64(l.items[pos].e);
    uint32_t no = (uint32_t)(mref & 0xFFFFFFFFFFFFULL);
    if (no < NTFS_FIRST_USER_RECORD) { el_free(&l); ix_close(&ix); return -1; }

    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) { el_free(&l); ix_close(&ix); return -1; }
    if (mft_read(fs, no, rec) || rec_has_attrlist(rec, rs)) { free(rec); el_free(&l); ix_close(&ix); return -1; }
    int is_dir = (rd16(rec + 22) & MFT_IS_DIR) != 0;
    if (is_dir && !dir_is_empty(fs, no)) { free(rec); el_free(&l); ix_close(&ix); return -1; }

    /* names to remove: the one given plus its DOS/Win32 twin in this directory */
    uint8_t ntype = l.items[pos].e[16 + 65];
    uint32_t twin = ~0u;
    if (ntype == FN_WIN32 || ntype == FN_DOS) {
        for (uint32_t i = 0; i < l.n; i++) {
            if (i == pos) continue;
            if ((rd64(l.items[i].e) & 0xFFFFFFFFFFFFULL) != no) continue;
            uint8_t t2 = l.items[i].e[16 + 65];
            if ((ntype == FN_WIN32 && t2 == FN_DOS) || (ntype == FN_DOS && t2 == FN_WIN32)) { twin = i; break; }
        }
    }
    uint16_t twin_name[NTFS_MAX_FILENAME];
    int twin_len = 0;
    if (twin != ~0u) {
        twin_len = (int)l.items[twin].e[16 + 64];
        memcpy(twin_name, l.items[twin].e + 16 + 66, (size_t)twin_len * 2);
    }
    if (twin != ~0u && twin > pos) { el_remove(&l, twin); el_remove(&l, pos); }
    else if (twin != ~0u) { el_remove(&l, pos); el_remove(&l, twin); }
    else el_remove(&l, pos);
    int rc = ix_rebuild(&ix, &l);
    el_free(&l);
    if (rc) { free(rec); ix_close(&ix); return -1; }

    /* drop the matching $FILE_NAME attributes from the file's record */
    uint64_t pref_no = parent_no;
    for (int i = 0;;) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, i);
        if (!a) break;
        uint8_t *fn = attr_res_value(a);
        int plen = (int)fn_name_len(fn);
        int match = 0;
        if ((rd64(fn) & 0xFFFFFFFFFFFFULL) == pref_no) {
            uint16_t tmp[NTFS_MAX_FILENAME];
            memcpy(tmp, fn + 66, (size_t)plen * 2);
            if (plen == nlen && collate(fs, tmp, plen, name, nlen) == 0) match = 1;
            if (twin_len && plen == twin_len && collate(fs, tmp, plen, twin_name, twin_len) == 0) match = 1;
        }
        if (match) rec_attr_remove(rec, a); else i++;
    }
    int any_names;
    uint16_t links = rec_recount_links(rec, rs, &any_names);
    wr16(rec + 18, links);
    if (!any_names) links = 0;

    uint64_t now = ntfs_now();
    std_touch(fs, ix.rec, now, 1);
    mft_write(fs, parent_no, ix.rec);
    ix_close(&ix);
    if (links == 0) {
        free(rec);
        return destroy_record(fs, no);
    }
    rc = mft_write(fs, no, rec);
    free(rec);
    return rc;
}

static int is_ancestor(ntfs_t *fs, uint32_t anc, uint32_t node)
{
    for (int i = 0; i < 64 && node != NTFS_REC_ROOT; i++) {
        if (node == anc) return 1;
        uint32_t p;
        if (rec_parent(fs, node, &p)) return 0;
        node = p;
    }
    return node == anc;
}

static int rename_node(ntfs_t *fs, uint32_t op, const uint16_t *oname, int olen, uint32_t np, const uint16_t *nname, int nlen)
{
    if (!fs->rw || !valid_name(nname, nlen)) return -1;
    uint32_t rs = fs->mft_record_size;
    uint32_t src;
    uint16_t sseq;
    if (dir_lookup(fs, op, oname, olen, &src, &sseq)) return -1;
    if (src < NTFS_FIRST_USER_RECORD) return -1;
    uint32_t tmp_no;
    if (dir_lookup(fs, np, nname, nlen, &tmp_no, 0) == 0 && !(tmp_no == src && op == np)) return -1;

    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    if (mft_read(fs, src, rec) || rec_has_attrlist(rec, rs)) { free(rec); return -1; }
    int is_dir = (rd16(rec + 22) & MFT_IS_DIR) != 0;
    if (is_dir && is_ancestor(fs, src, np)) { free(rec); return -1; }

    uint64_t now = ntfs_now();
    uint8_t *old = 0;
    for (int i = 0;; i++) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, i);
        if (!a) break;
        uint8_t *fn = attr_res_value(a);
        if ((rd64(fn) & 0xFFFFFFFFFFFFULL) == op && fn_name_len(fn) == (uint32_t)olen) {
            uint16_t tmp[NTFS_MAX_FILENAME];
            memcpy(tmp, fn + 66, (size_t)olen * 2);
            if (collate(fs, tmp, olen, oname, olen) == 0) { old = a; break; }
        }
    }
    if (!old) { free(rec); return -1; }
    uint8_t *ofn = attr_res_value(old);
    uint64_t alloc = rd64(ofn + 40), size = rd64(ofn + 48);
    uint32_t fattr = rd32(ofn + 56);

    /* remove the old name (and its DOS twin) from the record, add the new one */
    ntfs_idx_t oix;
    if (ix_open(fs, op, &oix)) { free(rec); return -1; }
    ent_list_t l;
    if (ix_collect(&oix, &l)) { ix_close(&oix); free(rec); return -1; }
    int found;
    uint32_t pos = el_find(fs, &l, oname, olen, &found);
    if (!found) { el_free(&l); ix_close(&oix); free(rec); return -1; }
    uint8_t ntype = l.items[pos].e[16 + 65];
    uint32_t twin = ~0u;
    if (ntype == FN_WIN32 || ntype == FN_DOS) {
        for (uint32_t i = 0; i < l.n; i++) {
            if (i == pos || (rd64(l.items[i].e) & 0xFFFFFFFFFFFFULL) != src) continue;
            uint8_t t2 = l.items[i].e[16 + 65];
            if ((ntype == FN_WIN32 && t2 == FN_DOS) || (ntype == FN_DOS && t2 == FN_WIN32)) { twin = i; break; }
        }
    }
    uint16_t twin_name[NTFS_MAX_FILENAME];
    int twin_len = 0;
    if (twin != ~0u) {
        twin_len = (int)l.items[twin].e[16 + 64];
        memcpy(twin_name, l.items[twin].e + 16 + 66, (size_t)twin_len * 2);
    }
    uint8_t *saved = (uint8_t *)malloc(rs);
    if (!saved) { el_free(&l); ix_close(&oix); free(rec); return -1; }
    memcpy(saved, rec, rs);

    for (int i = 0;;) {
        uint8_t *a = attr_find_nth(rec, rs, AT_FILE_NAME, i);
        if (!a) break;
        uint8_t *fn = attr_res_value(a);
        int plen = (int)fn_name_len(fn);
        int match = 0;
        if ((rd64(fn) & 0xFFFFFFFFFFFFULL) == op) {
            uint16_t tmp[NTFS_MAX_FILENAME];
            memcpy(tmp, fn + 66, (size_t)plen * 2);
            if (plen == olen && collate(fs, tmp, plen, oname, olen) == 0) match = 1;
            if (twin_len && plen == twin_len && collate(fs, tmp, plen, twin_name, twin_len) == 0) match = 1;
        }
        if (match) rec_attr_remove(rec, a); else i++;
    }
    uint8_t *pr = 0;
    ntfs_idx_t nix;
    int same = (np == op);
    if (!same && ix_open(fs, np, &nix)) { memcpy(rec, saved, rs); free(saved); el_free(&l); ix_close(&oix); free(rec); return -1; }
    uint64_t pref = mref_make(np, same ? rd16(oix.rec + 16) : rd16(nix.rec + 16));
    uint8_t fnbuf[66 + 2 * NTFS_MAX_FILENAME];
    uint32_t fnlen = fn_build(fnbuf, pref, nname, nlen, fattr, FN_POSIX, now, alloc, size);
    uint8_t *na = rec_attr_insert(rec, rs, AT_FILE_NAME, 0x18 + fnlen);
    if (!na) {
        /* a long name does not fit next to resident data: move the data out */
        uint8_t *dd = attr_find(rec, rs, AT_DATA, 0, 0);
        if (dd && !dd[8] && attr_make_nonres(fs, rec, dd, 0) == 0)
            na = rec_attr_insert(rec, rs, AT_FILE_NAME, 0x18 + fnlen);
    }
    if (!na) goto fail;
    wr32(na + 16, fnlen);
    wr16(na + 20, 0x18);
    na[22] = 1;
    memcpy(na + 0x18, fnbuf, fnlen);
    {
        uint8_t ebuf[16 + 66 + 2 * NTFS_MAX_FILENAME + 8];
        uint32_t elen = entry_build(ebuf, mref_make(src, sseq), fnbuf, fnlen);
        /* remove from the source list */
        if (twin != ~0u && twin > pos) { el_remove(&l, twin); el_remove(&l, pos); }
        else if (twin != ~0u) { el_remove(&l, pos); el_remove(&l, twin); }
        else el_remove(&l, pos);
        if (same) {
            int f2;
            uint32_t ip = el_find(fs, &l, nname, nlen, &f2);
            if (f2 || el_insert(&l, ip, ebuf, elen)) goto fail;
            if (ix_rebuild(&oix, &l)) goto fail;
            std_touch(fs, oix.rec, now, 1);
            mft_write(fs, op, oix.rec);
        } else {
            ent_list_t nl;
            if (ix_collect(&nix, &nl)) goto fail;
            int f2;
            uint32_t ip = el_find(fs, &nl, nname, nlen, &f2);
            if (f2 || el_insert(&nl, ip, ebuf, elen)) { el_free(&nl); goto fail; }
            if (ix_rebuild(&nix, &nl)) { el_free(&nl); goto fail; }
            el_free(&nl);
            if (ix_rebuild(&oix, &l)) goto fail;
            std_touch(fs, nix.rec, now, 1);
            mft_write(fs, np, nix.rec);
            std_touch(fs, oix.rec, now, 1);
            mft_write(fs, op, oix.rec);
        }
    }
    std_touch(fs, rec, now, 0);
    {
        int any;
        wr16(rec + 18, rec_recount_links(rec, rs, &any));
    }
    if (mft_write(fs, src, rec)) goto fail;
    (void)pr;
    if (!same) ix_close(&nix);
    free(saved); el_free(&l); ix_close(&oix); free(rec);
    return 0;
fail:
    if (!same) ix_close(&nix);
    free(saved); el_free(&l); ix_close(&oix); free(rec);
    return -1;
}

/* ---------- file operations ---------- */

static int get_info(ntfs_t *fs, uint32_t no, uint64_t *size, int *is_dir, int *blocked)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    if (mft_read(fs, no, rec)) { free(rec); return -1; }
    *is_dir = (rd16(rec + 22) & MFT_IS_DIR) != 0;
    *size = 0;
    *blocked = 0;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (d) {
        *size = attr_size(d);
        if (d[8] && attr_blocked(d)) *blocked = 1;
    }
    if (rec_has_attrlist(rec, rs) && !*is_dir) *blocked = 1;
    free(rec);
    return 0;
}

static int file_truncate(ntfs_t *fs, uint32_t no, uint64_t size)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (mft_read(fs, no, rec) || rec_has_attrlist(rec, rs)) goto out;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!d || (d[8] && attr_blocked(d))) goto out;
    if (attr_truncate(fs, rec, d, size)) goto out;
    std_touch(fs, rec, ntfs_now(), 1);
    if (mft_write(fs, no, rec)) goto out;
    rc = 0;
out:
    free(rec);
    if (rc == 0) sync_names(fs, no);
    return rc;
}

static int ntfs_vfs_open(void *ctx, const char *path, int flags)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    int want_write = (flags & 3) != 0 || (flags & (VFS_CREAT | VFS_TRUNC | VFS_APPEND));
    if (want_write && !fs->rw) return -1;
    int slot = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) if (!fs->fds[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    uint32_t no;
    if (walk(fs, path, &no)) {
        if (!(flags & VFS_CREAT)) return -1;
        char parent[256], name[NTFS_MAX_FILENAME * 3 + 1];
        if (split_path(path, parent, sizeof(parent), name, sizeof(name))) return -1;
        uint32_t pno;
        if (walk(fs, parent, &pno)) return -1;
        uint16_t n16[NTFS_MAX_FILENAME + 1];
        int nl = utf8_to_utf16(name, (int)strlen(name), n16, NTFS_MAX_FILENAME);
        if (nl <= 0) return -1;
        if (create_node(fs, pno, n16, nl, 0, &no)) return -1;
    }
    uint64_t size; int is_dir, blocked;
    if (get_info(fs, no, &size, &is_dir, &blocked)) return -1;
    if (is_dir && want_write) return -1;
    if (!is_dir && blocked && want_write) return -1;
    if ((flags & VFS_TRUNC) && !is_dir && size) {
        if (file_truncate(fs, no, 0)) return -1;
        size = 0;
    }
    ntfs_fd_t *f = &fs->fds[slot];
    f->used = 1;
    f->record = no;
    f->is_dir = is_dir;
    f->size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
    f->pos = (flags & VFS_APPEND) ? f->size : 0;
    f->append = (flags & VFS_APPEND) != 0;
    f->dirty = 0;
    return slot;
}

static int ntfs_vfs_close(void *ctx, int fd)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    if (fs->fds[fd].dirty && fs->rw) sync_names(fs, fs->fds[fd].record);
    fs->fds[fd].used = 0;
    return 0;
}

static int ntfs_vfs_read(void *ctx, int fd, void *buf, uint32_t size)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    ntfs_fd_t *f = &fs->fds[fd];
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (mft_read(fs, f->record, rec)) goto out;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!d || (d[8] && attr_blocked(d))) goto out;
    uint64_t dsize = attr_size(d);
    f->size = dsize > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)dsize;
    if (f->pos >= dsize) { rc = 0; goto out; }
    uint32_t n = size;
    if ((uint64_t)n > dsize - f->pos) n = (uint32_t)(dsize - f->pos);
    if (n && attr_read(fs, d, f->pos, buf, n)) goto out;
    f->pos += n;
    rc = (int)n;
out:
    free(rec);
    return rc;
}

static int ntfs_vfs_write(void *ctx, int fd, const void *buf, uint32_t size)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (!fs->rw) return -1;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used || fs->fds[fd].is_dir) return -1;
    if (size == 0) return 0;
    ntfs_fd_t *f = &fs->fds[fd];
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (mft_read(fs, f->record, rec) || rec_has_attrlist(rec, rs)) goto out;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!d || (d[8] && attr_blocked(d))) goto out;
    uint64_t pos = f->append ? attr_size(d) : f->pos;
    if (attr_write(fs, rec, d, pos, buf, size)) goto out;
    std_touch(fs, rec, ntfs_now(), 1);
    if (mft_write(fs, f->record, rec)) goto out;
    f->pos = (uint32_t)(pos + size);
    uint64_t ns = attr_size(d);
    f->size = ns > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)ns;
    f->dirty = 1;
    rc = (int)size;
out:
    free(rec);
    return rc;
}

static int ntfs_vfs_lseek(void *ctx, int fd, uint32_t offset, int whence)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    if (fd < 0 || fd >= VFS_MAX_FDS || !fs->fds[fd].used) return -1;
    ntfs_fd_t *f = &fs->fds[fd];
    uint64_t np;
    if (whence == VFS_SEEK_SET) np = offset;
    else if (whence == VFS_SEEK_CUR) np = (uint64_t)f->pos + offset;
    else if (whence == VFS_SEEK_END) np = (uint64_t)f->size + offset;
    else return -1;
    if (np > f->size) np = f->size;
    f->pos = (uint32_t)np;
    return (int)f->pos;
}

static int ntfs_vfs_readdir(void *ctx, const char *path, vfs_entry_t *entries, int max)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    uint32_t dno;
    if (walk(fs, path, &dno)) return -1;
    ntfs_idx_t ix;
    if (ix_open(fs, dno, &ix)) return -1;
    ent_list_t l;
    if (ix_collect(&ix, &l)) { ix_close(&ix); return -1; }
    int count = 0;
    for (uint32_t i = 0; i < l.n && count < max; i++) {
        const uint8_t *e = l.items[i].e;
        const uint8_t *key = e + 16;
        if (key[65] == FN_DOS) continue;
        uint32_t no = (uint32_t)(rd64(e) & 0xFFFFFFFFFFFFULL);
        uint16_t nm[NTFS_MAX_FILENAME];
        int nl = (int)key[64];
        memcpy(nm, key + 66, (size_t)nl * 2);
        if (no < 16 || (no < NTFS_FIRST_USER_RECORD && nm[0] == '$')) continue;
        vfs_entry_t *out = &entries[count];
        memset(out, 0, sizeof(*out));
        utf16_to_utf8(nm, nl, out->name, VFS_NAME_LEN);
        uint64_t size; int is_dir, blocked;
        if (get_info(fs, no, &size, &is_dir, &blocked)) {
            size = rd64(key + 48);
            is_dir = (rd32(key + 56) & FA_I30_INDEX) != 0;
        }
        out->size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
        out->is_dir = is_dir;
        out->inode = no;
        out->mode = is_dir ? 040755 : 0100644;
        count++;
    }
    el_free(&l);
    ix_close(&ix);
    return count;
}

static int ntfs_vfs_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)mode;
    ntfs_t *fs = (ntfs_t *)ctx;
    char parent[256], name[NTFS_MAX_FILENAME * 3 + 1];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name))) return -1;
    uint32_t pno;
    if (walk(fs, parent, &pno)) return -1;
    uint16_t n16[NTFS_MAX_FILENAME + 1];
    int nl = utf8_to_utf16(name, (int)strlen(name), n16, NTFS_MAX_FILENAME);
    if (nl <= 0) return -1;
    uint32_t no;
    return create_node(fs, pno, n16, nl, 1, &no);
}

static int ntfs_vfs_unlink(void *ctx, const char *path)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    char parent[256], name[NTFS_MAX_FILENAME * 3 + 1];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name))) return -1;
    uint32_t pno;
    if (walk(fs, parent, &pno)) return -1;
    uint16_t n16[NTFS_MAX_FILENAME + 1];
    int nl = utf8_to_utf16(name, (int)strlen(name), n16, NTFS_MAX_FILENAME);
    if (nl <= 0) return -1;
    return remove_node(fs, pno, n16, nl);
}

static int ntfs_vfs_stat(void *ctx, const char *path, vfs_entry_t *entry)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    uint32_t no;
    if (walk(fs, path, &no)) return -1;
    uint64_t size; int is_dir, blocked;
    if (get_info(fs, no, &size, &is_dir, &blocked)) return -1;
    memset(entry, 0, sizeof(*entry));
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    int s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    int nl = len - s;
    if (nl >= VFS_NAME_LEN) nl = VFS_NAME_LEN - 1;
    memcpy(entry->name, path + s, (size_t)nl);
    entry->name[nl] = 0;
    entry->size = size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size;
    entry->is_dir = is_dir;
    entry->inode = no;
    entry->mode = is_dir ? 040755 : 0100644;
    return 0;
}

static int ntfs_vfs_rename(void *ctx, const char *old, const char *new_path)
{
    ntfs_t *fs = (ntfs_t *)ctx;
    char op[256], on[NTFS_MAX_FILENAME * 3 + 1], np[256], nn[NTFS_MAX_FILENAME * 3 + 1];
    if (split_path(old, op, sizeof(op), on, sizeof(on))) return -1;
    if (split_path(new_path, np, sizeof(np), nn, sizeof(nn))) return -1;
    uint32_t opn, npn;
    if (walk(fs, op, &opn) || walk(fs, np, &npn)) return -1;
    uint16_t o16[NTFS_MAX_FILENAME + 1], n16[NTFS_MAX_FILENAME + 1];
    int ol = utf8_to_utf16(on, (int)strlen(on), o16, NTFS_MAX_FILENAME);
    int nl = utf8_to_utf16(nn, (int)strlen(nn), n16, NTFS_MAX_FILENAME);
    if (ol <= 0 || nl <= 0) return -1;
    return rename_node(fs, opn, o16, ol, npn, n16, nl);
}

static int ntfs_vfs_symlink(void *ctx, const char *target, const char *path)
{
    (void)ctx; (void)target; (void)path;
    return -1;
}

/* ---------- mounting ---------- */

static int vol_set_dirty(ntfs_t *fs, int dirty)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (mft_read(fs, NTFS_REC_VOLUME, rec) == 0) {
        uint8_t *a = attr_find(rec, rs, AT_VOL_INFO, 0, 0);
        if (a && !a[8] && attr_res_len(a) >= 12) {
            uint8_t *v = attr_res_value(a);
            uint16_t fl = rd16(v + 10);
            fl = dirty ? (uint16_t)(fl | VOLF_DIRTY) : (uint16_t)(fl & ~VOLF_DIRTY);
            wr16(v + 10, fl);
            rc = mft_write(fs, NTFS_REC_VOLUME, rec);
        }
    }
    free(rec);
    return rc;
}

static int logfile_clean(ntfs_t *fs)
{
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    uint8_t *page = (uint8_t *)malloc(4096);
    int clean = 0;
    if (!rec || !page) goto out;
    if (mft_read(fs, NTFS_REC_LOGFILE, rec)) goto out;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!d || !d[8] || attr_size(d) < 4096) goto out;
    if (attr_read(fs, d, 0, page, 4096)) goto out;
    int all_ff = 1;
    for (int i = 0; i < 64; i++) if (page[i] != 0xFF) { all_ff = 0; break; }
    if (all_ff) { clean = 1; goto out; }
    if (memcmp(page, "RSTR", 4) == 0) {
        uint32_t sps = rd32(page + 16);
        if (sps != 4096 && sps != 2048 && sps != 1024) goto out;
        if (sps != 4096) goto out;
        if (fixup_apply(page, 4096, "RSTR")) goto out;
        uint16_t ra = rd16(page + 24);
        if (ra + 16u > 4096) goto out;
        uint16_t in_use = rd16(page + ra + 12);
        uint16_t flags = rd16(page + ra + 14);
        clean = (in_use == 0xFFFF) || (flags & 2);
    }
out:
    free(rec);
    free(page);
    return clean;
}

static void ntfs_free_state(ntfs_t *fs)
{
    free(fs->upcase);
    free(fs->mft_runs);
    free(fs->bm_runs);
    fs->upcase = 0;
    fs->mft_runs = 0;
    fs->bm_runs = 0;
}

int ntfs_detect(blockdev_t *bd)
{
    uint8_t boot[512];
    if (blockdev_read_bytes(bd, 0, 512, boot) != 0) return -1;
    if (memcmp(boot + 3, "NTFS    ", 8) != 0) return -1;
    if (rd16(boot + 510) != 0xAA55) return -1;
    uint16_t bps = rd16(boot + 0x0B);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) return -1;
    if (boot[0x0D] == 0) return -1;
    return 0;
}

static int mount_internal(ntfs_t *fs, blockdev_t *bd, int allow_rw)
{
    uint8_t boot[512];
    if (blockdev_read_bytes(bd, 0, 512, boot) != 0) return -1;
    if (ntfs_detect(bd) != 0) return -1;

    memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    fs->bytes_per_sector = rd16(boot + 0x0B);
    uint32_t spc = boot[0x0D];
    if (spc > 0x80) spc = 1u << (256 - spc);
    fs->sectors_per_cluster = spc;
    fs->cluster_size = fs->bytes_per_sector * spc;
    if (spc == 0 || fs->cluster_size > 2 * 1024 * 1024) return -1;
    fs->total_sectors = rd64(boot + 0x28);
    fs->mft_lcn = rd64(boot + 0x30);
    fs->mftmirr_lcn = rd64(boot + 0x38);
    int8_t cpr = (int8_t)boot[0x40], cpi = (int8_t)boot[0x44];
    fs->mft_record_size = cpr < 0 ? (1u << (-cpr)) : (uint32_t)cpr * fs->cluster_size;
    fs->index_block_size = cpi < 0 ? (1u << (-cpi)) : (uint32_t)cpi * fs->cluster_size;
    if (fs->mft_record_size < 512 || fs->mft_record_size > 65536 || (fs->mft_record_size & 511)) return -1;
    if (fs->index_block_size < 512 || fs->index_block_size > 65536 || (fs->index_block_size & (fs->index_block_size - 1))) return -1;
    fs->total_clusters = fs->total_sectors / spc;
    if (fs->total_sectors * fs->bytes_per_sector > bd->total_sectors * (uint64_t)bd->sector_size) return -1;
    fs->ntfs_major = 3;
    fs->mirr_records = 4;

    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) return -1;
    int rc = -1;
    if (dev_read(fs, fs->mft_lcn * fs->cluster_size, rs, rec)) goto out;
    if (fixup_apply(rec, rs, "FILE")) goto out;
    uint8_t *d = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!d || !d[8] || attr_find(rec, rs, AT_ATTR_LIST, 0, 0)) goto out;
    if (mft_load_runs(fs, d)) goto out;

    /* $Volume: version and dirty flag */
    if (mft_read(fs, NTFS_REC_VOLUME, rec)) goto out;
    uint8_t *vi = attr_find(rec, rs, AT_VOL_INFO, 0, 0);
    int dirty = 1;
    if (vi && !vi[8] && attr_res_len(vi) >= 12) {
        uint8_t *v = attr_res_value(vi);
        fs->ntfs_major = v[8];
        dirty = (rd16(v + 10) & (VOLF_DIRTY | 0x4000)) != 0;
    }
    if (fs->ntfs_major < 1 || fs->ntfs_major > 3) goto out;

    if (mft_read(fs, NTFS_REC_MFTMIRR, rec) == 0) {
        uint8_t *md = attr_find(rec, rs, AT_DATA, 0, 0);
        if (md) {
            uint32_t m = (uint32_t)(attr_size(md) / rs);
            if (m >= 1 && m <= 16) fs->mirr_records = m;
        }
    }

    /* $Bitmap */
    if (mft_read(fs, NTFS_REC_BITMAP, rec)) goto out;
    uint8_t *bd_attr = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!bd_attr || !bd_attr[8]) goto out;
    {
        ntfs_run_t *runs; int nr;
        if (attr_runs(fs, bd_attr, &runs, &nr)) goto out;
        fs->bm_runs = runs;
        fs->bm_nruns = nr;
        fs->bm_data_size = rd64(bd_attr + 48);
    }

    /* $UpCase */
    fs->upcase_len = 65536;
    fs->upcase = (uint16_t *)malloc(65536 * 2);
    if (!fs->upcase) goto out;
    int up_ok = 0;
    if (mft_read(fs, NTFS_REC_UPCASE, rec) == 0) {
        uint8_t *ud = attr_find(rec, rs, AT_DATA, 0, 0);
        if (ud && attr_size(ud) >= 2 && attr_size(ud) <= 65536 * 2) {
            uint32_t n = (uint32_t)(attr_size(ud) / 2);
            if (attr_read(fs, ud, 0, fs->upcase, n * 2) == 0) { fs->upcase_len = n; up_ok = 1; }
        }
    }
    if (!up_ok) {
        for (uint32_t i = 0; i < 65536; i++) fs->upcase[i] = (uint16_t)((i >= 'a' && i <= 'z') ? i - 32 : i);
        fs->upcase_len = 65536;
    }

    fs->rw = allow_rw && !dirty && logfile_clean(fs);
    fs->mft_hint = NTFS_FIRST_USER_RECORD;
    rc = 0;
out:
    free(rec);
    if (rc) ntfs_free_state(fs);
    return rc;
}

int ntfs_probe_and_mount(ntfs_t *fs, blockdev_t *bd)
{
    return mount_internal(fs, bd, 1);
}

int ntfs_umount(ntfs_t *fs)
{
    if (fs->dirty_marked) {
        vol_set_dirty(fs, 0);
        fs->dirty_marked = 0;
    }
    fs->rw = 0;
    ntfs_free_state(fs);
    return 0;
}

void ntfs_mount_vfs(ntfs_t *fs, const char *mount_point)
{
    static vfs_ops_t ntfs_vfs_ops = {
        .open = ntfs_vfs_open,
        .close = ntfs_vfs_close,
        .read = ntfs_vfs_read,
        .write = ntfs_vfs_write,
        .lseek = ntfs_vfs_lseek,
        .readdir = ntfs_vfs_readdir,
        .mkdir = ntfs_vfs_mkdir,
        .unlink = ntfs_vfs_unlink,
        .stat = ntfs_vfs_stat,
        .rename = ntfs_vfs_rename,
        .symlink = ntfs_vfs_symlink,
    };
    if (fs->rw) {
        if (vol_set_dirty(fs, 1) == 0) fs->dirty_marked = 1;
        else fs->rw = 0;
    } else {
        klog_write("ntfs: mounted read-only (volume dirty, journal not clean or unsupported)\n");
    }
    vfs_mount(mount_point, &ntfs_vfs_ops, fs);
}

int ntfs_format(blockdev_t *bd, const char *label)
{
    (void)bd; (void)label;
    return -1;
}

static void set_err(char *err, int err_len, const char *msg)
{
    if (err && err_len > 0) { strncpy(err, msg, (size_t)err_len - 1); err[err_len - 1] = 0; }
}

/* Writes the primary boot sector's total_sectors and the backup copy kept in
 * the last sector of the volume. */
static int boot_set_total(ntfs_t *fs, uint64_t new_total)
{
    uint32_t bps = fs->bytes_per_sector;
    uint8_t *boot = (uint8_t *)malloc(bps);
    if (!boot) return -1;
    int rc = -1;
    if (dev_read(fs, 0, bps, boot)) goto out;
    wr64(boot + 0x28, new_total);
    if (dev_write(fs, 0, bps, boot)) goto out;
    if (dev_write(fs, new_total * bps, bps, boot)) goto out;
    rc = 0;
out:
    free(boot);
    return rc;
}

/* Grows the filesystem to fill `new_sectors` device sectors, or shrinks it
 * when every cluster that would be cut off is free. The volume covers all
 * sectors but the last, which holds the backup boot sector. */
int ntfs_resize(blockdev_t *bd, uint64_t new_sectors, char *err, int err_len)
{
    ntfs_t *fs = (ntfs_t *)malloc(sizeof(ntfs_t));
    if (!fs) { set_err(err, err_len, "out of memory"); return -1; }
    int rc = -1;
    if (mount_internal(fs, bd, 1) != 0) { set_err(err, err_len, "not a supported NTFS volume"); free(fs); return -1; }
    if (!fs->rw) { set_err(err, err_len, "volume is dirty or its journal is not clean; run chkdsk in Windows first"); goto out; }

    uint64_t dev_sectors = bd->total_sectors * (uint64_t)bd->sector_size / fs->bytes_per_sector;
    if (new_sectors == 0 || new_sectors > dev_sectors) new_sectors = dev_sectors;
    if (new_sectors < 64) { set_err(err, err_len, "size too small"); goto out; }
    uint64_t new_total = new_sectors - 1;
    uint64_t new_clusters = new_total / fs->sectors_per_cluster;
    uint64_t old_clusters = fs->total_clusters;
    if (new_clusters == old_clusters && new_total == fs->total_sectors) { set_err(err, err_len, "already that size"); rc = 0; goto out; }
    if (new_clusters < 64) { set_err(err, err_len, "size too small"); goto out; }

    uint64_t bm_bytes_new = ((new_clusters + 63) / 64) * 8;
    uint32_t rs = fs->mft_record_size;
    uint8_t *rec = (uint8_t *)malloc(rs);
    if (!rec) { set_err(err, err_len, "out of memory"); goto out; }

    if (new_clusters < old_clusters) {
        /* every cluster being cut must be free */
        uint8_t *win = (uint8_t *)malloc(4096);
        if (!win) { free(rec); set_err(err, err_len, "out of memory"); goto out; }
        int busy = 0;
        uint64_t c = new_clusters;
        while (c < old_clusters && !busy) {
            uint64_t b0 = c >> 3;
            uint32_t wl = 4096;
            if (b0 + wl > fs->bm_data_size) wl = (uint32_t)(fs->bm_data_size - b0);
            if (bm_io(fs, b0, win, wl, 0)) { busy = 2; break; }
            uint64_t lim = (b0 + wl) << 3;
            for (; c < old_clusters && c < lim; c++)
                if (win[(c >> 3) - b0] & (1u << (c & 7))) { busy = 1; break; }
        }
        free(win);
        if (busy) {
            free(rec);
            set_err(err, err_len, busy == 1 ? "data lives in the area being removed; cannot shrink that far" : "bitmap read failed");
            goto out;
        }
        if (fs->mftmirr_lcn + 1 > new_clusters) { free(rec); set_err(err, err_len, "$MFTMirr lies beyond the new end"); goto out; }
    }

    /* resize $Bitmap's data attribute. Growing it allocates clusters from
     * the old volume, so that has to happen before the bitmap is read. */
    if (mft_read(fs, NTFS_REC_BITMAP, rec)) { free(rec); set_err(err, err_len, "cannot read $Bitmap"); goto out; }
    uint8_t *bmd = attr_find(rec, rs, AT_DATA, 0, 0);
    if (!bmd || !bmd[8]) { free(rec); set_err(err, err_len, "unexpected $Bitmap layout"); goto out; }
    uint64_t old_bytes = rd64(bmd + 48);
    if (bm_bytes_new > old_bytes) {
        if (attr_grow_alloc(fs, rec, bmd, bm_bytes_new)) { free(rec); set_err(err, err_len, "no room to grow $Bitmap"); goto out; }
    }
    uint64_t keep_bytes = old_bytes > bm_bytes_new ? old_bytes : bm_bytes_new;
    uint8_t *bits = (uint8_t *)malloc((size_t)keep_bytes);
    if (!bits) { free(rec); set_err(err, err_len, "out of memory"); goto out; }
    memset(bits, 0, (size_t)keep_bytes);
    if (bm_io(fs, 0, bits, (uint32_t)old_bytes, 0)) { free(bits); free(rec); set_err(err, err_len, "bitmap read failed"); goto out; }

    /* clusters that become part of the volume are free; bits past the end are padding (set) */
    for (uint64_t cl = old_clusters; cl < new_clusters; cl++) bits[cl >> 3] &= (uint8_t)~(1u << (cl & 7));
    for (uint64_t cl = new_clusters; cl < bm_bytes_new * 8; cl++) bits[cl >> 3] |= (uint8_t)(1u << (cl & 7));

    if (bm_bytes_new > old_bytes) {
        wr64(bmd + 48, bm_bytes_new);
        wr64(bmd + 56, bm_bytes_new);
    } else if (bm_bytes_new < old_bytes) {
        if (attr_truncate(fs, rec, bmd, bm_bytes_new)) { free(bits); free(rec); set_err(err, err_len, "cannot shrink $Bitmap"); goto out; }
    }
    ntfs_run_t *nruns; int nn;
    if (attr_runs(fs, bmd, &nruns, &nn)) { free(bits); free(rec); set_err(err, err_len, "bitmap runlist error"); goto out; }
    int wrc = run_io(fs, nruns, nn, 0, bits, (uint32_t)bm_bytes_new, 1);
    if (wrc == 0) {
        free(fs->bm_runs);
        fs->bm_runs = nruns;
        fs->bm_nruns = nn;
        fs->bm_data_size = bm_bytes_new;
    } else {
        free(nruns);
    }
    free(bits);
    if (wrc || mft_write(fs, NTFS_REC_BITMAP, rec)) { free(rec); set_err(err, err_len, "writing $Bitmap failed"); goto out; }
    free(rec);

    fs->total_clusters = new_clusters;
    if (boot_set_total(fs, new_total)) { set_err(err, err_len, "writing boot sector failed"); goto out; }
    fs->total_sectors = new_total;
    rc = 0;
out:
    ntfs_umount(fs);
    free(fs);
    return rc;
}
