#include "config.h"
#include "jsonutil.h"
#include "fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sset(char *dst, size_t dsz, const char *src) {
    if (dsz == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < dsz; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void seed_from_romfs(void) {
    if (fs_exists(APPS_PATH)) {
        return;
    }
    size_t len = 0;
    char *def = json_read_file("romfs:/apps.json", &len);
    if (!def) {
        return;
    }
    fs_mkdir_p(CONFIG_DIR);
    FILE *f = fopen(APPS_PATH, "wb");
    if (f) {
        fwrite(def, 1, len, f);
        fclose(f);
    }
    free(def);
}

void apps_load(AppsConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    seed_from_romfs();

    size_t len = 0;
    char *js = json_read_file(APPS_PATH, &len);
    if (!js) {
        return;
    }
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(js, len, &ntok);
    if (!tok || tok[0].type != JSMN_OBJECT) {
        free(tok);
        free(js);
        return;
    }

    int ai = json_obj_get(js, tok, 0, "apps");
    if (ai >= 0 && tok[ai].type == JSMN_ARRAY) {
        int count = tok[ai].size;
        int child = ai + 1;
        for (int i = 0; i < count && cfg->count < MAX_APPS; i++) {
            if (tok[child].type == JSMN_OBJECT) {
                App *a = &cfg->apps[cfg->count];
                memset(a, 0, sizeof(*a));
                json_copy(js, tok, json_obj_get(js, tok, child, "name"), a->name,
                          sizeof(a->name));
                json_copy(js, tok, json_obj_get(js, tok, child, "repo"), a->repo,
                          sizeof(a->repo));
                json_copy(js, tok, json_obj_get(js, tok, child, "path"), a->path,
                          sizeof(a->path));
                json_copy(js, tok, json_obj_get(js, tok, child, "version"),
                          a->version, sizeof(a->version));
                if (a->repo[0] && a->path[0]) {
                    if (!a->name[0]) {
                        sset(a->name, sizeof(a->name), a->repo);
                    }
                    cfg->count++;
                }
            }
            child = json_tok_skip(tok, child);
        }
    }

    free(tok);
    free(js);
}

bool apps_save(const AppsConfig *cfg) {
    fs_mkdir_p(CONFIG_DIR);
    FILE *f = fopen(APPS_PATH, "wb");
    if (!f) {
        return false;
    }
    fputs("{\n  \"apps\": [\n", f);
    for (int i = 0; i < cfg->count; i++) {
        const App *a = &cfg->apps[i];
        fputs("    { \"name\": ", f);
        json_write_escaped(f, a->name);
        fputs(", \"repo\": ", f);
        json_write_escaped(f, a->repo);
        fputs(", \"path\": ", f);
        json_write_escaped(f, a->path);
        fputs(", \"version\": ", f);
        json_write_escaped(f, a->version);
        fputs(" }", f);
        fputs(i + 1 < cfg->count ? ",\n" : "\n", f);
    }
    fputs("  ]\n}\n", f);
    fclose(f);
    return true;
}

App *apps_add(AppsConfig *cfg, const char *name, const char *repo,
              const char *path) {
    if (!repo || !repo[0] || !path || !path[0] || cfg->count >= MAX_APPS) {
        return NULL;
    }
    App *a = &cfg->apps[cfg->count++];
    memset(a, 0, sizeof(*a));
    sset(a->name, sizeof(a->name), (name && name[0]) ? name : repo);
    sset(a->repo, sizeof(a->repo), repo);
    sset(a->path, sizeof(a->path), path);
    return a;
}

bool apps_remove(AppsConfig *cfg, int idx) {
    if (idx < 0 || idx >= cfg->count) {
        return false;
    }
    for (int i = idx; i < cfg->count - 1; i++) {
        cfg->apps[i] = cfg->apps[i + 1];
    }
    cfg->count--;
    return true;
}
