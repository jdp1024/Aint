/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* archive.c - AIN archive container: header, records, spans, list/extract/test/create. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fs.h"
#include "ain.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ---------------- small helpers ---------------- */

unsigned ain_quiet_level;

void ain_status(unsigned threshold, const char *format, ...)
{
    va_list args;
    if (ain_quiet_level >= threshold)
        return;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static void progress_print(const AinProgress *p, unsigned percent, uint64_t bytes)
{
    if (ain_quiet_level >= 1)
        return;
    fprintf(stderr, "%s: %u%% (%llu/%llu bytes)\n", p->label, percent,
            (unsigned long long)bytes, (unsigned long long)p->total);
}

void ain_progress_begin(AinProgress *p, const char *label, uint64_t total)
{
    p->label = label;
    p->total = total;
    p->processed = 0;
    p->reported_bytes = 0;
    p->percent = 0;
    p->reported_percent = 0;
    progress_print(p, 0, 0);
}

void ain_progress_advance(AinProgress *p, size_t bytes)
{
    p->processed += bytes;
    while (p->total && p->percent < 99) {
        unsigned next = p->percent + 1;
        /* ceil(total * next / 100), without multiplying the full total. */
        uint64_t threshold = (p->total / 100) * next +
            ((p->total % 100) * next + 99) / 100;
        if (p->processed < threshold)
            break;
        p->percent = next;
    }
    if (p->processed - p->reported_bytes > 1024 * 1024 &&
        p->percent > p->reported_percent) {
        progress_print(p, p->percent, p->processed);
        p->reported_bytes = p->processed;
        p->reported_percent = p->percent;
    }
}

void ain_progress_finish(AinProgress *p)
{
    p->percent = 100;
    progress_print(p, 100, p->processed);
}

int ain_size_summary(const char *path, uint64_t uncompressed, double started,
                     AinProgress *progress)
{
    FILE *f = fopen(path, "rb");
    char first_volume[1100];
    uint8_t header[AIN_HDR_SIZE];
    uint64_t compressed = 0;
    uint32_t size;
    double finished;
    if (!f) {
        char base[1024];
        size_t len = strlen(path);
        snprintf(base, sizeof base, "%s", path);
        if (len >= 4 && !ain_stricmp(base + len - 4, ".AIN"))
            base[len - 4] = 0;
        snprintf(first_volume, sizeof first_volume, "%s.AIN", base);
        f = fopen(first_volume, "rb");
        if (!f) {
            snprintf(first_volume, sizeof first_volume, "%s.001", base);
            f = fopen(first_volume, "rb");
        }
        if (f)
            path = first_volume;
    }
    if (!f) {
        fprintf(stderr, "error: cannot read archive size: %s\n", path);
        return -1;
    }
    if (fread(header, 1, sizeof header, f) != sizeof header) {
        fprintf(stderr, "error: cannot read archive header: %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (header[2] == AIN_FLAG_VOLS) {
        char base[1024], volume[1100];
        size_t len = strlen(path);
        unsigned i;
        int first_is_ain = len >= 4 &&
                           !ain_stricmp(path + len - 4, ".AIN");
        snprintf(base, sizeof base, "%s", path);
        if (len >= 4)
            base[len - 4] = 0;
        for (i = 0; i < header[7]; ++i) {
            if (first_is_ain && i == 0)
                snprintf(volume, sizeof volume, "%s.AIN", base);
            else
                snprintf(volume, sizeof volume, "%s.%03u", base,
                         first_is_ain ? i : i + 1);
            if (ain_fs_file_size(volume, &size))
                return -1;
            compressed += size;
        }
    } else if ((header[3] & AIN_FLAG_MORE_VOLUMES) &&
               header[6] == 0 && header[7] == 0) {
        char base[1024], volume[1100];
        size_t len = strlen(path);
        unsigned i;
        snprintf(base, sizeof base, "%s", path);
        if (len >= 4 && !ain_stricmp(base + len - 4, ".AIN"))
            base[len - 4] = 0;
        for (i = 0; i < 999; ++i) {
            int isdir, exists;
            if (i == 0)
                snprintf(volume, sizeof volume, "%s.AIN", base);
            else
                snprintf(volume, sizeof volume, "%s.%03u", base, i);
            exists = ain_fs_probe(volume, &isdir);
            if (exists < 0)
                return -1;
            if (!exists) {
                if (i == 0) {
                    fprintf(stderr, "error: missing first volume %s\n", volume);
                    return -1;
                }
                break;
            }
            if (ain_fs_file_size(volume, &size)) {
                return -1;
            }
            compressed += size;
        }
    } else {
        if (ain_fs_file_size(path, &size))
            return -1;
        compressed = size;
    }
    finished = ain_fs_time();
    if (finished < 0)
        return -1;
    fflush(stdout);
    ain_progress_finish(progress);
    fflush(stderr);
    if (ain_quiet_level >= 2)
        return 0;
    printf("---\nTotal processed: %llu bytes, compressed: %llu bytes, ratio: ",
           (unsigned long long)uncompressed, (unsigned long long)compressed);
    if (uncompressed)
        printf("%.2f%%", 100.0 * (double)compressed / (double)uncompressed);
    else
        printf("N/A");
    printf(", elapsed: %.2f s\n", finished - started);
    return 0;
}

static uint16_t sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    size_t i;
    for (i = 0; i < n; i++)
        s += p[i];
    return (uint16_t)(s & 0xFFFF);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---------------- open / parse ---------------- */

static int ain_parse_common(AinArchive *a)
{
    a->method = a->img[1] & 0x0F;
    a->garbled = (a->img[3] & AIN_FLAG_GARBLE) != 0;
    a->count = get16(a->img + 8);
    a->idxoff = get32(a->img + 14);
    if (a->idxoff >= a->n || a->method < 1 || a->method > 4)
        return -5;
    return 0;
}

static void vol_base(char *base, size_t n, const char *path)
{
    size_t bl = strlen(path);
    snprintf(base, n, "%s", path);
    if (bl >= 4 && base[bl - 4] == '.' && isdigit((unsigned char)base[bl - 1]) &&
        isdigit((unsigned char)base[bl - 2]) && isdigit((unsigned char)base[bl - 3]))
        base[bl - 4] = 0;                       /* strip ".NNN" volume suffix */
    else if (bl >= 4 && !ain_stricmp(base + bl - 4, ".AIN"))
        base[bl - 4] = 0;                       /* strip ".AIN"               */
}

int ain_open(AinArchive *a, const char *path)
{
    FILE *f;
    long sz;
    uint8_t h[AIN_HDR_SIZE];
    memset(a, 0, sizeof *a);
    ain_buf_init(&a->idx);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return -2; }

    if (h[0] != AIN_MAGIC) { fclose(f); return -4; }

    if (h[2] == AIN_FLAG_VOLS) {
        /* multi-volume: concatenate <base>.AIN and <base>.001..NNN */
        uint32_t frag;
        int gflag = (h[3] & AIN_FLAG_GARBLE) != 0;
        unsigned nv, k;
        int first_is_ain;
        char base[1024], vol[1100];
        AinBuf ib;
        /* garbled sets keep header[4..5] for the password hash: the
         * fragment size is then recovered from volume 1's file size    */
        frag = gflag ? 0 : ((uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                            ((uint32_t)h[6] << 16));
        nv = h[7];
        fclose(f);
        if (nv < 2 || (!gflag && frag < 32))
            return -5;
        vol_base(base, sizeof base, path);
        {
            size_t len = strlen(path);
            FILE *first;
            first_is_ain = len >= 4 &&
                           !ain_stricmp(path + len - 4, ".AIN");
            if (!first_is_ain) {
                snprintf(vol, sizeof vol, "%s.AIN", base);
                first = fopen(vol, "rb");
                if (first) {
                    uint8_t first_header[AIN_HDR_SIZE];
                    first_is_ain =
                        fread(first_header, 1, sizeof first_header, first) ==
                            sizeof first_header &&
                        first_header[0] == AIN_MAGIC &&
                        first_header[2] == AIN_FLAG_VOLS;
                    fclose(first);
                }
            }
        }
        ain_buf_init(&ib);
        for (k = 0; k < nv; k++) {
            FILE *g;
            long gs;
            if (first_is_ain && k == 0)
                snprintf(vol, sizeof vol, "%s.AIN", base);
            else
                snprintf(vol, sizeof vol, "%s.%03u", base,
                         first_is_ain ? k : k + 1);
            g = fopen(vol, "rb");
            if (!g) {
                fprintf(stderr, "error: need volume %s (volume %u of %u)\n",
                        vol, k + 1, nv);
                ain_buf_free(&ib);
                return -1;
            }
            fseek(g, 0, SEEK_END);
            gs = ftell(g);
            fseek(g, 0, SEEK_SET);
            if (gflag && k == 0) {
                frag = (uint32_t)gs;         /* derived: header bytes are busy */
                if (frag < 32) {
                    fclose(g);
                    ain_buf_free(&ib);
                    return -5;
                }
            }
            if (k + 1 < nv && (unsigned long)gs != frag) {
                fprintf(stderr, "error: %s is %ld bytes, expected %lu\n",
                        vol, gs, (unsigned long)frag);
                fclose(g);
                ain_buf_free(&ib);
                return -6;
            }
            if (ain_buf_reserve(&ib, (size_t)gs) ||
                fread(ib.p + ib.len, 1, (size_t)gs, g) != (size_t)gs) {
                fclose(g);
                ain_buf_free(&ib);
                return -3;
            }
            ib.len += (size_t)gs;
            fclose(g);
        }
        a->img = ib.p;
        a->n = ib.len;
        a->path = path;
        a->frag = frag;
        a->nvols = nv;
        if (ain_parse_common(a)) {
            ain_close(a);
            return -5;
        }
        return 0;
    }

    /* single volume */
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < AIN_HDR_SIZE) { fclose(f); return -2; }
    a->img = (uint8_t *)malloc((size_t)sz);
    if (!a->img || fread(a->img, 1, (size_t)sz, f) != (size_t)sz) {
        free(a->img);
        a->img = NULL;
        fclose(f);
        return -3;
    }
    fclose(f);
    a->n = (size_t)sz;
    a->path = path;
    a->nvols = 1;
    if (ain_parse_common(a)) {
        ain_close(a);
        return -5;
    }
    return 0;
}

int ain_header_check(AinArchive *a)
{
    uint16_t want = (uint16_t)(sum16(a->img, 22) ^ AIN_HDRXOR);
    if (want != get16(a->img + 22))
        return -1;
    if ((a->img[3] & AIN_FLAG_MORE_VOLUMES) && a->idxoff == 0)
        return 0;
    if (sum16(a->img + a->idxoff, a->n - a->idxoff) != get16(a->img + 18))
        return -2;
    return 0;
}

/* ---------------- garble (password) ---------------- */

int ain_set_password(AinArchive *a, const char *pw)
{
    uint16_t seed[3];
    if (!a->garbled)
        return 0;                        /* plain archive: nothing to do   */
    if (a->gseed_ok)
        return 0;                        /* already unlocked               */
    ain_garble_seed(pw, seed);
    if ((uint16_t)(seed[0] + seed[1] + seed[2]) != get16(a->img + 4))
        return -1;                       /* the original: Incorrect password */
    memcpy(a->gseed, seed, sizeof seed);
    a->gseed_ok = 1;
    /* one continuous keystream over the whole data region (byte 0 = first
     * data byte, runs across member boundaries); the index is not covered.
     * In garbled archives member checksums cover the *encrypted* bytes, so
     * keep a copy of the encrypted image for integrity comparisons       */
    if (a->idxoff > AIN_HDR_SIZE) {
        a->enc = (uint8_t *)malloc(a->n);
        if (!a->enc) {
            fprintf(stderr, "error: cannot allocate archive checksum data\n");
            a->gseed_ok = 0;
            return -1;
        }
        memcpy(a->enc, a->img, a->idxoff);
        ain_garble_xor(seed, a->img + AIN_HDR_SIZE, a->idxoff - AIN_HDR_SIZE);
    }
    return 0;
}

static int ain_load_index_single(AinArchive *a)
{
    int rc, i;
    size_t p, end;
    rc = ain_decode_stream(a->img + a->idxoff, a->n - a->idxoff, &a->idx);
    if (rc)
        return rc;
    a->recs = (AinRec *)calloc(a->count ? a->count : 1, sizeof(AinRec));
    if (!a->recs)
        return -6;
    p = 0;
    end = a->idx.len;
    for (i = 0; i < (int)a->count && p + AIN_REC_SIZE <= end; i++) {
        const uint8_t *r = a->idx.p + p;
        size_t q = p + AIN_REC_SIZE;
        const char *name;
        if (q >= end)
            break;
        name = (const char *)(a->idx.p + q);
        while (q < end && a->idx.p[q])
            q++;
        if (q >= end)
            break;
        q++;
        if (q < end)
            q++;
        a->recs[i].attr = r[0];
        a->recs[i].ts = get32(r + 1);
        a->recs[i].orig = get32(r + 5);
        a->recs[i].comp = get32(r + 9);
        a->recs[i].off = get32(r + 13);
        a->recs[i].f22 = get32(r + 22);
        a->recs[i].chk = get16(r + 27);
        a->recs[i].name = name;
        a->nrecs = i + 1;
        p = q;
    }
    return 0;
}

static int ain_open_volume_image(AinArchive *a, const char *path)
{
    FILE *f;
    long sz;
    memset(a, 0, sizeof *a);
    ain_buf_init(&a->idx);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < AIN_HDR_SIZE ||
        fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return -2;
    }
    a->img = (uint8_t *)malloc((size_t)sz);
    if (!a->img || fread(a->img, 1, (size_t)sz, f) != (size_t)sz) {
        free(a->img);
        a->img = NULL;
        fclose(f);
        return -3;
    }
    fclose(f);
    a->n = (size_t)sz;
    a->path = path;
    a->nvols = 1;
    return ain_parse_common(a);
}

