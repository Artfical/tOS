#ifndef TARFMT_H
#define TARFMT_H

/* Creates a ustar-format archive at `archive` from `npaths` files/
 * directories (directories are added recursively). Overwrites
 * `archive` if it already exists. Returns 0 on success, -1 on error
 * (with a message in `err`). */
int tar_create(const char *archive, const char **paths, int npaths, char *err, int err_len);

/* Extracts every entry in `archive` under `dest_dir` (must already
 * exist), recreating any intermediate directories an entry's path
 * needs. Returns the number of entries extracted, or -1 on error. */
int tar_extract(const char *archive, const char *dest_dir, char *err, int err_len);

/* Lists each entry's name (and a trailing '/' for directories) one
 * per line via tar_list_cb, in archive order. Returns the entry
 * count, or -1 on error. */
int tar_list(const char *archive, void (*cb)(const char *name, int is_dir, unsigned int size), char *err, int err_len);

#endif
