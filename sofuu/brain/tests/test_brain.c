/*
 * tests/test_brain.c — Sofuu Brain v2: black-box end-to-end acceptance test.
 *
 * Uses ONLY the frozen public API from sfb.h, plus sfb_broker.h for W2.4.
 * sfb_internal.h is NOT included: the internal constants this test needs (pin
 * role, pin floor) are mirrored below with their source named.
 *
 * One [ok] / [FAIL] line per check; [SKIP] only where the task allows it (the
 * broker could not start), [LIMIT] for a contract the frozen ABI cannot express.
 * Ends with a count summary and either "ALL BRAIN TESTS PASSED" (exit 0) or
 * exit 1. Temp state lives in /tmp/sfb_bt_<pid>/ and is removed on exit.
 *
 * Author: Haruhito
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "sfb.h"
#include "sfb_broker.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* constants mirrored from sfb_internal.h (reference only, not included) */
#define PIN_ROLE  "user_pin"   /* SFB_PIN_ROLE  */
#define PIN_FLOOR 0.2f         /* SFB_PIN_FLOOR */

/* ── harness ────────────────────────────────────────────────────────────── */
static int g_checks, g_pass, g_fail, g_skip;
static char g_base[256], g_path[512], g_nm[192];
static float g_v[4096], g_v2[4096], g_v3[4096];   /* scratch (max dim = 4096) */

#define NM(...) (snprintf(g_nm, sizeof g_nm, __VA_ARGS__), g_nm)
#define DIMV(d) ((float *)xmalloc((size_t)(d) * sizeof(float)))

static void subsection(const char *t) { printf("\n== %s ==\n", t); fflush(stdout); }

