#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One entry in the bundled known-apps catalog (romfs:/known_repos.json). This
 * is the pick-list the user adds tracked apps from; it is read-only data baked
 * into the .nro. */
typedef struct {
    char name[64];
    char repo[128];
    char path[512];  /* default_path */
    char asset[96];  /* asset filename/glob */
    char kind[8];    /* asset_kind ("nro" default) */
    char nacp[256];  /* nacp_name aliases, '\n'-separated (for SD-scan match) */
    bool prerelease;
} CatalogEntry;

typedef struct {
    CatalogEntry *items; /* malloc'd, count entries */
    int count;
} Catalog;

/* Load the catalog, preferring the OTA cache on the SD over the bundled romfs
 * copy. Returns true on success; leaves an empty catalog on failure. Caller
 * must catalog_free(). */
bool catalog_load(Catalog *cat);
void catalog_free(Catalog *cat);

/* Download the latest catalog from the repo (CATALOG_URL) into the SD cache.
 * Validates before replacing the cache. Returns: 0=fail, 1=updated,
 * 2=unchanged. Network call — run off the render thread. */
int catalog_update(void);

#ifdef __cplusplus
}
#endif

#endif /* CATALOG_H */
