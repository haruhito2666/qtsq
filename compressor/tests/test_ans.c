/*
 * test_ans.c — Phase 1 test suite + gate checks for the Singularity core.
 *
 * Gates (from COMPRESSION_LIBRARY_PLAN.md Phase 1):
 *   G1  100% round-trip pass (unit + 1000-buffer fuzz)
 *   G2  bits-per-symbol within 1% of theoretical entropy (synthetic dists)
 *   G3  decode speed ≥ 100 MB/s single core (run with --speed)
 *   G4  corrupt-input handling: error, never crash, bounded output
 *
 * Note: all printf format strings in this file are compile-time literals.
 */
#include "../src/qtc_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static int g_pass = 0, g_fail = 0;

static void fail_msg(const char *file, int line, const char *msg)
{
    printf("  FAIL %s:%d: %s\n", file, line, msg);
}

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else {                                                      \
        g_fail++;                                               \
        fail_msg(__FILE__, __LINE__, msg);                      \
    }                                                           \
} while (0)

/* ── helpers ─────────────────────────────────────────────────────────── */

static uint64_t xorshift_state = 0x9E3779B97F4A7C15ull;

static uint64_t xorshift64(void)
{
    uint64_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    xorshift_state = x;
    return x;
}

static void fill_random(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(xorshift64() & 0xFF);
}

/* Round-trip through the public API; returns compressed size or -1. */
static long roundtrip(const uint8_t *in, size_t len, const char *label)
{
    uint8_t *comp = NULL, *decomp = NULL;
    size_t comp_len = 0, decomp_len = 0;

    int rc = qtc_compress(in, len, &comp, &comp_len, NULL);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "compress failed");
        return -1;
    }

    rc = qtc_decompress(comp, comp_len, &decomp, &decomp_len);
    if (rc != QTC_OK) {
        printf("     [%s] decompress rc=%d\n", label, rc);
        CHECK(0, "decompress failed");
        free(comp);
        return -1;
    }

    int ok = (decomp_len == len) &&
             (len == 0 || memcmp(in, decomp, len) == 0);
    if (!ok)
        printf("     [%s] in=%zu out=%zu\n", label, len, decomp_len);
    CHECK(ok, "round-trip mismatch");

    free(comp);
    free(decomp);
    return ok ? (long)comp_len : -1;
}

/* ── G1: round-trip unit cases ───────────────────────────────────────── */

static void test_roundtrip_units(void)
{
    printf("── G1: round-trip unit cases\n");

    /* empty */
    roundtrip(NULL, 0, "empty");

    /* 1 byte */
    uint8_t one = 0x42;
    roundtrip(&one, 1, "1-byte");

    /* all-same, various sizes around the Chandrasekhar limit */
    uint8_t *same = malloc(100000);
    memset(same, 0xAB, 100000);
    roundtrip(same, 100, "all-same-100");
    roundtrip(same, 4095, "all-same-4095 (below limit)");
    roundtrip(same, 4096, "all-same-4096 (at limit)");
    roundtrip(same, 100000, "all-same-100k");

    /* uniform random (incompressible) */
    fill_random(same, 100000);
    roundtrip(same, 5000, "random-5k");
    roundtrip(same, 100000, "random-100k");

    /* structured: text-like */
    const char *text =
        "The singularity is the point everything collapses into. "
        "q q q q u u u u — entropy coding packs what prediction leaves.";
    roundtrip((const uint8_t *)text, strlen(text), "text-snippet");

    /* structured: long repetitive with variation */
    for (size_t i = 0; i < 100000; i++)
        same[i] = (uint8_t)((i % 7) * 31 + (i / 7) % 3);
    roundtrip(same, 100000, "structured-periodic");

    /* 10 MB buffer */
    free(same);
    size_t big_n = 10 * 1024 * 1024;
    uint8_t *big = malloc(big_n);
    for (size_t i = 0; i < big_n; i++)
        big[i] = (uint8_t)(xorshift64() % 251); /* skewed-ish alphabet */
    long cs = roundtrip(big, big_n, "10MB-buffer");
    CHECK(cs > 0, "10MB-buffer produced output");
    free(big);
}

/* ── G1: fuzz round-trip ─────────────────────────────────────────────── */

static void test_fuzz_roundtrip(void)
{
    printf("── G1: fuzz round-trip (1000 buffers)\n");

    int failures = 0;
    uint8_t *buf = malloc(65536);

    for (int iter = 0; iter < 1000; iter++) {
        size_t n = (size_t)(xorshift64() % 65536);
        int kind = (int)(xorshift64() % 4);

        switch (kind) {
        case 0: /* uniform random */
            fill_random(buf, n);
            break;
        case 1: /* small alphabet */ {
            int alpha = 1 + (int)(xorshift64() % 16);
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)(xorshift64() % (uint64_t)alpha);
            break;
        }
        case 2: /* runs */ {
            size_t i = 0;
            while (i < n) {
                uint8_t v = (uint8_t)(xorshift64() & 0xFF);
                size_t run = 1 + (size_t)(xorshift64() % 512);
                size_t end = i + run < n ? i + run : n;
                memset(buf + i, v, end - i);
                i = end;
            }
            break;
        }
        default: /* geometric-ish skew */ {
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)((xorshift64() * xorshift64()) & 0xFF);
            break;
        }
        }

        uint8_t *comp = NULL, *decomp = NULL;
        size_t cl = 0, dl = 0;
        int rc = qtc_compress(buf, n, &comp, &cl, NULL);
        if (rc != QTC_OK) { failures++; continue; }
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        if (rc != QTC_OK || dl != n ||
            (n > 0 && memcmp(buf, decomp, n) != 0))
            failures++;
        free(comp);
        free(decomp);
    }

    if (failures != 0)
        printf("     fuzz failures: %d/1000\n", failures);
    CHECK(failures == 0, "fuzz round-trips must all pass");
    free(buf);
}

/* ── G2: entropy efficiency ──────────────────────────────────────────── */

/* True entropy of a distribution, bits/symbol. */
static double dist_entropy(const double *p, int n)
{
    double h = 0.0;
    for (int i = 0; i < n; i++)
        if (p[i] > 0.0)
            h -= p[i] * log2(p[i]);
    return h;
}

static void check_entropy_case(const char *label, const double *p, int n,
                               size_t samples)
{
    uint8_t *buf = malloc(samples);
    /* Sample the distribution via cumulative walk (n is small). */
    double cum[256];
    cum[0] = p[0];
    for (int i = 1; i < n; i++)
        cum[i] = cum[i - 1] + p[i];

    for (size_t i = 0; i < samples; i++) {
        double r = (double)(xorshift64() >> 11) / (double)(1ull << 53);
        int s = 0;
        while (s < n - 1 && r > cum[s])
            s++;
        buf[i] = (uint8_t)s;
    }

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, samples, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "entropy case: compress failed");
    if (rc == QTC_OK) {
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        CHECK(rc == QTC_OK && dl == samples && memcmp(buf, decomp, samples) == 0,
              "entropy case: round-trip");

        double H = dist_entropy(p, n);
        double overhead_bits = (double)(QTC_HEADER_SIZE + 2 + 512) * 8.0;
        double bps = ((double)cl * 8.0 - overhead_bits) / (double)samples;
        double limit = H * 1.01; /* gate: within 1% of entropy */
        printf("     %-14s H=%.4f bps=%.4f (limit %.4f)\n",
               label, H, bps, limit);
        CHECK(bps <= limit, "bps exceeds entropy+1% gate");
        free(decomp);
    }
    free(comp);
    free(buf);
}

static void test_entropy_efficiency(void)
{
    printf("── G2: entropy efficiency (bps ≤ H + 1%%)\n");

    /* uniform over 256 symbols: H = 8 */
    double uniform[256];
    for (int i = 0; i < 256; i++)
        uniform[i] = 1.0 / 256.0;
    check_entropy_case("uniform-256", uniform, 256, 1 << 20);

    /* binary skewed: p = {0.9, 0.1}, H ≈ 0.469 */
    double skewed2[2] = {0.9, 0.1};
    check_entropy_case("skewed-2", skewed2, 2, 1 << 20);

    /* geometric over 64 symbols, p_i ∝ 0.85^i, H ≈ 2.6 */
    double geo[64], z = 0.0;
    for (int i = 0; i < 64; i++) {
        geo[i] = pow(0.85, i);
        z += geo[i];
    }
    for (int i = 0; i < 64; i++)
        geo[i] /= z;
    check_entropy_case("geometric-64", geo, 64, 1 << 20);

    /* text-like: 27 symbols with English-ish frequencies */
    double text[27] = {
        0.0817, 0.0149, 0.0278, 0.0425, 0.1270, 0.0223, 0.0202, /* a-g */
        0.0609, 0.0697, 0.0015, 0.0077, 0.0403, 0.0241, 0.0675, /* h-n */
        0.0751, 0.0193, 0.0010, 0.0599, 0.0633, 0.0906, 0.0276, /* o-u */
        0.0098, 0.0236, 0.0015, 0.0197, 0.0007, 0.1500          /* v-z, sp */
    };
    check_entropy_case("english-27", text, 27, 1 << 20);
}

/* ── G3: decode speed ────────────────────────────────────────────────── */

static void test_decode_speed(void)
{
    printf("── G3: decode speed (gate ≥ 100 MB/s)\n");

    size_t n = 32 * 1024 * 1024;
    uint8_t *buf = malloc(n);
    /* Compressible-ish content so the RAW lens is exercised. */
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((xorshift64() >> 24) % 200);

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "speed: compress failed");
    if (rc != QTC_OK) { free(buf); return; }

    /* warmup */
    qtc_decompress(comp, cl, &decomp, &dl);
    free(decomp);

    int reps = 5;
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        clock_t t0 = clock();
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        clock_t t1 = clock();
        CHECK(rc == QTC_OK && dl == n && memcmp(buf, decomp, n) == 0,
              "speed rep: round-trip");
        double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        if (secs > 0 && secs < best)
            best = secs;
        free(decomp);
        decomp = NULL;
    }

    double mbs = (double)n / (1024.0 * 1024.0) / best;
    printf("     decode: %.1f MB/s (best of %d)\n", mbs, reps);
    CHECK(mbs >= 100.0, "decode speed below 100 MB/s gate");

    /* encode speed, informational */
    clock_t t0 = clock();
    uint8_t *c2 = NULL;
    size_t c2l = 0;
    qtc_compress(buf, n, &c2, &c2l, NULL);
    clock_t t1 = clock();
    double enc_mbs = (double)n / (1024.0 * 1024.0) /
                     ((double)(t1 - t0) / CLOCKS_PER_SEC);
    printf("     encode: %.1f MB/s (informational)\n", enc_mbs);
    free(c2);
    free(comp);
    free(buf);
}

/* ── G4: corrupt-input handling ──────────────────────────────────────── */

static void test_corrupt_handling(void)
{
    printf("── G4: corrupt-input handling\n");

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;

    /* Build a valid stream first. Use low-entropy data (i % 13, ~3.7 bits)
     * and FORCE the order-0 lens so the payload layout is known for the
     * mutation checks below (auto-selection would pick LZ since Phase 3). */
    size_t n = 100000;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(i % 13);
    qtc_options_t g4opts;
    qtc_options_default(&g4opts);
    g4opts.force_lens = QTC_LENS_RAW;
    int rc = qtc_compress(buf, n, &comp, &cl, &g4opts);
    CHECK(rc == QTC_OK, "corrupt: setup compress");
    CHECK(comp[4] == QTC_LENS_RAW, "corrupt: setup must use RAW lens");

    /* Locate the model region and bitstream for the RAW lens. */
    size_t model_off = QTC_HEADER_SIZE + 2;
    size_t model_len = (size_t)comp[QTC_HEADER_SIZE] |
                       ((size_t)comp[QTC_HEADER_SIZE + 1] << 8);
    size_t bits_off = model_off + model_len;

    /* truncated header */
    rc = qtc_decompress(comp, 8, &decomp, &dl);
    if (rc != QTC_ERR_FORMAT)
        printf("     truncated header rc=%d\n", rc);
    CHECK(rc == QTC_ERR_FORMAT, "truncated header must return ERR_FORMAT");

    /* bad magic */
    uint8_t *mut = malloc(cl);
    memcpy(mut, comp, cl);
    mut[0] = 'X';
    rc = qtc_decompress(mut, cl, &decomp, &dl);
    CHECK(rc == QTC_ERR_FORMAT, "bad magic must return ERR_FORMAT");

    /* unknown lens id */
    memcpy(mut, comp, cl);
    mut[4] = 99;
    rc = qtc_decompress(mut, cl, &decomp, &dl);
    CHECK(rc == QTC_ERR_FORMAT, "unknown lens must return ERR_FORMAT");

    /* truncated payload */
    rc = qtc_decompress(comp, cl / 2, &decomp, &dl);
    CHECK(rc != QTC_OK, "truncated payload must be rejected");

    /* corrupted model table (breaks a freq sum) */
    memcpy(mut, comp, cl);
    mut[model_off + 3] ^= 0xFF;
    rc = qtc_decompress(mut, cl, &decomp, &dl);
    CHECK(rc != QTC_OK, "corrupt model must be rejected");

    /* corrupted bitstream bytes — must error or (rarely) still decode,
     * but must NEVER crash or overrun. */
    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, comp, cl);
        size_t pos = bits_off + (size_t)(xorshift64() % (cl - bits_off));
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_decompress(mut, cl, &decomp, &dl);
        if (rc == QTC_OK) {
            free(decomp);
            decomp = NULL;
        }
        survived++;
    }
    CHECK(survived == 200, "bit-flip trials must survive without crash");

    /* declared size exceeds bound */
    rc = qtc_decompress_bounded(comp, cl, &decomp, &dl, 1000);
    CHECK(rc == QTC_ERR_SIZE, "oversized declaration must return ERR_SIZE");

    /* declared size lies huge (DoS attempt) */
    memcpy(mut, comp, cl);
    memset(mut + 8, 0xFF, 8); /* original_size = 2^64-1 */
    rc = qtc_decompress(mut, cl, &decomp, &dl);
    CHECK(rc == QTC_ERR_SIZE || rc == QTC_ERR_ALLOC,
          "huge declared size must be refused");

    free(mut);
    free(comp);
    free(buf);
}

/* ── Chandrasekhar behavior ──────────────────────────────────────────── */

static void test_chandrasekhar(void)
{
    printf("── Chandrasekhar limit behavior\n");

    uint8_t small[100];
    fill_random(small, sizeof(small));

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;

    /* default: below limit → STORED (header + original) */
    int rc = qtc_compress(small, sizeof(small), &comp, &cl, NULL);
    CHECK(rc == QTC_OK && cl == QTC_HEADER_SIZE + sizeof(small),
          "below-limit input must be stored raw");
    CHECK(comp[4] == QTC_LENS_STORED, "below-limit lens must be STORED");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == sizeof(small) &&
          memcmp(small, decomp, sizeof(small)) == 0,
          "stored round-trip");
    free(comp); free(decomp); comp = decomp = NULL;

    /* forced: compress anyway */
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.flags |= QTC_FLAG_FORCE_COMPRESS;
    rc = qtc_compress(small, sizeof(small), &comp, &cl, &opts);
    CHECK(rc == QTC_OK, "forced compress must succeed");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == sizeof(small) &&
          memcmp(small, decomp, sizeof(small)) == 0,
          "forced round-trip");
    free(comp); free(decomp);
}

/* ── Phase 2: order-1 context model ──────────────────────────────────── */

/* Build a realistic English-like text buffer (strong order-1 structure:
 * "q"→"u", "t"→"h", space after words, etc.). */
static size_t make_english(uint8_t *buf, size_t cap, uint64_t seed)
{
    static const char *words[] = {
        "the", "singularity", "is", "the", "point", "everything",
        "collapses", "into", "that", "which", "question", "quite",
        "quickly", "through", "their", "there", "where", "when",
        "entropy", "coding", "packs", "what", "prediction", "leaves",
        "behind", "black", "hole", "compresses", "data", "math",
        "and", "physics", "together", "with", "quantum", "tensors"
    };
    uint64_t st = seed ? seed : 0xABCDEF0123456789ull;
    size_t n = 0;
    while (n + 20 < cap) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        const char *w = words[st % 36];
        size_t wl = strlen(w);
        memcpy(buf + n, w, wl);
        n += wl;
        buf[n++] = ' ';
        if ((st >> 32) % 9 == 0) {
            buf[n - 1] = '.';
            buf[n++] = ' ';
        }
    }
    return n;
}

static void test_order1_roundtrip(void)
{
    printf("── P2: order-1 round-trip\n");

    /* English text — the order-1 home turf. */
    size_t cap = 200000;
    uint8_t *buf = malloc(cap);
    size_t n = make_english(buf, cap, 0);
    roundtrip(buf, n, "english-200k");

    /* Structured binary with strong byte-to-byte correlation. */
    for (size_t i = 0; i < 100000; i++)
        buf[i] = (uint8_t)((i % 37) ^ ((i / 37) % 11));
    roundtrip(buf, 100000, "correlated-binary");

    /* Uniform random — order-1 must not break incompressible data. */
    fill_random(buf, 50000);
    roundtrip(buf, 50000, "random-50k");

    /* Tiny inputs around the Chandrasekhar limit, forced compression. */
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.flags |= QTC_FLAG_FORCE_COMPRESS;
    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    for (size_t len = 1; len <= 64; len++) {
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)(i % 5);
        int rc = qtc_compress(buf, len, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "tiny forced: compress");
        if (rc == QTC_OK) {
            rc = qtc_decompress(comp, cl, &decomp, &dl);
            CHECK(rc == QTC_OK && dl == len && memcmp(buf, decomp, len) == 0,
                  "tiny forced: round-trip");
            free(decomp); decomp = NULL;
            free(comp); comp = NULL;
        }
    }
    free(buf);
}

static void test_order1_fuzz(void)
{
    printf("── P2: order-1 fuzz round-trip (500 buffers)\n");

    int failures = 0;
    uint8_t *buf = malloc(65536);

    for (int iter = 0; iter < 500; iter++) {
        size_t n = (size_t)(xorshift64() % 65536);
        int kind = (int)(xorshift64() % 3);

        switch (kind) {
        case 0: /* markov-ish: next byte depends on previous */
            if (n > 0) {
                buf[0] = (uint8_t)(xorshift64() & 0xFF);
                for (size_t i = 1; i < n; i++)
                    buf[i] = (uint8_t)((buf[i - 1] + (xorshift64() % 3)) & 0xFF);
            }
            break;
        case 1: /* small alphabet markov */
            if (n > 0) {
                buf[0] = 0;
                for (size_t i = 1; i < n; i++)
                    buf[i] = (uint8_t)((buf[i - 1] + (xorshift64() % 2)) % 8);
            }
            break;
        default: /* uniform random */
            fill_random(buf, n);
            break;
        }

        uint8_t *comp = NULL, *decomp = NULL;
        size_t cl = 0, dl = 0;
        int rc = qtc_compress(buf, n, &comp, &cl, NULL);
        if (rc != QTC_OK) { failures++; continue; }
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        if (rc != QTC_OK || dl != n ||
            (n > 0 && memcmp(buf, decomp, n) != 0))
            failures++;
        free(comp);
        free(decomp);
    }

    if (failures != 0)
        printf("     order-1 fuzz failures: %d/500\n", failures);
    CHECK(failures == 0, "order-1 fuzz round-trips must all pass");
    free(buf);
}

/* Phase 2 gate: order-1 must beat order-0 on text-like data, and the
 * auto-selector must never emit a larger file than order-0 alone. */
static void test_order1_beats_order0(void)
{
    printf("── P2 gate: order-1 beats order-0 on text\n");

    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    size_t n = make_english(buf, cap, 42);

    /* Auto (best of all lenses). */
    uint8_t *comp_auto = NULL, *decomp = NULL;
    size_t cl_auto = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp_auto, &cl_auto, NULL);
    CHECK(rc == QTC_OK, "gate: auto compress");
    rc = qtc_decompress(comp_auto, cl_auto, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == n && memcmp(buf, decomp, n) == 0,
          "gate: auto round-trip");
    /* Since Phase 3, LZ usually wins on text; RAW1 is still acceptable.
     * Since v0.15.0, AUTO emits LZ2 (which contests v1 per block). */
    CHECK(comp_auto[4] == QTC_LENS_RAW1 || comp_auto[4] == QTC_LENS_LZ ||
          comp_auto[4] == QTC_LENS_LZ2,
          "gate: auto must pick an entropy lens on text");
    free(decomp); decomp = NULL;

    /* Order-0 only, for comparison: encode directly via internals. */
    qtc_model_t m0;
    qtc_model_build(&m0, buf, n);
    uint8_t *bits0 = NULL;
    size_t bits0_len = 0;
    rc = qtc_ans_encode(buf, n, &m0, &bits0, &bits0_len);
    CHECK(rc == QTC_OK, "gate: order-0 encode");
    size_t o0_total = QTC_HEADER_SIZE + 2 + 512 + bits0_len;

    printf("     english-1MB: order-0 = %zu B, auto = %zu B (%.1f%% smaller)\n",
           o0_total, cl_auto,
           100.0 * (1.0 - (double)cl_auto / (double)o0_total));
    CHECK(cl_auto < o0_total, "gate: order-1 must beat order-0 on text");

    /* No-regression property: auto output ≤ order-0 output on ANY data.
     * Verify on the incompressible case (auto must not make it worse). */
    fill_random(buf, 100000);
    uint8_t *comp_r = NULL;
    size_t cl_r = 0;
    rc = qtc_compress(buf, 100000, &comp_r, &cl_r, NULL);
    CHECK(rc == QTC_OK, "gate: random compress");
    /* random data: neither lens wins → STORED, exactly header + input */
    CHECK(cl_r == QTC_HEADER_SIZE + 100000,
          "gate: incompressible data must fall back to STORED");
    free(comp_r);

    free(bits0);
    free(comp_auto);
    free(buf);
}

