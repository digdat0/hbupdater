#include "backup.h"
#include "config.h"
#include "fsutil.h"
#include "jsonutil.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ACT_OVERWRITE 0
#define ACT_CREATE 1

struct Backup {
    char name[96];
    char repo[160];
    char kind[8];
    char prior[64];
    char newv[64];
    char iddir[400];    /* backups/<slug>/<id> */
    char filesdir[470]; /* backups/<slug>/<id>/files */
    struct {
        char *path;
        int action;
    } *ents;
    int n, cap;
};

static void slugify(const char *repo, char *out, size_t outsz) {
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

static const char *rel_of(const char *path) {
    if (strncmp(path, "sdmc:/", 6) == 0) {
        return path + 6;
    }
    const char *c = strchr(path, ':');
    if (c && c[1] == '/') {
        return c + 2;
    }
    return path[0] == '/' ? path + 1 : path;
}

static bool joinp(char *out, size_t osz, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1 + lb + 1 > osz) {
        return false;
    }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return true;
}

static void sset(char *dst, size_t dsz, const char *src) {
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < dsz; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/* backups/<slug> for a repo. */
static bool app_dir(const char *repo, char *out, size_t outsz) {
    char slug[160];
    slugify(repo, slug, sizeof(slug));
    return joinp(out, outsz, BACKUP_DIR, slug);
}

static void now_str(char *out, size_t sz) {
    time_t t = time(NULL);
    struct tm *lt = t > 0 ? localtime(&t) : NULL;
    if (lt) {
        strftime(out, sz, "%Y-%m-%d %H:%M:%S", lt);
    } else {
        sset(out, sz, "");
    }
}

Backup *backup_start(const char *repo, const char *name, const char *kind,
                     const char *prior_version, const char *new_version) {
    Backup *b = (Backup *)calloc(1, sizeof(Backup));
    if (!b) {
        return NULL;
    }
    sset(b->repo, sizeof(b->repo), repo);
    sset(b->name, sizeof(b->name), name);
    sset(b->kind, sizeof(b->kind), kind);
    sset(b->prior, sizeof(b->prior), prior_version);
    sset(b->newv, sizeof(b->newv), new_version);

    char appdir[340];
    if (!app_dir(repo, appdir, sizeof(appdir))) {
        free(b);
        return NULL;
    }
    fs_mkdir_p(appdir);

    /* Timestamped snapshot id, made unique if two land in the same second. */
    char id[32];
    time_t t = time(NULL);
    struct tm *lt = t > 0 ? localtime(&t) : NULL;
    if (lt) {
        strftime(id, sizeof(id), "%Y%m%d-%H%M%S", lt);
    } else {
        sset(id, sizeof(id), "backup");
    }
    if (!joinp(b->iddir, sizeof(b->iddir), appdir, id)) {
        free(b);
        return NULL;
    }
    for (int k = 2; k < 100 && fs_exists(b->iddir); k++) {
        char id2[40];
        snprintf(id2, sizeof(id2), "%s-%d", id, k);
        joinp(b->iddir, sizeof(b->iddir), appdir, id2);
    }
    joinp(b->filesdir, sizeof(b->filesdir), b->iddir, "files");
    fs_mkdir_p(b->iddir);
    return b;
}

bool backup_record(Backup *b, const char *dest) {
    if (!b) {
        return false;
    }
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 32;
        void *nn = realloc(b->ents, (size_t)nc * sizeof(*b->ents));
        if (!nn) {
            return false;
        }
        b->ents = nn;
        b->cap = nc;
    }
    int action = ACT_CREATE;
    if (fs_exists(dest)) {
        char store[1024];
        if (joinp(store, sizeof(store), b->filesdir, rel_of(dest)) &&
            fs_copy(dest, store)) {
            action = ACT_OVERWRITE;
        }
    }
    b->ents[b->n].path = strdup(dest);
    if (!b->ents[b->n].path) {
        return false;
    }
    b->ents[b->n].action = action;
    b->n++;
    return true;
}

