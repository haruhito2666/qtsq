/*
 * tests/test_int8.c — W3.6: INT8 scalar quantisation with per-segment scales
 *
 * What this file proves, in order:
 *   1. the int8 store is genuinely smaller: whole-store and payload-only
 *      ratios for one corpus written twice (cfg.quantize_int8 0 vs 1);
 *   2. the int8 payload has the documented byte layout (count, per-record f32
 *      scale table, then f32-style record frames with `dim` code bytes), the
 *      scale/codes round trip is exact and deterministic, and every per-record
 *      CRC still matches;
 *   3. RAM: the int8 vector store holds dim+4 bytes/row and keeps NO f32 copy,
 *      the f32 store holds 4*dim — measured from the live sfb_brain_t;
 *   4. an int8 store reopens with the same record count and self-recall is
 *      still ~perfect, and sfb_verify() passes;
 *   5. a float32 store (quantize_int8 = 0) still opens and recalls;
 *   6. a flipped payload byte is caught by sfb_verify(), and repair drops
 *      exactly the damaged segment;
 *   7. a segment carrying a flag bit this build does not know is rejected with
 *      SFB_E_CORRUPT instead of being misparsed — the same check an older build
 *      applies to bit 1, so an int8 segment fails loudly there, and a brain
 *      open on such a store quarantines the segment instead of handing back
 *      rows decoded out of a payload it cannot read;
 *   8. sealed int8 and sealed f32 stores both round-trip (encryption applies to
 *      the payload as a whole, scale table included);
 *   9. sfb_retain() compaction in int8 mode moves codes+scales, never builds an
 *      f32 array, and the tombstones it flushes survive a reopen.
 *
 * Build (the Makefile pattern rule does this for `make tests/test_int8`):
 *   cc -DSFB_TEST_HAVE_CRYPTO -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE \
 *      -I. -I../../include -I../../crypto/common -I../../crypto/argon2 \
 *      tests/test_int8.c -L. -lsfb -L../.. -lqtsq -lz -lpthread -lm \
 *      [-lc++ -framework CoreFoundation -framework Security on Darwin] \
 *      -o /tmp/t
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <dirent.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>

static int checks, fails;
#define CK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CK_MSG(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ── corpus ─────────────────────────────────────────────────────────────── */
#define T_SEG_HDR 48u            /* documented segment header length        */
#define T_SEG_HDR 48u            /* documented segment header length        */
#define T_SEG_HDR 48u            /* documented segment header length        */
#define T_N     4000u            /* >= 3000 records                          */
#define T_DIM   256u             /* >= 128                                   */
#define T_SEALN 200u
#define T_SEALDIM 128u

static uint64_t rs = 20261029u;
static double rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0;
}

static float *corpus;                    /* T_N * T_DIM, generated once        */
static char (*texts)[32];
static float *sealed_corpus;              /* T_SEALN * T_SEALDIM               */

static void rmrf(const char *p)
{
    char cmd[512];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd) != 0) { /* best effort */ }
}

static void make_corpus(void)
{
    corpus = (float *)malloc((size_t)T_N * T_DIM * sizeof(float));
    texts = (char (*)[32])malloc((size_t)T_N * 32u);
    sealed_corpus = (float *)malloc((size_t)T_SEALN * T_SEALDIM * sizeof(float));
    if (corpus == NULL || texts == NULL || sealed_corpus == NULL) {
        fprintf(stderr, "oom: corpus\n");
        exit(2);
    }
    for (uint32_t i = 0; i < T_N; i++) {
        for (uint32_t d = 0; d < T_DIM; d++)
            corpus[(size_t)i * T_DIM + d] = (float)(rnd() * 2.0 - 1.0);
        snprintf(texts[i], sizeof texts[i], "int8-corpus-%u", i);
    }
    for (uint32_t i = 0; i < T_SEALN; i++)
        for (uint32_t d = 0; d < T_SEALDIM; d++)
            sealed_corpus[(size_t)i * T_SEALDIM + d] =
                (float)(rnd() * 2.0 - 1.0);
}

/* ── build one store ────────────────────────────────────────────────────── */
static int build_store(const char *dir, uint32_t dim, uint32_t n,
                       const float *vecs, const char *key, int int8)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    uint64_t live = 0;
    int rc;

    rmrf(dir);
    sfb_config_default(&cfg, dir, dim);
    cfg.quantize_int8 = int8;
    cfg.key = key;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) {
        fprintf(stderr, "  build: open %s rc=%d (%s)\n", dir, rc, sfb_strerror(rc));
        return rc;
    }
    for (uint32_t i = 0; i < n; i++) {
        sfb_remember_result_t r;
        char text[64];

        if (dim == T_DIM)
            snprintf(text, sizeof text, "%s", texts[i]);
        else
            snprintf(text, sizeof text, "sealed-%u", i);
        rc = sfb_remember_ex(b, vecs + (size_t)i * dim, dim, text, "w36", &r);
        if (rc != SFB_OK) {
            fprintf(stderr, "  build: remember %u rc=%d (%s)\n", i, rc,
                    sfb_strerror(rc));
            (void)sfb_close(b);
            return rc;
        }
        if (r.merged) {
            fprintf(stderr, "  build: record %u was merged into %u\n", i, r.into);
            (void)sfb_close(b);
            return SFB_E_STATE;
        }
    }
    {
        sfb_flush_result_t fr;

        rc = sfb_flush(b, &fr);
        if (rc != SFB_OK)
            fprintf(stderr, "  build: flush rc=%d (%s)\n", rc, sfb_strerror(rc));
    }
    if (rc == SFB_OK && sfb_count(b, &live) == SFB_OK && live != n) {
        fprintf(stderr, "  build: live=%llu expected %u\n",
                (unsigned long long)live, n);
        rc = SFB_E_STATE;
    }
    (void)sfb_close(b);
    return rc;
}

