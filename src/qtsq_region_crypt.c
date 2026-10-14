/*
 * Quantum Tensor Sequence — Selective Region Encryption
 *
 * Classifies splats by which rectangular region they fall in,
 * then applies per-region geocrypt (position + color XOR) using
 * the region's 32-byte key. Splats outside all regions are left
 * in plaintext — they form the publicly visible background.
 *
 * Region metadata is stored in schema.encoding as a compact binary
 * blob prefixed with "REGC".
 *
 * Author: Haruhito
 */

#include "qtsq_region_crypt.h"
#include "qtsq_geocrypt.h"
#include "qtsq.h"
#include "../crypto/common/fips202.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Classify a splat to a region (-1 = no region / public) ── */

static int classify_splat(const splat2d_t *sp,
                           const qtsq_region_t *regions, uint8_t num_regions) {
    for (int r = 0; r < num_regions; r++) {
        if (sp->x >= regions[r].x && sp->x < regions[r].x + regions[r].w &&
            sp->y >= regions[r].y && sp->y < regions[r].y + regions[r].h) {
            return (int)regions[r].key_id;
        }
    }
    return -1; /* public */
}

/* ── Derive a region subkey from master + key_id ── */

static void region_subkey(const uint8_t key[32], uint8_t key_id, uint8_t out[32]) {
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, key, 32);
    sha3_256_inc_absorb(&ctx, &key_id, 1);
    sha3_256_inc_finalize(out, &ctx);
}

/* ── XOR splat position and color bytes with per-region keystream ── */

