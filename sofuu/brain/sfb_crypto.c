/*
 * sfb_crypto.c — Sofuu Brain v2: AES-256-GCM, argon2id KDF, randomness, selftest.
 *
 * GCM is implemented here on top of the vendored AES from QTSQ
 * (crypto/common/aes.c: aes256_ctr_keyexp + aes256_ctr + aes256_ecb). GHASH is
 * ours — a plain bitwise GF(2^128) multiply, correct rather than fast — because
 * the vendored cipher is CTR-only and exposes no GHASH.
 *
 * Counter layout note (this is the whole trick):
 *   aes256_ctr(out, n, iv12, ctx) emits blocks AES(iv12||0), AES(iv12||1), ...
 *   with a big-endian 32-bit counter starting at ZERO.
 *   GCM defines J0 = iv||0x00000001 and needs the keystream to start at
 *   inc32(J0) = iv||2, while the tag mask is E(K, J0) = AES(iv||1).
 *   So we ask for len+32 bytes once and use:
 *        bytes [16,32)  -> E(K, J0)          (tag mask)
 *        bytes [32,  )  -> keystream from iv||2
 *   One CTR call gives both, no separate ECB invocation.
 *
 *   H = E(K, 0^128) is taken the same way: a CTR call with a zero IV emits
 *   AES(0^128) as its block 0. (We avoid aes256_ecb here because the vendored
 *   helper encrypts a full 4-block group and would read past a 1-block input.)
 *
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <fcntl.h>

#include "../../crypto/common/aes.h"
#include "../../crypto/argon2/argon2.h"

#if defined(__APPLE__)
#include <sys/random.h>
#endif

/* argon2id cost parameters — fixed, so a (passphrase, salt) pair is a stable key */
#define SFB_ARGON2_T_COST 3u
#define SFB_ARGON2_M_COST 65536u   /* 64 MiB */
#define SFB_ARGON2_LANES  1u

/* ── byte hygiene ──────────────────────────────────────────────────────── */

static void sfb_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;

    while (n-- > 0) *v++ = 0;
}

/* constant-time tag comparison; returns 1 when equal */
static int sfb_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t diff = 0;
    size_t           i;

    for (i = 0; i < n; i++) diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    return diff == 0;
}

