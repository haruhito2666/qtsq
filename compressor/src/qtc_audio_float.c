/*
 * qtc_audio_float.c — v0.14.0: float32 audio lens (raw capture data).
 *
 * Microphone and DSP pipelines produce float32 samples. Two storage modes:
 *
 *   mode 0 (LOSSLESS) — bit-exact for every IEEE-754 bit pattern, including
 *     denormals, ±inf, NaN and −0. The samples are treated as opaque u32
 *     bit patterns (ZERO float arithmetic, so round-trip is exact by
 *     construction). Samples are deinterleaved per channel, then the encoder
 *     contests two reversible transforms and keeps the smaller:
 *
 *       transform 0 (planes)  — byte-transpose the 4 bytes of every sample
 *                               into 4 contiguous planes. The exponent/sign
 *                               byte becomes a smooth, repetitive plane; the
 *                               mantissa low byte carries the noise.
 *       transform 1 (xdelta)  — XOR each sample's bits with the previous
 *                               sample OF THE SAME CHANNEL first (slowly
 *                               varying audio cancels to tiny integers), then
 *                               byte-transpose.
 *
 *     Both feed the LZ77 lens. This mirrors the TENSOR lens's proven float
 *     technique, but adds per-channel temporal structure and the
 *     nframes/channels metadata the generic tensor lens does not carry.
 *
 *   mode 1 (LOSSY) — the float stream is scaled to a full-range int16
 *     intermediate and handed to the perceptual MDCT2 coder (mid/side +
 *     masking + rate control). The peak scale is stored in the payload so
 *     decode returns floats at the original level. Quality is therefore
 *     16-bit-equivalent (~96 dB dynamic-range ceiling — inherent to the
 *     int16 step) at MP3-class bitrates. opts->quality / target_kbps pass
 *     through to MDCT2.
 *
 * Payload layout (common 8-byte header):
 *   [u32 nframes][u8 channels][u8 mode][u8 param][u8 reserved]
 *   mode 0: param = transform (0/1), then LZ of nframes*channels*4 bytes
 *   mode 1: param = 0, then [float32 peak][MDCT2 payload]
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AFLOAT_HDR 8u

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

static void putf32(uint8_t *p, float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    put32(p, x);
}

static float getf32(const uint8_t *p)
{
    uint32_t x = get32(p);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static uint32_t fbits(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    return x;
}

static float bitsf(uint32_t x)
{
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* Deinterleave nframes*channels interleaved floats into a channel-major
 * array of u32 bit patterns: out[c*nframes + f]. */
static void deinterleave(const float *pcm, uint32_t nframes, uint8_t channels,
                         uint32_t *out)
{
    for (uint32_t f = 0; f < nframes; f++)
        for (uint8_t c = 0; c < channels; c++)
            out[(size_t)c * nframes + f] = fbits(pcm[(size_t)f * channels + c]);
}

/* Inverse of deinterleave. */
static void interleave(const uint32_t *in, uint32_t nframes, uint8_t channels,
                       float *pcm)
{
    for (uint32_t f = 0; f < nframes; f++)
        for (uint8_t c = 0; c < channels; c++)
            pcm[(size_t)f * channels + c] = bitsf(in[(size_t)c * nframes + f]);
}

/* Byte-transpose `count` u32s into 4 planes. */
static void transpose4(const uint32_t *u, size_t count, uint8_t *out)
{
    const uint8_t *src = (const uint8_t *)u;
    for (size_t i = 0; i < count; i++) {
        out[i]             = src[i * 4];
        out[count + i]     = src[i * 4 + 1];
        out[2 * count + i] = src[i * 4 + 2];
        out[3 * count + i] = src[i * 4 + 3];
    }
}

static void untranspose4(const uint8_t *in, size_t count, uint32_t *u)
{
    uint8_t *dst = (uint8_t *)u;
    for (size_t i = 0; i < count; i++) {
        dst[i * 4]     = in[i];
        dst[i * 4 + 1] = in[count + i];
        dst[i * 4 + 2] = in[2 * count + i];
        dst[i * 4 + 3] = in[3 * count + i];
    }
}