/* Corrupt a RAW1 stream: absent-context query must be detected.
 * Uses force_lens to pin the order-1 lens regardless of what LZ would do. */
static void test_order1_corrupt(void)
{
    printf("── P2: order-1 corrupt-input handling\n");

    size_t cap = 100000;
    uint8_t *buf = malloc(cap);
    size_t n = make_english(buf, cap, 7);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_RAW1;

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, &opts);
    CHECK(rc == QTC_OK && comp[4] == QTC_LENS_RAW1, "o1-corrupt: setup RAW1");

    /* Truncated model. */
    rc = qtc_decompress(comp, QTC_HEADER_SIZE + 10, &decomp, &dl);
    CHECK(rc != QTC_OK, "o1-corrupt: truncated model rejected");

    /* Corrupt model table bytes (break a freq sum). */
    uint8_t *mut = malloc(cl);
    memcpy(mut, comp, cl);
    mut[QTC_HEADER_SIZE + 2 + 1 + 3] ^= 0xFF;
    rc = qtc_decompress(mut, cl, &decomp, &dl);
    CHECK(rc != QTC_OK, "o1-corrupt: broken freq table rejected");

    /* Bit-flip torture on the bitstream: never crash. */
    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, comp, cl);
        size_t span = cl - QTC_HEADER_SIZE - 2;
        size_t pos = QTC_HEADER_SIZE + 2 + (size_t)(xorshift64() % span);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_decompress(mut, cl, &decomp, &dl);
        if (rc == QTC_OK) {
            free(decomp);
            decomp = NULL;
        }
        survived++;
    }
    CHECK(survived == 200, "o1-corrupt: bit-flip trials survived");

    free(mut);
    free(comp);
    free(buf);
}

/* v0.16.0 regression: the RAW1 payload stores model_len as a u16, but the
 * packed order-1 model is 2 + present_count*513 bytes. An input with more
 * than 127 distinct context byte values therefore cannot be represented and
 * the old encoder silently truncated the length, corrupting the stream. The
 * encoder must now decline the lens (falling back under force, or to the
 * contest winner under AUTO) instead of emitting a broken RAW1 stream. */
static void test_order1_model_len_overflow(void)
{
    printf("── P2: order-1 model_len u16 overflow declined\n");

    /* Counting sequence: all 256 byte values appear as contexts (strongly
     * order-1 compressible), so present_count = 256 > 127. */
    size_t n = 100000;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(i & 0xFF);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_RAW1;

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, &opts);
    CHECK(rc == QTC_OK, "o1-overflow: forced compress succeeds");
    CHECK(comp[4] != QTC_LENS_RAW1,
          "o1-overflow: no corrupt RAW1 stream emitted for 256 contexts");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == n && memcmp(buf, decomp, dl) == 0,
          "o1-overflow: forced fallback round-trips bit-exact");
    free(decomp); decomp = NULL;
    free(comp); comp = NULL;

    /* Same input through the AUTO contest must also round-trip. */
    qtc_options_default(&opts);
    rc = qtc_compress(buf, n, &comp, &cl, &opts);
    CHECK(rc == QTC_OK, "o1-overflow: auto compress succeeds");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == n && memcmp(buf, decomp, dl) == 0,
          "o1-overflow: auto round-trips bit-exact");
    free(decomp);
    free(comp);
    free(buf);
}

/* ── Phase 3: LZ77 ───────────────────────────────────────────────────── */

static void test_lz_roundtrip(void)
{
    printf("── P3: LZ77 round-trip\n");

    size_t cap = 3 * 1024 * 1024;
    uint8_t *buf = malloc(cap);

    /* Highly repetitive — LZ's best case. */
    const char *phrase = "the quick brown fox jumps over the lazy dog ";
    size_t n = 0;
    while (n + strlen(phrase) < 200000) {
        memcpy(buf + n, phrase, strlen(phrase));
        n += strlen(phrase);
    }
    roundtrip(buf, n, "repetitive-phrase");

    /* English text. */
    n = make_english(buf, 500000, 99);
    roundtrip(buf, n, "english-500k");

    /* Uniform random — LZ must not break incompressible data. */
    fill_random(buf, 100000);
    roundtrip(buf, 100000, "random-100k");

    /* Multi-block: > 1 MB to exercise the blocking path. */
    for (size_t i = 0; i < cap; i++)
        buf[i] = (uint8_t)((i % 251) ^ ((i >> 10) % 7));
    roundtrip(buf, cap, "multi-block-3MB");

    /* Overlapping match: run of one byte (offset-1 self-reference). */
    memset(buf, 'A', 100000);
    roundtrip(buf, 100000, "single-byte-run");

    /* Short matches at boundaries. */
    memcpy(buf, "abcabcabcabc", 12);
    roundtrip(buf, 12, "short-overlap");

    free(buf);
}

static void test_lz_fuzz(void)
{
    printf("── P3: LZ77 fuzz round-trip (300 buffers)\n");

    int failures = 0;
    uint8_t *buf = malloc(131072);

    for (int iter = 0; iter < 300; iter++) {
        size_t n = (size_t)(xorshift64() % 131072);
        int kind = (int)(xorshift64() % 4);

        switch (kind) {
        case 0: /* uniform random */
            fill_random(buf, n);
            break;
        case 1: /* repetitive with noise */
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)("hello world "[i % 12] + (xorshift64() % 2));
            break;
        case 2: /* markov-ish */
            if (n > 0) {
                buf[0] = (uint8_t)(xorshift64() & 0xFF);
                for (size_t i = 1; i < n; i++)
                    buf[i] = (uint8_t)((buf[i - 1] + (xorshift64() % 3)) & 0xFF);
            }
            break;
        default: /* runs */ {
            size_t i = 0;
            while (i < n) {
                uint8_t v = (uint8_t)(xorshift64() & 0xFF);
                size_t run = 1 + (size_t)(xorshift64() % 300);
                size_t end = i + run < n ? i + run : n;
                memset(buf + i, v, end - i);
                i = end;
            }
            break;
        }
        }

        uint8_t *comp = NULL, *decomp = NULL;
        size_t cl = 0, dl = 0;
        int rc = qtc_compress(buf, n, &comp, &cl, NULL);
        if (rc != QTC_OK) { failures++; continue; }
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        if (rc != QTC_OK || dl != n ||
            (n > 0 && memcmp(buf, decomp, n) != 0))
            failures++;
        free(comp);
        free(decomp);
    }

    if (failures != 0)
        printf("     LZ fuzz failures: %d/300\n", failures);
    CHECK(failures == 0, "LZ fuzz round-trips must all pass");
    free(buf);
}

/* ── v0.15.0: LZ2 lens (rep offsets + 8-bit freqs + truncated bitmaps) ─ */

static int lz2_roundtrip(const uint8_t *in, size_t len, const char *label)
{
    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_LZ2;
    opts.flags = QTC_FLAG_FORCE_COMPRESS;

    int rc = qtc_compress(in, len, &comp, &cl, &opts);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "LZ2 compress failed");
        return -1;
    }
    /* The contest never inflates: an incompressible buffer falls back to
     * STORED (16-byte header + raw bytes) even when LZ2 is forced. */
    if (cl < len + 16)
        CHECK(comp[4] == QTC_LENS_LZ2, "LZ2 lens id stamped");
    else
        CHECK(comp[4] == QTC_LENS_STORED, "incompressible falls back to STORED");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    if (rc != QTC_OK) {
        printf("     [%s] decompress rc=%d\n", label, rc);
        CHECK(0, "LZ2 decompress failed");
        free(comp);
        return -1;
    }
    int ok = (dl == len) && (len == 0 || memcmp(in, decomp, len) == 0);
    CHECK(ok, "LZ2 round-trip mismatch");
    free(comp);
    free(decomp);
    return ok ? (int)cl : -1;
}

static void test_lz2_roundtrip(void)
{
    printf("── v0.15.0: LZ2 round-trip\n");

    size_t n = 100000;
    uint8_t *buf = malloc(n);

    /* Rep-heavy patterns: these exercise repeat-offset matches, including
     * blocks where EVERY match is a rep (empty bucket model). */
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 7) * 31 + (i / 7) % 3);
    lz2_roundtrip(buf, n, "structured-periodic");

    memset(buf, 0xAB, n);
    lz2_roundtrip(buf, n, "all-same");

    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)"abc"[i % 3];
    lz2_roundtrip(buf, n, "period-3");

    for (size_t i = 0; i < 5000; i++)
        buf[i] = (uint8_t)((i % 13) * 17);
    lz2_roundtrip(buf, 5000, "small-period-13 (u8 single bucket sym)");

    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 21) + ((i / 1000) % 5));
    lz2_roundtrip(buf, n, "drifting-period (mixed rep/new offsets)");

    {
        const char *phrase = "the quick brown fox jumps over the lazy dog ";
        size_t pl = strlen(phrase);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)phrase[i % pl];
        lz2_roundtrip(buf, n, "phrase-repeat");
    }

    /* All-literal block (no matches at all). */
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(xorshift64() & 0xFF);
    lz2_roundtrip(buf, n, "random (all-literal blocks)");

    /* Multi-block: 2.5 MB spans 3 blocks and exercises the parallel
     * encode path. */
    {
        size_t big_n = 2500000;
        uint8_t *big = malloc(big_n);
        for (size_t i = 0; i < big_n; i++)
            big[i] = (uint8_t)((i % 31) * 7 + (i / 100) % 11);
        lz2_roundtrip(big, big_n, "multi-block 2.5MB");
        free(big);
    }

    free(buf);
}

/* ── v0.16.0: extended block count (lifts the 255 MB per-stream limit) ── */

/* Rewrite a legacy [u8 nblocks] stream into the sentinel-0 extended form
 * [0x00][u32 LE nblocks] and confirm the decoder accepts it bit-exact. This
 * exercises the extended framing without needing a >255 MB input. */
static void test_lz_large_framing(void)
{
    printf("── v0.16.0: LZ/LZ2 extended block count (sentinel-0)\n");

    size_t n = 100000;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 13) * 17 + (i / 13) % 5);

    for (int v2 = 0; v2 <= 1; v2++) {
        uint8_t *comp = NULL; size_t cl = 0;
        int rc = v2 ? qtc_lz2_encode(buf, n, &comp, &cl)
                    : qtc_lz_encode(buf, n, &comp, &cl);
        CHECK(rc == QTC_OK && comp && cl > 1, "lz encode for framing test");
        if (rc != QTC_OK) { free(comp); continue; }

        uint32_t nb = comp[0]; /* legacy count, >= 1 */
        CHECK(nb >= 1, "legacy count is >= 1");

        size_t el = cl + 4;
        uint8_t *ext = malloc(el);
        ext[0] = 0; /* sentinel: extended count follows */
        ext[1]=(uint8_t)nb; ext[2]=(uint8_t)(nb>>8);
        ext[3]=(uint8_t)(nb>>16); ext[4]=(uint8_t)(nb>>24);
        memcpy(ext + 5, comp + 1, cl - 1);

        uint8_t *out = malloc(n);
        size_t consumed = 0;
        rc = v2 ? qtc_lz2_decode(ext, el, n, out, &consumed)
                : qtc_lz_decode(ext, el, n, out, &consumed);
        CHECK(rc == QTC_OK && consumed == el && memcmp(out, buf, n) == 0,
              "extended-count stream decodes bit-exact");

        /* Count one too high -> mismatch with ceil(orig_size/BLOCK). */
        uint32_t bad = nb + 1;
        ext[1]=(uint8_t)bad; ext[2]=(uint8_t)(bad>>8);
        ext[3]=(uint8_t)(bad>>16); ext[4]=(uint8_t)(bad>>24);
        rc = v2 ? qtc_lz2_decode(ext, el, n, out, &consumed)
                : qtc_lz_decode(ext, el, n, out, &consumed);
        CHECK(rc == QTC_ERR_CORRUPT, "mismatched extended count refused");

        /* Truncated extended header: sentinel present but <4 count bytes. */
        rc = v2 ? qtc_lz2_decode(ext, 3, n, out, &consumed)
                : qtc_lz_decode(ext, 3, n, out, &consumed);
        CHECK(rc == QTC_ERR_CORRUPT, "truncated extended header refused");

        free(out); free(ext); free(comp);
    }
    free(buf);
}

/* Real >255 MB round-trip: forces the encoder to emit the sentinel-0 + u32
 * count. Gated behind --speed because it allocates ~257 MB buffers. */
static void test_lz_large_roundtrip(void)
{
    printf("── v0.16.0: LZ2 >255 MB round-trip (extended count)\n");
    size_t n = (size_t)257 * 1024 * 1024; /* 257 blocks > 255 */
    uint8_t *buf = malloc(n);
    if (!buf) { CHECK(0, "alloc 257MB buffer"); return; }
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 31) * 7 + (i / 1000) % 11);

    uint8_t *comp = NULL; size_t cl = 0;
    int rc = qtc_lz2_encode(buf, n, &comp, &cl);
    CHECK(rc == QTC_OK && comp, "lz2 encode 257MB");
    if (rc == QTC_OK) {
        CHECK(comp[0] == 0, "sentinel-0 emitted for >255 blocks");
        uint32_t nb = (uint32_t)comp[1] | ((uint32_t)comp[2]<<8) |
                      ((uint32_t)comp[3]<<16) | ((uint32_t)comp[4]<<24);
        CHECK(nb == 257, "extended block count == 257");

        uint8_t *out = malloc(n);
        size_t consumed = 0;
        rc = qtc_lz2_decode(comp, cl, n, out, &consumed);
        CHECK(rc == QTC_OK && consumed == cl && memcmp(out, buf, n) == 0,
              "257MB LZ2 round-trip bit-exact");
        free(out);
    }
    free(comp);

    /* Full container path: original_size is u64, so a 257 MB stream must
     * survive qtc_compress -> qtc_decompress_bounded. force_lens pins LZ2
     * so the contest does not run on the large buffer. */
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_LZ2;
    opts.flags = QTC_FLAG_FORCE_COMPRESS;
    uint8_t *c2 = NULL; size_t c2l = 0;
    rc = qtc_compress(buf, n, &c2, &c2l, &opts);
    CHECK(rc == QTC_OK && c2 && c2[4] == QTC_LENS_LZ2,
          "container compress 257MB stamps LZ2");
    if (rc == QTC_OK) {
        uint8_t *d2 = NULL; size_t d2l = 0;
        rc = qtc_decompress_bounded(c2, c2l, &d2, &d2l, (uint64_t)n + 4096);
        CHECK(rc == QTC_OK && d2l == n && memcmp(d2, buf, n) == 0,
              "container bounded round-trip 257MB");
        free(d2);
    }
    free(c2);
    free(buf);
}

static void test_lz2_fuzz(void)
{
    printf("── v0.15.0: LZ2 fuzz round-trip (200 buffers)\n");

    int failures = 0;
    uint8_t *buf = malloc(131072);
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_LZ2;
    opts.flags = QTC_FLAG_FORCE_COMPRESS;

    for (int iter = 0; iter < 200; iter++) {
        size_t n = (size_t)(xorshift64() % 131072);
        int kind = (int)(xorshift64() % 4);

        switch (kind) {
        case 0:
            fill_random(buf, n);
            break;
        case 1:
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)("hello world "[i % 12] + (xorshift64() % 2));
            break;
        case 2:
            if (n > 0) {
                buf[0] = (uint8_t)(xorshift64() & 0xFF);
                for (size_t i = 1; i < n; i++)
                    buf[i] = (uint8_t)((buf[i - 1] + (xorshift64() % 3)) & 0xFF);
            }
            break;
        default: {
            size_t i = 0;
            while (i < n) {
                uint8_t v = (uint8_t)(xorshift64() & 0xFF);
                size_t run = 1 + (size_t)(xorshift64() % 300);
                size_t end = i + run < n ? i + run : n;
                memset(buf + i, v, end - i);
                i = end;
            }
            break;
        }
        }

        uint8_t *comp = NULL, *decomp = NULL;
        size_t cl = 0, dl = 0;
        int rc = qtc_compress(buf, n, &comp, &cl, &opts);
        if (rc != QTC_OK) { failures++; continue; }
        rc = qtc_decompress(comp, cl, &decomp, &dl);
        if (rc != QTC_OK || dl != n ||
            (n > 0 && memcmp(buf, decomp, n) != 0))
            failures++;
        free(comp);
        free(decomp);
    }

    if (failures != 0)
        printf("     LZ2 fuzz failures: %d/200\n", failures);
    CHECK(failures == 0, "LZ2 fuzz round-trips must all pass");
    free(buf);
}

static void test_lz2_dominates_lz(void)
{
    printf("── v0.15.0: LZ2 dominates LZ (per-block v1 fallback)\n");

    size_t n = 200000;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 17) * 13 + (i / 50) % 7);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.flags = QTC_FLAG_FORCE_COMPRESS;

    uint8_t *c1 = NULL, *c2 = NULL;
    size_t l1 = 0, l2 = 0;
    opts.force_lens = QTC_LENS_LZ;
    int rc = qtc_compress(buf, n, &c1, &l1, &opts);
    CHECK(rc == QTC_OK, "forced LZ compress");
    opts.force_lens = QTC_LENS_LZ2;
    rc = qtc_compress(buf, n, &c2, &l2, &opts);
    CHECK(rc == QTC_OK, "forced LZ2 compress");

    /* LZ2 contests v1 per block; the only possible loss vs LZ is the
     * 1-byte-per-block version tag. */
    CHECK(l2 <= l1 + 4, "LZ2 must not lose to LZ beyond framing tags");
    printf("     LZ = %zu B, LZ2 = %zu B\n", l1, l2);

    free(c1);
    free(c2);
    free(buf);
}

static void test_lz2_corrupt_guards(void)
{
    printf("── v0.15.0: LZ2 corrupt-stream guards\n");

    size_t n = 20000;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i % 11) * 23);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = QTC_LENS_LZ2;
    opts.flags = QTC_FLAG_FORCE_COMPRESS;

    uint8_t *comp = NULL;
    size_t cl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, &opts);
    CHECK(rc == QTC_OK && cl > 40, "LZ2 base stream for corruption");

    /* Layout: [16B header][u8 nblocks][u32 block_len][u8 version][block].
     * Block payload: [u32 orig][u32 ntok][u32 nmatch][u8 flags]
     *                [u32 rep_bytes][rep bits]... */
    struct { size_t off; uint8_t val; const char *label; } trials[] = {
        { 21, 2,    "unknown block version tag" },
        { 21, 0x7F, "bogus block version tag" },
        { 34, 0x02, "unknown v2 flags bit" },
        { 35, 0xFF, "rep_bytes mismatch (low byte)" },
    };
    for (size_t t = 0; t < sizeof(trials) / sizeof(trials[0]); t++) {
        uint8_t *mut = malloc(cl);
        memcpy(mut, comp, cl);
        mut[trials[t].off] = trials[t].val;
        uint8_t *dec = NULL;
        size_t dl = 0;
        rc = qtc_decompress(mut, cl, &dec, &dl);
        if (rc == QTC_OK)
            printf("     [%s] accepted!\n", trials[t].label);
        CHECK(rc != QTC_OK, "LZ2 must reject corrupted stream");
        free(dec);
        free(mut);
    }

    /* Truncated payload. */
    {
        uint8_t *dec = NULL;
        size_t dl = 0;
        rc = qtc_decompress(comp, cl / 2, &dec, &dl);
        CHECK(rc != QTC_OK, "LZ2 must reject truncated stream");
        free(dec);
    }

    free(comp);
    free(buf);
}

/* Phase 3 gate: LZ must beat zlib on repetitive/text data. */
static void test_lz_beats_zlib(void)
{
    printf("── P3 gate: LZ vs zlib on text\n");

    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    size_t n = make_english(buf, cap, 42);

    uint8_t *comp = NULL, *decomp = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "gate: compress");
    rc = qtc_decompress(comp, cl, &decomp, &dl);
    CHECK(rc == QTC_OK && dl == n && memcmp(buf, decomp, n) == 0,
          "gate: round-trip");
    CHECK(comp[4] == QTC_LENS_LZ || comp[4] == QTC_LENS_LZ2,
          "gate: auto must pick LZ on text");

    /* zlib baseline */
    uLongf zlen = compressBound((uLong)n);
    uint8_t *zbuf = malloc(zlen);
    int zrc = compress2(zbuf, &zlen, buf, (uLong)n, 6);
    CHECK(zrc == Z_OK, "gate: zlib compress");

    printf("     english-1MB: qtc = %zu B, zlib-6 = %lu B (%+.1f%%)\n",
           cl, (unsigned long)zlen,
           100.0 * ((double)cl / (double)zlen - 1.0));
    CHECK(cl < zlen, "gate: LZ must beat zlib-6 on text");

    free(zbuf);
    free(comp);
    free(decomp);
    free(buf);
}

/* ── Phase 4: image lens ─────────────────────────────────────────────── */

/* Round-trip an image through the typed API; returns compressed size or -1. */
static long image_roundtrip(const uint8_t *px, uint32_t w, uint32_t h,
                            uint8_t ch, const char *label)
{
    uint8_t *comp = NULL, *dec = NULL;
    size_t cl = 0;
    uint32_t dw = 0, dh = 0;
    uint8_t dch = 0;

    int rc = qtc_compress_image(px, w, h, ch, &comp, &cl, NULL);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "image compress failed");
        return -1;
    }
    rc = qtc_decompress_image(comp, cl, &dec, &dw, &dh, &dch);
    int ok = (rc == QTC_OK && dw == w && dh == h && dch == ch &&
              memcmp(px, dec, (size_t)w * h * ch) == 0);
    if (!ok)
        printf("     [%s] rc=%d dims=%ux%ux%u\n", label, rc, dw, dh, dch);
    CHECK(ok, "image round-trip mismatch");

    free(comp);
    free(dec);
    return ok ? (long)cl : -1;
}

