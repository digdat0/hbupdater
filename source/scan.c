#include "scan.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SWITCH_DIR "sdmc:/switch"

/* NACP layout (see switchbrew): 16 language entries of 0x300 (name[0x200] +
 * author[0x100]) followed by fixed fields; display_version is 0x10 bytes at
 * NACP offset 0x3060. The whole struct is 0x4000 bytes. */
#define NACP_SIZE 0x4000
#define NACP_NAME_LEN 0x200
#define NACP_AUTHOR_LEN 0x100
#define NACP_VERSION_OFF 0x3060
#define NACP_VERSION_LEN 0x10

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

static bool ends_nro(const char *n) {
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".nro") == 0;
}

/* Read an NRO's appended asset section and pull name/author/version from its
 * NACP. Returns false if the file has no asset header or no NACP. */
static bool read_nacp(const char *path, ScannedApp *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    /* NRO total size lives at file offset 0x18; the asset header follows it. */
    uint32_t nro_size = 0;
    if (fseek(f, 0x18, SEEK_SET) != 0 || fread(&nro_size, 4, 1, f) != 1 ||
        nro_size < 0x80) {
        fclose(f);
        return false;
    }
    char magic[4];
    if (fseek(f, (long)nro_size, SEEK_SET) != 0 ||
        fread(magic, 1, 4, f) != 4 || memcmp(magic, "ASET", 4) != 0) {
        fclose(f);
        return false;
    }
    /* AssetHeader: icon at +0x08, nacp section {u64 off; u64 size} at +0x18. */
    uint64_t nacp_off = 0, nacp_size = 0;
    if (fseek(f, (long)nro_size + 0x18, SEEK_SET) != 0 ||
        fread(&nacp_off, 8, 1, f) != 1 || fread(&nacp_size, 8, 1, f) != 1 ||
        nacp_size < NACP_SIZE) {
        fclose(f);
        return false;
    }
    long nacp_base = (long)((uint64_t)nro_size + nacp_off);

    char nbuf[NACP_NAME_LEN + 1];
    char abuf[NACP_AUTHOR_LEN + 1];
    char vbuf[NACP_VERSION_LEN + 1];
    if (fseek(f, nacp_base, SEEK_SET) != 0 ||
        fread(nbuf, 1, NACP_NAME_LEN, f) != NACP_NAME_LEN ||
        fread(abuf, 1, NACP_AUTHOR_LEN, f) != NACP_AUTHOR_LEN) {
        fclose(f);
        return false;
    }
    if (fseek(f, nacp_base + NACP_VERSION_OFF, SEEK_SET) != 0 ||
        fread(vbuf, 1, NACP_VERSION_LEN, f) != NACP_VERSION_LEN) {
        fclose(f);
        return false;
    }
    fclose(f);
    nbuf[NACP_NAME_LEN] = '\0';
    abuf[NACP_AUTHOR_LEN] = '\0';
    vbuf[NACP_VERSION_LEN] = '\0';

    sset(out->name, sizeof(out->name), nbuf);
    sset(out->author, sizeof(out->author), abuf);
    sset(out->version, sizeof(out->version), vbuf);
    return out->name[0] != '\0';
}

static ScannedApp *grow(ScannedApp *arr, int cnt, int *cap) {
    if (cnt < *cap) {
        return arr;
    }
    int nc = *cap ? *cap * 2 : 16;
    ScannedApp *n = (ScannedApp *)realloc(arr, (size_t)nc * sizeof(ScannedApp));
    if (!n) {
        return arr; /* keep what we have; caller stops growing */
    }
    *cap = nc;
    return n;
}

static ScannedApp *try_file(const char *path, ScannedApp *arr, int *cnt,
                            int *cap) {
    ScannedApp tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!read_nacp(path, &tmp)) {
        return arr;
    }
    sset(tmp.path, sizeof(tmp.path), path);
    arr = grow(arr, *cnt, cap);
    if (*cnt < *cap) {
        arr[*cnt] = tmp;
        (*cnt)++;
    }
    return arr;
}

ScannedApp *scan_switch(int *count) {
    *count = 0;
    ScannedApp *arr = NULL;
    int cnt = 0, cap = 0;

    DIR *d = opendir(SWITCH_DIR);
    if (!d) {
        return NULL;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue; /* skip ., .., .overlays, album, etc. */
        }
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", SWITCH_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            DIR *sd = opendir(p);
            if (!sd) {
                continue;
            }
            struct dirent *se;
            while ((se = readdir(sd)) != NULL) {
                if (se->d_name[0] == '.' || !ends_nro(se->d_name)) {
                    continue;
                }
                char sp[2048];
                snprintf(sp, sizeof(sp), "%s/%s", p, se->d_name);
                struct stat ss;
                if (stat(sp, &ss) == 0 && S_ISREG(ss.st_mode)) {
                    arr = try_file(sp, arr, &cnt, &cap);
                }
            }
            closedir(sd);
        } else if (S_ISREG(st.st_mode) && ends_nro(e->d_name)) {
            arr = try_file(p, arr, &cnt, &cap);
        }
    }
    closedir(d);

    *count = cnt;
    return arr;
}
