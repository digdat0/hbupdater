#include "net.h"
#include "config.h"
#include "fsutil.h"

#include <switch.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_AGENT "HBUpdater (libnx)"

static bool g_ready = false;
static int g_rate_remaining = -1;

/* "Authorization: Bearer <token>" when a GitHub token is configured, else "".
 * Deliberately NOT included in net_log output so it never leaks to debug.log. */
static char g_auth[320] = "";

int net_rate_remaining(void) { return g_rate_remaining; }

void net_set_auth(const char *token) {
    if (token && token[0]) {
        snprintf(g_auth, sizeof(g_auth), "Authorization: Bearer %s", token);
    } else {
        g_auth[0] = '\0';
    }
}

/* Truncate the debug log if it exceeds this size. Keeps the newest half. */
#define LOG_MAX_SIZE (512L * 1024)

static void log_rotate(void) {
    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= LOG_MAX_SIZE) { fclose(f); return; }
    long keep = LOG_MAX_SIZE / 2;
    fseek(f, sz - keep, SEEK_SET);
    char *buf = (char *)malloc((size_t)keep);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)keep, f);
    fclose(f);
    /* Drop the first partial line. */
    size_t start = 0;
    for (size_t i = 0; i < rd; i++) {
        if (buf[i] == '\n') { start = i + 1; break; }
    }
    f = fopen(LOG_PATH, "wb");
    if (f) {
        fputs("... (log rotated) ...\n", f);
        if (start < rd) fwrite(buf + start, 1, rd - start, f);
        fclose(f);
    }
    free(buf);
}

/* Append a line to the debug log so failures are diagnosable on-device. */
void net_log(const char *fmt, ...) {
    fs_mkdir_p(CONFIG_DIR);
    log_rotate();
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

bool net_init(void) {
    if (g_ready) {
        return true;
    }
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        return false;
    }
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        socketExit();
        return false;
    }
    g_ready = true;
    return true;
}

void net_exit(void) {
    if (!g_ready) {
        return;
    }
    curl_global_cleanup();
    socketExit();
    g_ready = false;
}

/*
 * devkitPro's curl uses the libnx ssl-service backend (built --with-libnx,
 * --without-mbedtls), which performs TLS through the console's `ssl` system
 * service and verifies against the console's own certificate store. So no
 * cacert.pem / mbedtls is involved; leaving curl's defaults (VERIFYPEER on)
 * uses that store. This works on real hardware; emulators that stub the ssl
 * service (e.g. Ryujinx) will fail the handshake regardless.
 */
static void apply_tls(CURL *c) {
    (void)c;
    net_log("TLS: libnx ssl backend (console cert store)");
}

struct mem_buf {
    char *data;
    size_t len;
};

static size_t mem_write(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t add = size * nmemb;
    struct mem_buf *m = (struct mem_buf *)ud;
    char *np = (char *)realloc(m->data, m->len + add + 1);
    if (!np) {
        return 0;
    }
    m->data = np;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud) {
    (void)ud;
    size_t len = size * nitems;
    if (len > 24 && strncasecmp(buf, "x-ratelimit-remaining:", 22) == 0) {
        g_rate_remaining = atoi(buf + 22);
    }
    return len;
}

char *http_get(const char *url, long *http_code, size_t *out_len) {
    CURL *c = curl_easy_init();
    if (!c) {
        return NULL;
    }
    struct mem_buf m;
    m.data = (char *)malloc(1);
    m.len = 0;
    if (m.data) {
        m.data[0] = '\0';
    }

    struct curl_slist *hdrs = NULL;
    if (g_auth[0] && strncmp(url, "https://api.github.com", 22) == 0) {
        hdrs = curl_slist_append(hdrs, g_auth);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    if (hdrs) {
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    apply_tls(c);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (http_code) {
        *http_code = code;
    }
    if (hdrs) {
        curl_slist_free_all(hdrs);
    }
    curl_easy_cleanup(c);

    net_log("GET %s -> curl=%d(%s) http=%ld len=%lu", url, (int)rc,
            curl_easy_strerror(rc), code, (unsigned long)m.len);

    if (rc != CURLE_OK) {
        free(m.data);
        return NULL;
    }
    if (out_len) {
        *out_len = m.len;
    }
    return m.data;
}

struct dl_ctx {
    FILE *fp;
    net_progress_cb cb;
    void *ud;
    uint64_t base; /* resume offset, added to curl's session-relative counts */
};

static size_t file_write(void *ptr, size_t size, size_t nmemb, void *ud) {
    struct dl_ctx *d = (struct dl_ctx *)ud;
    return fwrite(ptr, 1, size * nmemb, d->fp);
}

static int xfer_info(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    struct dl_ctx *d = (struct dl_ctx *)ud;
    if (d->cb) {
        uint64_t now = d->base + (uint64_t)dlnow;
        uint64_t total = dltotal > 0 ? d->base + (uint64_t)dltotal : 0;
        return d->cb(d->ud, now, total);
    }
    return 0;
}

bool http_download(const char *url, const char *dest_path,
                   const char *extra_header,
                   net_progress_cb cb, void *userdata,
                   uint64_t resume_from,
                   long *http_code) {
    /* Append when resuming so the existing partial file is preserved. */
    FILE *fp = fopen(dest_path, resume_from > 0 ? "ab" : "wb");
    if (!fp) {
        return false;
    }
    CURL *c = curl_easy_init();
    if (!c) {
        fclose(fp);
        return false;
    }

    struct dl_ctx d;
    d.fp = fp;
    d.cb = cb;
    d.ud = userdata;
    d.base = resume_from;

    struct curl_slist *hdrs = NULL;
    if (extra_header && extra_header[0]) {
        hdrs = curl_slist_append(hdrs, extra_header);
    }
    if (g_auth[0] && strncmp(url, "https://api.github.com", 22) == 0) {
        hdrs = curl_slist_append(hdrs, g_auth);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_info);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 90L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L); /* treat 4xx/5xx as errors */
    if (resume_from > 0) {
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE,
                         (curl_off_t)resume_from);
    }
    if (hdrs) {
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    apply_tls(c);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (http_code) {
        *http_code = code;
    }
    if (hdrs) {
        curl_slist_free_all(hdrs);
    }
    curl_easy_cleanup(c);
    fclose(fp);

    net_log("DL  %s (resume=%llu) -> curl=%d(%s) http=%ld", url,
            (unsigned long long)resume_from, (int)rc, curl_easy_strerror(rc),
            code);

    /* 416 on a resumed transfer means the server has nothing past our offset:
     * the partial file already holds the whole thing. Treat as success. */
    if (rc == CURLE_HTTP_RETURNED_ERROR && code == 416 && resume_from > 0) {
        return true;
    }
    return rc == CURLE_OK;
}
