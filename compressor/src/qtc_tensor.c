/*
 * qtc_tensor.c — Phase 5.6: lossless float32 tensor lens.
 *
 * QTSQ's neural payloads (QTSQ_TYPE_TENSOR, TABLE_FLOAT, and the float
 * records inside surfel/mesh sets) are arrays of float32. Generic LZ on
 * raw float bytes is weak: the mantissa low bytes are near-random even
 * when the VALUES vary smoothly, so repetition hides from the match
 * finder.
 *
 * Two reversible transforms fix that, and the encoder runs both as a
 * contest (best-of-all, the project's standing philosophy):
 *
 *   mode 0 (planes)     — byte-transpose: the 4 bytes of every float are
 *                         deinterleaved into 4 contiguous planes. Byte 3
 *                         (exponent+sign) becomes a smooth, highly
 *                         repetitive plane; byte 0 carries the noisy
 *                         mantissa. LZ then sees long same-position runs.
 *   mode 1 (xdelta)     — XOR each float's bit pattern with its
 *                         predecessor first (the classic float-delta
 *                         trick: slowly varying series cancel to tiny
 *                         integers), THEN byte-transpose.
 *
 *   mode 4 (planes)     — xdelta first, then each byte plane is coded
 *                         independently (LZ or verbatim). Weight tensors
 *                         have mixed-entropy planes: the exponent plane
 *                         compresses while mantissa planes store raw.
 *
 * All bit-exact; the smaller payload wins. Output that does
 * not beat the raw size is left to the caller to store (the typed API
 * falls back to the STORED lens, like every other lens).
 *
 * v0.9.0 (optimization #10): LOSSY modes for neural-weight payloads,
 * selected by qtc_options_t.mode == QTC_MODE_LOSSY. Each float32 is
 * requantized to a 16-bit float, XOR-delta'd, transposed into 2 byte
 * planes and LZ'd — halving the pre-LZ size on top of everything else:
 *
 *   mode 2 (bf16)  — quality ≥ 0.5: bfloat16 (8-bit exponent like f32,
 *                    7-bit mantissa). No range surprises; ~2^-8 relative
 *                    error. This is QTSQ's own f16-path precision class.
 *   mode 3 (f16)   — quality < 0.5: IEEE half (5-bit exponent, 10-bit
 *                    mantissa, round-to-nearest-even). Finer precision
 *                    (~2^-11) for values inside its range; overflow
 *                    saturates to ±inf, deep underflow flushes to 0.
 *
 * Decode expands back to float32; the container's original_size stays
 * count*4, so the typed and generic decode paths are unchanged.
 *
 * Payload layout:
 *   [u32 count][u8 mode][u8 reserved][LZ of the transposed bytes]
 *   (modes 0/1: count*4 bytes; modes 2/3: count*2 bytes)
 *   mode 7 (store level): raw floats verbatim, no LZ.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

#if !defined(QTC_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#include <pthread.h>
#include <stdatomic.h>
#define TENSOR_THREADS 1
/* Cooperative thread budget: candidate-level threads run only when no
 * other tensor encode is active (e.g. under a cross-tensor pool each
 * worker runs a serial contest — no oversubscription, deterministic). */
static atomic_int qtc_encodes_active = 0;
#else
#define TENSOR_THREADS 0
#endif

#define TENSOR_HDR 6u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Deinterleave count floats (mode's XOR pre-pass already applied by the
 * caller when requested) into 4 byte planes. */
static void transpose_planes(const uint32_t *u, size_t count, uint8_t *out)
{
    const uint8_t *src = (const uint8_t *)u;
    for (size_t i = 0; i < count; i++) {
        out[i]           = src[i * 4];
        out[count + i]   = src[i * 4 + 1];
        out[2 * count + i] = src[i * 4 + 2];
        out[3 * count + i] = src[i * 4 + 3];
    }
}

static void untranspose_planes(const uint8_t *in, size_t count, uint32_t *u)
{
    uint8_t *dst = (uint8_t *)u;
    for (size_t i = 0; i < count; i++) {
        dst[i * 4]     = in[i];
        dst[i * 4 + 1] = in[count + i];
        dst[i * 4 + 2] = in[2 * count + i];
        dst[i * 4 + 3] = in[3 * count + i];
    }
}

/* ── 16-bit float conversions (lossy modes, v0.9.0) ──────────────────── */

/* bfloat16: top 16 bits of f32 with round-to-nearest-even. */
static uint16_t f32_to_bf16(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t lsb = (x >> 16) & 1u;
    x += 0x7FFFu + lsb;
    return (uint16_t)(x >> 16);
}

static float bf16_to_f32(uint16_t b)
{
    uint32_t x = (uint32_t)b << 16;
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* IEEE half: 5-bit exponent, 10-bit mantissa, round-to-nearest-even.
 * Overflow saturates to ±inf; deep underflow flushes to ±0. */
static uint16_t f32_to_f16(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (uint16_t)((x >> 16) & 0x8000u);
    uint32_t fexp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;

    if (fexp == 0xFFu)                      /* inf / nan */
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));

    int32_t exp = (int32_t)fexp - 127 + 15;
    if (exp >= 0x1F)
        return (uint16_t)(sign | 0x7C00u);  /* overflow → inf */
    if (exp <= 0) {
        if (exp < -10)
            return (uint16_t)sign;          /* underflow → 0 */
        mant |= 0x800000u;                  /* implicit bit */
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t hm = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1u);
        uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (hm & 1u)))
            hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t hm = mant >> 13;
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) {
        hm++;
        if (hm > 0x3FFu) {                  /* mantissa carry */
            hm = 0;
            exp++;
        }
        if (exp >= 0x1F)
            return (uint16_t)(sign | 0x7C00u);
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | hm);
}