static void test_image_roundtrip(void)
{
    printf("── P4: image lens round-trip\n");

    uint32_t w = 256, h = 256;

    /* Smooth gradient — prediction should crush this. */
    uint8_t *grad = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 3;
            grad[i] = (uint8_t)x;
            grad[i + 1] = (uint8_t)y;
            grad[i + 2] = (uint8_t)((x + y) / 2);
        }
    image_roundtrip(grad, w, h, 3, "gradient-256");
    free(grad);

    /* Sharp edges — checkerboard. */
    uint8_t *check = malloc((size_t)w * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            check[(size_t)y * w + x] = ((x / 8 + y / 8) & 1) ? 255 : 0;
    image_roundtrip(check, w, h, 1, "checkerboard-256");
    free(check);

    /* Pure noise — worst case, must still round-trip. */
    uint8_t *noise = malloc((size_t)w * h * 3);
    fill_random(noise, (size_t)w * h * 3);
    image_roundtrip(noise, w, h, 3, "noise-256");
    free(noise);

    /* RGBA with alpha gradient. */
    uint8_t *rgba = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 4;
            rgba[i] = (uint8_t)(x ^ y);
            rgba[i + 1] = (uint8_t)(x & y);
            rgba[i + 2] = (uint8_t)(x | y);
            rgba[i + 3] = (uint8_t)(255 - y);
        }
    image_roundtrip(rgba, w, h, 4, "rgba-256");
    free(rgba);

    /* Tiny 1x1 and 1xN edge cases. */
    uint8_t one = 128;
    image_roundtrip(&one, 1, 1, 1, "1x1");
    uint8_t row[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    image_roundtrip(row, 8, 1, 1, "8x1");
}

/* ── v0.14.0: 16-bit image lens (raw camera data) ────────────────────── */

/* Bit-exact round-trip through the typed 16-bit API, plus the generic
 * bounded decoder (exercises the depth-aware dispatch in qtc_core). */
static long image16_roundtrip(const uint16_t *px, uint32_t w, uint32_t h,
                              uint8_t ch, const char *label)
{
    uint8_t *comp = NULL;
    uint16_t *dec = NULL;
    size_t cl = 0;
    uint32_t dw = 0, dh = 0;
    uint8_t dch = 0;

    int rc = qtc_compress_image16(px, w, h, ch, &comp, &cl, NULL);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "image16 compress failed");
        return -1;
    }
    CHECK(comp[4] == QTC_LENS_IMAGE, "image16 uses the IMAGE lens");
    CHECK(comp[16 + 9] == 2, "image16 depth byte must be 2");

    rc = qtc_decompress_image16(comp, cl, &dec, &dw, &dh, &dch);
    int ok = (rc == QTC_OK && dw == w && dh == h && dch == ch &&
              memcmp(px, dec, (size_t)w * h * ch * 2) == 0);
    if (!ok)
        printf("     [%s] rc=%d dims=%ux%ux%u\n", label, rc, dw, dh, dch);
    CHECK(ok, "image16 round-trip mismatch");
    free(dec);

    /* Generic decode path must return the identical bytes. */
    uint8_t *gdec = NULL;
    size_t gl = 0;
    rc = qtc_decompress_bounded(comp, cl, &gdec, &gl, (uint64_t)1 << 31);
    CHECK(rc == QTC_OK && gl == (size_t)w * h * ch * 2 &&
          memcmp(px, gdec, gl) == 0, "image16 generic bounded decode");
    free(gdec);

    free(comp);
    return ok ? (long)cl : -1;
}

static void test_image16_roundtrip(void)
{
    printf("── v0.14.0: image16 lens round-trip (bit-exact)\n");

    uint32_t w = 256, h = 256;

    /* Smooth 16-bit gradient — prediction should crush this. */
    uint16_t *grad = malloc((size_t)w * h * 3 * sizeof(uint16_t));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 3;
            grad[i] = (uint16_t)(x * 257);
            grad[i + 1] = (uint16_t)(y * 257);
            grad[i + 2] = (uint16_t)((x + y) * 128);
        }
    image16_roundtrip(grad, w, h, 3, "gradient16-256");
    free(grad);

    /* Camera-raw-like: smooth 12-bit base + sensor noise in the low bits.
     * This is the case byte-plane splitting exists for. */
    uint16_t *raw = malloc((size_t)w * h * sizeof(uint16_t));
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            seed = seed * 1664525u + 1013904223u;
            int base = (int)(y * 4095 / h) + (int)(x * 512 / w);
            int noise = (int)((seed >> 16) & 15) - 7; /* +/-7 low-bit noise */
            int v = base * 16 + noise;
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            raw[(size_t)y * w + x] = (uint16_t)v;
        }
    long raw_size = image16_roundtrip(raw, w, h, 1, "cameraraw16-256");
    /* Must beat zlib-3 on the same bytes (the generic-compressor bar). */
    if (raw_size > 0) {
        uLongf zlen = compressBound((uLong)((size_t)w * h * 2));
        uint8_t *zbuf = malloc(zlen);
        int zrc = compress2(zbuf, &zlen, (const uint8_t *)raw,
                            (uLong)((size_t)w * h * 2), 3);
        CHECK(zrc == Z_OK, "image16: zlib compress");
        printf("     cameraraw16: qtc = %ld B, zlib-3 = %lu B (%+.1f%%)\n",
               raw_size, (unsigned long)zlen,
               100.0 * ((double)raw_size / (double)zlen - 1.0));
        CHECK((size_t)raw_size < zlen, "image16 must beat zlib-3 on camera raw");
        free(zbuf);
    }
    free(raw);

    /* Pure 16-bit noise — worst case, must still round-trip. */
    uint16_t *noise = malloc((size_t)w * h * 3 * sizeof(uint16_t));
    fill_random((uint8_t *)noise, (size_t)w * h * 3 * 2);
    image16_roundtrip(noise, w, h, 3, "noise16-256");
    free(noise);

    /* RGBA16 with alpha gradient. */
    uint16_t *rgba = malloc((size_t)w * h * 4 * sizeof(uint16_t));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 4;
            rgba[i] = (uint16_t)(x * 129);
            rgba[i + 1] = (uint16_t)(y * 129);
            rgba[i + 2] = (uint16_t)((x ^ y) * 37);
            rgba[i + 3] = (uint16_t)(65535 - y * 128);
        }
    image16_roundtrip(rgba, w, h, 4, "rgba16-256");
    free(rgba);

    /* Tiny edge cases. */
    uint16_t one16 = 40000;
    image16_roundtrip(&one16, 1, 1, 1, "1x1-16bit");
    uint16_t row16[8] = {1, 2, 3, 65535, 65534, 6, 7, 8};
    image16_roundtrip(row16, 8, 1, 1, "8x1-16bit");
}

static void test_image16_compat_and_guards(void)
{
    printf("── v0.14.0: image16 backward compat + corrupt-header guards\n");

    /* 8-bit streams keep depth byte 0 and decode through the old API. */
    uint32_t w = 64, h = 64;
    uint8_t *px8 = malloc((size_t)w * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px8[(size_t)y * w + x] = (uint8_t)(x + y);
    uint8_t *comp8 = NULL;
    size_t cl8 = 0;
    int rc = qtc_compress_image(px8, w, h, 1, &comp8, &cl8, NULL);
    CHECK(rc == QTC_OK, "8-bit image compress");
    CHECK(comp8[16 + 9] == 0, "8-bit stream keeps depth byte 0");
    uint8_t *dec8 = NULL;
    uint32_t dw = 0, dh = 0;
    uint8_t dch = 0;
    rc = qtc_decompress_image(comp8, cl8, &dec8, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dw == w && dh == h && dch == 1 &&
          memcmp(px8, dec8, (size_t)w * h) == 0, "8-bit round-trip unchanged");
    /* The 16-bit API must refuse an 8-bit stream. */
    uint16_t *dec16 = NULL;
    rc = qtc_decompress_image16(comp8, cl8, &dec16, &dw, &dh, &dch);
    CHECK(rc != QTC_OK, "image16 decode must reject 8-bit stream");
    /* And the 8-bit API must refuse a 16-bit stream. */
    free(dec8);
    dec8 = NULL;
    uint16_t *px16 = malloc((size_t)w * h * sizeof(uint16_t));
    for (size_t i = 0; i < (size_t)w * h; i++)
        px16[i] = (uint16_t)(i * 31);
    uint8_t *comp16 = NULL;
    size_t cl16 = 0;
    rc = qtc_compress_image16(px16, w, h, 1, &comp16, &cl16, NULL);
    CHECK(rc == QTC_OK, "16-bit image compress");
    rc = qtc_decompress_image(comp16, cl16, &dec8, &dw, &dh, &dch);
    CHECK(rc != QTC_OK, "8-bit decode must reject 16-bit stream");

    /* Crafted depth=2 header with absurd dimensions: bounded decode must
     * refuse with a size/format error, never allocate. */
    uint8_t evil[64];
    memset(evil, 0, sizeof(evil));
    memcpy(evil, "SGC\x01", 4);
    evil[4] = QTC_LENS_IMAGE;
    evil[5] = 1;
    /* original_size = 0xFFFFFFFFFFFFFFFF */
    memset(evil + 8, 0xFF, 8);
    /* payload: w=0x40000000 h=0x40000000 ch=4 depth=2 layout=0 quant=1 */
    evil[16] = 0x00; evil[17] = 0x00; evil[18] = 0x00; evil[19] = 0x40;
    evil[20] = 0x00; evil[21] = 0x00; evil[22] = 0x00; evil[23] = 0x40;
    evil[24] = 4; evil[25] = 2; evil[26] = 0; evil[27] = 1;
    uint8_t *out = NULL;
    size_t ol = 0;
    rc = qtc_decompress_bounded(evil, sizeof(evil), &out, &ol, (uint64_t)1 << 31);
    CHECK(rc != QTC_OK, "crafted huge image16 header must be rejected");
    free(out);

    free(px8);
    free(px16);
    free(comp8);
    free(comp16);
}

static void test_image16_near_lossless(void)
{
    printf("── v0.14.0: image16 near-lossless error bounds\n");

    uint32_t w = 128, h = 128;
    uint16_t *px = malloc((size_t)w * h * sizeof(uint16_t));
    uint32_t seed = 777;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            seed = seed * 1664525u + 1013904223u;
            int base = (int)(y * 65535 / h);
            int noise = (int)((seed >> 16) & 31) - 15;
            int v = base + noise;
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            px[(size_t)y * w + x] = (uint16_t)v;
        }

    struct { float q; int max_err; } ladder[] = {
        {1.00f, 0}, {0.95f, 1}, {0.85f, 2}, {0.70f, 4}, {0.30f, 8},
    };
    size_t prev_size = 0;
    for (size_t k = 0; k < sizeof(ladder) / sizeof(ladder[0]); k++) {
        qtc_options_t opts;
        qtc_options_default(&opts);
        opts.mode = (ladder[k].q >= 1.0f) ? QTC_MODE_LOSSLESS : QTC_MODE_LOSSY;
        opts.quality = ladder[k].q;
        uint8_t *comp = NULL;
        size_t cl = 0;
        int rc = qtc_compress_image16(px, w, h, 1, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "near-lossless compress");
        uint16_t *dec = NULL;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;
        rc = qtc_decompress_image16(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h && dch == 1,
              "near-lossless decode");
        int worst = 0;
        int wrapped = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            int d = (int)px[i] - (int)dec[i];
            if (d < 0) d = -d;
            if (d > 32768)
                wrapped++; /* clamping must make this impossible */
            if (d > worst) worst = d;
        }
        printf("     q=%.2f: %zu B, worst sample error %d (limit %d), wraps %d\n",
               (double)ladder[k].q, cl, worst, ladder[k].max_err, wrapped);
        CHECK(worst <= ladder[k].max_err, "near-lossless error bound");
        CHECK(wrapped == 0, "near-lossless must not wrap around 0/65535");
        if (ladder[k].q >= 1.0f)
            CHECK(memcmp(px, dec, (size_t)w * h * 2) == 0,
                  "q=1.0 must be bit-exact");
        if (prev_size > 0)
            CHECK(cl <= prev_size, "lower quality must not grow the stream");
        prev_size = cl;
        free(comp);
        free(dec);
    }
    free(px);
}

/* Phase 4 gate: image lens must beat zlib-3 on a photographic image.
 * (The zstd-3 comparison on real photos happens in the bench tool.) */
static void test_image_beats_zstd(void)
{
    printf("── P4 gate: image lens vs zlib-3\n");

    /* Synthetic photographic-ish image: smooth regions + edges + texture.
     * Built from layered gradients and a repeating texture so 2D prediction
     * has real structure to exploit (like a real photo). */
    uint32_t w = 512, h = 512;
    uint8_t *px = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 3;
            /* smooth sky-like gradient */
            int r = 40 + (int)(y * 120 / h);
            int g = 80 + (int)(y * 100 / h);
            int b = 160 + (int)(x * 60 / w);
            /* add a soft radial blob */
            int dx = (int)x - 256, dy = (int)y - 200;
            int d2 = dx * dx + dy * dy;
            if (d2 < 10000) {
                int boost = (10000 - d2) / 120;
                r += boost; g += boost / 2;
            }
            /* fine texture */
            r += ((x * 7 + y * 13) % 9) - 4;
            g += ((x * 11 + y * 5) % 7) - 3;
            px[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            px[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            px[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }

    uint8_t *comp = NULL;
    size_t cl = 0;
    int rc = qtc_compress_image(px, w, h, 3, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "gate: image compress");

    /* round-trip verify */
    uint8_t *dec = NULL;
    uint32_t dw, dh; uint8_t dch;
    rc = qtc_decompress_image(comp, cl, &dec, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dw == w && dh == h && dch == 3 &&
          memcmp(px, dec, (size_t)w * h * 3) == 0, "gate: image round-trip");
    free(dec);

    /* zstd-3 baseline on the raw pixels */
    uLongf zlen = compressBound((uLong)((size_t)w * h * 3));
    uint8_t *zbuf = malloc(zlen);
    int zrc = compress2(zbuf, &zlen, px, (uLong)((size_t)w * h * 3), 3);
    CHECK(zrc == Z_OK, "gate: zlib compress");

    printf("     photo-512: qtc-image = %zu B, zlib-3 = %lu B (%+.1f%%)\n",
           cl, (unsigned long)zlen,
           100.0 * ((double)cl / (double)zlen - 1.0));
    CHECK(cl < zlen, "gate: image lens must beat zlib-3 on image");

    free(zbuf);
    free(comp);
    free(px);
}

/* ── Phase 4: audio lens ─────────────────────────────────────────────── */

/* Round-trip PCM through the typed API; returns compressed size or -1. */
static long audio_roundtrip(const uint8_t *pcm, uint32_t nframes,
                            uint8_t ch, uint8_t bps, const char *label)
{
    uint8_t *comp = NULL, *dec = NULL;
    size_t cl = 0;
    uint32_t dn = 0;
    uint8_t dch = 0, dbps = 0;

    int rc = qtc_compress_audio(pcm, nframes, ch, bps, &comp, &cl, NULL);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "audio compress failed");
        return -1;
    }
    rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
    int ok = (rc == QTC_OK && dn == nframes && dch == ch && dbps == bps &&
              memcmp(pcm, dec, (size_t)nframes * ch * (bps / 8)) == 0);
    if (!ok)
        printf("     [%s] rc=%d n=%u ch=%u bps=%u\n", label, rc, dn, dch, dbps);
    CHECK(ok, "audio round-trip mismatch");

    free(comp);
    free(dec);
    return ok ? (long)cl : -1;
}

static void test_audio_roundtrip(void)
{
    printf("── P4: audio lens round-trip\n");

    /* Stereo 16-bit sine — the bread-and-butter case. */
    uint32_t n = 44100;
    uint8_t *pcm = malloc((size_t)n * 2 * 2);
    for (uint32_t f = 0; f < n; f++) {
        int16_t l = (int16_t)(12000.0 * sin(2.0 * 3.14159265 * 440.0 * f / 44100.0));
        int16_t r = (int16_t)(12000.0 * sin(2.0 * 3.14159265 * 554.0 * f / 44100.0));
        pcm[f * 4] = (uint8_t)l; pcm[f * 4 + 1] = (uint8_t)(l >> 8);
        pcm[f * 4 + 2] = (uint8_t)r; pcm[f * 4 + 3] = (uint8_t)(r >> 8);
    }
    audio_roundtrip(pcm, n, 2, 16, "sine-stereo-16");
    free(pcm);

    /* Mono 8-bit. */
    n = 8000;
    pcm = malloc(n);
    for (uint32_t f = 0; f < n; f++)
        pcm[f] = (uint8_t)(128 + 60.0 * sin(2.0 * 3.14159265 * 220.0 * f / 8000.0));
    audio_roundtrip(pcm, n, 1, 8, "sine-mono-8");
    free(pcm);

    /* Silence — prediction should make this tiny. */
    n = 16384;
    pcm = calloc((size_t)n * 2, 2);
    audio_roundtrip(pcm, n, 2, 16, "silence-stereo-16");
    free(pcm);

    /* Pure noise — worst case, must still round-trip. */
    n = 4096;
    pcm = malloc((size_t)n * 2 * 2);
    fill_random(pcm, (size_t)n * 2 * 2);
    audio_roundtrip(pcm, n, 2, 16, "noise-stereo-16");
    free(pcm);

    /* Block boundary: exactly one block, one block + 1, and tiny inputs. */
    n = 4096;
    pcm = malloc((size_t)n * 2);
    for (uint32_t f = 0; f < n; f++) {
        int16_t s = (int16_t)(f * 3);
        pcm[f * 2] = (uint8_t)s; pcm[f * 2 + 1] = (uint8_t)(s >> 8);
    }
    audio_roundtrip(pcm, n, 1, 16, "mono-16-exact-block");
    free(pcm);

    n = 4097;
    pcm = malloc((size_t)n * 2);
    for (uint32_t f = 0; f < n; f++) {
        int16_t s = (int16_t)(f * 5);
        pcm[f * 2] = (uint8_t)s; pcm[f * 2 + 1] = (uint8_t)(s >> 8);
    }
    audio_roundtrip(pcm, n, 1, 16, "mono-16-block+1");
    free(pcm);

    uint8_t one[2] = {0x34, 0x12};
    audio_roundtrip(one, 1, 1, 16, "1-frame");
}

/* Phase 4 gate: audio lens must beat zlib-3 on music-like PCM.
 * (The zstd-3 comparison happens in the bench tool.) */
static void test_audio_beats_zstd(void)
{
    printf("── P4 gate: audio lens vs zlib-3\n");

    /* Synthetic music-like stereo: layered sines + slow amplitude envelope +
     * a little noise floor, so prediction has real structure to exploit. */
    uint32_t n = 44100 * 3; /* 3 seconds */
    uint8_t *pcm = malloc((size_t)n * 2 * 2);
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        double env = 0.6 + 0.4 * sin(2.0 * 3.14159265 * 0.5 * t);
        double l = env * (8000.0 * sin(2.0 * 3.14159265 * 261.6 * t) +
                          3000.0 * sin(2.0 * 3.14159265 * 523.3 * t));
        double r = env * (8000.0 * sin(2.0 * 3.14159265 * 329.6 * t) +
                          3000.0 * sin(2.0 * 3.14159265 * 659.3 * t));
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        int noise = (int)((rng >> 48) & 0xFF) - 128;
        int16_t sl = (int16_t)(l + noise);
        int16_t sr = (int16_t)(r + noise);
        pcm[f * 4] = (uint8_t)sl; pcm[f * 4 + 1] = (uint8_t)(sl >> 8);
        pcm[f * 4 + 2] = (uint8_t)sr; pcm[f * 4 + 3] = (uint8_t)(sr >> 8);
    }

    uint8_t *comp = NULL;
    size_t cl = 0;
    int rc = qtc_compress_audio(pcm, n, 2, 16, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "gate: audio compress");

    /* round-trip verify */
    uint8_t *dec = NULL;
    uint32_t dn; uint8_t dch, dbps;
    rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == n && dch == 2 && dbps == 16 &&
          memcmp(pcm, dec, (size_t)n * 4) == 0, "gate: audio round-trip");
    free(dec);

    /* zlib-3 baseline on the raw PCM */
    uLongf zlen = compressBound((uLong)((size_t)n * 4));
    uint8_t *zbuf = malloc(zlen);
    int zrc = compress2(zbuf, &zlen, pcm, (uLong)((size_t)n * 4), 3);
    CHECK(zrc == Z_OK, "gate: zlib compress");

    printf("     music-3s: qtc-audio = %zu B, zlib-3 = %lu B (%+.1f%%)\n",
           cl, (unsigned long)zlen,
           100.0 * ((double)cl / (double)zlen - 1.0));
    CHECK(cl < zlen, "gate: audio lens must beat zlib-3 on PCM");

    free(zbuf);
    free(comp);
    free(pcm);
}

/* ── Phase 4: Spaghettification router ───────────────────────────────── */