static int ain_load_original_stream_volumes(AinArchive *a)
{
    char base[1024], path[1100];
    AinBuf stream, index, img;
    AinArchive local;
    unsigned volume, nvols = 1;
    int local_open = 0, rc = -1, more;
    uint32_t count = a->count;
    uint32_t fragment_size = a->n <= 0xFFFFFFul ? (uint32_t)a->n : 0;
    int method = a->method;
    int garbled = a->garbled;
    uint8_t *enc = NULL;
    size_t new_idxoff;

    if (garbled && !a->gseed_ok) {
        fprintf(stderr, "error: password required for original multi-volume archive\n");
        return -1;
    }
    if (ain_header_check(a)) {
        fprintf(stderr, "error: invalid first volume header in %s\n", a->path);
        return -1;
    }
    vol_base(base, sizeof base, a->path);
    ain_buf_init(&stream);
    ain_buf_init(&index);
    ain_buf_init(&img);
    if (a->n > AIN_HDR_SIZE &&
        ain_buf_put(&stream, a->img + AIN_HDR_SIZE, a->n - AIN_HDR_SIZE))
        goto done;

    for (volume = 1; volume < 999; volume++) {
        if (local_open) {
            ain_close(&local);
            local_open = 0;
        }
        snprintf(path, sizeof path, "%s.%03u", base, volume);
        if (ain_open_volume_image(&local, path)) {
            fprintf(stderr, "error: missing or invalid volume %s\n", path);
            goto done;
        }
        local_open = 1;
        if (local.img[2] == AIN_FLAG_VOLS || get16(local.img + 6) != volume ||
            local.method != method || local.count != count ||
            local.garbled != garbled || ain_header_check(&local)) {
            fprintf(stderr, "error: invalid original volume sequence at %s\n", path);
            goto done;
        }
        more = (local.img[3] & AIN_FLAG_MORE_VOLUMES) != 0;
        if (more) {
            if (local.idxoff != 0 || local.n <= AIN_HDR_SIZE ||
                ain_buf_put(&stream, local.img + AIN_HDR_SIZE,
                            local.n - AIN_HDR_SIZE))
                goto done;
        } else {
            if (local.idxoff <= AIN_HDR_SIZE || local.idxoff >= local.n ||
                ain_buf_put(&stream, local.img + AIN_HDR_SIZE,
                            local.idxoff - AIN_HDR_SIZE) ||
                ain_buf_put(&index, local.img + local.idxoff,
                            local.n - local.idxoff)) {
                fprintf(stderr, "error: invalid final volume index in %s\n", path);
                goto done;
            }
            nvols++;
            break;
        }
        nvols++;
    }
    if (volume == 999) {
        fprintf(stderr, "error: original volume set exceeds 999 parts\n");
        goto done;
    }
    if (stream.len > UINT32_MAX - AIN_HDR_SIZE) {
        fprintf(stderr, "error: original volume stream is too large\n");
        goto done;
    }
    new_idxoff = AIN_HDR_SIZE + stream.len;
    if (ain_buf_reserve(&img, new_idxoff + index.len))
        goto done;
    {
        uint8_t header[AIN_HDR_SIZE];
        uint16_t checksum;
        memcpy(header, a->img, sizeof header);
        header[3] &= (uint8_t)~AIN_FLAG_MORE_VOLUMES;
        put32(header + 14, (uint32_t)new_idxoff);
        checksum = sum16(index.p, index.len);
        header[18] = (uint8_t)checksum;
        header[19] = (uint8_t)(checksum >> 8);
        checksum = (uint16_t)(sum16(header, 22) ^ AIN_HDRXOR);
        header[22] = (uint8_t)checksum;
        header[23] = (uint8_t)(checksum >> 8);
        if (ain_buf_put(&img, header, sizeof header) ||
            (stream.len && ain_buf_put(&img, stream.p, stream.len)) ||
            ain_buf_put(&img, index.p, index.len))
            goto done;
    }
    if (garbled) {
        enc = (uint8_t *)malloc(new_idxoff);
        if (!enc) {
            fprintf(stderr, "error: cannot allocate original volume checksum data\n");
            goto done;
        }
        memcpy(enc, img.p, new_idxoff);
        ain_garble_xor(a->gseed, img.p + AIN_HDR_SIZE, stream.len);
    }
    free(a->img);
    free(a->enc);
    a->img = img.p;
    a->n = img.len;
    img.p = NULL;
    img.len = img.cap = 0;
    a->idxoff = (uint32_t)new_idxoff;
    a->nvols = (uint16_t)nvols;
    a->native_volumes = 1;
    a->native_method = (uint8_t)method;
    a->native_comp = stream.len;
    a->frag = fragment_size;
    a->enc = enc;
    enc = NULL;
    rc = 0;
done:
    if (local_open)
        ain_close(&local);
    free(enc);
    ain_buf_free(&stream);
    ain_buf_free(&index);
    ain_buf_free(&img);
    return rc;
}

static int ain_decode_original_volume(AinArchive *v, AinBuf *out)
{
    int i = 0, rc = 0;
    AinBuf stream;
    ain_buf_init(&stream);
    while (i < v->nrecs) {
        AinRec *first = &v->recs[i];
        int standalone = first->f22 == AIN_F_STANDALONE ||
                         (first->f22 & 0x18) == 0;
        int last = i;
        size_t expected = 0;
        uint32_t off, comp;
        uint16_t chk;
        if (standalone) {
            expected = first->orig;
            off = first->off;
            comp = first->comp;
        } else {
            while (last < v->nrecs &&
                   !(v->recs[last].f22 & AIN_F_SPAN_END)) {
                if (expected > SIZE_MAX - v->recs[last].orig) {
                    rc = -1;
                    goto done;
                }
                expected += v->recs[last].orig;
                last++;
            }
            if (last >= v->nrecs) {
                rc = -1;
                goto done;
            }
            if (expected > SIZE_MAX - v->recs[last].orig) {
                rc = -1;
                goto done;
            }
            expected += v->recs[last].orig;
            off = first->off;
            comp = v->recs[last].comp;
        }
        if (off < AIN_HDR_SIZE || off > v->idxoff ||
            comp > v->idxoff - off) {
            rc = -1;
            goto done;
        }
        chk = sum16((v->enc ? v->enc : v->img) + off, comp);
        if (chk != v->recs[last].chk) {
            fprintf(stderr, "error: %s: volume data checksum mismatch\n", v->path);
            rc = -1;
            goto done;
        }
        stream.len = 0;
        if (v->method == AIN_M_STORE) {
            if (comp != expected ||
                ain_buf_put(&stream, v->img + off, comp)) {
                rc = -1;
                goto done;
            }
        } else if (ain_decode_stream_sized(v->img + off, comp, &stream, expected)) {
            fprintf(stderr, "error: %s: cannot decode volume data stream\n", v->path);
            rc = -1;
            goto done;
        }
        if (stream.len != expected ||
            (stream.len && ain_buf_put(out, stream.p, stream.len))) {
            rc = -1;
            goto done;
        }
        i = last + 1;
    }
done:
    ain_buf_free(&stream);
    return rc;
}

static int ain_append_fragment(AinEnt **ents, int *nents, int *capacity,
                               const AinRec *r, const uint8_t *data)
{
    int i;
    AinEnt *e;
    for (i = 0; i < *nents; i++)
        if (!ain_stricmp((*ents)[i].name, r->name))
            break;
    if (i == *nents) {
        AinEnt *grown;
        int cap = *capacity ? *capacity * 2 : 16;
        if (*nents >= 65535)
            return -1;
        if (*nents == *capacity) {
            grown = (AinEnt *)realloc(*ents, (size_t)cap * sizeof **ents);
            if (!grown)
                return -1;
            *ents = grown;
            *capacity = cap;
        }
        e = &(*ents)[(*nents)++];
        memset(e, 0, sizeof *e);
        e->name = (char *)malloc(strlen(r->name) + 1);
        if (!e->name)
            return -1;
        strcpy(e->name, r->name);
        e->ts = r->ts;
        e->attr = r->attr;
    } else {
        e = &(*ents)[i];
    }
    if (r->orig > UINT32_MAX - e->n)
        return -1;
    if (r->orig) {
        uint8_t *grown = (uint8_t *)realloc(e->data, e->n + r->orig);
        if (!grown)
            return -1;
        e->data = grown;
        memcpy(e->data + e->n, data, r->orig);
        e->n += r->orig;
    }
    return 0;
}

