#include <MainApplication.hpp>
#include <pu/ui/extras/extras_Toast.hpp>
#include "version.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

extern "C" {
#include "config.h"
#include "catalog.h"
#include "scan.h"
#include "settings.h"
#include "net.h"
#include "update.h"
#include "fsutil.h"
#include "unzip.h"
#include "backup.h"
#include <switch.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
}

// ---- backend state --------------------------------------------------------
static AppsConfig g_cfg;
static std::vector<std::string> g_status; // per-app display status
static std::vector<std::string> g_latest; // per-app latest tag ("" = unknown)
static std::vector<std::string> g_url;     // per-app latest asset url
static std::vector<uint64_t> g_asset_size; // per-app latest asset size (bytes)
static std::vector<int> g_state;           // 0 none,1 uptodate,2 update,3 fail
static std::string g_launch_path;

static Catalog g_catalog;        // bundled known-apps list (romfs)
// 0=home, 1=catalog, 3=settings, 4=log-picker, 5=log-contents,
// 6=manage-backups, 7=excluded, 8=file-install, 9=advanced, 10=all-backups
static int g_mode = 0;
static s32 g_list_sel = 0;       // remembered tracked-list selection

// One SD-scan result paired with its catalog match.
struct ScanCand {
    std::string name;    // NACP title (or filename fallback)
    std::string version; // NACP display version
    std::string path;    // actual .nro path on SD
    std::string repo;    // matched repo ("" if no catalog match)
    int cat;             // catalog index, or -1
    bool tracked;        // already in the tracked list
};
static std::vector<ScanCand> g_scan; // working buffer for ReconcileInstalled

static std::vector<BackupInfo> g_bklist; // snapshots of the app being managed
static int g_bk_app = -1;                // tracked-app index for the backup view
static int g_bk_from = 0;                // mode to return to from backup view
static void scan_backup_dirs();          // forward decl

static Settings g_settings;          // user prefs (settings.json)
static Excludes g_excludes;          // opt-out list (repos hidden from home)
static bool g_check_stale = false;   // check-all: skip recently-checked apps
static int g_launch_action = 0;      // first frame: 1 = first-run prompt, 2 = auto-scan
static std::string g_pending_toast;  // shown on the first frame after launch
static s32 g_settings_sel = 0;       // remembered settings-screen selection
static s32 g_logmenu_sel = 0;        // remembered log-picker selection

// The three logs surfaced in Settings -> View logs.
static const char *LOG_PATHS[3] = {HISTORY_LOG, UNMATCHED_LOG, LOG_PATH};
static const char *LOG_TITLES[3] = {"Update history", "Unmatched scan",
                                    "Debug log"};

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

// Last-modified time of a file as "YYYY-MM-DD HH:MM", or "" if missing.
static std::string file_mtime_str(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return std::string();
    }
    time_t t = st.st_mtime;
    struct tm *lt = localtime(&t);
    char b[24];
    if (lt && strftime(b, sizeof(b), "%Y-%m-%d %H:%M", lt)) {
        return std::string(b);
    }
    return std::string();
}

// Read the last `cap` bytes of a file (logs are append-only; the tail is what
// matters). If truncated, the first partial line is dropped by the caller.
static std::string read_tail(const char *path, long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return std::string();
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    long off = (n > cap) ? n - cap : 0;
    fseek(f, off, SEEK_SET);
    std::string s;
    s.resize((size_t)(n - off));
    size_t rd = s.empty() ? 0 : fread(&s[0], 1, s.size(), f);
    s.resize(rd);
    fclose(f);
    return s;
}

// Kinds we can install: nro/ovl/bin overwrite a single file; zip extracts to
// the SD root. 7z stays check-only (no extractor).
static bool kind_installable(const char *k) {
    return strcmp(k, "nro") == 0 || strcmp(k, "ovl") == 0 ||
           strcmp(k, "bin") == 0 || strcmp(k, "zip") == 0;
}

// Install-risk class, derived from kind + destination path. Gates dangerous
// installs (sysmodules / boot payloads) behind opt-in settings.
enum { CAT_APP, CAT_OVERLAY, CAT_SYSMODULE, CAT_PAYLOAD };

static int classify_install(const char *kind, const char *path) {
    std::string p;
    for (const char *c = path; *c; c++) {
        p += (char)tolower((unsigned char)*c);
    }
    if (p.find("atmosphere/contents/") != std::string::npos) {
        return CAT_SYSMODULE; // boot-time background service
    }
    if (p.find("bootloader") != std::string::npos ||
        p.find("/payloads/") != std::string::npos || strcmp(kind, "bin") == 0) {
        return CAT_PAYLOAD; // RCM payload / boot chain
    }
    if (strcmp(kind, "ovl") == 0 || p.find(".overlays") != std::string::npos) {
        return CAT_OVERLAY;
    }
    return CAT_APP;
}

static const char *category_name(int cat) {
    switch (cat) {
    case CAT_OVERLAY:   return "overlay";
    case CAT_SYSMODULE: return "sysmodule";
    case CAT_PAYLOAD:   return "payload";
    default:            return "app";
    }
}

static bool category_allowed(int cat) {
    switch (cat) {
    case CAT_OVERLAY:   return g_settings.install_overlays;
    case CAT_SYSMODULE: return g_settings.install_sysmodules;
    case CAT_PAYLOAD:   return g_settings.install_payloads;
    default:            return true; // plain apps always allowed
    }
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static long file_size(const char *path); // defined in the log-viewer section

// True if `title` exactly matches one of the '\n'-separated aliases (ci).
static bool title_in_aliases(const char *aliases, const char *title) {
    if (!title[0]) {
        return false;
    }
    const char *p = aliases;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && strncasecmp(p, title, len) == 0 && title[len] == '\0') {
            return true;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return false;
}

// Match a scanned .nro to a catalog entry: by NACP title/alias first, then by
// the .nro filename vs the catalog default_path basename. Returns index or -1.
static int catalog_match(const Catalog *cat, const char *title,
                         const char *path) {
    for (int i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];
        if (title_in_aliases(e->nacp, title) ||
            (title[0] && strcasecmp(e->name, title) == 0)) {
            return i;
        }
    }
    const char *fn = basename_of(path);
    for (int i = 0; i < cat->count; i++) {
        if (strcasecmp(basename_of(cat->items[i].path), fn) == 0) {
            return i;
        }
    }
    return -1;
}

// ---- background worker ----------------------------------------------------
// Network I/O blocks for seconds, so it runs on a worker thread; the main
// (render) thread polls in Tick() and does all UI + file installs itself, since
// Plutonium rendering is not thread-safe. Result vectors are written only by the
// worker and read only by the main thread *after* threadWaitForExit (the join
// establishes happens-before), so no lock is needed beyond g_dl_prog/g_thr_done.
enum { JOB_NONE, JOB_CHECK_ONE, JOB_CHECK_ALL, JOB_DOWNLOAD, JOB_REVERT,
       JOB_CATALOG, JOB_SELFCHECK, JOB_SELFINSTALL };
static Thread g_thr;
static bool g_thr_active = false;        // a thread exists, awaiting join
static volatile bool g_thr_done = false; // worker has finished
static bool g_busy = false;              // a job is in flight (gates input)
static int g_job = JOB_NONE;
static int g_job_idx = -1;
static bool g_offer_update = false;      // after a single check, prompt to update
static volatile int g_check_done = 0;    // check-all progress
static volatile int g_check_total = 0;
static volatile float g_dl_prog = -1.0f; // 0..1 while downloading, <0 = idle
static bool g_dl_ok = false;             // download succeeded
static bool g_install_ok = false;        // move/extract/revert succeeded (worker)
static int g_phase = 0;                  // 0 = downloading, 1 = extracting
static bool g_force = false;             // reinstall even if up to date
static char g_revert_prior[48];          // version restored by a revert job
static char g_revert_id[40];             // which snapshot a revert job restores
static char g_fail_msg[40];              // worker-set failure reason ("" = none)
static bool g_catalog_ok = false;        // JOB_CATALOG result
static char g_self_tag[64];              // self-update: latest HBUpdater tag
static std::string g_self_url;           // self-update: .nro asset url
static uint64_t g_self_size = 0;         // self-update: expected asset size
static bool g_self_ok = false;           // JOB_SELFINSTALL result
static std::string g_dl_tmp;             // tmp file the download produced

// Set g_latest/g_state/g_status for one app from a latest tag. Shared by the
// live check and the cached-result restore on launch.
// version_cmp parses dotted ints, which collapses non-semver tags (rc4/rc5,
// dates, hashes). Guard: only "up to date" when the comparator says the
// installed tag is >= latest AND the tag strings actually match.
static void apply_version_state(int idx, const char *tag) {
    g_latest[idx] = tag;
    const char *inst = g_cfg.apps[idx].version;
    int c = inst[0] ? version_cmp(inst, tag) : -1;
    bool uptodate = inst[0] && (c > 0 || (c == 0 && strcmp(inst, tag) == 0));
    if (uptodate) {
        g_state[idx] = 1;
        g_status[idx] = std::string("up to date ") + tag;
    } else {
        g_state[idx] = 2;
        g_status[idx] = (inst[0] ? std::string(inst) + " -> " : std::string()) +
                        tag;
    }
}

