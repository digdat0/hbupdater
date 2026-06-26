#include "unzip.h"
#include "fsutil.h"

#include <zlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ZIP signatures. */
#define SIG_EOCD 0x06054b50u /* end of central directory */
#define SIG_CEN  0x02014b50u /* central directory file header */
#define SIG_LOC  0x04034b50u /* local file header */

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Build dest = root + name, rejecting absolute paths and ".." traversal.
 * Avoids snprintf-into-fixed (which trips -Wformat-truncation under -Werror). */
static bool safe_join(char *out, size_t outsz, const char *root,
                      const char *name) {
    if (!name[0] || name[0] == '/' || name[0] == '\\') {
        return false;
    }
    const char *p = name;
    while (*p) {
        const char *slash = strpbrk(p, "/\\");
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    size_t rl = strlen(root), nl = strlen(name);
    if (rl + nl + 1 > outsz) {
        return false;
    }
    memcpy(out, root, rl);
    memcpy(out + rl, name, nl + 1);
    for (char *q = out; *q; q++) {
        if (*q == '\\') {
            *q = '/';
        }
    }
    return true;
}

/* Inflate `comp` bytes (raw deflate) to the open file. */
static bool inflate_to_file(const unsigned char *cdata, uint32_t comp,
                            FILE *of) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return false;
    }
    zs.next_in = (Bytef *)cdata;
    zs.avail_in = comp;
    unsigned char out[16384];
    bool ok = true;
    int zr;
    do {
        zs.next_out = out;
        zs.avail_out = sizeof(out);
        zr = inflate(&zs, Z_NO_FLUSH);
        if (zr != Z_OK && zr != Z_STREAM_END) {
            ok = false;
            break;
        }
        size_t have = sizeof(out) - zs.avail_out;
        if (have && fwrite(out, 1, have, of) != have) {
            ok = false;
            break;
        }
    } while (zr != Z_STREAM_END);
    inflateEnd(&zs);
    return ok;
}

bool unzip_extract(const char *zip_path, const char *dest_root,
                   unzip_progress_cb cb, void *ud, unzip_prewrite_cb prewrite,
                   void *prewrite_ud, int *n_extracted) {
    if (n_extracted) {
        *n_extracted = 0;
    }
    FILE *f = fopen(zip_path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 22 || fsz > (512L * 1024 * 1024)) { /* min EOCD .. sanity cap */
        fclose(f);
        return false;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)fsz);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    /* Locate the end-of-central-directory record (scan back; comment <= 64K). */
    long eocd = -1;
    long start = fsz - 22;
    long limit = start > 65557 ? start - 65557 : 0;
    for (long i = start; i >= limit; i--) {
        if (rd32(buf + i) == SIG_EOCD) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(buf);
        return false;
    }
    uint16_t total = rd16(buf + eocd + 10);
    uint32_t cdoff = rd32(buf + eocd + 16);
    if (total == 0xFFFF || cdoff == 0xFFFFFFFFu) { /* ZIP64 unsupported */
        free(buf);
        return false;
    }
    if ((long)cdoff >= fsz) {
        free(buf);
        return false;
    }

    bool ok = true;
    int done = 0;
    long p = (long)cdoff;
    for (int e = 0; e < total && ok; e++) {
        if (p + 46 > fsz || rd32(buf + p) != SIG_CEN) {
            ok = false;
            break;
        }
        uint16_t flag = rd16(buf + p + 8);
        uint16_t method = rd16(buf + p + 10);
        uint32_t comp = rd32(buf + p + 20);
        uint32_t uncomp = rd32(buf + p + 24);
        uint16_t nlen = rd16(buf + p + 28);
        uint16_t elen = rd16(buf + p + 30);
        uint16_t clen = rd16(buf + p + 32);
        uint32_t lho = rd32(buf + p + 42);
        const char *name = (const char *)(buf + p + 46);

        char nm[512];
        size_t cpy = nlen < sizeof(nm) - 1 ? nlen : sizeof(nm) - 1;
        memcpy(nm, name, cpy);
        nm[cpy] = '\0';
        p += 46 + nlen + elen + clen; /* advance to next central entry */
        done++;

        /* Skip ZIP64 sentinels and encrypted entries we can't handle. */
        if (comp == 0xFFFFFFFFu || uncomp == 0xFFFFFFFFu ||
            lho == 0xFFFFFFFFu || (flag & 0x1)) {
            if (cb && cb(ud, done, total)) {
                ok = false;
                break;
            }
            continue;
        }

        bool isdir = (cpy > 0 && (nm[cpy - 1] == '/' || nm[cpy - 1] == '\\'));
        char dest[1024];
        if (!safe_join(dest, sizeof(dest), dest_root, nm)) {
            if (cb && cb(ud, done, total)) {
                ok = false;
                break;
            }
            continue; /* unsafe entry skipped */
        }
        if (isdir) {
            fs_mkdir_p(dest);
            if (cb && cb(ud, done, total)) {
                ok = false;
                break;
            }
            continue;
        }
        /* Let the caller back up an existing file before we overwrite it. */
        if (prewrite && prewrite(prewrite_ud, dest)) {
            ok = false;
            break;
        }
        fs_ensure_parent(dest);

        if ((long)lho + 30 > fsz || rd32(buf + lho) != SIG_LOC) {
            ok = false;
            break;
        }
        uint16_t lnlen = rd16(buf + lho + 26);
        uint16_t lelen = rd16(buf + lho + 28);
        long dataoff = (long)lho + 30 + lnlen + lelen;
        if (dataoff + (long)comp > fsz) {
            ok = false;
            break;
        }
        const unsigned char *cdata = buf + dataoff;

        FILE *of = fopen(dest, "wb");
        if (!of) {
            ok = false;
            break;
        }
        if (method == 0) { /* stored */
            if (comp && fwrite(cdata, 1, comp, of) != comp) {
                ok = false;
            }
        } else if (method == 8) { /* deflate */
            ok = inflate_to_file(cdata, comp, of);
        } else {
            ok = false; /* unsupported compression method */
        }
        fclose(of);
        if (!ok) {
            break;
        }
        if (cb && cb(ud, done, total)) {
            ok = false;
            break;
        }
    }

    free(buf);
    if (n_extracted) {
        *n_extracted = done;
    }
    return ok;
}
