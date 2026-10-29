/*
 * tests/test_crypto.c — AES-256-GCM / argon2id tests for Sofuu Brain v2.
 *
 * Build:  cc ... sfb_crypto.c tests/test_crypto.c -L../.. -lqtsq -o /tmp/sfb_test_crypto
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <time.h>

static int g_fail;
static int g_pass;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  [ok]   %s\n", what);
        g_pass++;
    } else {
        printf("  [FAIL] %s\n", what);
        g_fail++;
    }
}

static const char *rc_name(int rc)
{
    switch (rc) {
    case SFB_OK:         return "SFB_OK";
    case SFB_E_INVALID:  return "SFB_E_INVALID";
    case SFB_E_IO:       return "SFB_E_IO";
    case SFB_E_NOMEM:    return "SFB_E_NOMEM";
    case SFB_E_CORRUPT:  return "SFB_E_CORRUPT";
    default:             return "other";
    }
}

/* ── selftest ──────────────────────────────────────────────────────────── */

static void test_selftest(void)
{
    int rc;

    printf("NIST GCM known-answer self-test\n");
    rc = sfb_crypto_selftest();
    if (rc != SFB_OK) printf("         (selftest returned %d %s)\n", rc, rc_name(rc));
    check(rc == SFB_OK, "sfb_crypto_selftest() passes 4 NIST vectors + tamper cases");
}

/* ── 1 MiB round trip ──────────────────────────────────────────────────── */

#define BIG_LEN (1024u * 1024u)

static void test_roundtrip_1mb(void)
{
    uint8_t  key[SFB_KEY_LEN];
    uint8_t  iv[SFB_IV_LEN];
    uint8_t  tag[SFB_TAG_LEN];
    uint8_t *pt = NULL;
    uint8_t *ct = NULL;
    uint8_t *back = NULL;
    int      rc;

    printf("1 MiB seal/open round trip\n");

    pt   = (uint8_t *)malloc(BIG_LEN);
    ct   = (uint8_t *)malloc(BIG_LEN);
    back = (uint8_t *)calloc(1, BIG_LEN);
    if (pt == NULL || ct == NULL || back == NULL) {
        check(0, "allocate 1 MiB buffers");
        free(pt);
        free(ct);
        free(back);
        return;
    }

    check(sfb_random_bytes(key, sizeof key) == SFB_OK, "key from sfb_random_bytes");
    check(sfb_random_bytes(iv, sizeof iv) == SFB_OK, "iv from sfb_random_bytes");
    check(sfb_random_bytes(pt, BIG_LEN) == SFB_OK, "1 MiB plaintext from sfb_random_bytes");

    rc = sfb_gcm_seal(key, iv, "sfb-segment-header", 18, pt, BIG_LEN, ct, tag);
    check(rc == SFB_OK, "seal 1 MiB");

    rc = sfb_gcm_open(key, iv, "sfb-segment-header", 18, ct, BIG_LEN, tag, back);
    check(rc == SFB_OK, "open 1 MiB");

    check(memcmp(pt, back, BIG_LEN) == 0, "round-tripped plaintext is byte-identical");
    check(memcmp(pt, ct, BIG_LEN) != 0, "ciphertext differs from plaintext");

    free(pt);
    free(ct);
    free(back);
}

/* ── empty message edge case ───────────────────────────────────────────── */

