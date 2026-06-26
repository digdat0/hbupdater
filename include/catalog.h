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

/* Load the bundled catalog. Returns true and fills `cat` on success; on failure
 * leaves an empty (count==0) catalog. Caller must catalog_free(). */
bool catalog_load(Catalog *cat);
void catalog_free(Catalog *cat);

#ifdef __cplusplus
}
#endif

#endif /* CATALOG_H */