static int ain_load_original_volumes(AinArchive *a)
{
    char base[1024], path[1100];
    AinEnt *ents = NULL;
    int nents = 0, capacity = 0;
    unsigned volume, nvols = 0;
    int method = a->method, rc = -1, more;
    uint64_t compressed = 0;
    uint32_t fragment_size = a->n <= 0xFFFFFFul ? (uint32_t)a->n : 0;
    uint32_t archive_ts = get32(a->img + 10);
    AinBuf data, rawidx, idx, img;
    AinArchive local;
    int local_open = 0;
    AinArchive *v = a;
    int garbled = a->garbled;
    AinGarbleSt cipher;
    uint8_t *encrypted = NULL;

    if (garbled && !a->gseed_ok) {
        fprintf(stderr, "error: password required for original multi-volume archive\n");
        return -1;
    }
    vol_base(base, sizeof base, a->path);
    ain_buf_init(&data);
    ain_buf_init(&rawidx);
    ain_buf_init(&idx);
    ain_buf_init(&img);

    for (volume = 0; volume < 999; volume++) {
        int i;
        uint64_t expected = 0;
        AinBuf decoded;
        if (volume) {
            snprintf(path, sizeof path, "%s.%03u", base, volume);
            if (ain_open_volume_image(&local, path)) {
                fprintf(stderr, "error: missing or invalid volume %s\n", path);
                goto done;
            }
            local_open = 1;
            v = &local;
        }
        if (v->img[2] == AIN_FLAG_VOLS ||
            (get16(v->img + 6) != volume) || v->method != method ||
            v->garbled != garbled ||
            v->idxoff < AIN_HDR_SIZE || v->idxoff >= v->n ||
            ain_header_check(v) || ain_load_index_single(v)) {
            fprintf(stderr, "error: invalid original volume %s (sequence %u)\n",
                    v->path, volume);
            goto done;
        }
        if (volume && garbled) {
            if (get16(v->img + 4) != get16(a->img + 4)) {
                fprintf(stderr, "error: inconsistent volume password in %s\n", v->path);
                goto done;
            }
            v->enc = (uint8_t *)malloc(v->n);
            if (!v->enc) {
                fprintf(stderr, "error: cannot allocate volume checksum data\n");
                goto done;
            }
            memcpy(v->enc, v->img, v->n);
            ain_garble_xor(a->gseed, v->img + AIN_HDR_SIZE,
                           v->idxoff - AIN_HDR_SIZE);
        }
        if (v->nrecs != (int)v->count) {
            fprintf(stderr, "error: incomplete index in original volume %s\n", v->path);
            goto done;
        }
        compressed += v->idxoff - AIN_HDR_SIZE;
        nvols++;
        for (i = 0; i < v->nrecs; i++) {
            if (expected > UINT32_MAX - v->recs[i].orig) {
                fprintf(stderr, "error: volume member data is too large in %s\n", v->path);
                goto done;
            }
            expected += v->recs[i].orig;
        }
        ain_buf_init(&decoded);
        if (ain_decode_original_volume(v, &decoded)) {
            ain_buf_free(&decoded);
            fprintf(stderr, "error: invalid member stream in original volume %s\n", v->path);
            goto done;
        }
        if (decoded.len != expected) {
            fprintf(stderr, "error: volume output size mismatch in %s\n", v->path);
            ain_buf_free(&decoded);
            goto done;
        }
        {
            size_t pos = 0;
            for (i = 0; i < v->nrecs; i++) {
                AinRec *r = &v->recs[i];
                const uint8_t *fragment = r->orig ? decoded.p + pos : NULL;
                if (r->orig > decoded.len - pos ||
                    ain_append_fragment(&ents, &nents, &capacity, r, fragment)) {
                    ain_buf_free(&decoded);
                    fprintf(stderr, "error: cannot assemble volume member %s\n", r->name);
                    goto done;
                }
                pos += r->orig;
            }
        }
        ain_buf_free(&decoded);
        more = (v->img[3] & AIN_FLAG_MORE_VOLUMES) != 0;
        if (local_open) {
            ain_close(&local);
            local_open = 0;
        }
        v = a;
        if (!more)
            break;
    }
    if (volume == 999) {
        fprintf(stderr, "error: original volume set exceeds 999 parts\n");
        goto done;
    }
    if (!nvols || !nents) {
        fprintf(stderr, "error: original volume set contains no members\n");
        goto done;
    }

    if (garbled)
        ain_garble_init(&cipher, a->gseed);
    for (volume = 0; volume < (unsigned)nents; volume++) {
        uint8_t record[AIN_REC_SIZE];
        uint32_t off;
        size_t name_len = strlen(ents[volume].name);
        uint16_t chk;
        if (ents[volume].n > UINT32_MAX ||
            data.len > UINT32_MAX - AIN_HDR_SIZE ||
            ents[volume].n > UINT32_MAX - AIN_HDR_SIZE - data.len ||
            name_len > 1024) {
            fprintf(stderr, "error: normalized volume archive is too large\n");
            goto done;
        }
        off = (uint32_t)(AIN_HDR_SIZE + data.len);
        if (ents[volume].n &&
            ain_buf_put(&data, ents[volume].data, ents[volume].n)) {
            fprintf(stderr, "error: cannot allocate normalized member data\n");
            goto done;
        }
        if (garbled && ents[volume].n)
            ain_garble_run(&cipher, data.p + off - AIN_HDR_SIZE, ents[volume].n);
        chk = sum16(ents[volume].n ? data.p + off - AIN_HDR_SIZE : NULL,
                    ents[volume].n);
        memset(record, 0, sizeof record);
        record[0] = ents[volume].attr;
        put32(record + 1, ents[volume].ts);
        put32(record + 5, (uint32_t)ents[volume].n);
        put32(record + 9, (uint32_t)ents[volume].n);
        put32(record + 13, off);
        put32(record + 22, AIN_F_STANDALONE);
        record[27] = (uint8_t)chk;
        record[28] = (uint8_t)(chk >> 8);
        if (ain_buf_put(&rawidx, record, sizeof record) ||
            ain_buf_put(&rawidx, ents[volume].name, name_len + 1) ||
            ain_buf_putc(&rawidx, 0)) {
            fprintf(stderr, "error: cannot allocate normalized volume archive\n");
            goto done;
        }
    }
    if (ain_encode_stream(rawidx.p, rawidx.len, &idx, 2) ||
        data.len > UINT32_MAX - AIN_HDR_SIZE ||
        ain_buf_reserve(&img, AIN_HDR_SIZE + data.len + idx.len)) {
        fprintf(stderr, "error: cannot encode normalized volume index\n");
        goto done;
    }
    {
        uint8_t header[AIN_HDR_SIZE] = { 0 };
        uint32_t idxoff = (uint32_t)(AIN_HDR_SIZE + data.len);
        uint16_t hc;
        header[0] = AIN_MAGIC;
        header[1] = (uint8_t)(0x10 | AIN_M_STORE);
        if (garbled) {
            header[3] = AIN_FLAG_GARBLE;
            memcpy(header + 4, a->img + 4, 2);
        }
        header[8] = (uint8_t)nents;
        header[9] = (uint8_t)(nents >> 8);
        put32(header + 10, archive_ts);
        put32(header + 14, idxoff);
        {
            uint16_t ic = sum16(idx.p, idx.len);
            header[18] = (uint8_t)ic;
            header[19] = (uint8_t)(ic >> 8);
        }
        hc = (uint16_t)(sum16(header, 22) ^ AIN_HDRXOR);
        header[22] = (uint8_t)hc;
        header[23] = (uint8_t)(hc >> 8);
        if (ain_buf_put(&img, header, sizeof header) ||
            ain_buf_put(&img, data.p, data.len) ||
            ain_buf_put(&img, idx.p, idx.len)) {
            fprintf(stderr, "error: cannot allocate normalized volume archive\n");
            goto done;
        }
    }
    if (garbled) {
        encrypted = (uint8_t *)malloc(img.len);
        if (!encrypted) {
            fprintf(stderr, "error: cannot allocate normalized checksum data\n");
            goto done;
        }
        memcpy(encrypted, img.p, img.len);
        ain_garble_xor(a->gseed, img.p + AIN_HDR_SIZE, data.len);
    }
    free(a->img);
    free(a->enc);
    a->img = img.p;
    a->n = img.len;
    img.p = NULL;
    img.len = img.cap = 0;
    a->method = AIN_M_STORE;
    a->garbled = garbled;
    a->count = (uint32_t)nents;
    a->idxoff = (uint32_t)(AIN_HDR_SIZE + data.len);
    a->native_volumes = 1;
    a->native_method = (uint8_t)method;
    a->native_comp = compressed;
    a->frag = fragment_size;
    a->nvols = (uint16_t)nvols;
    a->enc = encrypted;
    encrypted = NULL;
    ain_buf_free(&a->idx);
    ain_buf_init(&a->idx);
    free(a->recs);
    a->recs = NULL;
    a->nrecs = 0;
    rc = 0;
done:
    free(encrypted);
    if (local_open)
        ain_close(&local);
    ain_free_entries(ents, nents);
    ain_buf_free(&data);
    ain_buf_free(&rawidx);
    ain_buf_free(&idx);
    ain_buf_free(&img);
    return rc;
}

int ain_load_index(AinArchive *a)
{
    int rc;
    if (!a->native_volumes && a->nvols == 1 &&
        (a->img[3] & AIN_FLAG_MORE_VOLUMES) && get16(a->img + 6) == 0) {
        if (a->idxoff == 0)
            rc = ain_load_original_stream_volumes(a);
        else if (a->idxoff >= AIN_HDR_SIZE)
            rc = ain_load_original_volumes(a);
        else
            return -5;
        if (rc)
            return rc;
    }
    return ain_load_index_single(a);
}

void ain_close(AinArchive *a)
{
    free(a->img);
    free(a->enc);
    ain_buf_free(&a->idx);
    free(a->recs);
    memset(a, 0, sizeof *a);
}

/* ---------------- DOS time helpers ---------------- */

static void unpack_dostime(uint32_t v, char *buf, size_t n)
{
    unsigned t = v & 0xFFFF, d = v >> 16;
    snprintf(buf, n, "%04u-%02u-%02u %02u:%02u:%02u",
             1980 + (d >> 9), (d >> 5) & 15, d & 31,
             t >> 11, (t >> 5) & 63, (t & 31) * 2);
}

/* ---------------- path helpers ---------------- */

static int mkdir_p(const char *path)    /* creates parent dirs of this file path */
{
    char buf[1024];
    char *p;
    size_t n = strlen(path);
    int rc;
    if (n >= sizeof buf) {
        fprintf(stderr, "error: path too long: %s\n", path);
        return -1;
    }
    memcpy(buf, path, n + 1);
    p = (char *)ain_fs_basename(buf);
    if (p > buf)
        --p;
    else
        return 0;
    if (p == buf)
        return 0;
    *p = 0;
    p = buf;
#ifdef _WIN32
    /* A UNC server/share is a root, not a directory to create. */
    if (buf[0] == '\\' && buf[1] == '\\') {
        p = strchr(buf + 2, '\\');
        if (!p)
            return 0;
        p = strchr(p + 1, '\\');
        if (!p)
            return 0;
        ++p;
    }
#endif
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = 0;
            /* skip only a true drive root ("C:\"), never a 1-char dir */
            if (buf[0] && !(p == buf + 2 && buf[1] == ':')) {
                rc = ain_fs_mkdir(buf);
                if (rc < 0)
                    return -1;
                if (!rc)
                    ain_status(1, "Creating directory %s\n", buf);
            }
            *p = save;
        }
    }
    rc = ain_fs_mkdir(buf);
    if (!rc)
        ain_status(1, "Creating directory %s\n", buf);
    return rc < 0 ? -1 : 0;
}

static void member_path(const char *dest, const char *name, char *out, size_t n,
                        int flat)
{
    const char *rel = name;
    char tmp[1024];
    int driveabs = 0;
    if (isalpha((unsigned char)rel[0]) && rel[1] == ':') {
        if (rel[2] == '\\' || rel[2] == '/')
            driveabs = 1;            /* drive-qualified absolute: X:\... */
        rel += 2;                    /* drive-relative X:FILE -> FILE    */
    }
    if (flat) {
        /* like the original's e command: base name only, everywhere */
        const char *b = rel + strlen(rel);
        while (b > rel && b[-1] != '\\' && b[-1] != '/')
            b--;
        rel = b;
        if (dest && *dest)
            snprintf(tmp, sizeof tmp, "%s" AIN_PATH_SEP "%s", dest, rel);
        else
            snprintf(tmp, sizeof tmp, "%s", rel);
    } else if (driveabs && !(dest && *dest)) {
#ifdef _WIN32
        /* Preserve the original tool's drive-qualified restoration. */
        snprintf(tmp, sizeof tmp, "%s", name);
#else
        while (*rel == '\\' || *rel == '/')
            ++rel;
        snprintf(tmp, sizeof tmp, "%s", rel);
#endif
    } else {
        while (*rel == '\\' || *rel == '/')
            rel++;
        if (dest && *dest)
            snprintf(tmp, sizeof tmp, "%s" AIN_PATH_SEP "%s", dest, rel);
        else
            snprintf(tmp, sizeof tmp, "%s", rel);
    }
    ain_fs_native_path(tmp);
    snprintf(out, n, "%s", tmp);
}

/* ---------------- extract / test core ---------------- */

static int write_output(const char *path, const uint8_t *data, size_t n,
                        uint32_t ts, uint8_t attr, AinProgress *progress)
{
    FILE *f;
    if (mkdir_p(path))
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    while (n) {
        size_t take = n < 65536 ? n : 65536;
        size_t written = fwrite(data, 1, take, f);
        if (written)
            ain_progress_advance(progress, written);
        if (written != take) { fclose(f); return -2; }
        data += take;
        n -= take;
    }
    if (fclose(f))
        return -2;
    if ((attr & 2) && ain_fs_set_hidden(path))
        return -3;
    if (ts && ain_fs_set_dostime(path, ts))
        return -3;
    return 0;
}

typedef int (*FileVisitor)(AinArchive *a, AinRec *r, const uint8_t *data, size_t n, void *ctx);