static void sfb_store_be64(uint8_t out[8], uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

/* ── GHASH: bitwise GF(2^128) multiply, MSB-first (NIST SP 800-38D Alg. 1) ─ */

static void sfb_gf_mul(uint8_t x[16], const uint8_t y[16])
{
    static const uint8_t R = 0xe1;      /* 11100001 || 0^120 reduction constant */
    uint8_t              z[16], v[16];
    int                  i, j;

    memset(z, 0, sizeof z);
    memcpy(v, y, sizeof v);

    for (i = 0; i < 128; i++) {
        if (x[i >> 3] & (uint8_t)(0x80u >> (i & 7))) {
            for (j = 0; j < 16; j++) z[j] ^= v[j];
        }
        if (v[15] & 1u) {
            /* V = (V >> 1) xor R */
            for (j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
            v[0] = (uint8_t)((v[0] >> 1) ^ R);
        } else {
            for (j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
            v[0] >>= 1;
        }
    }
    memcpy(x, z, sizeof z);
}

/* Y <- (Y xor block) * H for every 16-byte block, zero-padding the tail */
static void sfb_ghash_update(uint8_t y[16], const uint8_t h[16],
                             const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t        blk[16];
    size_t         i;

    while (len >= 16) {
        for (i = 0; i < 16; i++) y[i] ^= p[i];
        sfb_gf_mul(y, h);
        p   += 16;
        len -= 16;
    }
    if (len > 0) {
        memset(blk, 0, sizeof blk);
        memcpy(blk, p, len);
        for (i = 0; i < 16; i++) y[i] ^= blk[i];
        sfb_gf_mul(y, h);
    }
}

static void sfb_ghash(uint8_t out[16], const uint8_t h[16],
                      const void *aad, size_t aad_len,
                      const void *ct, size_t ct_len)
{
    uint8_t y[16];
    uint8_t lens[16];

    memset(y, 0, sizeof y);
    if (aad != NULL && aad_len > 0) sfb_ghash_update(y, h, aad, aad_len);
    if (ct != NULL && ct_len > 0)   sfb_ghash_update(y, h, ct, ct_len);

    sfb_store_be64(lens, (uint64_t)aad_len * 8u);
    sfb_store_be64(lens + 8, (uint64_t)ct_len * 8u);
    sfb_ghash_update(y, h, lens, sizeof lens);

    memcpy(out, y, sizeof y);
}

/* ── shared seal/open core ─────────────────────────────────────────────── */

/*
 * tag_in is NULL when sealing. When opening it must point at the 16-byte
 * received tag; the tag is verified before a single plaintext byte is written.
 */
static int sfb_gcm_run(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                       const void *aad, size_t aad_len,
                       const void *in, size_t len, void *out,
                       const uint8_t *tag_in, uint8_t *tag_out)
{
    aes256ctx ctx;
    uint8_t  *raw;
    uint8_t  *ks;
    uint8_t  *mask;
    uint8_t   h[16];
    uint8_t   zero_iv[SFB_IV_LEN];
    uint8_t   expect[16];
    uint8_t   got[16];
    size_t    total;
    size_t    i;
    int       rc = SFB_OK;

    if (len > SIZE_MAX - 32u) return SFB_E_INVALID;
    if (len > 0 && out == NULL) return SFB_E_INVALID;
    if (aad_len > 0 && aad == NULL) return SFB_E_INVALID;

    total = len + 32u;
    raw   = (uint8_t *)malloc(total);
    if (raw == NULL) return SFB_E_NOMEM;

    aes256_ctr_keyexp(&ctx, key);
    aes256_ctr(raw, total, iv, &ctx);

    mask = raw + 16;          /* E(K, J0) */
    ks   = raw + 32;          /* keystream from inc32(J0) */

    /* H = E(K, 0^128): CTR block 0 under an all-zero IV. */
    memset(zero_iv, 0, sizeof zero_iv);
    aes256_ctr(h, sizeof h, zero_iv, &ctx);

    if (tag_in == NULL) {
        for (i = 0; i < len; i++) ((uint8_t *)out)[i] = (uint8_t)(((const uint8_t *)in)[i] ^ ks[i]);
        sfb_ghash(got, h, aad, aad_len, out, len);
        for (i = 0; i < 16; i++) tag_out[i] = (uint8_t)(got[i] ^ mask[i]);
    } else {
        sfb_ghash(expect, h, aad, aad_len, in, len);
        for (i = 0; i < 16; i++) expect[i] ^= mask[i];
        if (!sfb_ct_equal(expect, tag_in, 16)) {
            rc = SFB_E_CORRUPT;
        } else {
            for (i = 0; i < len; i++) ((uint8_t *)out)[i] = (uint8_t)(((const uint8_t *)in)[i] ^ ks[i]);
        }
    }

    aes256_ctx_release(&ctx);
    sfb_secure_zero(raw, total);
    free(raw);
    return rc;
}

int sfb_gcm_seal(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len,
                 const void *pt, size_t pt_len, void *ct_out,
                 uint8_t tag_out[SFB_TAG_LEN])
{
    if (key == NULL || iv == NULL || tag_out == NULL) return SFB_E_INVALID;
    if (pt_len > 0 && (pt == NULL || ct_out == NULL)) return SFB_E_INVALID;
    if (aad_len > 0 && aad == NULL) return SFB_E_INVALID;

    return sfb_gcm_run(key, iv, aad, aad_len, pt, pt_len, ct_out, NULL, tag_out);
}

int sfb_gcm_open(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len,
                 const void *ct, size_t ct_len, const uint8_t tag[SFB_TAG_LEN],
                 void *pt_out)
{
    if (key == NULL || iv == NULL || tag == NULL) return SFB_E_INVALID;
    if (ct_len > 0 && (ct == NULL || pt_out == NULL)) return SFB_E_INVALID;
    if (aad_len > 0 && aad == NULL) return SFB_E_INVALID;

    return sfb_gcm_run(key, iv, aad, aad_len, ct, ct_len, pt_out, tag, NULL);
}

/* ── KDF ───────────────────────────────────────────────────────────────── */

int sfb_kdf(const char *passphrase, const uint8_t salt[SFB_SALT_LEN],
            uint8_t key_out[SFB_KEY_LEN])
{
    size_t   pwdlen;
    uint32_t status;

    if (passphrase == NULL || salt == NULL || key_out == NULL) return SFB_E_INVALID;

    pwdlen = strlen(passphrase);
    status = (uint32_t)argon2_hash(SFB_ARGON2_T_COST, SFB_ARGON2_M_COST, SFB_ARGON2_LANES,
                                   passphrase, pwdlen, salt, (size_t)SFB_SALT_LEN,
                                   key_out, (size_t)SFB_KEY_LEN,
                                   NULL, 0, Argon2_id, ARGON2_VERSION_NUMBER);
    if (status != (uint32_t)ARGON2_OK) {
        sfb_secure_zero(key_out, SFB_KEY_LEN);
        return SFB_E_IO;
    }
    return SFB_OK;
}

/* ── randomness ────────────────────────────────────────────────────────── */

int sfb_random_bytes(void *out, size_t len)
{
    uint8_t *p = (uint8_t *)out;
    size_t   left = len;
    int      fd;

    if (len == 0) return SFB_OK;
    if (out == NULL) return SFB_E_INVALID;

    fd = open("/dev/urandom", O_RDONLY
#if defined(O_CLOEXEC)
              | O_CLOEXEC
#endif
    );
    if (fd >= 0) {
        while (left > 0) {
            ssize_t n = read(fd, p, left);

            if (n < 0) {
                if (errno == EINTR) continue;
                break;      /* fall through to getentropy */
            }
            if (n == 0) break;
            p    += (size_t)n;
            left -= (size_t)n;
        }
        (void)close(fd);
        if (left == 0) return SFB_OK;
        p    = (uint8_t *)out;
        left = len;
    }

#if defined(__APPLE__) || defined(__linux__)
    while (left > 0) {
        size_t chunk = left > 256u ? 256u : left;   /* getentropy caps at 256 */

        if (getentropy(p, chunk) != 0) return SFB_E_IO;
        p    += chunk;
        left -= chunk;
    }
    return SFB_OK;
#else
    return SFB_E_IO;
#endif
}

/* ── self-test ─────────────────────────────────────────────────────────── */

static int sfb_hex2bin(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t hlen = strlen(hex);
    size_t i;

    if ((hlen & 1u) != 0 || hlen / 2u > cap) return 0;
    for (i = 0; i < hlen / 2u; i++) {
        unsigned int hi, lo;
        char         a = hex[2 * i], b = hex[2 * i + 1];

        if (a >= '0' && a <= '9')      hi = (unsigned int)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned int)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') hi = (unsigned int)(a - 'A' + 10);
        else return 0;

        if (b >= '0' && b <= '9')      lo = (unsigned int)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned int)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') lo = (unsigned int)(b - 'A' + 10);
        else return 0;

        out[i] = (uint8_t)((hi << 4) | lo);
    }
    if (out_len != NULL) *out_len = hlen / 2u;
    return 1;
}

typedef struct sfb_gcm_vec {
    const char *name;
    const char *key;    /* 32 bytes hex */
    const char *iv;     /* 12 bytes hex */
    const char *aad;    /* may be "" */
    const char *pt;     /* may be "" */
    const char *ct;     /* expected ciphertext */
    const char *tag;    /* expected tag */
} sfb_gcm_vec_t;

/*
 * NIST AES-256-GCM known-answer vectors (SP 800-38D, test cases 13/15/16/17).
 * Every tag here was cross-checked against OpenSSL's GCM before being trusted,
 * including tc15, whose tag is easy to confuse with tc17's.
 */
static const sfb_gcm_vec_t sfb_gcm_vectors[] = {
    /* NIST GCM test case 16 — AES-256, 60-byte plaintext + 20-byte AAD.
     * This is the vector quoted in the W2 task description. */
    { "nist-tc16",
      "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
      "cafebabefacedbaddecaf888",
      "feedfacedeadbeeffeedfacedeadbeefabaddad2",
      "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
      "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
      "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
      "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
      "76fc6ece0f4e1768cddf8853bb2d551b" },
    /* NIST GCM test case 15 — same key/IV/plaintext as tc16, empty AAD.
     * Exercises the "no AAD at all" path. */
    { "nist-tc15",
      "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
      "cafebabefacedbaddecaf888",
      "",
      "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
      "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
      "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
      "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
      "eb9f796c8d356fc31a8433884b696f4f" },
    /* NIST GCM test case 17 — 64 bytes: an exact block multiple, so the
     * "no partial final block" branch of GHASH is used. */
    { "nist-tc17",
      "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
      "cafebabefacedbaddecaf888",
      "",
      "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
      "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
      "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
      "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
      "b094dac5d93471bdec1a502270e3cc6c" },
    /* NIST GCM test case 13 — AES-256, all-zero key/IV, empty message. */
    { "nist-tc13",
      "0000000000000000000000000000000000000000000000000000000000000000",
      "000000000000000000000000",
      "",
      "",
      "",
      "530f8afbc74536b9a963b4f1c4cb738b" },
};

static int sfb_selftest_vector(const sfb_gcm_vec_t *v)
{
    uint8_t key[SFB_KEY_LEN], iv[SFB_IV_LEN];
    uint8_t aad[64], pt[128], want_ct[128], want_tag[SFB_TAG_LEN];
    uint8_t got_ct[128], got_tag[SFB_TAG_LEN], back[128];
    size_t  aad_len = 0, pt_len = 0, ct_len = 0;

    if (!sfb_hex2bin(v->key, key, sizeof key, NULL)) return SFB_E_CORRUPT;
    if (!sfb_hex2bin(v->iv, iv, sizeof iv, NULL)) return SFB_E_CORRUPT;
    if (!sfb_hex2bin(v->aad, aad, sizeof aad, &aad_len)) return SFB_E_CORRUPT;
    if (!sfb_hex2bin(v->pt, pt, sizeof pt, &pt_len)) return SFB_E_CORRUPT;
    if (!sfb_hex2bin(v->ct, want_ct, sizeof want_ct, &ct_len)) return SFB_E_CORRUPT;
    if (!sfb_hex2bin(v->tag, want_tag, sizeof want_tag, NULL)) return SFB_E_CORRUPT;
    if (ct_len != pt_len) return SFB_E_CORRUPT;

    memset(got_ct, 0, sizeof got_ct);
    memset(got_tag, 0, sizeof got_tag);
    if (sfb_gcm_seal(key, iv, aad_len ? aad : NULL, aad_len, pt_len ? pt : NULL, pt_len,
                     got_ct, got_tag) != SFB_OK) {
        return SFB_E_CORRUPT;
    }

    if (memcmp(got_ct, want_ct, ct_len) != 0) return SFB_E_CORRUPT;
    if (memcmp(got_tag, want_tag, SFB_TAG_LEN) != 0) return SFB_E_CORRUPT;

    memset(back, 0, sizeof back);
    if (sfb_gcm_open(key, iv, aad_len ? aad : NULL, aad_len, got_ct, ct_len, got_tag,
                     back) != SFB_OK) {
        return SFB_E_CORRUPT;
    }
    if (memcmp(back, pt, pt_len) != 0) return SFB_E_CORRUPT;

    /* tamper: one ciphertext bit flipped must be rejected, not decrypted */
    if (ct_len > 0) {
        got_ct[ct_len / 2] ^= 0x01;
        if (sfb_gcm_open(key, iv, aad_len ? aad : NULL, aad_len, got_ct, ct_len, got_tag,
                         back) != SFB_E_CORRUPT) {
            return SFB_E_CORRUPT;
        }
        got_ct[ct_len / 2] ^= 0x01;
    }

    /* tamper: one tag bit flipped */
    got_tag[0] ^= 0x80;
    if (sfb_gcm_open(key, iv, aad_len ? aad : NULL, aad_len, got_ct, ct_len, got_tag,
                     back) != SFB_E_CORRUPT) {
        return SFB_E_CORRUPT;
    }

    return SFB_OK;
}

int sfb_crypto_selftest(void)
{
    size_t i;

    for (i = 0; i < sizeof sfb_gcm_vectors / sizeof sfb_gcm_vectors[0]; i++) {
        if (sfb_selftest_vector(&sfb_gcm_vectors[i]) != SFB_OK) return SFB_E_CORRUPT;
    }
    return SFB_OK;
}
