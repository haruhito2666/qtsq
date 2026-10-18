/*
 * Quantum Tensor Sequence — Visual Secret Sharing (Shamir over GF-256)
 *
 * Novel: Naor & Shamir (1994) did visual secret sharing on pixels.
 * This operates on QTSQ geometric primitive byte streams, so each share
 * renders as geometric noise — unrecognizable even visually.
 *
 * Security: information-theoretic (unconditional), no computational
 * assumptions. Even an adversary with unlimited CPU cannot reconstruct
 * the secret from fewer than K shares.
 *
 * Author: Haruhito
 */

#include "qtsq_shamir.h"
#include "qtsq.h"
#include "../crypto/common/randombytes.h"
#include "qtsq_rng.h"
#include "../crypto/common/fips202.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════
 *  GF(256) arithmetic
 *  Irreducible polynomial: x^8 + x^4 + x^3 + x + 1 (AES poly, 0x11B)
 * ══════════════════════════════════════════════════════════════ */

/* Precomputed log/antilog tables (GF256, generator 3) */
static uint8_t gf_log[256];
static uint8_t gf_exp[512];   /* doubled to avoid modular arithmetic */
static int     gf_tables_init = 0;

static void gf256_init(void) {
    if (gf_tables_init) return;
    uint32_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        /* multiply by 3 (which is x + 1) */
        uint32_t x2 = x << 1;
        if (x2 & 0x100) x2 ^= 0x11B;
        x = x2 ^ x;
    }
    /* Extend exp table to simplify modular arithmetic */
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_log[0] = 0; /* log(0) is undefined — set sentinel */
    gf_tables_init = 1;
}

uint8_t qtsq_gf256_mul(uint8_t a, uint8_t b) {
    gf256_init();
    if (a == 0 || b == 0) return 0;
    return gf_exp[(int)gf_log[a] + (int)gf_log[b]];
}

uint8_t qtsq_gf256_inv(uint8_t a) {
    gf256_init();
    if (a == 0) return 0; /* undefined */
    return gf_exp[255 - (int)gf_log[a]];
}

/* GF add = XOR */
static inline uint8_t gf_add(uint8_t a, uint8_t b) { return a ^ b; }

/* ══════════════════════════════════════════════════════════════
 *  Polynomial evaluation at point x over GF(256)
 *  poly[0] = secret (constant term), poly[1..k-1] = random coefficients
 * ══════════════════════════════════════════════════════════════ */

static uint8_t poly_eval(const uint8_t *poly, uint8_t degree, uint8_t x) {
    /* Horner's method: poly[d] * x^d + ... + poly[1] * x + poly[0] */
    uint8_t result = poly[degree];
    for (int i = (int)degree - 1; i >= 0; i--) {
        result = gf_add(qtsq_gf256_mul(result, x), poly[i]);
    }
    return result;
}

/* ══════════════════════════════════════════════════════════════
 *  Lagrange Interpolation over GF(256)
 *  Recovers f(0) = secret given K (x_i, y_i) pairs
 * ══════════════════════════════════════════════════════════════ */

static uint8_t lagrange_at_zero(const uint8_t *xs, const uint8_t *ys, uint8_t k) {
    gf256_init();
    uint8_t secret = 0;
    for (uint8_t i = 0; i < k; i++) {
        /* Compute numerator and denominator of Lagrange basis L_i(0) */
        uint8_t num = 1, den = 1;
        for (uint8_t j = 0; j < k; j++) {
            if (i == j) continue;
            /* num *= (0 - x_j) = x_j  (since -x = x in GF(256)) */
            num = qtsq_gf256_mul(num, xs[j]);
            /* den *= (x_i - x_j) = x_i XOR x_j */
            den = qtsq_gf256_mul(den, gf_add(xs[i], xs[j]));
        }
        /* L_i(0) = num / den = num * inv(den) */
        uint8_t term = qtsq_gf256_mul(ys[i], qtsq_gf256_mul(num, qtsq_gf256_inv(den)));
        secret = gf_add(secret, term);
    }
    return secret;
}

/* ══════════════════════════════════════════════════════════════
 *  Polynomial coefficient generation using SHA3 CSPRNG
 *  Derives (k-1) random coefficients from a fresh random seed
 *  so each byte position gets independent random polynomials
 * ══════════════════════════════════════════════════════════════ */

