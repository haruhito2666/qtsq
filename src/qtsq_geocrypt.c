/*
 * Quantum Tensor Sequence — Geometric Cryptography Engine
 *
 * Encrypts geometric primitives (splat/triangle parameters) before
 * packing to the singularity payload. The image never exists as pixels
 * inside the encrypted container — only as scrambled geometry.
 *
 * This is novel: every other encrypted image format encrypts pixels.
 * QTSQ encrypts positions, colors, spreads, and render order of
 * geometric primitives directly.
 *
 * Author: Haruhito
 */

#include "qtsq_geocrypt.h"
#include "qtsq.h"
#include "../crypto/common/fips202.h"
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════════════════════════
 *  SHAKE128-based keystream PRNG (M7 fix: replaces xoshiro256**
 *  which is not a CSPRNG — its output is predictable after observing
 *  ~256 bits. SHAKE128 is a proper XOF keyed from the subkey.)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    shake128incctx state;
} ke_prng_t;

static void ke_prng_seed(ke_prng_t *prng, const uint8_t key[32]) {
    shake128_inc_init(&prng->state);
    shake128_inc_absorb(&prng->state, key, 32);
    shake128_inc_finalize(&prng->state);
}

static uint64_t ke_prng_next(ke_prng_t *prng) {
    uint8_t buf[8];
    shake128_inc_squeeze(buf, 8, &prng->state);
    uint64_t r;
    memcpy(&r, buf, 8);
    return r;
}

/* ══════════════════════════════════════════════════════════════
 *  Domain-separated subkeys — ensures position and color PRNG
 *  streams are independent even if the same master key is used.
 * ══════════════════════════════════════════════════════════════ */

static void derive_subkey(const uint8_t key[32],
                           const char *domain,
                           uint8_t out[32]) {
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, key, 32);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)domain, strlen(domain));
    sha3_256_inc_finalize(out, &ctx);
}

/* ══════════════════════════════════════════════════════════════
 *  Fisher-Yates keyed permutation  (encrypt = forward shuffle)
 * ══════════════════════════════════════════════════════════════ */

static void permute_records(uint8_t *data, uint32_t count, uint32_t stride,
                             const uint8_t key[32]) {
    if (count < 2) return;

    uint8_t subkey[32];
    derive_subkey(key, "GEOCRYPT-PERMUTE-v1", subkey);

    ke_prng_t rng;
    ke_prng_seed(&rng, subkey);

    uint8_t *tmp = (uint8_t *)malloc(stride);
    if (!tmp) return;

    for (uint32_t i = count - 1; i > 0; i--) {
        /* Uniform random j in [0, i] */
        uint64_t raw = ke_prng_next(&rng);
        uint32_t j   = (uint32_t)(raw % ((uint64_t)i + 1));

        if (i != j) {
            /* Swap records i and j */
            memcpy(tmp,                  data + (size_t)i * stride, stride);
            memcpy(data + (size_t)i * stride, data + (size_t)j * stride, stride);
            memcpy(data + (size_t)j * stride, tmp,                        stride);
        }
    }
    free(tmp);
}

/* Inverse permutation: record the forward shuffle then apply in reverse */
static void unpermute_records(uint8_t *data, uint32_t count, uint32_t stride,
                               const uint8_t key[32]) {
    if (count < 2) return;

    uint8_t subkey[32];
    derive_subkey(key, "GEOCRYPT-PERMUTE-v1", subkey);

    ke_prng_t rng;
    ke_prng_seed(&rng, subkey);

    /* Replay the forward shuffle to build the permutation table */
    uint32_t *perm = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!perm) return;
    for (uint32_t i = 0; i < count; i++) perm[i] = i;

    for (uint32_t i = count - 1; i > 0; i--) {
        uint64_t raw = ke_prng_next(&rng);
        uint32_t j   = (uint32_t)(raw % ((uint64_t)i + 1));
        if (i != j) {
            uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
    }

    /* Build inverse permutation */
    uint32_t *inv = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!inv) { free(perm); return; }
    for (uint32_t i = 0; i < count; i++) inv[perm[i]] = i;

    /* Apply inverse to data */
    uint8_t *scratch = (uint8_t *)malloc((size_t)count * stride);
    if (!scratch) { free(perm); free(inv); return; }
    memcpy(scratch, data, (size_t)count * stride);
    for (uint32_t i = 0; i < count; i++) {
        memcpy(data + (size_t)i * stride, scratch + (size_t)inv[i] * stride, stride);
    }
    free(scratch);
    free(perm);
    free(inv);
}

