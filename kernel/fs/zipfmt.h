#ifndef ZIPFMT_H
#define ZIPFMT_H

/* Creates a ZIP archive at `archive` from `npaths` files/directories
 * (directories added recursively). Entries are stored uncompressed
 * (method 0) — a valid, ordinary ZIP any unzip tool can read, just
 * without the size savings DEFLATE would give; tOS doesn't carry a
 * DEFLATE *encoder*, only a decoder (see inflate_raw_buffer() in
 * png.c, reused here for *extracting* deflated entries). Overwrites
 * `archive` if it already exists. Returns 0 on success, -1 on error. */
int zip_create(const char *archive, const char **paths, int npaths, char *err, int err_len);

/* Extracts every entry under `dest_dir` (must already exist),
 * recreating intermediate directories as needed. Understands both
 * stored (method 0) and deflated (method 8) entries, so it can read
 * real-world ZIP files, not just ones tOS made itself. Returns the
 * number of entries extracted, or -1 on error. */
int zip_extract(const char *archive, const char *dest_dir, char *err, int err_len);

#endif
