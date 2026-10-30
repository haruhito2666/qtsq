/*
 * tests/test_w5_semantics.c — Sofuu Brain v2: semantic-correctness suite.
 *
 * One test group per defect the W5 audit confirmed against the plan, each
 * written so that the bug it covers makes it FAIL:
 *
 *   1. W5.1  dedup distance units: SFB_DEDUP_L2 is a EUCLIDEAN radius and
 *            sfb_hnsw_search() reports SQUARED L2.  The gate used to compare
 *            the two directly, so the graph leg was sqrt(0.05) ≈ 0.2236 wide.
 *   2. W5.3  the decay policy (mode + half-life + floor) is persisted in the
 *            manifest and inherited by a reopen without an explicit config;
 *            TTL is covered; legacy (pre-W5.3) manifests still open.
 *   3. W5.4  "a pinned row is at least SFB_PIN_FLOOR strong" is an invariant,
 *            including with pins_immune = 0.
 *   4. W5.5  consolidation: members tombstoned, exactly one representative per
 *            group with role "representative" (never "summary"), its text and
 *            vector taken from a real member, all of it durable across reopen.
 *   5. W4.6  fusion: the +1.0 BM25-dominance and +2.0 exact-substring boosts
 *            no longer outvote the RRF budget (~0.049); exact phrase is an
 *            explicit first tier instead; BM25 uses real corpus idf.
 *   6. W3.3  open is lazy (0 payload reads, everything still works) and
 *            segments_verified moves on the verify_full path.
 *
 * Internal state is inspected through sfb_internal.h on purpose: the manifest
 * layout and the lazy-open counters have no public accessor, and a test that
 * cannot see them cannot pin them.
 *
 * Every check prints one [ok]/[FAIL] line.  Temp state lives in
 * /tmp/sfb_w5_<pid>/ and is removed on exit.
 *
 * Author: Haruhito
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "sfb_internal.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── harness ────────────────────────────────────────────────────────────── */
/* Mirrored from sfb_brain.c (deliberately not exported): the dedup radius is
 * a EUCLIDEAN distance and the graph reports SQUARED L2, so the graph-leg test
 * is 0.05^2.  If these two ever disagree with sfb_brain.c the unit pin in 1e
 * is what fails. */
#define W5_DEDUP_L2    0.05
#define W5_DEDUP_L2_SQ (W5_DEDUP_L2 * W5_DEDUP_L2)

static int g_checks, g_pass, g_fail;
static char g_base[512];