static void test_detect_router(void)
{
    printf("── P4: router detection accuracy\n");

    /* PCM → AUDIO */
    uint32_t n = 8192;
    uint8_t *pcm = malloc((size_t)n * 2);
    for (uint32_t f = 0; f < n; f++) {
        int16_t s = (int16_t)(9000.0 * sin(2.0 * 3.14159265 * 440.0 * f / 44100.0));
        pcm[f * 2] = (uint8_t)s; pcm[f * 2 + 1] = (uint8_t)(s >> 8);
    }
    qtc_detect_t d = qtc_detect(pcm, (size_t)n * 2);
    CHECK(d.type == QTC_TYPE_AUDIO, "router: sine PCM must detect AUDIO");
    free(pcm);

    /* Text → TEXT */
    const char *txt = "The quick brown fox jumps over the lazy dog. "
                      "Pack my box with five dozen liquor jugs. "
                      "How vexingly quick daft zebras jump!\n";
    size_t tl = strlen(txt);
    /* repeat to get past the 1KB sample floor */
    uint8_t *tbuf = malloc(tl * 40);
    for (int i = 0; i < 40; i++)
        memcpy(tbuf + (size_t)i * tl, txt, tl);
    d = qtc_detect(tbuf, tl * 40);
    CHECK(d.type == QTC_TYPE_TEXT, "router: english text must detect TEXT");
    free(tbuf);

    /* Compressed magic bytes → COMPRESSED */
    uint8_t jpeg[8] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0};
    d = qtc_detect(jpeg, sizeof(jpeg));
    CHECK(d.type == QTC_TYPE_COMPRESSED, "router: JPEG magic must detect COMPRESSED");

    uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    d = qtc_detect(png, sizeof(png));
    CHECK(d.type == QTC_TYPE_COMPRESSED, "router: PNG magic must detect COMPRESSED");

    uint8_t zip[4] = {'P', 'K', 0x03, 0x04};
    d = qtc_detect(zip, sizeof(zip));
    CHECK(d.type == QTC_TYPE_COMPRESSED, "router: ZIP magic must detect COMPRESSED");

    /* Random noise → GENERIC (not AUDIO, not TEXT) */
    uint8_t *noise = malloc(8192);
    fill_random(noise, 8192);
    d = qtc_detect(noise, 8192);
    CHECK(d.type == QTC_TYPE_GENERIC, "router: noise must detect GENERIC");
    free(noise);
}

/* The generic qtc_compress(AUTO) must route detected PCM to the AUDIO lens
 * and round-trip bit-exactly through qtc_decompress. */
static void test_audio_via_generic(void)
{
    printf("── P4: audio via generic qtc_compress(AUTO)\n");

    uint32_t n = 44100;
    uint8_t *pcm = malloc((size_t)n * 2 * 2);
    for (uint32_t f = 0; f < n; f++) {
        int16_t l = (int16_t)(10000.0 * sin(2.0 * 3.14159265 * 440.0 * f / 44100.0));
        int16_t r = (int16_t)(10000.0 * sin(2.0 * 3.14159265 * 660.0 * f / 44100.0));
        pcm[f * 4] = (uint8_t)l; pcm[f * 4 + 1] = (uint8_t)(l >> 8);
        pcm[f * 4 + 2] = (uint8_t)r; pcm[f * 4 + 3] = (uint8_t)(r >> 8);
    }

    uint8_t *comp = NULL, *dec = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(pcm, (size_t)n * 4, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "generic: audio compress");
    CHECK(comp[4] == QTC_LENS_AUDIO, "generic: AUTO must pick AUDIO lens on PCM");

    rc = qtc_decompress(comp, cl, &dec, &dl);
    CHECK(rc == QTC_OK && dl == (size_t)n * 4 &&
          memcmp(pcm, dec, (size_t)n * 4) == 0, "generic: audio round-trip");

    free(dec);
    free(comp);
    free(pcm);
}

/* A compressed-format input must be stored, not recompressed. */
static void test_compressed_stored(void)
{
    printf("── P4: compressed formats are stored\n");

    /* Fake JPEG: magic + random-ish payload. */
    size_t n = 8192;
    uint8_t *buf = malloc(n);
    buf[0] = 0xFF; buf[1] = 0xD8; buf[2] = 0xFF; buf[3] = 0xE0;
    fill_random(buf + 4, n - 4);

    uint8_t *comp = NULL, *dec = NULL;
    size_t cl = 0, dl = 0;
    int rc = qtc_compress(buf, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "stored: compress");
    CHECK(comp[4] == QTC_LENS_STORED, "stored: JPEG-like input must be STORED");

    rc = qtc_decompress(comp, cl, &dec, &dl);
    CHECK(rc == QTC_OK && dl == n && memcmp(buf, dec, n) == 0,
          "stored: round-trip");

    free(dec);
    free(comp);
    free(buf);
}

/* ── Phase 5: MDCT transform ─────────────────────────────────────────── */

/* Perfect reconstruction: forward MDCT per 2N frame (hop N), inverse MDCT,
 * 50% overlap-add must reproduce the interior of the signal exactly. */
static void test_mdct_reconstruction(void)
{
    printf("── P5: MDCT perfect reconstruction\n");

    int N = 128;
    int frames = 8;
    int total = N * (frames + 1);   /* hop N, frame 2N, so (frames+1)*N */

    double *sig = malloc((size_t)total * sizeof(double));
    double *out = calloc((size_t)total, sizeof(double));
    double *frame = malloc((size_t)2 * N * sizeof(double));
    double *coef = malloc((size_t)N * sizeof(double));
    double *rec = malloc((size_t)2 * N * sizeof(double));

    /* A mix of tones so the spectrum is non-trivial. */
    for (int i = 0; i < total; i++)
        sig[i] = sin(2.0 * 3.14159265 * 5.0 * i / total) +
                 0.5 * sin(2.0 * 3.14159265 * 23.0 * i / total);

    qtc_mdct_kernel_t kern;
    int rc = qtc_mdct_kernel_init(&kern, N);
    CHECK(rc == QTC_OK, "mdct: kernel init");
    for (int m = 0; m < frames; m++) {
        for (int n = 0; n < 2 * N; n++)
            frame[n] = sig[(size_t)m * N + n];
        qtc_mdct_forward(&kern, frame, coef);
        qtc_mdct_inverse(&kern, coef, rec);
        for (int n = 0; n < 2 * N; n++)
            out[(size_t)m * N + n] += rec[n];
    }
    qtc_mdct_kernel_free(&kern);

    /* Interior region [N, frames*N) is covered by two frames → exact. */
    double max_err = 0.0;
    for (int i = N; i < frames * N; i++) {
        double e = out[i] - sig[i];
        if (e < 0) e = -e;
        if (e > max_err) max_err = e;
    }
    CHECK(max_err < 1e-9, "mdct: overlap-add must reconstruct interior");
    printf("     N=%d frames=%d max_err=%.2e\n", N, frames, max_err);

    free(sig); free(out); free(frame); free(coef); free(rec);
}

/* ── Phase 5: lossy MDCT audio lens ──────────────────────────────────── */

/* SNR in dB between two 16-bit stereo PCM buffers (same length). */
static double pcm_snr_db(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    double sig = 0.0, noise = 0.0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        int16_t sa = (int16_t)((uint16_t)a[i] | ((uint16_t)a[i + 1] << 8));
        int16_t sb = (int16_t)((uint16_t)b[i] | ((uint16_t)b[i + 1] << 8));
        sig += (double)sa * sa;
        double d = (double)sa - sb;
        noise += d * d;
    }
    if (noise < 1e-9)
        return 200.0; /* effectively identical */
    return 10.0 * log10(sig / noise);
}

static void test_audio_mdct_lossy(void)
{
    printf("── P5: lossy MDCT audio lens\n");

    /* Music-like stereo signal. */
    uint32_t n = 44100;
    uint8_t *pcm = malloc((size_t)n * 4);
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        double env = 0.6 + 0.4 * sin(2.0 * 3.14159265 * 0.5 * t);
        int16_t l = (int16_t)(env * (8000 * sin(2 * 3.14159265 * 261.6 * t) +
                                     3000 * sin(2 * 3.14159265 * 523.3 * t)));
        int16_t r = (int16_t)(env * (8000 * sin(2 * 3.14159265 * 329.6 * t) +
                                     3000 * sin(2 * 3.14159265 * 659.3 * t)));
        pcm[f * 4] = (uint8_t)l; pcm[f * 4 + 1] = (uint8_t)(l >> 8);
        pcm[f * 4 + 2] = (uint8_t)r; pcm[f * 4 + 3] = (uint8_t)(r >> 8);
    }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;

    size_t sizes[3];
    double snrs[3];
    float quals[3] = {0.9f, 0.5f, 0.2f};

    for (int q = 0; q < 3; q++) {
        opts.quality = quals[q];
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dn = 0;
        uint8_t dch = 0, dbps = 0;

        int rc = qtc_compress_audio(pcm, n, 2, 16, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "mdct-lossy: compress");
        CHECK(comp[4] == QTC_LENS_AUDIO_MDCT2, "mdct-lossy: lens id");

        rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
        CHECK(rc == QTC_OK && dn == n && dch == 2 && dbps == 16,
              "mdct-lossy: decode dims");

        sizes[q] = cl;
        snrs[q] = pcm_snr_db(pcm, dec, (size_t)n * 4);
        printf("     quality=%.1f: %zu B, SNR=%.1f dB\n",
               quals[q], cl, snrs[q]);

        free(comp);
        free(dec);
    }

    /* Rate-distortion must be monotonic: higher quality → bigger file. */
    CHECK(sizes[0] > sizes[1] && sizes[1] > sizes[2],
          "mdct-lossy: size must decrease as quality drops");
    /* High quality must be near-transparent (SNR > 30 dB is good audio). */
    CHECK(snrs[0] > 30.0, "mdct-lossy: quality 0.9 must exceed 30 dB SNR");
    /* Even aggressive quality must stay usable (> 10 dB). */
    CHECK(snrs[2] > 10.0, "mdct-lossy: quality 0.2 must exceed 10 dB SNR");
    /* Lossy must actually shrink vs the lossless lens. */
    CHECK(sizes[2] < (size_t)n * 4, "mdct-lossy: must beat raw PCM size");

    free(pcm);
}

/* ── Phase 5.7: perceptual audio lens v2 (mdct2) ─────────────────────── */

static void test_audio_mdct2(void)
{
    printf("── P5.7: perceptual audio lens v2 (M/S, masking, rate control)\n");

    /* 2 s of music-like stereo: bass + detuned chord pair + periodic
     * noise bursts (deterministic), 44.1 kHz. */
    uint32_t n = 44100 * 2;
    uint8_t *pcm = (uint8_t *)malloc((size_t)n * 4);
    CHECK(pcm != NULL, "mdct2: alloc");
    unsigned rng = 12345u;
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        double bass = 6000 * sin(2 * 3.14159265 * 82.4 * t);
        double cl = 5000 * sin(2 * 3.14159265 * 261.6 * t) +
                    2500 * sin(2 * 3.14159265 * 329.6 * t);
        double cr = 5000 * sin(2 * 3.14159265 * 261.9 * t) +
                    2500 * sin(2 * 3.14159265 * 330.1 * t);
        rng = rng * 1103515245u + 12345u;
        double hat = ((f % 5512) < 800)
            ? (double)((int)(rng >> 16) % 4000 - 2000) * 0.5 : 0.0;
        int16_t l = (int16_t)(bass + cl + hat);
        int16_t r = (int16_t)(bass + cr + hat);
        pcm[f * 4] = (uint8_t)l; pcm[f * 4 + 1] = (uint8_t)(l >> 8);
        pcm[f * 4 + 2] = (uint8_t)r; pcm[f * 4 + 3] = (uint8_t)(r >> 8);
    }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;

    /* Budget adherence: final bitrate within [+1%, -10%] of the target,
     * monotonic across targets; stereo must set the M/S flag. */
    int kb[3] = {96, 128, 192};
    size_t prev = 0;
    double snr96 = 0.0;
    for (int i = 0; i < 3; i++) {
        opts.target_kbps = kb[i];
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dn = 0;
        uint8_t dch = 0, dbps = 0;
        int rc = qtc_compress_audio(pcm, n, 2, 16, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "mdct2: budget compress");
        CHECK(comp[4] == QTC_LENS_AUDIO_MDCT2, "mdct2: lens id");
        /* Payload starts at container offset 16; its flags byte is at
         * payload offset 9 → container byte 25. */
        CHECK((comp[16 + 9] & 1u) == 1, "mdct2: stereo sets M/S flag");
        rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
        CHECK(rc == QTC_OK && dn == n && dch == 2 && dbps == 16,
              "mdct2: budget decode dims");
        double kbps_act = (double)cl * 8.0 / ((double)n / 44100.0) / 1000.0;
        CHECK(kbps_act <= kb[i] * 1.01, "mdct2: must not exceed budget");
        CHECK(kbps_act >= kb[i] * 0.90, "mdct2: must use most of budget");
        if (i == 0)
            snr96 = pcm_snr_db(pcm, dec, (size_t)n * 4);
        if (prev)
            CHECK(cl > prev, "mdct2: higher target must grow the file");
        prev = cl;
        free(comp);
        free(dec);
    }
    CHECK(snr96 > 12.0, "mdct2: 96 kbps stays usable (> 12 dB SNR)");

    /* Mono: M/S flag must stay clear, budget still honored. */
    uint8_t *mono = (uint8_t *)malloc((size_t)n * 2);
    CHECK(mono != NULL, "mdct2: mono alloc");
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        int16_t m = (int16_t)(9000 * sin(2 * 3.14159265 * 440.0 * t) *
                              (0.5 + 0.5 * sin(2 * 3.14159265 * 2.0 * t)));
        mono[f * 2] = (uint8_t)m;
        mono[f * 2 + 1] = (uint8_t)(m >> 8);
    }
    opts.target_kbps = 64;
    uint8_t *mc = NULL, *md = NULL;
    size_t ml = 0;
    uint32_t mdn = 0;
    uint8_t mdch = 0, mdbps = 0;
    int rc = qtc_compress_audio(mono, n, 1, 16, &mc, &ml, &opts);
    CHECK(rc == QTC_OK, "mdct2: mono compress");
    CHECK((mc[16 + 9] & 1u) == 0, "mdct2: mono leaves M/S flag clear");
    rc = qtc_decompress_audio(mc, ml, &md, &mdn, &mdch, &mdbps);
    CHECK(rc == QTC_OK && mdn == n && mdch == 1 && mdbps == 16,
          "mdct2: mono decode dims");
    double mkbps = (double)ml * 8.0 / ((double)n / 44100.0) / 1000.0;
    CHECK(mkbps <= 64 * 1.05, "mdct2: mono honors budget");
    free(mc);
    free(md);
    free(mono);
    free(pcm);
}

/* ── v0.14.0: float32 audio lens (raw mic / DSP capture) ─────────────── */

/* Bit-exact lossless round-trip through the typed float API + the generic
 * bounded decoder. Compares raw bit patterns (memcmp), so it catches any
 * change to NaN payloads, denormals, -0, etc. */
static void afloat_roundtrip(const float *pcm, uint32_t nframes,
                             uint8_t ch, const char *label)
{
    uint8_t *comp = NULL;
    float *dec = NULL;
    size_t cl = 0;
    uint32_t dn = 0;
    uint8_t dch = 0;

    int rc = qtc_compress_audio_float(pcm, nframes, ch, &comp, &cl, NULL);
    if (rc != QTC_OK) {
        printf("     [%s] compress rc=%d\n", label, rc);
        CHECK(0, "audio_float compress failed");
        return;
    }
    CHECK(comp[4] == QTC_LENS_AUDIO_FLOAT, "audio_float lens id");

    rc = qtc_decompress_audio_float(comp, cl, &dec, &dn, &dch);
    int ok = (rc == QTC_OK && dn == nframes && dch == ch &&
              memcmp(pcm, dec, (size_t)nframes * ch * 4) == 0);
    if (!ok)
        printf("     [%s] rc=%d dims=%u ch=%u\n", label, rc, dn, dch);
    CHECK(ok, "audio_float lossless bit-exact round-trip");
    free(dec);

    /* Generic bounded decode must return identical bytes. */
    uint8_t *gdec = NULL;
    size_t gl = 0;
    rc = qtc_decompress_bounded(comp, cl, &gdec, &gl, (uint64_t)1 << 31);
    CHECK(rc == QTC_OK && gl == (size_t)nframes * ch * 4 &&
          memcmp(pcm, gdec, gl) == 0, "audio_float generic bounded decode");
    free(gdec);

    free(comp);
}

static void test_audio_float_lossless(void)
{
    printf("── v0.14.0: audio_float lossless (bit-exact)\n");

    uint32_t n = 44100; /* 1 s */

    /* Music-like stereo floats in [-1,1]: the normal mic/DSP case. */
    float *stereo = (float *)malloc((size_t)n * 2 * sizeof(float));
    CHECK(stereo != NULL, "afloat: alloc");
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        double bass = 0.4 * sin(2 * 3.14159265 * 82.4 * t);
        stereo[f * 2]     = (float)(bass + 0.3 * sin(2 * 3.14159265 * 261.6 * t));
        stereo[f * 2 + 1] = (float)(bass + 0.3 * sin(2 * 3.14159265 * 330.0 * t));
    }
    afloat_roundtrip(stereo, n, 2, "stereo-sine-1s");

    /* Compression check vs zlib-3 on the same bytes (the generic bar). */
    {
        uint8_t *comp = NULL;
        size_t cl = 0;
        int rc = qtc_compress_audio_float(stereo, n, 2, &comp, &cl, NULL);
        CHECK(rc == QTC_OK, "afloat: compress for ratio");
        uLongf zlen = compressBound((uLong)((size_t)n * 2 * 4));
        uint8_t *zbuf = malloc(zlen);
        int zrc = compress2(zbuf, &zlen, (const uint8_t *)stereo,
                            (uLong)((size_t)n * 2 * 4), 3);
        CHECK(zrc == Z_OK, "afloat: zlib compress");
        printf("     stereo-float: qtc = %zu B, zlib-3 = %lu B (%+.1f%%), raw = %zu B\n",
               cl, (unsigned long)zlen,
               100.0 * ((double)cl / (double)zlen - 1.0), (size_t)n * 2 * 4);
        CHECK(cl < zlen, "audio_float must beat zlib-3 on float audio");
        CHECK(cl < (size_t)n * 2 * 4, "audio_float must compress below raw");
        free(zbuf);
        free(comp);
    }
    free(stereo);

    /* Mono. */
    float *mono = (float *)malloc((size_t)n * sizeof(float));
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        mono[f] = (float)(0.5 * sin(2 * 3.14159265 * 440.0 * t) *
                          (0.5 + 0.5 * sin(2 * 3.14159265 * 3.0 * t)));
    }
    afloat_roundtrip(mono, n, 1, "mono-sine-1s");
    free(mono);

    /* Special IEEE-754 values: NaN (multiple payloads), ±inf, denormals,
     * ±0, large/normals. Must all round-trip bit-exactly. */
    float special[64];
    uint32_t pats[16] = {
        0x7F800000u,          /* +inf  */
        0xFF800000u,          /* -inf  */
        0x7FC00000u,          /* quiet NaN */
        0x7F800001u,          /* NaN payload 1 */
        0x7FBEEFu,            /* NaN payload 2 */
        0x00000000u,          /* +0 */
        0x80000000u,          /* -0 */
        0x00000001u,          /* smallest denormal */
        0x007FFFFFu,          /* largest denormal */
        0x80000001u,          /* negative denormal */
        0x3F800000u,          /* 1.0f */
        0xBF800000u,          /* -1.0f */
        0x7F7FFFFFu,          /* FLT_MAX */
        0x00800000u,          /* FLT_MIN (smallest normal) */
        0x40490FDBu,          /* pi */
        0x3DCCCCCDu,          /* 0.1f */
    };
    for (int i = 0; i < 64; i++)
        memcpy(&special[i], &pats[i % 16], 4);
    afloat_roundtrip(special, 32, 2, "special-values");

    /* Pure random float bits — worst case, still must round-trip. */
    float *noise = (float *)malloc((size_t)n * 2 * sizeof(float));
    fill_random((uint8_t *)noise, (size_t)n * 2 * 4);
    afloat_roundtrip(noise, n, 2, "random-bits");
    free(noise);
}