/* ── raw segment inspection (independent of the library's own reader) ───── */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t lo = rd32(p), hi = rd32(p + 4);

    return lo | (hi << 32);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Every segment file in <dir>/brain.qtsq.d, sorted by name for determinism. */
typedef struct {
    char    name[256];
    uint8_t *bytes;
    size_t   len;
} segfile_t;

static int cmp_seg(const void *a, const void *b)
{
    return strcmp(((const segfile_t *)a)->name, ((const segfile_t *)b)->name);
}

static segfile_t *load_segments(const char *dir, size_t *n_out)
{
    char segdir[1024];
    DIR *d;
    struct dirent *de;
    segfile_t *v = NULL;
    size_t n = 0;

    snprintf(segdir, sizeof segdir, "%s/%s", dir, SFB_SEG_DIR);
    d = opendir(segdir);
    if (d == NULL)
        return NULL;
    while ((de = readdir(d)) != NULL) {
        char path[2048];
        struct stat st;
        FILE *f;
        segfile_t *nv;

        if (strncmp(de->d_name, "seg-", 4) != 0)
            continue;
        snprintf(path, sizeof path, "%s/%s", segdir, de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 48)
            continue;
        nv = (segfile_t *)realloc(v, (n + 1u) * sizeof *nv);
        if (nv == NULL) { closedir(d); free(v); return NULL; }
        v = nv;
        memset(&v[n], 0, sizeof v[n]);
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name);
        v[n].len = (size_t)st.st_size;
        v[n].bytes = (uint8_t *)malloc(v[n].len);
        if (v[n].bytes == NULL) { closedir(d); free(v); return NULL; }
        f = fopen(path, "rb");
        if (f == NULL || fread(v[n].bytes, 1, v[n].len, f) != v[n].len) {
            if (f != NULL) fclose(f);
            closedir(d);
            return NULL;
        }
        fclose(f);
        n++;
    }
    closedir(d);
    if (n > 1)
        qsort(v, n, sizeof *v, cmp_seg);
    *n_out = n;
    return v;
}

static void free_segments(segfile_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i].bytes);
    free(v);
}

typedef struct {
    uint64_t payload;        /* sum of payload_len over segments             */
    uint64_t records;
    uint64_t segs;
    int      all_int8;       /* every segment carries SFB_SEG_FLAG_INT8      */
    int      any_int8;
    int      all_sealed;
    int      any_sealed;
} segsum_t;

static int scan_segments(const char *dir, segsum_t *out)
{
    segfile_t *v;
    size_t n = 0;

    memset(out, 0, sizeof *out);
    out->all_int8 = 1;
    out->all_sealed = 1;
    v = load_segments(dir, &n);
    if (v == NULL)
        return 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *b = v[i].bytes;
        uint16_t flags;

        if (v[i].len < 48 || rd32(b) != SFB_MAGIC_SEGMENT ||
            rd16(b + 4) != (uint16_t)SFB_FORMAT_VERSION) {
            free_segments(v, n);
            return 0;
        }
        flags = rd16(b + 6);
        out->payload += rd64(b + 24);
        out->records += rd32(b + 12);
        out->segs++;
        if (flags & SFB_SEG_FLAG_INT8)
            out->any_int8 = 1;
        else
            out->all_int8 = 0;
        if (flags & SFB_SEG_FLAG_SEALED)
            out->any_sealed = 1;
        else
            out->all_sealed = 0;
    }
    free_segments(v, n);
    return n > 0;
}

/* ── parse the int8 payload exactly as documented ───────────────────────── */
typedef struct {
    uint32_t  count;
    float    *scales;
    int8_t   *codes;         /* count * dim                                */
    uint32_t *ids;
    uint64_t  bytes;         /* payload bytes consumed                     */
    int       crc_bad;
} iparse_t;

static void iparse_free(iparse_t *p)
{
    free(p->scales);
    free(p->codes);
    free(p->ids);
    memset(p, 0, sizeof *p);
}