static void check(int cond, const char *name, const char *fmt, ...)
{
    va_list ap;

    g_checks++;
    if (cond) {
        g_pass++;
        printf("  [ok]   %s\n", name);
        fflush(stdout);
        return;
    }
    g_fail++;
    printf("  [FAIL] %s", name);
    if (fmt != NULL && fmt[0] != '\0') {
        printf("  |  ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
    fflush(stdout);
}

static void check_u64(const char *name, uint64_t want, uint64_t got)
{
    char buf[192];

    if (want == got) {
        check(1, name, NULL);
        return;
    }
    snprintf(buf, sizeof buf, "expected %llu, actual %llu",
             (unsigned long long)want, (unsigned long long)got);
    check(0, name, "%s", buf);
}

static const char *err_name(int rc)
{
    switch (rc) {
    case SFB_OK: return "SFB_OK";
    case SFB_E_INVALID: return "SFB_E_INVALID";
    case SFB_E_IO: return "SFB_E_IO";
    case SFB_E_NOMEM: return "SFB_E_NOMEM";
    case SFB_E_NOTFOUND: return "SFB_E_NOTFOUND";
    case SFB_E_STATE: return "SFB_E_STATE";
    case SFB_E_DIM: return "SFB_E_DIM";
    case SFB_E_CORRUPT: return "SFB_E_CORRUPT";
    case SFB_E_READONLY: return "SFB_E_READONLY";
    case SFB_E_CAPACITY: return "SFB_E_CAPACITY";
    default: return "?";
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int rm_rf(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char child[4096];

    if (lstat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if ((d = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        (void)rm_rf(child);
    }
    closedir(d);
    return rmdir(path);
}

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f;
    long n;
    uint8_t *buf;

    *out = NULL;
    *len = 0;
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0
        || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *len = (size_t)n;
    return 0;
}

static int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return -1;
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mkpath(char *out, size_t cap, const char *sub)
{
    snprintf(out, cap, "%s/%s", g_base, sub);
}

/* ── brain helpers ─────────────────────────────────────────────────────── */
static sfb_brain_t *open_at(const char *dir, uint32_t dim, int verify_full)
{
    sfb_config_t c;
    sfb_brain_t *b = NULL;
    int rc;

    sfb_config_default(&c, dir, dim);
    c.verify_full = verify_full;
    rc = sfb_open(&c, &b);
    if (rc != SFB_OK) {
        printf("  (sfb_open(%s) -> %s)\n", dir, err_name(rc));
        return NULL;
    }
    return b;
}

/* A reopen that says NOTHING about the decay policy: exactly the shape a
 * caller gets from memset(0) + path + dim. */
static sfb_brain_t *open_zeroed(const char *dir, uint32_t dim)
{
    sfb_config_t c;
    sfb_brain_t *b = NULL;
    int rc;

    memset(&c, 0, sizeof c);
    c.path = dir;
    c.dim = dim;
    rc = sfb_open(&c, &b);
    if (rc != SFB_OK) {
        printf("  (zeroed sfb_open(%s) -> %s)\n", dir, err_name(rc));
        return NULL;
    }
    return b;
}

static uint64_t brain_count(sfb_brain_t *b)
{
    uint64_t n = 0;

    (void)sfb_count(b, &n);
    return n;
}

static int remember(sfb_brain_t *b, const float *v, const char *text,
                    const char *role, sfb_remember_result_t *res)
{
    sfb_remember_result_t local;

    memset(&local, 0, sizeof local);
    if (res == NULL)
        res = &local;
    return sfb_remember(b, v, text, role, res);
}

/* Bookend a vector list with zeros so a single component can carry the whole
 * distance (keeps the fixtures readable). */
static void vec_zero(float *v, uint32_t dim)
{
    memset(v, 0, (size_t)dim * sizeof(float));
}

/* Strength of a live row, or -1 when it is not listed. */
static double listed_strength(sfb_brain_t *b, uint32_t id, int *found)
{
    sfb_hit_t hits[256];
    uint32_t n = 0, i;

    if (found != NULL)
        *found = 0;
    if (sfb_list(b, 0, 256, hits, &n) != SFB_OK)
        return -1.0;
    for (i = 0; i < n; i++) {
        if (hits[i].id == id) {
            if (found != NULL)
                *found = 1;
            return (double)hits[i].strength;
        }
    }
    return -1.0;
}

static sfb_flush_result_t flush_brain(sfb_brain_t *b)
{
    sfb_flush_result_t fr;

    memset(&fr, 0, sizeof fr);
    (void)sfb_flush(b, &fr);
    return fr;
}

/* ═ 1. W5.1 — one dedup radius, one unit ═════════════════════════════════ */
#define D1_DIM 128u

static void test_w51_dedup_units(void)
{
    const uint32_t dim = D1_DIM;
    char dir[600];
    float *v = (float *)malloc((size_t)dim * sizeof(float));
    float *w = (float *)malloc((size_t)dim * sizeof(float));
    float *w16 = (float *)malloc((size_t)dim * sizeof(float));
    sfb_remember_result_t r1, r2;
    sfb_brain_t *b;
    uint64_t live;
    int rc;

    printf("\n── W5.1 dedup gate: SFB_DEDUP_L2 is a Euclidean radius\n");
    if (v == NULL || w == NULL || w16 == NULL) {
        free(v); free(w); free(w16);
        check(0, "1.0 fixture allocation", "out of memory");
        return;
    }

    /* ── (a) Euclidean 0.2 (squared 0.04) + identical text must NOT merge.
     * Squared 0.04 is INSIDE the old graph-leg test (nh.dist < 0.05) and
     * outside the intended radius (0.0025), so this is the regression pin. */
    mkpath(dir, sizeof dir, "dedup_far_textsame");
    b = open_at(dir, dim, 0);
    check(b != NULL, "1a brain open", NULL);
    if (b != NULL) {
        vec_zero(v, dim);
        vec_zero(w, dim);
        v[0] = 0.25f;
        w[0] = 0.25f + 0.2f;              /* Euclidean distance exactly 0.2 */
        r1.id = r2.id = 0;
        rc = remember(b, v, "identical label for both rows", NULL, &r1);
        check(rc == SFB_OK && r1.merged == 0, "1a first row inserted",
              "rc=%s merged=%d", err_name(rc), r1.merged);
        rc = remember(b, w, "identical label for both rows", NULL, &r2);
        live = brain_count(b);
        check(rc == SFB_OK && r2.merged == 0 && live == 2u,
              "1a Euclidean 0.2 (squared 0.04 < old 0.05 gate) + identical text does NOT merge",
              "rc=%s merged=%d into=%u live=%llu (want merged=0, live=2; the old "
              "squared-vs-Euclidean comparison folded this pair)",
              err_name(rc), r2.merged, r2.into, (unsigned long long)live);
        (void)sfb_close(b);
    }

    /* ── (b) the exact-text gate still works: Euclidean 0.03 DOES merge. */
    mkpath(dir, sizeof dir, "dedup_near_textsame");
    b = open_at(dir, dim, 0);
    check(b != NULL, "1b brain open", NULL);
    if (b != NULL) {
        vec_zero(v, dim);
        vec_zero(w, dim);
        v[0] = 0.25f;
        w[0] = 0.25f + 0.03f;
        r1.id = r2.id = 0;
        (void)remember(b, v, "same words in the same order", NULL, &r1);
        rc = remember(b, w, "same words in the same order", NULL, &r2);
        live = brain_count(b);
        check(rc == SFB_OK && r2.merged == 1 && r2.into == r1.id && live == 1u,
              "1b Euclidean 0.03 + identical text still merges (gate is alive)",
              "rc=%s merged=%d into=%u (want 1 into %u), live=%llu",
              err_name(rc), r2.merged, r2.into, r1.id,
              (unsigned long long)live);
        (void)sfb_close(b);
    }

    /* ── (c) Jaccard < 0.85 with a very close vector must not merge. */
    mkpath(dir, sizeof dir, "dedup_close_fartext");
    b = open_at(dir, dim, 0);
    check(b != NULL, "1c brain open", NULL);
    if (b != NULL) {
        vec_zero(v, dim);
        vec_zero(w, dim);
        v[0] = 0.5f;
        w[0] = 0.51f;                     /* Euclidean 0.01, squared 1e-4  */
        r1.id = r2.id = 0;
        (void)remember(b, v, "alpha beta gamma", NULL, &r1);
        rc = remember(b, w, "kappa lambda mu", NULL, &r2);
        live = brain_count(b);
        check(rc == SFB_OK && r2.merged == 0 && live == 2u,
              "1c Jaccard 0 with a Euclidean-0.01 vector does NOT merge",
              "rc=%s merged=%d live=%llu", err_name(rc), r2.merged,
              (unsigned long long)live);
        (void)sfb_close(b);
    }

    /* ── (d) POSITIVE fuzzy merge through the graph leg: Jaccard >= 0.85 and
     * a vector inside the radius.  19 shared tokens of 21 union = 0.9048. */
    mkpath(dir, sizeof dir, "dedup_fuzzy_positive");
    b = open_at(dir, dim, 0);
    check(b != NULL, "1d brain open", NULL);
    if (b != NULL) {
        char ta[512], tb[512];
        size_t off;

        ta[0] = tb[0] = '\0';
        for (off = 0; off < 19u; off++) {
            char tok[32];

            snprintf(tok, sizeof tok, "tok%02u ", (unsigned)off);
            strcat(ta, tok);
            strcat(tb, tok);
        }
        strcat(ta, "tailA");             /* 19 + 1 = 20 distinct tokens    */
        strcat(tb, "tailB");             /* 19 + 1 = 20, union 21          */
        vec_zero(v, dim);
        vec_zero(w, dim);
        v[0] = -0.5f;
        w[0] = -0.47f;                   /* Euclidean 0.03                 */
        r1.id = r2.id = 0;
        (void)remember(b, v, ta, NULL, &r1);
        rc = remember(b, w, tb, NULL, &r2);
        live = brain_count(b);
        check(rc == SFB_OK && r2.merged == 1 && r2.into == r1.id && live == 1u,
              "1d Jaccard 0.905 + Euclidean 0.03 DOES merge (positive graph-leg case)",
              "rc=%s merged=%d into=%u (want 1 into %u), live=%llu",
              err_name(rc), r2.merged, r2.into, r1.id,
              (unsigned long long)live);
        (void)sfb_close(b);
    }

    /* ── (e) unit pin, read straight off the graph: the distance the HNSW
     * returns for a 0.03-apart pair is 0.0009, not 0.03.  If this ever
     * becomes Euclidean, SFB_DEDUP_L2_SQ is the wrong constant. */
    mkpath(dir, sizeof dir, "dedup_units");
    b = open_at(dir, dim, 0);
    check(b != NULL, "1e brain open", NULL);
    if (b != NULL) {
        sfb_hnsw_hit_t hit;
        uint32_t got = 0;

        memset(&hit, 0, sizeof hit);
        vec_zero(v, dim);
        vec_zero(w, dim);
        v[0] = 1.0f;
        w[0] = 1.03f;
        (void)remember(b, v, "unit probe row", NULL, NULL);
        got = 0;
        if (sfb_hnsw_search(b->index, w, 1u, 64u, &hit, &got) == SFB_OK && got > 0)
            check(fabs((double)hit.dist - 0.0009) < 2e-5,
                  "1e sfb_hnsw_search reports SQUARED L2 (0.03 -> 0.0009)",
                  "dist=%.6f (want 0.0009); SFB_DEDUP_L2_SQ = %.6f",
                  (double)hit.dist, (double)W5_DEDUP_L2_SQ);
        else
            check(0, "1e sfb_hnsw_search reports SQUARED L2", "search failed");
        (void)sfb_close(b);
    }

    free(v);
    free(w);
    free(w16);
}

/* ═ 2. W5.3 — the decay policy is part of the brain ══════════════════════ */
#define D2_DIM  64u
#define D2_HL   100.0

static void test_w53_policy_persistence(void)
{
    const uint32_t dim = D2_DIM;
    char dir[600], man[700];
    float v[D2_DIM];
    sfb_config_t c;
    sfb_brain_t *b;
    sfb_remember_result_t rr;
    uint32_t id0 = 0;
    double st;
    int found = 0, rc;

    printf("\n── W5.3 decay policy persisted in the manifest\n");

    /* ── (a) configure EXPONENTIAL + half-life 100, write, close ───────── */
    mkpath(dir, sizeof dir, "policy_exp");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_EXPONENTIAL;
    c.half_life_secs = D2_HL;
    c.decay_floor = 0.01;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2a open exponential brain",
          "rc=%s", err_name(rc));
    if (b == NULL)
        return;
    vec_zero(v, dim);
    v[1] = 1.0f;
    (void)remember(b, v, "policy row zero", NULL, &rr);
    id0 = rr.id;
    (void)remember(b, v, "policy row one", NULL, NULL);
    (void)remember(b, v, "policy row two", NULL, NULL);
    (void)flush_brain(b);
    (void)sfb_close(b);

    /* the manifest really carries the policy block */
    snprintf(man, sizeof man, "%s/brain.qtsq", dir);
    {
        uint8_t *raw = NULL;
        size_t len = 0;

        if (read_file(man, &raw, &len) == 0 && raw != NULL) {
            uint32_t nseg = get_u32le(raw + 36);
            uint16_t flags = get_u16le(raw + 6);
            uint32_t magic = get_u32le(raw + SFB_MAN_HDR_LEN
                                       + (size_t)nseg * SFB_MAN_ENT_LEN);

            check_u64("2b manifest keeps format version 2",
                      (uint64_t)SFB_FORMAT_VERSION, (uint64_t)get_u16le(raw + 4));
            check((flags & SFB_MAN_F_POLICY) != 0,
                  "2b manifest flags advertise the policy block",
                  "flags=0x%04x", (unsigned)flags);
            check(len == (size_t)SFB_MAN_HDR_LEN
                          + (size_t)nseg * SFB_MAN_ENT_LEN
                          + (size_t)SFB_MAN_POLICY_LEN + 4u,
                  "2b manifest size = header + table + policy block + crc",
                  "size=%llu nseg=%u", (unsigned long long)len, nseg);
            check(magic == SFB_MAGIC_POLICY,
                  "2b policy block starts with SFB_MAGIC_POLICY",
                  "magic=0x%08x", magic);
            check(get_u16le(raw + SFB_MAN_HDR_LEN
                            + (size_t)nseg * SFB_MAN_ENT_LEN + 6) ==
                      (uint16_t)SFB_DECAY_EXPONENTIAL,
                  "2b policy block stores decay mode EXPONENTIAL", NULL);
            free(raw);
        } else {
            check(0, "2b manifest readable", "read_file failed");
        }
    }

    /* ── (c) reopen with a ZEROED config: the policy must be inherited ─── */
    b = open_zeroed(dir, dim);
    check(b != NULL, "2c reopen with a zeroed config succeeds", NULL);
    if (b != NULL) {
        check_u64("2c all rows are there", 3u, brain_count(b));
        st = listed_strength(b, id0, &found);
        check(found && st > 0.9, "2c strength starts at 1.0",
              "found=%d strength=%.4f", found, st);
        /* sfb_decay_tick has always applied exp(-seconds / half_life), i.e.
         * half_life_secs is an e-folding time despite the name; one
         * half_life of time leaves 1/e = 0.3679.  What matters here is that
         * the number is the INHERITED 100 s and not the 86400 s default. */
        (void)sfb_decay_tick(b, (uint64_t)D2_HL);
        st = listed_strength(b, id0, &found);
        check(found && st > 0.30 && st < 0.45,
              "2c zeroed reopen inherited EXPONENTIAL and half_life=100 (one e-fold)",
              "found=%d strength=%.4f (want ~0.3679; NONE would stay 1.0, the "
              "default 86400 s half-life would stay ~0.9988)",
              found, st);
        (void)sfb_close(b);
    }

    /* ── (d) TTL is persisted too ─────────────────────────────────────── */
    mkpath(dir, sizeof dir, "policy_ttl");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_TTL;
    c.half_life_secs = 0.01;              /* TTL deadline = 2 * hl = 20 ms  */
    c.decay_floor = 0.01;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2d open TTL brain", "rc=%s", err_name(rc));
    if (b != NULL) {
        (void)remember(b, v, "ttl row", NULL, NULL);
        (void)flush_brain(b);
        (void)sfb_close(b);

        b = open_zeroed(dir, dim);
        check(b != NULL, "2d reopen TTL brain with a zeroed config", NULL);
        if (b != NULL) {
            check_u64("2d row is live right after the reopen", 1u, brain_count(b));
            sleep_ms(60);                 /* > TTL deadline of 20 ms        */
            (void)sfb_decay_tick(b, 0u);
            check_u64("2d the inherited TTL policy expired the row",
                      0u, brain_count(b));
            (void)sfb_close(b);
        }
    }

    /* ── (e) a NONE brain stays NONE ──────────────────────────────────── */
    mkpath(dir, sizeof dir, "policy_none");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_NONE;             /* explicit: default fills the rest */
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2e open NONE brain", "rc=%s", err_name(rc));
    if (b != NULL) {
        sfb_remember_result_t r2;

        (void)remember(b, v, "none row", NULL, &r2);
        (void)flush_brain(b);
        (void)sfb_close(b);

        b = open_zeroed(dir, dim);
        check(b != NULL, "2e reopen NONE brain with a zeroed config", NULL);
        if (b != NULL) {
            (void)sfb_decay_tick(b, 1000000u);
            st = listed_strength(b, r2.id, &found);
            check(found && st == 1.0,
                  "2e a NONE brain stays NONE after reopen (nothing decayed)",
                  "found=%d strength=%.6f (want exactly 1.0)", found, st);
            (void)sfb_close(b);
        }
    }

    /* ── (f) an explicit config still wins over the persisted policy ───── */
    b = NULL;
    mkpath(dir, sizeof dir, "policy_exp");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_TTL;              /* the store says EXPONENTIAL */
    c.half_life_secs = 0.01;
    c.decay_floor = 0.01;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2f reopen the EXP store with an explicit TTL config",
          "rc=%s", err_name(rc));
    if (b != NULL) {
        sleep_ms(60);
        (void)sfb_decay_tick(b, 0u);
        check_u64("2f the explicit config wins (TTL applied, not the stored EXPONENTIAL)",
                  0u, brain_count(b));
        (void)sfb_close(b);
    }

    /* ── (g) an OLD-FORMAT store still opens.  Built by rewriting the
     * manifest exactly as a pre-W5.3 build wrote it: no policy block, flag
     * bit clear, CRC over what remains. ───────────────────────────────── */
    mkpath(dir, sizeof dir, "policy_legacy");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_EXPONENTIAL;
    c.half_life_secs = 100.0;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2g build a policy-carrying store first",
          "rc=%s", err_name(rc));
    if (b != NULL) {
        (void)remember(b, v, "legacy row", NULL, &rr);
        (void)flush_brain(b);
        (void)sfb_close(b);

        snprintf(man, sizeof man, "%s/brain.qtsq", dir);
        {
            uint8_t *raw = NULL;
            size_t len = 0;

            if (read_file(man, &raw, &len) == 0 && raw != NULL) {
                uint32_t nseg = get_u32le(raw + 36);
                size_t keep = SFB_MAN_HDR_LEN
                            + (size_t)nseg * SFB_MAN_ENT_LEN;
                uint16_t flags = (uint16_t)(get_u16le(raw + 6)
                                            & ~SFB_MAN_F_POLICY);
                uint8_t *legacy;

                legacy = (uint8_t *)malloc(keep + 4u);
                if (legacy != NULL) {
                    memcpy(legacy, raw, keep);
                    put_u16le(legacy + 6, flags);
                    put_u32le(legacy + keep, sfb_crc32c(legacy, keep));
                    check(write_file(man, legacy, keep + 4u) == 0,
                          "2g rewrote the manifest in the pre-W5.3 layout", NULL);
                    free(legacy);
                }
                free(raw);
            } else {
                check(0, "2g manifest readable", "read_file failed");
            }
        }

        b = open_zeroed(dir, dim);
        check(b != NULL, "2g a pre-W5.3 manifest still opens", NULL);
        if (b != NULL) {
            check_u64("2g its rows are intact", 1u, brain_count(b));
            st = listed_strength(b, rr.id, &found);
            (void)sfb_decay_tick(b, 1000000u);
            {
                double st2 = listed_strength(b, rr.id, &found);

                check(found && st == 1.0 && st2 == 1.0,
                      "2g a legacy store inherits no policy (decay stays NONE, as before W5.3)",
                      "before=%.4f after=%.4f found=%d", st, st2, found);
            }
            (void)sfb_close(b);
        }
        /* the legacy layout survives a rewrite: nothing was configured, so
         * nothing is upgraded behind the caller's back */
        {
            uint8_t *raw = NULL;
            size_t len = 0;

            if (read_file(man, &raw, &len) == 0 && raw != NULL) {
                uint32_t nseg = get_u32le(raw + 36);

                check_u64("2g a legacy store is not silently upgraded",
                          (uint64_t)(SFB_MAN_HDR_LEN
                                     + (size_t)nseg * SFB_MAN_ENT_LEN + 4u),
                          (uint64_t)len);
                check((get_u16le(raw + 6) & SFB_MAN_F_POLICY) == 0,
                      "2g legacy manifest still has the policy flag clear", NULL);
                free(raw);
            } else {
                check(0, "2g manifest re-readable", "read_file failed");
            }
        }
    }

    /* ── (h) an unknown manifest flag bit is a loud SFB_E_CORRUPT ─────── */
    {
        uint8_t *raw = NULL;
        size_t len = 0;
        int bad = 0;

        mkpath(dir, sizeof dir, "policy_legacy");
        snprintf(man, sizeof man, "%s/brain.qtsq", dir);
        if (read_file(man, &raw, &len) == 0 && raw != NULL) {
            put_u16le(raw + 6,
                      (uint16_t)(get_u16le(raw + 6) | 0x0004u));
            put_u32le(raw + len - 4u, sfb_crc32c(raw, len - 4u));
            (void)write_file(man, raw, len);
            {
                sfb_brain_t *bb = NULL;
                sfb_config_t cz;

                memset(&cz, 0, sizeof cz);
                cz.path = dir;
                cz.dim = dim;
                bad = sfb_open(&cz, &bb);
                if (bad == SFB_OK && bb != NULL)
                    (void)sfb_close(bb);
                check(bad == SFB_E_CORRUPT && bb == NULL,
                      "2h an unknown manifest flag bit is rejected (SFB_E_CORRUPT)",
                      "rc=%s, *out %s", err_name(bad),
                      bb == NULL ? "NULL" : "non-NULL");
            }
            free(raw);
        } else {
            check(0, "2h manifest readable", "read_file failed");
        }
    }

    /* ── (i) a malformed policy block is rejected, not ignored ────────── */
    {
        uint8_t *raw = NULL;
        size_t len = 0;

        mkpath(dir, sizeof dir, "policy_exp2");
        sfb_config_default(&c, dir, dim);
        c.decay = SFB_DECAY_EXPONENTIAL;
        c.half_life_secs = 100.0;
        b = NULL;
        if (sfb_open(&c, &b) == SFB_OK && b != NULL) {
            (void)remember(b, v, "bad policy row", NULL, NULL);
            (void)flush_brain(b);
            (void)sfb_close(b);
        }
        snprintf(man, sizeof man, "%s/brain.qtsq", dir);
        if (read_file(man, &raw, &len) == 0 && raw != NULL) {
            uint32_t nseg = get_u32le(raw + 36);
            size_t off = SFB_MAN_HDR_LEN + (size_t)nseg * SFB_MAN_ENT_LEN;
            sfb_config_t cz;
            sfb_brain_t *bb = NULL;
            int rc2;

            put_u32le(raw + off, 0xDEADBEEFu);     /* bad policy magic */
            put_u32le(raw + len - 4u, sfb_crc32c(raw, len - 4u));
            (void)write_file(man, raw, len);
            memset(&cz, 0, sizeof cz);
            cz.path = dir;
            cz.dim = dim;
            rc2 = sfb_open(&cz, &bb);
            if (rc2 == SFB_OK && bb != NULL)
                (void)sfb_close(bb);
            check(rc2 == SFB_E_CORRUPT && bb == NULL,
                  "2i a malformed policy block is SFB_E_CORRUPT (never silently ignored)",
                  "rc=%s", err_name(rc2));
            free(raw);
        } else {
            check(0, "2i manifest readable", "read_file failed");
        }
    }

    /* ── (j) the policy survives maintenance: compact, backup + restore ──
     * Both re-save the manifest through the store, so the block must ride
     * along; a store that forgets its policy after a compact is just as bad as
     * one that never had it. */
    mkpath(dir, sizeof dir, "policy_maint");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_EXPONENTIAL;
    c.half_life_secs = D2_HL;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2j open the maintenance brain",
          "rc=%s", err_name(rc));
    if (b != NULL) {
        sfb_remember_result_t rm;
        char bak[640], rest[640];
        uint64_t before = 0, after = 0;

        (void)remember(b, v, "maint row a", NULL, &rm);
        (void)remember(b, v, "maint row b", NULL, NULL);
        (void)flush_brain(b);
        check(sfb_compact(b, &before, &after) == SFB_OK, "2j sfb_compact()",
              "before=%llu after=%llu", (unsigned long long)before,
              (unsigned long long)after);
        st = listed_strength(b, rm.id, &found);
        check(found && st > 0.9, "2j the row is still live after compact",
              "strength=%.4f", st);
        (void)sfb_close(b);

        /* Snapshot from a state where nothing has decayed yet, then prove the
         * policy on the compacted store and on the restored copy separately;
         * a tick persists the lower strength, so the order matters. */
        mkpath(bak, sizeof bak, "policy_maint_backup");
        mkpath(rest, sizeof rest, "policy_maint_restored");
        (void)rm_rf(bak);
        (void)rm_rf(rest);
        check(sfb_backup(dir, bak) == SFB_OK, "2j sfb_backup()", NULL);
        check(sfb_restore(bak, rest) == SFB_OK, "2j sfb_restore()", NULL);

        b = open_zeroed(dir, dim);
        check(b != NULL, "2j reopen the compacted brain with a zeroed config",
              NULL);
        if (b != NULL) {
            (void)sfb_decay_tick(b, (uint64_t)D2_HL);
            st = listed_strength(b, rm.id, &found);
            check(found && st > 0.30 && st < 0.45,
                  "2j the decay policy survived sfb_compact()",
                  "found=%d strength=%.4f (want ~0.3679)", found, st);
            (void)sfb_close(b);
        }

        b = open_zeroed(rest, dim);
        check(b != NULL, "2j reopen the restored brain with a zeroed config",
              NULL);
        if (b != NULL) {
            (void)sfb_decay_tick(b, (uint64_t)D2_HL);
            st = listed_strength(b, rm.id, &found);
            check(found && st > 0.30 && st < 0.45,
                  "2j the decay policy survived backup + restore",
                  "found=%d strength=%.4f (want ~0.3679)", found, st);
            (void)sfb_close(b);
        }
    }

    /* ── (k) the documented MODE_VECTOR_DB override ───────────────────── */
    mkpath(dir, sizeof dir, "policy_vdb");
    sfb_config_default(&c, dir, dim);
    c.mode = SFB_MODE_VECTOR_DB;          /* decay still EXPONENTIAL in cfg */
    c.half_life_secs = 10.0;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "2k open vector_db brain", "rc=%s", err_name(rc));
    if (b != NULL) {
        sfb_remember_result_t r2;

        (void)remember(b, v, "vdb row", NULL, &r2);
        (void)flush_brain(b);
        (void)sfb_close(b);

        b = open_zeroed(dir, dim);
        check(b != NULL, "2k reopen vector_db with a zeroed config", NULL);
        if (b != NULL) {
            (void)sfb_decay_tick(b, 1000000u);
            st = listed_strength(b, r2.id, &found);
            check(found && st == 1.0,
                  "2k MODE_VECTOR_DB keeps zero decay across a zeroed reopen (documented override)",
                  "found=%d strength=%.4f", found, st);
            (void)sfb_close(b);
        }
    }
}

