/*
 * Quantum Tensor Sequence — Wormhole Engine (Inter-file Links)
 * Feature: Deduplicate data across multiple .qtsq files
 * Wormholes are references to data stored in other files.
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "qtsq.h"

/* M-4: wormhole records are loaded from untrusted .qtsq files and carry a
 * target path, so resolving one used to open whatever local file a crafted
 * file named (and the path_hash "check" compared the path to its own hash
 * -- worthless). Only paths this process registered -- via
 * qtsq_add_wormhole or qtsq_wormhole_trust_path -- are opened. */
#define QTSQ_WORMHOLE_MAX_TRUSTED 64
static uint8_t  g_trusted_paths[QTSQ_WORMHOLE_MAX_TRUSTED][32];
static uint32_t g_trusted_count = 0;

static void trust_path(const char *path) {
    uint8_t h[32];
    qtsq_sha256((const uint8_t *)path, strlen(path), h);
    for (uint32_t i = 0; i < g_trusted_count; i++)
        if (memcmp(g_trusted_paths[i], h, 32) == 0) return;
    if (g_trusted_count < QTSQ_WORMHOLE_MAX_TRUSTED)
        memcpy(g_trusted_paths[g_trusted_count++], h, 32);
}

static int path_is_trusted(const char *path) {
    uint8_t h[32];
    qtsq_sha256((const uint8_t *)path, strlen(path), h);
    for (uint32_t i = 0; i < g_trusted_count; i++)
        if (memcmp(g_trusted_paths[i], h, 32) == 0) return 1;
    return 0;
}

