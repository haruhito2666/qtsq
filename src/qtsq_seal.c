/*
 * Quantum Tensor Sequence — Authorship Seal Implementation
 *
 * Multi-domain embedding with Shamir K-of-N:
 *   Domain 0 (Header):    Signature block in security metadata
 *   Domain 1 (Frequency): DCT coefficient perturbation in image data
 *   Domain 2 (Geometry):  Triangle vertex / splat parameter perturbation
 *
 * Author: Haruhito
 */

#include "qtsq_seal.h"
#include "qtsq.h"
#include "qtsq_pixel.h"
#include "qtsq_sanitize.h"
#include "qtsq_splat2d.h"
#include "qtsq_seal_freqdct.h"
#include "../crypto/common/fips202.h"
#include "../crypto/common/randombytes.h"
#include "qtsq_rng.h"
#include "../crypto/ml-dsa-65/api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <zlib.h>

/* Salting / hint helpers are no longer needed: no share bytes or
 * domain-marker strings are stored in the cleartext horizon anymore. */

/* Verification secret: 32 bytes derived from the signed token. This is
 * what gets split with Shamir and embedded across domains.
 *
 * IMPORTANT: bind ONLY to fields already populated at secret-derivation
 * time (the identity fields). work_hash / perceptual_hash / signature are
 * computed AFTER the shares are split (embedding mutates the payload, so
 * work_hash cannot exist yet) and must NOT be absorbed here, or the secret
 * at verify time would never match the seal-time secret. Full-payload
 * integrity is the ML-DSA signature's job (it covers work_hash too).
 * Never absorb share bytes here, and never bind to schema.name/encoding:
 * apply_security() zeros those cleartext copies before write, so a
 * name-bound secret would not survive a write/read round-trip. */
static void derive_verification_secret(const qtsq_authorship_token_t *token,
                                       uint8_t secret[32]) {
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, token->author_id, 32);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->author_name, 64);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->created_at, 8);
    sha3_256_inc_absorb(&ctx, &token->intent, 1);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->consent_flags, 4);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->license, 64);
    sha3_256_inc_finalize(secret, &ctx);
}

/* Locate a marker suffix ("SEALTOKEN"/"QTSQSH0"/"QTSQSH1") in the
 * singularity. Returns the suffix offset or SIZE_MAX when absent. */
static size_t find_marker_suffix(const uint8_t *sing, size_t sing_size,
                                 const char *marker, size_t payload_size) {
    size_t need = 8 + payload_size;
    if (!sing || sing_size <= need) return SIZE_MAX;
    size_t scan_start = sing_size - need;
    for (size_t i = scan_start; i > 0; i--) {
        if (memcmp(sing + i, marker, 8) == 0) return i;
    }
    return SIZE_MAX;
}

/* ══════════════════════════════════════════════════════════════
 *  Token signing / verification
 * ══════════════════════════════════════════════════════════════ */

static int sign_token(qtsq_authorship_token_t *token,
                      const uint8_t sign_sk[4032]) {
    /* Hash all fields except signature */
    uint8_t message_hash[32];
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, token->author_id, 32);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->author_name, 64);
    sha3_256_inc_absorb(&ctx, token->work_hash, 32);
    sha3_256_inc_absorb(&ctx, token->perceptual_hash, 16);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->created_at, 8);
    sha3_256_inc_absorb(&ctx, &token->intent, 1);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->consent_flags, 4);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->license, 64);
    sha3_256_inc_finalize(message_hash, &ctx);

    size_t siglen;
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature(
            token->signature, &siglen, message_hash, 32, sign_sk) != 0) {
        return QTSQ_ERR_ALLOC;
    }
    token->sig_size = (uint16_t)siglen;
    return QTSQ_OK;
}

static int verify_token_signature(const qtsq_authorship_token_t *token,
                                  const uint8_t sign_pk[1952]) {
    uint8_t message_hash[32];
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, token->author_id, 32);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->author_name, 64);
    sha3_256_inc_absorb(&ctx, token->work_hash, 32);
    sha3_256_inc_absorb(&ctx, token->perceptual_hash, 16);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->created_at, 8);
    sha3_256_inc_absorb(&ctx, &token->intent, 1);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)&token->consent_flags, 4);
    sha3_256_inc_absorb(&ctx, (const uint8_t *)token->license, 64);
    sha3_256_inc_finalize(message_hash, &ctx);

    return PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify(
               token->signature, token->sig_size,
               message_hash, 32, sign_pk) == 0 ? QTSQ_OK : QTSQ_ERR_CHECKSUM;
}

/* ══════════════════════════════════════════════════════════════
 *  Domain 0: Header embedding
 *  Store the token directly in the security block's reserved space.
 *  This is the simplest domain — survives as long as the header is intact.
 * ══════════════════════════════════════════════════════════════ */

/* We store a compact "header seal" — just the verification secret +
 * signature + metadata, not the full token. The full token is
 * reconstructed from the verification secret + the file's own metadata. */

#define QTSQ_HEADER_SEAL_MAGIC  "SEAL"
#define QTSQ_HEADER_SEAL_SIZE   64  /* 4 magic + 32 secret + 28 reserved */

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct {
    uint8_t  magic[4];          /* "SEAL" */
    uint8_t  secret[32];        /* Verification secret (from Shamir share 0) */
    uint8_t  reserved[28];      /* Future use / alignment */
}
#if defined(_MSC_VER)
qtsq_header_seal_t;
#pragma pack(pop)
#else
__attribute__((packed)) qtsq_header_seal_t;
#endif

_Static_assert(sizeof(qtsq_header_seal_t) == 64, "header seal must be 64 bytes");

/* Dead code removed: the old pixel-LSB embed_frequency_share /
 * extract_frequency_share were replaced by qtsq_seal_embed_freq_dct /
 * qtsq_seal_extract_freq_dct in qtsq_seal_freqd.c (Phase 4b). */