static int visit_files(AinArchive *a, FileVisitor cb, void *ctx, int with_data)
{
    int i = 0, rc = 0;
    AinBuf scratch, span;
    ain_buf_init(&scratch);
    ain_buf_init(&span);
    while (i < a->nrecs) {
        AinRec *r = &a->recs[i];
        int standalone = (r->f22 == AIN_F_STANDALONE) || ((r->f22 & 0x18) == 0);
        if (standalone) {
            if (with_data) {
                const uint8_t *d = NULL;
                size_t n = 0;
                scratch.len = 0;
                if (r->off + r->comp > a->n) { rc = -(100 + i); goto out; }
                if (a->method == AIN_M_STORE) {
                    d = a->img + r->off;
                    n = r->comp;
                } else {
                    rc = ain_decode_stream(a->img + r->off, r->comp, &scratch);
                    if (rc) { rc = -(100 + i); goto out; }
                    d = scratch.p;
                    n = scratch.len;
                }
                if (n != r->orig) { rc = -(200 + i); goto out; }
                rc = cb(a, r, d, n, ctx);
            } else {
                rc = cb(a, r, NULL, 0, ctx);
            }
            if (rc) goto out;
            i++;
            continue;
        }
        /* solid span beginning */
        {
            int j = i;
            size_t total_orig = 0, pos = 0;
            const uint8_t *blob = NULL;
            uint32_t comp = 0;
            while (j < a->nrecs) {
                total_orig += a->recs[j].orig;
                if (a->recs[j].f22 & AIN_F_SPAN_END) {
                    comp = a->recs[j].comp;
                    break;
                }
                j++;
            }
            if (j >= a->nrecs) { rc = -7; goto out; }     /* unterminated span */
            if (with_data) {
                int k;
                if (r->off + comp > a->n) { rc = -8; goto out; }
                span.len = 0;
                if (a->method == AIN_M_STORE) {
                    if (comp != total_orig) { rc = -(200 + i); goto out; }
                    blob = a->img + r->off;
                } else {
                    rc = ain_decode_stream(a->img + r->off, comp, &span);
                    if (rc) { rc = -(100 + i); goto out; }
                    if (span.len != total_orig) { rc = -(200 + i); goto out; }
                    blob = span.p;
                }
                for (k = i; k <= j; k++) {
                    AinRec *m = &a->recs[k];
                    rc = cb(a, m, blob + pos, m->orig, ctx);
                    if (rc) goto out;
                    pos += m->orig;
                }
            } else {
                int k;
                for (k = i; k <= j; k++) {
                    rc = cb(a, &a->recs[k], NULL, 0, ctx);
                    if (rc) goto out;
                }
            }
            i = j + 1;
        }
    }
out:
    ain_buf_free(&scratch);
    ain_buf_free(&span);
    return rc;
}

/* ---------------- public operations ---------------- */

/* DOS-style case-insensitive wildcard match: '*' spans separators, '?' any */
int ain_match(const char *pat, const char *str)
{
    if (*pat == '*') {
        if (ain_match(pat + 1, str))
            return 1;
        if (!*str)
            return 0;
        return ain_match(pat, str + 1);
    }
    if (!*pat)
        return !*str;
    if (!*str)
        return 0;
    {
        char p = (char)toupper((unsigned char)*pat);
        char s = (char)toupper((unsigned char)*str);
        if (p == '/')
            p = '\\';
        if (s == '/')
            s = '\\';
        if (p != s && p != '?')
            return 0;
    }
    return ain_match(pat + 1, str + 1);
}

static const char *basename_of(const char *name)
{
    const char *b = name;
    const char *p;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

int ain_filter_match(const char *pat, const char *base, const char *name)
{
    char full[1024];
    char p2[1100];
    const char *q;
    char *e;
    int haspath;
    if (base && *base) {
        size_t nb = strlen(base);
        snprintf(full, sizeof full, "%s%s%s", base,
                 (nb && base[nb - 1] != '\\' && base[nb - 1] != '/') ? "\\" : "",
                 pat);
        pat = full;
    }
    /* work on a copy without trailing separator */
    snprintf(p2, sizeof p2, "%s", pat);
    e = p2 + strlen(p2);
    while (e > p2 && (e[-1] == '\\' || e[-1] == '/'))
        *--e = 0;
    pat = p2;
    if (ain_match(pat, name))
        return 1;
    haspath = strchr(pat, '\\') || strchr(pat, '/') || strchr(pat, ':');
    /* directory-prefix semantics: "sub" also matches "sub\x\y" */
    {
        char pfx[1100];
        snprintf(pfx, sizeof pfx, "%s\\*", pat);
        if (ain_match(pfx, name))
            return 1;
        if (!haspath) {
            if (ain_match(pat, basename_of(name)))
                return 1;
        }
        /* suffix matching at path-component boundaries:
         * "\BC\BIN\*.*" or "sub" hit "C:\BC\BIN\X.EXE" / "x\sub\f" */
        for (q = name + 1; *q; q++)
            if (*q == '\\' || *q == '/') {
                if (ain_match(pat, q + 1))
                    return 1;
                if (ain_match(pfx, q + 1))
                    return 1;
            }
    }
    return 0;
}

static int any_match(const char **pats, int npats, const char *name)
{
    int i;
    if (npats <= 0)
        return 1;
    for (i = 0; i < npats; i++)
        if (ain_filter_match(pats[i], NULL, name))
            return 1;
    return 0;
}

int ain_list(AinArchive *a, FILE *out, int verbose,
             const char **pats, int npats)
{
    int i, shown = 0;
    int display_method = a->native_volumes ? a->native_method : a->method;
    uint32_t tbytes = 0, cbytes = 0;
    fprintf(out, "Archive %s\n", a->path);
    fprintf(out, "%d files, method %d%s%s%s",
            a->nrecs, display_method, a->garbled ? ", GARBLED (password)" : "",
            a->native_volumes ? ", original multi-volume" : "",
            npats > 0 ? " (filtered)" : "");
    if (a->native_volumes)
        fprintf(out, " (%u volumes)", a->nvols);
    fputc('\n', out);
    for (i = 0; i < a->nrecs; i++) {
        AinRec *r = &a->recs[i];
        char tbuf[32];
        int havec = !a->native_volumes &&
                    (r->f22 == AIN_F_STANDALONE || (r->f22 & AIN_F_SPAN_END));
        if (!any_match(pats, npats, r->name))
            continue;
        shown++;
        tbytes += r->orig;
        if (havec)
            cbytes += r->comp;
        unpack_dostime(r->ts, tbuf, sizeof tbuf);
        if (verbose) {
            char cbuf[32];
            snprintf(cbuf, sizeof cbuf, havec ? "%u" : "-", r->comp);
            fprintf(out, "%9u %9s %04X %s  %s\n", r->orig, cbuf, r->chk, tbuf, r->name);
        } else if (havec)
            fprintf(out, "%9u %9u %s  %s\n", r->orig, r->comp, tbuf, r->name);
        else
            fprintf(out, "%9u %9s %s  %s\n", r->orig, "-", tbuf, r->name);
    }
    if (a->native_volumes)
        fprintf(out, "%d files shown, %u bytes uncompressed, %llu compressed bytes across volumes\n",
                shown, tbytes, (unsigned long long)a->native_comp);
    else
        fprintf(out, "%d files shown, %u bytes stored %u\n", shown, tbytes, cbytes);
    return 0;
}

int ain_analyze(AinArchive *a, FILE *out, const char **pats, int npats)
{
    const uint8_t *h = a->img;
    const uint8_t *stored = a->garbled ? a->enc : a->img;
    uint16_t hc = (uint16_t)(sum16(h, 22) ^ AIN_HDRXOR);
    uint16_t ic = sum16(h + a->idxoff, a->n - a->idxoff);
    AinBuf decoded;
    char timebuf[32];
    int i, rc = 0, streams = 0;
    size_t total_orig = 0, total_comp = 0;

    ain_buf_init(&decoded);
    fprintf(out, "Archive %s\nImage: %zu bytes, %u volume(s), fragment=%u bytes\n",
            a->path, a->n, a->nvols, a->frag);
    if (a->native_volumes)
        fprintf(out, "Normalized original AIN volume set: %u volumes, source method %u\n",
                a->nvols, a->native_method);
    fprintf(out, "Header (offsets in bytes; multibyte fields little-endian):\n  Raw:");
    for (i = 0; i < AIN_HDR_SIZE; i++)
        fprintf(out, " %02X", h[i]);
    fprintf(out, "\n  [00]    magic: 0x%02X\n", h[0]);
    fprintf(out, "  [01]    method/format: 0x%02X; method=%d (%s), upper bits=0x%02X\n",
            h[1], a->method, a->method == AIN_M_STORE ? "stored" : "LZ77/Huffman",
            h[1] & 0xF0);
    fprintf(out, "  [02]    volume marker: 0x%02X (%s)\n", h[2],
            h[2] == AIN_FLAG_VOLS ? "split" : "single/other");
    fprintf(out, "  [03]    flags: 0x%02X; garbled=%s, other bits=0x%02X\n",
            h[3], a->garbled ? "yes" : "no", h[3] & ~AIN_FLAG_GARBLE);
    fprintf(out, "  [04-07] auxiliary bytes: %02X %02X %02X %02X\n",
            h[4], h[5], h[6], h[7]);
    if (a->garbled)
        fprintf(out, "           [04-05] password hash: 0x%04X\n", get16(h + 4));
    if (a->frag)
        fprintf(out, "           fragment size=%u (%s); [07] volumes=%u\n",
                a->frag, a->garbled ? "derived from volume 1" : "u24 at [04-06]", h[7]);
    fprintf(out, "  [08-09] member count: %u (parsed=%d)\n", a->count, a->nrecs);
    unpack_dostime(get32(h + 10), timebuf, sizeof timebuf);
    fprintf(out, "  [10-13] DOS timestamp: 0x%08X (%s)\n", get32(h + 10), timebuf);
    fprintf(out, "  [14-17] index offset: %u (0x%08X)\n", a->idxoff, a->idxoff);
    fprintf(out, "  [18-19] index checksum: 0x%04X; computed=0x%04X %s\n",
            get16(h + 18), ic, ic == get16(h + 18) ? "OK" : "MISMATCH");
    fprintf(out, "  [20-21] unknown/reserved: %02X %02X\n", h[20], h[21]);
    fprintf(out, "  [22-23] header checksum: 0x%04X; computed=0x%04X %s\n",
            get16(h + 22), hc, hc == get16(h + 22) ? "OK" : "MISMATCH");
    fprintf(out, "  Checksums: byte sum modulo 65536; header sum XOR 0x5555.\n");
    if (hc != get16(h + 22) || ic != get16(h + 18)) {
        fprintf(stderr, "error: %s: header/index checksum mismatch\n", a->path);
        rc = -1;
    }
    if (a->idxoff < AIN_HDR_SIZE || a->nrecs != (int)a->count) {
        fprintf(stderr, "error: %s: invalid data offset or incomplete index\n", a->path);
        rc = -1;
        goto done;
    }
    fprintf(out, "\nIndex stream: bytes [%u,%zu), encoded=%zu decoded=%zu bytes\n",
            a->idxoff, a->n, a->n - a->idxoff, a->idx.len);
    if (ain_analyze_stream(h + a->idxoff, a->n - a->idxoff, &decoded, out)) {
        fprintf(stderr, "error: %s: cannot analyze index stream\n", a->path);
        rc = -1;
        goto done;
    }
    if (a->garbled && (!a->gseed_ok || (a->idxoff > AIN_HDR_SIZE && !stored))) {
        fprintf(stderr, "error: %s: unlocked data and ciphertext required for analysis\n", a->path);
        rc = -1;
        goto done;
    }
    fprintf(out, "\nData streams%s (byte ranges in reassembled image; bit ranges per stream):\n",
            npats ? " (filtered; matching members include their entire solid span)" : "");
    for (i = 0; i < a->nrecs; ) {
        AinRec *first = &a->recs[i], *last;
        int j = i, k, selected = 0;
        int standalone = first->f22 == AIN_F_STANDALONE || !(first->f22 & 0x18);
        size_t orig = 0, comp, off = first->off;
        uint16_t chk;
        if (!standalone) {
            while (j < a->nrecs && !(a->recs[j].f22 & AIN_F_SPAN_END))
                j++;
            if (j == a->nrecs) {
                fprintf(stderr, "error: %s: unterminated solid span at member %d\n",
                        a->path, i + 1);
                rc = -1;
                goto done;
            }
        }
        last = &a->recs[j];
        comp = last->comp;
        for (k = i; k <= j; k++) {
            orig += a->recs[k].orig;
            if (any_match(pats, npats, a->recs[k].name))
                selected = 1;
        }
        if (!selected) {
            i = j + 1;
            continue;
        }
        if (off < AIN_HDR_SIZE || off > a->idxoff || comp > a->idxoff - off) {
            fprintf(stderr, "error: %s: member %d stream outside data region\n", a->path, i + 1);
            rc = -1;
            goto done;
        }
        streams++;
        total_orig += orig;
        total_comp += comp;
        chk = sum16((stored ? stored : h) + off, comp);
        fprintf(out, "\nStream %d: %s, members %d..%d, bytes [%zu,%zu),"
                " encoded=%zu decoded=%zu bytes\n",
                streams, a->method == AIN_M_STORE ? "stored" :
                standalone ? "standalone LZ77/Huffman" : "solid LZ77/Huffman",
                i + 1, j + 1, off, off + comp, comp, orig);
        fprintf(out, "  Checksum%s: stored=0x%04X computed=0x%04X %s\n",
                a->garbled ? " (ciphertext)" : "", last->chk, chk,
                last->chk == chk ? "OK" : "MISMATCH");
        if (last->chk != chk) {
            fprintf(stderr, "error: %s: member %d stream checksum mismatch\n", a->path, i + 1);
            rc = -1;
        }
        {
            size_t pos = 0;
            for (k = i; k <= j; k++) {
                AinRec *r = &a->recs[k];
                unpack_dostime(r->ts, timebuf, sizeof timebuf);
                fprintf(out, "  Member %d: %s%s\n"
                        "    decoded=[%zu,%zu) orig=%u comp=%u off=%u"
                        " flags=0x%08X checksum=0x%04X attr=0x%02X time=%s\n",
                        k + 1, r->name, npats && any_match(pats, npats, r->name) ? " (matched)" : "",
                        pos, pos + r->orig, r->orig, r->comp, r->off,
                        r->f22, r->chk, r->attr, timebuf);
                pos += r->orig;
            }
        }
        decoded.len = 0;
        if (a->method == AIN_M_STORE) {
            fprintf(out, "  Stored data: no Huffman blocks or LZ77 tokens.\n");
            if (comp != orig) {
                fprintf(stderr, "error: %s: stored stream size mismatch\n", a->path);
                rc = -1;
            }
        } else if (ain_analyze_stream(h + off, comp, &decoded, out)) {
            fprintf(stderr, "error: %s: cannot analyze member %d stream\n", a->path, i + 1);
            rc = -1;
            goto done;
        } else if (decoded.len != orig) {
            fprintf(stderr, "error: %s: decoded stream size mismatch (expected %zu, got %zu)\n",
                    a->path, orig, decoded.len);
            rc = -1;
        }
        i = j + 1;
    }
    fprintf(out, "\nAnalyzed %d data stream(s): encoded=%zu decoded=%zu bytes\n",
            streams, total_comp, total_orig);
done:
    ain_buf_free(&decoded);
    if (ferror(out)) {
        fprintf(stderr, "error: cannot write stream analysis\n");
        rc = -1;
    }
    return rc;
}

typedef struct {
    const char *dest;
    const char **pats;
    int npats;
    int nout;
    int flat;               /* e: base name only (original's flatten) */
    int overwrite;          /* 0: ask, 1: all yes, -1: all no */
    AinProgress progress;
} ExtractCtx;

/* Returns 1 to overwrite, 0 to skip, -1 to quit, -2 on input failure. */
static int confirm_overwrite(const char *path, ExtractCtx *ec)
{
    char answer[32], *p, *end;
    int ch, complete;
    if (ec->overwrite)
        return ec->overwrite > 0;
    for (;;) {
        printf("%s exists. Overwrite? [Y/N/AY/AN/Q] ", path);
        fflush(stdout);
        if (!fgets(answer, sizeof answer, stdin)) {
            fprintf(stderr, "error: cannot read overwrite answer for %s\n", path);
            return -2;
        }
        complete = strchr(answer, '\n') != NULL || feof(stdin);
        if (!complete) {
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
        }
        p = answer;
        while (isspace((unsigned char)*p))
            p++;
        end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1]))
            *--end = 0;
        if (complete) {
            if (!ain_stricmp(p, "Y"))
                return 1;
            if (!ain_stricmp(p, "N"))
                return 0;
            if (!ain_stricmp(p, "AY")) {
                ec->overwrite = 1;
                return 1;
            }
            if (!ain_stricmp(p, "AN")) {
                ec->overwrite = -1;
                return 0;
            }
            if (!ain_stricmp(p, "Q"))
                return -1;
        }
        fprintf(stderr, "Please answer Y, N, AY, AN, or Q.\n");
    }
}