static void region_xor_splat(splat2d_t *sp, uint32_t idx,
                              const uint8_t key[32]) {
    uint8_t domain[36];
    memcpy(domain, key, 32);
    domain[32] = (uint8_t)(idx);
    domain[33] = (uint8_t)(idx >> 8);
    domain[34] = (uint8_t)(idx >> 16);
    domain[35] = (uint8_t)(idx >> 24);

    uint8_t stream[16];
    shake128(stream, 16, domain, 36);

    /* XOR position bytes */
    /* DO NOT scramble x, y! If we scramble x, y, the splat will jump out of the
     * redaction region, polluting the public background and becoming unidentifiable
     * for decryption! Redaction must be spatially confined. */
    sp->sx    ^= stream[4];
    sp->sy    ^= stream[5];
    sp->theta ^= stream[6];
    /* XOR color bytes */
    sp->r     ^= stream[8];
    sp->g     ^= stream[9];
    sp->b     ^= stream[10];
    sp->alpha ^= stream[11];
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Region Metadata
 * ══════════════════════════════════════════════════════════════ */

int qtsq_region_store_metadata(qtsq_context_t *ctx,
                                const qtsq_region_t *regions, uint8_t num_regions) {
    if (!ctx || !regions || num_regions == 0 || num_regions > QTSQ_MAX_REGIONS)
        return QTSQ_ERR_FORMAT;

    /* Layout: "REGC" (4) + count (1) + per-region: key_id(1)+x(2)+y(2)+w(2)+h(2) = 9 bytes
     * Total: 5 + 9*N bytes. Encoding field is 64 bytes.
     * Max regions that fit: (64 - 5) / 9 = 6 */
    uint8_t max_regions_that_fit = (uint8_t)((64 - 5) / 9);
    if (num_regions > max_regions_that_fit) {
        return QTSQ_ERR_FORMAT; /* Too many regions for the encoding field */
    }

    uint8_t *enc = (uint8_t *)ctx->schema.encoding;
    memset(enc, 0, 64);
    enc[0] = 'R'; enc[1] = 'E'; enc[2] = 'G'; enc[3] = 'C';
    enc[4] = num_regions;
    for (int i = 0; i < num_regions; i++) {
        int off = 5 + i * 9;
        enc[off + 0] = regions[i].key_id;
        enc[off + 1] = (uint8_t)(regions[i].x);
        enc[off + 2] = (uint8_t)(regions[i].x >> 8);
        enc[off + 3] = (uint8_t)(regions[i].y);
        enc[off + 4] = (uint8_t)(regions[i].y >> 8);
        enc[off + 5] = (uint8_t)(regions[i].w);
        enc[off + 6] = (uint8_t)(regions[i].w >> 8);
        enc[off + 7] = (uint8_t)(regions[i].h);
        enc[off + 8] = (uint8_t)(regions[i].h >> 8);
    }
    return QTSQ_OK;
}

int qtsq_region_load_metadata(const qtsq_context_t *ctx,
                               qtsq_region_t *regions, uint8_t *num_regions) {
    if (!ctx || !regions || !num_regions) return QTSQ_ERR_NULL;

    const uint8_t *enc = (const uint8_t *)ctx->schema.encoding;
    if (enc[0] != 'R' || enc[1] != 'E' || enc[2] != 'G' || enc[3] != 'C')
        return QTSQ_ERR_FORMAT;

    *num_regions = enc[4];
    if (*num_regions > QTSQ_MAX_REGIONS) return QTSQ_ERR_FORMAT;

    for (int i = 0; i < *num_regions; i++) {
        int off = 5 + i * 9;
        regions[i].key_id = enc[off + 0];
        /* key_id indexes the caller's key arrays, which hold
         * QTSQ_MAX_REGIONS entries. It comes straight out of the
         * cleartext header, so an out-of-range value would read (and
         * on the encrypt path, pass to a memcpy) a pointer up to 8 KB
         * past the end of those arrays. */
        if (regions[i].key_id >= QTSQ_MAX_REGIONS) return QTSQ_ERR_FORMAT;
        regions[i].x = (uint32_t)(enc[off + 1]) | ((uint32_t)(enc[off + 2]) << 8);
        regions[i].y = (uint32_t)(enc[off + 3]) | ((uint32_t)(enc[off + 4]) << 8);
        regions[i].w = (uint32_t)(enc[off + 5]) | ((uint32_t)(enc[off + 6]) << 8);
        regions[i].h = (uint32_t)(enc[off + 7]) | ((uint32_t)(enc[off + 8]) << 8);
    }
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Encrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_region_encrypt_splats(splat2d_t *splats, uint32_t count,
                                const qtsq_region_t *regions, uint8_t num_regions,
                                const uint8_t keys[QTSQ_MAX_REGIONS][32],
                                qtsq_context_t *ctx) {
    if (!splats || !regions || !keys || !ctx || count == 0) return QTSQ_ERR_NULL;
    if (num_regions == 0 || num_regions > QTSQ_MAX_REGIONS) return QTSQ_ERR_FORMAT;

    uint32_t encrypted_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        int kid = classify_splat(&splats[i], regions, num_regions);
        if (kid < 0) continue; /* public splat, skip */

        uint8_t subkey[32];
        region_subkey(keys[kid], (uint8_t)kid, subkey);
        region_xor_splat(&splats[i], i, subkey);
        encrypted_count++;
    }

    ctx->header.flags |= QTSQ_FLAG_REGION_CRYPT;
    qtsq_region_store_metadata(ctx, regions, num_regions);
    fprintf(stderr, "[QTSQ] Region-encrypted %u/%u splats across %u regions\n",
            encrypted_count, count, num_regions);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Decrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_region_decrypt_splats(splat2d_t *splats, uint32_t count,
                                const uint8_t *keys[QTSQ_MAX_REGIONS],
                                const qtsq_context_t *ctx) {
    if (!splats || !ctx || count == 0) return QTSQ_ERR_NULL;

    qtsq_region_t regions[QTSQ_MAX_REGIONS];
    uint8_t num_regions = 0;
    if (qtsq_region_load_metadata(ctx, regions, &num_regions) != QTSQ_OK)
        return QTSQ_ERR_FORMAT;

    for (uint32_t i = 0; i < count; i++) {
        int kid = classify_splat(&splats[i], regions, num_regions);
        if (kid < 0) continue; /* public splat */
        if (kid >= QTSQ_MAX_REGIONS) continue; /* unreachable after the
                                                 * load-time check above;
                                                 * belt and braces */
        if (!keys[kid]) continue; /* no key for this region — leave encrypted */

        uint8_t subkey[32];
        region_subkey(keys[kid], (uint8_t)kid, subkey);
        region_xor_splat(&splats[i], i, subkey); /* XOR is self-inverse */
    }

    return QTSQ_OK;
}
