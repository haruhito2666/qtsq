/*
 * test_dims.c — exact dimension enforcement (W1.3 / W6.5)
 *
 * The plain entry points cannot see a caller's buffer length, so a short buffer
 * is undefined behaviour. The length-carrying variants must refuse anything but
 * exactly cfg.dim, with SFB_E_DIM, on both write and query — this suite proves
 * that and pins the 64..4096 bounds (W5.6).
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

static uint64_t rs = 1234567;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0; }

static int open_store(const char *path, uint32_t dim, sfb_brain_t **out)
{
    sfb_config_t cfg;
    sfb_config_default(&cfg, path, dim);
    return sfb_open(&cfg, out);
}

int main(void)
{
    const uint32_t dim = 128;
    const char *dir = "/tmp/sfb_test_dims";
    char cmd[512];
    float v[256];
    sfb_brain_t *b = NULL;
    int rc;

    printf("── exact dimension enforcement\n");
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }

    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        v[i] = (float)(rnd() * 2.0 - 1.0);

    rc = open_store(dir, dim, &b);
    CK_MSG(rc == SFB_OK, "open dim=%u rc=%d (%s)", dim, rc, sfb_strerror(rc));
    if (rc != SFB_OK)
        return 1;

    /* ── writes ────────────────────────────────────────────────────────── */
    sfb_remember_result_t rr;
    CK_MSG(sfb_remember_ex(b, v, dim, "exact", "user", &rr) == SFB_OK,
           "exact-length remember rejected");
    CK(rr.merged == 0);

    CK_MSG(sfb_remember_ex(b, v, dim - 1, "short", "user", &rr) == SFB_E_DIM,
           "one float short was accepted");
    CK_MSG(sfb_remember_ex(b, v, dim + 1, "long", "user", &rr) == SFB_E_DIM,
           "one float long was accepted");
    CK_MSG(sfb_remember_ex(b, v, 0, "empty", "user", &rr) == SFB_E_DIM,
           "a zero-length vector was accepted");
    CK_MSG(sfb_remember_ex(b, v, SIZE_MAX, "absurd", "user", &rr) == SFB_E_DIM,
           "SIZE_MAX was accepted");

    /* a rejected write must not have created a record */
    {
        uint64_t live = 0;
        CK(sfb_count(b, &live) == SFB_OK);
        CK_MSG(live == 1, "rejected writes still produced %llu records",
               (unsigned long long)live);
    }

    /* the finite check still runs through the checked path */
    {
        float bad[128];
        memcpy(bad, v, sizeof bad);
        bad[7] = (float)NAN;
        CK_MSG(sfb_remember_ex(b, bad, dim, "nan", "user", &rr) == SFB_E_NONFINITE,
               "NaN slipped through the length-checked path");
        bad[7] = (float)INFINITY;
        CK_MSG(sfb_remember_ex(b, bad, dim, "inf", "user", &rr) == SFB_E_NONFINITE,
               "Inf slipped through the length-checked path");
    }

    /* ── queries ───────────────────────────────────────────────────────── */
    {
        sfb_hit_t hits[5];
        uint32_t n = 0;

        CK_MSG(sfb_recall_ex(b, v, dim, 3, hits, &n) == SFB_OK, "exact query rejected");
        CK_MSG(n >= 1 && hits[0].text && strcmp(hits[0].text, "exact") == 0,
               "exact query did not return the stored record (n=%u)", n);

        n = 0;
        rc = sfb_recall_ex(b, v, dim - 1, 3, hits, &n);
        CK_MSG(rc == SFB_E_DIM, "short query returned rc=%d, expected SFB_E_DIM", rc);
        CK_MSG(n == 0, "short query produced %u hits", n);

        n = 0;
        rc = sfb_recall_ex(b, v, 4096, 3, hits, &n);
        CK_MSG(rc == SFB_E_DIM, "over-long query returned rc=%d", rc);
        CK_MSG(n == 0, "over-long query produced %u hits", n);

        /* the unchecked shim still behaves as before (source compatibility) */
        n = 0;
        CK(sfb_recall(b, v, 3, hits, &n) == SFB_OK);
        CK(n >= 1);
    }

    sfb_flush_result_t fr;
    CK(sfb_flush(b, &fr) == SFB_OK);
    sfb_close(b);

    /* ── bounds 64..4096 (W5.6) ────────────────────────────────────────── */
    {
        const struct { uint32_t dim; int ok; } cases[] = {
            { 63u, 0 }, { 64u, 1 }, { 4096u, 1 }, { 4097u, 0 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char p[256];
            sfb_brain_t *t = NULL;
            snprintf(p, sizeof p, "%s/d%u", dir, cases[i].dim);
            rc = open_store(p, cases[i].dim, &t);
            if (cases[i].ok)
                CK_MSG(rc == SFB_OK, "dim %u should be accepted, rc=%d (%s)",
                       cases[i].dim, rc, sfb_strerror(rc));
            else
                CK_MSG(rc == SFB_E_DIM, "dim %u should be rejected, rc=%d",
                       cases[i].dim, rc);
            if (rc == SFB_OK && t != NULL)
                sfb_close(t);
        }
    }

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }

    printf("test_dims: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_DIMS FAILED\n"); return 1; }
    printf("=== TEST_DIMS PASSED ===\n");
    return 0;
}