/* Returns 1 when the whole payload framed cleanly in the documented layout. */
static int parse_int8_payload(const segfile_t *sf, uint32_t dim, iparse_t *out)
{
    const uint8_t *h = sf->bytes;
    const uint8_t *pay = sf->bytes + T_SEG_HDR;
    uint64_t plen = rd64(h + 24);
    uint32_t want = rd32(h + 12);
    size_t off;
    uint32_t count;

    memset(out, 0, sizeof *out);
    if (sf->len != (size_t)T_SEG_HDR + (size_t)plen)
        return 0;
    if (rd32(h + 8) != dim || (rd16(h + 6) & SFB_SEG_FLAG_INT8) == 0)
        return 0;
    if (plen < 4u)
        return 0;
    count = rd32(pay);
    if (count != want)
        return 0;
    if (4u + (uint64_t)count * sizeof(float) > plen)
        return 0;

    out->count = count;
    out->scales = (float *)calloc(count ? count : 1u, sizeof(float));
    out->codes = (int8_t *)calloc((size_t)(count ? count : 1u) * dim, 1u);
    out->ids = (uint32_t *)calloc(count ? count : 1u, sizeof(uint32_t));
    if (out->scales == NULL || out->codes == NULL || out->ids == NULL) {
        iparse_free(out);
        return 0;
    }
    memcpy(out->scales, pay + 4, (size_t)count * sizeof(float));
    off = 4u + (size_t)count * sizeof(float);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id, flags, tier, tl, rl, crc_stored, crc;
        size_t need;

        if (off + 44u > plen)
            goto bad;
        id = rd32(pay + off + 0);
        flags = rd32(pay + off + 4);
        tier = rd32(pay + off + 8);
        tl = rd32(pay + off + 12);
        rl = rd32(pay + off + 16);
        crc_stored = rd32(pay + off + 40);
        need = 44u + (size_t)tl + (size_t)rl + dim;
        if (tl > (1u << 20) || rl > (1u << 20) || off + need > plen)
            goto bad;
        (void)flags; (void)tier;

        crc = sfb_crc32c(pay + off, 24u);
        crc = sfb_crc32c_update(crc, pay + off + 44u, tl);
        crc = sfb_crc32c_update(crc, pay + off + 44u + tl, rl);
        if (crc != crc_stored)
            out->crc_bad++;

        out->ids[i] = id;
        memcpy(out->codes + (size_t)i * dim, pay + off + 44u + tl + rl, dim);
        off += need;
    }
    out->bytes = plen;
    return off == (size_t)plen;

bad:
    iparse_free(out);
    return 0;
}

/* ── self recall ────────────────────────────────────────────────────────── */
/* Returns the number of the `q` sampled rows whose own vector comes back at
 * rank 1, and fills *top10 with the number found in the top 10. */
static int self_recall(sfb_brain_t *b, uint32_t dim, uint32_t n, uint32_t q,
                       uint32_t *top10)
{
    sfb_hit_t hits[16];
    uint32_t hits_n = 0, ok1 = 0, ok10 = 0;

    *top10 = 0;
    for (uint32_t t = 0; t < q; t++) {
        uint32_t row = (uint32_t)(rnd() * (double)n) % n;
        const float *v = corpus + (size_t)row * dim;

        if (sfb_recall_ex(b, v, dim, 10, hits, &hits_n) != SFB_OK)
            continue;
        if (hits_n > 0 && hits[0].id == row)
            ok1++;
        for (uint32_t i = 0; i < hits_n; i++) {
            if (hits[i].id == row) {
                ok10++;
                break;
            }
        }
    }
    *top10 = ok10;
    return (int)ok1;
}

