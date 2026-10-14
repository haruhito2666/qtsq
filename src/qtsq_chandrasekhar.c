/*
 * Quantum Tensor Sequence — Chandrasekhar Limit
 * Author: Haruhito
 */

#include "qtsq.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void qtsq_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

int qtsq_compress_white_dwarf(qtsq_context_t *ctx,
                             const uint8_t *data, size_t size) {
    if (!ctx || !data) return QTSQ_ERR_NULL;

    ctx->header.strategy = QTSQ_STRATEGY_NONE;
    ctx->header.data_type = QTSQ_TYPE_RAW;
    ctx->header.original_size = size;
    ctx->header.timestamp = (uint64_t)time(NULL);
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;

    qtsq_sha256(data, size, ctx->header.checksum);

    ctx->singularity = malloc(size);
    if (!ctx->singularity) return QTSQ_ERR_ALLOC;
    memcpy(ctx->singularity, data, size);
    ctx->singularity_size = size;
    ctx->header.singularity_size = size;

    ctx->header.horizon_size = 0;
    ctx->header.accretion_size = 0;
    ctx->header.radiation_size = 0;

    return QTSQ_OK;
}

int qtsq_below_chandrasekhar(size_t size) {
    return (size < QTSQ_CHANDRASEKHAR_LIMIT);
}