/* ═ 3. W5.4 — the pin floor is an invariant ══════════════════════════════ */
#define D3_DIM 64u

static void test_w54_pin_floor(void)
{
    const uint32_t dim = D3_DIM;
    char dir[600];
    float v[D3_DIM];
    sfb_config_t c;
    sfb_brain_t *b;
    sfb_remember_result_t rr, rplain;
    double st;
    int found = 0;

    printf("\n── W5.4 the pin floor holds whatever pins_immune says\n");

    /* ── (a) sfb_pin() raises a low-strength row to the floor ─────────── */
    mkpath(dir, sizeof dir, "pin_floor_raise");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_EXPONENTIAL;
    c.half_life_secs = 10.0;
    c.pins_immune = 1;
    b = NULL;
    check(sfb_open(&c, &b) == SFB_OK && b != NULL, "3a open", NULL);
    if (b == NULL)
        return;
    vec_zero(v, dim);
    v[0] = 1.0f;
    (void)remember(b, v, "pin me after fading", NULL, &rr);
    (void)sfb_decay_tick(b, 20u);          /* one tick = 2 half-lives: ~0.135 */
    st = listed_strength(b, rr.id, &found);
    check(found && st < 0.2,
          "3a the row is below the pin floor before pinning",
          "found=%d strength=%.4f (want < 0.2)", found, st);
    check(sfb_pin(b, rr.id, 1) == SFB_OK, "3a sfb_pin() on a live row", NULL);
    st = listed_strength(b, rr.id, &found);
    check(found && st >= 0.1999 && st <= 0.2001,
          "3a sfb_pin() raised the strength to exactly SFB_PIN_FLOOR (0.2)",
          "found=%d strength=%.4f (want 0.2; with the floor code deleted this "
          "stays ~0.135)", found, st);
    (void)sfb_decay_tick(b, 1000000u);
    st = listed_strength(b, rr.id, &found);
    check(found && st >= 0.1999,
          "3a an immune pin never sinks below the floor", "found=%d strength=%.4f",
          found, st);
    (void)sfb_close(b);

    /* ── (b) pins_immune = 0: pins may fade, never below the floor ─────
     * The row is created with the pin role, so it is pinned from birth. */
    mkpath(dir, sizeof dir, "pin_floor_nonimmune");
    sfb_config_default(&c, dir, dim);
    c.decay = SFB_DECAY_EXPONENTIAL;
    c.half_life_secs = 10.0;
    c.pins_immune = 0;                     /* the knob that used to disable
                                            * the floor entirely            */
    b = NULL;
    check(sfb_open(&c, &b) == SFB_OK && b != NULL, "3b open with pins_immune = 0",
          NULL);
    if (b != NULL) {
        vec_zero(v, dim);
        v[0] = 1.0f;
        (void)remember(b, v, "pinned but not immune", SFB_PIN_ROLE, &rr);
        vec_zero(v, dim);
        v[0] = -1.0f;
        (void)remember(b, v, "plain row", NULL, &rplain);

        (void)sfb_decay_tick(b, 20u);
        st = listed_strength(b, rr.id, &found);
        check(found && st >= 0.1999 && st <= 0.2001,
              "3b pins_immune = 0: the pin faded to the floor and stopped there",
              "found=%d strength=%.4f (want exactly 0.2, not ~0.135 and not 0)",
              found, st);
        (void)sfb_decay_tick(b, 1000000u);
        st = listed_strength(b, rr.id, &found);
        check(found && st >= 0.1999,
              "3b pins_immune = 0: decay never takes a pinned row below 0.2",
              "found=%d strength=%.4f", found, st);
        {
            int plain_found = 0;
            double pst = listed_strength(b, rplain.id, &plain_found);

            check(!plain_found && pst < 0.0,
                  "3b the unpinned row in the same brain did decay away (control)",
                  "found=%d strength=%.4f", plain_found, pst);
        }
        check_u64("3b sfb_retain() keeps the pinned row", 1u,
                  (sfb_retain(b) == SFB_OK) ? brain_count(b) : 0u);
        (void)sfb_close(b);
    }
}