/* ── 1+2+3: footprint, layout, RAM ──────────────────────────────────────── */
static void test_footprint_and_layout(const char *df, const char *dq,
                                      uint64_t *bytes_f32, uint64_t *bytes_int8)
{
    segsum_t sf, sq;
    iparse_t ip;
    segfile_t *v;
    size_t nv = 0;
    uint64_t f32_payload, int8_payload;
    double store_ratio, payload_ratio;
    sfb_brain_t *b = NULL;
    uint64_t vec_f32 = 0, vec_int8 = 0;

    printf("── footprint: %u records x dim %u, written twice\n", T_N, T_DIM);
    CK_MSG(build_store(df, T_DIM, T_N, corpus, NULL, 0) == SFB_OK,
           "float32 corpus build failed");
    CK_MSG(build_store(dq, T_DIM, T_N, corpus, NULL, 1) == SFB_OK,
           "int8 corpus build failed");

    {
        sfb_config_t c;
        sfb_brain_t *f = NULL;
        sfb_stats_t st;

        sfb_config_default(&c, df, T_DIM);
        CK(sfb_open(&c, &f) == SFB_OK);
        if (f != NULL) {
            CK(sfb_stats_get(f, &st) == SFB_OK);
            *bytes_f32 = st.bytes_on_disk;
            sfb_close(f);
        }
        sfb_config_default(&c, dq, T_DIM);
        c.quantize_int8 = 1;
        f = NULL;
        CK(sfb_open(&c, &f) == SFB_OK);
        if (f != NULL) {
            CK(sfb_stats_get(f, &st) == SFB_OK);
            *bytes_int8 = st.bytes_on_disk;
            sfb_close(f);
        }
    }
    CK_MSG(*bytes_f32 > 0 && *bytes_int8 > 0, "missing sizes (f32=%llu int8=%llu)",
           (unsigned long long)*bytes_f32, (unsigned long long)*bytes_int8);

    CK(scan_segments(df, &sf));
    CK(scan_segments(dq, &sq));
    CK_MSG(sf.records == T_N && sq.records == T_N,
           "segments hold f32=%llu int8=%llu records", (unsigned long long)sf.records,
           (unsigned long long)sq.records);
    CK_MSG(!sf.any_int8, "the float32 store wrote an int8 segment");
    CK_MSG(sq.all_int8, "the int8 store has segments without SFB_SEG_FLAG_INT8");
    CK_MSG(!sf.any_sealed && !sq.any_sealed, "unkeyed stores produced sealed segments");

    f32_payload = sf.payload;
    int8_payload = sq.payload;
    payload_ratio = (double)f32_payload / (double)int8_payload;
    store_ratio = (double)*bytes_f32 / (double)*bytes_int8;
    printf("   segments: f32=%llu int8=%llu\n",
           (unsigned long long)sf.segs, (unsigned long long)sq.segs);
    printf("   payload:  f32=%llu B (%.1f B/rec)  int8=%llu B (%.1f B/rec)\n",
           (unsigned long long)f32_payload, (double)f32_payload / T_N,
           (unsigned long long)int8_payload, (double)int8_payload / T_N);
    printf("   payload ratio  = %.2fx  (plan: 4x on the vector payload)\n", payload_ratio);
    printf("   whole store    = f32 %llu B, int8 %llu B, ratio %.2fx\n",
           (unsigned long long)*bytes_f32, (unsigned long long)*bytes_int8,
           store_ratio);
    CK_MSG(payload_ratio >= 3.0, "payload only shrank %.2fx (expected >= 3.0x)",
           payload_ratio);
    CK_MSG(store_ratio >= 2.5, "whole store only shrank %.2fx (expected >= 2.5x)",
           store_ratio);

    /* vector bytes per record, straight from the payload */
    {
        double f32_vec = (double)f32_payload / T_N - 44.0 - 11.0 - 4.0; /* text+role */
        double int8_vec = (double)int8_payload / T_N - 44.0 - 11.0 - 4.0;

        printf("   vector payload ~= f32 %.1f B/rec, int8 %.1f B/rec "
               "(4*dim vs dim+4)\n", f32_vec, int8_vec);
        CK_MSG(f32_vec / int8_vec >= 3.0,
               "vector-only ratio %.2f < 3.0", f32_vec / int8_vec);
    }

    /* ── layout of the first int8 segment ───────────────────────────────── */
    v = load_segments(dq, &nv);
    CK_MSG(v != NULL && nv > 0, "no int8 segment files");
    memset(&ip, 0, sizeof ip);
    if (v != NULL && nv > 0) {
        CK_MSG(parse_int8_payload(&v[0], T_DIM, &ip),
               "int8 payload does not frame as documented");
        if (ip.count > 0) {
            uint32_t bad_scale = 0, bad_round = 0, bad_id = 0;
            float *dec = (float *)malloc((size_t)T_DIM * sizeof(float));
            int8_t *rec = (int8_t *)malloc(T_DIM);

            CK(dec != NULL && rec != NULL);
            for (uint32_t i = 0; dec != NULL && rec != NULL && i < ip.count; i++) {
                float scale2 = 0.0f;

                if (!(ip.scales[i] > 0.0f) || !isfinite(ip.scales[i]))
                    bad_scale++;
                /* the decoded row must re-encode to the identical codes+scale:
                 * that is the determinism the payload promises */
                if (sfb_quant_decode_vec(ip.codes + (size_t)i * T_DIM, T_DIM,
                                         ip.scales[i], dec) == SFB_OK &&
                    sfb_quant_encode_vec(dec, T_DIM, rec, &scale2) == SFB_OK) {
                    if (scale2 != ip.scales[i] ||
                        memcmp(rec, ip.codes + (size_t)i * T_DIM, T_DIM) != 0)
                        bad_round++;
                } else {
                    bad_round++;
                }
                if (ip.ids[i] != i)
                    bad_id++;
            }
            CK_MSG(bad_scale == 0, "%u records have an unusable scale", bad_scale);
            CK_MSG(bad_round == 0, "%u records fail encode(decode()) == identity",
                   bad_round);
            CK_MSG(bad_id == 0, "%u records are out of id order", bad_id);
            CK_MSG(ip.crc_bad == 0, "%d records fail their per-record CRC", ip.crc_bad);
            printf("   layout ok: count=%u, %u f32 scales, %u record frames of "
                   "44+tl+rl+%u bytes, %llu payload bytes\n",
                   ip.count, ip.count, ip.count, T_DIM,
                   (unsigned long long)ip.bytes);
            free(dec);
            free(rec);
        }
        iparse_free(&ip);
        free_segments(v, nv);
    }

    /* ── RAM: what the live vector store holds per row ──────────────────── */
    {
        sfb_config_t c;

        sfb_config_default(&c, df, T_DIM);
        b = NULL;
        CK(sfb_open(&c, &b) == SFB_OK);
        if (b != NULL) {
            CK_MSG(b->vecs.vecs != NULL, "f32 brain has no f32 vector array");
            CK_MSG(b->vecs.qcodes == NULL, "f32 brain also kept int8 codes");
            vec_f32 = b->vecs.dim * sizeof(float);
            sfb_close(b);
        }
        sfb_config_default(&c, dq, T_DIM);
        c.quantize_int8 = 1;
        b = NULL;
        CK(sfb_open(&c, &b) == SFB_OK);
        if (b != NULL) {
            CK_MSG(b->vecs.vecs == NULL,
                   "int8 brain still holds a redundant f32 corpus copy");
            CK_MSG(b->vecs.qcodes != NULL && b->vecs.qscale != NULL,
                   "int8 brain has no code array / scale table");
            vec_int8 = (uint64_t)b->vecs.dim + sizeof(float);
            sfb_close(b);
        }
    }
    printf("   RAM/row in the vector store: f32 = 4*dim = %llu B, "
           "int8 = dim+4 = %llu B, ratio %.2fx\n",
           (unsigned long long)vec_f32, (unsigned long long)vec_int8,
           (double)vec_f32 / (double)vec_int8);
    CK_MSG(vec_f32 == T_DIM * 4u && vec_int8 == T_DIM + 4u,
           "unexpected per-row sizes (%llu / %llu)",
           (unsigned long long)vec_f32, (unsigned long long)vec_int8);
    CK_MSG((double)vec_f32 / (double)vec_int8 >= 3.5,
           "vector store shrank only %.2fx", (double)vec_f32 / (double)vec_int8);
}