// Network-only; MUST NOT touch Plutonium/UI. Runs on the worker thread.
static void job_check(int idx) {
    if (idx < 0 || idx >= g_cfg.count) {
        return;
    }
    char tag[64] = {0}, url[1024] = {0};
    uint64_t size = 0;
    if (!update_fetch_latest(g_cfg.apps[idx].repo, g_cfg.apps[idx].asset,
                             g_cfg.apps[idx].prerelease, tag, sizeof(tag), url,
                             sizeof(url), &size)) {
        g_state[idx] = 3;
        g_status[idx] = "check failed";
        g_latest[idx].clear();
        g_url[idx].clear();
        g_asset_size[idx] = 0;
        return;
    }
    g_url[idx] = url;
    g_asset_size[idx] = size;
    apply_version_state(idx, tag);
    snprintf(g_cfg.apps[idx].checked, sizeof(g_cfg.apps[idx].checked), "%lld",
             (long long)time(NULL));
}

// An app is "fresh" if it has a cached result checked within the last 6h. The
// launch auto-check skips fresh apps to stay under GitHub's rate limit.
static bool app_is_fresh(int idx) {
    const char *ck = g_cfg.apps[idx].checked;
    if (!ck[0] || !g_cfg.apps[idx].latest[0]) {
        return false;
    }
    long long now = (long long)time(NULL);
    return (now - atoll(ck)) < (6 * 3600);
}

// Size the per-app state vectors to the config and restore cached check results
// (latest tags persisted in apps.json) so update flags show without a network
// round-trip. Main-thread only.
static void seed_states_from_cache() {
    g_status.assign(g_cfg.count, std::string());
    g_latest.assign(g_cfg.count, std::string());
    g_url.assign(g_cfg.count, std::string());
    g_asset_size.assign(g_cfg.count, 0);
    g_state.assign(g_cfg.count, 0);
    for (int i = 0; i < g_cfg.count; i++) {
        if (g_cfg.apps[i].latest[0]) {
            apply_version_state(i, g_cfg.apps[i].latest);
        }
    }
}

// Persist the latest tags discovered by a check back into apps.json.
static void cache_writeback() {
    int n = g_cfg.count < (int)g_latest.size() ? g_cfg.count
                                               : (int)g_latest.size();
    for (int i = 0; i < n; i++) {
        snprintf(g_cfg.apps[i].latest, sizeof(g_cfg.apps[i].latest), "%s",
                 g_latest[i].c_str());
    }
    apps_save(&g_cfg);
}

static int dl_progress(void *, uint64_t now, uint64_t total) {
    g_dl_prog = (total > 0) ? (float)((double)now / (double)total) : 0.0f;
    return 0;
}

static int extract_progress(void *, int done, int total) {
    g_dl_prog = (total > 0) ? (float)done / (float)total : 0.0f;
    return 0;
}

// Pre-write hook for zip extraction: back up each file before it's overwritten.
static int backup_prewrite(void *ud, const char *dest) {
    Backup *b = (Backup *)ud;
    return backup_record(b, dest) ? 0 : 1; // non-zero aborts extraction
}