/* ═ 4. W5.5 — consolidation ══════════════════════════════════════════════ */
#define D4_DIM 64u

static void consolidate_texts(char out[3][64])
{
    snprintf(out[0], 64, "cluster member alpha");
    snprintf(out[1], 64, "cluster member bravo");
    snprintf(out[2], 64, "cluster member charlie");
}

static void test_w55_consolidate(void)
{
    const uint32_t dim = D4_DIM;
    char dir[600], texts[3][64];
    float v[D4_DIM];
    sfb_config_t c;
    sfb_brain_t *b;
    sfb_hit_t hits[32];
    sfb_stats_t st;
    uint32_t n = 0, i, reps = 0, rep_id = 0;
    const char *rep_text = NULL, *rep_role = NULL;
    int rep_tier = -1, rc;

    printf("\n── W5.5 consolidation: one representative per group, honestly labelled\n");
    consolidate_texts(texts);

    mkpath(dir, sizeof dir, "consolidate");
    sfb_config_default(&c, dir, dim);
    c.mode = SFB_MODE_VECTOR_DB;           /* no decay: the test is about tiers */
    c.decay = SFB_DECAY_NONE;
    b = NULL;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "4a open", "rc=%s", err_name(rc));
    if (b == NULL)
        return;

    /* three members inside the consolidation radius, two outsiders */
    for (i = 0; i < 3u; i++) {
        vec_zero(v, dim);
        v[0] = 0.4f + 0.01f * (float)i;    /* pairwise <= 0.02, well < 0.05  */
        rc = remember(b, v, texts[i], NULL, NULL);
        check(rc == SFB_OK, "4a cluster member inserted", "rc=%s", err_name(rc));
    }
    vec_zero(v, dim);
    v[0] = 10.0f;
    (void)remember(b, v, "outsider one", NULL, NULL);
    vec_zero(v, dim);
    v[0] = -10.0f;
    (void)remember(b, v, "outsider two", NULL, NULL);
    check_u64("4a five episodic rows", 5u, brain_count(b));

    check(sfb_consolidate(b) == SFB_OK, "4b sfb_consolidate()", NULL);
    check_u64("4b members tombstoned: two live rows left plus one representative",
              3u, brain_count(b));
    check(sfb_stats_get(b, &st) == SFB_OK && st.tombstones == 3u,
          "4b exactly the three group members are tombstoned",
          "tombstones=%llu (want 3)", (unsigned long long)st.tombstones);

    n = 0;
    check(sfb_list(b, 0, 32, hits, &n) == SFB_OK, "4c list the live set", NULL);
    for (i = 0; i < n; i++) {
        if (hits[i].role != NULL && strcmp(hits[i].role, "representative") == 0) {
            reps++;
            rep_id = hits[i].id;
            rep_text = hits[i].text;
            rep_role = hits[i].role;
            rep_tier = (int)hits[i].tier;
        }
    }
    check_u64("4c exactly one representative survives per group", 1u, reps);
    check(rep_role != NULL && strcmp(rep_role, "representative") == 0,
          "4c the representative's role is \"representative\"", "role=%s",
          rep_role != NULL ? rep_role : "(null)");
    check(rep_tier == (int)SFB_TIER_SEMANTIC,
          "4c the representative is promoted to SEMANTIC", "tier=%d", rep_tier);
    {
        int from_member = 0;

        for (i = 0; i < 3u; i++)
            if (rep_text != NULL && strcmp(rep_text, texts[i]) == 0)
                from_member = 1;
        check(from_member,
              "4c its text comes from a real member (no synthetic summary)",
              "text=\"%s\"", rep_text != NULL ? rep_text : "(null)");
    }
    {
        int any_summary = 0;

        for (i = 0; i < n; i++)
            if (hits[i].role != NULL && strcmp(hits[i].role, "summary") == 0)
                any_summary = 1;
        check(!any_summary,
              "4c no row is labelled \"summary\" (the label is not the role column's old meaning)",
              NULL);
    }
    /* vector from a real member: recalling the representative's own vector
     * must return it at distance 0 */
    if (reps == 1u) {
        uint32_t nh = 0;

        vec_zero(v, dim);
        /* find the member vector that produced the representative */
        for (i = 0; i < 3u; i++)
            if (rep_text != NULL && strcmp(rep_text, texts[i]) == 0)
                v[0] = 0.4f + 0.01f * (float)i;
        n = 0;
        if (sfb_recall(b, v, 2u, hits, &n) == SFB_OK && n > 0u) {
            nh = n;
            check(hits[0].id == rep_id && hits[0].distance < 1e-6f,
                  "4c its vector is the member's vector (recall by that vector is exact)",
                  "top id=%u (want %u), distance=%.6f", hits[0].id, rep_id,
                  (double)hits[0].distance);
        } else {
            check(0, "4c recall by the representative's vector", "no hits");
        }
        (void)nh;
    }

    /* ── reopen: the representative and its label survive ─────────────── */
    (void)flush_brain(b);
    (void)sfb_close(b);
    b = open_at(dir, dim, 0);
    check(b != NULL, "4d reopen", NULL);
    if (b != NULL) {
        sfb_stats_t st2;
        uint32_t reps2 = 0, n2 = 0;

        check_u64("4d live set after reopen", 3u, brain_count(b));
        if (sfb_stats_get(b, &st2) == SFB_OK)
            check_u64("4d members stay tombstoned across the reopen", 3u,
                      st2.tombstones);
        n2 = 0;
        (void)sfb_list(b, 0, 32, hits, &n2);
        for (i = 0; i < n2; i++)
            if (hits[i].role != NULL
                && strcmp(hits[i].role, "representative") == 0)
                reps2++;
        check_u64("4d the representative is still there after reopen", 1u, reps2);
        {
            int still = 0;

            for (i = 0; i < n2; i++)
                if (hits[i].id == rep_id && hits[i].role != NULL
                    && strcmp(hits[i].role, "representative") == 0
                    && hits[i].tier == SFB_TIER_SEMANTIC)
                    still = 1;
            check(still,
                  "4d its id, role and SEMANTIC tier survived the reopen", NULL);
        }
        check(sfb_consolidate(b) == SFB_OK, "4e a second consolidate runs", NULL);
        check_u64("4e consolidation is stable when the cluster is already collapsed",
                  3u, brain_count(b));
        (void)sfb_close(b);
    }
}