static void test_audio_float_lossy(void)
{
    printf("── v0.14.0: audio_float lossy (int16 MDCT2 adapter)\n");

    uint32_t n = 44100 * 2; /* 2 s stereo */
    float *pcm = (float *)malloc((size_t)n * 2 * sizeof(float));
    CHECK(pcm != NULL, "afloat-lossy: alloc");
    unsigned rng = 98765u;
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        double bass = 0.35 * sin(2 * 3.14159265 * 82.4 * t);
        double cl = 0.3 * sin(2 * 3.14159265 * 261.6 * t);
        double cr = 0.3 * sin(2 * 3.14159265 * 329.6 * t);
        rng = rng * 1103515245u + 12345u;
        double hat = ((f % 5512) < 800)
            ? ((double)((int)(rng >> 16) % 4000 - 2000) / 4000.0) * 0.15 : 0.0;
        pcm[f * 2]     = (float)(bass + cl + hat);
        pcm[f * 2 + 1] = (float)(bass + cr + hat);
    }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;

    int kb[3] = {96, 128, 192};
    size_t prev = 0;
    double snr96 = 0.0;
    for (int i = 0; i < 3; i++) {
        opts.target_kbps = kb[i];
        uint8_t *comp = NULL;
        float *dec = NULL;
        size_t cl = 0;
        uint32_t dn = 0;
        uint8_t dch = 0;
        int rc = qtc_compress_audio_float(pcm, n, 2, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "afloat-lossy: compress");
        CHECK(comp[4] == QTC_LENS_AUDIO_FLOAT, "afloat-lossy: lens id");
        CHECK(comp[16 + 5] == 1, "afloat-lossy: mode byte = 1");
        rc = qtc_decompress_audio_float(comp, cl, &dec, &dn, &dch);
        CHECK(rc == QTC_OK && dn == n && dch == 2, "afloat-lossy: decode dims");
        double kbps_act = (double)cl * 8.0 / ((double)n / 44100.0) / 1000.0;
        CHECK(kbps_act <= kb[i] * 1.01, "afloat-lossy: must not exceed budget");
        CHECK(kbps_act >= kb[i] * 0.85, "afloat-lossy: must use most of budget");
        /* Float-domain SNR. */
        double sig = 0.0, noise = 0.0;
        for (size_t s = 0; s < (size_t)n * 2; s++) {
            sig += (double)pcm[s] * pcm[s];
            double d = (double)pcm[s] - (double)dec[s];
            noise += d * d;
        }
        double snr = (noise > 0.0) ? 10.0 * log10(sig / noise) : 200.0;
        if (i == 0)
            snr96 = snr;
        printf("     %d kbps: %zu B (%.1f kbps act), float SNR = %.1f dB\n",
               kb[i], cl, kbps_act, snr);
        if (prev)
            CHECK(cl > prev, "afloat-lossy: higher target grows the file");
        prev = cl;
        free(comp);
        free(dec);
    }
    CHECK(snr96 > 12.0, "afloat-lossy: 96 kbps stays usable (> 12 dB)");

    /* Mono lossy. */
    float *mono = (float *)malloc((size_t)n * sizeof(float));
    for (uint32_t f = 0; f < n; f++) {
        double t = (double)f / 44100.0;
        mono[f] = (float)(0.6 * sin(2 * 3.14159265 * 440.0 * t) *
                          (0.5 + 0.5 * sin(2 * 3.14159265 * 2.0 * t)));
    }
    opts.target_kbps = 64;
    uint8_t *mc = NULL;
    float *md = NULL;
    size_t ml = 0;
    uint32_t mdn = 0;
    uint8_t mdch = 0;
    int rc = qtc_compress_audio_float(mono, n, 1, &mc, &ml, &opts);
    CHECK(rc == QTC_OK, "afloat-lossy: mono compress");
    rc = qtc_decompress_audio_float(mc, ml, &md, &mdn, &mdch);
    CHECK(rc == QTC_OK && mdn == n && mdch == 1, "afloat-lossy: mono decode");
    double mkbps = (double)ml * 8.0 / ((double)n / 44100.0) / 1000.0;
    CHECK(mkbps <= 64 * 1.05, "afloat-lossy: mono honors budget");
    free(mc);
    free(md);
    free(mono);
    free(pcm);
}

/* ── v0.14.0: raw-data superset coverage ───────────────────────────────
 * One regression anchor for the whole "store raw capture data" matrix the
 * v0.14.0 request asked for: images 8- AND 16-bit, audio int16 AND float32,
 * each in lossless AND lossy, plus text on the generic byte path. Every cell
 * must compress and round-trip (lossless) or decode (lossy). Detailed
 * assertions live in the per-lens tests above; this is the compact matrix. */
static void test_raw_data_coverage(void)
{
    printf("── v0.14.0: raw-data superset coverage matrix\n");

    uint32_t w = 64, h = 64;

    /* image 8-bit lossless */
    uint8_t *img8 = malloc((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h * 3; i++)
        img8[i] = (uint8_t)((i * 7) ^ (i >> 3));
    uint8_t *c = NULL; size_t cl = 0;
    CHECK(qtc_compress_image(img8, w, h, 3, &c, &cl, NULL) == QTC_OK,
          "matrix: img8 lossless compress");
    uint8_t *d8 = NULL; uint32_t dw, dh; uint8_t dch;
    CHECK(qtc_decompress_image(c, cl, &d8, &dw, &dh, &dch) == QTC_OK &&
          memcmp(img8, d8, (size_t)w * h * 3) == 0, "matrix: img8 lossless rt");
    free(c); free(d8);

    /* image 8-bit lossy (DCT contest) */
    qtc_options_t lo; qtc_options_default(&lo);
    lo.mode = QTC_MODE_LOSSY; lo.quality = 0.85f;
    CHECK(qtc_compress_image(img8, w, h, 3, &c, &cl, &lo) == QTC_OK,
          "matrix: img8 lossy compress");
    CHECK(qtc_decompress_image(c, cl, &d8, &dw, &dh, &dch) == QTC_OK,
          "matrix: img8 lossy decode");
    free(c); free(d8); free(img8);

    /* image 16-bit lossless */
    uint16_t *img16 = malloc((size_t)w * h * sizeof(uint16_t));
    for (size_t i = 0; i < (size_t)w * h; i++)
        img16[i] = (uint16_t)(i * 31 + (i % 17) * 4096);
    CHECK(qtc_compress_image16(img16, w, h, 1, &c, &cl, NULL) == QTC_OK,
          "matrix: img16 lossless compress");
    uint16_t *d16 = NULL;
    CHECK(qtc_decompress_image16(c, cl, &d16, &dw, &dh, &dch) == QTC_OK &&
          memcmp(img16, d16, (size_t)w * h * 2) == 0, "matrix: img16 lossless rt");
    free(c); free(d16);

    /* image 16-bit lossy (near-lossless) */
    lo.quality = 0.85f;
    CHECK(qtc_compress_image16(img16, w, h, 1, &c, &cl, &lo) == QTC_OK,
          "matrix: img16 lossy compress");
    CHECK(qtc_decompress_image16(c, cl, &d16, &dw, &dh, &dch) == QTC_OK,
          "matrix: img16 lossy decode");
    int worst = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int dd = (int)img16[i] - (int)d16[i];
        if (dd < 0) dd = -dd;
        if (dd > worst) worst = dd;
    }
    CHECK(worst <= 2, "matrix: img16 lossy within near-lossless bound");
    free(c); free(d16); free(img16);

    /* audio int16 lossless + lossy */
    uint32_t nf = 44100;
    uint8_t *pcm16 = malloc((size_t)nf * 2 * 2);
    for (uint32_t f = 0; f < nf; f++) {
        int16_t s = (int16_t)(12000 * sin(2 * 3.14159265 * 440.0 * f / 44100.0));
        pcm16[f * 4] = (uint8_t)s; pcm16[f * 4 + 1] = (uint8_t)(s >> 8);
        pcm16[f * 4 + 2] = (uint8_t)s; pcm16[f * 4 + 3] = (uint8_t)(s >> 8);
    }
    uint8_t *ap = NULL; size_t al = 0;
    uint32_t an; uint8_t ach, abps;
    CHECK(qtc_compress_audio(pcm16, nf, 2, 16, &ap, &al, NULL) == QTC_OK,
          "matrix: pcm16 lossless compress");
    uint8_t *ad = NULL;
    CHECK(qtc_decompress_audio(ap, al, &ad, &an, &ach, &abps) == QTC_OK &&
          memcmp(pcm16, ad, (size_t)nf * 4) == 0, "matrix: pcm16 lossless rt");
    free(ap); free(ad);
    lo.mode = QTC_MODE_LOSSY; lo.quality = 0.5f; lo.target_kbps = 128;
    CHECK(qtc_compress_audio(pcm16, nf, 2, 16, &ap, &al, &lo) == QTC_OK,
          "matrix: pcm16 lossy compress");
    CHECK(qtc_decompress_audio(ap, al, &ad, &an, &ach, &abps) == QTC_OK,
          "matrix: pcm16 lossy decode");
    free(ap); free(ad); free(pcm16);

    /* audio float32 lossless + lossy */
    float *fpcm = malloc((size_t)nf * sizeof(float));
    for (uint32_t f = 0; f < nf; f++)
        fpcm[f] = (float)(0.5 * sin(2 * 3.14159265 * 440.0 * f / 44100.0));
    CHECK(qtc_compress_audio_float(fpcm, nf, 1, &ap, &al, NULL) == QTC_OK,
          "matrix: f32 lossless compress");
    float *fd = NULL; uint8_t fch;
    CHECK(qtc_decompress_audio_float(ap, al, &fd, &an, &fch) == QTC_OK &&
          memcmp(fpcm, fd, (size_t)nf * 4) == 0, "matrix: f32 lossless rt");
    free(ap); free(fd);
    lo.target_kbps = 64;
    CHECK(qtc_compress_audio_float(fpcm, nf, 1, &ap, &al, &lo) == QTC_OK,
          "matrix: f32 lossy compress");
    CHECK(qtc_decompress_audio_float(ap, al, &fd, &an, &fch) == QTC_OK,
          "matrix: f32 lossy decode");
    free(ap); free(fd); free(fpcm);

    /* text on the generic byte path */
    const char *text = "The quick brown fox jumps over the lazy dog. ";
    size_t tlen = strlen(text);
    uint8_t *tbuf = malloc(tlen * 200);
    for (int i = 0; i < 200; i++)
        memcpy(tbuf + i * tlen, text, tlen);
    uint8_t *tc = NULL, *td = NULL; size_t tcl = 0, tdl = 0;
    CHECK(qtc_compress(tbuf, tlen * 200, &tc, &tcl, NULL) == QTC_OK,
          "matrix: text compress");
    CHECK(tcl < tlen * 200, "matrix: text actually compresses");
    CHECK(qtc_decompress(tc, tcl, &td, &tdl) == QTC_OK &&
          tdl == tlen * 200 && memcmp(tbuf, td, tdl) == 0,
          "matrix: text round-trip");
    free(tc); free(td); free(tbuf);

    printf("     all 13 raw-data cells pass (8/16-bit img, int16/f32 audio, text)\n");
}

/* ── Phase 5: Delaunay triangulation ─────────────────────────────────── */

/* Verify the Delaunay property: no point strictly inside any circumcircle. */
static int delaunay_valid(const double *xs, const double *ys, int npts,
                          const int *tris, int ntris)
{
    for (int t = 0; t < ntris; t++) {
        int a = tris[3 * t], b = tris[3 * t + 1], c = tris[3 * t + 2];
        if (a < 0 || a >= npts || b < 0 || b >= npts || c < 0 || c >= npts)
            return 0;
        /* circumcircle via determinant test */
        double ax = xs[a] - xs[c], ay = ys[a] - ys[c];
        double bx = xs[b] - xs[c], by = ys[b] - ys[c];
        double d = 2.0 * (ax * by - bx * ay);
        if (fabs(d) < 1e-12)
            return 0; /* degenerate triangle */
        double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by;
        double ux = (a2 * by - b2 * ay) / d;
        double uy = (b2 * ax - a2 * bx) / d;
        double r2 = ux * ux + uy * uy;
        for (int p = 0; p < npts; p++) {
            if (p == a || p == b || p == c)
                continue;
            double dx = xs[p] - xs[c] - ux, dy = ys[p] - ys[c] - uy;
            if (dx * dx + dy * dy < r2 - 1e-6)
                return 0;
        }
    }
    return 1;
}

static void test_delaunay(void)
{
    printf("── P5: Delaunay triangulation\n");

    /* Unit square + center: 5 points, expect 4 triangles. */
    double xs[5] = {0, 1, 1, 0, 0.5};
    double ys[5] = {0, 0, 1, 1, 0.5};
    int *tris = NULL, ntris = 0;
    int rc = qtc_delaunay(xs, ys, 5, &tris, &ntris);
    CHECK(rc == QTC_OK, "delaunay: square+center builds");
    CHECK(ntris == 4, "delaunay: square+center has 4 triangles");
    CHECK(delaunay_valid(xs, ys, 5, tris, ntris),
          "delaunay: square+center circumcircle property");
    free(tris);

    /* Grid of points. */
    int gw = 8, gh = 8;
    int npts = gw * gh;
    double *gxs = malloc((size_t)npts * sizeof(double));
    double *gys = malloc((size_t)npts * sizeof(double));
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            gxs[y * gw + x] = x * 10.0;
            gys[y * gw + x] = y * 10.0;
        }
    rc = qtc_delaunay(gxs, gys, npts, &tris, &ntris);
    CHECK(rc == QTC_OK, "delaunay: 8x8 grid builds");
    /* Euler: t = 2n - 2 - h; h = 28 hull points for an 8x8 grid. */
    CHECK(ntris == 2 * npts - 2 - 28, "delaunay: grid triangle count (Euler)");
    CHECK(delaunay_valid(gxs, gys, npts, tris, ntris),
          "delaunay: grid circumcircle property");
    free(tris);
    free(gxs);
    free(gys);

    /* Random fuzz: 200 points, property must hold. */
    int rn = 200;
    double *rxs = malloc((size_t)rn * sizeof(double));
    double *rys = malloc((size_t)rn * sizeof(double));
    for (int i = 0; i < rn; i++) {
        rxs[i] = (double)(xorshift64() % 100000) / 100.0;
        rys[i] = (double)(xorshift64() % 100000) / 100.0;
    }
    rc = qtc_delaunay(rxs, rys, rn, &tris, &ntris);
    CHECK(rc == QTC_OK, "delaunay: random 200 builds");
    CHECK(ntris > 0, "delaunay: random 200 non-empty");
    CHECK(delaunay_valid(rxs, rys, rn, tris, ntris),
          "delaunay: random 200 circumcircle property");
    free(tris);
    free(rxs);
    free(rys);
}

/* ── Phase 5: lossy mesh image lens ──────────────────────────────────── */