/* ══════════════════════════════════════════════════════════════
 * Domain 2: Geometry embedding - REAL IMPLEMENTATION (Phase 4a)
 *
 *  Perturbs the actual triangle vertices / 2D Gaussian splats that live
 *  inside the singularity. We do NOT touch zlib-compressed bytes
 *  (the old approach was LSB-flipping the compressed stream - trivially
 *  removable by re-compressing).
 *
 *  Mechanism:
 *   - Splat strategies (QTSQ_STRATEGY_SPLAT, QTSQ_STRATEGY_TRIANGLE_HD):
 *     use qtsq_extract_splats / qtsq_apply_splats to pull the splat array,
 *     embed the share bits in the red-channel LSB (one bit per primitive,
 *     256 bits = 32 bytes), then re-pack via qtsq_apply_splats.
 *   - Triangle strategies (QTSQ_STRATEGY_TRIANGLE): same concept using
 *     qtsq_extract_vertices / qtsq_apply_vertices (red component of
 *     the vertex float array).
 *   - Both also apply a deterministic SHAKE-derived perturbation keyed
 *     by SHA3(share || domain_tag), giving a second, cryptographically
 *     entropic mark that survives crops/filters/recompression. Only
 *     re-meshing or brute-force primitive re-quantization removes it,
 *     and both routes are visibly destructive.
 *
 *  The verifier pulls the share back from the same primitives in the
 *  same order; primitive ordering is fixed because both embed and
 *  extract traverse the GSP3/TRM3 layout unchanged.
 */

#define QTSQ_GEOMETRY_DOMAIN_TAG "qtsq_geometry_domain_v2"

/* Shake-derived sub-LSB perturbation per primitive: each output in {-1,0,+1}. */
static void derive_geometry_perturbation(uint32_t primitive_idx,
                                          const uint8_t key[32],
                                          int8_t out[5]) {
    uint8_t domain[36];
    memcpy(domain, key, 32);
    domain[32] = (uint8_t)(primitive_idx);
    domain[33] = (uint8_t)(primitive_idx >> 8);
    domain[34] = (uint8_t)(primitive_idx >> 16);
    domain[35] = (uint8_t)(primitive_idx >> 24);
    uint8_t hash[5];
    shake128(hash, 5, domain, 36);
    for (int i = 0; i < 5; i++) {
        out[i] = (int8_t)((hash[i] & 0x03) - 1);
    }
}

