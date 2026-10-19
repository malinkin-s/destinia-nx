/* apk.c -- minimal read-only ZIP/APK reader (stored + deflate) on top of zlib. MIT.
 * The player supplies their own DESTINIA_v1_06.apk; nothing from it is redistributed. */
#include "apk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct Apk {
    uint8_t *buf; size_t len;
    struct Ent { char *name; uint32_t method, csize, usize, off; } *ents; unsigned n;
};

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

Apk *apk_open(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    Apk *a = (Apk *)calloc(1, sizeof *a); a->buf = (uint8_t *)malloc((size_t)len); a->len = (size_t)len;
    if (fread(a->buf, 1, a->len, f) != a->len) { fclose(f); apk_close(a); return NULL; }
    fclose(f);
    size_t i = a->len >= 22 ? a->len - 22 : 0; int found = 0;
    for (;; i--) { if (rd32(a->buf + i) == 0x06054b50) { found = 1; break; } if (!i || a->len - i > 70000) break; }
    if (!found) { apk_close(a); return NULL; }
    unsigned n = rd16(a->buf + i + 10); uint32_t cd = rd32(a->buf + i + 16);
    a->ents = (struct Ent *)calloc(n, sizeof *a->ents);
    const uint8_t *p = a->buf + cd;
    for (unsigned k = 0; k < n; k++) {
        if (p + 46 > a->buf + a->len || rd32(p) != 0x02014b50) break;
        uint32_t nl = rd16(p + 28), el = rd16(p + 30), cl = rd16(p + 32);
        struct Ent *e = &a->ents[a->n++];
        e->method = rd16(p + 10); e->csize = rd32(p + 20); e->usize = rd32(p + 24); e->off = rd32(p + 42);
        e->name = (char *)malloc(nl + 1); memcpy(e->name, p + 46, nl); e->name[nl] = 0;
        p += 46 + nl + el + cl;
    }
    return a;
}

void apk_close(Apk *a) {
    if (!a) return;
    for (unsigned i = 0; i < a->n; i++) free(a->ents[i].name);
    free(a->ents); free(a->buf); free(a);
}

static struct Ent *find(Apk *a, const char *name) {
    for (unsigned i = 0; i < a->n; i++) if (!strcmp(a->ents[i].name, name)) return &a->ents[i];
    return NULL;
}

int apk_size(Apk *a, const char *name) { struct Ent *e = find(a, name); return e ? (int)e->usize : -1; }

uint8_t *apk_read(Apk *a, const char *name, size_t *out_len) {
    struct Ent *e = find(a, name); if (!e) return NULL;
    const uint8_t *lh = a->buf + e->off;
    if (rd32(lh) != 0x04034b50) return NULL;
    const uint8_t *data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
    uint8_t *out = (uint8_t *)malloc(e->usize ? e->usize : 1);
    if (e->method == 0) memcpy(out, data, e->usize);
    else if (e->method == 8) {
        z_stream z; memset(&z, 0, sizeof z);
        if (inflateInit2(&z, -15) != Z_OK) { free(out); return NULL; }
        z.next_in = (Bytef *)data; z.avail_in = e->csize; z.next_out = out; z.avail_out = e->usize;
        int rc = inflate(&z, Z_FINISH); inflateEnd(&z);
        if (rc != Z_STREAM_END) { free(out); return NULL; }
    } else { free(out); return NULL; }
    *out_len = e->usize; return out;
}
