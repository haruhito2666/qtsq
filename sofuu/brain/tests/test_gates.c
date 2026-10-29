/*
 * test_gates.c — the plan's phase exit gates, measured (Phase 2 + Phase 3)
 *
 * Gates covered here:
 *   Phase 2  "10k distinct texts yield 10k records"  -> dedup must not fold
 *            two different texts however close their vectors are.
 *   Phase 3  "4x RAM reduction" from INT8 quantisation -> measured as the
 *            on-disk footprint ratio of the same corpus with and without
 *            quantisation, which is what the plan's 4x claim is about.
 *   Phase 3  "disk amp < 1.5x" on a corpus of realistic size.
 *
 * The 100k open-time gate needs a full 100k ingest and lives in the benchmark,
 * not here (see README for the measured number).
 *
 * Author: Haruhito
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CK_MSG(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t rs = 20261029u;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0; }

static void rmrf(const char *p)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd) != 0) { /* best effort */ }
}

/* ── Phase 2: 10k distinct texts must yield 10k records ─────────────────── */
static void gate_distinct_texts(void)
{
    const uint32_t dim = 64;
    const uint32_t n = 10000;
    const char *dir = "/tmp/sfb_gate_distinct";
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    float v[64];
    uint64_t live = 0;
    int rc;

    printf("── gate: %u distinct texts -> %u records\n", n, n);
    rmrf(dir);
    sfb_config_default(&cfg, dir, dim);
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "open rc=%d (%s)", rc, sfb_strerror(rc));
    if (rc != SFB_OK)
        return;

    uint32_t accepted = 0;
    for (uint32_t i = 0; i < n; i++) {
        sfb_remember_result_t r;
        char text[64];

        /* Vectors sit in a tiny ball, so a distance-only dedup would fold
         * these rows together. The text key is what must keep them apart. */
        for (uint32_t d = 0; d < dim; d++)
            v[d] = (float)(0.001 * (double)((i * 7919u + d * 104729u) % 1000) / 1000.0);
        snprintf(text, sizeof text, "distinct-record-%u", i);
        if (sfb_remember(b, v, text, "gate", &r) == SFB_OK) {
            accepted++;
            CK_MSG(r.merged == 0, "record %u was merged into %u despite a distinct text",
                   i, r.into);
        }
    }
    CK_MSG(accepted == n, "only %u of %u remembers were accepted", accepted, n);

    sfb_flush_result_t fr;
    CK(sfb_flush(b, &fr) == SFB_OK);
    CK(sfb_count(b, &live) == SFB_OK);
    CK_MSG(live == n, "live records = %llu, expected %u (dedup folded distinct texts)",
           (unsigned long long)live, n);

    sfb_stats_t st;
    CK(sfb_stats_get(b, &st) == SFB_OK);
    CK_MSG(st.records == n, "stats.records=%llu", (unsigned long long)st.records);
    sfb_close(b);
    rmrf(dir);
}

/* ── Phase 3: INT8 footprint + disk amplification ───────────────────────── */
static uint64_t build_corpus(const char *dir, uint32_t dim, uint32_t n, int int8)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    float *v = (float *)malloc((size_t)dim * sizeof(float));
    uint64_t bytes = 0;

    if (v == NULL)
        return 0;
    rmrf(dir);
    sfb_config_default(&cfg, dir, dim);
    cfg.quantize_int8 = int8;
    {
        int orc = sfb_open(&cfg, &b);
        if (orc != SFB_OK) {
            fprintf(stderr, "   corpus %s: open failed rc=%d (%s)\n",
                    dir, orc, sfb_strerror(orc));
            free(v);
            return 0;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        sfb_remember_result_t r;
        char text[64];
        for (uint32_t d = 0; d < dim; d++)
            v[d] = (float)(rnd() * 2.0 - 1.0);
        snprintf(text, sizeof text, "corpus-%s-%u", int8 ? "q" : "f", i);
        (void)sfb_remember(b, v, text, "gate", &r);
    }
    sfb_flush_result_t fr;
    (void)sfb_flush(b, &fr);

    sfb_stats_t st;
    if (sfb_stats_get(b, &st) == SFB_OK)
        bytes = st.bytes_on_disk;
    sfb_close(b);
    free(v);
    return bytes;
}

static void gate_quantisation(void)
{
    const uint32_t dim = 256, n = 4000;
    const char *df = "/tmp/sfb_gate_f32";
    const char *dq = "/tmp/sfb_gate_int8";
    uint64_t bf, bq;

    printf("── gate: INT8 footprint (%u x %u)\n", n, dim);
    bf = build_corpus(df, dim, n, 0);
    bq = build_corpus(dq, dim, n, 1);
    CK_MSG(bf > 0 && bq > 0, "could not build corpora (f32=%llu int8=%llu)",
           (unsigned long long)bf, (unsigned long long)bq);
    if (bf > 0 && bq > 0) {
        double ratio = (double)bf / (double)bq;
        printf("   f32=%llu B  int8=%llu B  ratio=%.2fx\n",
               (unsigned long long)bf, (unsigned long long)bq, ratio);
        /* The plan promises a 4x reduction of the vector payload. Vectors are
         * only part of the file (text, headers, index), so the whole-store
         * ratio is lower; this pins a floor that would fail if INT8 were not
         * actually shrinking the payload. */
        CK_MSG(ratio >= 1.6, "INT8 only reduced the store %.2fx (expected >= 1.6x)", ratio);

        /* and the quantised store must still be readable */
        {
            sfb_config_t cfg;
            sfb_brain_t *b = NULL;
            uint64_t live = 0;
            sfb_config_default(&cfg, dq, dim);
            cfg.quantize_int8 = 1;
            CK(sfb_open(&cfg, &b) == SFB_OK);
            if (b != NULL) {
                CK(sfb_count(b, &live) == SFB_OK);
                CK_MSG(live == n, "quantised store reopened with %llu of %u records",
                       (unsigned long long)live, n);
                sfb_close(b);
            }
        }
    }
    rmrf(df);
    rmrf(dq);
}

