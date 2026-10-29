/*
 * sfb_quant.c — Sofuu Brain v2: INT8 scalar quantisation, one scale per block.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/). Implements exactly the
 * quantisation block of sfb_internal.h.
 *
 * Layout: a block owns ONE allocation of n*dim int8_t, vector i at offset
 * i*dim. One scale covers the whole block: scale = max|v| / 127, so the
 * largest magnitude in the block maps exactly onto ±127 and the worst case
 * per-element error is 0.5 * scale, i.e. < 0.4% of the block maximum.
 *
 * W3.6 adds the per-record form (sfb_quant_encode_vec / sfb_quant_decode_vec)
 * used by the segment payload: one scale per record, same arithmetic. See the
 * comment above those two functions.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <math.h>

/* All-zero (or denormal-only) block: a real scale would be 0 and every code
 * 0/0. Pick a tiny positive scale instead so the block stays decodable. */
#define SFB_QUANT_SCALE_EPS 1e-8f

int sfb_quant_encode(sfb_quant_block_t *b, const float *vecs, uint32_t n, uint32_t dim)
{
    if (!b || !vecs || n == 0 || dim == 0) return SFB_E_INVALID;
    if (n > SIZE_MAX / dim) return SFB_E_INVALID;

    const size_t total = (size_t)n * (size_t)dim;

    float vmax = 0.0f;
    for (size_t i = 0; i < total; i++) {
        float v = vecs[i];
        if (!isfinite(v)) return SFB_E_NONFINITE;
        float a = fabsf(v);
        if (a > vmax) vmax = a;
    }

    float scale = vmax / 127.0f;
    if (!isfinite(scale) || !(scale > 0.0f)) scale = SFB_QUANT_SCALE_EPS;

    /* Reuse the old allocation when the shape already matches. */
    if (!b->codes ||
        (uint64_t)b->n * (uint64_t)b->dim != (uint64_t)total) {
        int8_t *fresh = (int8_t *)malloc(total * sizeof *fresh);
        if (!fresh) return SFB_E_NOMEM;
        free(b->codes);
        b->codes = fresh;
    }

    const float inv = 1.0f / scale;
    for (size_t i = 0; i < total; i++) {
        long q = lrintf(vecs[i] * inv);
        if (q > 127) q = 127;
        if (q < -127) q = -127;          /* symmetric range, -128 unused */
        b->codes[i] = (int8_t)q;
    }

    b->scale = scale;
    b->n = n;
    b->dim = dim;
    return SFB_OK;
}

int sfb_quant_decode(const sfb_quant_block_t *b, uint32_t i, float *out)
{
    if (!b || !out || !b->codes || b->dim == 0 || i >= b->n) return SFB_E_INVALID;
    const int8_t *c = b->codes + (size_t)i * (size_t)b->dim;
    for (uint32_t k = 0; k < b->dim; k++) out[k] = (float)c[k] * b->scale;
    return SFB_OK;
}

/* Squared L2 between the query and the decoded vector, evaluated in code
 * space: (codes[k]*scale - q[k])^2. Accumulated in double so that long
 * dimensions do not lose the small residuals.
 */
int sfb_quant_distance(const sfb_quant_block_t *b, uint32_t i, const float *q, float *out)
{
    if (!b || !q || !out || !b->codes || b->dim == 0 || i >= b->n) return SFB_E_INVALID;

    const int8_t *c = b->codes + (size_t)i * (size_t)b->dim;
    const double s = (double)b->scale;
    double acc = 0.0;
    for (uint32_t k = 0; k < b->dim; k++) {
        double d = (double)c[k] * s - (double)q[k];
        acc += d * d;
    }
    *out = (float)acc;
    return SFB_OK;
}

void sfb_quant_free(sfb_quant_block_t *b)
{
    if (!b) return;
    free(b->codes);
    b->codes = NULL;
    b->scale = 0.0f;
    b->n = 0;
    b->dim = 0;
}

/* ── per-record form (W3.6) ──────────────────────────────────────────────
 * The segment payload stores ONE f32 scale per record instead of one per block,
 * because a segment holds unrelated memories whose magnitudes differ by orders
 * of magnitude; one global scale would clip the large ones or flatten the small
 * ones to zero.  The rule is byte-for-byte the block rule above, applied to one
 * vector, so the writer (sfb_brain.c), the reader (replay) and the tests all
 * agree on what a stored code means:
 *
 *   scale = max|v| / 127          (SFB_QUANT_SCALE_EPS when the row is all 0)
 *   code  = clamp(lrint(v/scale), -127, 127)      symmetric, -128 unused
 *   v'    = code * scale                          f32 multiply, exact
 *
 * Consequences the callers rely on:
 *   - the round trip is exact: encode(decode(codes,scale)) == (scale,codes),
 *     so an int8 segment read back and rewritten yields the same bytes;
 *   - an identical input vector yields an identical (scale,codes) pair, which
 *     is how the brain still recognises "same text + same vector" in int8 mode.
 */
int sfb_quant_encode_vec(const float *v, uint32_t dim, int8_t *codes,
                         float *scale_out)
{
    float vmax = 0.0f, scale, inv;

    if (v == NULL || codes == NULL || scale_out == NULL || dim == 0)
        return SFB_E_INVALID;

    for (uint32_t k = 0; k < dim; k++) {
        float a;

        if (!isfinite(v[k])) return SFB_E_NONFINITE;
        a = v[k] < 0.0f ? -v[k] : v[k];
        if (a > vmax) vmax = a;
    }

    scale = vmax / 127.0f;
    if (!isfinite(scale) || !(scale > 0.0f)) scale = SFB_QUANT_SCALE_EPS;

    inv = 1.0f / scale;
    for (uint32_t k = 0; k < dim; k++) {
        long q = lrintf(v[k] * inv);

        if (q > 127) q = 127;
        if (q < -127) q = -127;
        codes[k] = (int8_t)q;
    }
    *scale_out = scale;
    return SFB_OK;
}

int sfb_quant_decode_vec(const int8_t *codes, uint32_t dim, float scale,
                         float *out)
{
    if (codes == NULL || out == NULL || dim == 0)
        return SFB_E_INVALID;
    if (!isfinite(scale) || !(scale > 0.0f))
        return SFB_E_INVALID;          /* corrupt scale: refuse the row      */
    for (uint32_t k = 0; k < dim; k++)
        out[k] = (float)codes[k] * scale;
    return SFB_OK;
}
