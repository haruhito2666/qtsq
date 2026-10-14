/*
 * qtc_image.c — Phase 4: lossless image lens.
 *
 * Technique: 2D spatial prediction + residual coding. For each pixel we
 * predict its value from already-coded neighbors using the JPEG-LS MED
 * (Median Edge Detection) predictor, then store only the prediction error
 * (mod 256, so it stays byte-oriented and exactly invertible). The residual
 * stream — which has far lower entropy than the raw pixels — is then fed
 * to the LZ77 lens.
 *
 * Why this beats a generic compressor: zstd/zlib see an opaque byte stream
 * and can only exploit byte repetition. They cannot predict a pixel from its
 * 2D neighborhood. MED captures edges and smooth gradients, cutting several
 * bits per pixel before entropy coding even starts.
 *
 * Payload layout:
 *   [u32 width][u32 height][u8 channels][u8 reserved][LZ payload]
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

/* MED predictor from left (a), top (b), top-left (c). */
static inline uint8_t med_predict(uint8_t a, uint8_t b, uint8_t c)
{
    int ia = a, ib = b, ic = c;
    int pred;
    if (ic >= (ia < ib ? ia : ib))
        pred = ia + ib - ic;
    else if (ic <= (ia > ib ? ia : ib))
        pred = ia < ib ? ia : ib;
    else
        pred = ia > ib ? ia : ib;
    return (uint8_t)pred;
}

/* Forward: pixels → residual (mod 256). Shared with the video lens
 * (Phase 5.6), which applies it to inter-frame residual frames. */
void qtc_image_predict_forward(const uint8_t *px, uint32_t w, uint32_t h,
                               uint8_t ch, uint8_t *resid)
{
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                size_t i = ((size_t)(y * w + x)) * ch + c;
                uint8_t cur = px[i];
                uint8_t pred;
                if (y == 0 && x == 0) {
                    pred = 0;
                } else if (y == 0) {
                    pred = px[i - ch];                 /* left */
                } else if (x == 0) {
                    pred = px[i - (size_t)w * ch];     /* top */
                } else {
                    uint8_t a = px[i - ch];            /* left */
                    uint8_t b = px[i - (size_t)w * ch];/* top */
                    uint8_t cc = px[i - (size_t)w * ch - ch]; /* top-left */
                    pred = med_predict(a, b, cc);
                }
                resid[i] = (uint8_t)(cur - pred);      /* mod 256 */
            }
        }
    }
}

/* Inverse: residual → pixels (mod 256). Must mirror forward exactly. */
void qtc_image_predict_inverse(const uint8_t *resid, uint32_t w, uint32_t h,
                               uint8_t ch, uint8_t *px)
{
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                size_t i = ((size_t)(y * w + x)) * ch + c;
                uint8_t pred;
                if (y == 0 && x == 0) {
                    pred = 0;
                } else if (y == 0) {
                    pred = px[i - ch];
                } else if (x == 0) {
                    pred = px[i - (size_t)w * ch];
                } else {
                    uint8_t a = px[i - ch];
                    uint8_t b = px[i - (size_t)w * ch];
                    uint8_t cc = px[i - (size_t)w * ch - ch];
                    pred = med_predict(a, b, cc);
                }
                px[i] = (uint8_t)(resid[i] + pred);    /* mod 256 */
            }
        }
    }
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

int qtc_image_encode(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch,
                     uint8_t **out, size_t *out_len)
{
    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    size_t npix = (size_t)w * h * ch;

    uint8_t *resid = (uint8_t *)malloc(npix);
    if (!resid)
        return QTC_ERR_ALLOC;

    qtc_image_predict_forward(pixels, w, h, ch, resid);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(resid, npix, &lz, &lz_len);
    free(resid);
    if (rc != QTC_OK)
        return rc;

    size_t total = 4 + 4 + 1 + 1 + lz_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = 0;
    memcpy(buf + 10, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_image_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                     uint8_t *out_ch)
{
    if (payload_len < 10)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];

    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_CORRUPT;
    /* Depth byte 2 marks a 16-bit stream: qtc_image16_decode's job. */
    if (payload[9] == 2)
        return QTC_ERR_FORMAT;

    /* Guard against a crafted header forcing a huge allocation. */
    uint64_t npix64 = (uint64_t)w * h * ch;
    if (npix64 > ((uint64_t)1 << 32))
        return QTC_ERR_SIZE;
    size_t npix = (size_t)npix64;

    uint8_t *resid = (uint8_t *)malloc(npix ? npix : 1);
    uint8_t *px = (uint8_t *)malloc(npix ? npix : 1);
    if (!resid || !px) {
        free(resid); free(px);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + 10, payload_len - 10, npix, resid, &consumed);
    if (rc != QTC_OK) {
        free(resid); free(px);
        return rc;
    }

    qtc_image_predict_inverse(resid, w, h, ch, px);
    free(resid);

    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}