static void ck(int cond, const char *name, const char *fmt, ...)
{
    va_list ap;
    g_checks++;
    if (cond) { g_pass++; printf("[ok] %s\n", name); fflush(stdout); return; }
    g_fail++;
    printf("[FAIL] %s", name);
    if (fmt != NULL && fmt[0] != '\0') { printf("  |  "); va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
    printf("\n");
    fflush(stdout);
}

static void note(const char *fmt, ...)
{
    va_list ap;
    printf("        note: ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void ck_skip(const char *name, const char *why)
{
    g_skip++;
    printf("[SKIP] %s  |  %s\n", name, why);
    fflush(stdout);
}

static const char *err_name(int rc)
{
    switch (rc) {
    case SFB_OK: return "SFB_OK"; case SFB_E_INVALID: return "SFB_E_INVALID";
    case SFB_E_IO: return "SFB_E_IO"; case SFB_E_NOMEM: return "SFB_E_NOMEM";
    case SFB_E_NOTFOUND: return "SFB_E_NOTFOUND"; case SFB_E_STATE: return "SFB_E_STATE";
    case SFB_E_DIM: return "SFB_E_DIM"; case SFB_E_TYPE: return "SFB_E_TYPE";
    case SFB_E_NONFINITE: return "SFB_E_NONFINITE"; case SFB_E_STALE: return "SFB_E_STALE";
    case SFB_E_LOCKED: return "SFB_E_LOCKED"; case SFB_E_CORRUPT: return "SFB_E_CORRUPT";
    case SFB_E_ENOSPC: return "SFB_E_ENOSPC"; case SFB_E_READONLY: return "SFB_E_READONLY";
    case SFB_E_CAPACITY: return "SFB_E_CAPACITY"; default: return "?";
    }
}

static void ck_code(const char *name, int want, int got)
{
    char buf[200];
    if (want == got) { ck(1, name, NULL); return; }
    snprintf(buf, sizeof buf, "expected %s (%d), actual %s (%d)", err_name(want), want, err_name(got), got);
    ck(0, name, "%s", buf);
}

static void ck_u64(const char *name, uint64_t want, uint64_t got)
{
    char buf[200];
    if (want == got) { ck(1, name, NULL); return; }
    snprintf(buf, sizeof buf, "expected %llu, actual %llu", (unsigned long long)want, (unsigned long long)got);
    ck(0, name, "%s", buf);
}

/* ── deterministic PRNG: xorshift64* + Box–Muller (never rand()) ────────── */
static uint64_t g_rng = 0x243F6A8885A308D3ULL;

static uint64_t xr(void)
{
    uint64_t x = g_rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (g_rng = x);
}

static double urand(void) { return (double)(xr() >> 11) * (1.0 / 9007199254740992.0); }

static double ngauss(void)
{
    double u1 = urand(), u2 = urand();
    if (!(u1 > 1e-12)) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static void fill_unit(float *v, uint32_t dim)          /* random unit vector */
{
    double s = 0.0;
    uint32_t i;
    for (i = 0; i < dim; i++) v[i] = (float)ngauss(), s += (double)v[i] * v[i];
    for (s = sqrt(s) > 0.0 ? sqrt(s) : 1.0, i = 0; i < dim; i++) v[i] = (float)((double)v[i] / s);
}

static void fill_near(float *v, const float *c, uint32_t dim, double sigma)
{
    uint32_t i;
    for (i = 0; i < dim; i++) v[i] = (float)((double)c[i] + sigma * ngauss());
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n != 0 ? n : 1u);
    if (p == NULL) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

/* cfg->path points into g_path, which outlives every cfg used below */
static void mkcfg(sfb_config_t *cfg, const char *sub, uint32_t dim)
{
    int n = snprintf(g_path, sizeof g_path, "%s/%s", g_base, sub);
    if (n < 0 || (size_t)n >= sizeof g_path) { fprintf(stderr, "path too long\n"); exit(2); }
    sfb_config_default(cfg, g_path, dim);
}

static sfb_brain_t *bopen(const sfb_config_t *cfg, const char *what)
{
    sfb_brain_t *b = NULL;
    int rc = sfb_open(cfg, &b);
    ck(rc == SFB_OK && b != NULL, what, "sfb_open -> %s (%d)", err_name(rc), rc);
    return b;
}

static uint64_t bcount(sfb_brain_t *b) { uint64_t n = 0; (void)sfb_count(b, &n); return n; }

static int brec(sfb_brain_t *b, const float *v, const char *text, const char *role, uint32_t *id)
{
    sfb_remember_result_t rr;
    int rc;
    memset(&rr, 0, sizeof rr);
    rc = sfb_remember(b, v, text, role, &rr);
    if (id != NULL) *id = rr.id;
    return rc;
}

/* n random unit rows named "<prefix> <i>"; row 0's vector/id are returned */
static int seed_rows(sfb_brain_t *b, uint32_t dim, uint32_t n, const char *prefix,
                     const char *role, float *first, uint32_t *first_id)
{
    char text[160];
    uint32_t i, id = 0, id0 = 0;
    int rc = SFB_OK;
    for (i = 0; i < n; i++) {
        fill_unit(g_v, dim);
        snprintf(text, sizeof text, "%s %u", prefix, i);
        if ((rc = brec(b, g_v, text, role, &id)) != SFB_OK) break;
        if (i == 0) { id0 = id; if (first != NULL) memcpy(first, g_v, (size_t)dim * sizeof(float)); }
    }
    if (first_id != NULL) *first_id = id0;
    return rc;
}

static int rm_rf(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char child[4096];
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if ((d = opendir(path)) == NULL) return -1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        (void)rm_rf(child);
    }
    closedir(d);
    return rmdir(path);
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 0 = reaped, 1 = timed out, -1 = error */
static int wait_child(pid_t pid, int *status, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 0;
        if (r < 0) return -1;
        if (waited >= timeout_ms) return 1;
        sleep_ms(10);
        waited += 10;
    }
}

/* ═ 1. arbitrary dimensions (W5.6) ═══════════════════════════════════════ */
static void test_dims(void)
{
    static const uint32_t dims[] = { 64u, 384u, 768u, 1536u, 4096u }, bad[] = { 32u, 8192u, 0u };
    size_t i;

    subsection("1. arbitrary dimensions (W5.6)");
    for (i = 0; i < sizeof dims / sizeof dims[0]; i++) {
        uint32_t dim = dims[i], nh = 0;
        char sub[32];
        sfb_config_t cfg;
        sfb_brain_t *b;
        sfb_hit_t hits[8];
        int rc;
        snprintf(sub, sizeof sub, "dim%u", dim);
        mkcfg(&cfg, sub, dim);
        b = bopen(&cfg, NM("1a dim=%u open fresh brain", dim));
        rc = b ? seed_rows(b, dim, 50u, "dim record", NULL, NULL, NULL) : SFB_E_STATE;
        ck(rc == SFB_OK, NM("1a dim=%u remember 50 records", dim), "sfb_remember -> %s (%d)", err_name(rc), rc);
        ck_u64(NM("1b dim=%u count == 50", dim), 50u, bcount(b));
        fill_unit(g_v2, dim);
        rc = sfb_recall(b, g_v2, 5u, hits, &nh);
        ck(rc == SFB_OK && nh >= 1u, NM("1c dim=%u recall returns hits", dim), "rc=%s (%d), hits=%u (want >= 1)",
           err_name(rc), rc, nh);
        ck_code(NM("1d dim=%u close", dim), SFB_OK, sfb_close(b));
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char sub[32];
        sfb_config_t cfg;
        sfb_brain_t *b = NULL;
        int rc;
        snprintf(sub, sizeof sub, "dim_bad%u", bad[i]);
        mkcfg(&cfg, sub, bad[i]);
        rc = sfb_open(&cfg, &b);
        if (rc == SFB_E_DIM && b == NULL) { ck(1, NM("1e dim=%u open rejected with SFB_E_DIM", bad[i]), NULL); continue; }
        ck(0, NM("1e dim=%u open rejected with SFB_E_DIM", bad[i]),
           "expected %s (-1005), actual %s (%d), brain=%s", err_name(SFB_E_DIM), err_name(rc), rc,
           b ? "non-NULL" : "NULL");
        if (b != NULL) (void)sfb_close(b);
    }
}

/* ═ 2. basic life cycle: 1000 rows, flush, close, reopen ═════════════════ */
static void test_lifecycle(void)
{
    const uint32_t dim = 768u;
    char text[96], want[96];
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_flush_result_t fr;
    sfb_hit_t hits[8];
    uint32_t nh = 0;
    int rc = SFB_OK, i;
    float *v = g_v, *saved = g_v2;

    subsection("2. basic life cycle (1000 records + persistence)");
    mkcfg(&cfg, "life", dim);
    b = bopen(&cfg, "2a open 768 cognitive");
    for (i = 0; i < 1000 && b != NULL; i++) {
        fill_unit(v, dim);
        snprintf(text, sizeof text, "record %d %08x-%08x", i, (uint32_t)(xr() >> 32), (uint32_t)(xr() >> 32));
        if ((rc = brec(b, v, text, NULL, NULL)) != SFB_OK) break;
        if (i == 777) { memcpy(saved, v, (size_t)dim * sizeof(float)); snprintf(want, sizeof want, "%s", text); }
    }
    ck(rc == SFB_OK && i == 1000, "2b 1000 distinct vectors + distinct texts remembered",
       "sfb_remember -> %s (%d) after %d records", err_name(rc), rc, i);
    ck_u64("2c count == 1000", 1000u, bcount(b));
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    ck(rc == SFB_OK && fr.ok == 1, "2d flush ok", "rc=%s (%d), ok=%d, written=%llu", err_name(rc), rc, fr.ok,
       (unsigned long long)fr.records_written);
    ck_code("2e close", SFB_OK, sfb_close(b));
    b = bopen(&cfg, "2f reopen");
    ck_u64("2g count == 1000 after reopen (persistence)", 1000u, bcount(b));
    rc = sfb_recall(b, saved, 1u, hits, &nh);
    ck(rc == SFB_OK && nh == 1u && hits[0].id == 777u && hits[0].text && !strcmp(hits[0].text, want),
       "2h row 777 survives reopen (id, vector and text)", "rc=%s (%d), hits=%u, id=%u (want 777), text=\"%s\" (want \"%s\")",
       err_name(rc), rc, nh, nh ? hits[0].id : 0u, nh && hits[0].text ? hits[0].text : "", want);
    nh = 0;
    rc = sfb_recall_text(b, "record 777", 3u, hits, &nh);
    ck(rc == SFB_OK && nh >= 1u && hits[0].text && !strcmp(hits[0].text, want), "2i recall_text ranks the exact row first",
       "rc=%s (%d), hits=%u, top=\"%s\" (want \"%s\")", err_name(rc), rc, nh, nh && hits[0].text ? hits[0].text : "", want);
    ck_code("2j close", SFB_OK, sfb_close(b));
}

/* ═ 3. recall quality on clustered data ══════════════════════════════════ */
#define NCLUST 10
#define PER_CL 50
#define NQUERY 20

static void test_recall_quality(void)
{
    const uint32_t dim = 768u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    float *centres = DIMV(NCLUST * dim);
    sfb_hit_t hits[16];
    int rc = SFB_OK, c, m, qi, top1 = 0;
    double sum = 0.0;

    subsection("3. recall quality (10 clusters, sigma 0.02)");
    mkcfg(&cfg, "cluster", dim);
    cfg.mode = SFB_MODE_VECTOR_DB;
    cfg.decay = SFB_DECAY_NONE;
    b = bopen(&cfg, "3a open 768 vector_db");
    for (c = 0; c < NCLUST; c++) fill_unit(centres + (size_t)c * dim, dim);
    for (c = 0; c < NCLUST && rc == SFB_OK; c++) {
        for (m = 0; m < PER_CL; m++) {
            char text[64];
            fill_near(g_v, centres + (size_t)c * dim, dim, 0.02);
            snprintf(text, sizeof text, "cluster%d-%d", c, m);
            if ((rc = brec(b, g_v, text, NULL, NULL)) != SFB_OK) break;
        }
    }
    ck(rc == SFB_OK, "3b 500 clustered vectors remembered", "rc=%s (%d)", err_name(rc), rc);
    ck_u64("3c count == 500", 500u, bcount(b));
    for (qi = 0; qi < NQUERY; qi++) {
        uint32_t k = (uint32_t)(qi % NCLUST), nh = 0, i, in = 0;
        char pfx[32];
        int plen = snprintf(pfx, sizeof pfx, "cluster%u-", k);
        fill_near(g_v2, centres + (size_t)k * dim, dim, 0.02);
        if (sfb_recall(b, g_v2, 10u, hits, &nh) != SFB_OK) continue;
        if (nh > 0 && hits[0].text && !strncmp(hits[0].text, pfx, (size_t)plen)) top1++;
        for (i = 0; i < nh; i++) if (hits[i].text && !strncmp(hits[i].text, pfx, (size_t)plen)) in++;
        sum += (double)in / (double)(nh ? nh : 1u);
    }
    ck(top1 == NQUERY, "3d top-1 hit is from the queried cluster (20/20)", "%d/%d queries matched", top1, NQUERY);
    ck(sum / NQUERY >= 0.90, "3e recall@10 >= 9/10 (mean over 20 queries)",
       "mean in-cluster fraction of top-10 = %.3f (want >= 0.900)", sum / NQUERY);
    (void)sfb_close(b);
    free(centres);
}

/* ═ 4. dedup (W5.1 / W5.2) ═══════════════════════════════════════════════ */
static void test_dedup(void)
{
    const uint32_t dim = 768u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_remember_result_t r1, r2, r3, r4;
    uint64_t live;
    int rc;

    subsection("4. dedup (W5.1 / W5.2)");
    memset(&r1, 0, sizeof r1); memset(&r2, 0, sizeof r2);
    memset(&r3, 0, sizeof r3); memset(&r4, 0, sizeof r4);
    mkcfg(&cfg, "dedup", dim);
    b = bopen(&cfg, "4a open");
    fill_unit(g_v, dim);
    rc = sfb_remember(b, g_v, "the quick brown fox", NULL, &r1);
    ck(rc == SFB_OK && r1.merged == 0, "4b first insert is not a merge", "rc=%s (%d), merged=%d", err_name(rc), rc,
       r1.merged);
    ck_u64("4c count == 1", 1u, bcount(b));
    rc = sfb_remember(b, g_v, "the quick brown fox", NULL, &r2);
    ck(rc == SFB_OK && r2.merged == 1 && r2.into == r1.id && r2.id == r1.id,
       "4d same text + same vector merges into the first id",
       "rc=%s (%d), merged=%d (want 1), into=%u (want %u), id=%u (want %u)", err_name(rc), rc, r2.merged,
       r2.into, r1.id, r2.id, r1.id);
    ck_u64("4e count unchanged after the merge", 1u, bcount(b));
    rc = sfb_remember(b, g_v, "completely different words entirely", NULL, &r3);
    live = bcount(b);
    ck(rc == SFB_OK && r3.merged == 0 && live == 2u, "4f same vector + different text is NOT merged (Jaccard < 0.85)",
       "rc=%s (%d), merged=%d (want 0), count=%llu (want 2)", err_name(rc), rc, r3.merged, (unsigned long long)live);
    memcpy(g_v2, g_v, (size_t)dim * sizeof(float));
    g_v2[0] += 10.0f;
    rc = sfb_remember(b, g_v2, "the quick brown fox", NULL, &r4);
    live = bcount(b);
    ck(rc == SFB_OK && r4.merged == 0 && live == 3u, "4g same text + vector 10 units away is NOT merged",
       "rc=%s (%d), merged=%d (want 0), count=%llu (want 3)", err_name(rc), rc, r4.merged, (unsigned long long)live);
    (void)sfb_close(b);
}

/* ═ 5. validation (W1.1 / W1.3 / W6.5) ══════════════════════════════════ */
static void test_validation(void)
{
    const uint32_t dim = 768u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    float *shortv;
    sfb_hit_t hits[8];
    sfb_flush_result_t fr;
    uint32_t nh = 0;
    uint64_t before;
    int rc, rcf;
    float *v = g_v;

    subsection("5. validation (W1.1 / W1.3 / W6.5)");
    mkcfg(&cfg, "valid", dim);
    b = bopen(&cfg, "5a open");
    fill_unit(v, dim);
    before = bcount(b);
    v[100] = (float)NAN;
    ck_code("5b NaN component -> SFB_E_NONFINITE", SFB_E_NONFINITE, brec(b, v, "nan record", NULL, NULL));
    v[100] = (float)INFINITY;
    ck_code("5c +Inf component -> SFB_E_NONFINITE", SFB_E_NONFINITE, brec(b, v, "inf record", NULL, NULL));
    v[100] = 0.0f;
    ck_u64("5d rejected rows did not enter the brain", before, bcount(b));

    /* Wrong length. sfb_remember() takes no length argument, so a short buffer
     * is a caller contract violation that the API *cannot* detect: it reads
     * exactly `dim` floats. This is a documented limitation, not a library
     * failure, so it is reported as LIMIT and does not fail the suite. The
     * dimension contract is instead enforced at open (SFB_E_DIM), which case 1
     * covers for 32/8192/0. */
    shortv = DIMV(dim - 1u);
    memcpy(shortv, v, (size_t)(dim - 1u) * sizeof(float));
    rc = brec(b, shortv, "short vector", NULL, NULL);
    free(shortv);
    (void)rc;
    printf("[LIMIT] 5e short vector is undetectable: sfb_remember() has no length "
           "parameter (caller must pass exactly dim=%u floats)\n", dim);
    note("dim-1 buffer: reads one float past the caller's buffer by contract.");
    ck_code("5f NULL vector -> SFB_E_INVALID", SFB_E_INVALID, brec(b, NULL, "null vec", NULL, NULL));
    fill_unit(v, dim);
    rc = brec(b, v, NULL, NULL, NULL);
    if (rc == SFB_E_INVALID) {
        ck(1, "5g NULL text -> SFB_E_INVALID", NULL);
    } else {
        ck(0, "5g NULL text -> SFB_E_INVALID", "expected %s (-1), actual %s (%d)", err_name(SFB_E_INVALID), err_name(rc), rc);
        note("sfb_brain.c substituted \"\" for a NULL text before the 16:49 fix; the task spec (W1.1/W6.5) requires SFB_E_INVALID.");
    }
    fill_unit(v, dim);
    v[200] = (float)NAN;
    nh = 0;
    rc = sfb_recall(b, v, 5u, hits, &nh);
    if (rc == SFB_E_NONFINITE || (rc == SFB_OK && nh == 0u)) {
        ck(1, "5h NaN query survives (SFB_E_NONFINITE or 0 hits)", NULL);
    } else {
        ck(0, "5h NaN query survives (SFB_E_NONFINITE or 0 hits)",
           "expected %s (-1007) or 0 hits, actual rc=%s (%d), hits=%u, top dist=%.4f score=%.4f",
           err_name(SFB_E_NONFINITE), err_name(rc), rc, nh, nh ? (double)hits[0].distance : 0.0,
           nh ? (double)hits[0].score : 0.0);
        note("sfb_recall() did not validate the query vector before the 16:49 fix: a NaN query returned hits whose "
             "distance was still the FLT_MAX sentinel and whose score was 0.");
    }
    fill_unit(v, dim);
    rc = brec(b, v, "post-nan record", NULL, NULL);
    rcf = sfb_flush(b, &fr);
    ck(rc == SFB_OK && rcf == SFB_OK, "5i brain still usable after a NaN query (remember + flush)",
       "remember rc=%s (%d), flush rc=%s (%d)", err_name(rc), rc, err_name(rcf), rcf);
    ck_code("5j close", SFB_OK, sfb_close(b));
}

/* ═ 6. decay + tiers (W5.3) ══════════════════════════════════════════════ */
static void decay_hard(sfb_brain_t *b)
{
    int i;
    for (i = 0; i < 5; i++) {
        (void)sfb_decay_tick(b, 86400u * 400u);
        if (bcount(b) == 0u) break;
    }
}

static void test_decay_tiers(void)
{
    const uint32_t dim = 256u, n = 50u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    float *probe = g_v2, *fresh = DIMV(5u * dim);
    sfb_hit_t hits[16];
    uint32_t nh = 0, i, listed;
    uint64_t live;
    int rc = SFB_OK, ok = 1;

    subsection("6. decay + tiers (W5.3)");

    /* 6.1 cognitive: something that decayed must not come back from recall */
    mkcfg(&cfg, "decay_cog", dim);
    b = bopen(&cfg, "6a open cognitive brain");
    rc = b ? seed_rows(b, dim, n, "decay record", NULL, probe, NULL) : SFB_E_STATE;
    ck(rc == SFB_OK, "6b 50 records remembered", "rc=%s (%d)", err_name(rc), rc);
    ck_u64("6c 50 records live before decay", n, bcount(b));
    decay_hard(b);
    ck_u64("6d decayed rows left the live set (FORGOTTEN or dropped)", 0u, bcount(b));
    nh = 0;
    rc = sfb_recall(b, probe, 10u, hits, &nh);
    ck(rc == SFB_OK && nh == 0u, "6e decayed rows are not returned by recall", "rc=%s (%d), hits=%u (want 0)",
       err_name(rc), rc, nh);
    (void)sfb_list(b, 0u, 64u, hits, &nh);
    ck(nh == 0u, "6f nothing is listed that cannot be recalled", "sfb_list returned %u rows (want 0)", nh);

    /* 6.2 fresh rows in the same brain must be listed AND recallable */
    for (i = 0; i < 5u; i++) {
        char text[64];
        fill_unit(fresh + (size_t)i * dim, dim);
        snprintf(text, sizeof text, "fresh record %u", i);
        if (brec(b, fresh + (size_t)i * dim, text, NULL, NULL) != SFB_OK) ok = 0;
    }
    nh = 0;
    (void)sfb_list(b, 0u, 64u, hits, &nh);
    listed = nh;
    ck(ok && listed == 5u, "6g only the 5 fresh rows are listed", "listed=%u (want 5)", listed);
    for (i = 0; i < 5u && i < listed; i++) {
        sfb_hit_t h1[2];
        uint32_t nh1 = 0;
        if (sfb_recall(b, fresh + (size_t)i * dim, 2u, h1, &nh1) != SFB_OK || nh1 == 0u || h1[0].id != hits[i].id) ok = 0;
    }
    ck(ok, "6h every listed row is recallable (list <=> recall agree)", "a listed id could not be recalled by its own vector");
    (void)sfb_close(b);

    /* 6.3 vector_db with the documented zero-decay setting */
    mkcfg(&cfg, "decay_vdb", dim);
    cfg.mode = SFB_MODE_VECTOR_DB;
    cfg.decay = SFB_DECAY_NONE;
    b = bopen(&cfg, "6i open vector_db brain (decay NONE)");
    (void)seed_rows(b, dim, n, "vdb record", NULL, probe, NULL);
    decay_hard(b);
    live = bcount(b);
    nh = 0;
    rc = sfb_recall(b, probe, 3u, hits, &nh);
    ck(live == n && rc == SFB_OK && nh >= 1u && hits[0].id == 0u, "6j vector_db rows still recall after the same decay ticks",
       "count=%llu (want %u), rc=%s (%d), hits=%u, top id=%u (want 0)", (unsigned long long)live, n, err_name(rc), rc,
       nh, nh ? hits[0].id : 0u);
    (void)sfb_close(b);

    /* 6.4 vector_db built from sfb_config_default() alone: sfb.h documents the
     *     decay default as mode-dependent and vector_db as zero decay. */
    mkcfg(&cfg, "decay_vdb_default", dim);
    cfg.mode = SFB_MODE_VECTOR_DB;
    b = bopen(&cfg, "6k open vector_db brain (untouched default config)");
    (void)seed_rows(b, dim, n, "vdbd record", NULL, probe, NULL);
    decay_hard(b);
    live = bcount(b);
    nh = 0;
    rc = sfb_recall(b, probe, 3u, hits, &nh);
    if (live == n && rc == SFB_OK && nh >= 1u) {
        ck(1, "6l vector_db default config is decay-free", NULL);
    } else {
        ck(0, "6l vector_db default config is decay-free", "expected count %u and >= 1 hit, actual count=%llu, hits=%u", n,
           (unsigned long long)live, nh);
        note("before the 16:49 fix sfb_config_default() set decay=SFB_DECAY_EXPONENTIAL for both modes and sfb_open() "
             "never derived decay from the mode, so a default SFB_MODE_VECTOR_DB brain decayed to FORGOTTEN.");
    }
    (void)sfb_close(b);
    free(fresh);
}

/* ═ 7. pins (W5.4) ═══════════════════════════════════════════════════════ */
static void test_pins(void)
{
    const uint32_t dim = 256u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_hit_t hits[16];
    uint32_t pin0_id = 0, nh = 0, i;
    int rc, rcp, all_pins = 1, strength_ok = 1;

    subsection("7. pins (W5.4)");
    mkcfg(&cfg, "pins", dim);
    cfg.pins_immune = 1;
    b = bopen(&cfg, "7a open");
    rcp = seed_rows(b, dim, 10u, "pin record", PIN_ROLE, g_v2, &pin0_id);
    rc = seed_rows(b, dim, 10u, "normal record", NULL, NULL, NULL);
    ck(rcp == SFB_OK && rc == SFB_OK, "7b 20 rows inserted (10 pinned + 10 normal)", "pin rc=%s (%d), plain rc=%s (%d)",
       err_name(rcp), rcp, err_name(rc), rc);
    decay_hard(b);
    nh = 0;
    rc = sfb_recall(b, g_v2, 10u, hits, &nh);
    ck(rc == SFB_OK && nh >= 1u && hits[0].id == pin0_id && hits[0].strength >= PIN_FLOOR,
       "7c pinned row recallable after hard decay with strength >= 0.2",
       "rc=%s (%d), hits=%u, top id=%u (want %u), strength=%.4f (want >= %.2f)", err_name(rc), rc, nh,
       nh ? hits[0].id : 0u, pin0_id, nh ? (double)hits[0].strength : 0.0, (double)PIN_FLOOR);
    for (i = 0; i < nh; i++) {
        if (!hits[i].role || strcmp(hits[i].role, PIN_ROLE)) all_pins = 0;
        if (hits[i].strength < PIN_FLOOR) strength_ok = 0;
    }
    ck(all_pins && nh > 0u, "7d only pinned rows survive recall after decay", "%u hits returned, not all role=\"%s\"", nh,
       PIN_ROLE);
    ck(strength_ok, "7e every returned pin has strength >= 0.2", "one or more pinned hits came back below the pin floor");
    ck_u64("7f the 10 unpinned rows left the live set", 10u, bcount(b));
    (void)sfb_close(b);
}

/* ═ 8. capacity (W2.5) ═══════════════════════════════════════════════════ */
static void test_capacity(void)
{
    const uint32_t dim = 128u;
    const uint64_t cap = 100u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_hit_t hits[8];
    uint64_t live = 0, peak = 0;
    uint32_t nh = 0, id0 = 0, id299 = 0, j, id;
    int rc = SFB_OK, i, over = 0, err = 0, gone = 1;

    subsection("8. capacity (W2.5)");
    mkcfg(&cfg, "capacity", dim);
    cfg.mode = SFB_MODE_VECTOR_DB;
    cfg.decay = SFB_DECAY_NONE;
    cfg.capacity = cap;
    b = bopen(&cfg, "8a open with capacity=100");
    for (i = 0; i < 300 && b != NULL; i++) {
        char text[64];
        id = 0;
        fill_unit(g_v, dim);
        snprintf(text, sizeof text, "cap record %d", i);
        if ((rc = brec(b, g_v, text, NULL, &id)) != SFB_OK) { err = rc; break; }
        if (i == 0) { id0 = id; memcpy(g_v2, g_v, (size_t)dim * sizeof(float)); }
        if (i == 299) { id299 = id; memcpy(g_v3, g_v, (size_t)dim * sizeof(float)); }
        live = bcount(b);
        if (live > cap) over = 1;
        if (live > peak) peak = live;
    }
    ck(err == 0, "8b 300 inserts accepted without error or crash", "sfb_remember -> %s (%d) at insert %d", err_name(err),
       err, i);
    ck(!over, "8c count never exceeded capacity=100", "peak live count %llu (want <= 100)", (unsigned long long)peak);
    ck_u64("8d final count == 100", cap, bcount(b));
    (void)sfb_recall(b, g_v2, 3u, hits, &nh);
    for (j = 0; j < nh; j++) if (hits[j].id == id0) gone = 0;
    nh = 0;
    (void)sfb_recall(b, g_v3, 1u, hits, &nh);
    ck(gone && nh == 1u && hits[0].id == id299, "8e weakest (oldest) rows evicted, newest retained",
       "oldest id %u still recallable=%d, newest id %u on top=%d (hits=%u)", id0, !gone, id299,
       (nh == 1u && hits[0].id == id299), nh);
    (void)sfb_close(b);
}

/* ═ 9. forget + retain ═══════════════════════════════════════════════════ */
static void test_forget_retain(void)
{
    const uint32_t dim = 128u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_flush_result_t fr;
    uint32_t ids[40];
    int rc, i, ok = 1;

    subsection("9. forget + retain");
    mkcfg(&cfg, "forget", dim);
    b = bopen(&cfg, "9a open");
    for (i = 0; i < 40; i++) {
        char text[64];
        fill_unit(g_v, dim);
        snprintf(text, sizeof text, "forget record %d", i);
        (void)brec(b, g_v, text, NULL, &ids[i]);
    }
    ck_u64("9b 40 rows inserted", 40u, bcount(b));
    for (i = 0; i < 10; i++) if (sfb_forget(b, ids[i]) != SFB_OK) ok = 0;
    ck(ok, "9c forget 10 ids returns SFB_OK", "at least one sfb_forget failed");
    ck_u64("9d count dropped by 10", 30u, bcount(b));
    ck_code("9e retain() physically drops the tombstones", SFB_OK, sfb_retain(b));
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    ck_code("9f flush after retain", SFB_OK, rc);
    ck_code("9g close", SFB_OK, sfb_close(b));
    b = bopen(&cfg, "9h reopen");
    ck_u64("9i count stays reduced after reopen", 30u, bcount(b));
    (void)sfb_close(b);
}

/* ═ 10. list pagination (W6.4) ═══════════════════════════════════════════ */
static void test_list_pagination(void)
{
    const uint32_t dim = 128u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_hit_t hits[16];
    uint64_t live;
    uint32_t nh = 0;
    int rc;

    subsection("10. list pagination (W6.4)");
    mkcfg(&cfg, "listp", dim);
    b = bopen(&cfg, "10a open");
    (void)seed_rows(b, dim, 25u, "list record", NULL, NULL, NULL);
    live = bcount(b);
    rc = sfb_list(b, 0u, 10u, hits, &nh);
    ck(rc == SFB_OK && nh == 10u, "10b list(offset=0, limit=10) returns 10", "rc=%s (%d), n=%u (want 10)", err_name(rc), rc,
       nh);
    nh = 0;
    rc = sfb_list(b, live >= 5u ? live - 5u : 0u, 10u, hits, &nh);
    ck(rc == SFB_OK && nh == 5u, "10c list(offset=n-5, limit=10) returns 5", "n_total=%llu, rc=%s (%d), n=%u (want 5)",
       (unsigned long long)live, err_name(rc), rc, nh);
    (void)sfb_close(b);
    mkcfg(&cfg, "listp_empty", dim);
    b = bopen(&cfg, "10d open empty brain");
    nh = 1234u;
    rc = sfb_list(b, 0u, 10u, hits, &nh);
    ck(rc == SFB_OK && nh == 0u, "10e list on an empty brain returns 0", "rc=%s (%d), n=%u (want 0)", err_name(rc), rc, nh);
    (void)sfb_close(b);
}

/* ═ 11. events (W1.8) ════════════════════════════════════════════════════ */
typedef struct { int count, saw_type; char last[512]; } evstat_t;

static void on_event(const char *type, const char *json, void *ud)
{
    evstat_t *s = (evstat_t *)ud;
    (void)type;
    s->count++;
    if (json != NULL) {
        snprintf(s->last, sizeof s->last, "%s", json);
        if (strstr(json, "\"type\"") != NULL) s->saw_type = 1;
    }
}

static void test_events(void)
{
    const uint32_t dim = 128u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    evstat_t ev;
    sfb_flush_result_t fr;
    int rc;

    subsection("11. events (W1.8)");
    mkcfg(&cfg, "events", dim);
    cfg.capacity = 4u;                        /* forces a capacity eviction */
    b = bopen(&cfg, "11a open with capacity=4");
    memset(&ev, 0, sizeof ev);
    ck_code("11b sfb_on_event registers the callback", SFB_OK, sfb_on_event(b, on_event, &ev));
    (void)seed_rows(b, dim, 10u, "event record", NULL, NULL, NULL);
    rc = sfb_flush(b, &fr);
    ck_code("11c flush with the callback installed", SFB_OK, rc);
    ck(ev.count > 0, "11d callback fired (flush + capacity evictions)", "callback count = %d (want > 0)", ev.count);
    ck(ev.saw_type, "11e at least one event JSON carries a \"type\" key", "last event JSON = %s", ev.last[0] ? ev.last : "(none)");
    (void)sfb_close(b);
}

/* ═ 12. stats ════════════════════════════════════════════════════════════ */
static void test_stats(void)
{
    const uint32_t dim = 768u;
    sfb_config_t cfg;
    sfb_brain_t *b;
    sfb_flush_result_t fr;
    sfb_stats_t st;
    int rc;

    subsection("12. stats");
    mkcfg(&cfg, "stats", dim);
    b = bopen(&cfg, "12a open");
    (void)seed_rows(b, dim, 30u, "stats record", NULL, NULL, NULL);
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    ck_code("12b flush", SFB_OK, rc);
    memset(&st, 0, sizeof st);
    ck_code("12c sfb_stats_get", SFB_OK, sfb_stats_get(b, &st));
    ck(st.records > 0, "12d stats.records > 0", "records=%llu", (unsigned long long)st.records);
    ck(st.segments > 0, "12e stats.segments > 0 after flush", "segments=%llu", (unsigned long long)st.segments);
    ck(st.bytes_on_disk > 0, "12f stats.bytes_on_disk > 0", "bytes_on_disk=%llu", (unsigned long long)st.bytes_on_disk);
    ck(st.open_us > 0, "12g stats.open_us > 0", "open_us=%llu", (unsigned long long)st.open_us);
    (void)sfb_close(b);
}

/* ═ 13. writer lock (W2.1) ═══════════════════════════════════════════════ */
static void test_locked(void)
{
    const uint32_t dim = 128u;
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    int ready[2], go[2], rc, st = 0, waited;
    ssize_t got;
    pid_t pid;
    char byte = 0;

    subsection("13. writer lock (W2.1)");
    mkcfg(&cfg, "locked", dim);
    (void)mkdir(g_path, 0755);
    if (pipe(ready) != 0 || pipe(go) != 0) { ck(0, "13a fork a writer child", "pipe(): %s", strerror(errno)); return; }
    fflush(stdout); fflush(stderr);
    pid = fork();
    if (pid < 0) { ck(0, "13a fork a writer child", "fork(): %s", strerror(errno)); return; }
    if (pid == 0) {                              /* child: hold the writer lock */
        sfb_brain_t *cb = NULL;
        close(ready[0]); close(go[1]);
        if (sfb_open(&cfg, &cb) != SFB_OK) _exit(20);
        if (write(ready[1], "R", 1) != 1) _exit(21);
        close(ready[1]);
        while (read(go[0], &byte, 1) < 0 && errno == EINTR) { }
        close(go[0]);
        (void)sfb_close(cb);
        _exit(0);
    }
    close(ready[1]); close(go[0]);
    got = read(ready[0], &byte, 1);
    ck(got == 1, "13a child holds the brain as writer", "read()=%zd", got);
    close(ready[0]);
    rc = sfb_open(&cfg, &b);
    if (rc == SFB_E_LOCKED && b == NULL) {
        ck(1, "13b parent open while held -> SFB_E_LOCKED", NULL);
    } else {
        ck(0, "13b parent open while held -> SFB_E_LOCKED", "expected %s (-1002), actual %s (%d), brain=%s",
           err_name(SFB_E_LOCKED), err_name(rc), rc, b ? "non-NULL" : "NULL");
        if (b != NULL) (void)sfb_close(b);
    }
    if (write(go[1], "G", 1) != 1) ck(0, "13c child release", "pipe write failed");
    close(go[1]);
    waited = wait_child(pid, &st, 5000);
    ck(waited == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "13c child exited cleanly", "wait=%d, status=%d", waited, st);
    b = NULL;
    rc = sfb_open(&cfg, &b);
    ck_code("13d open succeeds after the child exits", SFB_OK, rc);
    if (b != NULL) (void)sfb_close(b);
}

/* ═ 14. broker (W2.4) ════════════════════════════════════════════════════ */
static volatile sig_atomic_t g_stop;
static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

static void test_broker(void)
{
    const uint32_t dim = 768u;
    char sock[600], why[256];
    sfb_config_t cfg;
    pid_t pid;
    int rc = SFB_OK, i, started = 0, st = 0, waited;
    uint64_t live = 0;

    subsection("14. broker (W2.4)");
    mkcfg(&cfg, "broker", dim);
    snprintf(sock, sizeof sock, "%s/brain.sock", g_path);
    fflush(stdout); fflush(stderr);
    pid = fork();
    if (pid < 0) { ck_skip("14a broker round-trip", "fork() failed"); return; }
    if (pid == 0) {                              /* child: own + serve the brain */
        sfb_brain_t *cb = NULL;
        struct sigaction sa;
        int crc;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigterm;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGTERM, &sa, NULL);
        if (sfb_open(&cfg, &cb) != SFB_OK) _exit(30);
        crc = sfb_broker_run(cb, sock, &g_stop);
        (void)sfb_close(cb);
        _exit(crc == SFB_OK ? 0 : 31);
    }
    for (i = 0; i < 60; i++) {                   /* wait for the socket to answer */
        sfb_remember_result_t rr;
        fill_unit(g_v, dim);
        rc = sfb_broker_remember(sock, g_v, dim, "broker record 0", &rr);
        if (rc == SFB_OK) { started = 1; break; }
        sleep_ms(50);
    }
    if (!started) {
        snprintf(why, sizeof why, "broker never accepted a connection (last rc=%s (%d)); socket path may be unusable",
                 err_name(rc), rc);
        (void)kill(pid, SIGTERM);
        if (wait_child(pid, &st, 3000) != 0) { (void)kill(pid, SIGKILL); (void)wait_child(pid, &st, 3000); }
        ck_skip("14a broker round-trip (20 remembers over the socket)", why);
        return;
    }
    ck(1, "14a broker accepted a connection from the parent", NULL);
    for (i = 1; i < 20; i++) {
        sfb_remember_result_t rr;
        char text[64];
        fill_unit(g_v, dim);
        snprintf(text, sizeof text, "broker record %d", i);
        if ((rc = sfb_broker_remember(sock, g_v, dim, text, &rr)) != SFB_OK) break;
    }
    ck(rc == SFB_OK, "14b 20 remembers written through the socket", "sfb_broker_remember -> %s (%d) at record %d",
       err_name(rc), rc, i);
    rc = sfb_broker_count(sock, &live);
    ck(rc == SFB_OK && live >= 20u, "14c broker count >= 20", "rc=%s (%d), live=%llu (want >= 20)", err_name(rc), rc,
       (unsigned long long)live);
    (void)kill(pid, SIGTERM);
    waited = wait_child(pid, &st, 5000);
    if (waited != 0) { (void)kill(pid, SIGKILL); (void)wait_child(pid, &st, 3000); }
    ck(waited == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "14d broker child stopped and was reaped",
       "wait=%d, status=%d (child: 30=open, 31=broker_run)", waited, st);
}

/* ═ main ═════════════════════════════════════════════════════════════════ */
int main(void)
{
    /* the PRNG seed is fixed in g_rng, so every run exercises identical data */
    snprintf(g_base, sizeof g_base, "/tmp/sfb_bt_%ld", (long)getpid());
    (void)rm_rf(g_base);
    if (mkdir(g_base, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", g_base, strerror(errno));
        return 2;
    }
    printf("Sofuu Brain v2 - black-box end-to-end test\ntemp root: %s\nsfb library: v%d.%d, format %d\n", g_base,
           SFB_VERSION_MAJOR, SFB_VERSION_MINOR, SFB_FORMAT_VERSION);
    test_dims();
    test_lifecycle();
    test_recall_quality();
    test_dedup();
    test_validation();
    test_decay_tiers();
    test_pins();
    test_capacity();
    test_forget_retain();
    test_list_pagination();
    test_events();
    test_stats();
    test_locked();
    test_broker();
    (void)rm_rf(g_base);
    printf("\n== summary ==\nchecks: %d   passed: %d   failed: %d   skipped: %d\n", g_checks, g_pass, g_fail, g_skip);
    if (g_fail == 0) { printf("ALL BRAIN TESTS PASSED\n"); return 0; }
    printf("BRAIN TESTS FAILED: %d check%s did not pass\n", g_fail, g_fail == 1 ? "" : "s");
    return 1;
}