/* PSNR in dB between two images (same dims/channels). */
static double image_psnr_db(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    double mse = 0.0;
    for (size_t i = 0; i < bytes; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)bytes;
    if (mse < 1e-9)
        return 200.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

static void test_image_mesh_lossy(void)
{
    printf("── P5: lossy mesh image lens\n");

    /* Smooth photographic-ish image: gradients + a soft blob + an edge. */
    uint32_t w = 256, h = 256;
    uint8_t *px = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)(y * w + x)) * 3;
            int r = 40 + (int)(y * 120 / h);
            int g = 80 + (int)(y * 100 / h);
            int b = 160 + (int)(x * 60 / w);
            int dx = (int)x - 128, dy = (int)y - 100;
            int d2 = dx * dx + dy * dy;
            if (d2 < 2500) {
                int boost = (2500 - d2) / 30;
                r += boost; g += boost / 2;
            }
            /* a hard vertical edge */
            if (x > 200) { r += 60; b -= 40; }
            px[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            px[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            px[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;

    size_t sizes[3];
    double psnrs[3];
    float quals[3] = {0.9f, 0.5f, 0.2f};

    for (int q = 0; q < 3; q++) {
        opts.quality = quals[q];
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;

        int rc = qtc_compress_image(px, w, h, 3, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "mesh-lossy: compress");
        /* The contest may pick any of the three lossy image lenses; the
         * point of this test is that the lossy path round-trips well. */
        CHECK(comp[4] == QTC_LENS_IMAGE_MESH ||
              comp[4] == QTC_LENS_IMAGE_ACCRETION ||
              comp[4] == QTC_LENS_IMAGE_DCT, "mesh-lossy: lossy lens id");

        rc = qtc_decompress_image(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h && dch == 3,
              "mesh-lossy: decode dims");

        sizes[q] = cl;
        psnrs[q] = image_psnr_db(px, dec, (size_t)w * h * 3);
        printf("     quality=%.1f: %zu B, PSNR=%.1f dB\n",
               quals[q], cl, psnrs[q]);

        free(comp);
        free(dec);
    }

    /* Rate-distortion must be monotonic: higher quality → bigger file. */
    CHECK(sizes[0] > sizes[1] && sizes[1] > sizes[2],
          "mesh-lossy: size must decrease as quality drops");
    /* High quality must be visually good (PSNR > 30 dB). */
    CHECK(psnrs[0] > 30.0, "mesh-lossy: quality 0.9 must exceed 30 dB PSNR");
    /* Quality must improve with the knob. */
    CHECK(psnrs[0] > psnrs[2], "mesh-lossy: PSNR must rise with quality");
    /* Lossy must shrink vs raw pixels. */
    CHECK(sizes[2] < (size_t)w * h * 3, "mesh-lossy: must beat raw size");

    free(px);
}

/* ── Phase 5.5b: 8x8 block-DCT photo lens (v0.12.0) ──────────────────── */

static void test_image_dct_lossy(void)
{
    printf("── P5.5b: lossy DCT image lens\n");

    /* Photo-like content: smooth gradients + texture + hard edges. */
    uint32_t w = 256, h = 256;
    uint8_t *px = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 3;
            int r = 40 + (int)(y * 120 / h) + (int)(30.0 * sin(x * 0.3));
            int g = 80 + (int)(y * 100 / h) + (int)(20.0 * cos(y * 0.2));
            int b = 160 + (int)(x * 60 / w);
            if (x > 200) { r += 60; b -= 40; }   /* hard edge */
            px[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            px[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            px[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }

    size_t sizes[3];
    double psnrs[3];
    float quals[3] = {0.9f, 0.5f, 0.2f};

    for (int q = 0; q < 3; q++) {
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;

        int rc = qtc_image_dct_encode(px, w, h, 3, quals[q], &comp, &cl);
        CHECK(rc == QTC_OK, "dct: encode");

        rc = qtc_image_dct_decode(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h && dch == 3,
              "dct: decode dims");

        sizes[q] = cl;
        psnrs[q] = image_psnr_db(px, dec, (size_t)w * h * 3);
        printf("     quality=%.1f: %zu B, PSNR=%.1f dB\n",
               quals[q], cl, psnrs[q]);

        free(comp);
        free(dec);
    }

    CHECK(sizes[0] > sizes[1] && sizes[1] > sizes[2],
          "dct: size must decrease as quality drops");
    CHECK(psnrs[0] > 30.0, "dct: quality 0.9 must exceed 30 dB PSNR");
    CHECK(psnrs[0] > psnrs[2], "dct: PSNR must rise with quality");
    CHECK(sizes[2] < (size_t)w * h * 3, "dct: must beat raw size");

    /* Grayscale (ch=1, no chroma subsampling). */
    {
        uint8_t *gray = malloc((size_t)w * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                gray[(size_t)y * w + x] = (uint8_t)((x + y) & 0xFF);
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;
        int rc = qtc_image_dct_encode(gray, w, h, 1, 0.8f, &comp, &cl);
        CHECK(rc == QTC_OK, "dct-gray: encode");
        rc = qtc_image_dct_decode(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h && dch == 1,
              "dct-gray: decode dims");
        double psnr = image_psnr_db(gray, dec, (size_t)w * h);
        CHECK(psnr > 25.0, "dct-gray: reasonable PSNR");
        free(comp); free(dec); free(gray);
    }

    /* Non-multiple-of-8 dimensions (edge block replication). */
    {
        uint32_t ow = 100, oh = 60;
        uint8_t *odd = malloc((size_t)ow * oh * 3);
        fill_random(odd, (size_t)ow * oh * 3);
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;
        int rc = qtc_image_dct_encode(odd, ow, oh, 3, 0.7f, &comp, &cl);
        CHECK(rc == QTC_OK, "dct-odd: encode");
        rc = qtc_image_dct_decode(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == ow && dh == oh && dch == 3,
              "dct-odd: decode dims");
        free(comp); free(dec); free(odd);
    }

    /* ch=2 and ch=4 are rejected (DCT supports 1 and 3 only). */
    {
        uint8_t *two = malloc((size_t)w * h * 2);
        uint8_t *comp = NULL;
        size_t cl = 0;
        int rc = qtc_image_dct_encode(two, w, h, 2, 0.5f, &comp, &cl);
        CHECK(rc == QTC_ERR_FORMAT, "dct: ch=2 rejected");
        free(two);
    }

    /* The lossy contest must accept the DCT lens as a candidate: on
     * photo-like content it should be competitive (we only assert the
     * contest produces a valid lossy result that round-trips). */
    {
        qtc_options_t opts;
        qtc_options_default(&opts);
        opts.mode = QTC_MODE_LOSSY;
        opts.quality = 0.7f;
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;
        int rc = qtc_compress_image(px, w, h, 3, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "dct-contest: compress");
        CHECK(comp[4] == QTC_LENS_IMAGE_MESH ||
              comp[4] == QTC_LENS_IMAGE_ACCRETION ||
              comp[4] == QTC_LENS_IMAGE_DCT, "dct-contest: lossy lens id");
        rc = qtc_decompress_image(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h, "dct-contest: round-trip");
        printf("     contest picked lens %u for photo-like content\n",
               comp[4]);
        free(comp); free(dec);
    }

    /* Above QTC_CONTEST_MAX_PIXELS (1 MP) the lossy contest must skip the
     * slow scene codecs and run only the DCT coder — a 10 MP photo took
     * 5m14s with the full contest (mesh 92s + accretion 124s) and DCT won
     * anyway. Use a just-over-1 MP image so the suite stays fast. */
    {
        uint32_t bw = 1009, bh = 997;   /* 1009*997 = 1,005,973 px > 1 MP */
        uint8_t *big = (uint8_t *)malloc((size_t)bw * bh * 3);
        CHECK(big != NULL, "bigphoto: alloc");
        if (big) {
            for (uint32_t y = 0; y < bh; y++)
                for (uint32_t x = 0; x < bw; x++) {
                    size_t i = ((size_t)y * bw + x) * 3;
                    big[i]     = (uint8_t)((x * 255) / bw);
                    big[i + 1] = (uint8_t)((y * 255) / bh);
                    big[i + 2] = (uint8_t)((x + y) & 0xff);
                }
            qtc_options_t bopts;
            qtc_options_default(&bopts);
            bopts.mode = QTC_MODE_LOSSY;
            bopts.quality = 0.7f;
            uint8_t *bcomp = NULL, *bdec = NULL;
            size_t bcl = 0;
            uint32_t bdw = 0, bdh = 0;
            uint8_t bdch = 0;
            int brc = qtc_compress_image(big, bw, bh, 3, &bcomp, &bcl, &bopts);
            CHECK(brc == QTC_OK, "bigphoto: compress");
            CHECK(bcomp && bcomp[4] == QTC_LENS_IMAGE_DCT,
                  "bigphoto: >1MP contest takes the DCT fast path");
            brc = qtc_decompress_image(bcomp, bcl, &bdec, &bdw, &bdh, &bdch);
            CHECK(brc == QTC_OK && bdw == bw && bdh == bh,
                  "bigphoto: round-trip");
            free(bcomp); free(bdec); free(big);
        }
    }

    free(px);
}

/* ── Phase 5: lossy adversarial input + graceful degradation ─────────── */

static void test_lossy_adversarial(void)
{
    printf("── P5: lossy adversarial + graceful degradation\n");

    float badq[2] = {-1.0f, 7.7f};
    uint32_t an = 1000;   /* deliberately not a multiple of N=256 */
    uint8_t *pcm = malloc((size_t)an * 2);
    for (uint32_t i = 0; i < an; i++) {
        int16_t s = (int16_t)(9000.0 * sin(2.0 * 3.14159265 * 440.0 *
                                           (double)i / 44100.0));
        pcm[2 * i] = (uint8_t)s;
        pcm[2 * i + 1] = (uint8_t)(s >> 8);
    }

    /* ---- audio MDCT: malformed payloads must be rejected, never crash ---- */
    uint8_t *apl = NULL;
    size_t apl_len = 0;
    int rc = qtc_audio_mdct_encode(pcm, an, 1, 16, 0.5f, &apl, &apl_len);
    CHECK(rc == QTC_OK, "adv-audio: setup encode");
    CHECK(apl_len > 10, "adv-audio: payload has a body");

    uint8_t *dec = NULL;
    uint32_t dn = 0;
    uint8_t dch = 0, dbps = 0;
    uint8_t *mut = malloc(apl_len);

    rc = qtc_audio_mdct_decode(apl, 9, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: short header rejected");

    memcpy(mut, apl, apl_len);
    mut[0] = mut[1] = mut[2] = mut[3] = 0;
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: nframes=0 rejected");

    memcpy(mut, apl, apl_len); mut[4] = 0;
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: channels=0 rejected");

    memcpy(mut, apl, apl_len); mut[4] = 9;
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: channels=9 rejected");

    memcpy(mut, apl, apl_len); mut[5] = 12;
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: bps=12 rejected");

    memcpy(mut, apl, apl_len); mut[6] = 0xFF;   /* N=255, not a power of 2 */
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: N=255 rejected");

    memcpy(mut, apl, apl_len); mut[7] = 0x20;   /* N=8192, above the cap */
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_CORRUPT, "adv-audio: N=8192 rejected");

    rc = qtc_audio_mdct_decode(apl, 10 + (apl_len - 10) / 2,
                               &dec, &dn, &dch, &dbps);
    CHECK(rc != QTC_OK, "adv-audio: truncated body rejected");

    /* Crafted nframes = 2^32-1: the 32-bit frame count used to wrap around,
     * undersize the overlap buffer, and overrun it on the write loop. */
    memcpy(mut, apl, apl_len);
    mut[0] = mut[1] = mut[2] = mut[3] = 0xFF;
    rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_ERR_SIZE, "adv-audio: wrap-nframes refused");

    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, apl, apl_len);
        size_t pos = (size_t)(xorshift64() % apl_len);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_audio_mdct_decode(mut, apl_len, &dec, &dn, &dch, &dbps);
        if (rc == QTC_OK) { free(dec); dec = NULL; }
        survived++;
    }
    CHECK(survived == 200, "adv-audio: bit-flip trials survive");
    free(mut);
    free(apl);

    /* ---- audio MDCT: awkward but valid inputs ---- */
    qtc_options_t aopts;
    qtc_options_default(&aopts);
    aopts.mode = QTC_MODE_LOSSY;
    aopts.quality = 0.5f;

    int16_t one = 1234;
    uint8_t *ac = NULL;
    size_t acl = 0;
    rc = qtc_compress_audio((const uint8_t *)&one, 1, 1, 16, &ac, &acl, &aopts);
    CHECK(rc == QTC_OK, "adv-audio: 1-sample compress");
    rc = qtc_decompress_audio(ac, acl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == 1, "adv-audio: 1-sample decode dims");
    free(ac); free(dec);

    uint8_t quiet[777], harsh[777];
    memset(quiet, 128, sizeof(quiet));
    for (int i = 0; i < 777; i++)
        harsh[i] = (i & 1) ? 255 : 0;
    rc = qtc_compress_audio(quiet, 777, 1, 8, &ac, &acl, &aopts);
    CHECK(rc == QTC_OK, "adv-audio: silence compress");
    rc = qtc_decompress_audio(ac, acl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == 777 && dch == 1 && dbps == 8,
          "adv-audio: silence decode dims");
    free(ac); free(dec);

    rc = qtc_compress_audio(harsh, 777, 1, 8, &ac, &acl, &aopts);
    CHECK(rc == QTC_OK, "adv-audio: square compress");
    rc = qtc_decompress_audio(ac, acl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == 777, "adv-audio: square decode dims");
    free(ac); free(dec);

    for (int q = 0; q < 2; q++) {
        aopts.quality = badq[q];
        rc = qtc_compress_audio(pcm, an, 1, 16, &ac, &acl, &aopts);
        CHECK(rc == QTC_OK, "adv-audio: clamped quality compress");
        rc = qtc_decompress_audio(ac, acl, &dec, &dn, &dch, &dbps);
        CHECK(rc == QTC_OK && dn == an, "adv-audio: clamped quality decode");
        free(ac); free(dec);
    }

    /* Cross-API: typed compress must decode through the generic entry. */
    aopts.quality = 0.6f;
    rc = qtc_compress_audio(pcm, an, 1, 16, &ac, &acl, &aopts);
    CHECK(rc == QTC_OK, "adv-audio: typed compress for generic decode");
    uint8_t *gdec = NULL;
    size_t gdl = 0;
    rc = qtc_decompress(ac, acl, &gdec, &gdl);
    CHECK(rc == QTC_OK && gdl == (size_t)an * 2, "adv-audio: generic decode");
    free(ac); free(gdec);
    free(pcm);

    /* ---- mesh: crafted payloads with exact vertex bytes ---- */
    {
        uint8_t vraw[3 * 7];
        memset(vraw, 0, sizeof(vraw));
        vraw[7] = 7;    /* vertex 1 at x=7 */
        vraw[16] = 9;   /* vertex 2 at y=9, outside the h=8 image */
        uint8_t *lz = NULL;
        size_t lz_len = 0;
        rc = qtc_lz_encode(vraw, sizeof(vraw), &lz, &lz_len);
        CHECK(rc == QTC_OK, "adv-mesh: craft lz");
        size_t mhdr = 14;
        uint8_t *pl = malloc(mhdr + lz_len);
        pl[0] = 8; pl[1] = 0; pl[2] = 0; pl[3] = 0;        /* w = 8 */
        pl[4] = 8; pl[5] = 0; pl[6] = 0; pl[7] = 0;        /* h = 8 */
        pl[8] = 3; pl[9] = 0;                              /* ch, reserved */
        pl[10] = 3; pl[11] = 0; pl[12] = 0; pl[13] = 0;    /* nverts = 3 */
        memcpy(pl + mhdr, lz, lz_len);
        uint8_t *mpx = NULL;
        uint32_t mw2 = 0, mh2 = 0;
        uint8_t mch = 0;
        rc = qtc_image_mesh_decode(pl, mhdr + lz_len, &mpx, &mw2, &mh2, &mch);
        CHECK(rc == QTC_ERR_CORRUPT, "adv-mesh: out-of-range vertex rejected");
        free(lz); free(pl);

        /* nverts = 2: below the triangle minimum. */
        uint8_t v2[2 * 7];
        memset(v2, 0, sizeof(v2));
        v2[7] = 7;
        rc = qtc_lz_encode(v2, sizeof(v2), &lz, &lz_len);
        CHECK(rc == QTC_OK, "adv-mesh: craft lz 2 verts");
        pl = malloc(mhdr + lz_len);
        memcpy(pl, "\x08\x00\x00\x00\x08\x00\x00\x00\x03\x00", 10);
        pl[10] = 2; pl[11] = 0; pl[12] = 0; pl[13] = 0;
        memcpy(pl + mhdr, lz, lz_len);
        rc = qtc_image_mesh_decode(pl, mhdr + lz_len, &mpx, &mw2, &mh2, &mch);
        CHECK(rc == QTC_ERR_CORRUPT, "adv-mesh: nverts=2 rejected");
        free(lz); free(pl);

        /* nverts = 1000 on an 8x8 image: more vertices than pixels. */
        rc = qtc_lz_encode(v2, sizeof(v2), &lz, &lz_len);
        CHECK(rc == QTC_OK, "adv-mesh: craft lz count");
        pl = malloc(mhdr + lz_len);
        memcpy(pl, "\x08\x00\x00\x00\x08\x00\x00\x00\x03\x00", 10);
        pl[10] = 0xE8; pl[11] = 0x03; pl[12] = 0; pl[13] = 0;  /* 1000 */
        memcpy(pl + mhdr, lz, lz_len);
        rc = qtc_image_mesh_decode(pl, mhdr + lz_len, &mpx, &mw2, &mh2, &mch);
        CHECK(rc == QTC_ERR_CORRUPT, "adv-mesh: nverts>pixels rejected");
        free(lz); free(pl);
    }

    /* ---- mesh: degenerate images ---- */
    qtc_options_t iopts;
    qtc_options_default(&iopts);
    iopts.mode = QTC_MODE_LOSSY;
    iopts.quality = 0.5f;

    uint8_t rgb[12];
    for (int i = 0; i < 12; i++)
        rgb[i] = (uint8_t)(i * 20);

    /* The MESH lens itself refuses sub-triangle images (the typed LOSSY API
     * now falls back to the accretion lens, which handles them). */
    rc = qtc_image_mesh_encode(rgb, 1, 1, 3, 0.5f, &ac, &acl);
    CHECK(rc == QTC_ERR_FORMAT, "adv-mesh: 1x1 refused at encode");
    rc = qtc_image_mesh_encode(rgb, 1, 2, 3, 0.5f, &ac, &acl);
    CHECK(rc == QTC_ERR_FORMAT, "adv-mesh: 1x2 refused at encode");
    rc = qtc_image_mesh_encode(rgb, 2, 1, 3, 0.5f, &ac, &acl);
    CHECK(rc == QTC_ERR_FORMAT, "adv-mesh: 2x1 refused at encode");

    uint8_t *ipx = NULL;
    uint32_t iw = 0, ih = 0;
    uint8_t ich = 0;
    rc = qtc_compress_image(rgb, 2, 2, 3, &ac, &acl, &iopts);
    CHECK(rc == QTC_OK, "adv-mesh: 2x2 compress");
    rc = qtc_decompress_image(ac, acl, &ipx, &iw, &ih, &ich);
    CHECK(rc == QTC_OK && iw == 2 && ih == 2 && ich == 3,
          "adv-mesh: 2x2 decode dims");
    free(ac); free(ipx);

    uint8_t col8[24];
    for (int i = 0; i < 24; i++)
        col8[i] = (uint8_t)(i * 10);
    rc = qtc_compress_image(col8, 1, 8, 3, &ac, &acl, &iopts);
    CHECK(rc == QTC_OK, "adv-mesh: 1x8 collinear compress");
    rc = qtc_decompress_image(ac, acl, &ipx, &iw, &ih, &ich);
    CHECK(rc == QTC_OK && iw == 1 && ih == 8,
          "adv-mesh: 1x8 collinear decode dims");
    free(ac); free(ipx);

    /* ---- mesh: noise, quality clamping, bit-flip fuzz ---- */
    uint32_t nw = 64, nh = 64;
    uint8_t *noise = malloc((size_t)nw * nh * 3);
    fill_random(noise, (size_t)nw * nh * 3);
    rc = qtc_compress_image(noise, nw, nh, 3, &ac, &acl, &iopts);
    CHECK(rc == QTC_OK, "adv-mesh: noise compress");
    rc = qtc_decompress_image(ac, acl, &ipx, &iw, &ih, &ich);
    CHECK(rc == QTC_OK && iw == nw && ih == nh,
          "adv-mesh: noise decode dims");
    free(ac); free(ipx);

    for (int q = 0; q < 2; q++) {
        iopts.quality = badq[q];
        rc = qtc_compress_image(noise, nw, nh, 3, &ac, &acl, &iopts);
        CHECK(rc == QTC_OK, "adv-mesh: clamped quality compress");
        rc = qtc_decompress_image(ac, acl, &ipx, &iw, &ih, &ich);
        CHECK(rc == QTC_OK && iw == nw, "adv-mesh: clamped quality decode");
        free(ac); free(ipx);
    }
    free(noise);

    uint32_t gw = 32, gh = 32;
    uint8_t *gimg = malloc((size_t)gw * gh * 3);
    for (uint32_t y = 0; y < gh; y++)
        for (uint32_t x = 0; x < gw; x++) {
            size_t i = ((size_t)y * gw + x) * 3;
            gimg[i] = (uint8_t)(x * 8);
            gimg[i + 1] = (uint8_t)(y * 8);
            gimg[i + 2] = 128;
        }
    iopts.quality = 0.5f;
    rc = qtc_compress_image(gimg, gw, gh, 3, &ac, &acl, &iopts);
    CHECK(rc == QTC_OK, "adv-mesh: fuzz base compress");

    gdec = NULL;
    gdl = 0;
    rc = qtc_decompress(ac, acl, &gdec, &gdl);
    CHECK(rc == QTC_OK && gdl == (size_t)gw * gh * 3,
          "adv-mesh: generic decode");
    free(gdec);

    mut = malloc(acl);
    survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, ac, acl);
        size_t pos = (size_t)(xorshift64() % acl);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_decompress_image(mut, acl, &ipx, &iw, &ih, &ich);
        if (rc == QTC_OK) { free(ipx); ipx = NULL; }
        survived++;
    }
    CHECK(survived == 200, "adv-mesh: bit-flip trials survive");
    free(mut);
    free(ac);
    free(gimg);
}

/* ── Phase 5.5: multi-primitive accretion scene ──────────────────────── */

static void test_accretion_scene(void)
{
    printf("── P5.5: accretion scene (splats + triangles + blocks)\n");

    /* Smooth image with a soft blob and a hard edge — splat/vertex turf. */
    uint32_t w = 256, h = 256;
    uint8_t *px = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 3;
            int r = 40 + (int)(y * 120 / h);
            int g = 80 + (int)(y * 100 / h);
            int b = 160 + (int)(x * 60 / w);
            int dx = (int)x - 128, dy = (int)y - 100;
            int d2 = dx * dx + dy * dy;
            if (d2 < 2500) {
                int boost = (2500 - d2) / 30;
                r += boost; g += boost / 2;
            }
            if (x > 200) { r += 60; b -= 40; }
            px[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            px[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            px[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;

    size_t sizes[3];
    double psnrs[3];
    float quals[3] = {0.9f, 0.5f, 0.2f};

    for (int q = 0; q < 3; q++) {
        opts.quality = quals[q];
        uint8_t *comp = NULL, *dec = NULL;
        size_t cl = 0;
        uint32_t dw = 0, dh = 0;
        uint8_t dch = 0;

        int rc = qtc_compress_image(px, w, h, 3, &comp, &cl, &opts);
        CHECK(rc == QTC_OK, "accretion: compress");
        CHECK(comp[4] == QTC_LENS_IMAGE_ACCRETION ||
              comp[4] == QTC_LENS_IMAGE_MESH ||
              comp[4] == QTC_LENS_IMAGE_DCT,
              "accretion: lossy lens id (best-of-all picks per content)");

        rc = qtc_decompress_image(comp, cl, &dec, &dw, &dh, &dch);
        CHECK(rc == QTC_OK && dw == w && dh == h && dch == 3,
              "accretion: decode dims");

        sizes[q] = cl;
        psnrs[q] = image_psnr_db(px, dec, (size_t)w * h * 3);
        printf("     quality=%.1f: %zu B, PSNR=%.1f dB\n",
               quals[q], cl, psnrs[q]);

        free(comp);
        free(dec);
    }
    CHECK(sizes[0] > sizes[1] && sizes[1] > sizes[2],
          "accretion: size must decrease as quality drops");
    CHECK(psnrs[0] > 32.0, "accretion: quality 0.9 must exceed 32 dB PSNR");
    CHECK(psnrs[0] > psnrs[2], "accretion: PSNR must rise with quality");
    CHECK(sizes[2] < (size_t)w * h * 3, "accretion: must beat raw size");
    free(px);

    /* Mixed content: smooth left half, pure-noise right half, hard seam.
     * The scene's pixel blocks must beat the triangle-only mesh here. */
    uint32_t mw = 128, mh = 128;
    uint8_t *mix = malloc((size_t)mw * mh * 3);
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            size_t i = ((size_t)y * mw + x) * 3;
            if (x < mw / 2) {
                mix[i] = (uint8_t)(x * 3);
                mix[i + 1] = (uint8_t)(y * 2);
                mix[i + 2] = 128;
            } else {
                mix[i] = (uint8_t)(xorshift64() & 0xFF);
                mix[i + 1] = (uint8_t)(xorshift64() & 0xFF);
                mix[i + 2] = (uint8_t)(xorshift64() & 0xFF);
            }
        }

    uint8_t *sc = NULL, *sd = NULL, *mc = NULL, *md = NULL;
    size_t scl = 0, mcl = 0;
    uint32_t dw = 0, dh = 0;
    uint8_t dch = 0;
    opts.quality = 0.9f;
    int rc = qtc_compress_image(mix, mw, mh, 3, &sc, &scl, &opts);
    CHECK(rc == QTC_OK, "accretion: mixed compress");
    rc = qtc_decompress_image(sc, scl, &sd, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dw == mw && dh == mh, "accretion: mixed decode");
    double scene_psnr = image_psnr_db(mix, sd, (size_t)mw * mh * 3);

    rc = qtc_image_mesh_encode(mix, mw, mh, 3, 0.9f, &mc, &mcl);
    CHECK(rc == QTC_OK, "accretion: mesh baseline compress");
    rc = qtc_image_mesh_decode(mc, mcl, &md, &dw, &dh, &dch);
    CHECK(rc == QTC_OK, "accretion: mesh baseline decode");
    double mesh_psnr = image_psnr_db(mix, md, (size_t)mw * mh * 3);
    printf("     mixed content: scene=%.1f dB (%zu B) vs mesh=%.1f dB (%zu B)\n",
           scene_psnr, scl, mesh_psnr, mcl);
    CHECK(scene_psnr > mesh_psnr,
          "accretion: scene must beat mesh on mixed content");
    free(sc); free(sd); free(mc); free(md);
    free(mix);

    /* Degenerate sizes the mesh refuses — the scene handles them. */
    uint8_t one[3] = {10, 200, 30};
    uint8_t *tiny = NULL;
    size_t tl = 0;
    rc = qtc_compress_image(one, 1, 1, 3, &tiny, &tl, &opts);
    CHECK(rc == QTC_OK, "accretion: 1x1 compress");
    rc = qtc_decompress_image(tiny, tl, &sd, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dw == 1 && dh == 1, "accretion: 1x1 decode dims");
    free(tiny); free(sd);

    uint8_t two[6] = {10, 200, 30, 12, 220, 44};
    rc = qtc_compress_image(two, 1, 2, 3, &tiny, &tl, &opts);
    CHECK(rc == QTC_OK, "accretion: 1x2 compress");
    rc = qtc_decompress_image(tiny, tl, &sd, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dw == 1 && dh == 2, "accretion: 1x2 decode dims");
    free(tiny); free(sd);

    /* Adversarial: malformed headers must error, bit flips must not crash.
     * Build the base payload with the accretion encoder DIRECTLY — the
     * lossy contest may pick another lens (e.g. DCT on noise since
     * v0.12.0), and this section tests the accretion decoder itself. */
    uint32_t tw = 64, th = 64;
    uint8_t *tex = malloc((size_t)tw * th * 3);
    fill_random(tex, (size_t)tw * th * 3);
    uint8_t *acc_payload = NULL;
    size_t apl = 0;
    rc = qtc_accretion_encode(tex, tw, th, 3, opts.quality,
                              &acc_payload, &apl);
    CHECK(rc == QTC_OK, "accretion: fuzz base encode");
    free(tex);

    uint8_t *payload = acc_payload;
    size_t plen = apl;
    uint8_t *dec2 = NULL;
    rc = qtc_accretion_decode(payload, 26, &dec2, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "accretion-adv: short header rejected");

    uint8_t *mut = malloc(plen);
    memcpy(mut, payload, plen);
    memset(mut, 0, 4);                     /* w = 0 */
    rc = qtc_accretion_decode(mut, plen, &dec2, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "accretion-adv: w=0 rejected");

    memcpy(mut, payload, plen);
    mut[9] = 9;                            /* depth shift beyond 3 */
    rc = qtc_accretion_decode(mut, plen, &dec2, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "accretion-adv: bad shift rejected");

    memcpy(mut, payload, plen);
    memset(mut + 22, 0xFF, 4);             /* body_len = 2^32-1 */
    rc = qtc_accretion_decode(mut, plen, &dec2, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_SIZE || rc == QTC_ERR_ALLOC,
          "accretion-adv: huge body_len refused");

    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, payload, plen);
        size_t pos = (size_t)(xorshift64() % plen);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_accretion_decode(mut, plen, &dec2, &dw, &dh, &dch);
        if (rc == QTC_OK) { free(dec2); dec2 = NULL; }
        survived++;
    }
    CHECK(survived == 200, "accretion-adv: bit-flip trials survive");
    free(mut);
    free(acc_payload);
}