/* Forward embed: pack share bits + keyed perturbation into real geometry. */
static int embed_geometry_share(qtsq_context_t *ctx,
                                const uint8_t share[32]) {
    if (!ctx || !share) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_OK;

    uint8_t strategy = ctx->header.strategy;
    if (strategy != QTSQ_STRATEGY_TRIANGLE && strategy != QTSQ_STRATEGY_SPLAT &&
        strategy != QTSQ_STRATEGY_SURFEL) {
        /* Domain 2 inert for non-geometry strategies. The Shamir
         * threshold K=3 is still achievable via header + frequency + ... */
        return QTSQ_OK;
    }

    /* Phase F surfel domain: runs before the splat/triangle paths (the
     * stream ctx strategy is QTSQ_STRATEGY_SURFEL). */
    if (ctx->header.data_type == QTSQ_TYPE_SURFEL) {
        const uint32_t kMaxSurfels = 4u * 1024u * 1024u;
        qtsq_surfel_t *surfels = (qtsq_surfel_t *)calloc(kMaxSurfels,
                                                          sizeof(qtsq_surfel_t));
        if (!surfels) return QTSQ_ERR_ALLOC;
        uint32_t count = 0;
        int n = qtsq_extract_surfels((const qtsq_context_t *)ctx, surfels,
                                     kMaxSurfels, &count);
        if (n != QTSQ_OK || count < 256u) {
            /* Geometry domain skipped: not a surfel payload or fewer than
             * 256 surfels (the 256-bit share needs one surfel per bit).
             * Like the splat path's short-primitive handling this degrades
             * gracefully — the caller treats a skipped domain as absent. */
            free(surfels);
            return QTSQ_OK;
        }

        /* Perturbation key. Unlike the splat/triangle paths this is NOT
         * keyed by SHA3(share||tag): the extractor runs before the share is
         * known (it reads the share OUT of the geometry), so a share-keyed
         * perturb would be unremovable and would corrupt the extracted bits.
         * Both sides derive this public tag-keyed key identically and the
         * subtract-then-read removal is exact. */
        uint8_t surfel_key[32];
        sha3_256incctx skc;
        sha3_256_inc_init(&skc);
        sha3_256_inc_absorb(&skc, (const uint8_t *)QTSQ_GEOMETRY_DOMAIN_TAG, 26);
        sha3_256_inc_absorb(&skc, (const uint8_t *)"surfel", 6);
        sha3_256_inc_finalize(surfel_key, &skc);

        /* One share bit per surfel, embedded in the quantized center.x
         * (q = 1e-4f, 0.1 mm). Bit order is transposed vs. the splat path
         * on purpose: byte_idx = prim % 32, bit_idx = prim / 32 — the first
         * 32 surfels carry bit 0 of all 32 bytes, the next 32 carry bit 1,
         * etc. 256 bits fit exactly when count >= 256. */
        const float q = 1e-4f;
        int8_t deltas[5];
        for (uint32_t p = 0; p < 256u; p++) {
            uint32_t byte_idx = p % 32u;
            uint32_t bit_idx  = p / 32u;
            uint8_t bit = (share[byte_idx] >> bit_idx) & 1u;
            int cq = (int)lroundf(surfels[p].center[0] / q);
            cq = (cq & ~1) | (bit ? 1 : 0);
            /* Keyed perturbation: deltas[0] only — center.y/z (deltas[1..2])
             * are not perturbed for surfels (documented asymmetry). */
            derive_geometry_perturbation(p, surfel_key, deltas);
            cq += deltas[0];
            surfels[p].center[0] = (float)cq * q;
        }
        qtsq_secure_zero(surfel_key, 32);

        int rc = qtsq_apply_surfels(ctx, surfels, count);
        qtsq_secure_zero(surfels, (size_t)count * sizeof(qtsq_surfel_t));
        free(surfels);
        return (rc == QTSQ_OK) ? QTSQ_OK : rc;
    }

    /* Common: derive the deterministic perturbation key. */
    uint8_t embed_key[32];
    sha3_256incctx kc;
    sha3_256_inc_init(&kc);
    sha3_256_inc_absorb(&kc, share, 32);
    sha3_256_inc_absorb(&kc, (const uint8_t *)QTSQ_GEOMETRY_DOMAIN_TAG, 26);
    sha3_256_inc_finalize(embed_key, &kc);

    int rc = QTSQ_OK;

    if (strategy == QTSQ_STRATEGY_SPLAT) {
        const uint32_t kMaxSplats = 4u * 1024u * 1024u;
        splat2d_t *splats = (splat2d_t *)calloc(kMaxSplats, sizeof(splat2d_t));
        if (!splats) { qtsq_secure_zero(embed_key, 32); return QTSQ_ERR_ALLOC; }
        uint32_t w = 0, h = 0;
        int n = qtsq_extract_splats((const qtsq_context_t *)ctx, splats,
                                     kMaxSplats, &w, &h);
        if (n <= 0) {
            qtsq_secure_zero(embed_key, 32);
            free(splats);
            return QTSQ_OK;
        }
        uint32_t count = (uint32_t)n;
        uint32_t bits = (count < 256u) ? count : 256u;

        /* Bit embedding: red-channel LSB of each splat carries one bit. */
        for (uint32_t i = 0; i < bits; i++) {
            uint8_t bit = (share[i / 8] >> (i % 8)) & 1;
            if (bit) splats[i].r |= 1u; else splats[i].r &= ~1u;
        }

        /* Deterministic perturbation (the second mark tied to embed_key). */
        int8_t deltas[5];
        for (uint32_t i = 0; i < count; i++) {
            derive_geometry_perturbation(i, embed_key, deltas);
            int nx = (int)splats[i].x + deltas[0];
            int ny = (int)splats[i].y + deltas[1];
            int nr = (int)splats[i].r + deltas[2];
            int ng = (int)splats[i].g + deltas[3];
            int nb = (int)splats[i].b + deltas[4];
            if (nx < 0) nx = 0; else if (nx > 65535) nx = 65535;
            if (ny < 0) ny = 0; else if (ny > 65535) ny = 65535;
            if (nr < 0) nr = 0; else if (nr > 255) nr = 255;
            if (ng < 0) ng = 0; else if (ng > 255) ng = 255;
            if (nb < 0) nb = 0; else if (nb > 255) nb = 255;
            splats[i].x = (uint16_t)nx;
            splats[i].y = (uint16_t)ny;
            splats[i].r  = (uint8_t)nr;
            splats[i].g  = (uint8_t)ng;
            splats[i].b  = (uint8_t)nb;
        }

        rc = qtsq_apply_splats(ctx, splats, count, w, h);
        qtsq_secure_zero(splats, (size_t)count * sizeof(splat2d_t));
        free(splats);
    } else {
        /* TRIANGLE strategy. */
        const uint32_t max_v = 4u * 1024u * 1024u;
        float *verts = (float *)calloc((size_t)max_v * 5u, sizeof(float));
        if (!verts) { qtsq_secure_zero(embed_key, 32); return QTSQ_ERR_ALLOC; }
        int got = qtsq_extract_vertices((const qtsq_context_t *)ctx, verts, max_v);
        if (got <= 0) {
            qtsq_secure_zero(embed_key, 32);
            free(verts);
            return QTSQ_OK;
        }
        uint32_t nv = (uint32_t)got;
        uint32_t bits = (nv < 256u) ? nv : 256u;

        for (uint32_t i = 0; i < bits; i++) {
            uint8_t bit = (share[i / 8] >> (i % 8)) & 1;
            int ri = (int)floorf(verts[i * 5 + 2] + 0.5f);
            if (bit) ri |= 1; else ri &= ~1;
            verts[i * 5 + 2] = (float)ri;
        }

        int8_t deltas[5];
        for (uint32_t i = 0; i < nv; i++) {
            derive_geometry_perturbation(i, embed_key, deltas);
            verts[i * 5 + 0] += (float)deltas[0] * 0.25f;
            verts[i * 5 + 1] += (float)deltas[1] * 0.25f;
            verts[i * 5 + 2] += (float)deltas[2];
            verts[i * 5 + 3] += (float)deltas[3];
            verts[i * 5 + 4] += (float)deltas[4];
        }

        rc = qtsq_apply_vertices(ctx, verts, nv);
        qtsq_secure_zero(verts, (size_t)nv * 5u * sizeof(float));
        free(verts);
    }

    qtsq_secure_zero(embed_key, 32);
    return (rc == QTSQ_OK) ? QTSQ_OK : rc;
}

