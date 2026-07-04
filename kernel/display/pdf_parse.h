#ifndef PDF_PARSE_H
#define PDF_PARSE_H

#include <stdint.h>

/* Minimal from-scratch PDF reader: extracts page geometry (MediaBox) and
 * positioned text runs (position + approximate font size + string) well
 * enough to display simple, mostly-text PDFs. It deliberately does NOT
 * implement the full PDF object model or renderer:
 *  - Objects are found by brute-force scanning for "N G obj ... endobj"
 *    rather than following the xref table, so it works on both classic
 *    xref-table PDFs and (most) xref-stream PDFs without needing to
 *    parse either.
 *  - Only FlateDecode-compressed content streams are decompressed;
 *    other stream filters (DCTDecode/JPX/LZW/RunLength/embedded fonts)
 *    are not supported.
 *  - Only the text-showing subset of content-stream operators is
 *    interpreted (BT, ET, Tf, Td, TD, Tm, T-star, TL, Tj, TJ, and the
 *    two quote-mark show-and-move operators). Vector graphics (paths,
 *    images, cm matrix transforms, q/Q state save/restore) are ignored
 *    entirely, and text position uses only the translation component
 *    of Tm (no rotation/skew/scale support).
 *  - Text is decoded assuming a single-byte ASCII/WinAnsi-ish encoding
 *    with no embedded font glyph mapping -- correct for ordinary Latin
 *    text PDFs, wrong for anything using a custom/symbolic font
 *    encoding (so glyphs may look garbled in those cases). */

#define PDF_MAX_PAGES          32
#define PDF_MAX_RUNS_PER_PAGE  256
#define PDF_MAX_RUN_LEN        96

typedef struct {
    double x, y;   /* baseline position, PDF user-space points (origin bottom-left) */
    double size;   /* approximate font size in points */
    char   text[PDF_MAX_RUN_LEN];
    int    len;
} pdf_run_t;

typedef struct {
    double mbx0, mby0, mbx1, mby1; /* MediaBox, in points */
    int run_count;
    pdf_run_t runs[PDF_MAX_RUNS_PER_PAGE];
} pdf_page_t;

typedef struct {
    int page_count;
    pdf_page_t pages[PDF_MAX_PAGES];
} pdf_doc_t;

/* *doc must point at caller-owned storage (this struct is ~1MB — heap
 * allocate it, never put it on the stack). Returns 0 on success, -1
 * with a human-readable message written into err/err_cap otherwise. */
int  pdf_parse(const uint8_t *data, uint32_t size, pdf_doc_t *doc, char *err, int err_cap);
void pdf_free(pdf_doc_t *doc);

#endif