static void test_empty_message(void)
{
    uint8_t key[SFB_KEY_LEN];
    uint8_t iv[SFB_IV_LEN] = { 0 };
    uint8_t tag[SFB_TAG_LEN];
    uint8_t tag2[SFB_TAG_LEN];
    int     rc;

    printf("empty plaintext / empty AAD\n");
    memset(key, 0x11, sizeof key);

    rc = sfb_gcm_seal(key, iv, NULL, 0, NULL, 0, NULL, tag);
    check(rc == SFB_OK, "seal of zero-length message succeeds");

    rc = sfb_gcm_seal(key, iv, NULL, 0, NULL, 0, NULL, tag2);
    check(rc == SFB_OK && memcmp(tag, tag2, SFB_TAG_LEN) == 0, "tag is deterministic");

    rc = sfb_gcm_open(key, iv, NULL, 0, NULL, 0, tag, NULL);
    check(rc == SFB_OK, "open of zero-length message succeeds");

    tag2[SFB_TAG_LEN - 1] ^= 0x01;
    rc = sfb_gcm_open(key, iv, NULL, 0, NULL, 0, tag2, NULL);
    check(rc == SFB_E_CORRUPT, "zero-length message with bad tag is rejected");
}

/* ── tamper detection ──────────────────────────────────────────────────── */

static void test_tamper(void)
{
    uint8_t  key[SFB_KEY_LEN];
    uint8_t  iv[SFB_IV_LEN];
    uint8_t  tag[SFB_TAG_LEN];
    uint8_t  bad_tag[SFB_TAG_LEN];
    char     aad[] = "gen:7|seq:3|dim:768";
    char     bad_aad[sizeof aad];
    uint8_t  pt[256];
    uint8_t  ct[256];
    uint8_t  back[256];
    uint8_t  alt_key[SFB_KEY_LEN];
    int      rc;
    size_t   aad_len = sizeof aad - 1;

    printf("tamper detection\n");
    memset(key, 0x2a, sizeof key);
    memset(alt_key, 0x2b, sizeof alt_key);
    memset(iv, 0x5c, sizeof iv);
    memset(pt, 0x77, sizeof pt);
    memset(back, 0, sizeof back);
    memcpy(bad_aad, aad, sizeof aad);

    rc = sfb_gcm_seal(key, iv, aad, aad_len, pt, sizeof pt, ct, tag);
    check(rc == SFB_OK, "seal reference message");

    /* baseline */
    rc = sfb_gcm_open(key, iv, aad, aad_len, ct, sizeof ct, tag, back);
    check(rc == SFB_OK && memcmp(back, pt, sizeof pt) == 0, "reference open succeeds");

    /* one ciphertext bit */
    ct[100] ^= 0x01;
    rc = sfb_gcm_open(key, iv, aad, aad_len, ct, sizeof ct, tag, back);
    check(rc == SFB_E_CORRUPT, "flipped ciphertext bit -> SFB_E_CORRUPT");
    ct[100] ^= 0x01;

    /* one AAD bit */
    bad_aad[3] ^= 0x01;
    rc = sfb_gcm_open(key, iv, bad_aad, aad_len, ct, sizeof ct, tag, back);
    check(rc == SFB_E_CORRUPT, "flipped AAD bit -> SFB_E_CORRUPT");
    memcpy(bad_aad, aad, sizeof aad);

    /* one tag bit */
    memcpy(bad_tag, tag, sizeof tag);
    bad_tag[8] ^= 0x40;
    rc = sfb_gcm_open(key, iv, aad, aad_len, ct, sizeof ct, bad_tag, back);
    check(rc == SFB_E_CORRUPT, "flipped tag bit -> SFB_E_CORRUPT");

    /* wrong key */
    rc = sfb_gcm_open(alt_key, iv, aad, aad_len, ct, sizeof ct, tag, back);
    check(rc == SFB_E_CORRUPT, "wrong key -> SFB_E_CORRUPT");

    /* wrong IV */
    {
        uint8_t bad_iv[SFB_IV_LEN];

        memcpy(bad_iv, iv, sizeof iv);
        bad_iv[0] ^= 0x01;
        rc = sfb_gcm_open(key, bad_iv, aad, aad_len, ct, sizeof ct, tag, back);
        check(rc == SFB_E_CORRUPT, "wrong IV -> SFB_E_CORRUPT");
    }

    /* an accepted message still round-trips afterwards */
    rc = sfb_gcm_open(key, iv, aad, aad_len, ct, sizeof ct, tag, back);
    check(rc == SFB_OK && memcmp(back, pt, sizeof pt) == 0,
          "untampered message still opens after rejections");
}