/* ═ 5. W4.6 — fusion and BM25 idf ════════════════════════════════════════ */
#define D5_DIM 64u

static void test_w46_idf(void)
{
    sfb_text_stats_t *st = (sfb_text_stats_t *)malloc(sizeof *st);
    double rare, common;

    printf("\n── W4.6(c) BM25 uses real corpus document frequency\n");
    if (st == NULL) {
        check(0, "5a stats allocation", "out of memory");
        return;
    }
    sfb_text_stats_begin(st, "rare common");
    sfb_text_stats_add(st, "rare x");
    sfb_text_stats_add(st, "common y");
    sfb_text_stats_add(st, "common z");
    check_u64("5a N = 3 documents", 3u, (uint64_t)st->n_docs);
    rare = sfb_text_bm25_stats(st, "rare x", 1.2, 0.75);
    common = sfb_text_bm25_stats(st, "common y", 1.2, 0.75);
    check(rare > 1.5 * common,
          "5a a term in 1 of 3 documents outweighs a term in 2 of 3 (same tf, same length)",
          "score(rare)=%.6f score(common)=%.6f (a constant idf scores these equal)",
          rare, common);
    {
        /* the single-document helper is still the constant-idf form, and the
         * numbers above must not be reproducible with it */
        double flat = sfb_text_bm25("rare common", "rare x", 2.0, 1.2, 0.75);

        check(fabs(flat - rare) > 1e-6,
              "5a the corpus form is not the constant-idf helper",
              "corpus=%.6f constant=%.6f", rare, flat);
    }
    free(st);
}