// Integrity check before install: the downloaded file must match the expected
// asset size (catches truncated downloads / HTML error pages) and, for nro/zip,
// the expected magic bytes. ovl/bin/7z are size-checked only.
static bool validate_download(const char *path, const char *kind,
                              uint64_t expected) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || (expected > 0 && (uint64_t)sz != expected)) {
        fclose(f);
        return false;
    }
    bool ok = true;
    if (strcmp(kind, "nro") == 0) {
        unsigned char m[4];
        if (fseek(f, 0x10, SEEK_SET) != 0 || fread(m, 1, 4, f) != 4 ||
            memcmp(m, "NRO0", 4) != 0) {
            ok = false;
        }
    } else if (strcmp(kind, "zip") == 0) {
        unsigned char m[2];
        if (fread(m, 1, 2, f) != 2 || m[0] != 'P' || m[1] != 'K') {
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

// Download the asset, verify it, then install: single-file kinds (nro/ovl/bin)
// move into place; zip extracts to the SD root. Every touched file is backed up
// first (manifest + history) so the install can be reverted. Worker only.
static void job_download(int idx) {
    g_dl_ok = false;
    g_install_ok = false;
    g_phase = 0;
    g_fail_msg[0] = '\0';
    if (idx < 0 || idx >= g_cfg.count || g_url[idx].empty()) {
        return;
    }
    const char *kind = g_cfg.apps[idx].kind[0] ? g_cfg.apps[idx].kind : "nro";
    uint64_t expect = g_asset_size[idx];
    net_log("INSTALL '%s' [%s] -> %s expect=%llu", g_cfg.apps[idx].name, kind,
            g_cfg.apps[idx].path, (unsigned long long)expect);

    // Free-space guard (download + extraction + backup headroom).
    if (expect) {
        uint64_t freeb = fs_free_bytes("sdmc:/");
        if (freeb != UINT64_MAX && freeb < expect + (64ULL << 20)) {
            net_log("  space: need %llu, free %llu -> abort",
                    (unsigned long long)(expect + (64ULL << 20)),
                    (unsigned long long)freeb);
            snprintf(g_fail_msg, sizeof(g_fail_msg), "not enough space");
            return;
        }
    }

    fs_mkdir_p(DL_TMP_DIR);
    g_dl_tmp = std::string(DL_TMP_DIR) + "/update.tmp";
    g_dl_prog = 0.0f;
    long code = 0;
    g_dl_ok = http_download(g_url[idx].c_str(), g_dl_tmp.c_str(), NULL,
                            dl_progress, NULL, 0, &code);
    long got = file_size(g_dl_tmp.c_str());
    net_log("  download -> %s (http=%ld, %ld bytes)", g_dl_ok ? "ok" : "FAIL",
            code, got);
    if (!g_dl_ok) {
        g_dl_prog = -1.0f;
        return;
    }
    if (!validate_download(g_dl_tmp.c_str(), kind, expect)) {
        net_log("  validate -> FAIL (got %ld, expect %llu)", got,
                (unsigned long long)expect);
        remove(g_dl_tmp.c_str());
        g_dl_ok = false;
        snprintf(g_fail_msg, sizeof(g_fail_msg), "bad download (size/format)");
        g_dl_prog = -1.0f;
        return;
    }
    net_log("  validate -> ok");

    Backup *b = nullptr;
    if (g_settings.auto_backup) {
        b = backup_start(g_cfg.apps[idx].repo, g_cfg.apps[idx].name, kind,
                         g_cfg.apps[idx].version, g_latest[idx].c_str());
    }
    if (strcmp(kind, "zip") == 0) {
        g_phase = 1;
        g_dl_prog = 0.0f;
        int n = 0;
        g_install_ok = unzip_extract(g_dl_tmp.c_str(), SD_ROOT, extract_progress,
                                     NULL, b ? backup_prewrite : NULL, b, &n);
        remove(g_dl_tmp.c_str());
        net_log("  extract -> %s (%d files to %s)",
                g_install_ok ? "ok" : "FAIL", n, SD_ROOT);
    } else {
        if (b) {
            backup_record(b, g_cfg.apps[idx].path);
        }
        g_install_ok = fs_move(g_dl_tmp.c_str(), g_cfg.apps[idx].path);
        if (!g_install_ok) {
            remove(g_dl_tmp.c_str());
        }
        net_log("  move -> %s (%s)", g_install_ok ? "ok" : "FAIL",
                g_cfg.apps[idx].path);
    }
    if (b) {
        backup_finish(b); // kept even on failure, so partial installs revert
    }
    if (!g_install_ok) {
        snprintf(g_fail_msg, sizeof(g_fail_msg), "install failed");
    }
    g_dl_prog = -1.0f;
}

// Fetch the latest catalog from the repo into the SD cache. Worker only.
static void job_catalog(void) { g_catalog_ok = catalog_update(); }

// Check HBUpdater's own repo for a newer release. Worker only.
static void job_selfcheck(void) {
    g_self_tag[0] = '\0';
    g_self_url.clear();
    g_self_size = 0;
    char tag[64] = {0}, url[1024] = {0};
    uint64_t size = 0;
    if (update_fetch_latest(UPDATE_REPO, "HBUpdater.nro", false, tag, sizeof(tag),
                            url, sizeof(url), &size)) {
        snprintf(g_self_tag, sizeof(g_self_tag), "%s", tag);
        g_self_url = url;
        g_self_size = size;
        net_log("SELF check: latest=%s size=%llu (running %s)", tag,
                (unsigned long long)size, APP_VERSION_STR);
    } else {
        net_log("SELF check: fetch FAILED (running %s)", APP_VERSION_STR);
    }
}

// Overwrite dst with src's bytes via fopen "wb" (truncate IN PLACE, no remove).
// The running .nro can't be deleted-then-recreated, but it can be truncated and
// rewritten, so this avoids fs_move's remove(dst) step.
static bool self_overwrite(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    net_log("SELF write: in=%d out=%d (%s)", in ? 1 : 0, out ? 1 : 0, dst);
    if (!in || !out) {
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
        return false;
    }
    char buf[65536];
    size_t r;
    bool ok = true;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) {
            ok = false;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = false; // surface write/flush errors
    }
    return ok;
}

// The app's own .nro path. argv[0] may lack the "sdmc:" device prefix; add it,
// or fall back to the canonical install path.
static std::string resolve_self_path() {
    if (g_launch_path.rfind("sdmc:/", 0) == 0) {
        return g_launch_path;
    }
    if (!g_launch_path.empty() && g_launch_path[0] == '/') {
        return std::string("sdmc:") + g_launch_path;
    }
    return std::string(DEFAULT_SELF_PATH);
}

// Apply a pending staged self-update (written on a previous run) if the .nro is
// now writable. The running .nro is locked, so the very start of launch is the
// only window. Returns true if it applied AND queued a chainload of the new nro
// (the caller should then exit so hbloader runs the new version this launch).
static bool apply_staged_self_update() {
    std::string self = resolve_self_path();
    std::string staged = self + ".new";
    if (!fs_exists(staged.c_str())) {
        return false;
    }
    bool ok = self_overwrite(staged.c_str(), self.c_str());
    net_log("STARTUP apply: '%s' -> '%s' ok=%d", staged.c_str(), self.c_str(),
            ok ? 1 : 0);
    if (!ok) {
        return false; // locked: leave the .new for a retry or a manual swap
    }
    remove(staged.c_str());
    if (envHasNextLoad()) {
        envSetNextLoad(self.c_str(), self.c_str()); // chainload the new nro now
        return true;
    }
    return false; // applied; runs old this launch, new next launch
}

bool MainApplication::ApplyPendingUpdate() { return apply_staged_self_update(); }

// Download the new HBUpdater.nro and STAGE it beside the running one (applied at
// next launch by apply_staged_self_update). Worker only.
static void job_selfinstall(void) {
    g_self_ok = false;
    g_fail_msg[0] = '\0';
    if (g_self_url.empty()) {
        return;
    }
    fs_mkdir_p(DL_TMP_DIR);
    std::string tmp = std::string(DL_TMP_DIR) + "/self.nro";
    g_dl_prog = 0.0f;
    long code = 0;
    bool dok = http_download(g_self_url.c_str(), tmp.c_str(), NULL, dl_progress,
                             NULL, 0, &code);
    long got = file_size(tmp.c_str());
    net_log("SELF install: download -> %s (http=%ld, %ld bytes, expect %llu)",
            dok ? "ok" : "FAIL", code, got, (unsigned long long)g_self_size);
    if (!dok) {
        remove(tmp.c_str());
        snprintf(g_fail_msg, sizeof(g_fail_msg), "download failed");
        g_dl_prog = -1.0f;
        return;
    }
    if (!validate_download(tmp.c_str(), "nro", g_self_size)) {
        net_log("SELF install: validate -> FAIL");
        remove(tmp.c_str());
        snprintf(g_fail_msg, sizeof(g_fail_msg), "bad download (size/format)");
        g_dl_prog = -1.0f;
        return;
    }
    // Resolve the target .nro path. argv[0] may arrive without the "sdmc:" device
    // prefix our fs layer needs (the cause of "app update failed"); add it, or
    // fall back to the canonical install path.
    // Stage the validated nro beside the target (a NEW file, so writable). The
    // running .nro is locked; it gets replaced from the staged copy at the next
    // launch (apply_staged_self_update), or the user can swap it manually.
    std::string self = resolve_self_path();
    std::string staged = self + ".new";
    g_self_ok = self_overwrite(tmp.c_str(), staged.c_str());
    net_log("SELF install: staged='%s' ok=%d", staged.c_str(),
            g_self_ok ? 1 : 0);
    remove(tmp.c_str());
    if (!g_self_ok) {
        snprintf(g_fail_msg, sizeof(g_fail_msg), "staging failed");
    }
    g_dl_prog = -1.0f;
}

// Restore a specific backup snapshot (g_revert_id). Worker only.
static void job_revert(int idx) {
    g_install_ok = false;
    g_revert_prior[0] = '\0';
    if (idx < 0 || idx >= g_cfg.count) {
        return;
    }
    g_install_ok = backup_revert_id(g_cfg.apps[idx].repo, g_revert_id,
                                    g_revert_prior, sizeof(g_revert_prior));
}

static void worker_main(void *) {
    switch (g_job) {
    case JOB_CHECK_ONE:
        job_check(g_job_idx);
        break;
    case JOB_CHECK_ALL:
        g_check_total = g_cfg.count;
        g_check_done = 0;
        for (int i = 0; i < g_cfg.count; i++) {
            if (!g_cfg.apps[i].pinned &&
                !(g_check_stale && app_is_fresh(i))) {
                job_check(i);
            }
            g_check_done = i + 1;
        }
        break;
    case JOB_DOWNLOAD:
        job_download(g_job_idx);
        break;
    case JOB_REVERT:
        job_revert(g_job_idx);
        break;
    case JOB_CATALOG:
        job_catalog();
        break;
    case JOB_SELFCHECK:
        job_selfcheck();
        break;
    case JOB_SELFINSTALL:
        job_selfinstall();
        break;
    default:
        break;
    }
    __sync_synchronize();
    g_thr_done = true;
}

// Spawn the worker for one job. Returns false if one is already running.
static bool start_job(int job, int idx) {
    if (g_busy) {
        return false;
    }
    g_job = job;
    g_job_idx = idx;
    g_thr_done = false;
    g_dl_prog = -1.0f;
    // 512KB stack: curl + the libnx ssl/TLS path are very stack-hungry, and a
    // check-all fires many handshakes back-to-back. 256KB was marginal and could
    // overflow into a hard crash. Default prio/core.
    if (R_FAILED(threadCreate(&g_thr, worker_main, NULL, NULL, 0x80000, 0x2C,
                              -2))) {
        return false;
    }
    if (R_FAILED(threadStart(&g_thr))) {
        threadClose(&g_thr);
        return false;
    }
    g_thr_active = true;
    g_busy = true;
    return true;
}

// ---- helpers --------------------------------------------------------------
static pu::ui::Color state_color(int st) {
    switch (st) {
    case 1: return pu::ui::Color(130, 225, 150, 255); // up to date (green)
    case 2: return pu::ui::Color(240, 210, 120, 255); // update available (amber)
    case 3: return pu::ui::Color(240, 110, 110, 255); // failed (red)
    default: return pu::ui::Color(170, 175, 185, 255); // not checked (gray)
    }
}

// ---- MainLayout -----------------------------------------------------------
MainLayout::MainLayout() : Layout::Layout() {
    this->SetBackgroundColor(pu::ui::Color(12, 12, 14, 255));
    const s32 sw = (s32)pu::ui::render::ScreenWidth;
    const s32 sh = (s32)pu::ui::render::ScreenHeight;

    this->header = pu::ui::elm::Rectangle::New(0, 0, sw, 110,
                                               pu::ui::Color(33, 64, 124, 255));
    this->Add(this->header);
    // Brand (always shown, far left).
    this->product = pu::ui::elm::TextBlock::New(45, 32, "hbUpdater");
    this->product->SetColor(pu::ui::Color(255, 255, 255, 255));
    this->Add(this->product);
    // Page name as a breadcrumb just right of the brand: "hbUpdater > Page".
    s32 pw = this->product->GetWidth();
    s32 tx = (pw > 0) ? (45 + pw + 22) : 250;
    this->title = pu::ui::elm::TextBlock::New(tx, 32, "");
    this->title->SetColor(pu::ui::Color(165, 185, 220, 255));
    this->Add(this->title);
    // Status, right-aligned in SetStatus so it never runs off-screen.
    this->status = pu::ui::elm::TextBlock::New(sw - 360, 38, "");
    this->status->SetColor(pu::ui::Color(210, 222, 245, 255));
    this->Add(this->status);

    const s32 footer_h = 64;
    const s32 list_y = 118;
    const s32 row_h = 84;
    const s32 rows = (sh - list_y - footer_h) / row_h;
    this->list = TableList::New(0, list_y, sw, row_h, rows);
    this->Add(this->list);

    s32 list_bottom = list_y + rows * row_h;
    s32 footer_top = sh - footer_h;
    if (list_bottom < footer_top) {
        auto fill = pu::ui::elm::Rectangle::New(0, list_bottom, sw,
                        footer_top - list_bottom, pu::ui::Color(22, 23, 27, 255));
        this->Add(fill);
    }

    this->footer = pu::ui::elm::Rectangle::New(0, sh - footer_h, sw, footer_h,
                                               pu::ui::Color(22, 42, 80, 255));
    this->Add(this->footer);
    for (int i = 0; i < 8; i++) {
        auto seg = pu::ui::elm::TextBlock::New(0, sh - footer_h + 14, "");
        seg->SetColor(pu::ui::Color(206, 216, 238, 255));
        this->Add(seg);
        this->footer_segs.push_back(seg);
    }
}

void MainLayout::SetTitle(const std::string &t) {
    // Breadcrumb separator after the brand.
    this->title->SetText(t.empty() ? std::string() : std::string("> ") + t);
}
void MainLayout::SetStatus(const std::string &t) {
    this->status->SetText(t);
    // Pin the right edge so long status text grows leftward, never off-screen.
    const s32 sw = (s32)pu::ui::render::ScreenWidth;
    this->status->SetX(sw - 30 - this->status->GetWidth());
}
void MainLayout::SetFooter(const std::string &t) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && t[i] == ' ') {
            i++;
        }
        if (i >= t.size()) {
            break;
        }
        size_t end = t.size();
        for (size_t j = i; j + 1 < t.size(); j++) {
            if (t[j] == ' ' && t[j + 1] == ' ') {
                end = j;
                break;
            }
        }
        segs.push_back(t.substr(i, end - i));
        i = end;
    }
    const s32 sw = (s32)pu::ui::render::ScreenWidth;
    const s32 margin = 30;
    int n = (int)segs.size();
    for (int k = 0; k < (int)this->footer_segs.size(); k++) {
        if (k < n) {
            this->footer_segs[k]->SetText(segs[k]);
            s32 cell = (sw - 2 * margin) / (n > 0 ? n : 1);
            s32 center = margin + cell * k + cell / 2;
            this->footer_segs[k]->SetX(center -
                                       this->footer_segs[k]->GetWidth() / 2);
        } else {
            this->footer_segs[k]->SetText("");
        }
    }
}
void MainLayout::ClearList() { this->list->Clear(); }
void MainLayout::SetColumns(const std::string &a, const std::string &b,
                            const std::string &c) {
    this->list->SetHeaders(a, b, c);
}
void MainLayout::ClearColumns() { this->list->ClearHeaders(); }
void MainLayout::AddRow(const std::string &left, const std::string &right,
                        pu::ui::Color lclr, pu::ui::Color rclr) {
    this->list->AddRow2(left, right, lclr, rclr);
}
void MainLayout::AddRow3(const std::string &name, const std::string &ver,
                         const std::string &status, pu::ui::Color nclr,
                         pu::ui::Color vclr, pu::ui::Color sclr) {
    this->list->AddRow3(name, ver, status, nclr, vclr, sclr);
}
s32 MainLayout::Sel() { return this->list->GetSelected(); }
void MainLayout::SetSel(s32 i) { this->list->SetSelected(i); }
s32 MainLayout::Count() { return this->list->Count(); }
void MainLayout::Step(s32 d) { this->list->Step(d); }
void MainLayout::PageUp() { this->list->MoveBy(-this->list->RowsVisible()); }
void MainLayout::PageDown() { this->list->MoveBy(this->list->RowsVisible()); }