/* ── 16-bit/channel lossless + near-lossless (v0.14.0, raw camera data) ─
 *
 * Same JPEG-LS MED predictor widened to 16 bits (residuals mod 65536), so
 * camera-raw 16-bit samples get the same 2D prediction the 8-bit path has.
 * Two byte layouts of the residual stream are contested at encode time:
 * interleaved little-endian pairs vs split high/low byte planes. Camera
 * sensor noise lives mostly in the low byte, so plane splitting lets LZ
 * model the two bytes separately and usually wins.
 *
 * Near-lossless (quant > 1): residuals are rounded to a multiple of quant
 * (a power of two) BEFORE reconstruction, and prediction runs on the
 * reconstructed values at both ends (JPEG-LS style feedback), so the error
 * is bounded by quant/2 per sample and never drifts. quant == 1 is exactly
 * lossless.
 *
 * Payload: [u32 w][u32 h][u8 ch][u8 depth=2][u8 layout][u8 quant][LZ]
 *   layout 0 = interleaved LE residual bytes, 1 = high plane then low plane
 */

#define IMAGE16_HDR 12

static inline uint16_t med_predict16(uint16_t a, uint16_t b, uint16_t c)
{
    int ia = a, ib = b, ic = c;
    int pred;
    if (ic >= (ia < ib ? ia : ib))
        pred = ia + ib - ic;
    else if (ic <= (ia > ib ? ia : ib))
        pred = ia < ib ? ia : ib;
    else
        pred = ia > ib ? ia : ib;
    return (uint16_t)pred;
}

/* Forward with reconstruction feedback: prediction always reads already-
 * reconstructed neighbors, which equals the input when quant == 1.
 *
 * Near-lossless quantization is done on the SIGNED error and the
 * reconstruction is clamped to [0,65535] (JPEG-LS style). Quantizing in
 * modular space instead would round some residuals across the 65535→0
 * boundary, flipping a near-black pixel to saturated white — visually
 * catastrophic for camera data even though it is "circularly close".
 * We store stored = recon − pred (mod 65536); the decoder's modular add
 * reproduces the clamped recon exactly, so the round-trip stays bit-exact
 * and the per-sample error stays within quant/2. */
static int image16_predict_forward(const uint16_t *px, uint32_t w, uint32_t h,
                                   uint8_t ch, uint8_t quant, uint16_t *resid)
{
    /* Two rolling rows of reconstructed samples (prev + current). */
    uint16_t *prev = (uint16_t *)calloc((size_t)w * ch, sizeof(uint16_t));
    uint16_t *cur = (uint16_t *)calloc((size_t)w * ch, sizeof(uint16_t));
    if (!prev || !cur) {
        free(prev); free(cur);
        return QTC_ERR_ALLOC;
    }
    int32_t half = (int32_t)quant / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                size_t i = ((size_t)(y * w + x)) * ch + c;
                uint16_t pred;
                if (y == 0 && x == 0) {
                    pred = 0;
                } else if (y == 0) {
                    pred = cur[(size_t)(x - 1) * ch + c];
                } else if (x == 0) {
                    pred = prev[c];
                } else {
                    uint16_t a = cur[(size_t)(x - 1) * ch + c];
                    uint16_t b = prev[(size_t)x * ch + c];
                    uint16_t cc = prev[(size_t)(x - 1) * ch + c];
                    pred = med_predict16(a, b, cc);
                }
                int32_t e = (int32_t)px[i] - (int32_t)pred; /* signed error */
                int32_t eq = e;
                if (quant > 1)
                    eq = ((e + (e >= 0 ? half : -half)) / (int32_t)quant)
                         * (int32_t)quant;
                int32_t recon = (int32_t)pred + eq;
                if (recon < 0) recon = 0;
                if (recon > 65535) recon = 65535;
                resid[i] = (uint16_t)((recon - (int32_t)pred) & 0xFFFF);
                cur[(size_t)x * ch + c] = (uint16_t)recon;
            }
        }
        memcpy(prev, cur, (size_t)w * ch * sizeof(uint16_t));
    }
    free(prev);
    free(cur);
    return QTC_OK;
}