/* ══════════════════════════════════════════════════════════════
 *  Splat field offsets within splat2d_t (12 bytes):
 *    x   [0..1]  uint16
 *    y   [2..3]  uint16
 *    sx  [4]     uint8
 *    sy  [5]     uint8
 *    theta [6]   uint8
 *    r   [7]     uint8
 *    g   [8]     uint8
 *    b   [9]     uint8
 *    alpha [10]  uint8
 *    z   [11]    uint8
 * ══════════════════════════════════════════════════════════════ */

#define SPLAT_STRIDE 12

/* XOR position bytes (x:2B, y:2B, sx:1B, sy:1B, theta:1B) with PRNG */
static void xor_splat_positions(splat2d_t *splats, uint32_t count,
                                 const uint8_t key[32]) {
    uint8_t subkey[32];
    derive_subkey(key, "GEOCRYPT-POSITION-v1", subkey);

    ke_prng_t rng;
    ke_prng_seed(&rng, subkey);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t r = ke_prng_next(&rng);
        uint8_t *b = (uint8_t *)&r;
        /* XOR x (2 bytes) */
        *((uint8_t *)&splats[i].x)     ^= b[0];
        *((uint8_t *)&splats[i].x + 1) ^= b[1];
        /* XOR y (2 bytes) */
        *((uint8_t *)&splats[i].y)     ^= b[2];
        *((uint8_t *)&splats[i].y + 1) ^= b[3];
        /* XOR sx, sy, theta */
        splats[i].sx    ^= b[4];
        splats[i].sy    ^= b[5];
        splats[i].theta ^= b[6];
        /* b[7] unused — keeping stream aligned */
    }
}