// ---- MainApplication ------------------------------------------------------
void MainApplication::SetLaunchPath(const std::string &p) { g_launch_path = p; }

static MainLayout::Ref g_layout;

void MainApplication::Toast(const std::string &msg) {
    auto tb = pu::ui::elm::TextBlock::New(0, 0, msg);
    tb->SetColor(pu::ui::Color(255, 255, 255, 255));
    auto t = pu::ui::extras::Toast::New(tb, pu::ui::Color(46, 120, 78, 240));
    this->StartOverlayWithTimeout(t, 1500);
}
void MainApplication::ToastErr(const std::string &msg) {
    auto tb = pu::ui::elm::TextBlock::New(0, 0, msg);
    tb->SetColor(pu::ui::Color(255, 255, 255, 255));
    auto t = pu::ui::extras::Toast::New(tb, pu::ui::Color(160, 52, 52, 240));
    this->StartOverlayWithTimeout(t, 1800);
}
bool MainApplication::Confirm(const std::string &title, const std::string &msg) {
    int r = this->CreateShowDialog(title, msg, {"Cancel", "Yes"}, false);
    return r == 1;
}

void MainApplication::Refresh() {
    g_status.resize(g_cfg.count);
    g_latest.resize(g_cfg.count);
    g_url.resize(g_cfg.count);
    g_asset_size.resize(g_cfg.count);
    g_state.resize(g_cfg.count);

    g_layout->SetTitle("My Apps");
    int outdated = 0;
    for (int i = 0; i < g_cfg.count; i++) {
        if (g_state[i] == 2) {
            outdated++;
        }
    }
    int rate = net_rate_remaining();
    char st[96];
    if (outdated > 0 && rate >= 0) {
        snprintf(st, sizeof(st), "%d app%s  %d update%s  API:%d", g_cfg.count,
                 g_cfg.count == 1 ? "" : "s", outdated, outdated == 1 ? "" : "s",
                 rate);
    } else if (outdated > 0) {
        snprintf(st, sizeof(st), "%d app%s  %d update%s", g_cfg.count,
                 g_cfg.count == 1 ? "" : "s", outdated, outdated == 1 ? "" : "s");
    } else if (rate >= 0) {
        snprintf(st, sizeof(st), "%d app%s  API:%d", g_cfg.count,
                 g_cfg.count == 1 ? "" : "s", rate);
    } else {
        snprintf(st, sizeof(st), "%d app%s", g_cfg.count,
                 g_cfg.count == 1 ? "" : "s");
    }
    g_layout->SetStatus(st);
    g_layout->SetFooter("A open  X check all  R settings  "
                        "- exclude  ZL/ZR page  + exit");
    g_layout->SetColumns("Name", "Installed", "Update");

    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    const pu::ui::Color pin_clr(180, 160, 220, 255);
    for (int i = 0; i < g_cfg.count; i++) {
        const char *iv = g_cfg.apps[i].version;
        std::string ver = iv[0] ? iv : "-";
        std::string status = g_status[i];
        if (g_cfg.apps[i].pinned) {
            status = "pinned";
        } else if (status.empty()) {
            status = iv[0] ? "" : "not checked";
        }
        pu::ui::Color sc = g_cfg.apps[i].pinned ? pin_clr
                                                 : state_color(g_state[i]);
        g_layout->AddRow3(g_cfg.apps[i].name, ver, status, name_clr, dim_clr,
                          sc);
    }
    if (g_cfg.count == 0) {
        g_layout->AddRow3("(no recognized apps - update catalog in settings)",
                          "", "", dim_clr, dim_clr, dim_clr);
    }
    g_layout->SetSel(keep);
}

void MainApplication::CheckAll() {
    if (g_cfg.count == 0) {
        return;
    }
    if (!start_job(JOB_CHECK_ALL, -1)) {
        this->ToastErr("Busy");
    }
}

void MainApplication::StartCheck(int idx, bool offer_update) {
    if (idx < 0 || idx >= g_cfg.count) {
        return;
    }
    g_offer_update = offer_update;
    if (!start_job(JOB_CHECK_ONE, idx)) {
        this->ToastErr("Busy");
    }
}