/* ── 4+5: reopen, recall, verify ────────────────────────────────────────── */
static void test_reopen_recall_verify(const char *df, const char *dq)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    uint64_t live = 0;
    char json[8192];
    int rc;

    printf("── reopen + recall + verify\n");

    sfb_config_default(&cfg, dq, T_DIM);
    cfg.quantize_int8 = 1;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "int8 reopen rc=%d (%s)", rc, sfb_strerror(rc));
    if (b != NULL) {
        uint32_t top10 = 0;
        int ok1;

        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == T_N, "int8 reopen: live=%llu of %u",
               (unsigned long long)live, T_N);
        ok1 = self_recall(b, T_DIM, T_N, 20, &top10);
        printf("   int8 self-recall after reopen: rank1 %d/20, in top10 %u/20\n",
               ok1, top10);
        CK_MSG(ok1 >= 19, "int8 self-recall rank1 %d/20 (expected >= 19)", ok1);
        CK_MSG(top10 >= 19, "int8 self-recall top10 %u/20 (expected >= 19)", top10);
        {
            sfb_stats_t st;

            CK(sfb_stats_get(b, &st) == SFB_OK);
            CK_MSG(st.records_dropped == 0, "int8 reopen dropped %llu records",
                   (unsigned long long)st.records_dropped);
            CK_MSG(st.segments_quarantined == 0, "int8 reopen quarantined segments");
        }
        sfb_close(b);
        b = NULL;
    }
    json[0] = '\0';
    rc = sfb_verify(dq, NULL, 0, json, sizeof json);
    CK_MSG(rc == SFB_OK, "sfb_verify(int8) rc=%d (%s) json=%s", rc,
           sfb_strerror(rc), json);
    CK_MSG(strstr(json, "\"ok\":true") != NULL, "verify json not ok: %s", json);

    sfb_config_default(&cfg, df, T_DIM);
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "float32 reopen rc=%d (%s)", rc, sfb_strerror(rc));
    if (b != NULL) {
        uint32_t top10 = 0;
        int ok1;

        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == T_N, "float32 reopen: live=%llu of %u",
               (unsigned long long)live, T_N);
        ok1 = self_recall(b, T_DIM, T_N, 5, &top10);
        printf("   float32 self-recall after reopen: rank1 %d/5, in top10 %u/5\n",
               ok1, top10);
        CK_MSG(ok1 == 5, "float32 self-recall rank1 %d/5", ok1);
        sfb_close(b);
    }
    json[0] = '\0';
    rc = sfb_verify(df, NULL, 0, json, sizeof json);
    CK_MSG(rc == SFB_OK, "sfb_verify(f32) rc=%d (%s) json=%s", rc,
           sfb_strerror(rc), json);
    CK_MSG(strstr(json, "\"ok\":true") != NULL, "f32 verify json not ok: %s", json);
}

/* ── 7: unknown flag bit is a loud format error ─────────────────────────── */
static int patch_unknown_flag(const char *dir)
{
    sfb_config_t cfg;
    sfb_store_t st;
    sfb_seg_entry_t *e;
    char path[4096], tmp[4096];
    FILE *f;
    uint8_t *buf;
    size_t len;
    uint32_t crc;
    int rc;

    sfb_config_default(&cfg, dir, T_DIM);
    rc = sfb_store_open(&st, &cfg, NULL);
    if (rc != SFB_OK)
        return rc;
    if (st.man.nseg == 0) {
        sfb_store_close(&st);
        return SFB_E_STATE;
    }
    e = &st.man.segs[st.man.nseg - 1];
    snprintf(path, sizeof path, "%s/seg-%06u-%06u.qtsq", st.seg_dir,
             (unsigned)e->gen, (unsigned)e->seq);
    snprintf(tmp, sizeof tmp, "%s.patched", path);

    f = fopen(path, "rb");
    if (f == NULL) { sfb_store_close(&st); return SFB_E_IO; }
    fseek(f, 0, SEEK_END);
    len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(len);
    if (buf == NULL || fread(buf, 1, len, f) != len) {
        if (buf != NULL) free(buf);
        fclose(f);
        sfb_store_close(&st);
        return SFB_E_IO;
    }
    fclose(f);

    /* set an undefined flag bit (bit 2) and re-derive the whole-file digest so
     * the manifest still matches: the ONLY thing wrong is the unknown flag. */
    buf[6] = (uint8_t)(buf[6] | 0x04u);
    crc = sfb_crc32c(buf, len);
    e->digest = crc;

    f = fopen(tmp, "wb");
    if (f == NULL || fwrite(buf, 1, len, f) != len) {
        if (f != NULL) fclose(f);
        free(buf);
        sfb_store_close(&st);
        return SFB_E_IO;
    }
    fclose(f);
    free(buf);
    if (rename(tmp, path) != 0) {
        sfb_store_close(&st);
        return SFB_E_IO;
    }
    rc = sfb_store_manifest_save(&st);
    sfb_store_close(&st);
    return rc;
}