/* XOR color bytes (r,g,b,alpha) with PRNG */
static void xor_splat_colors(splat2d_t *splats, uint32_t count,
                              const uint8_t key[32]) {
    uint8_t subkey[32];
    derive_subkey(key, "GEOCRYPT-COLOR-v1", subkey);

    ke_prng_t rng;
    ke_prng_seed(&rng, subkey);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t r = ke_prng_next(&rng);
        uint8_t *b = (uint8_t *)&r;
        splats[i].r     ^= b[0];
        splats[i].g     ^= b[1];
        splats[i].b     ^= b[2];
        splats[i].alpha ^= b[3];
        /* z (depth/shape): leave unencrypted so renderer knows shape type */
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Public API — Splat
 * ══════════════════════════════════════════════════════════════ */

int qtsq_geocrypt_encrypt_splats(splat2d_t *splats, uint32_t count,
                                  const uint8_t key[32], uint8_t mode) {
    if (!splats || !key || count == 0) return QTSQ_ERR_NULL;

    /* Apply in order: position → color → permute
     * (permute last so decryption order can simply reverse) */
    if (mode & QTSQ_GEOCRYPT_POSITION) xor_splat_positions(splats, count, key);
    if (mode & QTSQ_GEOCRYPT_COLOR)    xor_splat_colors(splats, count, key);
    if (mode & QTSQ_GEOCRYPT_PERMUTE)  permute_records((uint8_t *)splats, count, SPLAT_STRIDE, key);

    return QTSQ_OK;
}

int qtsq_geocrypt_decrypt_splats(splat2d_t *splats, uint32_t count,
                                  const uint8_t key[32], uint8_t mode) {
    if (!splats || !key || count == 0) return QTSQ_ERR_NULL;

    /* Reverse order: unpermute → color XOR (self-inverse) → position XOR (self-inverse) */
    if (mode & QTSQ_GEOCRYPT_PERMUTE)  unpermute_records((uint8_t *)splats, count, SPLAT_STRIDE, key);
    if (mode & QTSQ_GEOCRYPT_COLOR)    xor_splat_colors(splats, count, key);   /* XOR is its own inverse */
    if (mode & QTSQ_GEOCRYPT_POSITION) xor_splat_positions(splats, count, key);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Public API — Generic Vertex Buffer (TRM3 / GSP binary)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_geocrypt_encrypt_vertices(uint8_t *data, size_t size,
                                    const uint8_t key[32],
                                    uint8_t mode, uint32_t stride) {
    if (!data || !key || size == 0 || stride == 0) return QTSQ_ERR_NULL;

    uint32_t count = (uint32_t)(size / stride);
    if (count == 0) return QTSQ_OK;

    /* Derive subkeys independently for each operation */
    if (mode & QTSQ_GEOCRYPT_POSITION) {
        /* XOR first half of each record (position bytes) */
        uint8_t subkey[32];
        derive_subkey(key, "GEOCRYPT-POSITION-v1", subkey);
        ke_prng_t rng; ke_prng_seed(&rng, subkey);
        uint32_t pos_bytes = (stride >= 4) ? 4 : stride; /* XOR up to 4 position bytes */
        for (uint32_t i = 0; i < count; i++) {
            uint64_t r = ke_prng_next(&rng);
            uint8_t *rec = data + (size_t)i * stride;
            for (uint32_t b = 0; b < pos_bytes; b++) rec[b] ^= ((uint8_t *)&r)[b];
        }
    }
    if (mode & QTSQ_GEOCRYPT_COLOR) {
        /* XOR bytes after position area */
        uint8_t subkey[32];
        derive_subkey(key, "GEOCRYPT-COLOR-v1", subkey);
        ke_prng_t rng; ke_prng_seed(&rng, subkey);
        uint32_t col_off = (stride >= 4) ? 4 : 0;
        uint32_t col_len = (stride > col_off) ? stride - col_off : 0;
        if (col_len > 0) {
            for (uint32_t i = 0; i < count; i++) {
                uint64_t r = ke_prng_next(&rng);
                uint8_t *rec = data + (size_t)i * stride + col_off;
                uint32_t lim = col_len < 8 ? col_len : 8;
                for (uint32_t b = 0; b < lim; b++) rec[b] ^= ((uint8_t *)&r)[b];
            }
        }
    }
    if (mode & QTSQ_GEOCRYPT_PERMUTE) {
        permute_records(data, count, stride, key);
    }

    return QTSQ_OK;
}

int qtsq_geocrypt_decrypt_vertices(uint8_t *data, size_t size,
                                    const uint8_t key[32],
                                    uint8_t mode, uint32_t stride) {
    if (!data || !key || size == 0 || stride == 0) return QTSQ_ERR_NULL;

    uint32_t count = (uint32_t)(size / stride);
    if (count == 0) return QTSQ_OK;

    /* Reverse order */
    if (mode & QTSQ_GEOCRYPT_PERMUTE) unpermute_records(data, count, stride, key);

    /* XOR is self-inverse — reuse encrypt path */
    uint8_t mode_no_perm = mode & ~QTSQ_GEOCRYPT_PERMUTE;
    if (mode_no_perm) {
        qtsq_geocrypt_encrypt_vertices(data, size, key, mode_no_perm, stride);
    }

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Key Derivation
 * ══════════════════════════════════════════════════════════════ */

void qtsq_geocrypt_derive_key(const uint8_t master_key[32],
                               uint8_t out_key[32]) {
    static const char domain[] = "QTSQ-GeoCrypt-v1";
    derive_subkey(master_key, domain, out_key);
}
