/*
 * tests/test_text_quant.c — unit tests for sfb_text.c and sfb_quant.c.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/). Self-contained:
 * links only the two modules under test plus libm.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void chk(int cond, const char *what)
{
    if (!cond) fprintf(stderr, "FAIL: %s\n", what);
    assert(cond);
}

static void chk_near(double got, double want, double tol, const char *what)
{
    if (!(fabs(got - want) <= tol)) {
        fprintf(stderr, "FAIL: %s (got %.12g, want %.12g)\n", what, got, want);
    }
    assert(fabs(got - want) <= tol);
}

/* ── deterministic RNG: xorshift64, so runs are reproducible ───────────── */
static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;

static uint64_t xorshift64(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

/* uniform in [-1, 1) */
static float frand_sym(void)
{
    double u = (double)(xorshift64() >> 11) / 9007199254740992.0;
    return (float)(u * 2.0 - 1.0);
}

/* ── text: jaccard ─────────────────────────────────────────────────────── */
static void test_jaccard(void)
{
    chk_near(sfb_text_jaccard("hello world", "hello world"), 1.0, 1e-12,
             "jaccard identical strings == 1.0");
    chk_near(sfb_text_jaccard("a b", "c d"), 0.0, 1e-12,
             "jaccard disjoint sets == 0.0");
    chk_near(sfb_text_jaccard("a b c", "a b"), 2.0 / 3.0, 1e-9,
             "jaccard |A∩B|/|A∪B| == 2/3");

    /* case-insensitive */
    chk_near(sfb_text_jaccard("HELLO World", "hello wORLD"), 1.0, 1e-12,
             "jaccard is case-insensitive");
    chk_near(sfb_text_jaccard("Alpha Beta", "alpha gamma"), 1.0 / 3.0, 1e-9,
             "case-insensitive partial overlap");

    /* punctuation-insensitive */
    chk_near(sfb_text_jaccard("Hello, World!", "hello world"), 1.0, 1e-12,
             "jaccard ignores punctuation");
    chk_near(sfb_text_jaccard("a-b_c/d", "a b c d"), 1.0, 1e-12,
             "jaccard splits on any non-alnum byte");
    chk_near(sfb_text_jaccard("...***", "a"), 0.0, 1e-12,
             "jaccard ignores tokenless input");

    /* sets, not bags */
    chk_near(sfb_text_jaccard("a a a b", "a b"), 1.0, 1e-12,
             "jaccard counts token sets, not occurrences");
    chk_near(sfb_text_jaccard("x x x", "x y y"), 1.0 / 2.0, 1e-9,
             "repeats do not inflate the union");

    /* empty / NULL semantics */
    chk_near(sfb_text_jaccard("", ""), 1.0, 1e-12, "both empty == 1.0");
    chk_near(sfb_text_jaccard(NULL, NULL), 1.0, 1e-12, "both NULL == 1.0");
    chk_near(sfb_text_jaccard("", "abc"), 0.0, 1e-12, "one empty == 0.0");
    chk_near(sfb_text_jaccard(NULL, "abc"), 0.0, 1e-12, "one NULL == 0.0");

    /* truncation: >256 distinct tokens must not crash or return nonsense */
    char big[8192];
    size_t o = 0;
    for (int i = 0; i < 400 && o < sizeof big - 16; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "w%d ", i);
    double r = sfb_text_jaccard(big, big);
    chk(r >= 0.0 && r <= 1.0 && isfinite(r), "jaccard truncates gracefully");

    printf("  jaccard        ok (self=%.3f, 2/3=%.6f, trunc=%.4f)\n",
           1.0, 2.0 / 3.0, r);
}

/* ── text: bm25 ────────────────────────────────────────────────────────── */
static void test_bm25(void)
{
    const char *q = "alpha beta gamma";

    double exact   = sfb_text_bm25(q, "alpha beta gamma", 3.0, 1.2, 0.75);
    double partial = sfb_text_bm25(q, "alpha omega sigma", 3.0, 1.2, 0.75);
    double none    = sfb_text_bm25(q, "delta epsilon", 3.0, 1.2, 0.75);

    chk(exact > 0.0, "bm25 exact match scores positive");
    chk(exact > partial, "bm25 ranks exact match above partial match");
    chk(partial > none, "bm25 ranks partial match above no match");
    chk(none == 0.0, "bm25 no-overlap doc scores exactly 0");

    /* same term count, different lengths: the shorter doc must win */
    double short_doc = sfb_text_bm25("alpha beta", "alpha beta", 20.0, 1.2, 0.75);
    double long_doc  = sfb_text_bm25("alpha beta",
                                     "alpha beta filler filler filler filler",
                                     20.0, 1.2, 0.75);
    chk(short_doc > long_doc, "bm25 length-norm favours the shorter doc");

    /* a doc with three separate hits beats one with a single hit */
    chk(sfb_text_bm25("alpha beta gamma", "alpha beta gamma", 3.0, 1.2, 0.75) >
        sfb_text_bm25("alpha beta gamma", "alpha alpha alpha", 3.0, 1.2, 0.75),
        "bm25 coverage beats raw term frequency");

    /* never NaN/Inf, never negative — including hostile parameters */
    const char *docs[] = { "alpha beta gamma", "", "alpha", "!!!", NULL };
    const double modes[][3] = {
        { 3.0, 1.2, 0.75 }, { 0.0, 1.2, 0.75 }, { -5.0, 1.2, 0.75 },
        { 1.0, 0.0, 0.0 }, { 1.0, -1.0, 2.0 }, { 1.0, 1e300, -1e300 },
        { NAN, NAN, NAN }, { INFINITY, INFINITY, INFINITY },
    };
    int combos = 0;
    for (size_t d = 0; d < sizeof docs / sizeof *docs; d++) {
        for (size_t m = 0; m < sizeof modes / sizeof *modes; m++) {
            double s = sfb_text_bm25(q, docs[d], modes[m][0], modes[m][1], modes[m][2]);
            if (!isfinite(s) || s < 0.0) {
                fprintf(stderr, "FAIL: bm25 doc=%zu mode=%zu -> %g\n", d, m, s);
            }
            assert(isfinite(s) && s >= 0.0);
            combos++;
        }
    }
    assert(isfinite(sfb_text_bm25(NULL, "alpha", 1.0, 1.2, 0.75)));
    assert(isfinite(sfb_text_bm25("alpha", NULL, 1.0, 1.2, 0.75)));
    assert(sfb_text_bm25(NULL, NULL, 1.0, 1.2, 0.75) == 0.0);

    printf("  bm25           ok (exact=%.4f partial=%.4f none=%.4f, %d finite combos)\n",
           exact, partial, none, combos);
}

/* ── text: hash ────────────────────────────────────────────────────────── */
static void test_hash(void)
{
    /* canonical FNV-1a 64 test vectors */
    chk(sfb_text_hash("") == 0xcbf29ce484222325ULL, "fnv1a64 empty == basis");
    chk(sfb_text_hash("a") == 0xaf63dc4c8601ec8cULL, "fnv1a64 \"a\"");
    chk(sfb_text_hash("foobar") == 0x85944171f73967e8ULL, "fnv1a64 \"foobar\"");
    chk(sfb_text_hash(NULL) == sfb_text_hash(""), "hash(NULL) == hash(\"\")");
    chk(sfb_text_hash("abc") != sfb_text_hash("abd"), "hash distinguishes bytes");
    chk(sfb_text_hash("AAAA") != sfb_text_hash("aaaa"), "hash is byte-exact, case-sensitive");

    /* distribution smoke test: no collisions over 4096 short keys */
    static uint64_t seen[4096];
    int nseen = 0, coll = 0;
    char buf[32];
    for (int i = 0; i < 4096; i++) {
        snprintf(buf, sizeof buf, "key-%d", i);
        uint64_t h = sfb_text_hash(buf);
        for (int k = 0; k < nseen; k++) if (seen[k] == h) { coll++; break; }
        seen[nseen++] = h;
    }
    chk(coll == 0, "fnv1a64 has no collision over 4096 keys");
    printf("  hash           ok (fnv1a64 vectors match, 4096 keys, 0 collisions)\n");
}

/* ── quant: encode / decode / distance ─────────────────────────────────── */
#define QN      1000u
#define QDIM    64u
#define QRELTOL 0.015   /* spec: max relative error < 1.5% */

static void test_quant(void)
{
    static float vecs[QN * QDIM];
    static float query[QDIM];
    static float dec[QDIM];

    sfb_quant_block_t blk;
    memset(&blk, 0, sizeof blk);

    for (size_t i = 0; i < QN * QDIM; i++) {
        /* mixed magnitudes so the block scale is exercised realistically */
        float v = frand_sym();
        if ((xorshift64() & 7u) == 0u) v *= 0.01f;
        vecs[i] = v;
    }

    int rc = sfb_quant_encode(&blk, vecs, QN, QDIM);
    chk(rc == SFB_OK, "quant_encode returns SFB_OK");
    chk(blk.n == QN && blk.dim == QDIM, "quant_encode records shape");
    chk(blk.scale > 0.0f && isfinite(blk.scale), "quant_encode scale is finite and positive");

    double max_rel = 0.0, max_rms = 0.0, max_dist_rel = 0.0, max_dist_exact = 0.0;
    double sum_abs_err = 0.0;
    size_t nerr = 0;

    for (uint32_t i = 0; i < QN; i++) {
        const float *v = vecs + (size_t)i * QDIM;
        chk(sfb_quant_decode(&blk, i, dec) == SFB_OK, "quant_decode returns SFB_OK");

        double vmax = 0.0;
        for (uint32_t k = 0; k < QDIM; k++) {
            double a = fabs((double)v[k]);
            if (a > vmax) vmax = a;
        }
        chk(vmax > 0.0, "test vector is not all-zero");

        double worst = 0.0, sq = 0.0;
        for (uint32_t k = 0; k < QDIM; k++) {
            double e = fabs((double)dec[k] - (double)v[k]);
            if (e > worst) worst = e;
            sq += e * e;
            sum_abs_err += e;
            nerr++;
        }
        double rel = worst / vmax;                       /* vs block max-abs */
        double rms = sqrt(sq / (double)QDIM) / vmax;
        if (rel > max_rel) max_rel = rel;
        if (rms > max_rms) max_rms = rms;

        /* distance: exact against the decoded vector, then honest vs original */
        for (uint32_t k = 0; k < QDIM; k++) query[k] = frand_sym();

        double brute_dec = 0.0, brute_orig = 0.0;
        for (uint32_t k = 0; k < QDIM; k++) {
            double a = (double)dec[k] - (double)query[k];
            double b = (double)v[k]   - (double)query[k];
            brute_dec  += a * a;
            brute_orig += b * b;
        }
        float got = -1.0f;
        chk(sfb_quant_distance(&blk, i, query, &got) == SFB_OK,
            "quant_distance returns SFB_OK");
        chk(isfinite(got) && got >= 0.0f, "quant_distance is finite and non-negative");

        double drel = fabs((double)got - brute_dec) / (brute_dec > 0.0 ? brute_dec : 1.0);
        if (drel > max_dist_exact) max_dist_exact = drel;

        if (brute_orig > 1e-12) {
            double drel_o = fabs((double)got - brute_orig) / brute_orig;
            if (drel_o > max_dist_rel) max_dist_rel = drel_o;
        }
    }

    /* Error budget: half a code step is 0.5/127 ≈ 0.394% of the BLOCK max,
     * so relative to a single vector's own max-abs it can only ever be a
     * little larger — never anywhere near 1.5%. */
    printf("  quant encode   ok (%u vectors x %u dim, scale precision)\n", QN, QDIM);
    printf("  max quant error %.4f%% (per-vector max-abs; bound 1.5%%), "
           "max RMS %.4f%%, mean |err| %.3e\n",
           max_rel * 100.0, max_rms * 100.0, sum_abs_err / (double)nerr);
    chk(max_rel < QRELTOL, "quant max relative error < 1.5%");
    chk(max_rms < QRELTOL, "quant RMS relative error < 1.5%");

    printf("  quant distance ok (vs decoded: %.2e rel; vs original: %.4f%% rel, tol 2%%)\n",
           max_dist_exact, max_dist_rel * 100.0);
    chk(max_dist_exact < 1e-5, "quant_distance matches brute-force L2 on decoded exactly");
    chk(max_dist_rel < 0.02, "quant_distance matches brute-force L2 within 2%");

    /* scale must map the block maximum onto exactly ±127 */
    float bmax = 0.0f;
    for (size_t i = 0; i < QN * QDIM; i++) if (fabsf(vecs[i]) > bmax) bmax = fabsf(vecs[i]);
    int saw_full = 0;
    for (size_t i = 0; i < QN * QDIM; i++)
        if (blk.codes[i] == 127 || blk.codes[i] == -127) { saw_full = 1; break; }
    chk(saw_full, "quant uses the full INT8 range");
    chk(bmax / 127.0f == blk.scale, "quant scale == max|v| / 127");

    sfb_quant_free(&blk);
    chk(blk.codes == NULL && blk.n == 0 && blk.dim == 0, "quant_free clears the block");
    sfb_quant_free(&blk);            /* double free must be a no-op */
    sfb_quant_free(NULL);            /* NULL must be a no-op */
}

/* ── quant: degenerate and hostile inputs ──────────────────────────────── */
static void test_quant_edges(void)
{
    sfb_quant_block_t z;
    memset(&z, 0, sizeof z);

    float zeros[128];
    memset(zeros, 0, sizeof zeros);

    chk(sfb_quant_encode(&z, zeros, 2, 64) == SFB_OK, "all-zero encode succeeds");
    chk(isfinite(z.scale) && z.scale > 0.0f, "all-zero vector gets the 1e-8 epsilon scale");
    chk_near((double)z.scale, 1e-8, 1e-12, "all-zero scale == 1e-8 (no div-by-zero)");
    for (size_t i = 0; i < 2 * 64; i++) chk(z.codes[i] == 0, "all-zero codes are 0");

    float out[64];
    chk(sfb_quant_decode(&z, 1, out) == SFB_OK, "decode of zero block succeeds");
    for (int i = 0; i < 64; i++) chk(out[i] == 0.0f, "decode of zero block is 0");

    float q[64];
    for (int i = 0; i < 64; i++) q[i] = 0.0f;
    float d = -1.0f;
    chk(sfb_quant_distance(&z, 0, q, &d) == SFB_OK, "distance on zero block succeeds");
    chk(d == 0.0f, "distance on zero block is 0");
    sfb_quant_free(&z);

    /* single vector, dim 1, exact representable value */
    sfb_quant_block_t one;
    memset(&one, 0, sizeof one);
    float v1[1] = { 127.0f };
    chk(sfb_quant_encode(&one, v1, 1, 1) == SFB_OK, "single-value encode succeeds");
    chk(one.codes[0] == 127, "127.0 encodes to code 127");
    float o1[1];
    chk(sfb_quant_decode(&one, 0, o1) == SFB_OK, "single-value decode succeeds");
    chk_near((double)o1[0], 127.0, 1e-5, "single-value round trip is exact");
    sfb_quant_free(&one);

    /* non-finite input is rejected, never silently encoded */
    sfb_quant_block_t bad;
    memset(&bad, 0, sizeof bad);
    float nanv[4] = { 0.0f, NAN, 0.0f, 0.0f };
    float infv[4] = { 0.0f, INFINITY, 0.0f, 0.0f };
    chk(sfb_quant_encode(&bad, nanv, 1, 4) == SFB_E_NONFINITE, "NaN input rejected");
    chk(sfb_quant_encode(&bad, infv, 1, 4) == SFB_E_NONFINITE, "+Inf input rejected");
    chk(bad.codes == NULL, "rejected encode leaves no allocation");

    /* NULL / out-of-range handling */
    float four[4] = { 1.0f, -2.0f, 3.0f, -4.0f };
    sfb_quant_block_t b;
    memset(&b, 0, sizeof b);
    chk(sfb_quant_encode(&b, four, 1, 4) == SFB_OK, "setup encode ok");

    chk(sfb_quant_encode(NULL, four, 1, 4) == SFB_E_INVALID, "encode(NULL block) rejected");
    chk(sfb_quant_encode(&b, NULL, 1, 4) == SFB_E_INVALID, "encode(NULL data) rejected");
    chk(sfb_quant_encode(&b, four, 0, 4) == SFB_E_INVALID, "encode(n == 0) rejected");
    chk(sfb_quant_encode(&b, four, 1, 0) == SFB_E_INVALID, "encode(dim == 0) rejected");

    chk(sfb_quant_decode(NULL, 0, out) == SFB_E_INVALID, "decode(NULL block) rejected");
    chk(sfb_quant_decode(&b, 0, NULL) == SFB_E_INVALID, "decode(NULL out) rejected");
    chk(sfb_quant_decode(&b, 1, out) == SFB_E_INVALID, "decode(i >= n) rejected");

    chk(sfb_quant_distance(NULL, 0, q, &d) == SFB_E_INVALID, "distance(NULL block) rejected");
    chk(sfb_quant_distance(&b, 0, NULL, &d) == SFB_E_INVALID, "distance(NULL query) rejected");
    chk(sfb_quant_distance(&b, 0, q, NULL) == SFB_E_INVALID, "distance(NULL out) rejected");
    chk(sfb_quant_distance(&b, 5, q, &d) == SFB_E_INVALID, "distance(i >= n) rejected");

    /* re-encoding a different shape allocates; the same total reuses */
    float more[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
    chk(sfb_quant_encode(&b, more, 2, 4) == SFB_OK, "re-encode to (2,4) succeeds");
    chk(b.n == 2 && b.dim == 4, "re-encode updates shape");
    int8_t *reused = b.codes;
    chk(sfb_quant_encode(&b, more, 4, 2) == SFB_OK, "re-encode to (4,2) succeeds");
    chk(b.n == 4 && b.dim == 2, "re-encode updates shape again");
    chk(b.codes == reused, "same total elements reuses the allocation");
    sfb_quant_free(&b);

    printf("  quant edges    ok (zero-vector eps=1e-8, non-finite rejected, NULLs safe)\n");
}

int main(void)
{
    printf("── tests/test_text_quant (SFB v%d.%d)\n",
           SFB_VERSION_MAJOR, SFB_VERSION_MINOR);

    test_jaccard();
    test_bm25();
    test_hash();
    test_quant();
    test_quant_edges();

    printf("=== ALL TEXT+QUANT TESTS PASSED ===\n");
    return 0;
}
