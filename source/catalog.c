#include "catalog.h"
#include "jsonutil.h"

#include <stdlib.h>
#include <string.h>

#define CATALOG_PATH "romfs:/known_repos.json"

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

bool catalog_load(Catalog *cat) {
    cat->items = NULL;
    cat->count = 0;

    size_t len = 0;
    char *js = json_read_file(CATALOG_PATH, &len);
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

void catalog_free(Catalog *cat) {
    free(cat->items);
    cat->items = NULL;
    cat->count = 0;
}