/* Per-channel XOR-delta along time (in place, channel-major). */
static void xdelta_channels(uint32_t *u, uint32_t nframes, uint8_t channels,
                            int inverse)
{
    for (uint8_t c = 0; c < channels; c++) {
        uint32_t *p = u + (size_t)c * nframes;
        if (!inverse) {
            for (uint32_t f = nframes; f-- > 1;)
                p[f] ^= p[f - 1];
        } else {
            for (uint32_t f = 1; f < nframes; f++)
                p[f] ^= p[f - 1];
        }
    }
}

/* Encode one lossless transform; returns a malloc'd full payload. */
static int afloat_try(const uint32_t *work, uint32_t nframes,
                      uint8_t channels, uint8_t transform,
                      uint8_t **out, size_t *out_len)
{
    size_t count = (size_t)nframes * channels;
    size_t raw = count * 4;
    uint32_t *tmp = (uint32_t *)malloc(raw ? raw : 1);
    uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
    if (!tmp || !planes) {
        free(tmp); free(planes);
        return QTC_ERR_ALLOC;
    }

    memcpy(tmp, work, raw);
    if (transform == 1)
        xdelta_channels(tmp, nframes, channels, 0);
    transpose4(tmp, count, planes);
    free(tmp);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(planes, raw, &lz, &lz_len);
    free(planes);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(AFLOAT_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, nframes);
    buf[4] = channels;
    buf[5] = 0;              /* mode 0 = lossless */
    buf[6] = transform;
    buf[7] = 0;
    memcpy(buf + AFLOAT_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = AFLOAT_HDR + lz_len;
    return QTC_OK;
}

static int afloat_encode_lossless(const float *pcm, uint32_t nframes,
                                  uint8_t channels,
                                  uint8_t **out, size_t *out_len)
{
    size_t count = (size_t)nframes * channels;
    uint32_t *work = (uint32_t *)malloc(count * 4);
    if (!work)
        return QTC_ERR_ALLOC;
    deinterleave(pcm, nframes, channels, work);

    uint8_t *best = NULL;
    size_t best_len = 0;
    for (uint8_t t = 0; t <= 1; t++) {
        uint8_t *p = NULL;
        size_t pl = 0;
        int rc = afloat_try(work, nframes, channels, t, &p, &pl);
        if (rc != QTC_OK) {
            free(work);
            free(best);
            return rc;
        }
        if (!best || pl < best_len) {
            free(best);
            best = p;
            best_len = pl;
        } else {
            free(p);
        }
    }
    free(work);

    *out = best;
    *out_len = best_len;
    return QTC_OK;
}

/* Lossy: scale to full-range int16, run the MDCT2 perceptual coder, store
 * the peak so decode restores float levels. */
static int afloat_encode_lossy(const float *pcm, uint32_t nframes,
                               uint8_t channels, float quality,
                               int target_kbps,
                               uint8_t **out, size_t *out_len)
{
    size_t total = (size_t)nframes * channels;

    float peak = 0.0f;
    for (size_t i = 0; i < total; i++) {
        float a = fabsf(pcm[i]);
        if (a > peak)
            peak = a;
    }

    int16_t *i16 = (int16_t *)malloc(total * sizeof(int16_t));
    if (!i16)
        return QTC_ERR_ALLOC;

    if (peak > 0.0f) {
        double scale = 32767.0 / (double)peak;
        for (size_t i = 0; i < total; i++) {
            double v = (double)pcm[i] * scale;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            i16[i] = (int16_t)lrint(v);
        }
    } else {
        memset(i16, 0, total * sizeof(int16_t));
    }

    uint8_t *mdct = NULL;
    size_t mdct_len = 0;
    int rc = qtc_audio_mdct2_encode((const uint8_t *)i16, nframes, channels,
                                    16, quality, target_kbps,
                                    &mdct, &mdct_len);
    free(i16);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(AFLOAT_HDR + 4 + mdct_len);
    if (!buf) {
        free(mdct);
        return QTC_ERR_ALLOC;
    }
    put32(buf, nframes);
    buf[4] = channels;
    buf[5] = 1;              /* mode 1 = lossy */
    buf[6] = 0;
    buf[7] = 0;
    putf32(buf + AFLOAT_HDR, peak);
    memcpy(buf + AFLOAT_HDR + 4, mdct, mdct_len);
    free(mdct);

    *out = buf;
    *out_len = AFLOAT_HDR + 4 + mdct_len;
    return QTC_OK;
}

int qtc_audio_float_encode(const float *pcm, uint32_t nframes,
                           uint8_t channels, float quality, int target_kbps,
                           int lossy, uint8_t **out, size_t *out_len)
{
    if (!pcm || nframes == 0 || channels == 0 ||
        channels > QTC_AUDIO_FLOAT_MAX_CH)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if ((uint64_t)nframes * channels > ((uint64_t)1 << 30))
        return QTC_ERR_SIZE;

    if (lossy)
        return afloat_encode_lossy(pcm, nframes, channels, quality,
                                   target_kbps, out, out_len);
    return afloat_encode_lossless(pcm, nframes, channels, out, out_len);
}

static int afloat_decode_lossless(const uint8_t *payload, size_t payload_len,
                                  uint32_t nframes, uint8_t channels,
                                  uint8_t transform, float **out_pcm)
{
    size_t count = (size_t)nframes * channels;
    size_t raw = count * 4;

    uint8_t *planes = (uint8_t *)malloc(raw ? raw : 1);
    uint32_t *work = (uint32_t *)malloc(raw ? raw : 1);
    float *pcm = (float *)malloc(raw ? raw : 1);
    if (!planes || !work || !pcm) {
        free(planes); free(work); free(pcm);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + AFLOAT_HDR, payload_len - AFLOAT_HDR,
                           raw, planes, &consumed);
    if (rc != QTC_OK) {
        free(planes); free(work); free(pcm);
        return rc;
    }

    untranspose4(planes, count, work);
    free(planes);
    if (transform == 1)
        xdelta_channels(work, nframes, channels, 1);
    interleave(work, nframes, channels, pcm);
    free(work);

    *out_pcm = pcm;
    return QTC_OK;
}

static int afloat_decode_lossy(const uint8_t *payload, size_t payload_len,
                               uint32_t nframes, uint8_t channels,
                               float **out_pcm)
{
    if (payload_len < AFLOAT_HDR + 4)
        return QTC_ERR_CORRUPT;
    float peak = getf32(payload + AFLOAT_HDR);

    uint8_t *i16pcm = NULL;
    uint32_t nf = 0;
    uint8_t ch = 0, bps = 0;
    int rc = qtc_audio_mdct2_decode(payload + AFLOAT_HDR + 4,
                                    payload_len - AFLOAT_HDR - 4,
                                    &i16pcm, &nf, &ch, &bps);
    if (rc != QTC_OK)
        return rc;
    if (nf != nframes || ch != channels || bps != 16) {
        free(i16pcm);
        return QTC_ERR_CORRUPT;
    }

    size_t total = (size_t)nframes * channels;
    float *pcm = (float *)malloc(total * sizeof(float));
    if (!pcm) {
        free(i16pcm);
        return QTC_ERR_ALLOC;
    }

    const int16_t *s = (const int16_t *)i16pcm;
    double inv = (double)peak / 32767.0;
    for (size_t i = 0; i < total; i++)
        pcm[i] = (float)((double)s[i] * inv);
    free(i16pcm);

    *out_pcm = pcm;
    return QTC_OK;
}

int qtc_audio_float_decode(const uint8_t *payload, size_t payload_len,
                           float **out_pcm, uint32_t *out_nframes,
                           uint8_t *out_channels)
{
    if (!payload || !out_pcm || !out_nframes || !out_channels)
        return QTC_ERR_ALLOC;
    *out_pcm = NULL;
    *out_nframes = 0;
    *out_channels = 0;

    if (payload_len < AFLOAT_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t nframes = get32(payload);
    uint8_t channels = payload[4];
    uint8_t mode = payload[5];
    uint8_t transform = payload[6];

    if (nframes == 0 || channels == 0 || channels > QTC_AUDIO_FLOAT_MAX_CH)
        return QTC_ERR_CORRUPT;
    if (mode > 1)
        return QTC_ERR_FORMAT;
    if (mode == 0 && transform > 1)
        return QTC_ERR_CORRUPT;
    if ((uint64_t)nframes * channels > ((uint64_t)1 << 30))
        return QTC_ERR_SIZE;

    float *pcm = NULL;
    int rc;
    if (mode == 0)
        rc = afloat_decode_lossless(payload, payload_len, nframes, channels,
                                    transform, &pcm);
    else
        rc = afloat_decode_lossy(payload, payload_len, nframes, channels,
                                 &pcm);
    if (rc != QTC_OK)
        return rc;

    *out_pcm = pcm;
    *out_nframes = nframes;
    *out_channels = channels;
    return QTC_OK;
}