/* ── KDF ───────────────────────────────────────────────────────────────── */

static void test_kdf(void)
{
    uint8_t salt_a[SFB_SALT_LEN];
    uint8_t salt_b[SFB_SALT_LEN];
    uint8_t k1[SFB_KEY_LEN];
    uint8_t k2[SFB_KEY_LEN];
    uint8_t k3[SFB_KEY_LEN];
    clock_t t0, t1;
    int     rc;

    printf("argon2id KDF determinism\n");
    memset(salt_a, 0x01, sizeof salt_a);
    memset(salt_b, 0x02, sizeof salt_b);

    t0 = clock();
    rc = sfb_kdf("correct horse battery staple", salt_a, k1);
    check(rc == SFB_OK, "sfb_kdf() returns SFB_OK");

    rc = sfb_kdf("correct horse battery staple", salt_a, k2);
    check(rc == SFB_OK, "second sfb_kdf() with the same inputs returns SFB_OK");

    rc = sfb_kdf("correct horse battery staple", salt_b, k3);
    check(rc == SFB_OK, "sfb_kdf() with a different salt returns SFB_OK");
    t1 = clock();
    printf("         (3 x argon2id t=3 m=64MiB p=1 took %.2f s)\n",
           (double)(t1 - t0) / (double)CLOCKS_PER_SEC);

    check(memcmp(k1, k2, SFB_KEY_LEN) == 0, "same passphrase+salt -> identical key");
    check(memcmp(k1, k3, SFB_KEY_LEN) != 0, "different salt -> different key");

    /* a key derived from a passphrase is actually usable for GCM */
    {
        uint8_t iv[SFB_IV_LEN] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
        uint8_t pt[32], ct[32], back[32], tag[SFB_TAG_LEN];

        memset(pt, 0x33, sizeof pt);
        rc = sfb_gcm_seal(k1, iv, NULL, 0, pt, sizeof pt, ct, tag);
        check(rc == SFB_OK, "seal with a KDF-derived key");
        rc = sfb_gcm_open(k1, iv, NULL, 0, ct, sizeof ct, tag, back);
        check(rc == SFB_OK && memcmp(back, pt, sizeof pt) == 0,
              "open with the same KDF-derived key");
    }
}

/* ── randomness ────────────────────────────────────────────────────────── */

static void test_random_bytes(void)
{
    uint8_t a[64], b[64], c[4096];
    size_t  i;
    int     all_zero = 1;
    int     rc;

    printf("sfb_random_bytes sanity\n");

    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    rc = sfb_random_bytes(a, sizeof a);
    check(rc == SFB_OK, "fills 64 bytes");
    rc = sfb_random_bytes(b, sizeof b);
    check(rc == SFB_OK, "fills another 64 bytes");

    for (i = 0; i < sizeof a; i++) if (a[i] != 0) { all_zero = 0; break; }
    check(!all_zero, "output is not all zero");
    check(memcmp(a, b, sizeof a) != 0, "two consecutive calls differ");
    check(sfb_random_bytes(c, sizeof c) == SFB_OK, "fills 4096 bytes");
    check(sfb_random_bytes(a, 0) == SFB_OK, "zero-length request succeeds");
    check(sfb_random_bytes(NULL, 0) == SFB_OK, "NULL with zero length succeeds");
    check(sfb_random_bytes(NULL, 8) == SFB_E_INVALID, "NULL with nonzero length is rejected");
}

int main(void)
{
    printf("── sfb_crypto tests\n");

    test_selftest();
    test_empty_message();
    test_roundtrip_1mb();
    test_tamper();
    test_kdf();
    test_random_bytes();

    printf("── %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("CRYPTO TESTS FAILED\n");
        return 1;
    }
    printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
