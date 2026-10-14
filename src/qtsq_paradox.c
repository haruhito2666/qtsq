/*
 * Quantum Tensor Sequence — Information Paradox Engine (Error Recovery)
 * Feature: Reed-Solomon-inspired error detection and recovery
 * Adds parity data to .qtsq files for corruption resilience
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include "qtsq.h"

/* ── Configuration ── */

#define PARADOX_CHUNK_SIZE   256   /* Bytes per chunk for checksumming */
#define PARADOX_PARITY_RATIO 4    /* 1 parity byte per 4 data bytes (25% overhead) */

/* ══════════════════════════════════════════════════════════════
 *  qtsq_add_recovery — Add ECC parity data to context
 * ══════════════════════════════════════════════════════════════
 *
 * Divides the singularity into chunks, computes:
 *   1. CRC-32 per chunk (for detection)
 *   2. XOR-based parity blocks (for recovery)
 *
 * Stores recovery data in ctx->recovery.
 */

int qtsq_add_recovery(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0)
        return QTSQ_ERR_FORMAT;

    size_t data_size = ctx->singularity_size;
    uint32_t num_chunks = (uint32_t)((data_size + PARADOX_CHUNK_SIZE - 1) / PARADOX_CHUNK_SIZE);

    /* Free existing recovery data */
    if (ctx->recovery.chunk_checksums) {
        free(ctx->recovery.chunk_checksums);
        ctx->recovery.chunk_checksums = NULL;
    }
    if (ctx->recovery.reed_solomon_parity) {
        free(ctx->recovery.reed_solomon_parity);
        ctx->recovery.reed_solomon_parity = NULL;
    }

    /* Allocate chunk checksums (CRC-32 per chunk) */
    ctx->recovery.chunk_checksums = (uint32_t *)calloc(num_chunks, sizeof(uint32_t));
    if (!ctx->recovery.chunk_checksums) return QTSQ_ERR_ALLOC;

    /* Compute CRC-32 per chunk */
    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t offset = i * PARADOX_CHUNK_SIZE;
        uint32_t chunk_len = PARADOX_CHUNK_SIZE;
        if (offset + chunk_len > data_size) {
            chunk_len = data_size - offset;
        }
        ctx->recovery.chunk_checksums[i] =
            qtsq_crc32(ctx->singularity + offset, chunk_len);
    }

    /* Compute XOR parity blocks
     * Group chunks into sets of 4, XOR them together.
     * If one chunk is corrupted, XOR with the other 3 + parity recovers it. */
    uint32_t parity_groups = (num_chunks + 3) / 4;
    uint32_t parity_size = parity_groups * PARADOX_CHUNK_SIZE;

    ctx->recovery.reed_solomon_parity = (uint8_t *)calloc(parity_size, 1);
    if (!ctx->recovery.reed_solomon_parity) {
        free(ctx->recovery.chunk_checksums);
        ctx->recovery.chunk_checksums = NULL;
        return QTSQ_ERR_ALLOC;
    }

    for (uint32_t g = 0; g < parity_groups; g++) {
        uint8_t *parity = ctx->recovery.reed_solomon_parity + g * PARADOX_CHUNK_SIZE;
        memset(parity, 0, PARADOX_CHUNK_SIZE);

        for (uint32_t j = 0; j < 4; j++) {
            uint32_t chunk_idx = g * 4 + j;
            if (chunk_idx >= num_chunks) break;

            uint32_t offset = chunk_idx * PARADOX_CHUNK_SIZE;
            uint32_t chunk_len = PARADOX_CHUNK_SIZE;
            if (offset + chunk_len > data_size) {
                chunk_len = data_size - offset;
            }

            for (uint32_t b = 0; b < chunk_len; b++) {
                parity[b] ^= ctx->singularity[offset + b];
            }
        }
    }

    ctx->recovery.num_chunks = num_chunks;
    ctx->recovery.parity_size = parity_size;
    ctx->has_recovery = 1;
    ctx->header.flags |= QTSQ_FLAG_HAS_ECC;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_health_check — Check integrity of all chunks
 * ══════════════════════════════════════════════════════════════
 *
 * Returns:
 *   QTSQ_OK       — all chunks intact
 *   QTSQ_ERR_CORRUPT — one or more chunks corrupted (recoverable)
 *   QTSQ_ERR_RECOVERY_FAIL — too many chunks corrupted (unrecoverable)
 */