void qtsq_wormhole_trust_path(const char *path) {
    if (path) trust_path(path);
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_add_wormhole — Add a reference to data in another file
 * ══════════════════════════════════════════════════════════════
 *
 * Instead of storing duplicate data, a wormhole links to a byte
 * range in another .qtsq file. When resolved, the data is read
 * from the target file.
 *
 * target_path:   path to the .qtsq file containing the data
 * target_offset: byte offset within the target's decompressed output
 * target_size:   number of bytes to reference
 */

int qtsq_add_wormhole(qtsq_context_t *ctx,
                     const char *target_path,
                     uint32_t target_offset, uint32_t target_size) {
    if (!ctx || !target_path) return QTSQ_ERR_NULL;

    /* Expand wormhole array */
    uint32_t new_count = ctx->num_wormholes + 1;
    qtsq_wormhole_t *new_wormholes = (qtsq_wormhole_t *)realloc(
        ctx->wormholes, new_count * sizeof(qtsq_wormhole_t));
    if (!new_wormholes) return QTSQ_ERR_ALLOC;

    ctx->wormholes = new_wormholes;
    qtsq_wormhole_t *wh = &ctx->wormholes[ctx->num_wormholes];

    /* Fill wormhole entry */
    memset(wh, 0, sizeof(qtsq_wormhole_t));
    wh->type = QTSQ_WORMHOLE_LOCAL;
    strncpy(wh->target_path, target_path, sizeof(wh->target_path) - 1);
    wh->target_offset = target_offset;
    wh->target_size = target_size;

    /* Compute hash of the target path for verification */
    qtsq_sha256((const uint8_t *)target_path, strlen(target_path),
                wh->target_hash);

    /* The caller named this path, so it is trusted for resolution. */
    trust_path(target_path);

    ctx->num_wormholes = new_count;
    ctx->header.flags |= QTSQ_FLAG_HAS_WORMHOLES;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_resolve_wormholes — Fetch data from all linked files
 * ══════════════════════════════════════════════════════════════
 *
 * Opens each target .qtsq file, decompresses its data, extracts
 * the referenced byte range, and appends it to the current
 * context's singularity buffer.
 */

int qtsq_resolve_wormholes(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (ctx->num_wormholes == 0) return QTSQ_OK; /* nothing to resolve */

    for (uint32_t i = 0; i < ctx->num_wormholes; i++) {
        qtsq_wormhole_t *wh = &ctx->wormholes[i];

        /* Only resolve local wormholes (LAN/cloud would need network) */
        if (wh->type != QTSQ_WORMHOLE_LOCAL) continue;

        /* M-4: never open a path that came out of a file. */
        if (!path_is_trusted(wh->target_path)) return QTSQ_ERR_WORMHOLE;

        /* Open target file */
        qtsq_context_t target;
        int result = qtsq_init(&target);
        if (result != QTSQ_OK) return result;

        result = qtsq_read(&target, wh->target_path);
        if (result != QTSQ_OK) {
            qtsq_free(&target);
            return QTSQ_ERR_WORMHOLE;
        }

        /* Verify target hash */
        uint8_t path_hash[32];
        qtsq_sha256((const uint8_t *)wh->target_path,
                     strlen(wh->target_path), path_hash);
        if (memcmp(path_hash, wh->target_hash, 32) != 0) {
            qtsq_free(&target);
            return QTSQ_ERR_WORMHOLE_HASH;
        }

        /* Decompress target */
        uint8_t *target_data = NULL;
        size_t target_size = 0;
        result = qtsq_decompress(&target, &target_data, &target_size);
        if (result != QTSQ_OK) {
            qtsq_free(&target);
            return result;
        }

        /* Bounds check — widen to uint64_t to prevent (offset + size)
         * from wrapping past UINT32_MAX before the comparison. */
        if ((uint64_t)wh->target_offset + wh->target_size > target_size) {
            free(target_data);
            qtsq_free(&target);
            return QTSQ_ERR_WORMHOLE;
        }

        /* Append wormhole data to singularity */
        size_t new_sing_size = (size_t)ctx->singularity_size + wh->target_size;
        uint8_t *new_sing = (uint8_t *)realloc(ctx->singularity, new_sing_size);
        if (!new_sing) {
            free(target_data);
            qtsq_free(&target);
            return QTSQ_ERR_ALLOC;
        }

        memcpy(new_sing + ctx->singularity_size,
               target_data + wh->target_offset,
               wh->target_size);

        ctx->singularity = new_sing;
        ctx->singularity_size = (uint64_t)new_sing_size;

        free(target_data);
        qtsq_free(&target);
    }

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Wormhole serialization helpers (for file I/O)
 * ══════════════════════════════════════════════════════════════ */

size_t qtsq_wormholes_packed_size(const qtsq_context_t *ctx) {
    if (!ctx) return 0;
    /* 4 bytes count + each wormhole is sizeof(qtsq_wormhole_t) */
    return 4 + (size_t)ctx->num_wormholes * sizeof(qtsq_wormhole_t);
}

int qtsq_pack_wormholes(const qtsq_context_t *ctx, uint8_t *buf, size_t capacity) {
    if (!ctx || !buf) return QTSQ_ERR_NULL;

    size_t needed = qtsq_wormholes_packed_size(ctx);
    if (capacity < needed) return QTSQ_ERR_ALLOC;

    size_t pos = 0;
    memcpy(buf + pos, &ctx->num_wormholes, 4); pos += 4;

    for (uint32_t i = 0; i < ctx->num_wormholes; i++) {
        memcpy(buf + pos, &ctx->wormholes[i], sizeof(qtsq_wormhole_t));
        pos += sizeof(qtsq_wormhole_t);
    }

    return QTSQ_OK;
}

int qtsq_unpack_wormholes(qtsq_context_t *ctx, const uint8_t *buf, size_t size) {
    if (!ctx || !buf) return QTSQ_ERR_NULL;
    if (size < 4) return QTSQ_ERR_FORMAT;

    uint32_t count;
    memcpy(&count, buf, 4);

    if (4 + (size_t)count * sizeof(qtsq_wormhole_t) > size)
        return QTSQ_ERR_FORMAT;

    if (ctx->wormholes) free(ctx->wormholes);

    ctx->wormholes = (qtsq_wormhole_t *)calloc(count, sizeof(qtsq_wormhole_t));
    if (!ctx->wormholes && count > 0) return QTSQ_ERR_ALLOC;

    size_t pos = 4;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&ctx->wormholes[i], buf + pos, sizeof(qtsq_wormhole_t));
        /* Force NUL termination: a crafted record can fill target_path
         * completely and any later strlen() would run off the struct. */
        ctx->wormholes[i].target_path[sizeof(ctx->wormholes[i].target_path) - 1] = '\0';
        pos += sizeof(qtsq_wormhole_t);
    }

    ctx->num_wormholes = count;
    return QTSQ_OK;
}