static void test_unknown_flag(const char *dq, const char *dunknown)
{
    sfb_config_t cfg;
    sfb_store_t st;
    void *out = NULL;
    size_t len = 0;
    segfile_t *v;
    size_t nv = 0;
    int rc;

    printf("── unknown segment flag must fail loudly\n");
    rmrf(dunknown);
    {
        char cmd[1024];

        snprintf(cmd, sizeof cmd, "cp -R '%s' '%s'", dq, dunknown);
        CK_MSG(system(cmd) == 0, "cp -R failed");
    }
    CK(patch_unknown_flag(dunknown) == SFB_OK);

    sfb_config_default(&cfg, dunknown, T_DIM);
    rc = sfb_store_open(&st, &cfg, NULL);
    CK_MSG(rc == SFB_OK, "store_open on patched copy rc=%d (%s)", rc,
           sfb_strerror(rc));
    if (rc == SFB_OK) {
        rc = sfb_store_segment_read(&st, &st.man.segs[st.man.nseg - 1], &out,
                                    &len);
        CK_MSG(rc == SFB_E_CORRUPT,
               "reader accepted a segment with an unknown flag bit (rc=%d)", rc);
        CK_MSG(out == NULL, "reader returned bytes for an unknown flag bit");
        rc = sfb_store_segment_verify(&st, &st.man.segs[st.man.nseg - 1]);
        CK_MSG(rc == SFB_E_CORRUPT, "verify accepted an unknown flag bit (rc=%d)", rc);
        sfb_store_close(&st);
    }
    /* The decoder is the layer that must not misparse.  sfb_verify() in
     * sfb_ops.c re-checks CRCs and record framing but never looks at the flag
     * word, so it stays happy here (out of scope for this change; reported).
     * What matters is the brain: opening this store must NOT hand back
     * plausible-looking rows read out of a payload it cannot decode. */
    {
        sfb_brain_t *b = NULL;
        uint64_t live = 0;
        sfb_stats_t st2;

        sfb_config_default(&cfg, dunknown, T_DIM);
        cfg.quantize_int8 = 1;
        rc = sfb_open(&cfg, &b);
        CK_MSG(rc == SFB_OK, "brain open on patched copy rc=%d", rc);
        if (b != NULL) {
            CK(sfb_count(b, &live) == SFB_OK);
            CK(sfb_stats_get(b, &st2) == SFB_OK);
            printf("   brain open on the unknown-flag store: live=%llu, "
                   "dropped=%llu, quarantined=%llu\n",
                   (unsigned long long)live,
                   (unsigned long long)st2.records_dropped,
                   (unsigned long long)st2.segments_quarantined);
            /* patch_unknown_flag() patched the newest segment; with rows
             * grouped SFB_SEGMENT_CAP at a time that is the tail segment. */
            CK_MSG(live == T_N - (T_N % SFB_SEGMENT_CAP),
                   "unknown-flag segment was not rejected wholesale "
                   "(live=%llu)", (unsigned long long)live);
            CK_MSG(st2.records_dropped == (T_N % SFB_SEGMENT_CAP),
                   "records_dropped=%llu (expected the %u rows of that segment)",
                   (unsigned long long)st2.records_dropped, T_N % SFB_SEGMENT_CAP);
            CK_MSG(st2.segments_quarantined == 1,
                   "segments_quarantined=%llu",
                   (unsigned long long)st2.segments_quarantined);
            sfb_close(b);
        }
    }

    /* And the concrete property an older build relies on: the pre-W3.6 mask
     * (bit 0 only) rejects a real int8 header, so it cannot misparse it. */
    v = load_segments(dq, &nv);
    CK(v != NULL && nv > 0);
    if (v != NULL && nv > 0) {
        uint16_t flags = rd16(v[0].bytes + 6);

        CK_MSG((flags & SFB_SEG_FLAG_INT8) != 0, "first int8 segment flag=0x%04x",
               flags);
        CK_MSG((flags & ~(uint16_t)0x0001u) != 0,
               "old-build mask would have ACCEPTED this int8 segment");
        printf("   int8 segment flags=0x%04x; a bit0-only mask rejects it "
               "(SFB_E_CORRUPT), never misparses it\n", flags);
        free_segments(v, nv);
    }
}