/* The fusion fixture.  Values are chosen so the LEXICAL winner is also far
 * from the centroid, while the second lexical row owns the vector and recency
 * lists — i.e. two signals against one. */
typedef struct {
    float  a[D5_DIM], b[D5_DIM], c[D5_DIM];
    char   ta[128], tb[128], tc[128];
} fusion_fixture_t;

static void fusion_fill(fusion_fixture_t *f)
{
    memset(f, 0, sizeof *f);
    f->a[0] = 0.0f;                    /* close to the centroid            */
    f->b[0] = 10.0f;                   /* the outlier                      */
    f->c[0] = -1.0f;
    snprintf(f->ta, sizeof f->ta, "beta gamma delta epsilon alpha zeta eta theta");
    snprintf(f->tb, sizeof f->tb, "alpha zeta beta");
    snprintf(f->tc, sizeof f->tc, "alpha");
}

/* BM25 of one live row, computed from the public live set (sfb_list) so the
 * fixture's "B is the lexical winner" claim is measured, not assumed. */
static double bm25_of(sfb_brain_t *b, const char *query, uint32_t id,
                      int *found)
{
    sfb_hit_t hits[64];
    sfb_text_stats_t *st;
    uint32_t n = 0, i;
    double s = -1.0;
    char target[512];

    if (found != NULL)
        *found = 0;
    target[0] = '\0';
    st = (sfb_text_stats_t *)malloc(sizeof *st);
    if (st == NULL)
        return -1.0;
    sfb_text_stats_begin(st, query);
    if (sfb_list(b, 0, 64, hits, &n) == SFB_OK) {
        for (i = 0; i < n; i++) {
            if (hits[i].id == id) {
                snprintf(target, sizeof target, "%s",
                         hits[i].text != NULL ? hits[i].text : "");
                if (found != NULL)
                    *found = 1;
            }
            sfb_text_stats_add(st, hits[i].text != NULL ? hits[i].text : "");
        }
    }
    if (target[0] != '\0')
        s = sfb_text_bm25_stats(st, target, 1.2, 0.75);
    free(st);
    return s;
}