static void free_ents(Backup *b) {
    for (int i = 0; i < b->n; i++) {
        free(b->ents[i].path);
    }
    free(b->ents);
    free(b);
}

bool backup_finish(Backup *b) {
    if (!b) {
        return false;
    }
    char manifest[480];
    joinp(manifest, sizeof(manifest), b->iddir, "manifest.json");

    int overwrites = 0, creates = 0;
    for (int i = 0; i < b->n; i++) {
        if (b->ents[i].action == ACT_OVERWRITE) {
            overwrites++;
        } else {
            creates++;
        }
    }
    char when[32];
    now_str(when, sizeof(when));

    FILE *f = fopen(manifest, "wb");
    if (!f) {
        free_ents(b);
        return false;
    }
    fputs("{\n  \"name\": ", f);
    json_write_escaped(f, b->name);
    fputs(",\n  \"repo\": ", f);
    json_write_escaped(f, b->repo);
    fputs(",\n  \"kind\": ", f);
    json_write_escaped(f, b->kind);
    fputs(",\n  \"prior_version\": ", f);
    json_write_escaped(f, b->prior);
    fputs(",\n  \"new_version\": ", f);
    json_write_escaped(f, b->newv);
    fputs(",\n  \"time\": ", f);
    json_write_escaped(f, when);
    fputs(",\n  \"entries\": [\n", f);
    for (int i = 0; i < b->n; i++) {
        fputs("    { \"path\": ", f);
        json_write_escaped(f, b->ents[i].path);
        fputs(", \"action\": ", f);
        json_write_escaped(f, b->ents[i].action == ACT_OVERWRITE ? "overwrite"
                                                                  : "create");
        fputs(i + 1 < b->n ? " },\n" : " }\n", f);
    }
    fputs("  ]\n}\n", f);
    fclose(f);

    FILE *h = fopen(HISTORY_LOG, "ab");
    if (h) {
        fprintf(h, "[%s] %s  %s -> %s  (kind=%s, overwrite=%d, create=%d)\n",
                when[0] ? when : "?", b->name[0] ? b->name : b->repo,
                b->prior[0] ? b->prior : "-", b->newv[0] ? b->newv : "-",
                b->kind, overwrites, creates);
        for (int i = 0; i < b->n; i++) {
            fprintf(h, "    %s %s\n",
                    b->ents[i].action == ACT_OVERWRITE ? "OW " : "NEW",
                    b->ents[i].path);
        }
        fclose(h);
    }

    free_ents(b);
    return true;
}

void backup_cancel(Backup *b) {
    if (!b) {
        return;
    }
    fs_rm_rf(b->iddir);
    free_ents(b);
}

/* Read one field from a snapshot manifest. */
static void manifest_field(const char *manifest, const char *key, char *out,
                           size_t outsz) {
    out[0] = '\0';
    size_t len = 0;
    char *js = json_read_file(manifest, &len);
    if (!js) {
        return;
    }
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(js, len, &ntok);
    if (tok && tok[0].type == JSMN_OBJECT) {
        int i = json_obj_get(js, tok, 0, key);
        if (i >= 0) {
            json_copy(js, tok, i, out, outsz);
        }
    }
    free(tok);
    free(js);
}