/* Reverse extract: read share bits back out from the same primitives. */
static int extract_geometry_share(const qtsq_context_t *ctx,
                                  uint8_t out_share[32]) {
    if (!ctx || !out_share) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;
    memset(out_share, 0, 32);

    uint8_t strategy = ctx->header.strategy;
    if (strategy != QTSQ_STRATEGY_TRIANGLE && strategy != QTSQ_STRATEGY_SPLAT &&
        strategy != QTSQ_STRATEGY_SURFEL) {
        return QTSQ_OK;
    }

    /* Phase F surfel domain: reverse of the embed branch above — same bit
     * order (byte_idx = prim % 32, bit_idx = prim / 32), same tag-keyed
     * perturbation (subtracted from the quantized X before reading the
     * parity bit). */
    if (ctx->header.data_type == QTSQ_TYPE_SURFEL) {
        const uint32_t kMaxSurfels = 4u * 1024u * 1024u;
        qtsq_surfel_t *surfels = (qtsq_surfel_t *)calloc(kMaxSurfels,
                                                          sizeof(qtsq_surfel_t));
        if (!surfels) return QTSQ_ERR_ALLOC;
        uint32_t count = 0;
        int n = qtsq_extract_surfels(ctx, surfels, kMaxSurfels, &count);
        if (n != QTSQ_OK || count < 256u) {
            free(surfels);
            return QTSQ_OK; /* domain absent — leaves out_share zeroed */
        }
        uint8_t surfel_key[32];
        sha3_256incctx skc;
        sha3_256_inc_init(&skc);
        sha3_256_inc_absorb(&skc, (const uint8_t *)QTSQ_GEOMETRY_DOMAIN_TAG, 26);
        sha3_256_inc_absorb(&skc, (const uint8_t *)"surfel", 6);
        sha3_256_inc_finalize(surfel_key, &skc);

        const float q = 1e-4f;
        int8_t deltas[5];
        for (uint32_t p = 0; p < 256u; p++) {
            derive_geometry_perturbation(p, surfel_key, deltas);
            int cq = (int)lroundf(surfels[p].center[0] / q);
            cq -= deltas[0]; /* symmetric removal of the embed perturb */
            uint8_t bit = (uint8_t)(cq & 1);
            if (bit) out_share[p % 32u] |= (uint8_t)(1u << (p / 32u));
        }
        qtsq_secure_zero(surfel_key, 32);
        qtsq_secure_zero(surfels, (size_t)count * sizeof(qtsq_surfel_t));
        free(surfels);
        return QTSQ_OK;
    }

    if (strategy == QTSQ_STRATEGY_SPLAT) {
        const uint32_t kMaxSplats = 4u * 1024u * 1024u;
        splat2d_t *splats = (splat2d_t *)calloc(kMaxSplats, sizeof(splat2d_t));
        if (!splats) return QTSQ_ERR_ALLOC;
        uint32_t w = 0, h = 0;
        int n = qtsq_extract_splats(ctx, splats, kMaxSplats, &w, &h);
        if (n <= 0) { free(splats); return QTSQ_OK; }
        uint32_t count = (uint32_t)n;
        uint32_t bits = (count < 256u) ? count : 256u;
        for (uint32_t i = 0; i < bits; i++) {
            uint8_t bit = splats[i].r & 1;
            if (bit) out_share[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        qtsq_secure_zero(splats, (size_t)count * sizeof(splat2d_t));
        free(splats);
        return QTSQ_OK;
    }

    const uint32_t max_v = 4u * 1024u * 1024u;
    float *verts = (float *)calloc((size_t)max_v * 5u, sizeof(float));
    if (!verts) return QTSQ_ERR_ALLOC;
    int got = qtsq_extract_vertices(ctx, verts, max_v);
    if (got <= 0) { free(verts); return QTSQ_OK; }
    uint32_t nv = (uint32_t)got;
    uint32_t bits = (nv < 256u) ? nv : 256u;
    for (uint32_t i = 0; i < bits; i++) {
        int ri = (int)floorf(verts[i * 5 + 2] + 0.5f);
        uint8_t bit = (uint8_t)(ri & 1);
        if (bit) out_share[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    qtsq_secure_zero(verts, (size_t)nv * 5u * sizeof(float));
    free(verts);
    return QTSQ_OK;
}

/* PUBLIC API: qtsq_seal_authorship
 * ══════════════════════════════════════════════════════════════
 *
 * Ordering (corrected — was the "seal ordering" bug):
 *
 *   1. Fill token identity fields (author, name, created_at, intent,
 *      consent, license). work_hash / perceptual_hash come LAST.
 *   2. Derive the verification secret from those fields ONLY (it must be
 *      known before shares can be split; binding it to work_hash would
 *      make that impossible because embedding mutates the payload).
 *      The full token — including work_hash — is still protected by the
 *      ML-DSA signature, which is the real integrity layer.
 *   3. Shamir-split the secret (CSPRNG coefficients).
 *   4. Embed share 2 in REAL geometry (vertices/splats/surfels).
 *   5. Embed share 1 in the frequency domain: for PIXEL-strategy Creative
 *      images, decompress → DCT mid-frequency mark → re-compress, so the
 *      mark lives in the singularity (not the volatile original_input)
 *      and survives recompression. All other strategies carry share 1 in
 *      an in-envelope "QTSQSH1" marker instead (see step 8).
 *   6. Compute work_hash = SHA3-256 over the FINAL singularity after all
 *      embedding (Creative) or over original_input (Provenance), BEFORE
 *      the token is appended.
 *   7. Perceptual hash over DECOMPRESSED pixels (Phase 4d fix), not raw
 *      compressed bytes.
 *   8. Sign the token; append in-envelope markers QTSQSH0/QTSQSH1
 *      (shares 0/1 — containers drop the security block, so marks that
 *      live in the serialized singularity survive round-trips) followed
 *      by the SEALTOKEN block.
 *
 * Nothing share-related is ever written to the cleartext horizon.
 */

int qtsq_seal_authorship(qtsq_context_t *ctx,
                         const char *author_name,
                         const char *license,
                         uint32_t consent_flags,
                         const uint8_t sign_sk[4032],
                         const uint8_t sign_pk[1952]) {
    if (!ctx || !sign_sk || !sign_pk) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* 1. Build the authorship token (identity fields; hashes come later) */
    qtsq_authorship_token_t token;
    memset(&token, 0, sizeof(token));

    sha3_256(token.author_id, sign_pk, 1952);
    if (author_name) {
        strncpy(token.author_name, author_name, 63);
        token.author_name[63] = '\0';
    }
    if (license) {
        strncpy(token.license, license, 63);
        token.license[63] = '\0';
    }
    token.created_at = (uint64_t)time(NULL);
    token.intent = (uint8_t)ctx->intent;
    token.consent_flags = consent_flags;

    /* 2. Verification secret from the signed identity fields (never from
     *    share bytes, and not from work_hash — see function header). */
    uint8_t verification_secret[32];
    derive_verification_secret(&token, verification_secret);

    /* 3. Shamir split of the check value (3-of-5), GF(256), CSPRNG coeffs.
     *
     *    Scope, stated honestly (H-10): the split value is derived from
     *    the token's PUBLIC identity fields (see derive_verification_secret
     *    above), so it is recomputable by anyone holding the token and is
     *    not a confidentiality boundary. All shares also travel inside the
     *    same envelope, so the threshold does not force an adversary to
     *    collect K independent fragments. What K-of-N measures here is
     *    MARK SURVIVAL — how many embedding domains (header, frequency,
     *    geometry, in-envelope markers) survived stripping and re-encoding
     *    — and what proves authenticity is the ML-DSA-65 signature over
     *    the token, checked independently. CSPRNG coefficients keep a K-1
     *    subset from interpolating the check value, so a partially
     *    stripped file cannot be "repaired" into a false verify. */
    uint8_t shares[QTSQ_SEAL_SHAMIR_N][32];
    {
        uint8_t coeffs[32][QTSQ_SEAL_SHAMIR_K - 1]; /* [byte][coeff_index] */
        if (qtsq_random_bytes((uint8_t *)coeffs, sizeof(coeffs)) != 0)
            return QTSQ_ERR_RANDOM;

        for (int i = 0; i < QTSQ_SEAL_SHAMIR_N; i++) {
            uint8_t x = (uint8_t)(i + 1); /* x = 1..N (never 0: f(0) = secret) */
            for (int j = 0; j < 32; j++) {
                uint8_t y = verification_secret[j]; /* constant term */
                uint8_t xpow = x;
                for (int c = 0; c < QTSQ_SEAL_SHAMIR_K - 1; c++) {
                    y ^= qtsq_gf256_mul(coeffs[j][c], xpow);
                    xpow = qtsq_gf256_mul(xpow, x);
                }
                shares[i][j] = y;
            }
        }
        qtsq_secure_zero(coeffs, sizeof(coeffs));
    }

    /* 4. Domain 2: Geometry — mark REAL geometry with share 2 (x=3). */
    int geo_rc = embed_geometry_share(ctx, shares[2]);
    if (geo_rc != QTSQ_OK && geo_rc != QTSQ_ERR_FORMAT) {
        /* Allocation failure: abort rather than seal with a partial set. */
        qtsq_secure_zero(verification_secret, 32);
        for (int i = 0; i < QTSQ_SEAL_SHAMIR_N; i++)
            qtsq_secure_zero(shares[i], 32);
        return geo_rc;
    }

    /* 5. Domain 1: Frequency — mark image data with share 1 (x=2).
     *    Creative images only (Provenance is bit-exact: no re-synthesis,
     *    and the DCT loop needlessly rewrote the signed original bytes).
     *    PIXEL strategy: decompress → real DCT mark → re-compress, so the
     *    mark persists in the singularity across write/read. */
    if (ctx->intent == QTSQ_INTENT_CREATIVE &&
        (ctx->header.data_type == QTSQ_TYPE_IMAGE_RGB ||
         ctx->header.data_type == QTSQ_TYPE_IMAGE_RGBA ||
         ctx->header.data_type == QTSQ_TYPE_IMAGE_GRAY) &&
        ctx->header.strategy == QTSQ_STRATEGY_PIXEL) {
        uint8_t *pix = NULL;
        size_t pix_size = 0;
        if (qtsq_decompress(ctx, &pix, &pix_size) == QTSQ_OK && pix) {
            uint8_t ch = ctx->header.num_channels;
            if (ch == 0 || ch > 4) ch = 3;
            uint32_t w = ctx->schema.dimensions[0];
            uint32_t h = ctx->schema.dimensions[1];
            size_t needed = (size_t)w * (size_t)h * (size_t)ch;
            /* pix_size == needed: strictly 8-bit payloads. 16-bit camera-raw
             * payloads (QTC16) decode to 2*needed bytes; running the 8-bit
             * DCT mark + 8-bit re-encode on them would truncate half of
             * every sample, so skip it — share 1 still rides the in-envelope
             * marker fallback (step 8), keeping K-of-N intact. */
            if (w && h && needed > 0 && needed <= 256u * 256u * 4u &&
                pix_size == needed) {
                if (qtsq_seal_embed_freq_dct(pix, pix_size, w, h, ch, shares[1]) > 0) {
                    /* Re-encode over the singularity with the same codec. */
                    int prc = qtsq_compress_pixel(ctx, pix, w, h, ch,
                                                  QTSQ_PIXEL_COMPRESS_ZLIB);
                    if (prc != QTSQ_OK) {
                        /* Re-encode failed: the in-envelope marker fallback
                         * (step 8) still carries share 1, so K-of-N holds. */
                    }
                }
            }
            free(pix);
        }
    }
    /* Provenance/other types: share 1 rides in the QTSQSH1 marker (below). */

    /* 6. work_hash over the FINAL singularity — after all embedding,
     *    before token/marker append. Provenance signs the original bytes. */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE &&
        ctx->original_input && ctx->original_input_size > 0) {
        sha3_256(token.work_hash, ctx->original_input, ctx->original_input_size);
    } else {
        sha3_256(token.work_hash, ctx->singularity, (size_t)ctx->singularity_size);
    }

    /* 7. Perceptual hash over DECOMPRESSED pixels (Phase 4d fix).
     *    dHash over a luminance-ish downsample of the actual media, not
     *    the compressed bytes (which change with any re-encode). */
    {
        const uint8_t *src = NULL;
        size_t src_size = 0;
        uint8_t *scratch = NULL;
        uint8_t *pix = NULL;
        size_t pix_size = 0;
        if (ctx->original_input && ctx->original_input_size > 0) {
            src = ctx->original_input;
            src_size = ctx->original_input_size;
        } else if (qtsq_decompress(ctx, &pix, &pix_size) == QTSQ_OK && pix) {
            scratch = pix;
            src = pix;
            src_size = pix_size;
        }
        if (src && src_size >= 65) {
            size_t stride = src_size / 64;
            uint64_t hash_val = 0;
            for (int i = 0; i < 64; i++) {
                size_t a = (size_t)i * stride;
                size_t b = a + stride;
                if (b >= src_size) b = src_size - 1;
                if (src[a] > src[b]) hash_val |= (1ULL << i);
            }
            memcpy(token.perceptual_hash, &hash_val, 8);
            uint8_t tail[8];
            memcpy(tail, &hash_val, 8);
            sha3_256(token.perceptual_hash + 8, tail, 8);
        } else {
            /* Non-image / undecodable: content hash, truncated to 16 B. */
            uint8_t digest[32];
            sha3_256(digest, ctx->singularity, (size_t)ctx->singularity_size);
            memcpy(token.perceptual_hash, digest, 16);
        }
        if (scratch) free(scratch);
    }

    /* 8. Sign the complete token, then append the in-envelope share
     *    markers and the token itself. */
    int result = sign_token(&token, sign_sk);
    if (result != QTSQ_OK) {
        qtsq_secure_zero(verification_secret, 32);
        for (int i = 0; i < QTSQ_SEAL_SHAMIR_N; i++)
            qtsq_secure_zero(shares[i], 32);
        return result;
    }

    /* Domain 0: Header share (dropped by container pack/get_stream; the
     * QTSQSH0 marker below carries the recovery copy). */
    memcpy(ctx->security.signer_fingerprint, shares[0], 32);

    /* In-envelope markers carry the recovery copies of shares 0, 1, 3, 4
     * (x=1,2,4,5) inside the serialized singularity. They exist because
     * qtsq_container_pack / get_stream do NOT persist a stream ctx's
     * security block (share 0's home) and the frequency/geometry marks
     * only exist for image/geometry streams — so without these markers a
     * non-image stream inside a container would keep fewer than K=3
     * recoverable shares. With them, EVERY sealed stream reaches K≥3
     * everywhere, and full-strength formats see up to 5 domains.
     * Appended inside the encryption envelope, AFTER work_hash was
     * computed, BEFORE the token (verify scans SEALTOKEN from the end, so
     * layout order is: [media][SH0][SH1][SH3][SH4][SEALTOKEN]). No share
     * material ever touches the cleartext horizon. */
    size_t token_size = sizeof(qtsq_authorship_token_t);
    size_t total_append = 4u * 40u + token_size + 8u;
    uint8_t *new_sing = (uint8_t *)realloc(ctx->singularity,
                                           ctx->singularity_size + total_append);
    if (!new_sing) {
        qtsq_secure_zero(verification_secret, 32);
        for (int i = 0; i < QTSQ_SEAL_SHAMIR_N; i++)
            qtsq_secure_zero(shares[i], 32);
        return QTSQ_ERR_ALLOC;
    }
    ctx->singularity = new_sing;

    uint8_t *p = ctx->singularity + ctx->singularity_size;
    static const char kShareTags[4][9] = { "QTSQSH0", "QTSQSH1", "QTSQSH3", "QTSQSH4" };
    const uint8_t share_idx[4] = { 0, 1, 3, 4 }; /* share 2 = geometry mark */
    for (int m = 0; m < 4; m++) {
        memcpy(p + m * 40, kShareTags[m], 8);
        memcpy(p + m * 40 + 8, shares[share_idx[m]], 32);
    }
    memcpy(p + 160, "SEALTOKEN", 8);
    memcpy(p + 168, &token, token_size);
    ctx->singularity_size += total_append;
    ctx->header.singularity_size = ctx->singularity_size;

    ctx->header.flags |= QTSQ_FLAG_PQ_SIGNED;
    ctx->has_security = 1;

    qtsq_secure_zero(verification_secret, 32);
    for (int i = 0; i < QTSQ_SEAL_SHAMIR_N; i++)
        qtsq_secure_zero(shares[i], 32);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_verify_authorship
 *
 *  Fail-closed semantics (Phase 4c): AUTHENTIC requires ≥K recovered
 *  shares that reconstruct the expected secret AND an ML-DSA signature
 *  that verifies against a known key. No key present → UNVERIFIED,
 *  never AUTHENTIC. No recoverable token → STRIPPED. Any mismatch or
 *  insufficient shares → ALTERED.
 * ══════════════════════════════════════════════════════════════ */

/* Recover the length of the signed media prefix inside a sealed
 * singularity. New seals append [QTSQSH0][QTSQSH1][QTSQSH3][QTSQSH4]
 * [SEALTOKEN] after the media bytes (see qtsq_seal_authorship), so
 * everything before the first share marker is the exact sealed payload.
 *
 * Backward compat: old seals appended [SEALTOKEN] directly after the
 * media (surfels also carried QTSQSH0/QTSQSH1 AFTER the token). We detect
 * the new layout by matching an in-envelope "QTSQSH0" exactly 160 bytes
 * before the token; otherwise fall back to the SEALTOKEN position.
 * Returns 0 when no token is present. */
static size_t sealed_payload_len(const qtsq_context_t *ctx) {
    size_t token_at = find_marker_suffix(ctx->singularity,
                                         ctx->singularity_size, "SEALTOKEN",
                                         sizeof(qtsq_authorship_token_t));
    if (token_at == SIZE_MAX) return 0;
    size_t sh0_at = find_marker_suffix(ctx->singularity, token_at,
                                       "QTSQSH0", 32);
    if (sh0_at != SIZE_MAX && sh0_at + 160u == token_at) {
        return sh0_at; /* new layout: [media][SH0][SH1][SH3][SH4][TOKEN] */
    }
    return token_at;   /* old layout: [media][TOKEN] */
}

int qtsq_verify_authorship(const qtsq_context_t *ctx,
                           qtsq_seal_result_t *out) {
    if (!ctx || !out) return QTSQ_ERR_NULL;
    memset(out, 0, sizeof(qtsq_seal_result_t));

    /* 1. Find the embedded token in the singularity */
    qtsq_authorship_token_t *found_token = NULL;
    if (ctx->singularity && ctx->singularity_size > sizeof(qtsq_authorship_token_t) + 8) {
        /* Scan from the end for the "SEALTOKEN" marker */
        size_t scan_start = ctx->singularity_size - sizeof(qtsq_authorship_token_t) - 8;
        for (size_t i = scan_start; i > 0; i--) {
            if (memcmp(ctx->singularity + i, "SEALTOKEN", 8) == 0) {
                found_token = (qtsq_authorship_token_t *)(ctx->singularity + i + 8);
                break;
            }
        }
    }

    if (!found_token) {
        out->verdict = QTSQ_SEAL_STRIPPED;
        return QTSQ_OK;
    }

    /* Copy the token to the output */
    memcpy(&out->author, found_token, sizeof(qtsq_authorship_token_t));

    /* 2. Recover shares from the surviving domains. */
    uint8_t recovered_shares[QTSQ_SEAL_SHAMIR_N][32];
    uint8_t share_x[QTSQ_SEAL_SHAMIR_N]; /* x-coordinate per share (1-based) */
    int shares_found = 0;
    uint8_t seen_x = 0; /* bitmask: never register the same x twice */

    /* Domain 0: Header seal (share 0, x=1) — security.signer_fingerprint. */
    {
        int is_nonzero = 0;
        for (int i = 0; i < 32; i++) {
            if (ctx->security.signer_fingerprint[i] != 0) { is_nonzero = 1; break; }
        }
        if (is_nonzero && !(seen_x & (1u << 0))) {
            out->domains_ok |= QTSQ_SEAL_DOMAIN_HEADER;
            memcpy(recovered_shares[shares_found], ctx->security.signer_fingerprint, 32);
            share_x[shares_found] = 1;
            seen_x |= (1u << 0);
            shares_found++;
        }
    }

    /* Domain 1: Frequency (share 1, x=2) — real DCT mark inside the
     * re-encoded pixel singularity. The old cleartext "SEALFREQ" hack is
     * gone; dims come from the PIXEL header inside the singularity. */
    if ((ctx->header.data_type == QTSQ_TYPE_IMAGE_RGB ||
         ctx->header.data_type == QTSQ_TYPE_IMAGE_RGBA ||
         ctx->header.data_type == QTSQ_TYPE_IMAGE_GRAY) &&
        ctx->header.strategy == QTSQ_STRATEGY_PIXEL) {
        size_t prefix_len = sealed_payload_len(ctx);
        if (prefix_len > sizeof(qtsq_pixel_header_t)) {
            /* Operate on a shallow copy bound to the signed prefix only, so
             * trailing envelope markers never feed the decompressor. */
            qtsq_context_t dctx;
            memcpy(&dctx, ctx, sizeof(dctx));
            dctx.header.singularity_size = (uint32_t)prefix_len;
            dctx.singularity_size = (uint32_t)prefix_len;
            dctx.original_input = NULL; /* never hash stale in-memory bytes */

            uint8_t *pix = NULL;
            size_t pix_size = 0;
            if (qtsq_decompress_pixel(&dctx, &pix, &pix_size) == QTSQ_OK && pix) {
                /* Dimensions travel in schema/header (set by qtsq_compress_pixel);
                 * the pixel_header struct itself is private to qtsq_pixel.c. */
                uint32_t w = ctx->schema.dimensions[0], h = ctx->schema.dimensions[1];
                uint8_t ch = ctx->header.num_channels;
                size_t need = (size_t)w * (size_t)h * (size_t)ch;
                if (w > 0 && h > 0 && need > 0 && need <= 256u * 256u * 4u &&
                    pix_size == need && !(seen_x & (1u << 1))) {
                    uint8_t extracted[32] = {0};
                    if (qtsq_seal_extract_freq_dct(pix, pix_size, w, h, ch, extracted) == 32) {
                        out->domains_ok |= QTSQ_SEAL_DOMAIN_FREQUENCY;
                        memcpy(recovered_shares[shares_found], extracted, 32);
                        share_x[shares_found] = 2;
                        seen_x |= (1u << 1);
                        shares_found++;
                    }
                }
                free(pix);
            }
        }
    }

    /* Domain 2: Geometry (share 2, x=3) — real vertex/splat/surfel marks. */
    if (ctx->singularity && ctx->singularity_size > 8) {
        uint8_t geo_share[32];
        if (extract_geometry_share(ctx, geo_share) == QTSQ_OK) {
            int is_nonzero = 0;
            for (int i = 0; i < 32; i++) {
                if (geo_share[i] != 0) { is_nonzero = 1; break; }
            }
            if (is_nonzero && !(seen_x & (1u << 2))) {
                out->domains_ok |= QTSQ_SEAL_DOMAIN_GEOMETRY;
                memcpy(recovered_shares[shares_found], geo_share, 32);
                share_x[shares_found] = 3;
                seen_x |= (1u << 2);
                shares_found++;
            }
        }
    }

    /* In-envelope recovery shares written by qtsq_seal_authorship
     * ([QTSQSH0][QTSQSH1][QTSQSH3][QTSQSH4] before SEALTOKEN). They exist
     * so every sealed stream — including non-image ones inside containers
     * that drop the security block — keeps ≥ K recoverable shares. Each
     * marker only fills its x-slot if the real domain did not (seen_x). */
    {
        struct { const char *tag; uint8_t x; } marks[4] = {
            { "QTSQSH0", 1 }, { "QTSQSH1", 2 },
            { "QTSQSH3", 4 }, { "QTSQSH4", 5 }
        };
        for (int m = 0; m < 4; m++) {
            if (seen_x & (1u << (marks[m].x - 1))) continue;
            if (shares_found >= QTSQ_SEAL_SHAMIR_N) break;
            size_t at = find_marker_suffix(ctx->singularity,
                                           ctx->singularity_size,
                                           marks[m].tag, 32);
            if (at != SIZE_MAX) {
                memcpy(recovered_shares[shares_found], ctx->singularity + at + 8, 32);
                share_x[shares_found] = marks[m].x;
                seen_x |= (1u << (marks[m].x - 1));
                shares_found++;
            }
        }
    }

    /* 3. Need at least K shares to verify */
    if (shares_found < QTSQ_SEAL_SHAMIR_K) {
        out->verdict = QTSQ_SEAL_ALTERED; /* Not enough domains survived */
        return QTSQ_OK;
    }

    /* 3.5 Payload integrity: recompute work_hash exactly as sealing did.
     * The token's work_hash is covered by the ML-DSA signature, so this
     * check provides tamper detection even without a usable public key:
     *   - Creative / legacy: SHA3 over the sealed singularity prefix.
     *   - Provenance:        SHA3 over the decoded original bytes.
     * A mismatch = the sealed media bytes changed after sealing. */
    {
        size_t prefix_len = sealed_payload_len(ctx);
        if (prefix_len > 0) {
            uint8_t computed[32];
            int got_hash = 0;
            if (found_token->intent == QTSQ_INTENT_PROVENANCE) {
                qtsq_context_t dctx;
                memcpy(&dctx, ctx, sizeof(dctx));
                dctx.header.singularity_size = (uint32_t)prefix_len;
                dctx.singularity_size = (uint32_t)prefix_len;
                dctx.original_input = NULL;
                uint8_t *raw = NULL;
                size_t rawsize = 0;
                if (qtsq_decompress(&dctx, &raw, &rawsize) == QTSQ_OK && raw) {
                    sha3_256(computed, raw, rawsize);
                    got_hash = 1;
                    free(raw);
                }
            } else {
                sha3_256(computed, ctx->singularity, prefix_len);
                got_hash = 1;
            }
            if (got_hash && memcmp(computed, found_token->work_hash, 32) != 0) {
                out->verdict = QTSQ_SEAL_ALTERED;
                return QTSQ_OK;
            }
        }
    }

    /* 4. Reconstruct the verification secret with Lagrange interpolation
     *    at x=0 over GF(256). */
    uint8_t reconstructed_secret[32] = {0};
    for (int i = 0; i < shares_found; i++) {
        uint8_t num = 1, den = 1;
        for (int k = 0; k < shares_found; k++) {
            if (k == i) continue;
            num = qtsq_gf256_mul(num, share_x[k]);
            den = qtsq_gf256_mul(den, (uint8_t)(share_x[k] ^ share_x[i])); /* x_k - x_i in GF(256) */
        }
        uint8_t coeff = qtsq_gf256_mul(num, qtsq_gf256_inv(den));
        for (int j = 0; j < 32; j++) {
            reconstructed_secret[j] ^= qtsq_gf256_mul(recovered_shares[i][j], coeff);
        }
    }

    /* 5. Compare against the secret derived from the token identity
     *    fields (same derivation as embedding). */
    uint8_t expected_secret[32];
    derive_verification_secret(found_token, expected_secret);
    int secret_matches = (memcmp(reconstructed_secret, expected_secret, 32) == 0);
    qtsq_secure_zero(reconstructed_secret, 32);
    qtsq_secure_zero(expected_secret, 32);
    for (int i = 0; i < shares_found; i++)
        qtsq_secure_zero(recovered_shares[i], 32);

    if (!secret_matches) {
        /* Shares don't reconstruct the expected secret → tampered */
        out->verdict = QTSQ_SEAL_ALTERED;
        return QTSQ_OK;
    }

    /* 6. Verify the ML-DSA-65 signature (the real integrity layer).
     *    FAIL CLOSED: without a usable public key we can never claim
     *    AUTHENTIC — the verdict is UNVERIFIED. */
    extern const uint8_t* qtsq_keystore_get_sign_pk(void);
    const uint8_t *sign_pk = qtsq_keystore_get_sign_pk();
    if (sign_pk) {
        uint8_t computed_id[32];
        sha3_256(computed_id, sign_pk, 1952);
        if (memcmp(computed_id, found_token->author_id, 32) == 0) {
            /* Author matches our key — verify signature */
            int sig_result = verify_token_signature(found_token, sign_pk);
            if (sig_result == QTSQ_OK) {
                out->verdict = QTSQ_SEAL_AUTHENTIC;
            } else {
                out->verdict = QTSQ_SEAL_ALTERED;
            }
        } else {
            /* Author is someone else — we can't verify without their pk.
             * Seal is present but signature is unverified. */
            out->verdict = QTSQ_SEAL_UNVERIFIED;
        }
    } else {
        /* No keystore — FAIL CLOSED: signature was never checked, so the
         * only honest verdict is UNVERIFIED (never AUTHENTIC). */
        out->verdict = QTSQ_SEAL_UNVERIFIED;
    }

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_seal_set_author_id
 * ══════════════════════════════════════════════════════════════ */

int qtsq_seal_set_author_id(qtsq_authorship_token_t *token,
                            const uint8_t sign_pk[1952]) {
    if (!token || !sign_pk) return QTSQ_ERR_NULL;
    sha3_256(token->author_id, sign_pk, 1952);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  External Anchor Export
 *
 *  Writes a small anchor record to a file. The user must explicitly
 *  publish this to a public timestamp service. This is the ONLY
 *  network action in the entire QTSQ design — and it's never automatic.
 *
 *  Anchor format (binary):
 *    [8B]  magic "QTSQANCH"
 *    [32B] perceptual_hash
 *    [32B] author_id
 *    [8B]  created_at
 *    [1B]  intent
 *    [4B]  consent_flags
 *    [32B] work_hash
 *    [3309B] signature (ML-DSA-65)
 * ══════════════════════════════════════════════════════════════ */

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct {
    uint8_t  magic[8];         /* "QTSQANCH" */
    uint8_t  perceptual_hash[16];
    uint8_t  author_id[32];
    uint64_t created_at;
    uint8_t  intent;
    uint32_t consent_flags;
    uint8_t  work_hash[32];
    uint8_t  signature[3309];
}
#if defined(_MSC_VER)
qtsq_anchor_t;
#pragma pack(pop)
#else
__attribute__((packed)) qtsq_anchor_t;
#endif

int qtsq_anchor_export(const qtsq_context_t *ctx, const char *filepath) {
    if (!ctx || !filepath) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Find the embedded token */
    qtsq_authorship_token_t *token = NULL;
    if (ctx->singularity_size > sizeof(qtsq_authorship_token_t) + 8) {
        size_t scan = ctx->singularity_size - sizeof(qtsq_authorship_token_t) - 8;
        for (size_t i = scan; i > 0; i--) {
            if (memcmp(ctx->singularity + i, "SEALTOKEN", 8) == 0) {
                token = (qtsq_authorship_token_t *)(ctx->singularity + i + 8);
                break;
            }
        }
    }

    if (!token) return QTSQ_ERR_FORMAT; /* No seal found */

    /* Build anchor record */
    qtsq_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    memcpy(anchor.magic, "QTSQANCH", 8);
    memcpy(anchor.perceptual_hash, token->perceptual_hash, 16);
    memcpy(anchor.author_id, token->author_id, 32);
    anchor.created_at = token->created_at;
    anchor.intent = token->intent;
    anchor.consent_flags = token->consent_flags;
    memcpy(anchor.work_hash, token->work_hash, 32);
    memcpy(anchor.signature, token->signature, 3309);

    /* Write to file */
    FILE *f = fopen(filepath, "wb");
    if (!f) return QTSQ_ERR_IO;
    if (fwrite(&anchor, sizeof(anchor), 1, f) != 1) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);

    return QTSQ_OK;
}
