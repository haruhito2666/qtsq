/*
 * qtc_audio_mdct.c — Phase 5: lossy audio lens (MDCT transform coder).
 *
 * The "Gravitational Lensing" layer from CONCEPT.md, done properly:
 *   1. Split each channel into 50%-overlapping frames of 2N samples.
 *   2. Forward MDCT per frame → N frequency coefficients. The transform
 *      concentrates energy: most of a signal's information lives in a few
 *      low-frequency coefficients.
 *   3. Quantize each coefficient with a step derived from the quality knob,
 *      weighted so high frequencies (which the ear tolerates less
 *      precisely) are quantized more coarsely.
 *   4. Entropy-code the quantized indices with the LZ77 lens (runs of zeros
 *      at high frequency compress extremely well).
 *   5. Decode: dequantize → inverse MDCT → overlap-add → PCM.
 *
 * This is LOSSY: the output is close to, not equal to, the input. Quality is
 * measured by SNR in the bench harness, not by memcmp.
 *
 * Payload layout:
 *   [u32 nframes][u8 channels][u8 bps][u16 N][u8 quality*255][u8 reserved]
 *   [LZ payload of int32 quantized coefficients, frame-major, channel-major]
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MDCT_N 256   /* coefficients per frame; frame = 2N samples */

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

/* LEB128 varint (7 bits/byte, high bit = continuation). */
static size_t put_varint(uint8_t *p, uint32_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

static int get_varint(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return QTC_OK;
        }
        shift += 7;
        if (shift > 28)
            return QTC_ERR_CORRUPT;   /* longer than any encoded u32 */
    }
    return QTC_ERR_CORRUPT;
}

