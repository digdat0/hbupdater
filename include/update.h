#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Query a repo's releases and return the highest version's tag plus the download
 * URL of the matching asset. `asset_pat` is a filename/glob (e.g. "App.nro",
 * "*.ovl"); NULL/"" means "*.nro". If the glob matches nothing, the asset whose
 * extension matches the glob's extension is used as a fallback. Prereleases are
 * considered only when allow_prerelease is true. Returns true on success.
 */
bool update_fetch_latest(const char *repo, const char *asset_pat,
                         bool allow_prerelease, char *tag, size_t tag_sz,
                         char *url, size_t url_sz, uint64_t *asset_size);

/* Compare dotted versions ("1.2.3", optionally "v"-prefixed).
 * Returns <0 if a<b, 0 if equal, >0 if a>b. */
int version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_H */