int backup_count(const char *repo) {
    char appdir[340];
    if (!app_dir(repo, appdir, sizeof(appdir))) {
        return 0;
    }
    DIR *d = opendir(appdir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char man[480];
        char sub[400];
        if (joinp(sub, sizeof(sub), appdir, e->d_name) &&
            joinp(man, sizeof(man), sub, "manifest.json") && fs_exists(man)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

bool backup_exists(const char *repo) { return backup_count(repo) > 0; }

static int cmp_info_desc(const void *a, const void *b) {
    return strcmp(((const BackupInfo *)b)->id, ((const BackupInfo *)a)->id);
}

int backup_list(const char *repo, BackupInfo *out, int max) {
    char appdir[340];
    if (!app_dir(repo, appdir, sizeof(appdir)) || max <= 0) {
        return 0;
    }
    DIR *d = opendir(appdir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char sub[400], man[480];
        if (!joinp(sub, sizeof(sub), appdir, e->d_name) ||
            !joinp(man, sizeof(man), sub, "manifest.json") || !fs_exists(man)) {
            continue;
        }
        BackupInfo *bi = &out[n];
        memset(bi, 0, sizeof(*bi));
        sset(bi->id, sizeof(bi->id), e->d_name);
        manifest_field(man, "prior_version", bi->prior, sizeof(bi->prior));
        manifest_field(man, "new_version", bi->newv, sizeof(bi->newv));
        manifest_field(man, "time", bi->when, sizeof(bi->when));
        manifest_field(man, "kind", bi->kind, sizeof(bi->kind));
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(BackupInfo), cmp_info_desc); /* newest first */
    return n;
}

static bool id_safe(const char *id) {
    if (!id || !id[0] || id[0] == '.') return false;
    for (const char *p = id; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
        if (*p == '.' && *(p + 1) == '.') return false;
    }
    return true;
}

bool backup_revert_id(const char *repo, const char *id, char *prior_version,
                      size_t pv_sz) {
    if (prior_version && pv_sz) {
        prior_version[0] = '\0';
    }
    if (!id_safe(id)) return false;
    char appdir[340], iddir[400], man[480], filesdir[470];
    if (!app_dir(repo, appdir, sizeof(appdir)) ||
        !joinp(iddir, sizeof(iddir), appdir, id) ||
        !joinp(man, sizeof(man), iddir, "manifest.json") ||
        !joinp(filesdir, sizeof(filesdir), iddir, "files")) {
        return false;
    }
    size_t len = 0;
    char *js = json_read_file(man, &len);
    if (!js) {
        return false;
    }
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(js, len, &ntok);
    if (!tok || tok[0].type != JSMN_OBJECT) {
        free(tok);
        free(js);
        return false;
    }
    int pv = json_obj_get(js, tok, 0, "prior_version");
    if (pv >= 0 && prior_version) {
        json_copy(js, tok, pv, prior_version, pv_sz);
    }

    bool ok = true;
    int ai = json_obj_get(js, tok, 0, "entries");
    if (ai >= 0 && tok[ai].type == JSMN_ARRAY) {
        int count = tok[ai].size;
        int child = ai + 1;
        for (int i = 0; i < count; i++) {
            if (tok[child].type == JSMN_OBJECT) {
                char path[1024] = "", action[16] = "";
                json_copy(js, tok, json_obj_get(js, tok, child, "path"), path,
                          sizeof(path));
                json_copy(js, tok, json_obj_get(js, tok, child, "action"),
                          action, sizeof(action));
                if (path[0]) {
                    if (strcmp(action, "overwrite") == 0) {
                        char store[1024];
                        if (joinp(store, sizeof(store), filesdir,
                                  rel_of(path)) &&
                            !fs_copy(store, path)) {
                            ok = false;
                        }
                    } else {
                        remove(path);
                    }
                }
            }
            child = json_tok_skip(tok, child);
        }
    }
    free(tok);
    free(js);
    return ok; /* snapshot kept; clearing is manual */
}

bool backup_delete_id(const char *repo, const char *id) {
    if (!id_safe(id)) return false;
    char appdir[340], iddir[400];
    if (!app_dir(repo, appdir, sizeof(appdir)) ||
        !joinp(iddir, sizeof(iddir), appdir, id)) {
        return false;
    }
    return fs_rm_rf(iddir);
}

bool backup_clear(const char *repo) {
    char appdir[340];
    if (!app_dir(repo, appdir, sizeof(appdir))) {
        return false;
    }
    return fs_rm_rf(appdir);
}