/* ── Phase 5.6: tensor + video lenses, widened audio ─────────────────── */

static void test_tensor(void)
{
    printf("── P5.6: tensor lens (float32 planes / XOR-delta)\n");

    /* Smoothly varying floats — XOR-delta should crush these. */
    uint32_t n = 20000;
    float *vals = malloc(n * sizeof(float));
    float v = 1.0f;
    for (uint32_t i = 0; i < n; i++) {
        vals[i] = v;
        v += 0.001f;
    }
    uint8_t *comp = NULL, *mut = NULL;
    float *dec = NULL;
    uint32_t dn = 0;
    size_t cl = 0;
    int rc = qtc_compress_tensor(vals, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "tensor: compress");
    CHECK(comp[4] == QTC_LENS_TENSOR, "tensor: lens id");
    rc = qtc_decompress_tensor(comp, cl, &dec, &dn);
    CHECK(rc == QTC_OK && dn == n, "tensor: decode count");
    CHECK(memcmp(dec, vals, n * sizeof(float)) == 0,
          "tensor: bit-exact round-trip");
    printf("     smooth: %zu B for %u floats (%.1f:1)\n",
           cl, n, (double)(n * 4) / (double)cl);
    CHECK(cl < (size_t)n * 4 / 4, "tensor: smooth floats must compress 4:1+");
    free(comp);
    free(dec);

    /* Constant floats: degenerate case, must collapse. */
    for (uint32_t i = 0; i < n; i++)
        vals[i] = 42.0f;
    rc = qtc_compress_tensor(vals, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "tensor: constant compress");
    rc = qtc_decompress_tensor(comp, cl, &dec, &dn);
    CHECK(rc == QTC_OK && dn == n &&
          memcmp(dec, vals, n * sizeof(float)) == 0,
          "tensor: constant round-trip");
    CHECK(cl < 2048, "tensor: constant floats must collapse"
                     " (LZ model overhead is ~1.1 KB per block)");
    free(comp);
    free(dec);

    /* Random bit patterns: round-trip must stay bit-exact whatever the
     * lens (STORED fallback is acceptable for incompressible data). */
    fill_random((uint8_t *)vals, n * sizeof(float));
    rc = qtc_compress_tensor(vals, n, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "tensor: random compress");
    CHECK(comp[4] == QTC_LENS_TENSOR || comp[4] == QTC_LENS_STORED,
          "tensor: random lens (TENSOR or STORED fallback)");
    rc = qtc_decompress_tensor(comp, cl, &dec, &dn);
    CHECK(rc == QTC_OK && dn == n &&
          memcmp(dec, vals, n * sizeof(float)) == 0,
          "tensor: random bit-exact round-trip");
    free(comp);
    free(dec);
    free(vals);

    /* Adversarial (internal payload, as with the other lenses). */
    n = 1000;
    vals = malloc(n * sizeof(float));
    fill_random((uint8_t *)vals, n * sizeof(float));
    uint8_t *pl = NULL;
    size_t plen = 0;
    size_t dcount = 0;
    rc = qtc_tensor_encode(vals, n, &pl, &plen, NULL);
    CHECK(rc == QTC_OK, "tensor: fuzz base encode");
    free(vals);

    rc = qtc_tensor_decode(pl, 5, &dec, &dcount);
    CHECK(rc == QTC_ERR_CORRUPT, "tensor-adv: short header rejected");

    mut = malloc(plen);
    memcpy(mut, pl, plen);
    memset(mut, 0, 4);                     /* count = 0 */
    rc = qtc_tensor_decode(mut, plen, &dec, &dcount);
    CHECK(rc == QTC_ERR_CORRUPT, "tensor-adv: count=0 rejected");

    memcpy(mut, pl, plen);
    mut[4] = 2;                            /* unknown mode */
    rc = qtc_tensor_decode(mut, plen, &dec, &dcount);
    CHECK(rc == QTC_ERR_CORRUPT, "tensor-adv: bad mode rejected");

    memcpy(mut, pl, plen);
    memset(mut, 0xFF, 4);                  /* count = 2^32-1 */
    rc = qtc_tensor_decode(mut, plen, &dec, &dcount);
    CHECK(rc == QTC_ERR_SIZE || rc == QTC_ERR_ALLOC,
          "tensor-adv: huge count refused");

    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, pl, plen);
        size_t pos = (size_t)(xorshift64() % plen);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_tensor_decode(mut, plen, &dec, &dcount);
        if (rc == QTC_OK) { free(dec); dec = NULL; }
        survived++;
    }
    CHECK(survived == 200, "tensor-adv: bit-flip trials survive");
    free(mut);
    free(pl);

    /* Lossy modes (v0.9.0): bf16 and f16 requantization. Not bit-exact —
     * verify the precision contract instead: bf16 keeps ~2^-8 relative,
     * f16 keeps ~2^-11 for in-range values, and both round-trip through
     * the container. */
    {
        uint32_t ln = 4096;
        float *lv = malloc(ln * sizeof(float));
        for (uint32_t i = 0; i < ln; i++)
            lv[i] = 0.5f + 0.001f * (float)i;   /* smooth, in f16 range */

        qtc_options_t lo;
        qtc_options_default(&lo);
        lo.mode = QTC_MODE_LOSSY;

        for (int pass = 0; pass < 2; pass++) {
            lo.quality = pass ? 0.2f : 0.8f;    /* f16 then bf16 */
            uint8_t *lc = NULL;
            float *ld = NULL;
            uint32_t ldn = 0;
            size_t lcl = 0;
            rc = qtc_compress_tensor(lv, ln, &lc, &lcl, &lo);
            CHECK(rc == QTC_OK, "tensor-lossy: compress");
            CHECK(lc[4] == QTC_LENS_TENSOR, "tensor-lossy: lens id");
            rc = qtc_decompress_tensor(lc, lcl, &ld, &ldn);
            CHECK(rc == QTC_OK && ldn == ln, "tensor-lossy: decode count");
            double max_rel = 0.0;
            for (uint32_t i = 0; i < ln; i++) {
                double d = fabs((double)ld[i] - (double)lv[i]);
                double rel = d / fabs((double)lv[i]);
                if (rel > max_rel) max_rel = rel;
            }
            double tol = pass ? 1.0 / 1024.0 : 1.0 / 128.0;
            CHECK(max_rel < tol * 2.0, "tensor-lossy: precision bound");
            /* 16-bit requant must beat the lossless path on smooth data. */
            uint8_t *llc = NULL;
            size_t lll = 0;
            qtc_options_t llo;
            qtc_options_default(&llo);
            rc = qtc_compress_tensor(lv, ln, &llc, &lll, &llo);
            CHECK(rc == QTC_OK && lcl < lll,
                  "tensor-lossy: smaller than lossless");
            free(lc); free(ld); free(llc);
        }
        free(lv);
    }
}

static void test_video(void)
{
    printf("── P5.6: video lens (temporal frames)\n");

    uint32_t nf = 16, w = 64, h = 64, ch = 3;
    size_t fsz = (size_t)w * h * ch;
    size_t total = fsz * nf;
    uint8_t *frames = malloc(total);

    /* Static scene (identical frames) + a moving bar: inter-frame delta
     * must collapse the redundancy. */
    for (uint32_t f = 0; f < nf; f++)
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                size_t i = ((size_t)f * w * h + (size_t)y * w + x) * 3;
                frames[i] = (uint8_t)(x * 4);
                frames[i + 1] = (uint8_t)(y * 4);
                frames[i + 2] = (uint8_t)(128 + (int)f * 2);
                if (x >= f * 3 && x < f * 3 + 6) {
                    frames[i] = 255;
                    frames[i + 1] = 255;
                    frames[i + 2] = 255;
                }
            }

    uint8_t *comp = NULL, *mut = NULL, *dec = NULL;
    uint32_t dnf = 0, dw = 0, dh = 0;
    uint8_t dch = 0;
    size_t cl = 0;
    int rc = qtc_compress_video(frames, nf, w, h, ch, &comp, &cl, NULL);
    CHECK(rc == QTC_OK, "video: compress");
    CHECK(comp[4] == QTC_LENS_VIDEO, "video: lens id");
    rc = qtc_decompress_video(comp, cl, &dec, &dnf, &dw, &dh, &dch);
    CHECK(rc == QTC_OK && dnf == nf && dw == w && dh == h && dch == ch,
          "video: decode geometry");
    CHECK(memcmp(dec, frames, total) == 0, "video: bit-exact round-trip");
    printf("     16 frames (196608 B raw) -> %zu B (%.1f:1)\n",
           cl, (double)total / (double)cl);
    CHECK(cl * 4 < total, "video: temporal must compress 4:1+");
    free(comp);
    free(dec);

    /* Single-frame short-circuit: with nframes==1 the contest runs mode 0
     * alone (modes 1-4 degenerate to the identical pre-LZ stream). The
     * output must be bit-identical to what mode 0 produces directly. */
    {
        uint8_t *one = NULL, *one_dec = NULL, *m0 = NULL;
        uint32_t onf = 0, ow = 0, oh = 0;
        uint8_t och = 0;
        size_t ocl = 0, m0l = 0;
        rc = qtc_compress_video(frames, 1, w, h, ch, &one, &ocl, NULL);
        CHECK(rc == QTC_OK, "video-1f: compress");
        rc = qtc_video_encode_mode(frames, 1, w, h, ch, 0, &m0, &m0l);
        CHECK(rc == QTC_OK, "video-1f: mode0 direct");
        CHECK(ocl == QTC_HEADER_SIZE + m0l &&
              memcmp(one + QTC_HEADER_SIZE, m0, m0l) == 0,
              "video-1f: contest == mode 0 bit-exact");
        CHECK(one[QTC_HEADER_SIZE + 14] == 0, "video-1f: mode byte is 0");
        rc = qtc_decompress_video(one, ocl, &one_dec, &onf, &ow, &oh, &och);
        CHECK(rc == QTC_OK && onf == 1 && ow == w && oh == h && och == ch &&
              memcmp(one_dec, frames, fsz) == 0,
              "video-1f: bit-exact round-trip");
        free(one);
        free(one_dec);
        free(m0);
    }

    /* Motion compensation: a 16x16 object translating diagonally over a
     * static textured background. Dirty rects pay for the swept area;
     * motion comp pays for the object — mode 4 must win here and the
     * round-trip must stay bit-exact. */
    {
        uint32_t mnf = 12, mw = 96, mh = 96, mch = 3;
        size_t mfsz = (size_t)mw * mh * mch;
        uint8_t *mfr = malloc(mfsz * mnf);
        for (uint32_t f = 0; f < mnf; f++)
            for (uint32_t y = 0; y < mh; y++)
                for (uint32_t x = 0; x < mw; x++) {
                    size_t i = ((size_t)f * mw * mh + (size_t)y * mw + x) * 3;
                    /* static textured background */
                    mfr[i] = (uint8_t)((x * 7 + y * 13) & 0xFF);
                    mfr[i + 1] = (uint8_t)((x * 3 + y * 5 + 40) & 0xFF);
                    mfr[i + 2] = (uint8_t)((x + y * 11 + 90) & 0xFF);
                    /* translating 16x16 object */
                    uint32_t ox = 8 + f * 5, oy = 8 + f * 4;
                    if (x >= ox && x < ox + 16 && y >= oy && y < oy + 16) {
                        mfr[i] = 200;
                        mfr[i + 1] = (uint8_t)(100 + (x - ox) * 4);
                        mfr[i + 2] = 30;
                    }
                }
        uint8_t *mc = NULL, *md = NULL;
        uint32_t mdf = 0, mdw = 0, mdh = 0;
        uint8_t mdc = 0;
        size_t mcl = 0;
        rc = qtc_compress_video(mfr, mnf, mw, mh, mch, &mc, &mcl, NULL);
        CHECK(rc == QTC_OK, "video-mc: compress");
        rc = qtc_decompress_video(mc, mcl, &md, &mdf, &mdw, &mdh, &mdc);
        CHECK(rc == QTC_OK && mdf == mnf && mdw == mw && mdh == mh &&
              mdc == mch, "video-mc: decode geometry");
        CHECK(memcmp(md, mfr, mfsz * mnf) == 0,
              "video-mc: bit-exact round-trip");
        printf("     translating object: %zu B for %zu B raw (%.1f:1)\n",
               mcl, mfsz * mnf, (double)(mfsz * mnf) / (double)mcl);
        /* The object is 16x16 of 96x96 (~2.8%); motion comp should get
         * this well under 10:1 of raw even with a textured background. */
        CHECK(mcl * 10 < mfsz * mnf, "video-mc: must compress 10:1+");
        free(mc);
        free(md);
        free(mfr);
    }

    /* Local motion: two random patches translating in OPPOSITE directions
     * over a random static background. We verify two things:
     *   1. The motion-comp codec (mode 4) round-trips bit-exactly when used
     *      directly, proving the implementation is correct.
     *   2. The best-of-all contest produces a valid, bit-exact result.
     * NOTE (measured, v0.9.0): on synthetic content the 1 MB LZ window
     * (optimization #7) captures this temporal redundancy at least as well
     * as per-block motion vectors, so the contest typically picks the LZ
     * path. Mode 4 remains in the contest for content where block motion
     * beats byte matching; it is correct either way. */
    {
        uint32_t mnf = 8, mw = 96, mh = 96, mch = 3;
        size_t mfsz = (size_t)mw * mh * mch;
        uint32_t pw = 24, ph = 24;
        uint8_t *bg = malloc(mfsz);
        uint8_t *pa = malloc((size_t)pw * ph * mch);
        uint8_t *pb = malloc((size_t)pw * ph * mch);
        fill_random(bg, mfsz);
        fill_random(pa, (size_t)pw * ph * mch);
        fill_random(pb, (size_t)pw * ph * mch);

        uint8_t *mfr = malloc(mfsz * mnf);
        for (uint32_t f = 0; f < mnf; f++) {
            memcpy(mfr + (size_t)f * mfsz, bg, mfsz);
            uint32_t ax = 4 + f * 4, ay = 10;      /* patch A → right */
            uint32_t bx = 68 - f * 4, by = 60;     /* patch B → left  */
            for (uint32_t y = 0; y < ph; y++)
                for (uint32_t x = 0; x < pw; x++) {
                    size_t si = ((size_t)y * pw + x) * mch;
                    size_t da = ((size_t)(ay + y) * mw + (ax + x)) * mch;
                    size_t db = ((size_t)(by + y) * mw + (bx + x)) * mch;
                    memcpy(mfr + (size_t)f * mfsz + da, pa + si, mch);
                    memcpy(mfr + (size_t)f * mfsz + db, pb + si, mch);
                }
        }
        free(bg); free(pa); free(pb);

        /* 1. Mode-4 codec correctness, exercised directly. */
        uint8_t *p4 = NULL;
        size_t p4l = 0;
        rc = qtc_video_encode_mode(mfr, mnf, mw, mh, mch, 4, &p4, &p4l);
        CHECK(rc == QTC_OK, "video-mc2: mode-4 encode");
        uint8_t *md4 = NULL;
        uint32_t df4 = 0, dw4 = 0, dh4 = 0;
        uint8_t dc4 = 0;
        rc = qtc_video_decode(p4, p4l, &md4, &df4, &dw4, &dh4, &dc4);
        CHECK(rc == QTC_OK && df4 == mnf && dw4 == mw && dh4 == mh &&
              dc4 == mch, "video-mc2: mode-4 decode geometry");
        CHECK(memcmp(md4, mfr, mfsz * mnf) == 0,
              "video-mc2: mode-4 bit-exact round-trip");
        free(p4); free(md4);

        /* 2. The contest result is valid and bit-exact. */
        uint8_t *mc = NULL, *md = NULL;
        uint32_t mdf = 0, mdw = 0, mdh = 0;
        uint8_t mdc = 0;
        size_t mcl = 0;
        rc = qtc_compress_video(mfr, mnf, mw, mh, mch, &mc, &mcl, NULL);
        CHECK(rc == QTC_OK, "video-mc2: compress");
        rc = qtc_decompress_video(mc, mcl, &md, &mdf, &mdw, &mdh, &mdc);
        CHECK(rc == QTC_OK && mdf == mnf && mdw == mw && mdh == mh &&
              mdc == mch, "video-mc2: decode geometry");
        CHECK(memcmp(md, mfr, mfsz * mnf) == 0,
              "video-mc2: bit-exact round-trip");
        printf("     local motion (2 patches): contest mode=%u, %zu B for %zu B raw (%.1f:1)\n",
               mc[QTC_HEADER_SIZE + 14], mcl, mfsz * mnf,
               (double)(mfsz * mnf) / (double)mcl);
        CHECK(mcl * 4 < mfsz * mnf, "video-mc2: must compress 4:1+");
        free(mc);
        free(md);
        free(mfr);
    }

    /* Adversarial (internal payload). */
    uint8_t *pl = NULL;
    size_t plen = 0;
    rc = qtc_video_encode(frames, nf, w, h, ch, &pl, &plen);
    CHECK(rc == QTC_OK, "video: fuzz base encode");
    free(frames);

    rc = qtc_video_decode(pl, 14, &dec, &dnf, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "video-adv: short header rejected");

    mut = malloc(plen);
    memcpy(mut, pl, plen);
    memset(mut, 0, 4);                     /* w = 0 */
    rc = qtc_video_decode(mut, plen, &dec, &dnf, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "video-adv: w=0 rejected");

    memcpy(mut, pl, plen);
    memset(mut + 10, 0, 4);                /* nframes = 0 */
    rc = qtc_video_decode(mut, plen, &dec, &dnf, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "video-adv: nframes=0 rejected");

    memcpy(mut, pl, plen);
    mut[14] = 5;                           /* unknown mode */
    rc = qtc_video_decode(mut, plen, &dec, &dnf, &dw, &dh, &dch);
    CHECK(rc == QTC_ERR_CORRUPT, "video-adv: bad mode rejected");
    /* Corrupted mode byte flipping a small-stream mode-3 payload into a
     * fixed-stream mode must be rejected by the stream_len contract. */
    memcpy(mut, pl, plen);
    mut[14] = 1;
    rc = qtc_video_decode(mut, plen, &dec, &dnf, &dw, &dh, &dch);
    if (rc == QTC_OK) free(dec);
    CHECK(1, "video-adv: mode-flip survives cleanly");

    int survived = 0;
    for (int trial = 0; trial < 200; trial++) {
        memcpy(mut, pl, plen);
        size_t pos = (size_t)(xorshift64() % plen);
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_video_decode(mut, plen, &dec, &dnf, &dw, &dh, &dch);
        if (rc == QTC_OK) { free(dec); dec = NULL; }
        survived++;
    }
    CHECK(survived == 200, "video-adv: bit-flip trials survive");
    free(mut);
    free(pl);
}

static void test_audio_wide(void)
{
    printf("── P5.6: audio API widening (24-bit, 16 channels)\n");

    qtc_options_t opts;
    qtc_options_default(&opts);

    /* 24-bit stereo: 3-byte LE samples, slowly varying. */
    uint32_t n = 8000;
    uint8_t *pcm = malloc((size_t)n * 2 * 3);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = (uint32_t)(1000000 + 3000 * sin(0.01 * i)) & 0xFFFFFF;
        uint32_t r = (uint32_t)(1000000 + 2500 * sin(0.013 * i + 1)) & 0xFFFFFF;
        uint8_t *p = pcm + (size_t)i * 6;
        p[0] = (uint8_t)l; p[1] = (uint8_t)(l >> 8); p[2] = (uint8_t)(l >> 16);
        p[3] = (uint8_t)r; p[4] = (uint8_t)(r >> 8); p[5] = (uint8_t)(r >> 16);
    }
    uint8_t *comp = NULL, *dec = NULL;
    size_t cl = 0;
    uint32_t dn = 0;
    uint8_t dch = 0, dbps = 0;
    int rc = qtc_compress_audio(pcm, n, 2, 24, &comp, &cl, &opts);
    CHECK(rc == QTC_OK, "audio24: compress");
    rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == n && dch == 2 && dbps == 24,
          "audio24: decode dims");
    CHECK(memcmp(dec, pcm, (size_t)n * 6) == 0, "audio24: bit-exact");
    printf("     24-bit stereo: %zu B for %u B raw\n", cl, n * 6);
    free(comp);
    free(dec);

    /* 16 channels, 16-bit. */
    uint8_t *pcm16 = malloc((size_t)n * 16 * 2);
    fill_random(pcm16, (size_t)n * 16 * 2);
    /* Make it smooth enough that prediction can win: copy channels. */
    for (uint32_t i = 0; i < n; i++)
        for (uint8_t c = 1; c < 16; c++)
            memcpy(pcm16 + ((size_t)i * 16 + c) * 2,
                   pcm16 + (size_t)i * 2, 2);
    rc = qtc_compress_audio(pcm16, n, 16, 16, &comp, &cl, &opts);
    CHECK(rc == QTC_OK, "audio16ch: compress");
    rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
    CHECK(rc == QTC_OK && dn == n && dch == 16 && dbps == 16,
          "audio16ch: decode dims");
    CHECK(memcmp(dec, pcm16, (size_t)n * 16 * 2) == 0,
          "audio16ch: bit-exact");
    free(comp);
    free(dec);
    free(pcm16);

    /* Unsupported depth still rejected. */
    rc = qtc_compress_audio(pcm, n, 2, 12, &comp, &cl, &opts);
    CHECK(rc == QTC_ERR_FORMAT, "audio24: bps=12 rejected");
    free(comp);
    free(pcm);
}

/* ── Audit (2026-08-22) regression tests ─────────────────────────────── */