/* ── 6: payload corruption is detected, repair drops the segment ────────── */
static void test_corruption(const char *dq, const char *dbad)
{
    char cmd[1024];
    char segdir[1024], path[2048];
    DIR *d;
    struct dirent *de;
    char victim[256] = {0};
    FILE *f;
    uint8_t hdr[48];
    char json[8192];
    uint64_t before_dropped;
    int rc;

    printf("── payload corruption: verify catches it, repair drops the segment\n");
    rmrf(dbad);
    snprintf(cmd, sizeof cmd, "cp -R '%s' '%s'", dq, dbad);
    CK_MSG(system(cmd) == 0, "cp -R failed");

    snprintf(segdir, sizeof segdir, "%s/%s", dbad, SFB_SEG_DIR);
    d = opendir(segdir);
    CK(d != NULL);
    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "seg-", 4) == 0) {
            snprintf(victim, sizeof victim, "%s", de->d_name);
            break;
        }
    }
    closedir(d);
    CK_MSG(victim[0] != '\0', "no segment to corrupt");

    snprintf(path, sizeof path, "%s/%s", segdir, victim);
    f = fopen(path, "r+b");
    CK(f != NULL);
    if (f == NULL)
        return;
    CK(fread(hdr, 1, sizeof hdr, f) == sizeof hdr);
    CK(fseek(f, (long)(T_SEG_HDR + 100u), SEEK_SET) == 0);
    {
        int ch = fgetc(f);

        CK(ch != EOF);
        CK(fseek(f, (long)(T_SEG_HDR + 100u), SEEK_SET) == 0);
        CK(fputc(ch ^ 0x5A, f) != EOF);       /* flip one payload byte       */
    }
    fclose(f);

    json[0] = '\0';
    rc = sfb_verify(dbad, NULL, 0, json, sizeof json);
    printf("   verify(corrupted) rc=%d json=%s\n", rc, json);
    CK_MSG(rc != SFB_OK, "sfb_verify missed a flipped payload byte");
    CK_MSG(strstr(json, "\"ok\":false") != NULL, "verify json claims ok");
    CK_MSG(strstr(json, "\"damaged_segments\":0") == NULL,
           "damaged_segments is still 0: %s", json);

    json[0] = '\0';
    rc = sfb_verify(dbad, NULL, 1, json, sizeof json);
    printf("   repair rc=%d json=%s\n", rc, json);
    CK_MSG(rc == SFB_OK, "repair rc=%d (%s)", rc, sfb_strerror(rc));
    CK_MSG(strstr(json, "\"damaged_segments\":0") != NULL,
           "repair did not clear the damage: %s", json);
    before_dropped = 0;
    {
        const char *p = strstr(json, "\"records_dropped_estimate\":");
        if (p != NULL)
            before_dropped = strtoull(p + strlen("\"records_dropped_estimate\":"),
                                      NULL, 10);
    }
    CK_MSG(before_dropped > 0, "repair dropped no records");

    /* the repaired store reopens with exactly the surviving rows */
    {
        sfb_config_t cfg;
        sfb_brain_t *b = NULL;
        uint64_t live = 0;

        sfb_config_default(&cfg, dbad, T_DIM);
        cfg.quantize_int8 = 1;
        rc = sfb_open(&cfg, &b);
        CK_MSG(rc == SFB_OK, "reopen after repair rc=%d", rc);
        if (b != NULL) {
            CK(sfb_count(b, &live) == SFB_OK);
            printf("   after repair: live=%llu of %u (dropped %llu)\n",
                   (unsigned long long)live, T_N,
                   (unsigned long long)before_dropped);
            CK_MSG((uint64_t)live + before_dropped == T_N,
                   "live %llu + dropped %llu != %u",
                   (unsigned long long)live, (unsigned long long)before_dropped,
                   T_N);
            sfb_close(b);
        }
        json[0] = '\0';
        rc = sfb_verify(dbad, NULL, 0, json, sizeof json);
        CK_MSG(rc == SFB_OK, "verify after repair rc=%d json=%s", rc, json);
    }
}

/* ── 8: sealed variants of both encodings ───────────────────────────────── */
static void test_sealed(const char *dsf, const char *dsq)
{
    const char *key = "correct horse battery staple";
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    uint64_t live = 0;
    segsum_t sf, sq;
    char json[8192];
    int rc;

    printf("── sealed (AES-256-GCM) f32 and int8 stores\n");
    CK(build_store(dsf, T_SEALDIM, T_SEALN, sealed_corpus, key, 0) == SFB_OK);
    CK(build_store(dsq, T_SEALDIM, T_SEALN, sealed_corpus, key, 1) == SFB_OK);
    CK(scan_segments(dsf, &sf));
    CK(scan_segments(dsq, &sq));
    CK_MSG(sf.all_sealed && !sf.any_int8, "sealed f32 segment flags wrong");
    CK_MSG(sq.all_sealed && sq.all_int8,
           "sealed int8 segments must carry bit0|bit1 (all_sealed=%d all_int8=%d)",
           sq.all_sealed, sq.all_int8);
    printf("   sealed f32 payload=%llu B, sealed int8 payload=%llu B, ratio %.2fx\n",
           (unsigned long long)sf.payload, (unsigned long long)sq.payload,
           (double)sf.payload / (double)sq.payload);

    sfb_config_default(&cfg, dsq, T_SEALDIM);
    cfg.quantize_int8 = 1;
    cfg.key = key;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "sealed int8 reopen rc=%d (%s)", rc, sfb_strerror(rc));
    if (b != NULL) {
        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == T_SEALN, "sealed int8 reopen live=%llu of %u",
               (unsigned long long)live, T_SEALN);
        sfb_close(b);
        b = NULL;
    }
    /* a sealed int8 store must not open without the key */
    sfb_config_default(&cfg, dsq, T_SEALDIM);
    cfg.quantize_int8 = 1;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc != SFB_OK, "sealed int8 store opened without the passphrase");
    if (b != NULL)
        sfb_close(b);

    json[0] = '\0';
    rc = sfb_verify(dsq, key, 0, json, sizeof json);
    CK_MSG(rc == SFB_OK, "verify(sealed int8) rc=%d json=%s", rc, json);
    json[0] = '\0';
    rc = sfb_verify(dsf, key, 0, json, sizeof json);
    CK_MSG(rc == SFB_OK, "verify(sealed f32) rc=%d json=%s", rc, json);

    sfb_config_default(&cfg, dsf, T_SEALDIM);
    cfg.key = key;
    b = NULL;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "sealed f32 reopen rc=%d (%s)", rc, sfb_strerror(rc));
    if (b != NULL) {
        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == T_SEALN, "sealed f32 reopen live=%llu of %u",
               (unsigned long long)live, T_SEALN);
        sfb_close(b);
    }
}

/* ── 9: retention compaction in int8 mode ───────────────────────────────── */
/* sfb_retain() compacts the vector array in place; in int8 mode it must move
 * codes+scales and never build an f32 corpus array.  Ids are renumbered, so a
 * row that was id i must still be found by its vector under its new id. */