/* Inverse: residual → pixels (mod 65536). Mirrors forward exactly. */
static void image16_predict_inverse(const uint16_t *resid, uint32_t w,
                                    uint32_t h, uint8_t ch, uint16_t *px)
{
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                size_t i = ((size_t)(y * w + x)) * ch + c;
                uint16_t pred;
                if (y == 0 && x == 0) {
                    pred = 0;
                } else if (y == 0) {
                    pred = px[i - ch];
                } else if (x == 0) {
                    pred = px[i - (size_t)w * ch];
                } else {
                    uint16_t a = px[i - ch];
                    uint16_t b = px[i - (size_t)w * ch];
                    uint16_t cc = px[i - (size_t)w * ch - ch];
                    pred = med_predict16(a, b, cc);
                }
                px[i] = (uint16_t)(resid[i] + pred);
            }
        }
    }
}

int qtc_image16_encode(const uint16_t *pixels, uint32_t w, uint32_t h,
                       uint8_t ch, uint8_t quant,
                       uint8_t **out, size_t *out_len)
{
    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    if (quant != 1 && quant != 2 && quant != 4 && quant != 8 && quant != 16)
        return QTC_ERR_FORMAT;
    size_t npix = (size_t)w * h * ch;

    uint16_t *resid = (uint16_t *)malloc(npix * sizeof(uint16_t));
    uint8_t *inter = (uint8_t *)malloc(npix * 2);
    uint8_t *planes = (uint8_t *)malloc(npix * 2);
    if (!resid || !inter || !planes) {
        free(resid); free(inter); free(planes);
        return QTC_ERR_ALLOC;
    }

    int rc = image16_predict_forward(pixels, w, h, ch, quant, resid);
    if (rc != QTC_OK) {
        free(resid); free(inter); free(planes);
        return rc;
    }

    /* Layout 0: interleaved little-endian. Layout 1: high plane, low plane. */
    for (size_t i = 0; i < npix; i++) {
        inter[i * 2] = (uint8_t)resid[i];
        inter[i * 2 + 1] = (uint8_t)(resid[i] >> 8);
        planes[i] = (uint8_t)(resid[i] >> 8);
        planes[npix + i] = (uint8_t)resid[i];
    }
    free(resid);

    uint8_t *lz0 = NULL, *lz1 = NULL;
    size_t len0 = 0, len1 = 0;
    rc = qtc_lz_encode(inter, npix * 2, &lz0, &len0);
    if (rc == QTC_OK)
        rc = qtc_lz_encode(planes, npix * 2, &lz1, &len1);
    free(inter);
    free(planes);
    if (rc != QTC_OK) {
        free(lz0);
        free(lz1);
        return rc;
    }

    int use_planes = len1 < len0;
    uint8_t *lz = use_planes ? lz1 : lz0;
    size_t lz_len = use_planes ? len1 : len0;
    free(use_planes ? lz0 : lz1);

    size_t total = IMAGE16_HDR + lz_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = 2;                        /* depth: 16-bit */
    buf[10] = (uint8_t)(use_planes ? 1 : 0);
    buf[11] = quant;
    memcpy(buf + IMAGE16_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_image16_decode(const uint8_t *payload, size_t payload_len,
                       uint16_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                       uint8_t *out_ch)
{
    if (payload_len < IMAGE16_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];
    uint8_t depth = payload[9];
    uint8_t layout = payload[10];

    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_CORRUPT;
    if (depth != 2 || layout > 1)
        return QTC_ERR_FORMAT;

    /* Guard against a crafted header forcing a huge allocation. */
    uint64_t npix64 = (uint64_t)w * h * ch;
    if (npix64 > ((uint64_t)1 << 32))
        return QTC_ERR_SIZE;
    size_t npix = (size_t)npix64;

    uint8_t *bytes = (uint8_t *)malloc(npix ? npix * 2 : 1);
    uint16_t *resid = (uint16_t *)malloc((npix ? npix : 1) * sizeof(uint16_t));
    uint16_t *px = (uint16_t *)malloc((npix ? npix : 1) * sizeof(uint16_t));
    if (!bytes || !resid || !px) {
        free(bytes); free(resid); free(px);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + IMAGE16_HDR, payload_len - IMAGE16_HDR,
                           npix * 2, bytes, &consumed);
    if (rc != QTC_OK) {
        free(bytes); free(resid); free(px);
        return rc;
    }

    if (layout == 0) {
        for (size_t i = 0; i < npix; i++)
            resid[i] = (uint16_t)(bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8));
    } else {
        for (size_t i = 0; i < npix; i++)
            resid[i] = (uint16_t)(bytes[npix + i] | ((uint16_t)bytes[i] << 8));
    }
    free(bytes);

    image16_predict_inverse(resid, w, h, ch, px);
    free(resid);

    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}