static int extract_cb(AinArchive *a, AinRec *r, const uint8_t *data, size_t n, void *vctx)
{
    ExtractCtx *ec = (ExtractCtx *)vctx;
    char path[1024];
    char *fullpath;
    int exists, isdir;
    (void)a;
    if (!any_match(ec->pats, ec->npats, r->name))
        return 0;
    member_path(ec->dest, r->name, path, sizeof path, ec->flat);
    exists = ain_fs_probe(path, &isdir);
    if (exists < 0)
        return 1;
    if (exists) {
        int answer = confirm_overwrite(path, ec);
        if (answer == -1)
            return 2;
        if (answer == -2)
            return 3;
        if (!answer) {
            ain_progress_advance(&ec->progress, n);
            return 0;
        }
    }
    if (write_output(path, data, n, r->ts, r->attr, &ec->progress)) {
        fprintf(stderr, "error: cannot write %s\n", path);
        return 1;
    }
    fullpath = ain_fs_fullpath(path);
    if (!fullpath)
        return 1;
    if (ec->flat) {
        /* the original's e reports just the base name */
        const char *b = strrchr(r->name, '\\');
        const char *s2 = strrchr(r->name, '/');
        if (s2 && (!b || s2 > b))
            b = s2;
        ain_status(1, "Extracting %s to %s (%u%%)\n", b ? b + 1 : r->name, fullpath,
               ec->progress.percent);
    } else {
        ain_status(1, "Extracting %s to %s (%u%%)\n", r->name, fullpath, ec->progress.percent);
    }
    free(fullpath);
    ec->nout++;
    return 0;
}

int ain_extract(AinArchive *a, const char *dest, const char **pats, int npats,
                int flat, int yes)
{
    int rc, i;
    uint64_t total = 0;
    ExtractCtx ec;
    if (a->garbled && !a->gseed_ok) {
        fprintf(stderr, "error: %s is garble-protected; supply -g<password>\n",
                a->path);
        return -20;
    }
    ec.dest = dest;
    ec.pats = pats;
    ec.npats = npats;
    ec.nout = 0;
    ec.flat = flat;
    ec.overwrite = yes ? 1 : 0;
    for (i = 0; i < a->nrecs; ++i)
        if (any_match(pats, npats, a->recs[i].name))
            total += a->recs[i].orig;
    ain_progress_begin(&ec.progress, "Extracting", total);
    rc = visit_files(a, extract_cb, &ec, 1);
    if (rc < 0)
        fprintf(stderr, "error: archive corrupted (code %d)\n", rc);
    else if (rc == 2)
        fprintf(stderr, "Extraction cancelled.\n");
    else if (!rc) {
        ain_progress_finish(&ec.progress);
        ain_status(2, "%d files processed\n", ec.nout);
    }
    return rc;
}

typedef struct {
    const char **pats;
    int npats;
    int nverified;
} TestCtx;

static int test_cb(AinArchive *a, AinRec *r, const uint8_t *data, size_t n, void *vctx)
{
    TestCtx *tc = (TestCtx *)vctx;
    (void)a; (void)data; (void)n;
    if (!any_match(tc->pats, tc->npats, r->name))
        return 0;
    tc->nverified++;
    ain_status(1, "OK   %s\n", r->name);
    return 0;
}

int ain_test(AinArchive *a, const char **pats, int npats)
{
    int rc, i;
    TestCtx tc;
    if (a->garbled && !a->gseed_ok) {
        fprintf(stderr, "error: %s is garble-protected; supply -g<password>\n",
                a->path);
        return -20;
    }
    if (ain_header_check(a))
        fprintf(stderr, "warning: header checksum mismatch\n");
    /* garbled archives: stored checksums cover the encrypted bytes       */
    {
        const uint8_t *base = a->enc ? a->enc : a->img;
        for (i = 0; i < a->nrecs; i++) {
            AinRec *r = &a->recs[i];
            if (r->f22 == AIN_F_STANDALONE && r->comp) {
                if (r->off + r->comp <= a->n && sum16(base + r->off, r->comp) != r->chk)
                    fprintf(stderr, "warning: checksum mismatch for %s\n", r->name);
            } else if (r->f22 & AIN_F_SPAN_END) {
                uint32_t start = 0;
                int j;
                for (j = i; j >= 0; j--)
                    if (a->recs[j].f22 & AIN_F_SPAN_START) { start = a->recs[j].off; break; }
                if (start + r->comp <= a->n && sum16(base + start, r->comp) != r->chk)
                    fprintf(stderr, "warning: span checksum mismatch at %s\n", r->name);
            }
        }
    }
    tc.pats = pats;
    tc.npats = npats;
    tc.nverified = 0;
    rc = visit_files(a, test_cb, &tc, 1);
    if (rc) {
        fprintf(stderr, "error: integrity check FAILED (code %d)\n", rc);
        return rc;
    }
    ain_status(2, "All %d files verified\n", tc.nverified);
    return 0;
}

/* ---------------- entry-based writer (create / update / delete) -------- */

static int write_indexed_volumes(const char *archpath, int method,
                                 const AinEnt *ents, const char **names,
                                 const uint32_t *sizes, int n,
                                 unsigned long long frag, int speed,
                                 const char *pw, unsigned threads,
                                 AinProgress *progress);

int ain_write(const char *archpath, int method, const AinEnt *ents, int n,
              unsigned long long frag, int speed, const char *pw)
{
    return ain_write_mt(archpath, method, ents, n, frag, speed, pw, 1);
}