static void test_w46_fusion(void)
{
    const uint32_t dim = D5_DIM;
    const char *query = "alpha beta";
    char dir[600];
    fusion_fixture_t *f = (fusion_fixture_t *)malloc(sizeof *f);
    sfb_config_t c;
    sfb_brain_t *b;
    sfb_hit_t hits[8];
    sfb_remember_result_t ra, rb, rc2;
    uint32_t n = 0;
    double bm_a, bm_b;
    int ok;

    printf("\n── W4.6(a)(b) fusion: two signals must beat one\n");
    if (f == NULL) {
        check(0, "5b fixture allocation", "out of memory");
        return;
    }
    fusion_fill(f);

    mkpath(dir, sizeof dir, "fusion");
    sfb_config_default(&c, dir, dim);
    c.mode = SFB_MODE_VECTOR_DB;
    c.decay = SFB_DECAY_NONE;
    b = NULL;
    ok = (sfb_open(&c, &b) == SFB_OK && b != NULL);
    check(ok, "5b open", NULL);
    if (!ok) {
        free(f);
        return;
    }

    /* insertion order sets recency: B oldest, C middle, A newest */
    check(remember(b, f->b, f->tb, NULL, &rb) == SFB_OK, "5b B inserted", NULL);
    check(remember(b, f->c, f->tc, NULL, &rc2) == SFB_OK, "5b C inserted", NULL);
    check(remember(b, f->a, f->ta, NULL, &ra) == SFB_OK, "5b A inserted", NULL);

    /* none of the texts contains the query verbatim, so the exact-phrase
     * first tier cannot explain the ordering below */
    check(strcasestr(f->ta, query) == NULL && strcasestr(f->tb, query) == NULL
              && strcasestr(f->tc, query) == NULL,
          "5b no fixture text contains the query verbatim (tier is not in play)",
          NULL);

    {
        int fa = 0, fb = 0;

        bm_b = bm25_of(b, query, rb.id, &fb);
        bm_a = bm25_of(b, query, ra.id, &fa);
        check(fa && fb, "5b both fixture rows are visible to sfb_list", NULL);
    }
    check(bm_b > bm_a,
          "5b B is the lexical winner", "bm25(B)=%.4f bm25(A)=%.4f", bm_b, bm_a);
    check(bm_b >= 1.5 * bm_a,
          "5b B's BM25 lead is >= 1.5x, i.e. the old +1.0 dominance boost would fire",
          "bm25(B)=%.4f bm25(A)=%.4f ratio=%.3f (the old code added +1.0 here)",
          bm_b, bm_a, bm_a > 0.0 ? bm_b / bm_a : 0.0);

    n = 0;
    check(sfb_recall_text(b, query, 3u, hits, &n) == SFB_OK && n == 3u,
          "5b recall_text returns all three rows", "n=%u", n);
    if (n == 3u) {
        check(hits[0].id == ra.id,
              "5b the two-signal row (medium BM25 + closest vector + newest) outranks the lexical winner",
              "top id=%u (want A=%u); order: %u, %u, %u",
              hits[0].id, ra.id, hits[0].id, hits[1].id, hits[2].id);
        check(hits[1].id == rb.id && hits[2].id == rc2.id,
              "5b the rest of the order is B then C",
              "order: %u, %u, %u (want %u, %u, %u)",
              hits[0].id, hits[1].id, hits[2].id, ra.id, rb.id, rc2.id);
        check(hits[0].score < 0.06f && hits[1].score < 0.06f,
              "5b fused scores stay inside the RRF budget (no +1.0/+2.0 boost)",
              "scores: %.6f, %.6f, %.6f (budget 3/61 = %.6f)",
              (double)hits[0].score, (double)hits[1].score,
              (double)hits[2].score, 3.0 / 61.0);
        check(hits[0].score >= hits[1].score && hits[1].score >= hits[2].score,
              "5b returned scores are monotonically non-increasing", NULL);
    }
    (void)sfb_close(b);

    /* ── exact-phrase first tier: a weak row containing the query verbatim
     * still wins, and its score is the tier marker plus a fused score — not
     * the old +2.0 ------------------------------------------------------- */
    mkpath(dir, sizeof dir, "fusion_phrase");
    sfb_config_default(&c, dir, dim);
    c.mode = SFB_MODE_VECTOR_DB;
    c.decay = SFB_DECAY_NONE;
    b = NULL;
    ok = (sfb_open(&c, &b) == SFB_OK && b != NULL);
    check(ok, "5c open", NULL);
    if (ok) {
        float weak[D5_DIM];
        sfb_remember_result_t rweak;

        (void)remember(b, f->a, "alpha filler filler filler filler filler filler beta",
                       NULL, NULL);
        vec_zero(weak, dim);
        weak[0] = 50.0f;               /* far from the centroid            */
        check(remember(b, weak, "alpha beta", NULL, &rweak) == SFB_OK,
              "5c insert the exact-phrase row", NULL);
        n = 0;
        (void)sfb_recall_text(b, query, 2u, hits, &n);
        check(n > 0u && hits[0].id == rweak.id && hits[0].score >= 1.0f,
              "5c an exact phrase wins as an explicit first tier",
              "top id=%u (want %u), score=%.4f", n ? hits[0].id : 0u, rweak.id,
              n ? (double)hits[0].score : 0.0);
        check(n > 1u && hits[1].score < 0.06f,
              "5c non-tier rows keep a fused score inside the RRF budget",
              "second score=%.6f", n > 1u ? (double)hits[1].score : 0.0);
        (void)sfb_close(b);
    }
    free(f);
}