/* Dead code removed: gen_coefficients was replaced by randombytes() in
 * Phase 2c. It also used a VLA (uint8_t domain[seed_len + 4]) which was
 * a stack-overflow risk for large seeds. */

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_shamir_split
 *  Phase 2c: Use CSPRNG per-coefficient (not SHAKE expansion of one seed)
 *  to restore information-theoretic security.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_shamir_split(const qtsq_context_t *ctx,
                       uint8_t k, uint8_t n,
                       qtsq_context_t *shares) {
    if (!ctx || !shares) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;
    if (k < 2 || n < k || n > 254) return QTSQ_ERR_FORMAT;

    gf256_init();

    size_t sz = (size_t)ctx->singularity_size;

    /* Allocate share singularities */
    for (uint8_t i = 0; i < n; i++) {
        shares[i] = *ctx; /* copy all header/schema fields */
        /* Null out pointer fields that belong to the source ctx — the shares
         * must not inherit ownership of the source's transforms/radiation/
         * wormholes/original_input, or qtsq_write would traverse freed memory
         * after the source is freed (use-after-free). */
        shares[i].singularity = NULL; /* allocated below */
        shares[i].transforms = NULL;
        shares[i].radiation_index = NULL;
        shares[i].wormholes = NULL;
        shares[i].original_input = NULL;
        shares[i].recovery.chunk_checksums = NULL;
        shares[i].recovery.reed_solomon_parity = NULL;
        shares[i].recovery.parity_size = 0;
        shares[i].num_transforms = 0;
        shares[i].num_radiation = 0;
        shares[i].num_wormholes = 0;
        shares[i].original_input_size = 0;
        shares[i].singularity = (uint8_t *)malloc(sz);
        if (!shares[i].singularity) {
            /* Free already allocated */
            for (uint8_t j = 0; j < i; j++) free(shares[j].singularity);
            return QTSQ_ERR_ALLOC;
        }
        /* Share x-coordinates are 1..N (never 0, as 0 = secret) */
        shares[i].schema.dimensions[4] = i + 1;  /* share index (1-based) */
        shares[i].schema.dimensions[5] = k;
        shares[i].schema.dimensions[6] = n;
        shares[i].header.flags |= QTSQ_FLAG_SECRET_SHARE;
    }

    uint8_t coeffs[254]; /* max k-1 = 253 coefficients */
    uint8_t xs[255];
    for (uint8_t i = 0; i < n; i++) xs[i] = i + 1;

    /* For every byte in the singularity, evaluate the polynomial at N points.
     * Phase 2c fix: draw coefficients directly from CSPRNG, not SHAKE expansion. */
    for (size_t pos = 0; pos < sz; pos++) {
        uint8_t secret_byte = ctx->singularity[pos];

        /* Generate k-1 truly random coefficients from CSPRNG (not derived from one seed) */
        if (qtsq_random_bytes(coeffs, k - 1) != 0) {
            for (uint8_t j = 0; j < n; j++) free(shares[j].singularity);
            return QTSQ_ERR_RANDOM;
        }

        /* Build polynomial: poly[0]=secret, poly[1..k-1]=random coeffs */
        uint8_t poly[255];
        poly[0] = secret_byte;
        for (uint8_t c = 0; c < k - 1; c++) poly[c + 1] = coeffs[c];

        /* Evaluate at each share's x coordinate */
        for (uint8_t i = 0; i < n; i++) {
            shares[i].singularity[pos] = poly_eval(poly, k - 1, xs[i]);
        }
    }

    /* H-9: shares used to carry SHA3-256(secret) in cleartext
     * (dimensions[7] + schema.encoding), so any single share answered
     * "is the secret X?" offline and Shamir's information-theoretic
     * property -- the reason this scheme exists -- was gone. Each share
     * now carries a digest of ITS OWN bytes instead. That still catches a
     * corrupted or half-rewritten share at reconstruct time and reveals
     * nothing beyond the share itself. (Detecting a fully re-forged share
     * set would need verifiable secret sharing; a hash commitment never
     * provided that -- a forger just commits to the forged secret.) */
    for (uint8_t i = 0; i < n; i++) {
        uint8_t digest[32];
        sha3_256(digest, shares[i].singularity, sz);
        memcpy(&shares[i].schema.dimensions[7], digest, 4);   /* bytes 0-3 */
        memcpy(shares[i].schema.encoding, digest + 4, 28);    /* bytes 4-31 */
        memset(digest, 0, sizeof(digest));
    }

    /* No seed/poly_master to clean since we use randombytes directly */
    memset(coeffs, 0, sizeof(coeffs));

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_shamir_reconstruct
 *  Phase 2c: Add integrity check via commitment verification
 * ══════════════════════════════════════════════════════════════ */

int qtsq_shamir_reconstruct(const qtsq_context_t *shares, uint8_t k,
                              qtsq_context_t *out) {
    if (!shares || !out || k < 2) return QTSQ_ERR_NULL;

    /* Validate all shares have the SECRET_SHARE flag and consistent sizes */
    size_t sz = (size_t)shares[0].singularity_size;
    uint8_t expected_k = (uint8_t)shares[0].schema.dimensions[5];

    if (k < expected_k) return QTSQ_ERR_FORMAT; /* Not enough shares */

    for (uint8_t i = 0; i < k; i++) {
        if (!(shares[i].header.flags & QTSQ_FLAG_SECRET_SHARE)) return QTSQ_ERR_FORMAT;
        if ((size_t)shares[i].singularity_size != sz) return QTSQ_ERR_FORMAT;
        if (!shares[i].singularity) return QTSQ_ERR_FORMAT;
    }

    gf256_init();

    /* Collect x coordinates from share indices */
    uint8_t xs[255];
    for (uint8_t i = 0; i < k; i++) {
        xs[i] = (uint8_t)shares[i].schema.dimensions[4]; /* 1-based share index */
        if (xs[i] == 0) return QTSQ_ERR_FORMAT; /* x=0 is reserved for secret */
    }

    /* Allocate output singularity.
     * M-9: shares read back from disk (qtsq_keystore_recover) own live
     * transform/radiation/wormhole/original_input/recovery buffers. The
     * struct copy must not make `out` a second owner, or freeing both
     * double-frees. Detach every borrowed pointer BEFORE the malloc can
     * fail, because callers free `out` on the error path. */
    *out = shares[0]; /* Copy header/schema from first share */
    out->header.flags &= ~QTSQ_FLAG_SECRET_SHARE; /* Remove share flag */
    out->transforms = NULL;
    out->radiation_index = NULL;
    out->wormholes = NULL;
    out->original_input = NULL;
    out->recovery.chunk_checksums = NULL;
    out->recovery.reed_solomon_parity = NULL;
    out->recovery.parity_size = 0;
    out->num_transforms = 0;
    out->num_radiation = 0;
    out->num_wormholes = 0;
    out->original_input_size = 0;
    out->singularity = (uint8_t *)malloc(sz);
    if (!out->singularity) return QTSQ_ERR_ALLOC;
    out->singularity_size = (uint32_t)sz;

    /* Reconstruct each byte via Lagrange interpolation at x=0 */
    uint8_t ys[255];
    for (size_t pos = 0; pos < sz; pos++) {
        for (uint8_t i = 0; i < k; i++) ys[i] = shares[i].singularity[pos];
        out->singularity[pos] = lagrange_at_zero(xs, ys, k);
    }

    /* Integrity check, two generations:
     *
     *  - New (H-9): dimensions[7] + encoding hold SHA3-256 of the share's
     *    OWN bytes, verified per share before interpolation above.
     *  - Legacy: the same 32 bytes held SHA3-256(secret) (the pre-fix
     *    commitment). Those files still reconstruct; the check runs on the
     *    reconstructed bytes exactly as before.
     *
     * Distinguish by recomputing the self-digest of share 0: it matches
     * only for new-format shares. A legacy share can never collide. */
    uint8_t self_digest[32], embedded[32];
    sha3_256(self_digest, shares[0].singularity, sz);
    memcpy(embedded, &shares[0].schema.dimensions[7], 4);
    memcpy(embedded + 4, shares[0].schema.encoding, 28);
    int is_self_checked = memcmp(self_digest, embedded, 32) == 0;
    memset(self_digest, 0, sizeof(self_digest));
    memset(embedded, 0, sizeof(embedded));

    if (is_self_checked) {
        /* Verify every share before trusting the interpolation result. */
        for (uint8_t i = 0; i < k; i++) {
            uint8_t d[32], e[32];
            sha3_256(d, shares[i].singularity, sz);
            memcpy(e, &shares[i].schema.dimensions[7], 4);
            memcpy(e + 4, shares[i].schema.encoding, 28);
            int ok = memcmp(d, e, 32) == 0;
            memset(d, 0, sizeof(d));
            memset(e, 0, sizeof(e));
            if (!ok) {
                free(out->singularity);
                out->singularity = NULL;
                return QTSQ_ERR_CHECKSUM;
            }
        }
    } else {
        /* Legacy secret commitment (see H-9): still verified. */
        uint8_t expected_commitment[32];
        memcpy(expected_commitment, &shares[0].schema.dimensions[7], 4);
        memcpy(expected_commitment + 4, shares[0].schema.encoding, 28);

        uint8_t actual_commitment[32];
        sha3_256(actual_commitment, out->singularity, sz);

        if (memcmp(expected_commitment, actual_commitment, 32) != 0) {
            /* Integrity check failed - forged or corrupted shares */
            free(out->singularity);
            out->singularity = NULL;
            return QTSQ_ERR_CHECKSUM;
        }
    }

    return QTSQ_OK;
}