int ain_write_mt(const char *archpath, int method, const AinEnt *ents, int n,
                 unsigned long long frag, int speed, const char *pw,
                 unsigned threads)
{
    FILE *f;
    AinBuf data, idx, recs;
    uint32_t *offs;
    int i, rc = 0, g_on = 0;
    uint16_t gseed[3] = { 0 };
    uint32_t ts_now;
    uint8_t h[AIN_HDR_SIZE];

    if (n < 0 || n > 65535 || method < 1 || method > 4 ||
        speed < 1 || speed > 3 || threads > 256)
        return -1;
    if (frag && n)
        return write_indexed_volumes(archpath, method, ents, NULL, NULL, n, frag,
                                     speed, pw, threads, NULL);

    ain_buf_init(&data);
    ain_buf_init(&idx);
    ain_buf_init(&recs);
    offs = (uint32_t *)calloc(n ? (size_t)n : 1, sizeof(uint32_t));
    if (!offs)
        return -2;

    /* concatenate raw file contents */
    for (i = 0; i < n; i++) {
        if (ents[i].n > UINT32_MAX - AIN_HDR_SIZE ||
            data.len > UINT32_MAX - AIN_HDR_SIZE - ents[i].n) {
            fprintf(stderr, "error: archive data exceeds the 32-bit format limit\n");
            rc = -5;
            goto out;
        }
        offs[i] = (uint32_t)data.len;
        if (ain_buf_reserve(&data, ents[i].n)) { rc = -5; goto out; }
        if (ents[i].n)
            memcpy(data.p + data.len, ents[i].data, ents[i].n);
        data.len += ents[i].n;
    }

    if (method != AIN_M_STORE) {
        /* one solid stream over all file contents (as original -m1/-m2/-m3) */
        AinBuf raw = data;
        ain_buf_init(&data);
        if (threads == 1) {
            rc = ain_encode_stream(raw.p, raw.len, &data, speed);
        } else {
            AinEncS *encoder = ain_enc_open_mt(&data, speed,
                                              speed <= 2 ? 65536 : 2097152,
                                              threads);
            rc = encoder ? ain_enc_feed(encoder, raw.p, raw.len) : -1;
            if (!rc)
                rc = ain_enc_finish(encoder);
            ain_enc_free(encoder);
        }
        ain_buf_free(&raw);
        if (rc) {
            fprintf(stderr, "error: encoding failed (%d)\n", rc);
            rc = -7;
            goto out;
        }
    }
    if (data.len > UINT32_MAX - AIN_HDR_SIZE) {
        fprintf(stderr, "error: encoded archive exceeds the 32-bit format limit\n");
        rc = -5;
        goto out;
    }

    /* garble: encrypt the data region *before* records are built, because
     * in garbled archives the stored member checksums cover the encrypted
     * bytes (verified against the original tool)                         */
    if (pw && *pw) {
        ain_garble_seed(pw, gseed);
        g_on = 1;
        ain_garble_xor(gseed, data.p, data.len);
    }

    for (i = 0; i < n; i++) {
        uint8_t r[AIN_REC_SIZE];
        uint32_t f22, comp = 0, off = 0, osz;
        uint16_t chk = 0;
        char nmz[1024];
        size_t j;
        memset(r, 0, sizeof r);
        osz = (uint32_t)ents[i].n;
        r[0] = ents[i].attr;
        memcpy(r + 1, &ents[i].ts, 4);
        memcpy(r + 5, &osz, 4);
        if (method == AIN_M_STORE) {
            f22 = AIN_F_STANDALONE;
            comp = osz;
            off = AIN_HDR_SIZE + offs[i];
            chk = sum16(data.p + offs[i], ents[i].n);
        } else if (n == 1) {
            f22 = AIN_F_STANDALONE;
            comp = (uint32_t)data.len;
            off = AIN_HDR_SIZE;
            chk = sum16(data.p, data.len);
        } else if (i == 0) {
            f22 = AIN_F_SPAN_START;
            off = AIN_HDR_SIZE;
        } else if (i == n - 1) {
            f22 = AIN_F_SPAN_END;
            comp = (uint32_t)data.len;
            chk = sum16(data.p, data.len);
        } else {
            f22 = 0;
        }
        memcpy(r + 9, &comp, 4);
        memcpy(r + 13, &off, 4);
        memcpy(r + 22, &f22, 4);
        memcpy(r + 27, &chk, 2);
        if (strlen(ents[i].name) >= sizeof nmz ||
            ain_buf_put(&recs, r, sizeof r)) { rc = -5; goto out; }
        snprintf(nmz, sizeof nmz, "%s", ents[i].name);
        for (j = 0; nmz[j]; j++)
            if (nmz[j] == '/')
                nmz[j] = '\\';
        if (ain_buf_put(&recs, nmz, strlen(nmz) + 1) ||
            ain_buf_putc(&recs, 0)) { rc = -5; goto out; }
    }

    rc = ain_encode_stream(recs.p, recs.len, &idx, speed > 2 ? 2 : speed);
    if (rc) {
        fprintf(stderr, "error: index encoding failed (%d)\n", rc);
        rc = -8;
        goto out;
    }
    if (idx.len > UINT32_MAX - AIN_HDR_SIZE - data.len) {
        fprintf(stderr, "error: archive index exceeds the 32-bit format limit\n");
        rc = -5;
        goto out;
    }

    ts_now = ain_fs_now();
    memset(h, 0, sizeof h);
    h[0] = AIN_MAGIC;
    h[1] = (uint8_t)(0x10 | method);
    if (g_on) {
        uint16_t gs = (uint16_t)(gseed[0] + gseed[1] + gseed[2]);
        h[3] = AIN_FLAG_GARBLE;
        h[4] = (uint8_t)(gs & 0xFF);     /* password hash, LE (hdr[4..5]) */
        h[5] = (uint8_t)(gs >> 8);
    }
    h[8] = (uint8_t)(n & 0xFF);
    h[9] = (uint8_t)((n >> 8) & 0xFF);
    memcpy(h + 10, &ts_now, 4);
    {
        uint32_t idxoff = (uint32_t)(AIN_HDR_SIZE + data.len);
        uint16_t ic = sum16(idx.p, idx.len);
        uint16_t hc;
        memcpy(h + 14, &idxoff, 4);
        memcpy(h + 18, &ic, 2);
        hc = (uint16_t)(sum16(h, 22) ^ AIN_HDRXOR);
        memcpy(h + 22, &hc, 2);
    }

    f = fopen(archpath, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot create %s\n", archpath);
        rc = -9;
        goto out;
    }

    /* assemble the complete archive image */
    {
        AinBuf img;
        ain_buf_init(&img);
        if (ain_buf_reserve(&img, AIN_HDR_SIZE + data.len + idx.len) ||
            ain_buf_put(&img, h, sizeof h) ||
            (data.len && ain_buf_put(&img, data.p, data.len)) ||
            (idx.len && ain_buf_put(&img, idx.p, idx.len))) {
            ain_buf_free(&img);
            fclose(f);
            rc = -10;
            goto out;
        }

        {
            if (fwrite(img.p, 1, img.len, f) != img.len) {
                fprintf(stderr, "error: cannot write %s\n", archpath);
                fclose(f);
                ain_buf_free(&img);
                rc = -10;
                goto out;
            }
            if (fclose(f)) {
                fprintf(stderr, "error: cannot close %s\n", archpath);
                ain_buf_free(&img);
                rc = -10;
                goto out;
            }
        }
        ain_buf_free(&img);
    }

out:
    free(offs);
    ain_buf_free(&data);
    ain_buf_free(&idx);
    ain_buf_free(&recs);
    return rc;
}

/* ---------------- create from disk file names (streaming) ----------------
 * Files are consumed one at a time and the archive is written straight to
 * disk, so peak memory is bounded by the encoder block size regardless of
 * the archive size:
 *   m4: raw copy, per-member checksums over the stored bytes
 *   m1..3: fed through the multi-block encoder (tree reload every 64 KiB
 *          at speeds 1 and 2, 2 MiB at speed 3; window history carried across blocks)
 * A placeholder header reserves room and is patched at close; the index is
 * assembled in memory (metadata records only).  Garbled sets XOR through a
 * running keystream, and checksums cover the bytes as written (encrypted),
 * matching the original tool's records.
 * ain_write() above stays for the d/u/y rewrite paths, which already hold
 * decoded members in memory.                                                   */

#define AIN_CHUNK (2u << 20)
#define AIN_CHUNK_ADAPTIVE (64u << 10) /* adapt trees to local symbol frequencies */

typedef struct {
    FILE *file;
} VolumeOutput;

static int volume_output_open(VolumeOutput *out, const char *path)
{
    memset(out, 0, sizeof *out);
    out->file = fopen(path, "wb");
    return out->file ? 0 : -1;
}

static int volume_output_write(VolumeOutput *out, const uint8_t *p, size_t n)
{
    return fwrite(p, 1, n, out->file) == n ? 0 : -1;
}

static int volume_output_close(VolumeOutput *out)
{
    int rc = 0;
    if (out->file) {
        if (fclose(out->file))
            rc = -1;
        out->file = NULL;
    }
    return rc;
}

typedef struct {
    VolumeOutput *output;
    uint32_t len;                 /* data bytes written                    */
    uint16_t sum;                 /* sum16 over all bytes written          */
    int      garble;
    AinGarbleSt gst;              /* running keystream state               */
    uint8_t  xbuf[65536];
} Sink;

/* XOR (if garbled), checksum and append one slice; *acc (optional) gets
 * the slice sums for a per-member checksum */
static int sink_write(Sink *s, const uint8_t *p, size_t n, uint16_t *acc)
{
    size_t off = 0;
    while (off < n) {
        size_t take = n - off;
        uint16_t cs;
        if (take > sizeof s->xbuf)
            take = sizeof s->xbuf;
        memcpy(s->xbuf, p + off, take);
        if (s->garble)
            ain_garble_run(&s->gst, s->xbuf, take);
        cs = sum16(s->xbuf, take);
        s->sum = (uint16_t)(s->sum + cs);
        if (acc)
            *acc = (uint16_t)(*acc + cs);
        if (volume_output_write(s->output, s->xbuf, take))
            return -1;
        off += take;
    }
    s->len += (uint32_t)n;
    return 0;
}

static int put_rec(AinBuf *recs, uint8_t attr, uint32_t ts, uint32_t orig,
                   uint32_t comp, uint32_t off, uint32_t f22, uint16_t chk,
                   const char *name)
{
    uint8_t r[AIN_REC_SIZE];
    char nmz[1024];
    size_t j;
    memset(r, 0, sizeof r);
    r[0] = attr;
    memcpy(r + 1, &ts, 4);
    memcpy(r + 5, &orig, 4);
    memcpy(r + 9, &comp, 4);
    memcpy(r + 13, &off, 4);
    memcpy(r + 22, &f22, 4);
    memcpy(r + 27, &chk, 2);
    if (ain_buf_put(recs, r, sizeof r))
        return -1;
    snprintf(nmz, sizeof nmz, "%s", name);
    for (j = 0; nmz[j]; j++)
        if (nmz[j] == '/')
            nmz[j] = '\\';
    if (ain_buf_put(recs, nmz, strlen(nmz) + 1))
        return -1;
    return ain_buf_putc(recs, 0);
}

static int write_indexed_volume(const char *path, int method, unsigned volume,
                                 int more, unsigned count, const AinBuf *data,
                                 const AinBuf *idx, const char *pw)
{
    uint8_t h[AIN_HDR_SIZE] = { 0 };
    uint16_t checksum;
    FILE *f;
    int failed;
    h[0] = AIN_MAGIC;
    h[1] = (uint8_t)(0x10 | method);
    h[3] = more ? AIN_FLAG_MORE_VOLUMES : 0;
    if (pw && *pw) {
        uint16_t hash = ain_garble_hash(pw);
        h[3] |= AIN_FLAG_GARBLE;
        h[4] = (uint8_t)hash;
        h[5] = (uint8_t)(hash >> 8);
    }
    h[6] = (uint8_t)volume;
    h[7] = (uint8_t)(volume >> 8);
    h[8] = (uint8_t)count;
    h[9] = (uint8_t)(count >> 8);
    put32(h + 10, ain_fs_now());
    put32(h + 14, (uint32_t)(AIN_HDR_SIZE + data->len));
    checksum = sum16(idx->p, idx->len);
    h[18] = (uint8_t)checksum;
    h[19] = (uint8_t)(checksum >> 8);
    checksum = (uint16_t)(sum16(h, 22) ^ AIN_HDRXOR);
    h[22] = (uint8_t)checksum;
    h[23] = (uint8_t)(checksum >> 8);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    failed = fwrite(h, 1, sizeof h, f) != sizeof h ||
        (data->len && fwrite(data->p, 1, data->len, f) != data->len) ||
        fwrite(idx->p, 1, idx->len, f) != idx->len;
    if (fclose(f))
        failed = 1;
    if (failed) {
        fprintf(stderr, "error: cannot write volume %s\n", path);
        return -1;
    }
    ain_status(1, "Writing volume %s\n", path);
    return 0;
}

typedef struct {
    const char *name;
    uint32_t size, ts;
    uint8_t attr;
} VolumeMember;

typedef struct {
    const VolumeMember *members;
    const AinEnt *ents;
    int count, next;
    uint32_t offset;
    FILE *file;
} VolumeInput;

static int read_volume_input(VolumeInput *input, uint8_t *out, size_t bytes)
{
    while (input->next < input->count) {
        const VolumeMember *member = &input->members[input->next];
        size_t take = member->size - input->offset;
        if (!bytes && take)
            break;
        if (take > bytes)
            take = bytes;
        if (input->ents) {
            if (take)
                memcpy(out, input->ents[input->next].data + input->offset, take);
        } else {
            if (!input->file) {
                input->file = fopen(member->name, "rb");
                if (!input->file) {
                    fprintf(stderr, "error: cannot open %s\n", member->name);
                    return -1;
                }
            }
            if (fread(out, 1, take, input->file) != take) {
                fprintf(stderr, "error: cannot read %s\n", member->name);
                return -1;
            }
        }
        out += take;
        bytes -= take;
        input->offset += (uint32_t)take;
        if (input->offset == member->size) {
            if (input->file) {
                int extra = fgetc(input->file);
                int failed = extra != EOF || ferror(input->file);
                if (fclose(input->file))
                    failed = 1;
                input->file = NULL;
                if (failed) {
                    fprintf(stderr, "error: file changed or cannot be closed: %s\n",
                            member->name);
                    return -1;
                }
            }
            input->next++;
            input->offset = 0;
        }
    }
    if (bytes) {
        fprintf(stderr, "error: incomplete volume input\n");
        return -1;
    }
    return 0;
}