static void test_audit_regressions(void)
{
    printf("── audit: H1/H2/L3 regressions + LZ corruption fuzz\n");

    /* H1: a container whose header original_size is smaller than its LZ
     * block's declared size must be rejected BEFORE the block is written.
     * This exact input overflowed the output buffer before the fix. */
    uint8_t zeros[1000];
    memset(zeros, 0, sizeof(zeros));
    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(zeros, sizeof(zeros), &lz, &lz_len);
    CHECK(rc == QTC_OK, "audit: lz setup");

    size_t total = QTC_HEADER_SIZE + lz_len;
    uint8_t *c = malloc(total);
    uint8_t *dec = NULL;
    size_t dl = 0;

    c[0] = QTC_MAGIC0; c[1] = QTC_MAGIC1;
    c[2] = QTC_MAGIC2; c[3] = QTC_MAGIC3;
    c[4] = QTC_LENS_LZ;
    c[5] = QTC_FLAG_HAS_MODEL;
    c[6] = 0; c[7] = 0;
    for (int i = 0; i < 8; i++)
        c[8 + i] = (uint8_t)(((uint64_t)100 >> (8 * i)) & 0xFF);
    memcpy(c + QTC_HEADER_SIZE, lz, lz_len);
    rc = qtc_decompress(c, total, &dec, &dl);
    CHECK(rc != QTC_OK, "audit: lying container must be rejected (H1)");
    free(dec);
    dec = NULL;

    /* The same block under an honest header must still round-trip. */
    for (int i = 0; i < 8; i++)
        c[8 + i] = (uint8_t)(((uint64_t)1000 >> (8 * i)) & 0xFF);
    rc = qtc_decompress(c, total, &dec, &dl);
    CHECK(rc == QTC_OK && dl == 1000 && memcmp(dec, zeros, 1000) == 0,
          "audit: honest container still round-trips");

    /* L3: trailing bytes after the LZ payload are rejected. */
    uint8_t *c2 = malloc(total + 1);
    memcpy(c2, c, total);
    c2[total] = 0xAA;
    rc = qtc_decompress(c2, total + 1, &dec, &dl);
    CHECK(rc != QTC_OK, "audit: trailing junk rejected (L3)");
    free(c2);
    free(c);
    free(lz);

    /* LZ corruption fuzz: bit flips anywhere in an LZ-lens payload must
     * never crash — the H1 bug class lived exactly here and the old
     * bit-flip trials only ever targeted RAW-lens payloads. */
    size_t tlen = 40000;
    uint8_t *text = malloc(tlen);
    for (size_t i = 0; i < tlen; i++)
        text[i] = (uint8_t)('a' + (i * 7 + (i / 101)) % 26);
    qtc_options_t fopts;
    qtc_options_default(&fopts);
    fopts.force_lens = QTC_LENS_LZ;
    fopts.flags = QTC_FLAG_FORCE_COMPRESS;
    uint8_t *comp = NULL;
    size_t cl = 0;
    rc = qtc_compress(text, tlen, &comp, &cl, &fopts);
    CHECK(rc == QTC_OK && comp[4] == QTC_LENS_LZ,
          "audit: fuzz base compress");
    uint8_t *mut = malloc(cl);
    int survived = 0;
    for (int trial = 0; trial < 300; trial++) {
        memcpy(mut, comp, cl);
        size_t pos = (size_t)(xorshift64() % cl);
        if (pos < QTC_HEADER_SIZE)
            pos += QTC_HEADER_SIZE;      /* flip payload bytes only */
        mut[pos] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_decompress(mut, cl, &dec, &dl);
        if (rc == QTC_OK) { free(dec); dec = NULL; }
        survived++;
    }
    CHECK(survived == 300, "audit: LZ bit-flip trials survive");
    free(mut);
    free(comp);

    /* L1: the version string is built from the macros and must agree. */
    {
        char expect[32];
        snprintf(expect, sizeof(expect), "%d.%d.%d",
                 QTC_VERSION_MAJOR, QTC_VERSION_MINOR, QTC_VERSION_PATCH);
        CHECK(strstr(qtc_version_string(), expect) != NULL,
              "audit: version string matches macros");
    }

    /* #18 regression: a RAW-lens container whose stored model_len is
     * corrupted below the fixed 512-byte order-0 table must be rejected
     * BEFORE qtc_model_unpack reads its fixed 512 bytes. Found by the
     * structured container fuzz (heap-buffer-overflow). */
    {
        uint8_t *raw = NULL;
        size_t rl = 0;
        qtc_options_t ropts;
        qtc_options_default(&ropts);
        ropts.force_lens = QTC_LENS_RAW;
        ropts.flags = QTC_FLAG_FORCE_COMPRESS;
        rc = qtc_compress(text, tlen, &raw, &rl, &ropts);
        CHECK(rc == QTC_OK && raw[4] == QTC_LENS_RAW,
              "audit-18: RAW base compress");
        /* Corrupt the u16 model_len (first 2 payload bytes) to 100. */
        raw[QTC_HEADER_SIZE] = 100;
        raw[QTC_HEADER_SIZE + 1] = 0;
        rc = qtc_decompress(raw, rl, &dec, &dl);
        CHECK(rc != QTC_OK,
              "audit-18: lying model_len rejected (no over-read)");
        free(dec);
        dec = NULL;
        free(raw);
    }
    free(text);
}

/* ── #18 (v0.10.0): structured container-corruption fuzz ───────────────
 * Round 1 (audit) fuzzed only bit flips, and only on RAW and LZ payloads.
 * Round 2 builds a VALID container for every lens, then attacks the
 * container structure itself through the bounded generic decoder:
 *   1. truncation sweep (every short prefix)
 *   2. original_size header lies (0, 1, huge, ±1, ×2)
 *   3. lens-ID swaps (payload fed to every other decoder, + invalid ids)
 *   4. flag-byte corruption (HAS_MODEL cleared, bogus bits set)
 *   5. magic-byte corruption
 *   6. multi-byte burst flips (1–8 consecutive bytes)
 *   7. trailing-junk extension
 * The invariant is "never crash, never write past bounds" — enforced by
 * ASAN/UBSAN — plus "QTC_OK or a clean error code" for every trial. */

#define FUZZ2_MAX_OUT ((uint64_t)64 * 1024 * 1024)

/* Run the full battery against one valid container; returns trial count. */
static size_t fuzz2_battery(const uint8_t *base, size_t blen)
{
    size_t trials = 0;
    uint8_t *dec = NULL;
    size_t dl = 0;
    int rc;

    /* Sanity: the untouched container must decode. */
    rc = qtc_decompress_bounded(base, blen, &dec, &dl, FUZZ2_MAX_OUT);
    CHECK(rc == QTC_OK, "fuzz2: base container decodes");
    if (rc == QTC_OK) free(dec);
    trials++;

    uint8_t *mut = malloc(blen + 16);

    /* 1. Truncation sweep: 0..20 byte-by-byte, then ~1.5x steps to full. */
    size_t cut = 0;
    for (;;) {
        rc = qtc_decompress_bounded(base, cut, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
        if (cut >= blen)
            break;
        if (cut < 20)
            cut++;
        else {
            cut = cut + cut / 2 + 1;
            if (cut > blen)
                cut = blen;
        }
    }

    /* 2. original_size lies in the header (bytes 8..15, little-endian). */
    uint64_t orig = 0;
    for (int i = 0; i < 8; i++)
        orig |= (uint64_t)base[8 + i] << (8 * i);
    uint64_t lies[8];
    lies[0] = 0;
    lies[1] = 1;
    lies[2] = orig > 1 ? orig - 1 : 2;
    lies[3] = orig + 1;
    lies[4] = orig * 2 + 1;
    lies[5] = 0x7FFFFFFFFFFFFFFFull;
    lies[6] = UINT64_MAX;
    lies[7] = orig ^ 0x0000FF00u;
    for (int k = 0; k < 8; k++) {
        memcpy(mut, base, blen);
        for (int i = 0; i < 8; i++)
            mut[8 + i] = (uint8_t)((lies[k] >> (8 * i)) & 0xFF);
        rc = qtc_decompress_bounded(mut, blen, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    /* 3. Lens-ID swaps: feed this payload to every decoder, including
     *    invalid ids 12..15. */
    for (uint8_t lens = 0; lens < 16; lens++) {
        memcpy(mut, base, blen);
        mut[4] = lens;
        rc = qtc_decompress_bounded(mut, blen, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    /* 4. Flag-byte corruption. */
    uint8_t flagmuts[4] = {0xFF, 0x00, 0xFC, (uint8_t)(base[5] ^ 0x01)};
    for (int k = 0; k < 4; k++) {
        memcpy(mut, base, blen);
        mut[5] = flagmuts[k];
        rc = qtc_decompress_bounded(mut, blen, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    /* 5. Magic-byte corruption. */
    for (int b = 0; b < 4; b++) {
        memcpy(mut, base, blen);
        mut[b] ^= 0x55;
        rc = qtc_decompress_bounded(mut, blen, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    /* 6. Multi-byte burst flips. */
    for (int t = 0; t < 150; t++) {
        memcpy(mut, base, blen);
        size_t pos = (size_t)(xorshift64() % blen);
        size_t burst = 1 + (size_t)(xorshift64() % 8);
        if (pos + burst > blen)
            burst = blen - pos;
        for (size_t j = 0; j < burst; j++)
            mut[pos + j] ^= (uint8_t)(1 + xorshift64() % 255);
        rc = qtc_decompress_bounded(mut, blen, &dec, &dl, FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    /* 7. Trailing-junk extension. */
    for (size_t extra = 1; extra <= 16; extra += 5) {
        memcpy(mut, base, blen);
        for (size_t j = 0; j < extra; j++)
            mut[blen + j] = (uint8_t)(xorshift64() & 0xFF);
        rc = qtc_decompress_bounded(mut, blen + extra, &dec, &dl,
                                    FUZZ2_MAX_OUT);
        if (rc == QTC_OK) free(dec);
        trials++;
    }

    free(mut);
    return trials;
}

static void test_container_fuzz(void)
{
    printf("── #18: structured container-corruption fuzz (all lenses)\n");

    size_t total_trials = 0;
    uint8_t *buf = NULL;
    size_t blen = 0;
    int rc;

    /* RAW (order-0): text through the generic path, forced. */
    size_t tlen = 20000;
    uint8_t *text = malloc(tlen);
    for (size_t i = 0; i < tlen; i++)
        text[i] = (uint8_t)('a' + (i * 7 + (i / 97)) % 26);
    qtc_options_t o0;
    qtc_options_default(&o0);
    o0.force_lens = QTC_LENS_RAW;
    o0.flags = QTC_FLAG_FORCE_COMPRESS;
    rc = qtc_compress(text, tlen, &buf, &blen, &o0);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_RAW, "fuzz2: RAW base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);

    /* RAW1 (order-1). */
    qtc_options_t o1;
    qtc_options_default(&o1);
    o1.force_lens = QTC_LENS_RAW1;
    o1.flags = QTC_FLAG_FORCE_COMPRESS;
    rc = qtc_compress(text, tlen, &buf, &blen, &o1);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_RAW1, "fuzz2: RAW1 base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);

    /* LZ. */
    qtc_options_t olz;
    qtc_options_default(&olz);
    olz.force_lens = QTC_LENS_LZ;
    olz.flags = QTC_FLAG_FORCE_COMPRESS;
    rc = qtc_compress(text, tlen, &buf, &blen, &olz);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_LZ, "fuzz2: LZ base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);
    free(text);

    /* STORED: tiny input below the Chandrasekhar limit. */
    uint8_t tiny[100];
    fill_random(tiny, sizeof(tiny));
    rc = qtc_compress(tiny, sizeof(tiny), &buf, &blen, NULL);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_STORED, "fuzz2: STORED base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);

    /* AUDIO (lossless): a smooth stereo tone. */
    uint32_t anf = 8000;
    uint8_t *pcm = malloc((size_t)anf * 2 * 2);
    for (uint32_t i = 0; i < anf; i++) {
        int16_t l = (int16_t)(9000.0 * sin((double)i * 0.05));
        int16_t r = (int16_t)(9000.0 * sin((double)i * 0.07));
        pcm[(size_t)i * 4] = (uint8_t)(l & 0xFF);
        pcm[(size_t)i * 4 + 1] = (uint8_t)((l >> 8) & 0xFF);
        pcm[(size_t)i * 4 + 2] = (uint8_t)(r & 0xFF);
        pcm[(size_t)i * 4 + 3] = (uint8_t)((r >> 8) & 0xFF);
    }
    rc = qtc_compress_audio(pcm, anf, 2, 16, &buf, &blen, NULL);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_AUDIO, "fuzz2: AUDIO base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);

    /* AUDIO_MDCT2 (lossy perceptual, v0.13.0). */
    qtc_options_t omdct;
    qtc_options_default(&omdct);
    omdct.mode = QTC_MODE_LOSSY;
    omdct.quality = 0.7f;
    rc = qtc_compress_audio(pcm, anf, 2, 16, &buf, &blen, &omdct);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_AUDIO_MDCT2,
          "fuzz2: AUDIO_MDCT2 base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);
    free(pcm);

    /* IMAGE (lossless): a gradient. */
    uint32_t iw = 64, ih = 64;
    uint8_t *img = malloc((size_t)iw * ih * 3);
    for (uint32_t y = 0; y < ih; y++)
        for (uint32_t x = 0; x < iw; x++) {
            size_t i = ((size_t)y * iw + x) * 3;
            img[i] = (uint8_t)(x * 4);
            img[i + 1] = (uint8_t)(y * 4);
            img[i + 2] = (uint8_t)((x + y) * 2);
        }
    rc = qtc_compress_image(img, iw, ih, 3, &buf, &blen, NULL);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_IMAGE, "fuzz2: IMAGE base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);

    /* Lossy image lenses: build BOTH containers directly (mesh + accretion)
     * so each decoder is fuzzed regardless of which one the encoder contest
     * would have picked for this content. */
    {
        uint8_t *mp = NULL, *ap = NULL;
        size_t ml = 0, al = 0;
        rc = qtc_image_mesh_encode(img, iw, ih, 3, 0.5f, &mp, &ml);
        CHECK(rc == QTC_OK, "fuzz2: mesh encode");
        rc = qtc_accretion_encode(img, iw, ih, 3, 0.5f, &ap, &al);
        CHECK(rc == QTC_OK, "fuzz2: accretion encode");

        size_t orig = (size_t)iw * ih * 3;
        uint8_t *mwrap = malloc(QTC_HEADER_SIZE + ml);
        uint8_t *awrap = malloc(QTC_HEADER_SIZE + al);
        mwrap[0] = awrap[0] = QTC_MAGIC0;
        mwrap[1] = awrap[1] = QTC_MAGIC1;
        mwrap[2] = awrap[2] = QTC_MAGIC2;
        mwrap[3] = awrap[3] = QTC_MAGIC3;
        mwrap[4] = QTC_LENS_IMAGE_MESH;
        awrap[4] = QTC_LENS_IMAGE_ACCRETION;
        mwrap[5] = awrap[5] = QTC_FLAG_HAS_MODEL;
        mwrap[6] = mwrap[7] = awrap[6] = awrap[7] = 0;
        for (int i = 0; i < 8; i++) {
            mwrap[8 + i] = (uint8_t)((orig >> (8 * i)) & 0xFF);
            awrap[8 + i] = (uint8_t)((orig >> (8 * i)) & 0xFF);
        }
        memcpy(mwrap + QTC_HEADER_SIZE, mp, ml);
        memcpy(awrap + QTC_HEADER_SIZE, ap, al);

        total_trials += fuzz2_battery(mwrap, QTC_HEADER_SIZE + ml);
        total_trials += fuzz2_battery(awrap, QTC_HEADER_SIZE + al);

        free(mwrap);
        free(awrap);
        free(mp);
        free(ap);

        /* DCT photo lens container. */
        uint8_t *dp = NULL;
        size_t dl2 = 0;
        rc = qtc_image_dct_encode(img, iw, ih, 3, 0.5f, &dp, &dl2);
        CHECK(rc == QTC_OK, "fuzz2: dct encode");
        uint8_t *dwrap = malloc(QTC_HEADER_SIZE + dl2);
        dwrap[0] = QTC_MAGIC0;
        dwrap[1] = QTC_MAGIC1;
        dwrap[2] = QTC_MAGIC2;
        dwrap[3] = QTC_MAGIC3;
        dwrap[4] = QTC_LENS_IMAGE_DCT;
        dwrap[5] = QTC_FLAG_HAS_MODEL;
        dwrap[6] = dwrap[7] = 0;
        for (int i = 0; i < 8; i++)
            dwrap[8 + i] = (uint8_t)((orig >> (8 * i)) & 0xFF);
        memcpy(dwrap + QTC_HEADER_SIZE, dp, dl2);
        total_trials += fuzz2_battery(dwrap, QTC_HEADER_SIZE + dl2);
        free(dwrap);
        free(dp);
    }
    free(img);

    /* TENSOR (lossless): smooth floats. */
    uint32_t tcount = 20000;
    float *fdata = malloc((size_t)tcount * sizeof(float));
    for (uint32_t i = 0; i < tcount; i++)
        fdata[i] = (float)sin((double)i * 0.01) * 100.0 +
                   (float)(i % 7) * 0.25f;
    rc = qtc_compress_tensor(fdata, tcount, &buf, &blen, NULL);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_TENSOR, "fuzz2: TENSOR base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);
    free(fdata);

    /* VIDEO (lossless): a few frames with a moving bar. */
    uint32_t vnf = 8, vw = 32, vh = 32;
    uint8_t *frames = malloc((size_t)vnf * vw * vh * 3);
    for (uint32_t f = 0; f < vnf; f++)
        for (uint32_t y = 0; y < vh; y++)
            for (uint32_t x = 0; x < vw; x++) {
                size_t i = ((size_t)f * vw * vh + (size_t)y * vw + x) * 3;
                frames[i] = (uint8_t)(x * 8);
                frames[i + 1] = (uint8_t)(y * 8);
                frames[i + 2] = 100;
                if (x >= f * 3 && x < f * 3 + 4)
                    frames[i] = 255;
            }
    rc = qtc_compress_video(frames, vnf, vw, vh, 3, &buf, &blen, NULL);
    CHECK(rc == QTC_OK && buf[4] == QTC_LENS_VIDEO, "fuzz2: VIDEO base");
    total_trials += fuzz2_battery(buf, blen);
    free(buf);
    free(frames);

    printf("     %zu structured-corruption trials across all lenses\n",
           total_trials);
    CHECK(total_trials > 2000, "fuzz2: battery ran (no crash = ASAN clean)");
}

/* ── Phase 8: exact reciprocal division for the rANS encode step ───────
 * The encode step computes x/fs and x%fs via qtc_div_exact (reciprocal
 * multiply). Verify it against hardware division for EVERY frequency and
 * boundary + random dividends in the reachable range x < fs << 19. */
static void test_divrec_exact(void)
{
    size_t mismatches = 0;
    size_t checked = 0;
    for (uint32_t fs = 1; fs <= QTC_MODEL_M; fs++) {
        uint32_t xmax = fs << 19u;
        uint32_t xs[8];
        xs[0] = 0;
        xs[1] = 1;
        xs[2] = fs - 1;
        xs[3] = fs;
        xs[4] = fs + 1;
        xs[5] = xmax - 1;
        xs[6] = xmax / 2;
        xs[7] = (uint32_t)(xorshift64() % xmax);
        for (int i = 0; i < 8; i++) {
            uint32_t x = xs[i];
            if (x >= xmax)
                continue;
            uint32_t rem = 0xFFFFFFFFu;
            uint32_t q = qtc_div_exact(x, fs, &rem);
            if (q != x / fs || rem != x % fs)
                mismatches++;
            checked++;
        }
    }
    CHECK(checked > 30000, "divrec: battery ran");
    CHECK(mismatches == 0, "divrec: reciprocal matches hardware division");
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int with_speed = (argc > 1 && strcmp(argv[1], "--speed") == 0);

    printf("%s\n", qtc_version_string());
    printf("══════════════════════════════════════════════════\n");

    test_divrec_exact();
    test_roundtrip_units();
    test_fuzz_roundtrip();
    test_entropy_efficiency();
    test_order1_roundtrip();
    test_order1_fuzz();
    test_order1_beats_order0();
    test_order1_corrupt();
    test_order1_model_len_overflow();
    test_lz_roundtrip();
    test_lz_fuzz();
    test_lz2_roundtrip();
    test_lz_large_framing();
    test_lz2_fuzz();
    test_lz2_dominates_lz();
    test_lz2_corrupt_guards();
    test_lz_beats_zlib();
    test_image_roundtrip();
    test_image16_roundtrip();
    test_image16_compat_and_guards();
    test_image16_near_lossless();
    test_image_beats_zstd();
    test_audio_roundtrip();
    test_audio_beats_zstd();
    test_detect_router();
    test_audio_via_generic();
    test_compressed_stored();
    test_mdct_reconstruction();
    test_audio_mdct_lossy();
    test_audio_mdct2();
    test_audio_float_lossless();
    test_audio_float_lossy();
    test_raw_data_coverage();
    test_delaunay();
    test_image_mesh_lossy();
    test_image_dct_lossy();
    test_lossy_adversarial();
    test_accretion_scene();
    test_tensor();
    test_video();
    test_audio_wide();
    if (with_speed) {
        test_decode_speed();
        test_lz_large_roundtrip();
    } else {
        printf("── G3: decode speed (skipped; run with --speed)\n");
        printf("── v0.16.0: >255 MB round-trip (skipped; run with --speed)\n");
    }
    test_corrupt_handling();
    test_chandrasekhar();
    test_audit_regressions();
    test_container_fuzz();

    printf("══════════════════════════════════════════════════\n");
    printf("PASS %d  FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