// Per-frame poll. While a job runs, shows live status; on completion it joins
// the worker and performs all UI / dialogs / file installs on the main thread.
void MainApplication::Tick() {
    if (!g_pending_toast.empty()) {
        this->Toast(g_pending_toast);
        g_pending_toast.clear();
    }
    if (g_launch_action && !g_busy) {
        int a = g_launch_action;
        g_launch_action = 0;
        if (a == 1) {
            if (g_cfg.count > 0 &&
                this->Confirm("Welcome",
                              "Check your installed apps for updates now?")) {
                this->CheckAll();
            }
        } else if (a == 2) {
            if (g_cfg.count > 0)
                this->CheckAll();
        }
        return;
    }
    if (!g_thr_active) {
        return;
    }
    if (!g_thr_done) {
        if (g_job == JOB_DOWNLOAD) {
            float p = g_dl_prog;
            const char *verb = (g_phase == 1) ? "extracting" : "downloading";
            char s[48];
            if (p >= 0.0f) {
                snprintf(s, sizeof(s), "%s %d%%", verb, (int)(p * 100));
            } else {
                snprintf(s, sizeof(s), "%s...", verb);
            }
            g_layout->SetStatus(s);
        } else if (g_job == JOB_REVERT) {
            g_layout->SetStatus("reverting...");
        } else if (g_job == JOB_CATALOG) {
            g_layout->SetStatus("updating catalog...");
        } else if (g_job == JOB_SELFCHECK) {
            g_layout->SetStatus("checking for app update...");
        } else if (g_job == JOB_SELFINSTALL) {
            float p = g_dl_prog;
            char s[48];
            if (p >= 0.0f) {
                snprintf(s, sizeof(s), "updating app %d%%", (int)(p * 100));
            } else {
                snprintf(s, sizeof(s), "updating app...");
            }
            g_layout->SetStatus(s);
        } else if (g_job == JOB_CHECK_ALL) {
            char s[48];
            snprintf(s, sizeof(s), "Checking %d / %d ...", g_check_done,
                     g_check_total);
            g_layout->SetTitle(s); // prominent: it's clearly progressing
            g_layout->SetStatus("");
        } else {
            g_layout->SetStatus("checking...");
        }
        return;
    }

    // Job finished: join, then handle the result on the main thread.
    threadWaitForExit(&g_thr);
    threadClose(&g_thr);
    g_thr_active = false;
    g_busy = false;
    int job = g_job, idx = g_job_idx;
    g_job = JOB_NONE;

    if (job == JOB_CHECK_ALL) {
        cache_writeback(); // persist latest tags so flags survive a relaunch
        this->Refresh();
        this->Toast("Checked all");
        return;
    }
    if (job == JOB_CHECK_ONE) {
        cache_writeback();
        this->Refresh();
        if (!g_offer_update) {
            return;
        }
        g_offer_update = false;
        bool force = g_force;
        g_force = false;
        if (idx < 0 || idx >= g_cfg.count) {
            return;
        }
        bool have_update = (g_state[idx] == 2);
        bool have_tag = !g_latest[idx].empty();
        if (have_update || (force && have_tag)) {
            const char *kind =
                g_cfg.apps[idx].kind[0] ? g_cfg.apps[idx].kind : "nro";
            if (!kind_installable(kind)) {
                this->ToastErr(std::string(kind) + ": no auto-install yet");
                return;
            }
            int cat = classify_install(kind, g_cfg.apps[idx].path);
            if (!category_allowed(cat)) {
                this->ToastErr(std::string(category_name(cat)) +
                               " installs disabled (R: settings)");
                return;
            }
            const char *verb = have_update ? "Update " : "Reinstall ";
            std::string msg = std::string(verb) + g_cfg.apps[idx].name + " to " +
                              g_latest[idx] + "?";
            if (strcmp(kind, "zip") == 0) {
                msg += "\n(extracts to SD root)";
            }
            if (this->Confirm(have_update ? "Update" : "Reinstall", msg)) {
                if (!start_job(JOB_DOWNLOAD, idx)) {
                    this->ToastErr("Busy");
                }
            }
        } else if (g_state[idx] == 3) {
            this->ToastErr("Check failed");
        } else {
            this->Toast("Already up to date");
        }
        return;
    }
    if (job == JOB_DOWNLOAD) {
        // The worker already downloaded + installed (moved or extracted).
        if (idx >= 0 && idx < g_cfg.count && g_dl_ok && g_install_ok) {
            snprintf(g_cfg.apps[idx].version, sizeof(g_cfg.apps[idx].version),
                     "%s", g_latest[idx].c_str());
            apps_save(&g_cfg);
            g_state[idx] = 1;
            g_status[idx] = std::string("updated to ") + g_latest[idx];
            this->Refresh();
            this->Toast("Updated");
        } else {
            const char *why = g_fail_msg[0]
                                  ? g_fail_msg
                                  : (g_dl_ok ? "install failed" : "download failed");
            if (idx >= 0 && idx < g_cfg.count) {
                g_state[idx] = 3;
                g_status[idx] = why;
            }
            this->Refresh();
            this->ToastErr(why);
        }
        return;
    }
    if (job == JOB_REVERT) {
        if (idx >= 0 && idx < g_cfg.count && g_install_ok) {
            snprintf(g_cfg.apps[idx].version, sizeof(g_cfg.apps[idx].version),
                     "%s", g_revert_prior);
            g_cfg.apps[idx].latest[0] = '\0'; // drop cache -> recheck cleanly
            apps_save(&g_cfg);
            g_latest[idx].clear();
            g_state[idx] = 0;
            g_status[idx] = g_revert_prior[0]
                                ? std::string("reverted to ") + g_revert_prior
                                : std::string("reverted");
            this->Toast("Reverted");
        } else {
            this->ToastErr("Revert failed");
        }
        // Revert is launched from the backup screen; return to the tracked list.
        g_mode = 0;
        this->Refresh();
        g_layout->SetSel(g_list_sel);
        return;
    }
    if (job == JOB_CATALOG) {
        if (g_catalog_ok) {
            catalog_free(&g_catalog);
            catalog_load(&g_catalog);
            this->ReconcileInstalled();
            char m[48];
            snprintf(m, sizeof(m), "Catalog updated: %d apps", g_catalog.count);
            this->Toast(m);
        } else {
            this->ToastErr("Catalog update failed");
        }
        if (g_mode == 3) {
            this->RefreshSettings();
        }
        return;
    }
    if (job == JOB_SELFCHECK) {
        if (g_self_tag[0]) {
            // APP_VERSION_STR vs latest tag (version_cmp tolerates a 'v' prefix).
            if (version_cmp(APP_VERSION_STR, g_self_tag) < 0) {
                if (this->Confirm("Update HBUpdater",
                                  std::string("v") + APP_VERSION_STR + "  ->  " +
                                      g_self_tag + "\nDownload and install?")) {
                    if (!start_job(JOB_SELFINSTALL, -1)) {
                        this->ToastErr("Busy");
                    }
                }
            } else {
                this->Toast(std::string("HBUpdater is up to date (v") +
                            APP_VERSION_STR + ")");
            }
        } else {
            this->ToastErr("Update check failed");
        }
        if (g_mode == 3) {
            this->RefreshSettings();
        }
        return;
    }
    if (job == JOB_SELFINSTALL) {
        if (g_self_ok) {
            this->Toast("Update staged - restart to apply");
        } else {
            this->ToastErr(g_fail_msg[0] ? g_fail_msg : "App update failed");
        }
        if (g_mode == 3) {
            this->RefreshSettings();
        }
        return;
    }
}

// ---- supported-apps browse (read-only) ------------------------------------
void MainApplication::OpenCatalog() {
    if (g_catalog.count == 0) {
        this->ToastErr("Catalog unavailable");
        return;
    }
    g_settings_sel = g_layout->Sel();
    g_mode = 1;
    g_layout->SetSel(0);
    this->RefreshCatalog();
}

void MainApplication::CloseCatalog() {
    g_mode = 3; // back to Settings (this is now "Supported apps")
    this->RefreshSettings();
    g_layout->SetSel(g_settings_sel);
}

void MainApplication::RefreshCatalog() {
    std::string mt = file_mtime_str(CATALOG_CACHE);
    char st[96];
    if (mt.empty()) {
        snprintf(st, sizeof(st), "%d records · bundled", g_catalog.count);
    } else {
        snprintf(st, sizeof(st), "%d records · OTA %s", g_catalog.count,
                 mt.c_str());
    }
    g_layout->SetTitle("Supported apps");
    g_layout->SetStatus(st);
    g_layout->SetFooter("A info  B back  ZL/ZR page  + exit");
    g_layout->SetColumns("Name", "Kind", "Status");

    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    for (int i = 0; i < g_catalog.count; i++) {
        const CatalogEntry *e = &g_catalog.items[i];
        bool tracked = false;
        for (int j = 0; j < g_cfg.count; j++) {
            if (strcasecmp(g_cfg.apps[j].repo, e->repo) == 0) {
                tracked = true;
                break;
            }
        }
        std::string status;
        pu::ui::Color rc;
        if (tracked) {
            status = "tracked";
            rc = pu::ui::Color(130, 225, 150, 255); // green
        } else if (kind_installable(e->kind)) {
            status = "addable";
            rc = dim_clr;
        } else {
            status = "check only";
            rc = pu::ui::Color(240, 210, 120, 255); // amber
        }
        g_layout->AddRow3(e->name, e->kind, status, name_clr, dim_clr, rc);
    }
    g_layout->SetSel(keep);
}

// ---- SD-card scan ---------------------------------------------------------
// Scan the SD, match against the catalog, and populate g_scan (sorted). Returns
// the number found. No UI; safe to call from launch or the L action.
static int build_scan() {
    int n = 0;
    ScannedApp *found = scan_switch(&n);
    g_scan.clear();
    for (int i = 0; i < n; i++) {
        ScanCand c;
        c.name = found[i].name;
        c.version = found[i].version;
        c.path = found[i].path;
        c.cat = catalog_match(&g_catalog, found[i].name, found[i].path);
        c.repo = (c.cat >= 0) ? g_catalog.items[c.cat].repo : "";
        c.tracked = false;
        for (int j = 0; j < g_cfg.count; j++) {
            if (strcasecmp(g_cfg.apps[j].path, c.path.c_str()) == 0 ||
                (!c.repo.empty() &&
                 strcasecmp(g_cfg.apps[j].repo, c.repo.c_str()) == 0)) {
                c.tracked = true;
                break;
            }
        }
        g_scan.push_back(c);
    }
    free(found);

    // Sort alphabetically by name (case-insensitive), path as tie-breaker.
    std::stable_sort(g_scan.begin(), g_scan.end(),
                     [](const ScanCand &a, const ScanCand &b) {
                         int c = strcasecmp(a.name.c_str(), b.name.c_str());
                         return c != 0 ? c < 0 : a.path < b.path;
                     });

    // Log apps we found but couldn't match to the catalog, so they can be added
    // to known_repos.json later. Snapshot (overwritten each scan).
    FILE *lf = fopen(UNMATCHED_LOG, "wb");
    if (lf) {
        fputs("# HBUpdater - installed .nro with no catalog match\n", lf);
        fputs("# name\tversion\tpath\n", lf);
        int unm = 0;
        for (auto &c : g_scan) {
            if (c.cat < 0) {
                fprintf(lf, "%s\t%s\t%s\n", c.name.c_str(),
                        c.version.empty() ? "-" : c.version.c_str(),
                        c.path.c_str());
                unm++;
            }
        }
        fprintf(lf, "# %d unmatched of %d found\n", unm, (int)g_scan.size());
        fclose(lf);
    }
    return (int)g_scan.size();
}

// ---- settings screen ------------------------------------------------------
// File install submenu (mode 8)
#define FILEINST_COUNT 3
static const char *FILEINST_LABELS[FILEINST_COUNT] = {
    "Install overlays (.ovl)", "Install sysmodules",
    "Install payloads / bootloader"};
static bool *fileinst_ptr(int i) {
    switch (i) {
    case 0: return &g_settings.install_overlays;
    case 1: return &g_settings.install_sysmodules;
    case 2: return &g_settings.install_payloads;
    default: return nullptr;
    }
}
static bool fileinst_is_risky(int i) { return i == 1 || i == 2; }

// Advanced submenu (mode 9)
#define ADVANCED_COUNT 2
static const char *ADVANCED_LABELS[ADVANCED_COUNT] = {
    "Check for updates on launch", "Automatic backup"};
static bool *advanced_ptr(int i) {
    switch (i) {
    case 0: return &g_settings.scan_on_launch;
    case 1: return &g_settings.auto_backup;
    default: return nullptr;
    }
}

// Settings main: action-only rows (no toggles on the main screen anymore)
#define SETTINGS_ACTION_COUNT 7

void MainApplication::OpenSettings() {
    g_list_sel = g_layout->Sel();
    g_mode = 3;
    g_layout->SetSel(0);
    this->RefreshSettings();
}

