#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_APPS 128

/* Where this app stores its own data on the SD card. */
#define CONFIG_DIR "sdmc:/switch/HBUpdater"
#define APPS_PATH  "sdmc:/switch/HBUpdater/apps.json"
#define LOG_PATH   "sdmc:/switch/HBUpdater/debug.log"
#define DL_TMP_DIR "sdmc:/switch/HBUpdater/downloads"

/* GitHub repo this updater pulls its OWN releases from (for self-update). */
#define UPDATE_REPO "digdat0/hbupdater"
#define DEFAULT_SELF_PATH "sdmc:/switch/HBUpdater/HBUpdater.nro"

/* One tracked homebrew app. */
typedef struct {
    char name[64];     /* display name */
    char repo[128];    /* GitHub "owner/name" */
    char path[512];    /* SD path to the .nro this updater overwrites */
    char version[48];  /* last-installed release tag ("" = unknown) */
} App;

typedef struct {
    App apps[MAX_APPS];
    int count;
} AppsConfig;

/* Load apps.json (seeds an empty file from romfs on first run). Always returns
 * a usable (possibly empty) config. */
void apps_load(AppsConfig *cfg);
bool apps_save(const AppsConfig *cfg);

/* Append a new tracked app. Returns the new App, or NULL if full. */
App *apps_add(AppsConfig *cfg, const char *name, const char *repo,
              const char *path);

/* Remove the app at index idx. Returns true if removed. */
bool apps_remove(AppsConfig *cfg, int idx);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