int qtsq_health_check(const qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->has_recovery) return QTSQ_OK; /* no ECC data, assume OK */

    size_t data_size = ctx->singularity_size;
    uint32_t num_chunks = ctx->recovery.num_chunks;
    uint32_t corrupted = 0;
    uint32_t prev_group = UINT32_MAX;
    uint32_t corrupted_in_group = 0;

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t offset = i * PARADOX_CHUNK_SIZE;
        uint32_t chunk_len = PARADOX_CHUNK_SIZE;
        if (offset + chunk_len > data_size) {
            chunk_len = data_size - offset;
        }

        uint32_t actual_crc = qtsq_crc32(ctx->singularity + offset, chunk_len);
        if (actual_crc != ctx->recovery.chunk_checksums[i]) {
            corrupted++;

            uint32_t group = i / 4;
            if (group == prev_group) {
                corrupted_in_group++;
            } else {
                prev_group = group;
                corrupted_in_group = 1;
            }

            /* XOR parity can only recover 1 chunk per group of 4 */
            if (corrupted_in_group > 1) {
                return QTSQ_ERR_RECOVERY_FAIL;
            }
        }
    }

    return corrupted > 0 ? QTSQ_ERR_CORRUPT : QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_recover — Attempt to recover corrupted chunks
 * ══════════════════════════════════════════════════════════════
 *
 * For each group of 4 chunks, if exactly 1 is corrupted:
 *   recovered = parity XOR (good_chunk_1) XOR (good_chunk_2) XOR (good_chunk_3)
 */

int qtsq_recover(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->has_recovery || !ctx->recovery.reed_solomon_parity)
        return QTSQ_ERR_FORMAT;

    size_t data_size = ctx->singularity_size;
    uint32_t num_chunks = ctx->recovery.num_chunks;
    uint32_t parity_groups = (num_chunks + 3) / 4;
    int recovered_any = 0;

    for (uint32_t g = 0; g < parity_groups; g++) {
        /* Find corrupted chunk in this group (if any) */
        int bad_idx = -1;
        int bad_count = 0;

        for (uint32_t j = 0; j < 4; j++) {
            uint32_t chunk_idx = g * 4 + j;
            if (chunk_idx >= num_chunks) break;

            uint32_t offset = chunk_idx * PARADOX_CHUNK_SIZE;
            uint32_t chunk_len = PARADOX_CHUNK_SIZE;
            if (offset + chunk_len > data_size) {
                chunk_len = data_size - offset;
            }

            uint32_t actual_crc = qtsq_crc32(ctx->singularity + offset, chunk_len);
            if (actual_crc != ctx->recovery.chunk_checksums[chunk_idx]) {
                bad_idx = (int)j;
                bad_count++;
            }
        }

        /* Can only recover if exactly 1 chunk is bad */
        if (bad_count == 0) continue;
        if (bad_count > 1) return QTSQ_ERR_RECOVERY_FAIL;

        /* Recover: XOR parity with all good chunks */
        uint32_t bad_chunk_idx = g * 4 + (uint32_t)bad_idx;
        uint32_t bad_offset = bad_chunk_idx * PARADOX_CHUNK_SIZE;
        uint32_t bad_len = PARADOX_CHUNK_SIZE;
        if (bad_offset + bad_len > data_size) {
            bad_len = data_size - bad_offset;
        }

        const uint8_t *parity = ctx->recovery.reed_solomon_parity + g * PARADOX_CHUNK_SIZE;

        /* Start with parity */
        uint8_t recovered[PARADOX_CHUNK_SIZE];
        memcpy(recovered, parity, PARADOX_CHUNK_SIZE);

        /* XOR with all good chunks in the group */
        for (uint32_t j = 0; j < 4; j++) {
            if ((int)j == bad_idx) continue;

            uint32_t chunk_idx = g * 4 + j;
            if (chunk_idx >= num_chunks) break;

            uint32_t offset = chunk_idx * PARADOX_CHUNK_SIZE;
            uint32_t chunk_len = PARADOX_CHUNK_SIZE;
            if (offset + chunk_len > data_size) {
                chunk_len = data_size - offset;
            }

            for (uint32_t b = 0; b < chunk_len; b++) {
                recovered[b] ^= ctx->singularity[offset + b];
            }
        }

        /* Write recovered chunk */
        memcpy(ctx->singularity + bad_offset, recovered, bad_len);

        /* Verify recovery */
        uint32_t new_crc = qtsq_crc32(ctx->singularity + bad_offset, bad_len);
        if (new_crc != ctx->recovery.chunk_checksums[bad_chunk_idx]) {
            return QTSQ_ERR_RECOVERY_FAIL; /* recovery failed */
        }

        recovered_any = 1;
    }

    /* H13 fix: both branches were QTSQ_OK, making recovery success
     * indistinguishable from failure. Return a distinct code so callers
     * can detect "nothing recovered." */
    return recovered_any ? QTSQ_OK : QTSQ_ERR_CORRUPT;
}
