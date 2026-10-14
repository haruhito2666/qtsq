/*
 * qtc_audio.c — Phase 4: lossless audio lens.
 *
 * Technique (FLAC-style, rewritten standalone):
 *   1. Channel decorrelation — stereo is stored as (L, L−R) when the side
 *      channel carries less energy than the raw right channel. All math is
 *      mod 2^bps, so the transform is exactly invertible.
 *   2. Linear prediction — per channel, per block, we pick the predictor
 *      with the lowest residual energy:
 *        orders 0..4 — the FLAC fixed-coefficient set,
 *        orders 5..8 — custom coefficients solved per block with
 *                      Levinson–Durbin and stored quantized to 12 bits
 *                      (optimization #8, v0.9.0).
 *      The first `order` samples of each block are stored raw as warm-up.
 *   3. The residual byte stream goes to the LZ77 lens.
 *
 * Why this beats a generic compressor: zlib/zstd see interleaved 16-bit
 * samples as opaque bytes and can only exploit byte-level repetition. Audio
 * is smooth in time — a sample is almost its predecessor — so prediction
 * removes several bits per sample before entropy coding even starts.
 *
 * Payload layout:
 *   [u32 nframes][u8 channels][u8 bps][u8 decor][u8 reserved][LZ payload]
 *
 * LZ payload input (the residual stream):
 *   per block of up to AUDIO_BLOCK frames:
 *     [u8 order per channel][i16 LE coeff table: channels*8 entries]
 *     [residual samples, interleaved, bps/8 bytes LE]
 *   The table area is fixed-size (zero-filled for fixed orders) so the
 *   stream length stays exactly computable from the geometry.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AUDIO_BLOCK     4096u   /* frames per prediction block */
#define AUDIO_FIXED_MAX 4       /* orders 0..4: FLAC fixed coefficients */
#define AUDIO_MAX_ORDER 8       /* orders 5..8: per-block LPC (v0.9.0) */
#define AUDIO_MAX_CH    16      /* Phase 5.6: matches QTSQ's channel cap */
#define AUDIO_LPC_SCALE 2048.0  /* 12-bit coefficient quantization */
#define AUDIO_LPC_QMAX  2047

/* FLAC fixed-predictor coefficients, orders 1..4. */
static const int32_t pred_coef[AUDIO_FIXED_MAX + 1][AUDIO_FIXED_MAX] = {
    { 0,  0, 0,  0},
    { 1,  0, 0,  0},
    { 2, -1, 0,  0},
    { 3, -3, 1,  0},
    { 4, -6, 4, -1},
};

/* ── Per-block LPC (optimization #8, v0.9.0) ───────────────────────────
 * Levinson–Durbin solves the Yule–Walker equations for the block's
 * autocorrelation. Coefficients are quantized to 12 bits (scale 2048)
 * and stored in the block header; the decoder uses the SAME quantized
 * values, so prediction is bit-identical on both sides. */
static void lpc_solve(const int32_t *s, uint32_t n, int order,
                      int32_t *qcoef /* order entries */)
{
    double r[AUDIO_MAX_ORDER + 1];
    for (int k = 0; k <= order; k++) {
        double acc = 0.0;
        for (uint32_t i = (uint32_t)k; i < n; i++)
            acc += (double)s[i] * (double)s[i - k];
        r[k] = acc;
    }

    double a[AUDIO_MAX_ORDER + 1] = {0};
    double err = r[0];
    if (err <= 1e-9) {
        /* Silent block: predict zero. */
        for (int k = 0; k < order; k++)
            qcoef[k] = 0;
        return;
    }
    for (int i = 1; i <= order; i++) {
        double lambda = 0.0;
        for (int j = 1; j < i; j++)
            lambda += a[j] * r[i - j];
        lambda = (r[i] - lambda) / err;
        a[i] = lambda;
        for (int j = 1; j < i; j++)
            a[j] -= lambda * a[i - j];
        err *= (1.0 - lambda * lambda);
        if (err <= 1e-9)
            break;
    }

    for (int k = 0; k < order; k++) {
        double v = a[k + 1] * AUDIO_LPC_SCALE;
        long q = lround(v);
        if (q > AUDIO_LPC_QMAX) q = AUDIO_LPC_QMAX;
        if (q < -AUDIO_LPC_QMAX) q = -AUDIO_LPC_QMAX;
        qcoef[k] = (int32_t)q;
    }
}