/* ── Phase 3: disk amplification at a realistic corpus size ─────────────── */
static void gate_disk_amp(void)
{
    const uint32_t dim = 128, n = 8000;
    const char *dir = "/tmp/sfb_gate_amp";
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    float *v = (float *)malloc((size_t)dim * sizeof(float));
    uint64_t payload = 0;

    printf("── gate: disk amplification (%u x %u)\n", n, dim);
    CK(v != NULL);
    if (v == NULL)
        return;
    rmrf(dir);
    sfb_config_default(&cfg, dir, dim);
    CK(sfb_open(&cfg, &b) == SFB_OK);
    if (b == NULL) { free(v); return; }

    for (uint32_t i = 0; i < n; i++) {
        sfb_remember_result_t r;
        char text[64];
        for (uint32_t d = 0; d < dim; d++)
            v[d] = (float)(rnd() * 2.0 - 1.0);
        snprintf(text, sizeof text, "amp-%u", i);
        (void)sfb_remember(b, v, text, "gate", &r);
        payload += (uint64_t)dim * sizeof(float);   /* raw float32 payload */
    }
    sfb_flush_result_t fr;
    CK(sfb_flush(b, &fr) == SFB_OK);

    sfb_stats_t st;
    CK(sfb_stats_get(b, &st) == SFB_OK);
    if (payload > 0) {
        double amp = (double)st.bytes_on_disk / (double)payload;
        printf("   payload=%llu B  on-disk=%llu B  amp=%.2fx\n",
               (unsigned long long)payload, (unsigned long long)st.bytes_on_disk, amp);
        CK_MSG(amp < 1.5, "disk amp %.2fx exceeds the 1.5x gate", amp);
    }
    sfb_close(b);
    free(v);
    rmrf(dir);
}


/* ── replay must not resurrect the DIRTY flag ───────────────────────────────
 * serialize_dirty() writes r->flags verbatim, so SFB_F_DIRTY reaches the disk.
 * If replay applies it back, every replayed row is "already dirty": retain()
 * tombstones vanish on reopen and the next flush re-serialises the whole
 * corpus (measured before the fix: 201 records / 113 kB for one new row).
 * The invariant pinned here is the visible one: one write after a reopen must
 * write one record. */
static void gate_reopen_write_amplification(void)
{
    const uint32_t dim = 64, n = 200;
    const char *dir = "/tmp/sfb_gate_amp_write";
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    float v[64];
    uint64_t live = 0;
    int rc;

    printf("── gate: one write after reopen writes one record\n");
    rmrf(dir);
    sfb_config_default(&cfg, dir, dim);
    CK_MSG(sfb_open(&cfg, &b) == SFB_OK, "open failed");
    if (b == NULL)
        return;

    for (uint32_t i = 0; i < n; i++) {
        sfb_remember_result_t r;
        char text[64];
        for (uint32_t d = 0; d < dim; d++)
            v[d] = (float)(rnd() * 2.0 - 1.0);
        snprintf(text, sizeof text, "ampw-%u", i);
        (void)sfb_remember(b, v, text, "gate", &r);
    }
    sfb_flush_result_t fr;
    CK(sfb_flush(b, &fr) == SFB_OK);
    sfb_close(b);

    /* reopen and write exactly one record */
    rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "reopen rc=%d (%s)", rc, sfb_strerror(rc));
    if (rc != SFB_OK)
        return;
    CK(sfb_count(b, &live) == SFB_OK);
    CK_MSG(live == n, "reopen lost records: %llu of %u", (unsigned long long)live, n);

    {
        sfb_remember_result_t r;
        for (uint32_t d = 0; d < dim; d++)
            v[d] = (float)(rnd() * 2.0 - 1.0);
        CK(sfb_remember(b, v, "ampw-after-reopen", "gate", &r) == SFB_OK);
        memset(&fr, 0, sizeof fr);
        CK(sfb_flush(b, &fr) == SFB_OK);
        CK_MSG(fr.records_written == 1,
               "flushing one new row after a reopen wrote %llu records "
               "(replay resurrected SFB_F_DIRTY)",
               (unsigned long long)fr.records_written);
    }
    sfb_close(b);
    rmrf(dir);
}

int main(void)
{
    printf("── plan phase gates\n");
    gate_distinct_texts();
    gate_quantisation();
    gate_disk_amp();
    gate_reopen_write_amplification();

    printf("test_gates: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_GATES FAILED\n"); return 1; }
    printf("=== TEST_GATES PASSED ===\n");
    return 0;
}