static int make_volume_index(const VolumeMember *members, int n,
                              int first, uint32_t offset, size_t bytes,
                              const AinBuf *data, AinBuf *recs, AinBuf *idx,
                              int speed, unsigned limit, unsigned *count,
                              int *next, uint32_t *next_offset)
{
    size_t last = 0;
    unsigned records = 0;
    recs->len = idx->len = 0;
    while (first < n && records < limit) {
        const VolumeMember *member = &members[first];
        size_t take = member->size - offset;
        if (!bytes && take)
            break;
        if (take > bytes)
            take = bytes;
        last = recs->len;
        if (put_rec(recs, member->attr, member->ts, (uint32_t)take, 0,
                    records ? 0 : AIN_HDR_SIZE,
                    (records ? 0 : AIN_F_SPAN_START) |
                    (take < member->size - offset ? AIN_F_MORE_FRAGMENTS : 0),
                    0, member->name)) {
            fprintf(stderr, "error: cannot allocate volume index\n");
            return -1;
        }
        put32(recs->p + last + 17, offset);
        records++;
        offset += (uint32_t)take;
        bytes -= take;
        if (offset == member->size) {
            first++;
            offset = 0;
        }
    }
    if (bytes) {
        fprintf(stderr, "error: invalid volume member range\n");
        return -1;
    }
    if (!records) {
        *count = 0;
        *next = first;
        *next_offset = offset;
        return 0;
    }
    put32(recs->p + last + 9, (uint32_t)data->len);
    put32(recs->p + last + 22, get32(recs->p + last + 22) | AIN_F_SPAN_END);
    {
        uint16_t checksum = sum16(data->p, data->len);
        recs->p[last + 27] = (uint8_t)checksum;
        recs->p[last + 28] = (uint8_t)(checksum >> 8);
    }
    if (ain_encode_stream(recs->p, recs->len, idx, speed > 2 ? 2 : speed)) {
        fprintf(stderr, "error: volume index encoding failed\n");
        return -1;
    }
    *count = records;
    *next = first;
    *next_offset = offset;
    return 0;
}

static int write_indexed_volumes(const char *archpath, int method,
                                 const AinEnt *ents, const char **names,
                                 const uint32_t *sizes, int n,
                                 unsigned long long frag, int speed,
                                 const char *pw, unsigned threads,
                                 AinProgress *progress)
{
    char base[1024], path[1100];
    AinBuf data, recs, idx, encoded, trial_idx, trial_input, cache;
    AinEncS *encoder = NULL;
    VolumeMember *members = NULL;
    VolumeInput input = { 0 };
    unsigned volume = 0, count = 0, written = 0;
    int i, first = 0, next = 0, trial_next = 0, rc = -1;
    uint32_t offset = 0, next_offset = 0, trial_offset = 0;
    size_t capacity, raw_limit, buffered = 0, accepted = 0;
    uint16_t seed[3];
    int garbled = pw && *pw;

    if (frag < 32 || frag > 0xFFFFFFul) {
        fprintf(stderr, "error: volume size is outside the supported range\n");
        return -1;
    }
    if (strlen(archpath) >= sizeof base) {
        fprintf(stderr, "error: archive path is too long: %s\n", archpath);
        return -1;
    }
    capacity = (size_t)frag - AIN_HDR_SIZE;
    raw_limit = capacity * 8;
    if (raw_limit > 64u * 1024u * 1024u)
        raw_limit = 64u * 1024u * 1024u;
    if (method != AIN_M_STORE) {
        size_t chunk = speed <= 2 ? AIN_CHUNK_ADAPTIVE : AIN_CHUNK;
        if (raw_limit >= chunk)
            raw_limit -= raw_limit % chunk;
    }
    strcpy(base, archpath);
    if (strlen(base) >= 4 && !ain_stricmp(base + strlen(base) - 4, ".AIN"))
        base[strlen(base) - 4] = 0;
    snprintf(path, sizeof path, "%s", archpath);
    ain_buf_init(&data);
    ain_buf_init(&recs);
    ain_buf_init(&idx);
    ain_buf_init(&encoded);
    ain_buf_init(&trial_idx);
    ain_buf_init(&trial_input);
    ain_buf_init(&cache);
    members = (VolumeMember *)calloc((size_t)n, sizeof *members);
    if (!members) {
        fprintf(stderr, "error: cannot allocate volume input buffer\n");
        goto done;
    }
    if (ain_buf_reserve(&trial_input, 1))
        goto memory_error;
    if (garbled)
        ain_garble_seed(pw, seed);
    for (i = 0; i < n; i++) {
        VolumeMember *member = &members[i];
        member->name = names ? names[i] : ents[i].name;
        if (names) {
            int isdir;
            member->size = sizes[i];
            member->attr = ain_fs_dosattr(member->name, &isdir);
            member->ts = ain_fs_dostime(member->name);
            if (isdir) {
                fprintf(stderr, "error: cannot open %s\n", member->name);
                goto done;
            }
        } else {
            if (ents[i].n > UINT32_MAX) {
                fprintf(stderr, "error: member exceeds AIN size limit: %s\n",
                        member->name);
                goto done;
            }
            member->size = (uint32_t)ents[i].n;
            member->ts = ents[i].ts;
            member->attr = ents[i].attr;
        }
    }
    input.members = members;
    input.ents = ents;
    input.count = n;
    while (first < n) {
        uint64_t member_limit = 0;
        int end = n - first > 65535 ? first + 65535 : n;
        for (i = first; i < end; i++)
            member_limit += members[i].size;
        member_limit -= offset;
        {
            size_t take;
            size_t available, lower, upper, best, slack;
            unsigned trial_count, record_limit = 65535;
            int final_trial;
            uint64_t rest = member_limit - accepted;
            take = rest > raw_limit ? raw_limit : (size_t)rest;
            available = take;
            lower = best = 0;
            upper = take;
            slack = capacity / 50;
            if (slack < 64)
                slack = 64;
            final_trial = 0;
            if (buffered < available) {
                if (ain_buf_reserve(&trial_input, available - buffered))
                    goto memory_error;
                if (read_volume_input(&input, trial_input.p + trial_input.len,
                                      available - buffered))
                    goto done;
                trial_input.len += available - buffered;
                buffered = available;
            } else if (read_volume_input(&input, trial_input.p + trial_input.len, 0))
                goto done;
            if (method != AIN_M_STORE && !encoder) {
                cache.len = 0;
                encoder = ain_enc_open_mt(&cache, speed,
                    speed <= 2 ? AIN_CHUNK_ADAPTIVE : AIN_CHUNK, threads);
                if (!encoder)
                    goto memory_error;
            }
retry:
            encoded.len = 0;
            if (method == AIN_M_STORE) {
                if (accepted + take &&
                    ain_buf_put(&encoded, trial_input.p, accepted + take))
                    goto memory_error;
            } else {
                size_t block_used = 0;
                size_t index_reserve = trial_idx.len ? trial_idx.len : idx.len;
                size_t budget = capacity > index_reserve + 128 ?
                    capacity - index_reserve - 128 : 0;
                size_t chunk = speed <= 2 ? AIN_CHUNK_ADAPTIVE : AIN_CHUNK;
                size_t candidate = accepted + take;
                if (candidate < accepted + available &&
                    accepted + available - candidate >= chunk - candidate % chunk)
                    candidate += chunk - candidate % chunk;
                if (!final_trial && budget &&
                    ain_enc_prefix_blocks(encoder, trial_input.p,
                        candidate, budget, &block_used, &encoded)) {
                    fprintf(stderr, "error: volume block encoding failed\n");
                    goto done;
                }
                if (!final_trial && block_used > accepted &&
                    block_used - accepted >= lower &&
                    block_used - accepted <= upper &&
                    (take < available || block_used == accepted + available ||
                     (encoded.len <= budget && budget - encoded.len <= slack)) &&
                    block_used <= accepted + available) {
                    take = block_used - accepted;
                } else {
                    if (ain_enc_prefix(encoder, trial_input.p, accepted + take,
                                       &encoded)) {
                        fprintf(stderr, "error: volume data encoding failed\n");
                        goto done;
                    }
                }
            }
            if (garbled)
                ain_garble_xor(seed, encoded.p, encoded.len);
            if (make_volume_index(members, n, first, offset, accepted + take,
                                  &encoded, &recs, &trial_idx, speed, record_limit,
                                  &trial_count, &trial_next, &trial_offset))
                goto done;
            {
                size_t used = encoded.len + trial_idx.len;
                int fits = used <= capacity && trial_count &&
                           (!available || take != 0 || trial_next > first);
                if (!fits && !take && !accepted && trial_count > 1) {
                    record_limit = trial_count / 2;
                    goto retry;
                }
                if (fits && !take)
                    goto accept_fragment;
                if (!final_trial && available &&
                    (!fits || (take < available && capacity - used > slack))) {
                    size_t next;
                    if (fits) {
                        best = take;
                        lower = take + 1;
                    } else {
                        upper = take ? take - 1 : 0;
                    }
                    if (lower <= upper && take) {
                        size_t baseline = data.len + idx.len;
                        next = lower + (upper - lower) / 2;
                        if (baseline < capacity && used > baseline) {
                            uint64_t estimate = (uint64_t)take *
                                (capacity - baseline) / (used - baseline);
                            if (estimate >= lower && estimate <= upper)
                                next = (size_t)estimate;
                            else if (fits && estimate > upper)
                                next = upper;
                            else if (!fits && estimate < lower)
                                next = lower;
                        }
                        take = next;
                        goto retry;
                    }
                    if (best) {
                        take = best;
                        final_trial = 1;
                        goto retry;
                    }
                } else if (fits) {
                    goto accept_fragment;
                }
            }
            if (count) {
                goto close_volume;
            } else {
                fprintf(stderr, "error: member and index do not fit in a volume: %s\n",
                        members[first].name);
                goto done;
            }
accept_fragment:
            {
                AinBuf swap = data;
                data = encoded;
                encoded = swap;
                accepted += take;
            }
            {
                AinBuf swap = idx;
                idx = trial_idx;
                trial_idx = swap;
            }
            count = trial_count;
            if (progress)
                ain_progress_advance(progress, take);
            for (i = next; progress && i < trial_next; i++)
                ain_status(1, "Adding %s (%u%%)\n", members[i].name, progress->percent);
            next = trial_next;
            next_offset = trial_offset;
            buffered -= take;
            if (capacity - data.len - idx.len > slack && next < n &&
                count < 65535 && take)
                continue;
close_volume:
            if (volume == 254 && next < n) {
                fprintf(stderr, "error: volume set exceeds 255 volumes\n");
                goto done;
            }
            written = volume + 1;
            if (write_indexed_volume(path, method, volume, next < n, count,
                                     &data, &idx, pw))
                goto done;
            if (next == n)
                break;
            volume++;
            snprintf(path, sizeof path, "%s.%03u", base, volume);
            first = next;
            offset = next_offset;
            if (encoder && (accepted ?
                ain_enc_rebase_prefix(encoder, trial_input.p, accepted) :
                ain_enc_reset_prefix(encoder))) {
                fprintf(stderr, "error: cannot advance volume encoder\n");
                goto done;
            }
            memmove(trial_input.p, trial_input.p + accepted, buffered);
            trial_input.len = buffered;
            data.len = recs.len = idx.len = accepted = 0;
            count = 0;
        }
    }
    rc = 0;
    if (!rc && volume &&
        (strlen(archpath) < 4 ||
         ain_stricmp(archpath + strlen(archpath) - 4, ".AIN"))) {
        snprintf(path, sizeof path, "%s.AIN", base);
        if ((remove(path) && errno != ENOENT) || rename(archpath, path)) {
            fprintf(stderr, "error: cannot name first volume %s: %s\n",
                    path, strerror(errno));
            rc = -1;
        }
    }
    goto done;
memory_error:
    fprintf(stderr, "error: cannot allocate volume encoding buffers\n");
done:
    ain_enc_free(encoder);
    if (input.file)
        fclose(input.file);
    if (rc) {
        unsigned k;
        for (k = 0; k < written; k++) {
            if (!k)
                snprintf(path, sizeof path, "%s", archpath);
            else
                snprintf(path, sizeof path, "%s.%03u", base, k);
            if (remove(path) && errno != ENOENT)
                fprintf(stderr, "error: cannot remove incomplete volume %s: %s\n",
                        path, strerror(errno));
        }
    }
    free(members);
    ain_buf_free(&data);
    ain_buf_free(&recs);
    ain_buf_free(&idx);
    ain_buf_free(&encoded);
    ain_buf_free(&trial_idx);
    ain_buf_free(&trial_input);
    ain_buf_free(&cache);
    return rc;
}

int ain_create(const char *archpath, int method, const char **names, int nfiles,
               unsigned long long frag, int speed, const char *pw)
{
    return ain_create_mt(archpath, method, names, nfiles, frag, speed, pw, 1);
}