/* ═ 6. W3.3 — lazy open and the verify counters ══════════════════════════ */
#define D6_DIM 128u
#define D6_N   300u

static int build_segstore(const char *dir, uint32_t dim, uint32_t n)
{
    sfb_config_t c;
    sfb_brain_t *b = NULL;
    uint32_t i;
    int rc;

    (void)rm_rf(dir);
    sfb_config_default(&c, dir, dim);
    c.mode = SFB_MODE_VECTOR_DB;
    c.decay = SFB_DECAY_NONE;
    rc = sfb_open(&c, &b);
    if (rc != SFB_OK)
        return rc;
    for (i = 0; i < n; i++) {
        float v[D6_DIM];
        char text[64];

        vec_zero(v, dim);
        v[i % dim] = (float)(i + 1u) * 0.001f;
        v[(i + 7u) % dim] = 1.0f;
        snprintf(text, sizeof text, "lazy row %u", (unsigned)i);
        rc = remember(b, v, text, NULL, NULL);
        if (rc != SFB_OK)
            break;
    }
    if (rc == SFB_OK)
        rc = sfb_flush(b, NULL);
    if (rc == SFB_OK) {
        uint64_t live = 0;

        rc = sfb_count(b, &live);
    }
    (void)sfb_close(b);
    return rc;
}

static void test_w33_lazy(void)
{
    const uint32_t dim = D6_DIM;
    char dir[600];
    sfb_config_t c;
    sfb_brain_t *b = NULL;
    uint64_t live = 0;
    int rc;

    printf("\n── W3.3 open is lazy; verify_full is the paranoid path\n");
    mkpath(dir, sizeof dir, "lazy");
    rc = build_segstore(dir, dim, D6_N);
    check(rc == SFB_OK, "6a fixture: 300 rows flushed", "rc=%s", err_name(rc));
    if (rc != SFB_OK)
        return;

    sfb_config_default(&c, dir, dim);
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "6b a normal open succeeds", "rc=%s",
          err_name(rc));
    if (b != NULL) {
        uint32_t nseg = b->store.man.nseg;

        check(nseg >= 2u, "6b the fixture spans several segments", "nseg=%u", nseg);
        check_u64("6b a normal open reads ZERO segment payloads",
                  0u, b->store.segments_read);
        check_u64("6b a normal open verifies no payload CRC or digest",
                  0u, b->store.segments_verified);
        /* everything that touches rows still works (the ensure_loaded path) */
        check(sfb_count(b, &live) == SFB_OK && live == D6_N,
              "6b the first row access materialises the store",
              "live=%llu (want %u)", (unsigned long long)live, D6_N);
        check_u64("6b exactly one replay", nseg, b->store.segments_read);
        check(sfb_decay_tick(b, 1u) == SFB_OK,
              "6b decay_tick works on a lazily opened brain", NULL);
        (void)sfb_close(b);
        b = NULL;
    }

    /* verify_full: every segment read AND verified */
    sfb_config_default(&c, dir, dim);
    c.verify_full = 1;
    rc = sfb_open(&c, &b);
    check(rc == SFB_OK && b != NULL, "6c a verify_full open succeeds", "rc=%s",
          err_name(rc));
    if (b != NULL) {
        uint32_t nseg = b->store.man.nseg;

        check_u64("6c verify_full reads every payload", (uint64_t)nseg,
                  b->store.segments_read);
        check_u64("6c verify_full increments segments_verified for every segment",
                  (uint64_t)nseg, b->store.segments_verified);
        check(sfb_count(b, &live) == SFB_OK && live == D6_N,
              "6c rows are in RAM on return", "live=%llu",
              (unsigned long long)live);
        (void)sfb_close(b);
        b = NULL;
    }
}

/* ═ main ════════════════════════════════════════════════════════════════ */
int main(void)
{
    int rc;

    snprintf(g_base, sizeof g_base, "/tmp/sfb_w5_%ld", (long)getpid());
    (void)rm_rf(g_base);
    rc = sfb_mkdir_p(g_base, 0755);
    if (rc != SFB_OK) {
        fprintf(stderr, "cannot create %s\n", g_base);
        return 2;
    }

    printf("sfb W5 semantics suite (base=%s)\n", g_base);
    test_w51_dedup_units();
    test_w53_policy_persistence();
    test_w54_pin_floor();
    test_w55_consolidate();
    test_w46_idf();
    test_w46_fusion();
    test_w33_lazy();

    (void)rm_rf(g_base);
    printf("\n== summary ==\nchecks: %d   passed: %d   failed: %d\n",
           g_checks, g_pass, g_fail);
    if (g_fail == 0) {
        printf("ALL W5 SEMANTICS TESTS PASSED\n");
        return 0;
    }
    printf("W5 SEMANTICS TESTS FAILED\n");
    return 1;
}
