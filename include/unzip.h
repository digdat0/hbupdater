#ifndef UNZIP_H
#define UNZIP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: return non-zero to abort. done/total are entry counts. */
typedef int (*unzip_progress_cb)(void *ud, int done, int total);

/* Called just before a file entry is written, with its absolute dest path (so a
 * caller can back it up). Return non-zero to abort. May be NULL. */
typedef int (*unzip_prewrite_cb)(void *ud, const char *dest_path);

/*
 * Extract every entry of `zip_path` under `dest_root` (e.g. "sdmc:/"),
 * preserving the archive's internal directory structure, creating directories
 * as needed and overwriting existing files. Unsafe entries (absolute paths or
 * ".." traversal) and encrypted/unsupported-method entries are skipped.
 * Uses zlib's raw inflate (deflate, method 8) and stored (method 0).
 * Returns true if extraction completed without a hard error; *n_extracted (if
 * non-NULL) gets the number of entries processed.
 */
bool unzip_extract(const char *zip_path, const char *dest_root,
                   unzip_progress_cb cb, void *ud, unzip_prewrite_cb prewrite,
                   void *prewrite_ud, int *n_extracted);

#ifdef __cplusplus
}
#endif

#endif /* UNZIP_H */
