#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_APPS 128

/* Where this app stores its own data on the SD card. */
#define CONFIG_DIR    "sdmc:/switch/HBUpdater"
#define APPS_PATH     "sdmc:/switch/HBUpdater/apps.json"
#define EXCLUDES_PATH "sdmc:/switch/HBUpdater/excludes.json"
#define SETTINGS_PATH "sdmc:/switch/HBUpdater/settings.json"
#define LOG_PATH      "sdmc:/switch/HBUpdater/debug.log"
#define UNMATCHED_LOG "sdmc:/switch/HBUpdater/unmatched.log"
#define DL_TMP_DIR    "sdmc:/switch/HBUpdater/downloads"
#define BACKUP_DIR    "sdmc:/switch/HBUpdater/backups"
#define HISTORY_LOG   "sdmc:/switch/HBUpdater/history.log"

/* Root that zip archives are extracted to (Switch homebrew convention: zips
 * carry their own atmosphere/.. , switch/.. paths and extract to the SD root). */
#define SD_ROOT       "sdmc:/"

/* GitHub repo this updater pulls its OWN releases from (for self-update). */
#define UPDATE_REPO "digdat0/hbupdater"
#define DEFAULT_SELF_PATH "sdmc:/switch/HBUpdater/HBUpdater.nro"

/* One tracked homebrew app. */
typedef struct {
    char name[64];     /* display name */
    char repo[128];    /* GitHub "owner/name" */
    char path[512];    /* SD path to the file this updater overwrites */
    char version[48];  /* last-installed release tag ("" = unknown) */
    char latest[48];   /* cached last-seen latest tag ("" = never checked) */
    char checked[16];  /* epoch seconds of last update check ("" = never) */
    char asset[96];    /* release asset filename/glob ("" -> "*.nro") */
    char kind[8];      /* "nro"(default)/"ovl"/"bin"/"zip"/"7z" */
    bool prerelease;   /* consider prereleases when checking */
} App;

typedef struct {
    App apps[MAX_APPS];
    int count;
} AppsConfig;

/* Load apps.json (seeds an empty file from romfs on first run). Always returns
 * a usable (possibly empty) config. */
void apps_load(AppsConfig *cfg);
bool apps_save(const AppsConfig *cfg);

/* Append a new tracked app. asset/kind may be NULL/"" (kind defaults to "nro").
 * Returns the new App, or NULL if full. */
App *apps_add(AppsConfig *cfg, const char *name, const char *repo,
              const char *path, const char *asset, const char *kind,
              bool prerelease);

/* Remove the app at index idx. Returns true if removed. */
bool apps_remove(AppsConfig *cfg, int idx);

/* Find the index of an app by repo (case-insensitive), or -1. */
int apps_find(const AppsConfig *cfg, const char *repo);

/* Opt-out list: repos the user excluded from scanning/updates. */
typedef struct {
    char repos[MAX_APPS][128];
    int count;
} Excludes;

void excludes_load(Excludes *ex);
bool excludes_save(const Excludes *ex);
bool excludes_contains(const Excludes *ex, const char *repo);
void excludes_add(Excludes *ex, const char *repo);
void excludes_remove_at(Excludes *ex, int idx);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
