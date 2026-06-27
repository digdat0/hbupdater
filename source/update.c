#include "update.h"
#include "net.h"
#include "jsonutil.h"

#include <switch.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void parse_ver(const char *s, int *a, int *b, int *c) {
    while (*s && !(*s >= '0' && *s <= '9')) {
        s++; /* skip leading 'v' or other prefix */
    }
    *a = *b = *c = 0;
    sscanf(s, "%d.%d.%d", a, b, c);
}

int version_cmp(const char *a, const char *b) {
    int a1, b1, c1, a2, b2, c2;
    parse_ver(a, &a1, &b1, &c1);
    parse_ver(b, &a2, &b2, &c2);
    if (a1 != a2) {
        return a1 < a2 ? -1 : 1;
    }
    if (b1 != b2) {
        return b1 < b2 ? -1 : 1;
    }
    if (c1 != c2) {
        return c1 < c2 ? -1 : 1;
    }
    return 0;
}

/* Case-insensitive glob match supporting '*' and '?'. Iterative to avoid
 * stack overflow on patterns with many '*' characters. */
static bool wmatch(const char *p, const char *s) {
    const char *pback = NULL, *sback = NULL;
    while (*s) {
        if (*p == '*') {
            pback = ++p;
            sback = s;
            continue;
        }
        if (*p == '?' ||
            tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
            p++;
            s++;
            continue;
        }
        if (pback) {
            p = pback;
            s = ++sback;
            continue;
        }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

/* Pick the download URL (and byte size) of the asset matching `pat` on one
 * release object (token index `rel`). Falls back to the first asset sharing the
 * glob's file extension. Writes into out (empty if none). */
static void pick_asset_url(const char *body, const jsmntok_t *tok, int rel,
                           const char *pat, char *out, size_t out_sz,
                           uint64_t *out_size) {
    out[0] = '\0';
    if (out_size) {
        *out_size = 0;
    }
    char fallback[1024] = "";
    uint64_t fallback_size = 0;
    const char *ext = strrchr(pat, '.'); /* ".nro" / ".ovl" / NULL */

    int ai = json_obj_get(body, tok, rel, "assets");
    if (ai < 0 || tok[ai].type != JSMN_ARRAY) {
        return;
    }
    int cnt = tok[ai].size;
    int child = ai + 1;
    for (int i = 0; i < cnt; i++) {
        if (tok[child].type == JSMN_OBJECT) {
            char name[256];
            json_copy(body, tok, json_obj_get(body, tok, child, "name"), name,
                      sizeof(name));
            if (name[0]) {
                if (wmatch(pat, name)) {
                    json_copy(body, tok,
                              json_obj_get(body, tok, child,
                                           "browser_download_url"),
                              out, out_sz);
                    if (out_size) {
                        *out_size = json_u64(
                            body, tok, json_obj_get(body, tok, child, "size"));
                    }
                    return; /* exact glob match wins */
                }
                if (ext && !fallback[0]) {
                    size_t ln = strlen(name), el = strlen(ext);
                    if (ln >= el && strcasecmp(name + ln - el, ext) == 0) {
                        json_copy(body, tok,
                                  json_obj_get(body, tok, child,
                                               "browser_download_url"),
                                  fallback, sizeof(fallback));
                        fallback_size = json_u64(
                            body, tok, json_obj_get(body, tok, child, "size"));
                    }
                }
            }
        }
        child = json_tok_skip(tok, child);
    }
    if (fallback[0]) {
        snprintf(out, out_sz, "%s", fallback);
        if (out_size) {
            *out_size = fallback_size;
        }
    }
}

bool update_fetch_latest(const char *repo, const char *asset_pat,
                         bool allow_prerelease, char *tag, size_t tag_sz,
                         char *url, size_t url_sz, uint64_t *asset_size) {
    tag[0] = '\0';
    url[0] = '\0';
    if (asset_size) {
        *asset_size = 0;
    }
    const char *pat = (asset_pat && asset_pat[0]) ? asset_pat : "*.nro";

    /* Use the releases *list*, not /releases/latest: the latter has been seen
     * returning intermittent 504s, and it relies on GitHub's "latest" flag
     * (which bulk-migrating releases can leave pointing at an older tag). We
     * fetch the list and pick the highest version ourselves, with retries for
     * transient transport/5xx errors. */
    char api[256];
    snprintf(api, sizeof(api),
             "https://api.github.com/repos/%s/releases?per_page=100", repo);

    char *body = NULL;
    long code = 0;
    size_t len = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        body = http_get(api, &code, &len);
        if (body && code == 200 && len >= 2) {
            break; /* success */
        }
        /* Only transport errors and 5xx are worth retrying. A 4xx is definitive:
         * 404 (not found) and 403 (rate-limited / abuse) will never change on
         * retry, and re-firing them just burns GitHub's 60/hr limit ~3x faster.
         * Stop immediately so one bad/rate-limited repo costs one request. */
        bool retryable = (!body) || (code == 0) || (code >= 500);
        free(body);
        body = NULL;
        if (!retryable) {
            break;
        }
        if (attempt < 2) {
            svcSleepThread(700000000ULL); /* ~0.7s before retrying */
        }
    }
    if (!body) {
        return false;
    }

    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(body, len, &ntok);
    if (!tok || tok[0].type != JSMN_ARRAY) {
        free(tok);
        free(body);
        return false;
    }

    char best_tag[64] = "";
    char best_url[1024] = "";
    uint64_t best_size = 0;
    int nrel = tok[0].size;
    int rel = 1;
    for (int r = 0; r < nrel; r++) {
        if (tok[rel].type != JSMN_OBJECT) {
            rel = json_tok_skip(tok, rel);
            continue;
        }
        bool draft =
            json_bool(body, tok, json_obj_get(body, tok, rel, "draft"));
        bool pre =
            json_bool(body, tok, json_obj_get(body, tok, rel, "prerelease"));
        bool skip = draft || (pre && !allow_prerelease);
        char rtag[64] = "";
        char rurl[1024] = "";
        uint64_t rsize = 0;
        if (!skip) {
            json_copy(body, tok, json_obj_get(body, tok, rel, "tag_name"), rtag,
                      sizeof(rtag));
            pick_asset_url(body, tok, rel, pat, rurl, sizeof(rurl), &rsize);
        }
        if (!skip && rtag[0] && rurl[0] &&
            (!best_tag[0] || version_cmp(rtag, best_tag) > 0)) {
            snprintf(best_tag, sizeof(best_tag), "%s", rtag);
            snprintf(best_url, sizeof(best_url), "%s", rurl);
            best_size = rsize;
        }
        rel = json_tok_skip(tok, rel);
    }

    free(tok);
    free(body);

    if (!best_tag[0] || !best_url[0]) {
        return false;
    }
    snprintf(tag, tag_sz, "%s", best_tag);
    snprintf(url, url_sz, "%s", best_url);
    if (asset_size) {
        *asset_size = best_size;
    }
    return true;
}