/* Same, but pulling one channel out of the interleaved block plane. */
static void lpc_solve_block(const int32_t *plane, uint32_t bf,
                            uint32_t channels, uint32_t c, int order,
                            int32_t *qcoef)
{
    int32_t tmp[AUDIO_BLOCK];
    for (uint32_t f = 0; f < bf; f++)
        tmp[f] = plane[(size_t)f * channels + c];
    lpc_solve(tmp, bf, order, qcoef);
}

/* Predict sample f of channel c from the already-decoded plane, using
 * either the fixed FLAC set (order ≤ 4) or the block's quantized LPC
 * coefficients (order 5..8). */
static inline int32_t audio_predict(const int32_t *plane, uint32_t f,
                                    uint32_t c, uint32_t channels, int order,
                                    const int32_t *qcoef)
{
    int32_t pred = 0;
    if (order <= AUDIO_FIXED_MAX) {
        for (int k = 0; k < order; k++)
            pred += pred_coef[order][k] *
                    plane[(size_t)(f - 1 - k) * channels + c];
    } else {
        for (int k = 0; k < order; k++)
            pred += (int32_t)(((int64_t)qcoef[k] *
                               plane[(size_t)(f - 1 - k) * channels + c])
                              >> 11);
    }
    return pred;
}

static inline int32_t read_sample(const uint8_t *p, int bps)
{
    if (bps == 8)
        return p[0];
    if (bps == 24)
        return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                         ((uint32_t)p[2] << 16));
    return (int32_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void write_sample(uint8_t *p, int32_t v, int bps)
{
    if (bps == 8) {
        p[0] = (uint8_t)v;
    } else if (bps == 24) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static inline int64_t iabs(int64_t x)
{
    return x < 0 ? -x : x;
}

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

/* Residual stream size for a given geometry. Fixed-size per block:
 * order bytes + coefficient table (channels*8 i16) + samples. */
static size_t resid_stream_size(uint32_t nframes, uint32_t channels,
                                uint32_t bps_bytes)
{
    size_t nblocks = ((size_t)nframes + AUDIO_BLOCK - 1) / AUDIO_BLOCK;
    size_t per_block_hdr = channels + (size_t)channels * 8 * 2;
    return nblocks * per_block_hdr +
           (size_t)nframes * channels * bps_bytes;
}

/* Forward: pick predictor + emit residuals for one block of decorrelated
 * samples (plane[f*channels + c]). Returns bytes written to dst. */
static size_t encode_block(const int32_t *plane, uint32_t bf,
                           uint32_t channels, int bps, int32_t mask,
                           uint8_t *dst)
{
    int bps_bytes = bps / 8;
    uint8_t orders[AUDIO_MAX_CH];
    int32_t qcoef[AUDIO_MAX_CH][AUDIO_MAX_ORDER];
    memset(qcoef, 0, sizeof(qcoef));

    /* Choose the predictor with the lowest residual energy per channel:
     * orders 0..4 from the fixed FLAC set, orders 5..8 from per-block
     * Levinson–Durbin coefficients quantized to 12 bits. The energy of a
     * custom order is measured with the QUANTIZED coefficients — the same
     * ones the decoder will use — so the choice is honest. */
    for (uint32_t c = 0; c < channels; c++) {
        int best_order = 0;
        int64_t best_e = -1;
        int32_t cand[AUDIO_MAX_ORDER];

        for (int o = 0; o <= AUDIO_MAX_ORDER; o++) {
            if (o > AUDIO_FIXED_MAX)
                lpc_solve_block(plane, bf, channels, c, o, cand);
            int64_t e = 0;
            for (uint32_t f = (uint32_t)o; f < bf; f++) {
                int32_t cur = plane[(size_t)f * channels + c];
                int32_t pred = 0;
                if (o <= AUDIO_FIXED_MAX) {
                    for (int k = 0; k < o; k++)
                        pred += pred_coef[o][k] *
                                plane[(size_t)(f - 1 - k) * channels + c];
                } else {
                    for (int k = 0; k < o; k++)
                        pred += (int32_t)(((int64_t)cand[k] *
                                           plane[(size_t)(f - 1 - k) *
                                                 channels + c]) >> 11);
                }
                int32_t r = (cur - pred) & mask;
                int64_t sr = (r <= mask / 2) ? r : r - ((int64_t)mask + 1);
                e += iabs(sr);
            }
            if (best_e < 0 || e < best_e) {
                best_e = e;
                best_order = o;
                if (o > AUDIO_FIXED_MAX)
                    memcpy(qcoef[c], cand, (size_t)o * sizeof(int32_t));
            }
        }
        orders[c] = (uint8_t)best_order;
    }

    uint8_t *w = dst;
    for (uint32_t c = 0; c < channels; c++)
        *w++ = orders[c];
    /* Coefficient table: channels*8 i16 LE, zero for fixed orders. */
    for (uint32_t c = 0; c < channels; c++)
        for (int k = 0; k < AUDIO_MAX_ORDER; k++) {
            int32_t v = (orders[c] > AUDIO_FIXED_MAX) ? qcoef[c][k] : 0;
            *w++ = (uint8_t)(v & 0xFF);
            *w++ = (uint8_t)((v >> 8) & 0xFF);
        }

    for (uint32_t f = 0; f < bf; f++) {
        for (uint32_t c = 0; c < channels; c++) {
            int32_t cur = plane[(size_t)f * channels + c];
            int32_t r;
            if (f < orders[c]) {
                r = cur;                       /* warm-up: store raw */
            } else {
                int32_t pred = audio_predict(plane, f, c, channels,
                                             orders[c], qcoef[c]);
                r = (cur - pred) & mask;
            }
            write_sample(w, r, bps);
            w += bps_bytes;
        }
    }
    return (size_t)(w - dst);
}

/* Inverse: rebuild one block of decorrelated samples from residuals.
 * Returns 0 on success, QTC_ERR_CORRUPT on a bad order byte or overrun. */
static int decode_block(const uint8_t *src, size_t src_len, size_t *consumed,
                        int32_t *plane, uint32_t bf, uint32_t channels,
                        int bps, int32_t mask)
{
    int bps_bytes = bps / 8;
    size_t need = channels + (size_t)channels * 8 * 2 +
                  (size_t)bf * channels * bps_bytes;
    if (src_len < need)
        return QTC_ERR_CORRUPT;

    uint8_t orders[AUDIO_MAX_CH];
    int32_t qcoef[AUDIO_MAX_CH][AUDIO_MAX_ORDER];
    const uint8_t *r = src;
    for (uint32_t c = 0; c < channels; c++) {
        if (*r > AUDIO_MAX_ORDER)
            return QTC_ERR_CORRUPT;
        orders[c] = *r++;
    }
    for (uint32_t c = 0; c < channels; c++)
        for (int k = 0; k < AUDIO_MAX_ORDER; k++) {
            int32_t v = (int32_t)(int16_t)((uint32_t)r[0] |
                                           ((uint32_t)r[1] << 8));
            if (v < -AUDIO_LPC_QMAX || v > AUDIO_LPC_QMAX)
                return QTC_ERR_CORRUPT;
            qcoef[c][k] = v;
            r += 2;
        }

    for (uint32_t f = 0; f < bf; f++) {
        for (uint32_t c = 0; c < channels; c++) {
            int32_t rv = read_sample(r, bps);
            r += bps_bytes;
            int32_t s;
            if (f < orders[c]) {
                s = rv;
            } else {
                int32_t pred = audio_predict(plane, f, c, channels,
                                             orders[c], qcoef[c]);
                s = (rv + pred) & mask;
            }
            plane[(size_t)f * channels + c] = s;
        }
    }
    *consumed = need;
    return QTC_OK;
}

int qtc_audio_encode(const uint8_t *pcm, uint32_t nframes, uint8_t channels,
                     uint8_t bps, uint8_t **out, size_t *out_len)
{
    if (nframes == 0 || channels == 0 || channels > AUDIO_MAX_CH)
        return QTC_ERR_FORMAT;
    /* 24-bit matches QTSQ's float→24-bit quantization range (Phase 5.6). */
    if (bps != 8 && bps != 16 && bps != 24)
        return QTC_ERR_FORMAT;

    int bps_bytes = bps / 8;
    int32_t mask = (int32_t)((1u << bps) - 1);
    int32_t center = 1 << (bps - 1);

    /* Stereo decorrelation decision: store side = L−R instead of R when
     * the side channel has less raw energy than the right channel. */
    int decor = 0;
    if (channels == 2) {
        int64_t e_r = 0, e_side = 0;
        for (uint32_t f = 0; f < nframes; f++) {
            const uint8_t *p = pcm + (size_t)f * 2 * bps_bytes;
            int32_t l = read_sample(p, bps);
            int32_t r = read_sample(p + bps_bytes, bps);
            e_r += iabs(r - center);
            e_side += iabs((l - r) & mask);
        }
        decor = e_side < e_r;
    }

    size_t rs_len = resid_stream_size(nframes, channels, (uint32_t)bps_bytes);
    uint8_t *resid = (uint8_t *)malloc(rs_len);
    int32_t *plane = (int32_t *)malloc((size_t)AUDIO_BLOCK * channels *
                                       sizeof(int32_t));
    if (!resid || !plane) {
        free(resid); free(plane);
        return QTC_ERR_ALLOC;
    }

    size_t rw = 0;
    uint32_t done = 0;
    while (done < nframes) {
        uint32_t bf = nframes - done;
        if (bf > AUDIO_BLOCK)
            bf = AUDIO_BLOCK;

        /* Load + decorrelate this block. */
        for (uint32_t f = 0; f < bf; f++) {
            const uint8_t *p = pcm + (size_t)(done + f) * channels * bps_bytes;
            for (uint32_t c = 0; c < channels; c++) {
                int32_t s = read_sample(p + (size_t)c * bps_bytes, bps);
                if (decor && c == 1) {
                    int32_t l = plane[(size_t)f * channels];
                    s = (s - l) & mask;
                }
                plane[(size_t)f * channels + c] = s;
            }
        }

        rw += encode_block(plane, bf, channels, bps, mask, resid + rw);
        done += bf;
    }

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(resid, rs_len, &lz, &lz_len);
    free(resid);
    free(plane);
    if (rc != QTC_OK)
        return rc;

    size_t total = 4 + 1 + 1 + 1 + 1 + lz_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, nframes);
    buf[4] = channels;
    buf[5] = bps;
    buf[6] = (uint8_t)decor;
    buf[7] = 0;
    memcpy(buf + 8, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_audio_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_pcm, uint32_t *out_nframes,
                     uint8_t *out_channels, uint8_t *out_bps)
{
    if (payload_len < 8)
        return QTC_ERR_CORRUPT;

    uint32_t nframes = get32(payload);
    uint8_t channels = payload[4];
    uint8_t bps = payload[5];
    uint8_t decor = payload[6];

    if (nframes == 0 || channels == 0 || channels > AUDIO_MAX_CH)
        return QTC_ERR_CORRUPT;
    if (bps != 8 && bps != 16 && bps != 24)
        return QTC_ERR_CORRUPT;
    if (decor > 1)
        return QTC_ERR_CORRUPT;
    if (decor && channels != 2)
        return QTC_ERR_CORRUPT;

    int bps_bytes = bps / 8;
    int32_t mask = (int32_t)((1u << bps) - 1);

    /* Guard against a crafted header forcing a huge allocation. */
    uint64_t total_bytes = (uint64_t)nframes * channels * bps_bytes;
    if (total_bytes > ((uint64_t)1 << 32))
        return QTC_ERR_SIZE;

    size_t rs_len = resid_stream_size(nframes, channels, (uint32_t)bps_bytes);
    uint8_t *resid = (uint8_t *)malloc(rs_len);
    int32_t *plane = (int32_t *)malloc((size_t)AUDIO_BLOCK * channels *
                                       sizeof(int32_t));
    uint8_t *pcm = (uint8_t *)malloc((size_t)total_bytes);
    if (!resid || !plane || !pcm) {
        free(resid); free(plane); free(pcm);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + 8, payload_len - 8, rs_len, resid,
                           &consumed);
    if (rc != QTC_OK) {
        free(resid); free(plane); free(pcm);
        return rc;
    }

    size_t rr = 0;
    uint32_t done = 0;
    while (done < nframes && rc == QTC_OK) {
        uint32_t bf = nframes - done;
        if (bf > AUDIO_BLOCK)
            bf = AUDIO_BLOCK;

        size_t used = 0;
        rc = decode_block(resid + rr, rs_len - rr, &used, plane, bf,
                          channels, bps, mask);
        if (rc != QTC_OK)
            break;
        rr += used;

        /* Inverse decorrelation + emit interleaved bytes. */
        for (uint32_t f = 0; f < bf; f++) {
            uint8_t *p = pcm + (size_t)(done + f) * channels * bps_bytes;
            for (uint32_t c = 0; c < channels; c++) {
                int32_t s = plane[(size_t)f * channels + c];
                if (decor && c == 1) {
                    int32_t l = plane[(size_t)f * channels];
                    s = (s + l) & mask;
                }
                write_sample(p + (size_t)c * bps_bytes, s, bps);
            }
        }
        done += bf;
    }

    free(resid);
    free(plane);
    if (rc != QTC_OK) {
        free(pcm);
        return rc;
    }

    *out_pcm = pcm;
    *out_nframes = nframes;
    *out_channels = channels;
    *out_bps = bps;
    return QTC_OK;
}