static float f16_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {                            /* subnormal */
            int32_t e = -14;
            while (!(mant & 0x400u)) { mant <<= 1; e--; }
            mant &= 0x3FFu;
            x = sign | ((uint32_t)(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* Deinterleave count 16-bit values into 2 byte planes. */
static void transpose_planes16(const uint16_t *u, size_t count, uint8_t *out)
{
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint8_t)(u[i] & 0xFF);
        out[count + i] = (uint8_t)(u[i] >> 8);
    }
}

static void untranspose_planes16(const uint8_t *in, size_t count, uint16_t *u)
{
    for (size_t i = 0; i < count; i++)
        u[i] = (uint16_t)(in[i] | ((uint16_t)in[count + i] << 8));
}

/* Encode one mode; returns QTC_OK with a malloc'd payload (header + LZ). */
static int tensor_try(const uint32_t *u, size_t count, uint8_t mode,
                      uint8_t **out, size_t *out_len){
    size_t raw = count * 4;
    uint32_t *work = (uint32_t *)malloc(raw);
    uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
    if (!work || !planes) {
        free(work);
        free(planes);
        return QTC_ERR_ALLOC;
    }

    if (mode == 1) {
        work[0] = u[0];
        for (size_t i = 1; i < count; i++)
            work[i] = u[i] ^ u[i - 1];
    } else if (mode == 6) {
        /* Second-order arithmetic delta (mod 2^32, bit-exact):
         * d2[i] = u[i] - 2*u[i-1] + u[i-2]. Locally-linear series
         * collapse toward zero; anything else loses the contest. */
        if (count > 0) work[0] = u[0];
        if (count > 1) work[1] = u[1] - u[0];
        for (size_t i = 2; i < count; i++)
            work[i] = u[i] - 2u * u[i - 1] + u[i - 2];
    } else {
        memcpy(work, u, raw);
    }
    transpose_planes(work, count, planes);
    free(work);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(planes, raw, &lz, &lz_len);
    free(planes);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(TENSOR_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, (uint32_t)count);
    buf[4] = mode;
    buf[5] = 0;
    memcpy(buf + TENSOR_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = TENSOR_HDR + lz_len;
    return QTC_OK;
}

/* Per-plane candidate (mode 4): XOR-delta the values, transpose into
 * byte planes, then code EACH plane independently (LZ or verbatim).
 * Weight tensors have mixed-entropy planes — the exponent plane
 * compresses while mantissa planes are near-random — so per-plane
 * choice beats whole-buffer all-or-nothing. Bit-exact.
 *
 * Framing: [u32 count][u8 mode=4][u8 0][u32 len0..3][plane payloads].
 * len == 0xFFFFFFFF marks a verbatim plane (count raw bytes follow). */
#define PLANE_STORED 0xFFFFFFFFu

static int tensor_try_planes(const uint32_t *u, size_t count,
                             uint8_t **out, size_t *out_len)
{
    uint32_t *work = (uint32_t *)malloc(count * 4);
    uint8_t *planes = (uint8_t *)malloc(count ? count * 4 : 1);
    if (!work || !planes) {
        free(work);
        free(planes);
        return QTC_ERR_ALLOC;
    }
    work[0] = u[0];
    for (size_t i = 1; i < count; i++)
        work[i] = u[i] ^ u[i - 1];
    transpose_planes(work, count, planes);
    free(work);

    uint8_t *buf = (uint8_t *)malloc(TENSOR_HDR + 16 + count * 4 + 64);
    if (!buf) {
        free(planes);
        return QTC_ERR_ALLOC;
    }
    put32(buf, (uint32_t)count);
    buf[4] = 4;
    buf[5] = 0;
    size_t p = TENSOR_HDR + 16;

    for (int e = 0; e < 4; e++) {
        uint8_t *lz = NULL;
        size_t lz_len = 0;
        int rc = qtc_lz_encode(planes + (size_t)e * count, count, &lz, &lz_len);
        if (rc != QTC_OK) {
            free(planes);
            free(buf);
            return rc;
        }
        if (lz_len < count) {
            put32(buf + TENSOR_HDR + (size_t)e * 4, (uint32_t)lz_len);
            memcpy(buf + p, lz, lz_len);
            p += lz_len;
        } else {
            /* Incompressible plane: verbatim, no LZ tax. */
            put32(buf + TENSOR_HDR + (size_t)e * 4, PLANE_STORED);
            memcpy(buf + p, planes + (size_t)e * count, count);
            p += count;
        }
        free(lz);
    }
    free(planes);

    uint8_t *shrunk = (uint8_t *)realloc(buf, p);
    *out = shrunk ? shrunk : buf;
    *out_len = p;
    return QTC_OK;
}

/* Contest job: one candidate mode. File-scope so worker threads share it. */
typedef struct {
    const uint32_t *u;
    size_t count;
    uint8_t mode;   /* 0, 1, 6 via tensor_try; 4 via tensor_try_planes */
    uint8_t *out;
    size_t out_len;
    int rc;
} tensor_job_t;

static void tensor_job_run(tensor_job_t *j)
{
    if (j->mode == 4)
        j->rc = tensor_try_planes(j->u, j->count, &j->out, &j->out_len);
    else
        j->rc = tensor_try(j->u, j->count, j->mode, &j->out, &j->out_len);
}

#if TENSOR_THREADS
static void *tensor_job_fn(void *arg)
{
    tensor_job_run((tensor_job_t *)arg);
    return NULL;
}
#endif

#if TENSOR_THREADS
/* Claim candidate-level parallelism. Succeeds only when no other
 * encode holds the budget (CAS); the holder releases it after the
 * contest. Keeps total threads bounded under nested/pooled encodes. */
static int tensor_claim_parallel(void)
{
    int expected = 0;
    return atomic_compare_exchange_strong(&qtc_encodes_active, &expected, 1);
}

static void tensor_release_parallel(void)
{
    atomic_store(&qtc_encodes_active, 0);
}
#endif

int qtc_tensor_encode(const float *data, size_t count,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts)
{
    if (!data && count > 0)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (count > ((size_t)1 << 30))
        return QTC_ERR_SIZE;

    /* Level 0 (store): no contest — verbatim framing at memcpy speed.
     * Mode 7 = raw floats verbatim. Still bit-exact + checksummed
     * upstream; ratio ~1.0 by design (safetensors-class speed). */
    if (opts && opts->level == 0) {
        size_t raw = count * 4;
        uint8_t *buf = (uint8_t *)malloc(TENSOR_HDR + raw);
        if (!buf)
            return QTC_ERR_ALLOC;
        put32(buf, (uint32_t)count);
        buf[4] = 7;
        buf[5] = 0;
        if (raw)
            memcpy(buf + TENSOR_HDR, data, raw);
        *out = buf;
        *out_len = TENSOR_HDR + raw;
        return QTC_OK;
    }

    /* Contest: bit-exact modes; the smaller payload wins. Full contest
     * runs modes 0,1,6,4; fast (opts->level == 1) sample-gates {0,4}:
     * both run on a 256K-unit prefix, only the sample winner runs full —
     * still bit-exact, different (usually slightly larger) bytes.
     * Candidates run on worker threads when available (deterministic:
     * ties resolve in mode order, so bytes never change). */
    const uint32_t *u = (const uint32_t *)data;
    static const uint8_t full_modes[4] = { 0, 1, 6, 4 };
    static const uint8_t fast_modes[2] = { 0, 4 };
    static const uint8_t gated0[1] = { 0 };
    static const uint8_t gated4[1] = { 4 };
    const uint8_t *cand_modes = full_modes;
    int ncand = 4;
    if (opts && opts->level == 1) {
        if (count > 262144) {
            /* Sample gate: rank {0,4} on a 256K-unit prefix, full-run
             * only the sample winner. */
            uint8_t *s0 = NULL, *s4 = NULL;
            size_t l0 = 0, l4 = 0;
            int r0 = tensor_try(u, 65536, 0, &s0, &l0);
            int r4 = tensor_try_planes(u, 65536, &s4, &l4);
            if (r0 == QTC_OK && r4 == QTC_OK) {
                cand_modes = (l4 < l0) ? gated4 : gated0;
            } else if (r0 == QTC_OK) {
                cand_modes = gated0;
            } else if (r4 == QTC_OK) {
                cand_modes = gated4;
            } else {
                free(s0);
                free(s4);
                return r0;
            }
            free(s0);
            free(s4);
            ncand = 1;
        } else {
            cand_modes = fast_modes;
            ncand = 2;
        }
    }
    tensor_job_t jobs[4];
    for (int k = 0; k < ncand; k++) {
        jobs[k].u = u;
        jobs[k].count = count;
        jobs[k].mode = cand_modes[k];
        jobs[k].out = NULL;
        jobs[k].out_len = 0;
        jobs[k].rc = -1;
    }
#if TENSOR_THREADS
    {
        pthread_t tids[4];
        int nstarted = 0;
        /* Small inputs: threads cost more than they save. Nested or
         * pooled encodes run serially (budget claimed by another). */
        int use_threads = (count >= 65536) && tensor_claim_parallel();
        if (use_threads) {
            for (int k = 0; k < ncand; k++) {
                if (pthread_create(&tids[k], NULL, tensor_job_fn,
                                   &jobs[k]) == 0)
                    nstarted++;
                else
                    break;
            }
        }
        for (int k = nstarted; k < ncand; k++)
            tensor_job_run(&jobs[k]);
        for (int k = 0; k < nstarted; k++)
            pthread_join(tids[k], NULL);
        if (use_threads)
            tensor_release_parallel();
    }
#else
    for (int k = 0; k < ncand; k++)
        tensor_job_run(&jobs[k]);
#endif
    uint8_t *best = NULL;
    size_t best_len = 0;
    for (int k = 0; k < ncand; k++) {
        if (jobs[k].rc != QTC_OK) {
            for (int j = 0; j < ncand; j++) free(jobs[j].out);
            return jobs[k].rc;
        }
    }
    for (int k = 0; k < ncand; k++) {
        if (!best || jobs[k].out_len < best_len) {
            free(best);
            best = jobs[k].out;
            best_len = jobs[k].out_len;
            jobs[k].out = NULL;
        }
    }
    for (int k = 0; k < ncand; k++) free(jobs[k].out);
    if (!best)
        return QTC_ERR_ALLOC;

    *out = best;
    *out_len = best_len;
    return QTC_OK;
}

/* Lossy path (v0.9.0): requantize to 16-bit floats, XOR-delta, transpose
 * into 2 planes, LZ. mode 2 = bf16, mode 3 = f16. */
int qtc_tensor_encode_lossy(const float *data, size_t count, uint8_t mode,
                            uint8_t **out, size_t *out_len)
{
    if (!data || count == 0)
        return QTC_ERR_FORMAT;
    if (mode != 2 && mode != 3)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (count > ((size_t)1 << 30))
        return QTC_ERR_SIZE;

    uint16_t *h = (uint16_t *)malloc(count * sizeof(uint16_t));
    uint8_t *planes = (uint8_t *)malloc(count ? count * 2 : 1);
    if (!h || !planes) {
        free(h);
        free(planes);
        return QTC_ERR_ALLOC;
    }

    for (size_t i = 0; i < count; i++)
        h[i] = (mode == 2) ? f32_to_bf16(data[i]) : f32_to_f16(data[i]);

    /* XOR-delta on the 16-bit patterns. */
    for (size_t i = count; i-- > 1;)
        h[i] = (uint16_t)(h[i] ^ h[i - 1]);

    transpose_planes16(h, count, planes);
    free(h);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(planes, count * 2, &lz, &lz_len);
    free(planes);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(TENSOR_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, (uint32_t)count);
    buf[4] = mode;
    buf[5] = 0;
    memcpy(buf + TENSOR_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = TENSOR_HDR + lz_len;
    return QTC_OK;
}

int qtc_tensor_decode(const uint8_t *payload, size_t payload_len,
                      float **out_data, size_t *out_count){
    if (!payload || !out_data || !out_count)
        return QTC_ERR_ALLOC;
    *out_data = NULL;
    *out_count = 0;

    if (payload_len < TENSOR_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t count = get32(payload);
    uint8_t mode = payload[4];
    if (count == 0 || mode > 7 || mode == 5)
        return QTC_ERR_CORRUPT;
    if (count > ((uint32_t)1 << 30))
        return QTC_ERR_SIZE;

    float *out = (float *)malloc((size_t)count * 4);
    if (!out)
        return QTC_ERR_ALLOC;

    if (mode == 7) {
        /* Verbatim raw floats (store level). */
        if (payload_len - TENSOR_HDR != (size_t)count * 4) {
            free(out);
            return QTC_ERR_CORRUPT;
        }
        memcpy(out, payload + TENSOR_HDR, (size_t)count * 4);
        *out_data = out;
        *out_count = count;
        return QTC_OK;
    }

    if (mode == 4) {
        /* Per-plane layout: [hdr][4 x u32 lens][planes]. */
        if (payload_len < TENSOR_HDR + 16)
            goto corrupt_out;
        uint32_t lens[4];
        for (int e = 0; e < 4; e++) {
            lens[e] = (uint32_t)payload[TENSOR_HDR + (size_t)e * 4] |
                      ((uint32_t)payload[TENSOR_HDR + (size_t)e * 4 + 1] << 8) |
                      ((uint32_t)payload[TENSOR_HDR + (size_t)e * 4 + 2] << 16) |
                      ((uint32_t)payload[TENSOR_HDR + (size_t)e * 4 + 3] << 24);
            if (lens[e] != PLANE_STORED && lens[e] > count + 64)
                goto corrupt_out;
        }
        uint8_t *planes = (uint8_t *)malloc((size_t)count * 4);
        uint32_t *u = (uint32_t *)malloc((size_t)count * 4);
        if (!planes || !u) {
            free(planes); free(u);
            free(out);
            return QTC_ERR_ALLOC;
        }
        size_t off = TENSOR_HDR + 16;
        for (int e = 0; e < 4; e++) {
            if (lens[e] == PLANE_STORED) {
                if (off + count > payload_len)
                    goto corrupt_planes;
                memcpy(planes + (size_t)e * count, payload + off, count);
                off += count;
            } else {
                if (off + lens[e] > payload_len)
                    goto corrupt_planes;
                size_t consumed = 0;
                int rc = qtc_lz_decode(payload + off, lens[e],
                                       count, planes + (size_t)e * count,
                                       &consumed);
                if (rc != QTC_OK)
                    goto corrupt_planes;
                off += lens[e];
            }
        }
        if (off != payload_len)
            goto corrupt_planes;
        untranspose_planes(planes, count, u);
        free(planes);
        for (size_t i = 1; i < count; i++)
            u[i] ^= u[i - 1];
        memcpy(out, u, (size_t)count * 4);
        free(u);
        *out_data = out;
        *out_count = count;
        return QTC_OK;
corrupt_planes:
        free(planes);
        free(u);
corrupt_out:
        free(out);
        return QTC_ERR_CORRUPT;
    }

    if (mode <= 1 || mode == 6) {
        size_t raw = (size_t)count * 4;
        uint8_t *planes = (uint8_t *)malloc(raw);
        uint32_t *u = (uint32_t *)malloc(raw);
        if (!planes || !u) {
            free(planes); free(u); free(out);
            return QTC_ERR_ALLOC;
        }

        size_t consumed = 0;
        int rc = qtc_lz_decode(payload + TENSOR_HDR, payload_len - TENSOR_HDR,
                               raw, planes, &consumed);
        if (rc != QTC_OK) {
            free(planes); free(u); free(out);
            return rc;
        }

        untranspose_planes(planes, count, u);
        free(planes);

        if (mode == 1) {
            for (size_t i = 1; i < count; i++)
                u[i] ^= u[i - 1];
        } else if (mode == 6) {
            /* Invert d2[i] = u[i] - 2*u[i-1] + u[i-2] (mod 2^32):
             * u[0], u[1] stored directly (u[1] as first difference). */
            if (count > 1)
                u[1] += u[0];
            for (size_t i = 2; i < count; i++)
                u[i] += 2u * u[i - 1] - u[i - 2];
        }
        memcpy(out, u, raw);
        free(u);
    } else {
        /* Lossy modes: 2 planes of 16-bit values, expanded to f32. */
        size_t raw16 = (size_t)count * 2;
        uint8_t *planes = (uint8_t *)malloc(raw16);
        uint16_t *h = (uint16_t *)malloc(raw16);
        if (!planes || !h) {
            free(planes); free(h); free(out);
            return QTC_ERR_ALLOC;
        }

        size_t consumed = 0;
        int rc = qtc_lz_decode(payload + TENSOR_HDR, payload_len - TENSOR_HDR,
                               raw16, planes, &consumed);
        if (rc != QTC_OK) {
            free(planes); free(h); free(out);
            return rc;
        }

        untranspose_planes16(planes, count, h);
        free(planes);

        for (size_t i = 1; i < count; i++)
            h[i] = (uint16_t)(h[i] ^ h[i - 1]);
        for (size_t i = 0; i < count; i++)
            out[i] = (mode == 2) ? bf16_to_f32(h[i]) : f16_to_f32(h[i]);
        free(h);
    }

    *out_data = out;
    *out_count = count;
    return QTC_OK;
}

/* ── Generic same-element array lens (quantized weights, int sensors) ──
 *
 * Same contest philosophy as the float32 lens, generalized to 1/2/4/8
 * byte elements: transpose the element bytes into elem planes (mode 0),
 * or XOR-delta whole elements first and then transpose (mode 1). For
 * packed-nibble payloads (int4) a nibble-split candidate (mode 2) joins:
 * high nibbles and low nibbles become two byte planes. All bit-exact;
 * each candidate is LZ'd and the smallest wins.
 *
 * Payload layout: [u32 count][u8 mode][u8 elem][LZ of planes]
 * count = element count (nibble mode: packed-byte count). */

#define ARRAY_HDR 6u

static void transpose_elem(const uint8_t *src, size_t count, uint8_t elem,
                           uint8_t *out)
{
    for (uint8_t e = 0; e < elem; e++)
        for (size_t i = 0; i < count; i++)
            out[(size_t)e * count + i] = src[i * elem + e];
}

static void untranspose_elem(const uint8_t *in, size_t count, uint8_t elem,
                             uint8_t *dst)
{
    for (uint8_t e = 0; e < elem; e++)
        for (size_t i = 0; i < count; i++)
            dst[i * elem + e] = in[(size_t)e * count + i];
}

static int array_try(const uint8_t *data, size_t count, uint8_t elem,
                     uint8_t mode, uint8_t **out, size_t *out_len)
{
    size_t raw = count * elem;
    uint8_t *work = (uint8_t *)malloc(raw ? raw : 1);
    uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
    if (!work || !planes) {
        free(work);
        free(planes);
        return QTC_ERR_ALLOC;
    }

    if (mode == 2) {
        /* Nibble-split: high nibbles → plane 0, low nibbles → plane 1. */
        for (size_t i = 0; i < count; i++) {
            planes[i] = (uint8_t)(data[i] >> 4);
            planes[count + i] = (uint8_t)(data[i] & 0x0F);
        }
    } else if (mode == 5) {
        /* Second-order arithmetic delta per byte lane (mod 256,
         * bit-exact). Element 0 stored raw, element 1 as first
         * difference, rest as delta-of-delta. The second-difference
         * pass runs FIRST (descending, reading pristine values);
         * the first difference is derived last. */
        memcpy(work, data, raw);
        for (size_t i = count; i-- > 2;) {
            for (uint8_t b = 0; b < elem; b++) {
                uint8_t x = work[i * elem + b];
                uint8_t y = work[(i - 1) * elem + b];
                uint8_t z = work[(i - 2) * elem + b];
                work[i * elem + b] = (uint8_t)(x - 2u * y + z);
            }
        }
        if (count > 1) {
            for (uint8_t b = 0; b < elem; b++)
                work[elem + b] = (uint8_t)(work[elem + b] - work[b]);
        }
        transpose_elem(work, count, elem, planes);
    } else {
        if (mode == 1) {
            memcpy(work, data, raw);
            for (size_t i = count; i-- > 1;)
                for (uint8_t b = 0; b < elem; b++)
                    work[i * elem + b] ^= work[(i - 1) * elem + b];
        } else {
            memcpy(work, data, raw);
        }
        transpose_elem(work, count, elem, planes);
    }
    free(work);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(planes, raw, &lz, &lz_len);
    free(planes);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(ARRAY_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, (uint32_t)count);
    buf[4] = mode;
    buf[5] = elem;
    memcpy(buf + ARRAY_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = ARRAY_HDR + lz_len;
    return QTC_OK;
}

/* Per-plane candidate for multi-byte elements (mode 4): XOR-delta
 * whole elements, transpose, then code each plane independently
 * (LZ or verbatim). Same mixed-entropy win as the float32 mode 4.
 * Framing: [u32 count][u8 mode=4][u8 elem][lens...][planes]. */
static int array_try_planes(const uint8_t *data, size_t count, uint8_t elem,
                            uint8_t **out, size_t *out_len)
{
    size_t raw = count * elem;
    uint8_t *work = (uint8_t *)malloc(raw ? raw : 1);
    uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
    if (!work || !planes) {
        free(work);
        free(planes);
        return QTC_ERR_ALLOC;
    }
    memcpy(work, data, raw);
    for (size_t i = count; i-- > 1;)
        for (uint8_t b = 0; b < elem; b++)
            work[i * elem + b] ^= work[(i - 1) * elem + b];
    transpose_elem(work, count, elem, planes);
    free(work);

    uint8_t *buf = (uint8_t *)malloc(ARRAY_HDR + (size_t)elem * 4 + raw + 64);
    if (!buf) {
        free(planes);
        return QTC_ERR_ALLOC;
    }
    put32(buf, (uint32_t)count);
    buf[4] = 4;
    buf[5] = elem;
    size_t p = ARRAY_HDR + (size_t)elem * 4;

    for (uint8_t e = 0; e < elem; e++) {
        uint8_t *lz = NULL;
        size_t lz_len = 0;
        int rc = qtc_lz_encode(planes + (size_t)e * count, count, &lz, &lz_len);
        if (rc != QTC_OK) {
            free(planes);
            free(buf);
            return rc;
        }
        if (lz_len < count) {
            put32(buf + ARRAY_HDR + (size_t)e * 4, (uint32_t)lz_len);
            memcpy(buf + p, lz, lz_len);
            p += lz_len;
        } else {
            put32(buf + ARRAY_HDR + (size_t)e * 4, PLANE_STORED);
            memcpy(buf + p, planes + (size_t)e * count, count);
            p += count;
        }
        free(lz);
    }
    free(planes);

    uint8_t *shrunk = (uint8_t *)realloc(buf, p);
    *out = shrunk ? shrunk : buf;
    *out_len = p;
    return QTC_OK;
}

/* Array contest job: one candidate mode. File-scope for workers. */
typedef struct {
    const uint8_t *data;
    size_t count;
    uint8_t elem;
    uint8_t mode;   /* 0,1,2,5 via array_try; 4 via array_try_planes */
    uint8_t *out;
    size_t out_len;
    int rc;
} array_job_t;

static void array_job_run(array_job_t *j)
{
    if (j->mode == 4)
        j->rc = array_try_planes(j->data, j->count, j->elem,
                                 &j->out, &j->out_len);
    else
        j->rc = array_try(j->data, j->count, j->elem, j->mode,
                          &j->out, &j->out_len);
}

#if TENSOR_THREADS
static void *array_job_fn(void *arg)
{
    array_job_run((array_job_t *)arg);
    return NULL;
}
#endif

int qtc_compress_array(const uint8_t *data, uint32_t count, uint8_t elem_size,
                       int packed_nibbles,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts){
    (void)opts;   /* NULL = full contest; level 1 = fast subset */
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if ((!data && count > 0) || count == 0)
        return QTC_ERR_FORMAT;
    if (elem_size != 1 && elem_size != 2 && elem_size != 4 && elem_size != 8)
        return QTC_ERR_FORMAT;

    size_t raw = (size_t)count * elem_size;

    /* Level 0 (store): skip the contest, emit STORED framing directly —
     * memcpy speed, ratio ~1.0, still bit-exact. */
    if (opts && opts->level == 0) {
        uint8_t *buf = (uint8_t *)malloc(ARRAY_HDR + raw);
        if (!buf)
            return QTC_ERR_ALLOC;
        put32(buf, count);
        buf[4] = 3;
        buf[5] = elem_size;
        if (raw)
            memcpy(buf + ARRAY_HDR, data, raw);
        *out = buf;
        *out_len = ARRAY_HDR + raw;
        return QTC_OK;
    }

    /* STORED fallback when the transform cannot help. */
    uint8_t lens = 0; /* resolved below */
    (void)lens;

    /* Contest: 0 raw-transpose, 1 xor-delta, 2 nibble-split (packed
     * int4 only), 5 second-order delta, 4 per-plane (multi-byte).
     * Fast (opts->level == 1) sample-gates {0,4} (+2): candidates run
     * on a 256K-unit prefix, only the sample winner runs full — still
     * bit-exact, different (usually slightly larger) bytes.
     * Workers run in parallel when available; ties resolve in mode
     * order, so bytes never change. */
    int fast = (opts && opts->level == 1);
    /* Fast sample gate needs the same candidate set as the full fast
     * path; compute it once into a mode list. */
    uint8_t fast_modes[3];
    int nfast = 0;
    fast_modes[nfast++] = 0;
    if (packed_nibbles && elem_size == 1)
        fast_modes[nfast++] = 2;
    if (elem_size > 1)
        fast_modes[nfast++] = 4;
    uint8_t gate_winner = 0;
    int gated = 0;
    if (fast && count > 262144) {
        size_t sc = 65536;
        if (sc > count) sc = count;
        uint8_t *sp[3] = { NULL, NULL, NULL };
        size_t sl[3] = { 0, 0, 0 };
        int sr[3] = { -1, -1, -1 };
        for (int k = 0; k < nfast; k++) {
            if (fast_modes[k] == 4)
                sr[k] = array_try_planes(data, sc, elem_size, &sp[k], &sl[k]);
            else
                sr[k] = array_try(data, sc, elem_size, fast_modes[k],
                                  &sp[k], &sl[k]);
        }
        int best_k = -1;
        for (int k = 0; k < nfast; k++) {
            if (sr[k] != QTC_OK) {
                free(sp[k]);
                continue;
            }
            if (best_k < 0 || sl[k] < sl[best_k])
                best_k = k;
            else
                free(sp[k]);
        }
        if (best_k < 0) {
            for (int k = 0; k < nfast; k++) free(sp[k]);
            return sr[0] != QTC_OK ? sr[0] : QTC_ERR_ALLOC;
        }
        free(sp[best_k]);
        gate_winner = fast_modes[best_k];
        gated = 1;
    }
    array_job_t ajobs[5];
    int naj = 0;
    if (gated) {
        ajobs[naj].data = data; ajobs[naj].count = count;
        ajobs[naj].elem = elem_size; ajobs[naj].mode = gate_winner;
        ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
    } else {
        ajobs[naj].data = data; ajobs[naj].count = count;
        ajobs[naj].elem = elem_size; ajobs[naj].mode = 0;
        ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
        if (!fast) {
            ajobs[naj].data = data; ajobs[naj].count = count;
            ajobs[naj].elem = elem_size; ajobs[naj].mode = 1;
            ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
        }
        if (packed_nibbles && elem_size == 1) {
            ajobs[naj].data = data; ajobs[naj].count = count;
            ajobs[naj].elem = elem_size; ajobs[naj].mode = 2;
            ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
        }
        if (!fast) {
            ajobs[naj].data = data; ajobs[naj].count = count;
            ajobs[naj].elem = elem_size; ajobs[naj].mode = 5;
            ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
        }
        if (elem_size > 1) {
            ajobs[naj].data = data; ajobs[naj].count = count;
            ajobs[naj].elem = elem_size; ajobs[naj].mode = 4;
            ajobs[naj].out = NULL; ajobs[naj].out_len = 0; ajobs[naj].rc = -1; naj++;
        }
    }
#if TENSOR_THREADS
    {
        pthread_t tids[5];
        int nstarted = 0;
        /* Small inputs: threads cost more than they save. Nested or
         * pooled encodes run serially (budget claimed by another). */
        int use_threads = (raw >= 65536) && tensor_claim_parallel();
        if (use_threads) {
            for (int k = 0; k < naj; k++) {
                if (pthread_create(&tids[k], NULL, array_job_fn,
                                   &ajobs[k]) == 0)
                    nstarted++;
                else
                    break;
            }
        }
        for (int k = nstarted; k < naj; k++)
            array_job_run(&ajobs[k]);
        for (int k = 0; k < nstarted; k++)
            pthread_join(tids[k], NULL);
        if (use_threads)
            tensor_release_parallel();
    }
#else
    for (int k = 0; k < naj; k++)
        array_job_run(&ajobs[k]);
#endif
    uint8_t *best = NULL;
    size_t best_len = 0;
    for (int k = 0; k < naj; k++) {
        if (ajobs[k].rc != QTC_OK) {
            for (int j = 0; j < naj; j++) free(ajobs[j].out);
            return ajobs[k].rc;
        }
    }
    for (int k = 0; k < naj; k++) {
        if (!best || ajobs[k].out_len < best_len) {
            free(best);
            best = ajobs[k].out;
            best_len = ajobs[k].out_len;
            ajobs[k].out = NULL;
        }
    }
    for (int k = 0; k < naj; k++) free(ajobs[k].out);
    if (!best)
        return QTC_ERR_ALLOC;

    if (!best || best_len >= raw) {
        /* STORED: emit the same header framing with mode 3 so decode
         * stays uniform (mode 3 = verbatim planes). */
        free(best);
        uint8_t *buf = (uint8_t *)malloc(ARRAY_HDR + raw);
        if (!buf)
            return QTC_ERR_ALLOC;
        put32(buf, count);
        buf[4] = 3;
        buf[5] = elem_size;
        if (raw)
            memcpy(buf + ARRAY_HDR, data, raw);
        *out = buf;
        *out_len = ARRAY_HDR + raw;
        return QTC_OK;
    }

    *out = best;
    *out_len = best_len;
    return QTC_OK;
}

int qtc_decompress_array(const uint8_t *in, size_t in_len,
                         uint8_t **out_data, uint32_t *out_count,
                         uint8_t *out_elem_size)
{
    if (!out_data || !out_count)
        return QTC_ERR_ALLOC;
    *out_data = NULL;
    *out_count = 0;

    if (!in || in_len < ARRAY_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t count = get32(in);
    uint8_t mode = in[4];
    uint8_t elem = in[5];
    if (count == 0 || mode > 5)
        return QTC_ERR_CORRUPT;
    if (elem != 1 && elem != 2 && elem != 4 && elem != 8)
        return QTC_ERR_CORRUPT;
    if (mode == 2 && elem != 1)
        return QTC_ERR_CORRUPT;
    if (mode == 4 && elem < 2)
        return QTC_ERR_CORRUPT;
    if (count > ((uint32_t)1 << 30))
        return QTC_ERR_SIZE;
    if ((size_t)count > ((size_t)-1) / elem)
        return QTC_ERR_CORRUPT;

    size_t raw = (size_t)count * elem;
    uint8_t *out = (uint8_t *)malloc(raw ? raw : 1);
    if (!out)
        return QTC_ERR_ALLOC;

    if (mode == 4) {
        size_t hdr = ARRAY_HDR + (size_t)elem * 4;
        if (in_len < hdr) {
            free(out);
            return QTC_ERR_CORRUPT;
        }
        uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
        if (!planes) {
            free(out);
            return QTC_ERR_ALLOC;
        }
        size_t off = hdr;
        int bad = 0;
        for (uint8_t e = 0; e < elem && !bad; e++) {
            uint32_t len = (uint32_t)in[ARRAY_HDR + (size_t)e * 4] |
                           ((uint32_t)in[ARRAY_HDR + (size_t)e * 4 + 1] << 8) |
                           ((uint32_t)in[ARRAY_HDR + (size_t)e * 4 + 2] << 16) |
                           ((uint32_t)in[ARRAY_HDR + (size_t)e * 4 + 3] << 24);
            if (len != PLANE_STORED && len > count + 64) {
                bad = 1;
                break;
            }
            if (len == PLANE_STORED) {
                if (off + count > in_len) { bad = 1; break; }
                memcpy(planes + (size_t)e * count, in + off, count);
                off += count;
            } else {
                if (off + len > in_len) { bad = 1; break; }
                size_t consumed = 0;
                if (qtc_lz_decode(in + off, len, count,
                                  planes + (size_t)e * count, &consumed) != QTC_OK) {
                    bad = 1;
                    break;
                }
                off += len;
            }
        }
        if (bad || off != in_len) {
            free(planes);
            free(out);
            return QTC_ERR_CORRUPT;
        }
        untranspose_elem(planes, count, elem, out);
        free(planes);
        for (size_t i = 1; i < count; i++)
            for (uint8_t b = 0; b < elem; b++)
                out[i * elem + b] ^= out[(i - 1) * elem + b];
        *out_data = out;
        *out_count = count;
        if (out_elem_size)
            *out_elem_size = elem;
        return QTC_OK;
    }

    if (mode == 3) {
        /* Verbatim. */
        if (in_len - ARRAY_HDR < raw) {
            free(out);
            return QTC_ERR_CORRUPT;
        }
        if (raw)
            memcpy(out, in + ARRAY_HDR, raw);
    } else {
        uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
        if (!planes) {
            free(out);
            return QTC_ERR_ALLOC;
        }
        size_t consumed = 0;
        int rc = qtc_lz_decode(in + ARRAY_HDR, in_len - ARRAY_HDR,
                               raw, planes, &consumed);
        if (rc != QTC_OK) {
            free(planes);
            free(out);
            return rc;
        }
        if (mode == 2) {
            for (size_t i = 0; i < count; i++)
                out[i] = (uint8_t)((planes[i] << 4) | (planes[count + i] & 0x0F));
        } else {
            untranspose_elem(planes, count, elem, out);
            if (mode == 1) {
                for (size_t i = 1; i < count; i++)
                    for (uint8_t b = 0; b < elem; b++)
                        out[i * elem + b] ^= out[(i - 1) * elem + b];
            } else if (mode == 5) {
                /* Invert the arithmetic second difference (mod 256). */
                if (count > 1) {
                    for (uint8_t b = 0; b < elem; b++)
                        out[elem + b] = (uint8_t)(out[elem + b] + out[b]);
                }
                for (size_t i = 2; i < count; i++) {
                    for (uint8_t b = 0; b < elem; b++) {
                        uint8_t d = out[i * elem + b];
                        uint8_t y = out[(i - 1) * elem + b];
                        uint8_t z = out[(i - 2) * elem + b];
                        out[i * elem + b] = (uint8_t)(d + 2u * y - z);
                    }
                }
            } else if (mode != 0) {
                free(planes);
                free(out);
                return QTC_ERR_CORRUPT;
            }
        }
        free(planes);
    }

    *out_data = out;
    *out_count = count;
    if (out_elem_size)
        *out_elem_size = elem;
    return QTC_OK;
}