static inline int32_t read_sample(const uint8_t *p, int bps)
{
    if (bps == 8)
        return (int32_t)p[0] - 128;   /* center 8-bit on 0 */
    /* Sign-extend: the MDCT is a real-valued transform and needs true
     * signed samples (the lossless lens can treat them mod 2^16, we can't). */
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void write_sample(uint8_t *p, int32_t v, int bps)
{
    if (bps == 8) {
        v += 128;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        p[0] = (uint8_t)v;
    } else {
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

/* Quantization step for a coefficient bin k given the base step. High
 * frequencies get a coarser step (perceptual weighting). */
static double bin_step(double base, int k, int N)
{
    double f = (double)k / N;
    return base * (1.0 + 3.0 * f * f);
}

/* Map quality [0,1] → base quantization step. quality 1 → step ~1
 * (near-transparent); quality 0 → step ~4096 (very aggressive). */
static double quality_to_step(float quality)
{
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    return pow(2.0, (1.0 - (double)quality) * 12.0);
}

int qtc_audio_mdct_encode(const uint8_t *pcm, uint32_t nframes,
                          uint8_t channels, uint8_t bps, float quality,
                          uint8_t **out, size_t *out_len)
{
    int N = MDCT_N;
    if (nframes == 0 || channels == 0 || channels > 8)
        return QTC_ERR_FORMAT;
    if (bps != 8 && bps != 16)
        return QTC_ERR_FORMAT;

    int bps_bytes = bps / 8;
    /* Clamp BEFORE quantizing to the stored byte: casting a negative float
     * straight to uint8_t is undefined behavior (graceful quality knob). */
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    uint8_t qbyte = (uint8_t)(quality * 255.0f + 0.5f);
    double base = quality_to_step((float)qbyte / 255.0f);

    /* Number of MDCT frames covering [0, total) with a 2N window, hop N.
     * Ceiling division: when nframes % N != 0 the last real samples still
     * need the final frame for the second half of their overlap-add.
     * Compute in 64-bit so a huge nframes cannot wrap the sum. */
    size_t mf = ((size_t)nframes + (size_t)N - 1) / (size_t)N + 1;

    /* Buffer sized for the last frame: it starts at (mf-1)*N and spans 2N. */
    size_t total = (mf + 1) * N;
    double *sig = (double *)calloc(total, sizeof(double));
    double *frame = (double *)malloc((size_t)2 * N * sizeof(double));
    double *coef = (double *)malloc((size_t)N * sizeof(double));
    if (!sig || !frame || !coef) {
        free(sig); free(frame); free(coef);
        return QTC_ERR_ALLOC;
    }

    size_t coeff_count = mf * N * channels;
    int32_t *qbuf = (int32_t *)malloc(coeff_count * sizeof(int32_t));
    if (!qbuf) {
        free(sig); free(frame); free(coef);
        return QTC_ERR_ALLOC;
    }

    size_t qw = 0;
    qtc_mdct_kernel_t kern;
    int rc = qtc_mdct_kernel_init(&kern, N);
    if (rc != QTC_OK)
        return rc;
    for (uint8_t c = 0; c < channels; c++) {
        /* Load this channel into sig[N .. N+nframes). */
        memset(sig, 0, total * sizeof(double));
        for (uint32_t f = 0; f < nframes; f++) {
            const uint8_t *p = pcm + ((size_t)f * channels + c) * bps_bytes;
            sig[N + f] = (double)read_sample(p, bps);
        }

        for (size_t m = 0; m < mf; m++) {
            for (int n = 0; n < 2 * N; n++)
                frame[n] = sig[m * N + n];
            qtc_mdct_forward(&kern, frame, coef);
            for (int k = 0; k < N; k++) {
                double s = bin_step(base, k, N);
                qbuf[qw++] = (int32_t)llround(coef[k] / s);
            }
        }
    }

    qtc_mdct_kernel_free(&kern);
    free(sig); free(frame); free(coef);

    /* Serialize quantized indices as a zigzag-varint + zero-run byte
     * stream (optimization #2, v0.8.0): quantized coefficients are small
     * integers with long zero runs at high frequency — raw int32 LE wasted
     * 4 B per value and hid the runs from LZ. Nonzero v → varint(zigzag(v))
     * (first byte ≥ 1); a zero RUN → marker byte 0x00 + varint(length). */
    size_t raw_cap = coeff_count * 5 + 16;
    uint8_t *raw = (uint8_t *)malloc(raw_cap);
    if (!raw) {
        free(qbuf);
        return QTC_ERR_ALLOC;
    }
    size_t o = 0;
    for (size_t i = 0; i < coeff_count; ) {
        if (qbuf[i] == 0) {
            size_t run = 1;
            while (i + run < coeff_count && qbuf[i + run] == 0)
                run++;
            raw[o++] = 0;
            o += put_varint(raw + o, (uint32_t)run);
            i += run;
        } else {
            uint32_t z = ((uint32_t)qbuf[i] << 1) ^
                         (uint32_t)(qbuf[i] >> 31);
            o += put_varint(raw + o, z);
            i++;
        }
    }
    size_t raw_len = o;
    free(qbuf);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    rc = qtc_lz_encode(raw, raw_len, &lz, &lz_len);
    free(raw);
    if (rc != QTC_OK)
        return rc;

    size_t hdr = 4 + 1 + 1 + 2 + 1 + 1 + 4;
    uint8_t *buf = (uint8_t *)malloc(hdr + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, nframes);
    buf[4] = channels;
    buf[5] = bps;
    buf[6] = (uint8_t)(N & 0xFF);
    buf[7] = (uint8_t)(N >> 8);
    buf[8] = qbyte;   /* the exact byte the step was derived from */
    buf[9] = 0;
    put32(buf + 10, (uint32_t)raw_len);
    memcpy(buf + hdr, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = hdr + lz_len;
    return QTC_OK;
}

int qtc_audio_mdct_decode(const uint8_t *payload, size_t payload_len,
                          uint8_t **out_pcm, uint32_t *out_nframes,
                          uint8_t *out_channels, uint8_t *out_bps)
{
    size_t hdr = 4 + 1 + 1 + 2 + 1 + 1 + 4;   /* + stream_len (v0.8.0) */
    if (payload_len < hdr)
        return QTC_ERR_CORRUPT;

    uint32_t nframes = get32(payload);
    uint8_t channels = payload[4];
    uint8_t bps = payload[5];
    int N = (int)payload[6] | ((int)payload[7] << 8);
    uint8_t qbyte = payload[8];

    if (nframes == 0 || channels == 0 || channels > 8)
        return QTC_ERR_CORRUPT;
    if (bps != 8 && bps != 16)
        return QTC_ERR_CORRUPT;
    if (N < 16 || N > 4096 || (N & (N - 1)) != 0)
        return QTC_ERR_CORRUPT;

    int bps_bytes = bps / 8;
    double base = quality_to_step((float)qbyte / 255.0f);

    /* Mirror the encoder's frame count in 64-bit: a crafted nframes near
     * 2^32 would wrap a 32-bit ceiling division into a tiny frame count,
     * undersize the overlap-add buffer, and send the write loop out of
     * bounds. */
    size_t mf = ((size_t)nframes + (size_t)N - 1) / (size_t)N + 1;
    size_t total = (mf + 1) * N;
    size_t coeff_count = mf * N * channels;

    /* Guard against a crafted header forcing a huge allocation. */
    if (coeff_count > ((size_t)1 << 30))
        return QTC_ERR_SIZE;
    /* And a huge declared output: >2 GB of PCM is never a legitimate
     * single call (that would be hours of audio). */
    if ((size_t)nframes * channels * (size_t)bps_bytes > ((size_t)1 << 31))
        return QTC_ERR_SIZE;

    /* v0.8.0: stream_len of the varint coefficient stream (was the raw
     * int32 array length). Bounded by 5 bytes per coefficient + markers. */
    uint32_t stream_len = get32(payload + 10);
    if (stream_len == 0 ||
        (uint64_t)stream_len > (uint64_t)coeff_count * 5 + 64)
        return QTC_ERR_CORRUPT;

    uint8_t *raw = (uint8_t *)malloc(stream_len);
    int32_t *qbuf = (int32_t *)malloc(coeff_count * sizeof(int32_t));
    if (!raw || !qbuf) {
        free(raw); free(qbuf);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + hdr, payload_len - hdr, stream_len,
                           raw, &consumed);
    if (rc != QTC_OK) {
        free(raw); free(qbuf);
        return rc;
    }

    /* Parse the varint stream back into quantized indices. Every read is
     * bounds-checked; the stream must fill exactly coeff_count values. */
    {
        const uint8_t *p = raw;
        const uint8_t *end = raw + stream_len;
        size_t i = 0;
        while (i < coeff_count) {
            if (p >= end) { rc = QTC_ERR_CORRUPT; goto parse_fail; }
            if (*p == 0) {
                p++;
                uint32_t run;
                if (get_varint(&p, end, &run) != QTC_OK || run == 0 ||
                    i + run > coeff_count) {
                    rc = QTC_ERR_CORRUPT;
                    goto parse_fail;
                }
                memset(qbuf + i, 0, run * sizeof(int32_t));
                i += run;
            } else {
                uint32_t z;
                if (get_varint(&p, end, &z) != QTC_OK) {
                    rc = QTC_ERR_CORRUPT;
                    goto parse_fail;
                }
                qbuf[i++] = (int32_t)((z >> 1) ^ (uint32_t)(-(int32_t)(z & 1)));
            }
        }
        if (p != end) { rc = QTC_ERR_CORRUPT; goto parse_fail; }
    }
    free(raw);
    raw = NULL;
    goto parse_done;

parse_fail:
    free(raw);
    free(qbuf);
    return rc;

parse_done:
    ;

    double *sig = (double *)calloc(total, sizeof(double));
    double *coef = (double *)malloc((size_t)N * sizeof(double));
    double *rec = (double *)malloc((size_t)2 * N * sizeof(double));
    uint8_t *pcm = (uint8_t *)malloc((size_t)nframes * channels * bps_bytes);
    if (!sig || !coef || !rec || !pcm) {
        free(raw); free(sig); free(coef); free(rec); free(pcm); free(qbuf);
        return QTC_ERR_ALLOC;
    }

    size_t rr = 0;
    qtc_mdct_kernel_t kern;
    rc = qtc_mdct_kernel_init(&kern, N);
    if (rc != QTC_OK) {
        free(raw); free(sig); free(coef); free(rec); free(pcm); free(qbuf);
        return rc;
    }
    for (uint8_t c = 0; c < channels; c++) {
        memset(sig, 0, total * sizeof(double));
        for (size_t m = 0; m < mf; m++) {
            for (int k = 0; k < N; k++) {
                int32_t q = qbuf[rr++];
                coef[k] = (double)q * bin_step(base, k, N);
            }
            qtc_mdct_inverse(&kern, coef, rec);
            for (int n = 0; n < 2 * N; n++)
                sig[m * N + n] += rec[n];
        }
        /* Write this channel back, trimming the N-sample pads. */
        for (uint32_t f = 0; f < nframes; f++) {
            uint8_t *p = pcm + ((size_t)f * channels + c) * bps_bytes;
            write_sample(p, (int32_t)llround(sig[N + f]), bps);
        }
    }

    qtc_mdct_kernel_free(&kern);
    free(raw); free(sig); free(coef); free(rec); free(qbuf);

    *out_pcm = pcm;
    *out_nframes = nframes;
    *out_channels = channels;
    *out_bps = bps;
    return QTC_OK;
}