static void test_retain_int8(const char *dret)
{
    const uint32_t n = 200u, dim = 128u, drop = 40u;
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    uint64_t live = 0;
    sfb_hit_t hits[8];
    uint32_t got = 0;
    char json[4096];
    int rc;

    printf("── retention compaction in int8 mode (codes + scales, no f32 array)\n");
    CK(build_store(dret, dim, n, sealed_corpus, NULL, 1) == SFB_OK);
    sfb_config_default(&cfg, dret, dim);
    cfg.quantize_int8 = 1;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "int8 retain store open rc=%d", rc);
    if (b == NULL)
        return;
    for (uint32_t i = 0; i < drop; i++)
        CK_MSG(sfb_forget(b, i) == SFB_OK, "forget(%u) failed", i);
    rc = sfb_retain(b);
    CK_MSG(rc == SFB_OK, "sfb_retain rc=%d (%s)", rc, sfb_strerror(rc));
    CK(sfb_count(b, &live) == SFB_OK);
    CK_MSG(live == n - drop, "after retain live=%llu of %u",
           (unsigned long long)live, n - drop);
    CK_MSG(b->vecs.vecs == NULL, "retain materialised an f32 corpus array");
    CK_MSG(b->vecs.qcodes != NULL && b->vecs.qscale != NULL,
           "retain lost the code array or the scale table");
    /* row 150 survives, renumbered to 150 - 40 = 110 */
    CK(sfb_recall_ex(b, sealed_corpus + (size_t)150 * dim, dim, 5, hits, &got) ==
       SFB_OK);
    CK_MSG(got > 0 && hits[0].id == 150u - drop,
           "survivor recall after retain: got=%u id=%u (expected %u)", got,
           got > 0 ? hits[0].id : 0u, 150u - drop);
    sfb_close(b);

    /* and it is durable */
    b = NULL;
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "reopen after retain rc=%d", rc);
    if (b != NULL) {
        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == n - drop, "reopened after retain live=%llu of %u",
               (unsigned long long)live, n - drop);
        /* Renumbering after a compaction is a RAM-side property: the segments
         * still name the survivor 150 (the 40 forgotten rows come back as
         * tombstones, dead and invisible), so its id is 150 again here. */
        got = 0;
        CK(sfb_recall_ex(b, sealed_corpus + (size_t)150 * dim, dim, 5, hits,
                         &got) == SFB_OK);
        CK_MSG(got > 0 && hits[0].id == 150u,
               "survivor recall after retain+reopen: id=%u (expected 150; "
               "renumbering is RAM-only)", got > 0 ? hits[0].id : 0u);
        sfb_close(b);
    }
    json[0] = '\0';
    rc = sfb_verify(dret, NULL, 0, json, sizeof json);
    CK_MSG(rc == SFB_OK, "verify after retain rc=%d json=%s", rc, json);
}

int main(void)
{
    /* Per-process directories: two copies of this binary (or a stale leftover
     * from a killed run) must never share a store, or one run's rmrf() pulls
     * the files out from under the other's open brain. */
    char p_f32[64], p_i8[64], p_unk[64], p_bad[64], p_sf[64], p_sq[64], p_ret[64];
    const char *df, *dq, *dunk, *dbad, *dsf, *dsq, *dret;
    uint64_t bytes_f32 = 0, bytes_int8 = 0;

    snprintf(p_f32, sizeof p_f32, "/tmp/sfb_int8_%ld_f32", (long)getpid());
    snprintf(p_i8, sizeof p_i8, "/tmp/sfb_int8_%ld_i8", (long)getpid());
    snprintf(p_unk, sizeof p_unk, "/tmp/sfb_int8_%ld_unk", (long)getpid());
    snprintf(p_bad, sizeof p_bad, "/tmp/sfb_int8_%ld_bad", (long)getpid());
    snprintf(p_sf, sizeof p_sf, "/tmp/sfb_int8_%ld_sf", (long)getpid());
    snprintf(p_sq, sizeof p_sq, "/tmp/sfb_int8_%ld_sq", (long)getpid());
    snprintf(p_ret, sizeof p_ret, "/tmp/sfb_int8_%ld_ret", (long)getpid());
    df = p_f32; dq = p_i8; dunk = p_unk; dbad = p_bad;
    dsf = p_sf; dsq = p_sq; dret = p_ret;

    printf("── W3.6 INT8 quantisation\n");
    make_corpus();

    test_footprint_and_layout(df, dq, &bytes_f32, &bytes_int8);
    test_reopen_recall_verify(df, dq);
    test_unknown_flag(dq, dunk);
    test_corruption(dq, dbad);
    test_sealed(dsf, dsq);
    test_retain_int8(dret);

    printf("\n   RESULT payload/whole-store: f32 %llu B vs int8 %llu B "
           "(%.2fx)\n", (unsigned long long)bytes_f32,
           (unsigned long long)bytes_int8,
           bytes_int8 ? (double)bytes_f32 / (double)bytes_int8 : 0.0);

    rmrf(df); rmrf(dq); rmrf(dunk); rmrf(dbad); rmrf(dsf); rmrf(dsq);
    rmrf(dret);
    free(corpus);
    free(texts);
    free(sealed_corpus);

    printf("test_int8: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_INT8 FAILED\n"); return 1; }
    printf("=== TEST_INT8 PASSED ===\n");
    return 0;
}