void MainApplication::RefreshSettings() {
    g_layout->SetTitle("Settings");
    g_layout->SetStatus("");
    g_layout->SetFooter("A select  B back  + exit");
    g_layout->SetColumns("", "", "");
    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    const pu::ui::Color act(150, 190, 240, 255);
    char sup[24], exc[24];
    snprintf(sup, sizeof(sup), "%d apps", g_catalog.count);
    snprintf(exc, sizeof(exc), "%d", g_excludes.count);
    // 0-6: all action rows
    g_layout->AddRow3("Update HBUpdater", std::string("v") + APP_VERSION_STR, ">",
                      name_clr, dim_clr, act);
    g_layout->AddRow3("Update catalog", sup, ">", name_clr, dim_clr, act);
    g_layout->AddRow3("Supported apps", sup, ">", name_clr, dim_clr, act);
    g_layout->AddRow3("Excluded apps", exc, ">", name_clr, dim_clr, act);
    g_layout->AddRow3("View logs", "", ">", name_clr, dim_clr, act);
    g_layout->AddRow3("File install", "", ">", name_clr, dim_clr, act);
    g_layout->AddRow3("Advanced", "", ">", name_clr, dim_clr, act);
    g_layout->SetSel(keep);
}

void MainApplication::UpdateCatalog() {
    if (!start_job(JOB_CATALOG, -1)) {
        this->ToastErr("Busy");
    }
}

void MainApplication::UpdateSelf() {
    if (!start_job(JOB_SELFCHECK, -1)) {
        this->ToastErr("Busy");
    }
}

void MainApplication::ToggleSetting() {
    int i = g_layout->Sel();
    if (g_mode == 8) {
        bool *p = fileinst_ptr(i);
        if (!p) return;
        *p = !*p;
        settings_save(&g_settings);
        this->RefreshFileInstall();
        if (fileinst_is_risky(i) && *p)
            this->ToastErr(std::string(FILEINST_LABELS[i]) + ": enabled - be careful");
    } else if (g_mode == 9) {
        bool *p = advanced_ptr(i);
        if (!p) return;
        *p = !*p;
        settings_save(&g_settings);
        this->RefreshAdvanced();
    }
}

static void render_toggle_list(MainLayout::Ref &lay, const char **labels,
                               bool *(*ptr_fn)(int), bool (*risky_fn)(int),
                               int count) {
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    s32 keep = lay->Sel();
    lay->ClearList();
    for (int i = 0; i < count; i++) {
        bool v = *ptr_fn(i);
        std::string val = v ? "ON" : "OFF";
        pu::ui::Color vc;
        if (!v)
            vc = dim_clr;
        else if (risky_fn && risky_fn(i))
            vc = pu::ui::Color(240, 160, 90, 255);
        else
            vc = pu::ui::Color(130, 225, 150, 255);
        lay->AddRow3(labels[i], "", val, name_clr, dim_clr, vc);
    }
    lay->SetSel(keep);
}

void MainApplication::RefreshFileInstall() {
    g_layout->SetTitle("File install");
    g_layout->SetStatus("");
    g_layout->SetFooter("A toggle  B back  + exit");
    g_layout->SetColumns("Setting", "", "Value");
    render_toggle_list(g_layout, FILEINST_LABELS, fileinst_ptr,
                       fileinst_is_risky, FILEINST_COUNT);
}

void MainApplication::RefreshAdvanced() {
    g_layout->SetTitle("Advanced");
    g_layout->SetStatus("");
    g_layout->SetFooter("A select  B back  + exit");
    g_layout->SetColumns("Setting", "", "Value");
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    const pu::ui::Color act(150, 190, 240, 255);
    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    for (int i = 0; i < ADVANCED_COUNT; i++) {
        bool v = *advanced_ptr(i);
        std::string val = v ? "ON" : "OFF";
        pu::ui::Color vc = v ? pu::ui::Color(130, 225, 150, 255) : dim_clr;
        g_layout->AddRow3(ADVANCED_LABELS[i], "", val, name_clr, dim_clr, vc);
    }
    g_layout->AddRow3("Manage backups", "", ">", name_clr, dim_clr, act);
    g_layout->SetSel(keep);
}

// ---- log viewer -----------------------------------------------------------
void MainApplication::OpenLogs() {
    g_settings_sel = g_layout->Sel();
    g_mode = 4;
    g_layout->SetSel(0);
    this->RefreshLogMenu();
}

void MainApplication::RefreshLogMenu() {
    g_layout->SetTitle("View logs");
    g_layout->SetStatus("");
    g_layout->SetFooter("A open  B back  + exit");
    g_layout->SetColumns("Log", "", "Size");
    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    for (int i = 0; i < 3; i++) {
        long sz = file_size(LOG_PATHS[i]);
        char szs[24];
        if (sz < 0) {
            snprintf(szs, sizeof(szs), "missing");
        } else if (sz == 0) {
            snprintf(szs, sizeof(szs), "empty");
        } else if (sz < 1024) {
            snprintf(szs, sizeof(szs), "%ld B", sz);
        } else {
            snprintf(szs, sizeof(szs), "%ld KB", sz / 1024);
        }
        g_layout->AddRow3(LOG_TITLES[i], "", szs, name_clr, dim_clr, dim_clr);
    }
    g_layout->SetSel(keep);
}

void MainApplication::OpenLog(int idx) {
    if (idx < 0 || idx >= 3) {
        return;
    }
    g_logmenu_sel = g_layout->Sel();
    g_mode = 5;
    g_layout->SetTitle(LOG_TITLES[idx]);
    g_layout->SetFooter("B back  + exit");
    g_layout->ClearColumns(); // log lines use the full width, no header
    g_layout->ClearList();

    const pu::ui::Color line_clr(206, 214, 228, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    long sz = file_size(LOG_PATHS[idx]);
    if (sz < 0) {
        g_layout->SetStatus("missing");
        g_layout->AddRow3("(log file not found)", "", "", dim_clr, dim_clr,
                          dim_clr);
        g_layout->SetSel(0);
        return;
    }
    if (sz == 0) {
        g_layout->SetStatus("empty");
        g_layout->AddRow3("(log is empty)", "", "", dim_clr, dim_clr, dim_clr);
        g_layout->SetSel(0);
        return;
    }

    std::string text = read_tail(LOG_PATHS[idx], 200000);
    bool truncated = (sz > 200000);
    size_t start = 0;
    if (truncated) {
        size_t nl = text.find('\n'); // drop the first partial line
        start = (nl == std::string::npos) ? 0 : nl + 1;
        g_layout->AddRow3("... (older lines truncated) ...", "", "", dim_clr,
                          dim_clr, dim_clr);
    }
    int lines = 0;
    size_t i = start;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            line = " "; // keep blank lines visible as a row
        }
        g_layout->AddRow3(line, "", "", line_clr, line_clr, line_clr);
        lines++;
        if (nl == std::string::npos) {
            break;
        }
        i = nl + 1;
    }
    char st[32];
    snprintf(st, sizeof(st), "%d lines", lines);
    g_layout->SetStatus(st);
    g_layout->SetSel(g_layout->Count() - 1); // jump to the newest (bottom)
}

// ---- excluded apps (opt-out manager) --------------------------------------
void MainApplication::OpenExcluded() {
    g_settings_sel = g_layout->Sel();
    g_mode = 7;
    g_layout->SetSel(0);
    this->RefreshExcluded();
}

void MainApplication::RefreshExcluded() {
    g_layout->SetTitle("Excluded apps");
    char st[40];
    snprintf(st, sizeof(st), "%d excluded", g_excludes.count);
    g_layout->SetStatus(st);
    g_layout->SetFooter("A re-include  B back  + exit");
    g_layout->ClearColumns();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    if (g_excludes.count == 0) {
        g_layout->AddRow3("(nothing excluded)", "", "", dim_clr, dim_clr,
                          dim_clr);
    } else {
        for (int i = 0; i < g_excludes.count; i++) {
            g_layout->AddRow3(g_excludes.repos[i], "", "", name_clr, dim_clr,
                              dim_clr);
        }
    }
    g_layout->SetSel(0);
}

void MainApplication::Unexclude() {
    int i = g_layout->Sel();
    if (i < 0 || i >= g_excludes.count) {
        return;
    }
    std::string repo = g_excludes.repos[i];
    excludes_remove_at(&g_excludes, i);
    excludes_save(&g_excludes);

    // Re-add to the home list right away (no rescan): rebuild the entry from the
    // catalog by repo. Path comes from the catalog default; the next rescan/launch
    // refreshes it to the actual install path if it differs.
    bool readded = false;
    if (apps_find(&g_cfg, repo.c_str()) < 0) {
        for (int c = 0; c < g_catalog.count; c++) {
            if (strcasecmp(g_catalog.items[c].repo, repo.c_str()) == 0) {
                const CatalogEntry *e = &g_catalog.items[c];
                if (apps_add(&g_cfg, e->name, e->repo, e->path, e->asset, e->kind,
                             e->prerelease)) {
                    apps_save(&g_cfg);
                    seed_states_from_cache(); // resize state vectors to new count
                    readded = true;
                }
                break;
            }
        }
    }
    this->RefreshExcluded();
    this->Toast(readded ? "Re-included" : "Re-included (rescan to detect)");
}

// ---- manage backups -------------------------------------------------------
static void load_bklist(int appidx) {
    g_bklist.clear();
    if (appidx < 0 || appidx >= g_cfg.count) {
        return;
    }
    BackupInfo tmp[64];
    int n = backup_list(g_cfg.apps[appidx].repo, tmp, 64);
    for (int k = 0; k < n; k++) {
        g_bklist.push_back(tmp[k]);
    }
}

