#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User-configurable behavior, stored at SETTINGS_PATH (hand-editable JSON, or
 * via the in-app Settings screen). Install-class gates default the risky classes
 * OFF: a bad sysmodule/payload can cause boot loops or instability, so the user
 * must opt in. Plain apps (.nro) are always allowed. */
typedef struct {
    bool first_run_done;     /* internal: first-run scan prompt has been shown */
    bool scan_on_launch;     /* opt-in: auto-scan every launch (default false) */
    bool install_overlays;   /* allow installing .ovl overlays (default true) */
    bool install_sysmodules; /* allow atmosphere/contents sysmodules (false) */
    bool install_payloads;   /* allow .bin / bootloader payloads (false) */
    bool test_mode;          /* backups on install + revert/reinstall (false) */
    char github_token[256];  /* optional PAT for 5000/hr API limit ("" = none) */
} Settings;

/* Load settings, applying defaults for any missing file/keys. Writes a default
 * file on first run so the options are discoverable. */
void settings_load(Settings *s);
bool settings_save(const Settings *s);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