int ain_create_mt(const char *archpath, int method, const char **names, int nfiles,
                  unsigned long long frag, int speed, const char *pw,
                  unsigned threads)
{
    VolumeOutput output;
    Sink sink;
    AinBuf recs, idx, eout;
    AinEncS *enc = NULL;
    uint8_t *attr;
    uint32_t *ts, *osz;
    int i, rc = 0, output_open = 0;
    int solid = method != AIN_M_STORE;
    int have_g = pw && *pw;
    uint32_t local_idxoff;
    uint16_t index_sum;
    uint8_t rb[65536];
    uint8_t h[AIN_HDR_SIZE];
    uint64_t total = 0;
    AinProgress progress;
    double started = ain_fs_time();

    if (started < 0 || nfiles <= 0 || method < 1 || method > 4)
        return -1;
    attr = (uint8_t *)calloc((size_t)nfiles, sizeof *attr);
    ts   = (uint32_t *)calloc((size_t)nfiles, sizeof *ts);
    osz  = (uint32_t *)calloc((size_t)nfiles, sizeof *osz);
    if (!attr || !ts || !osz) {
        free(attr); free(ts); free(osz);
        return -2;
    }
    for (i = 0; i < nfiles; ++i) {
        if (ain_fs_file_size(names[i], &osz[i])) {
            free(attr); free(ts); free(osz);
            return -3;
        }
        total += osz[i];
    }
    if (frag) {
        ain_status(2, "Creating archive %s\n", archpath);
        ain_progress_begin(&progress, "Adding", total);
        rc = write_indexed_volumes(archpath, method, NULL, names, osz, nfiles,
                                   frag, speed, pw, threads, &progress);
        if (!rc)
            rc = ain_size_summary(archpath, total, started, &progress);
        free(attr); free(ts); free(osz);
        return rc;
    }
    ain_status(2, "Creating archive %s\n", archpath);

    ain_buf_init(&recs);
    ain_buf_init(&idx);
    ain_buf_init(&eout);
    memset(&sink, 0, sizeof sink);
    memset(h, 0, sizeof h);

    if (volume_output_open(&output, archpath)) {
        fprintf(stderr, "error: cannot create %s\n", archpath);
        free(attr); free(ts); free(osz);
        return -9;
    }
    output_open = 1;
    if (volume_output_write(&output, h, AIN_HDR_SIZE)) {
        rc = -10;
        goto done;
    }
    sink.output = &output;
    if (have_g) {
        uint16_t seed[3];
        sink.garble = 1;
        ain_garble_seed(pw, seed);
        ain_garble_init(&sink.gst, seed);
    }
    if (solid) {
        enc = ain_enc_open_mt(&eout, speed,
                             speed == 1 || speed == 2 ? AIN_CHUNK_ADAPTIVE : AIN_CHUNK,
                             threads);
        if (!enc) {
            fprintf(stderr, "error: cannot initialize encoder\n");
            rc = -2;
            goto done;
        }
    }
    ain_progress_begin(&progress, "Adding", total);

    for (i = 0; i < nfiles; i++) {
        FILE *g = fopen(names[i], "rb");
        int isdir;
        uint32_t off_here = AIN_HDR_SIZE + sink.len;
        size_t got;
        uint64_t read_bytes = 0;
        if (!g) {
            fprintf(stderr, "error: cannot open %s\n", names[i]);
            rc = -3;
            goto done;
        }
        attr[i] = ain_fs_dosattr(names[i], &isdir);
        if (isdir) {
            fprintf(stderr, "error: %s is a directory\n", names[i]);
            fclose(g);
            rc = -4;
            goto done;
        }
        ts[i] = ain_fs_dostime(names[i]);
        if (solid) {
            while ((got = fread(rb, 1, sizeof rb, g)) > 0) {
                read_bytes += got;
                if (read_bytes > osz[i]) {
                    fclose(g);
                    fprintf(stderr, "error: file size changed while reading %s\n", names[i]);
                    rc = -5;
                    goto done;
                }
                if (ain_enc_feed(enc, rb, got)) {
                    fclose(g);
                    fprintf(stderr, "error: encoding failed\n");
                    rc = -7;
                    goto done;
                }
                if (eout.len && sink_write(&sink, eout.p, eout.len, NULL)) {
                    fclose(g);
                    rc = -10;
                    goto done;
                }
                eout.len = 0;
                ain_progress_advance(&progress, got);
            }
        } else {
            uint16_t mchk = 0;
            while ((got = fread(rb, 1, sizeof rb, g)) > 0) {
                read_bytes += got;
                if (read_bytes > osz[i]) {
                    fclose(g);
                    fprintf(stderr, "error: file size changed while reading %s\n", names[i]);
                    rc = -5;
                    goto done;
                }
                if (sink_write(&sink, rb, got, &mchk)) {
                    fclose(g);
                    rc = -10;
                    goto done;
                }
                ain_progress_advance(&progress, got);
            }
            if (put_rec(&recs, attr[i], ts[i], osz[i], osz[i], off_here,
                        AIN_F_STANDALONE, mchk, names[i])) {
                fclose(g);
                rc = -10;
                goto done;
            }
        }
        if (ferror(g) || read_bytes != osz[i]) {
            fclose(g);
            fprintf(stderr, "error: cannot read %s or file size changed\n", names[i]);
            rc = -5;
            goto done;
        }
        fclose(g);
        ain_status(1, "Adding %s (%u%%)\n", names[i], progress.percent);
    }

    if (solid) {
        if (ain_enc_finish(enc) ||
            (eout.len && sink_write(&sink, eout.p, eout.len, NULL))) {
            fprintf(stderr, "error: encoding failed\n");
            rc = -7;
            goto done;
        }
        for (i = 0; i < nfiles; i++) {
            uint32_t f22, comp = 0, off = 0;
            uint16_t chk = 0;
            if (nfiles == 1) {
                f22 = AIN_F_STANDALONE;
                comp = sink.len;
                off = AIN_HDR_SIZE;
                chk = sink.sum;
            } else if (i == 0) {
                f22 = AIN_F_SPAN_START;
                off = AIN_HDR_SIZE;
            } else if (i == nfiles - 1) {
                f22 = AIN_F_SPAN_END;
                comp = sink.len;
                chk = sink.sum;
            } else {
                f22 = 0;
            }
            if (put_rec(&recs, attr[i], ts[i], osz[i], comp, off, f22, chk,
                        names[i])) {
                rc = -10;
                goto done;
            }
        }
    }

    rc = ain_encode_stream(recs.p, recs.len, &idx, speed > 2 ? 2 : speed);
    if (rc) {
        fprintf(stderr, "error: index encoding failed (%d)\n", rc);
        rc = -8;
        goto done;
    }
    local_idxoff = AIN_HDR_SIZE + sink.len;
    if (idx.len && volume_output_write(&output, idx.p, idx.len)) {
        rc = -10;
        goto done;
    }
    index_sum = sum16(idx.p, idx.len);
    if (!output.file || fflush(output.file)) {
        rc = -10;
        goto done;
    }

    memset(h, 0, sizeof h);
    h[0] = AIN_MAGIC;
    h[1] = (uint8_t)(0x10 | method);
    if (have_g) {
        uint16_t gs = ain_garble_hash(pw);
        h[3] = AIN_FLAG_GARBLE;
        h[4] = (uint8_t)(gs & 0xFF);     /* password hash, LE (hdr[4..5]) */
        h[5] = (uint8_t)(gs >> 8);
    }
    h[8] = (uint8_t)(nfiles & 0xFF);
    h[9] = (uint8_t)((nfiles >> 8) & 0xFF);
    {
        uint32_t ts_now = ain_fs_now();
        memcpy(h + 10, &ts_now, 4);
        memcpy(h + 14, &local_idxoff, 4);
        memcpy(h + 18, &index_sum, 2);
    }
    {
        uint16_t hc = (uint16_t)(sum16(h, 22) ^ AIN_HDRXOR);
        memcpy(h + 22, &hc, 2);
    }
    {
        FILE *header_file;
        if (volume_output_close(&output)) {
            rc = -10;
            goto done;
        }
        header_file = fopen(archpath, "r+b");
        if (!header_file) {
            rc = -10;
            goto done;
        }
        if (fwrite(h, 1, sizeof h, header_file) != sizeof h) {
            fclose(header_file);
            rc = -10;
            goto done;
        }
        if (fclose(header_file)) {
            rc = -10;
            goto done;
        }
    }
    if (!rc && ain_size_summary(archpath, total, started, &progress))
        rc = -1;

done:
    if (enc)
        ain_enc_free(enc);
    ain_buf_free(&eout);
    ain_buf_free(&recs);
    ain_buf_free(&idx);
    free(attr); free(ts); free(osz);
    if (output_open) {
        if (volume_output_close(&output))
            rc = -10;
    }
    if (rc && output_open) {
        remove(archpath);
    }
    return rc;
}

/* ---------------- load all members into memory (for d/u rewrite) -------- */

typedef struct { AinEnt *arr; int n, cap; } LoadCtx;

static int load_cb(AinArchive *a, AinRec *r, const uint8_t *data, size_t n, void *vctx)
{
    LoadCtx *lc = (LoadCtx *)vctx;
    AinEnt *e;
    (void)a;
    if (n == SIZE_MAX)
        return -1;
    if (lc->n == lc->cap) {
        int nc = lc->cap ? lc->cap * 2 : 16;
        AinEnt *na = (AinEnt *)realloc(lc->arr, (size_t)nc * sizeof *na);
        if (!na)
            return -1;
        lc->arr = na;
        lc->cap = nc;
    }
    e = &lc->arr[lc->n];
    e->name = (char *)malloc(strlen(r->name) + 1);
    e->data = (uint8_t *)malloc(n + 1);
    if (!e->name || !e->data) {
        free(e->name);
        free(e->data);
        return -1;
    }
    strcpy(e->name, r->name);
    memcpy(e->data, data, n);
    e->n = n;
    e->ts = r->ts;
    e->attr = r->attr;
    lc->n++;
    return 0;
}

int ain_load_entries(AinArchive *a, AinEnt **out, int *nout)
{
    LoadCtx lc;
    int rc;
    if (a->garbled && !a->gseed_ok)
        return -20;
    memset(&lc, 0, sizeof lc);
    rc = visit_files(a, load_cb, &lc, 1);
    if (rc) {
        ain_free_entries(lc.arr, lc.n);
        return rc;
    }
    *out = lc.arr;
    *nout = lc.n;
    return 0;
}

void ain_free_entries(AinEnt *e, int n)
{
    int i;
    if (!e)
        return;
    for (i = 0; i < n; i++) {
        free(e[i].name);
        free(e[i].data);
    }
    free(e);
}

/* ---------------- misc public helpers ---------------- */

int ain_set_dostime(const char *path, uint32_t ts)
{
    return ain_fs_set_dostime(path, ts);
}

uint32_t ain_file_dostime(const char *path)
{
    return ain_fs_dostime(path);
}

/* read one disk file into a freshly allocated entry (for update) */
int ain_read_file(const char *path, AinEnt *out)
{
    uint32_t size;
    if (ain_fs_file_size(path, &size))
        return -1;
    return ain_read_file_progress(path, out, size, NULL);
}

int ain_read_file_progress(const char *path, AinEnt *out, uint32_t expected_size,
                           AinProgress *progress)
{
    FILE *g;
    int isdir;
    size_t pos = 0;
    if ((uint64_t)expected_size + 1 > SIZE_MAX) {
        fprintf(stderr, "error: file exceeds addressable memory: %s\n", path);
        return -3;
    }
    g = fopen(path, "rb");
    if (!g)
        return -1;
    out->attr = ain_fs_dosattr(path, &isdir);
    if (isdir) {
        fclose(g);
        return -2;
    }
    out->ts = ain_fs_dostime(path);
    out->name = (char *)malloc(strlen(path) + 1);
    out->data = (uint8_t *)malloc((size_t)expected_size + 1);
    if (!out->name || !out->data) {
        free(out->name);
        free(out->data);
        fclose(g);
        return -3;
    }
    strcpy(out->name, path);
    while (pos < expected_size) {
        size_t take = expected_size - pos;
        size_t got;
        if (take > 65536)
            take = 65536;
        got = fread(out->data + pos, 1, take, g);
        pos += got;
        if (progress && got)
            ain_progress_advance(progress, got);
        if (got != take)
            break;
    }
    if (pos != expected_size || fgetc(g) != EOF || ferror(g)) {
        free(out->name);
        free(out->data);
        fclose(g);
        return -4;
    }
    out->n = expected_size;
    if (fclose(g)) {
        free(out->name);
        free(out->data);
        return -4;
    }
    return 0;
}