void MainApplication::OpenBackups(int appidx) {
    g_bk_app = appidx;
    g_bk_from = g_mode;
    g_list_sel = g_layout->Sel();
    load_bklist(appidx);
    g_mode = 6;
    g_layout->SetSel(0);
    this->RefreshBackups();
}

void MainApplication::RefreshBackups() {
    std::string title = std::string("Backups: ") +
                        (g_bk_app >= 0 ? g_cfg.apps[g_bk_app].name : "");
    g_layout->SetTitle(title);
    char st[40];
    snprintf(st, sizeof(st), "%d snapshot%s", (int)g_bklist.size(),
             g_bklist.size() == 1 ? "" : "s");
    g_layout->SetStatus(st);
    g_layout->SetFooter("A restore  X delete  Y clear all  B back  + exit");
    g_layout->SetColumns("Restore to", "Was", "When");
    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    if (g_bklist.empty()) {
        g_layout->AddRow3("(no backups)", "", "", dim_clr, dim_clr, dim_clr);
    } else {
        for (auto &bi : g_bklist) {
            std::string prior = bi.prior[0] ? bi.prior : "-";
            std::string newv = bi.newv[0] ? bi.newv : "-";
            std::string when = bi.when;
            if (when.size() >= 16) {
                when = when.substr(5, 11); // "MM-DD HH:MM"
            }
            g_layout->AddRow3(prior, newv, when, name_clr, dim_clr, dim_clr);
        }
    }
    g_layout->SetSel(keep);
}

void MainApplication::RevertBackup() {
    if (g_bk_app < 0 || g_bk_app >= g_cfg.count) {
        return;
    }
    s32 i = g_layout->Sel();
    if (i < 0 || i >= (int)g_bklist.size()) {
        return;
    }
    const BackupInfo &bi = g_bklist[i];
    std::string m = std::string("Restore ") + g_cfg.apps[g_bk_app].name +
                    (bi.prior[0] ? std::string(" to ") + bi.prior
                                 : std::string()) +
                    "?";
    if (this->Confirm("Restore", m)) {
        snprintf(g_revert_id, sizeof(g_revert_id), "%s", bi.id);
        if (!start_job(JOB_REVERT, g_bk_app)) {
            this->ToastErr("Busy");
        }
    }
}

void MainApplication::DeleteBackup() {
    if (g_bk_app < 0 || g_bk_app >= g_cfg.count) {
        return;
    }
    s32 i = g_layout->Sel();
    if (i < 0 || i >= (int)g_bklist.size()) {
        return;
    }
    const BackupInfo &bi = g_bklist[i];
    std::string label = bi.prior[0] ? bi.prior : bi.id;
    if (this->Confirm("Delete backup", std::string("Delete snapshot ") + label +
                                           "?")) {
        net_log("BACKUP delete snapshot: %s / %s", g_cfg.apps[g_bk_app].repo, bi.id);
        backup_delete_id(g_cfg.apps[g_bk_app].repo, bi.id);
        load_bklist(g_bk_app);
        this->RefreshBackups();
        this->Toast("Deleted");
    }
}

void MainApplication::ClearBackups() {
    if (g_bk_app < 0 || g_bk_app >= g_cfg.count) {
        return;
    }
    if (this->Confirm("Clear all backups",
                      std::string("Delete ALL backups for ") +
                          g_cfg.apps[g_bk_app].name + "?")) {
        net_log("BACKUP clear all: %s", g_cfg.apps[g_bk_app].repo);
        backup_clear(g_cfg.apps[g_bk_app].repo);
        this->Toast("Cleared");
        if (g_bk_from == 10) {
            scan_backup_dirs();
            g_mode = 10;
            this->RefreshAllBackups();
        } else {
            g_mode = 0;
            this->Refresh();
            g_layout->SetSel(g_list_sel);
        }
    }
}

// ---- global backup browser (mode 10) --------------------------------------
struct BkEntry {
    std::string name;  // display name (app name or slug if orphaned)
    std::string slug;  // directory name on disk
    std::string repo;  // matched repo from g_cfg ("" if orphaned)
    int app_idx;       // index into g_cfg.apps (-1 if orphaned)
    int count;         // number of snapshots
};
static std::vector<BkEntry> g_allbk;

