#ifndef BACKUP_H
#define BACKUP_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-app, per-install backups so any install can be reverted. Each install
 * creates a new timestamped snapshot under backups/<slug>/<id>/: files that
 * already existed are copied in ("overwrite"), new files are noted ("create").
 * Reverting a snapshot restores its overwritten files and deletes its created
 * files. All snapshots are kept (a full history) until the user clears them. A
 * per-snapshot manifest plus a global history log are written. Keyed by repo. */

typedef struct Backup Backup;

/* One snapshot's summary (for the manage-backups list). */
typedef struct {
    char id[32];    /* snapshot dir name (sortable timestamp) */
    char prior[48]; /* version this snapshot restores */
    char newv[48];  /* version it was replaced with */
    char when[24];  /* human time */
    char kind[8];
} BackupInfo;

/* Begin a new snapshot for an app. Returns NULL on failure. */
Backup *backup_start(const char *repo, const char *name, const char *kind,
                     const char *prior_version, const char *new_version);

/* Record a file about to be written at `dest` (absolute sdmc path). Existing
 * files are copied into the snapshot. Call BEFORE writing. */
bool backup_record(Backup *b, const char *dest);

/* Write the manifest + append to the history log, then free. */
bool backup_finish(Backup *b);
/* Discard the in-progress snapshot (removes its dir) and free. */
void backup_cancel(Backup *b);

/* How many snapshots exist for an app. */
int backup_count(const char *repo);
bool backup_exists(const char *repo);
/* Fill `out` with up to `max` snapshots, newest first. Returns the count. */
int backup_list(const char *repo, BackupInfo *out, int max);

/* Restore a specific snapshot (overwrites restored, creates deleted). The
 * snapshot is NOT removed (clearing is manual). Fills prior_version. */
bool backup_revert_id(const char *repo, const char *id, char *prior_version,
                      size_t pv_sz);
/* Delete one snapshot, or all snapshots for an app. */
bool backup_delete_id(const char *repo, const char *id);
bool backup_clear(const char *repo);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_H */
