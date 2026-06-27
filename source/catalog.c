#include "catalog.h"
#include "config.h"
#include "jsonutil.h"
#include "net.h"
#include "fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG_BUNDLED "romfs:/known_repos.json" /* baked into the NRO */

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

static bool load_from(Catalog *cat, const char *path) {
    cat->items = NULL;
    cat->count = 0;

    size_t len = 0;
    char *js = json_read_file(path, &len);
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
    int ai = json_obj_get(js, tok, 0, "apps");
    if (ai < 0 || tok[ai].type != JSMN_ARRAY) {
        free(tok);
        free(js);
        return false;
    }

    int count = tok[ai].size;
    cat->items = (CatalogEntry *)calloc(count > 0 ? count : 1,
                                        sizeof(CatalogEntry));
    if (!cat->items) {
        free(tok);
        free(js);
        return false;
    }

    int child = ai + 1;
    for (int i = 0; i < count; i++) {
        if (tok[child].type == JSMN_OBJECT) {
            CatalogEntry *e = &cat->items[cat->count];
            memset(e, 0, sizeof(*e));
            json_copy(js, tok, json_obj_get(js, tok, child, "name"), e->name,
                      sizeof(e->name));
            json_copy(js, tok, json_obj_get(js, tok, child, "repo"), e->repo,
                      sizeof(e->repo));
            json_copy(js, tok, json_obj_get(js, tok, child, "default_path"),
                      e->path, sizeof(e->path));
            json_copy(js, tok, json_obj_get(js, tok, child, "asset"), e->asset,
                      sizeof(e->asset));
            json_copy(js, tok, json_obj_get(js, tok, child, "asset_kind"),
                      e->kind, sizeof(e->kind));
            /* Flatten the nacp_name string array into one '\n'-separated blob so
             * the SD scanner can test a parsed NACP title against each alias. */
            int ni = json_obj_get(js, tok, child, "nacp_name");
            if (ni >= 0 && tok[ni].type == JSMN_ARRAY) {
                int an = tok[ni].size, ac = ni + 1;
                size_t used = 0;
                for (int k = 0; k < an; k++) {
                    char alias[128];
                    json_copy(js, tok, ac, alias, sizeof(alias));
                    size_t al = strlen(alias);
                    if (al && used + al + 1 < sizeof(e->nacp)) {
                        if (used) {
                            e->nacp[used++] = '\n';
                        }
                        memcpy(e->nacp + used, alias, al);
                        used += al;
                        e->nacp[used] = '\0';
                    }
                    ac = json_tok_skip(tok, ac);
                }
            }
            e->prerelease = json_bool(
                js, tok, json_obj_get(js, tok, child, "prerelease"));
            if (!e->kind[0]) {
                sset(e->kind, sizeof(e->kind), "nro");
            }
            if (e->repo[0] && e->path[0]) {
                if (!e->name[0]) {
                    sset(e->name, sizeof(e->name), e->repo);
                }
                cat->count++;
            }
        }
        child = json_tok_skip(tok, child);
    }

    free(tok);
    free(js);
    return cat->count > 0;
}

bool catalog_load(Catalog *cat) {
    /* Prefer the OTA cache on the SD (updated independently of the NRO); fall
     * back to the copy bundled in romfs. */
    if (load_from(cat, CATALOG_CACHE)) {
        return true;
    }
    catalog_free(cat); /* discard any partial */
    return load_from(cat, CATALOG_BUNDLED);
}

static bool files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return false; }
    fseek(fa, 0, SEEK_END); long sa = ftell(fa); rewind(fa);
    fseek(fb, 0, SEEK_END); long sb = ftell(fb); rewind(fb);
    if (sa != sb || sa <= 0) { fclose(fa); fclose(fb); return false; }
    bool eq = true;
    char ba[4096], bb[4096];
    while (sa > 0) {
        size_t n = sa < (long)sizeof(ba) ? (size_t)sa : sizeof(ba);
        if (fread(ba, 1, n, fa) != n || fread(bb, 1, n, fb) != n ||
            memcmp(ba, bb, n) != 0) { eq = false; break; }
        sa -= (long)n;
    }
    fclose(fa); fclose(fb);
    return eq;
}

int catalog_update(void) {
    fs_mkdir_p(CONFIG_DIR);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", CATALOG_CACHE);

    net_log("CATALOG update: %s", CATALOG_URL);
    long code = 0;
    bool dok = http_download(CATALOG_URL, tmp, NULL, NULL, NULL, 0, &code);
    long got = -1;
    FILE *tf = fopen(tmp, "rb");
    if (tf) {
        fseek(tf, 0, SEEK_END);
        got = ftell(tf);
        fclose(tf);
    }
    net_log("  download -> %s (http=%ld, %ld bytes)", dok ? "ok" : "FAIL", code,
            got);
    if (!dok) {
        remove(tmp);
        return 0;
    }
    Catalog test;
    bool ok = load_from(&test, tmp);
    int n = test.count;
    catalog_free(&test);
    net_log("  validate -> %s (%d records)", ok ? "ok" : "FAIL", n);
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (files_equal(tmp, CATALOG_CACHE)) {
        net_log("  unchanged (same as cached)");
        remove(tmp);
        return 2;
    }
    bool mv = fs_move(tmp, CATALOG_CACHE);
    net_log("  saved -> %s (%s)", mv ? "ok" : "FAIL", CATALOG_CACHE);
    return mv ? 1 : 0;
}

void catalog_free(Catalog *cat) {
    free(cat->items);
    cat->items = NULL;
    cat->count = 0;
}