static void slugify_local(const char *repo, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; repo[i] && j + 1 < outsz; i++) {
        char c = repo[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static void scan_backup_dirs() {
    g_allbk.clear();
    DIR *d = opendir(BACKUP_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string path = std::string(BACKUP_DIR) + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int snapshots = 0;
        DIR *sub = opendir(path.c_str());
        if (sub) {
            struct dirent *se;
            while ((se = readdir(sub)) != nullptr) {
                if (se->d_name[0] != '.') snapshots++;
            }
            closedir(sub);
        }
        std::string repo;
        int app_idx = -1;
        char slug[160];
        for (int i = 0; i < g_cfg.count; i++) {
            slugify_local(g_cfg.apps[i].repo, slug, sizeof(slug));
            if (strcmp(slug, e->d_name) == 0) {
                repo = g_cfg.apps[i].repo;
                app_idx = i;
                break;
            }
        }
        std::string label = repo.empty() ? std::string(e->d_name) : std::string(g_cfg.apps[app_idx].name);
        g_allbk.push_back({label, e->d_name, repo, app_idx, snapshots});
    }
    closedir(d);
    std::sort(g_allbk.begin(), g_allbk.end(),
              [](const BkEntry &a, const BkEntry &b) { return a.name < b.name; });
}

void MainApplication::RefreshAllBackups() {
    g_layout->SetTitle("Manage backups");
    char st[40];
    snprintf(st, sizeof(st), "%d app%s", (int)g_allbk.size(),
             g_allbk.size() == 1 ? "" : "s");
    g_layout->SetStatus(st);
    g_layout->SetFooter("A view  X delete  B back  + exit");
    g_layout->SetColumns("App", "", "Snapshots");
    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    const pu::ui::Color name_clr(232, 234, 240, 255);
    const pu::ui::Color dim_clr(170, 175, 185, 255);
    if (g_allbk.empty()) {
        g_layout->AddRow3("(no backups)", "", "", dim_clr, dim_clr, dim_clr);
    } else {
        for (auto &bk : g_allbk) {
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%d", bk.count);
            g_layout->AddRow3(bk.name, "", cnt, name_clr, dim_clr, dim_clr);
        }
    }
    g_layout->SetSel(keep);
}

void MainApplication::DeleteBackupFolder() {
    s32 i = g_layout->Sel();
    if (i < 0 || i >= (int)g_allbk.size()) return;
    const BkEntry &bk = g_allbk[i];
    if (!this->Confirm("Delete backups",
                       "Delete ALL backups for " + bk.name + "?"))
        return;
    std::string path = std::string(BACKUP_DIR) + "/" + bk.slug;
    net_log("BACKUP delete folder: %s", path.c_str());
    if (fs_rm_rf(path.c_str())) {
        net_log("BACKUP delete folder: ok");
        this->Toast("Deleted " + bk.name);
    } else {
        net_log("BACKUP delete folder: FAILED");
        this->ToastErr("Delete failed");
    }
    scan_backup_dirs();
    this->RefreshAllBackups();
}

// Opt-out model: the home list is whatever is currently installed + recognized,
// minus the user's excludes. Rescan the SD and reconcile g_cfg to match: add
// newly-found recognized apps, refresh paths, drop apps no longer installed (or
// now excluded). Installed versions are taken from NACP only for new apps (so a
// tool-set release tag isn't clobbered on rescan). Then auto-check (stale only).
void MainApplication::ReconcileInstalled() {
    build_scan(); // fills g_scan (all installed .nro, matched + unmatched)

    int unmatched = 0;
    std::vector<std::string> desired; // repos that should be on the home list
    for (auto &c : g_scan) {
        if (c.cat < 0) {
            unmatched++;
            continue; // not recognized -> only logged, never on home
        }
        if (excludes_contains(&g_excludes, c.repo.c_str())) {
            continue; // opted out
        }
        desired.push_back(c.repo);
        const CatalogEntry *e = &g_catalog.items[c.cat];
        int idx = apps_find(&g_cfg, c.repo.c_str());
        if (idx < 0) {
            App *a = apps_add(&g_cfg, e->name, e->repo, c.path.c_str(), e->asset,
                              e->kind, e->prerelease);
            if (a) {
                snprintf(a->version, sizeof(a->version), "%s",
                         c.version.c_str());
            }
        } else {
            // Keep the recorded version (may be a tool-set tag); just refresh
            // the install path in case it moved.
            snprintf(g_cfg.apps[idx].path, sizeof(g_cfg.apps[idx].path), "%s",
                     c.path.c_str());
        }
    }
    // Drop apps that are no longer installed or were excluded.
    for (int i = g_cfg.count - 1; i >= 0; i--) {
        bool keep = false;
        for (auto &r : desired) {
            if (strcasecmp(r.c_str(), g_cfg.apps[i].repo) == 0) {
                keep = true;
                break;
            }
        }
        if (!keep) {
            apps_remove(&g_cfg, i);
        }
    }
    std::sort(g_cfg.apps, g_cfg.apps + g_cfg.count,
              [](const App &a, const App &b) {
                  return strcasecmp(a.name, b.name) < 0;
              });
    apps_save(&g_cfg);
    seed_states_from_cache();
    this->Refresh();

    if (unmatched > 0) {
        char m[64];
        snprintf(m, sizeof(m), "%d unrecognized app%s (see logs)", unmatched,
                 unmatched == 1 ? "" : "s");
        g_pending_toast = m;
    }

}

void MainApplication::HandleInput(u64 down, u64 held) {
    const u64 NAV_UP = HidNpadButton_Up | HidNpadButton_StickLUp;
    const u64 NAV_DOWN = HidNpadButton_Down | HidNpadButton_StickLDown;
    if (down & NAV_DOWN) {
        g_layout->Step(1);
    }
    if (down & NAV_UP) {
        g_layout->Step(-1);
    }
    {
        static int hold = 0;
        int dir = (held & NAV_DOWN) ? 1 : (held & NAV_UP) ? -1 : 0;
        if (dir == 0) {
            hold = 0;
        } else if (++hold > 22 && ((hold - 22) % 3) == 0) {
            g_layout->Step(dir);
        }
    }
    if (down & HidNpadButton_ZL) {
        g_layout->PageUp();
    }
    if (down & HidNpadButton_ZR) {
        g_layout->PageDown();
    }

    // While a network job runs, only navigation is allowed (the worker reads
    // g_cfg and the result vectors; blocking actions avoids races + double jobs).
    if (g_busy) {
        return;
    }

    // ---- supported-apps browse (read-only) ----
    if (g_mode == 1) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            this->CloseCatalog();
        } else if (down & HidNpadButton_A) {
            s32 sel = g_layout->Sel();
            if (sel >= 0 && sel < g_catalog.count) {
                const CatalogEntry *e = &g_catalog.items[sel];
                bool tracked = apps_find(&g_cfg, e->repo) >= 0;
                char info[1024];
                snprintf(info, sizeof(info),
                         "Repo: %s\nPath: %s\nKind: %s\nAsset: %s\n"
                         "Prerelease: %s\nStatus: %s",
                         e->repo, e->path, e->kind,
                         e->asset[0] ? e->asset : "(default)",
                         e->prerelease ? "yes" : "no",
                         tracked ? "tracked" : "not tracked");
                this->CreateShowDialog(e->name, info, {"OK"}, true);
            }
        }
        return;
    }

    // ---- excluded-apps manager ----
    if (g_mode == 7) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 3;
            this->RefreshSettings();
            g_layout->SetSel(g_settings_sel);
        } else if (down & HidNpadButton_A) {
            this->Unexclude();
        }
        return;
    }

    // ---- settings mode ----
    if (g_mode == 3) {
        if (down & HidNpadButton_Plus) {
            this->Close();
            return;
        }
        if (down & HidNpadButton_B) {
            g_mode = 0;
            this->Refresh();
            g_layout->SetSel(g_list_sel);
        } else if (down & HidNpadButton_A) {
            int sel = g_layout->Sel();
            if (sel == 0) {
                this->UpdateSelf();
            } else if (sel == 1) {
                this->UpdateCatalog();
            } else if (sel == 2) {
                this->OpenCatalog();
            } else if (sel == 3) {
                this->OpenExcluded();
            } else if (sel == 4) {
                this->OpenLogs();
            } else if (sel == 5) {
                g_settings_sel = g_layout->Sel();
                g_mode = 8;
                g_layout->SetSel(0);
                this->RefreshFileInstall();
            } else if (sel == 6) {
                g_settings_sel = g_layout->Sel();
                g_mode = 9;
                g_layout->SetSel(0);
                this->RefreshAdvanced();
            }
        }
        return;
    }

    if (g_mode == 8) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 3;
            this->RefreshSettings();
            g_layout->SetSel(g_settings_sel);
        } else if (down & HidNpadButton_A) {
            this->ToggleSetting();
        }
        return;
    }

    if (g_mode == 9) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 3;
            this->RefreshSettings();
            g_layout->SetSel(g_settings_sel);
        } else if (down & HidNpadButton_A) {
            int sel = g_layout->Sel();
            if (sel == ADVANCED_COUNT) {
                scan_backup_dirs();
                g_mode = 10;
                g_layout->SetSel(0);
                this->RefreshAllBackups();
            } else {
                this->ToggleSetting();
            }
        }
        return;
    }

    if (g_mode == 10) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 9;
            this->RefreshAdvanced();
        } else if (down & HidNpadButton_A) {
            s32 sel = g_layout->Sel();
            if (sel >= 0 && sel < (int)g_allbk.size() && g_allbk[sel].app_idx >= 0) {
                this->OpenBackups(g_allbk[sel].app_idx);
            } else if (sel >= 0 && sel < (int)g_allbk.size()) {
                this->Toast("No tracked app for this backup");
            }
        } else if (down & HidNpadButton_X) {
            this->DeleteBackupFolder();
        }
        return;
    }

    // ---- log picker ----
    if (g_mode == 4) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 3;
            this->RefreshSettings();
            g_layout->SetSel(g_settings_sel);
        } else if (down & HidNpadButton_A) {
            this->OpenLog(g_layout->Sel());
        }
        return;
    }

    // ---- log contents ----
    if (g_mode == 5) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            g_mode = 4;
            this->RefreshLogMenu();
            g_layout->SetSel(g_logmenu_sel);
        }
        return;
    }

    // ---- manage backups ----
    if (g_mode == 6) {
        if (down & HidNpadButton_Plus) { this->Close(); return; }
        if (down & HidNpadButton_B) {
            if (g_bk_from == 10) {
                scan_backup_dirs();
                g_mode = 10;
                this->RefreshAllBackups();
                g_layout->SetSel(g_list_sel);
            } else {
                g_mode = 0;
                this->Refresh();
                g_layout->SetSel(g_list_sel);
            }
        } else if (down & HidNpadButton_A) {
            this->RevertBackup();
        } else if (down & HidNpadButton_X) {
            this->DeleteBackup();
        } else if (down & HidNpadButton_Y) {
            this->ClearBackups();
        }
        return;
    }

    // ---- home ("My Apps") mode ----
    if (down & HidNpadButton_Plus) {
        this->Close();
        return;
    }
    if (down & HidNpadButton_X) {
        if (g_cfg.count > 0) {
            g_check_stale = false; // manual: force a full re-check
            this->CheckAll();
        }
        return;
    }
    if (down & HidNpadButton_R) {
        this->OpenSettings();
        return;
    }
    s32 i = g_layout->Sel();
    if (down & HidNpadButton_A) {
        if (i < 0 || i >= g_cfg.count) {
            return;
        }
        bool has_backup = backup_exists(g_cfg.apps[i].repo);
        bool pinned = g_cfg.apps[i].pinned;
        std::vector<std::string> opts = {"Check / Update"};
        int manage_idx = -1, force_idx = -1, pin_idx = -1;
        if (has_backup) {
            manage_idx = (int)opts.size();
            opts.push_back("Manage backups");
        }
        force_idx = (int)opts.size();
        opts.push_back("Force reinstall");
        pin_idx = (int)opts.size();
        opts.push_back(pinned ? "Unpin version" : "Pin version");
        opts.push_back("Cancel");
        int r = this->CreateShowDialog("Actions", g_cfg.apps[i].name, opts,
                                       true);
        if (r == 0) {
            g_force = false;
            this->StartCheck(i, true);
        } else if (r == manage_idx && has_backup) {
            this->OpenBackups(i);
        } else if (r == force_idx) {
            g_force = true;
            this->StartCheck(i, true);
        } else if (r == pin_idx) {
            g_cfg.apps[i].pinned = !g_cfg.apps[i].pinned;
            apps_save(&g_cfg);
            this->Refresh();
            this->Toast(g_cfg.apps[i].pinned ? "Pinned" : "Unpinned");
        }
    } else if (down & HidNpadButton_Minus) {
        if (i >= 0 && i < g_cfg.count) {
            if (this->Confirm("Exclude", std::string("Exclude ") +
                                             g_cfg.apps[i].name +
                                             " from updates?\n(undo in Settings "
                                             "-> Excluded apps)")) {
                excludes_add(&g_excludes, g_cfg.apps[i].repo);
                excludes_save(&g_excludes);
                apps_remove(&g_cfg, i);
                apps_save(&g_cfg);
                this->Refresh();
                this->Toast("Excluded");
            }
        }
    }
}

void MainApplication::OnLoad() {
    romfsInit();
    net_init();
    settings_load(&g_settings);
    net_set_auth(g_settings.github_token); // optional PAT -> 5000/hr API limit
    excludes_load(&g_excludes);
    apps_load(&g_cfg);
    catalog_load(&g_catalog);
    seed_states_from_cache(); // restore update flags from the last session

    g_layout = MainLayout::New();
    this->LoadLayout(g_layout);

    this->ReconcileInstalled();

    if (!g_settings.first_run_done) {
        g_settings.first_run_done = true;
        settings_save(&g_settings);
        g_launch_action = 1; // prompt: offer to check for updates
    } else if (g_settings.scan_on_launch) {
        g_launch_action = 2; // opt-in: auto-check for updates
    }

    // SetOnInput fires every frame (down==0 when idle), so it doubles as the
    // per-frame tick that polls the background worker.
    this->SetOnInput([&](const u64 down, const u64 up, const u64 held,
                         const pu::ui::TouchPoint touch) {
        (void)up;
        (void)touch;
        this->Tick();
        this->HandleInput(down, held);
    });
}
