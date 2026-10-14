/*
 * qtc_audio_mdct2.c — v0.13.0: perceptual lossy audio coder (Phase 5.7).
 *
 * Rebuild of the lossy audio lens aimed at MP3-class bitrates (90-130 kbps
 * stereo). The v1 MDCT lens used one global quantization step shaped by a
 * fixed curve, coded channels independently, and had no rate control; it
 * bottomed out around 200 kbps. v2 adds the four things that make MP3
 * efficient:
 *
 *   1. Mid/side joint stereo — for 2-channel input, encode M=(L+R)/2 and
 *      S=(L-R)/2; the side signal usually carries far less energy and is
 *      quantized much more coarsely at equal perceived quality.
 *   2. A larger transform — N=1024 (2048-sample window, ~46 ms at 44.1 kHz)
 *      for better energy compaction of tonal content.
 *   3. Masking-based bit allocation — per frame, per perceptual band:
 *      measure energy, spread masking across bands, apply the absolute
 *      threshold of hearing, and only spend bits where quantization
 *      noise would be audible. Bands whose energy sits below the raised
 *      noise floor are switched off entirely, and a "knee" rule refuses
 *      to code any band at less than 2 bits/coefficient (below the knee
 *      the bits are better spent elsewhere).
 *   4. Exact rate control — coefficients are packed at a fixed per-band
 *      bit width (offset binary); a global gain lambda is binary-searched
 *      against the ACTUAL post-LZ stream size (the MDCT is computed once
 *      and cached, so each search step re-runs only quantize+pack+LZ).
 *
 * Per-frame side info per band: 7-bit quantization-step index
 * (delta = 2^((idx-40)/4), idx 127 = band off) + 4-bit coefficient width
 * (0 = all zero, else bits per coefficient, offset-binary packed).
 *
 * Payload layout (14-byte header, same shape as v1):
 *   [u32 nframes][u8 channels][u8 bps][u16 N][u8 qbyte][u8 flags][u32 raw_len]
 *   [LZ payload of: per channel, per frame:
 *       [packed band side info][bit-packed coefficients]]
 *
 * flags bit0: mid/side stereo (channels==2 only).
 *
 * The bitrate target is expressed as bits per MDCT coefficient, which is
 * sample-rate independent: the same target yields the same perceived
 * quality at any rate, with byte size scaling with duration. The
 * target_kbps argument (or the quality knob) is converted assuming a
 * 44.1 kHz reference.
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MDCT2_N          1024
#define MDCT2_MAX_BANDS  24
#define MDCT2_OFF        127     /* band step index meaning "band off"   */
#define MDCT2_RATE_REF   44100.0 /* reference rate for kbps conversion   */
#define MDCT2_MAX_WIDTH  15      /* 4-bit field for coefficient width    */

/* ── small byte helpers (same convention as v1) ─────────────────────── */

static void put32b(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32b(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── bitstream writer/reader (MSB-first) ────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   nbits;
    int      overflow;
} bw_t;

static void bw_put(bw_t *b, uint32_t v, int bits)
{
    if (bits <= 0)
        return;
    if (b->nbits + (size_t)bits > b->cap * 8) {
        b->overflow = 1;
        return;
    }
    for (int i = bits - 1; i >= 0; i--) {
        size_t byte = b->nbits >> 3;
        int bit = 7 - (int)(b->nbits & 7);
        if ((v >> i) & 1u)
            b->buf[byte] |= (uint8_t)(1u << bit);
        b->nbits++;
    }
}

typedef struct {
    const uint8_t *buf;
    size_t         nbits;   /* total available */
    size_t         pos;
    int            error;
} br_t;

static uint32_t br_get(br_t *b, int bits)
{
    uint32_t v = 0;
    if (bits <= 0)
        return 0;
    if (b->pos + (size_t)bits > b->nbits) {
        b->error = 1;
        return 0;
    }
    for (int i = 0; i < bits; i++) {
        size_t byte = b->pos >> 3;
        int bit = 7 - (int)(b->pos & 7);
        v = (v << 1) | (uint32_t)((b->buf[byte] >> bit) & 1u);
        b->pos++;
    }
    return v;
}

/* ── sample I/O: everything runs on a 16-bit-equivalent scale ───────── */

static inline double read_norm(const uint8_t *p, int bps)
{
    if (bps == 8)
        return ((double)p[0] - 128.0) * 256.0;
    int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (double)s;
}

static inline void write_norm(uint8_t *p, double v, int bps)
{
    int32_t x = (int32_t)llround(v);
    if (bps == 8) {
        int32_t y = (x >> 8) + 128;
        if (y < 0) y = 0;
        if (y > 255) y = 255;
        p[0] = (uint8_t)y;
    } else {
        if (x < -32768) x = -32768;
        if (x > 32767) x = 32767;
        p[0] = (uint8_t)x;
        p[1] = (uint8_t)(x >> 8);
    }
}

/* ── perceptual band table (deterministic from N) ───────────────────── */

typedef struct {
    int    nbands;
    int    edge[MDCT2_MAX_BANDS + 1];  /* bin edges, edge[nbands] == N   */
    double coord[MDCT2_MAX_BANDS];      /* log2 spreading coordinate      */
    double ath[MDCT2_MAX_BANDS];       /* abs. threshold, dB re fullscale*/
    double loss[MDCT2_MAX_BANDS][MDCT2_MAX_BANDS]; /* spread loss L[b][s]*/
} bands2_t;

static int bands2_build(bands2_t *bt, int N)
{
    memset(bt, 0, sizeof(*bt));
    int edges[MDCT2_MAX_BANDS + 1];
    int nb = 0;
    int e = 0;
    edges[0] = 0;
    int lo_w = N / 102;
    if (lo_w < 1) lo_w = 1;
    for (int i = 0; i < 4 && e + lo_w < N && nb < MDCT2_MAX_BANDS; i++) {
        e += lo_w;
        edges[++nb] = e;
    }
    while (e < N && nb < MDCT2_MAX_BANDS) {
        int ne = e + (int)(e * 0.32 + 0.5);
        if (ne <= e) ne = e + 1;
        if (ne >= N) {
            edges[++nb] = N;
            break;
        }
        edges[++nb] = ne;
        e = ne;
    }
    if (edges[nb] != N || nb < 1)
        return QTC_ERR_FORMAT;
    bt->nbands = nb;
    memcpy(bt->edge, edges, (size_t)(nb + 1) * sizeof(int));
    for (int b = 0; b < nb; b++) {
        double center = 0.5 * (edges[b] + edges[b + 1]);
        bt->coord[b] = log2(center + 1.0);
        /* Absolute threshold of hearing (Terhardt approximation), in dB
         * SPL, referenced to full-scale sine = 93 dB SPL. Frequency is
         * taken as a fraction of a 22.05 kHz Nyquist; at other rates the
         * curve is slightly off but the allocation still behaves. */
        double f = (center / (double)N) * 22.05;
        if (f < 0.02) f = 0.02;
        double spl = 3.64 * pow(f, -0.8) -
                     6.5 * exp(-0.6 * (f - 3.3) * (f - 3.3)) +
                     0.001 * f * f * f * f;
        bt->ath[b] = spl - 93.0;
    }
    /* Spreading loss from source band s into target band b: asymmetric,
     * 24 dB/octave toward higher bands, 10 dB/octave toward lower.
     * Precomputed once — the allocation runs this matrix thousands of
     * times per stream. */
    for (int b = 0; b < nb; b++) {
        for (int s = 0; s < nb; s++) {
            double dz = bt->coord[b] - bt->coord[s];
            bt->loss[b][s] = (dz > 0.0) ? 24.0 * dz : -10.0 * dz;
        }
    }
    return QTC_OK;
}

/* Side info per band: 7-bit step index + 4-bit width = 11 bits. */
static inline int bands2_side_bytes(const bands2_t *bt)
{
    return (bt->nbands * 11 + 7) / 8;
}

static inline double step_of_idx(int idx)
{
    return pow(2.0, ((double)idx - 40.0) / 4.0);
}

static inline int idx_of_step(double delta)
{
    if (delta <= 0.0)
        return 0;
    int idx = (int)llround(4.0 * log2(delta) + 40.0);
    if (idx < 0) idx = 0;
    if (idx > 126) idx = 126;
    return idx;
}

/* Smallest width (2..15) whose offset-binary range [-half, half-1]
 * covers [-maxq, maxq], i.e. half >= maxq+1; 0 if maxq==0. */
static inline int width_for_maxq(int32_t maxq)
{
    if (maxq <= 0)
        return 0;
    int w = 2;
    while (w <= MDCT2_MAX_WIDTH && (1 << (w - 1)) <= maxq)
        w++;
    if (w > MDCT2_MAX_WIDTH)
        w = MDCT2_MAX_WIDTH;
    return w;
}

/* ── quality knob → bits-per-coefficient budget ─────────────────────── */

static double quality_to_beta(float quality)
{
    /* Anchors chosen so a 44.1 kHz stereo stream lands on familiar
     * bitrates: q=1.0→320k, 0.85→192k, 0.5→128k, 0.25→96k, 0.0→48k.
     * beta = bits per MDCT coefficient (per transformed channel); total
     * kbps ~= beta * 44.1 * nchannels, so the same beta means the same
     * per-coefficient quality regardless of channel count. */
    static const float qs[5] = {0.0f, 0.25f, 0.5f, 0.85f, 1.0f};
    static const double kb[5] = {48.0, 96.0, 128.0, 192.0, 320.0};
    double kbps;
    if (quality <= 0.0f)
        kbps = kb[0];
    else if (quality >= 1.0f)
        kbps = kb[4];
    else {
        int i = 1;
        while (i < 5 && quality > qs[i])
            i++;
        if (i > 4) i = 4;
        double t = ((double)quality - (double)qs[i - 1]) /
                   ((double)qs[i] - (double)qs[i - 1]);
        double lkb = log2(kb[i - 1]) + t * (log2(kb[i]) - log2(kb[i - 1]));
        kbps = pow(2.0, lkb);
    }
    return kbps / (2.0 * MDCT2_RATE_REF / 1000.0);
}

/* ── bit allocation: energies + lambda → per-frame band decisions ───── */

/* Per-frame per-band analysis: E = energy, MX = max |coefficient|. */
typedef struct {
    uint8_t idx;      /* step index, MDCT2_OFF = off          */
    uint8_t width;    /* predicted coefficient width (bits)   */
} band_dec_t;

static void alloc_decisions(const bands2_t *bt, const float *E,
                            const float *MX, size_t mf, int nch,
                            double lambda, band_dec_t *dec,
                            double *est_bits_out)
{
    int nb = bt->nbands;
    double est = 0.0;
    double sl[MDCT2_MAX_BANDS], mask[MDCT2_MAX_BANDS];
    double floor_db = 10.0 * log10(lambda > 0.0 ? lambda : 1e-30);
    for (int c = 0; c < nch; c++) {
        for (size_t m = 0; m < mf; m++) {
            const float *src = E + ((size_t)c * mf + m) * (size_t)nb;
            const float *mx = MX + ((size_t)c * mf + m) * (size_t)nb;
            band_dec_t *d = dec + ((size_t)c * mf + m) * (size_t)nb;
            /* Per-band level once; the masking spread is then a plain
             * max over the precomputed loss matrix. */
            for (int s = 0; s < nb; s++) {
                int ws = bt->edge[s + 1] - bt->edge[s];
                sl[s] = 10.0 * log10((double)src[s] / ws + 1e-20);
            }
            for (int b = 0; b < nb; b++) {
                double mk = -1e9;
                for (int s = 0; s < nb; s++) {
                    double v = sl[s] - bt->loss[b][s];
                    if (v > mk) mk = v;
                }
                mask[b] = mk - 12.0;
            }
            for (int b = 0; b < nb; b++) {
                int w = bt->edge[b + 1] - bt->edge[b];
                double lvl = sl[b];
                double T = (bt->ath[b] > mask[b]) ? bt->ath[b] : mask[b];
                /* Audibility gate + noise floor: a band below its masking
                 * threshold contributes nothing audible, and once lambda
                 * raises the global noise floor above a band's own level,
                 * coding it would add more noise than the signal it carries
                 * — either way, off costs only the 11 side-info bits.
                 * With the geometric-noise law below, band noise lands at
                 * (T + floor)/2 dB, so that is the off criterion. */
                if (2.0 * lvl < T + floor_db) {
                    d[b].idx = MDCT2_OFF;
                    d[b].width = 0;
                    est += 11.0;
                    continue;
                }
                /* Perceptual step, geometric law: noise_b = sqrt(lambda *
                 * allow_b). Two reference points bracket it:
                 *  - pure water-filling min(lambda, allow) maximizes SNR
                 *    but has a size plateau (every band pinned at its
                 *    threshold) that sits ABOVE MP3-class budgets — the
                 *    search can never reach them (measured: 175 kbps floor
                 *    on the reference song);
                 *  - pure multiplicative lambda*allow reaches any bitrate
                 *    but pins every loud band at ~the SMR offset — global
                 *    SNR stuck near 9 dB at 128 kbps vs LAME's 21.7.
                 * The geometric mean keeps size monotonic in lambda all
                 * the way down while giving loud bands (large allow)
                 * proportionally more SNR, which is where global SNR
                 * comes from. */
                double allow = pow(10.0, T / 10.0);
                double delta = sqrt(12.0 * sqrt(lambda * allow));
                int idx = idx_of_step(delta);
                int32_t maxq = (int32_t)llround((double)mx[b] /
                                                step_of_idx(idx));
                int width = width_for_maxq(maxq);
                /* Knee: a band coded at <2 bits/coeff is better off
                 * (its bits are worth more elsewhere). */
                if (width < 2) {
                    d[b].idx = MDCT2_OFF;
                    d[b].width = 0;
                    est += 11.0;
                    continue;
                }
                d[b].idx = (uint8_t)idx;
                d[b].width = (uint8_t)width;
                est += 11.0 + (double)w * (double)width;
            }
        }
    }
    if (est_bits_out)
        *est_bits_out = est;
}

/* Pack one candidate allocation into the raw bitstream from the stored
 * transform coefficients. The forward MDCT does not depend on lambda, so
 * the rate-control search reuses one coefficient buffer and only re-runs
 * this (cheap) quantize+pack step plus LZ per candidate. */
static int pack_stream(const bands2_t *bt, const band_dec_t *dec,
                       const float *coef_all, size_t mf, int nch,
                       uint8_t *raw, size_t raw_cap, size_t *out_raw_len)
{
    int nb = bt->nbands;
    int N = bt->edge[nb];
    /* bw_put only ORs 1-bits and the raw buffer is reused across search
     * steps, so it must start clean each time. */
    memset(raw, 0, raw_cap);
    bw_t bw;
    bw.buf = raw;
    bw.cap = raw_cap;
    bw.nbits = 0;
    bw.overflow = 0;
    for (int c = 0; c < nch; c++) {
        for (size_t m = 0; m < mf; m++) {
            const float *cs = coef_all + (((size_t)c * mf + m) * (size_t)N);
            const band_dec_t *d = dec + ((size_t)c * mf + m) * (size_t)nb;
            for (int b = 0; b < nb; b++) {
                bw_put(&bw, d[b].idx, 7);
                bw_put(&bw, d[b].width, 4);
            }
            for (int b = 0; b < nb; b++) {
                if (d[b].idx == MDCT2_OFF || d[b].width == 0)
                    continue;
                double delta = step_of_idx(d[b].idx);
                int half = 1 << (d[b].width - 1);
                for (int k = bt->edge[b]; k < bt->edge[b + 1]; k++) {
                    int32_t q = (int32_t)llround((double)cs[k] / delta);
                    if (q > half - 1) q = half - 1;
                    if (q < -half) q = -half;
                    bw_put(&bw, (uint32_t)(q + half), d[b].width);
                }
            }
        }
    }
    if (bw.overflow)
        return QTC_ERR_SIZE;
    *out_raw_len = (bw.nbits + 7) / 8;
    return QTC_OK;
}

/* ── encoder ────────────────────────────────────────────────────────── */

int qtc_audio_mdct2_encode(const uint8_t *pcm, uint32_t nframes,
                           uint8_t channels, uint8_t bps, float quality,
                           int target_kbps,
                           uint8_t **out, size_t *out_len)
{
    int N = MDCT2_N;
    if (nframes == 0 || channels == 0 || channels > 8)
        return QTC_ERR_FORMAT;
    if (bps != 8 && bps != 16)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    uint8_t qbyte = (uint8_t)(quality * 255.0f + 0.5f);

    int bps_bytes = bps / 8;
    int ms = (channels == 2);
    int nch = channels;   /* transformed channel count (M,S for stereo) */

    bands2_t bt;
    int rc = bands2_build(&bt, N);
    if (rc != QTC_OK)
        return rc;
    int nb = bt.nbands;
    int side_bytes = bands2_side_bytes(&bt);

    double beta = (target_kbps > 0)
        ? (double)target_kbps / ((MDCT2_RATE_REF / 1000.0) * nch)
        : quality_to_beta(quality);

    size_t mf = ((size_t)nframes + (size_t)N - 1) / (size_t)N + 1;
    size_t total = (mf + 1) * (size_t)N;
    if (mf > ((size_t)1 << 24))
        return QTC_ERR_SIZE;

    float *E = (float *)malloc((size_t)nch * mf * (size_t)nb * sizeof(float));
    float *MX = (float *)malloc((size_t)nch * mf * (size_t)nb * sizeof(float));
    band_dec_t *dec = (band_dec_t *)malloc((size_t)nch * mf * (size_t)nb *
                                           sizeof(band_dec_t));
    float *coef_all = (float *)malloc((size_t)nch * mf * (size_t)N *
                                      sizeof(float));
    double *sig = (double *)calloc(total, sizeof(double));
    double *frame = (double *)malloc((size_t)2 * N * sizeof(double));
    double *coef = (double *)malloc((size_t)N * sizeof(double));
    if (!E || !MX || !dec || !coef_all || !sig || !frame || !coef) {
        free(E); free(MX); free(dec); free(coef_all);
        free(sig); free(frame); free(coef);
        return QTC_ERR_ALLOC;
    }

    qtc_mdct_kernel_t kern;
    rc = qtc_mdct_kernel_init(&kern, N);
    if (rc != QTC_OK) {
        free(E); free(MX); free(dec); free(coef_all);
        free(sig); free(frame); free(coef);
        return rc;
    }

    /* Pass 1: forward MDCT every frame of every transformed channel; store
     * the coefficients (the rate-control search reuses them) and record
     * band energies + max coefficient magnitudes. For stereo the channels
     * are M and S. */
    for (int c = 0; c < nch; c++) {
        memset(sig, 0, total * sizeof(double));
        for (uint32_t f = 0; f < nframes; f++) {
            const uint8_t *p = pcm + (size_t)f * channels * bps_bytes;
            double v;
            if (ms) {
                double l = read_norm(p, bps);
                double r = read_norm(p + bps_bytes, bps);
                v = (c == 0) ? 0.5 * (l + r) : 0.5 * (l - r);
            } else {
                v = read_norm(p + c * bps_bytes, bps);
            }
            sig[N + f] = v;
        }
        for (size_t m = 0; m < mf; m++) {
            for (int n = 0; n < 2 * N; n++)
                frame[n] = sig[m * N + n];
            qtc_mdct_forward(&kern, frame, coef);
            float *cdst = coef_all + (((size_t)c * mf + m) * (size_t)N);
            for (int k = 0; k < N; k++)
                cdst[k] = (float)coef[k];
            float *edst = E + ((size_t)c * mf + m) * (size_t)nb;
            float *mdst = MX + ((size_t)c * mf + m) * (size_t)nb;
            for (int b = 0; b < nb; b++) {
                double s = 0.0, mxv = 0.0;
                for (int k = bt.edge[b]; k < bt.edge[b + 1]; k++) {
                    s += coef[k] * coef[k];
                    double a = (coef[k] < 0.0) ? -coef[k] : coef[k];
                    if (a > mxv) mxv = a;
                }
                edst[b] = (float)s;
                mdst[b] = (float)mxv;
            }
        }
    }

    /* The transform is done; the kernel and time-domain buffers are no
     * longer needed (pack_stream works from coef_all). */
    qtc_mdct_kernel_free(&kern);
    free(sig); free(frame); free(coef);
    sig = NULL; frame = NULL; coef = NULL;

    /* Rate control: binary-search the global gain lambda so the FINAL
     * (post-LZ) stream size lands on the budget. The forward MDCT is
     * lambda-independent and already stored in coef_all, so each search
     * step is just alloc_decisions + pack_stream + one LZ pass — no
     * re-transform. Larger lambda = coarser quantization = smaller file.
     * Invariant: lo is always a lambda whose file is too big, hi always a
     * lambda whose file fits; we converge on the finest lambda that still
     * fits, which maximizes quality at the size cap. (An earlier version
     * targeted the raw packed size and corrected by an estimated LZ ratio;
     * the ratio itself moves with lambda, so that feedback oscillated and
     * overshot the budget by ~37%.) */
    /* Budget in bits for the whole stream, derived from the target rate
     * over the REAL duration (nframes, not the zero-padded mf*N), so short
     * clips don't overshoot the target by the padding fraction. Subtract
     * the 16-byte container header, which the rate search does not model
     * (it only sees the 14-byte payload header + LZ payload). */
    double eff_kbps = beta * (MDCT2_RATE_REF / 1000.0) * (double)nch;
    double budget_bits = eff_kbps * 1000.0 *
                         ((double)nframes / MDCT2_RATE_REF) - 128.0;
    if (budget_bits < 2048.0)
        budget_bits = 2048.0;
    size_t raw_cap = ((size_t)nch * mf * ((size_t)side_bytes +
                      (size_t)N * 2)) + 64;
    if (raw_cap > ((size_t)1 << 32)) {
        free(E); free(MX); free(dec); free(coef_all);
        return QTC_ERR_SIZE;
    }
    uint8_t *raw = (uint8_t *)malloc(raw_cap);
    uint8_t *lz = NULL;
    size_t raw_len = 0, lz_len = 0;
    if (!raw) {
        free(E); free(MX); free(dec); free(coef_all);
        return QTC_ERR_ALLOC;
    }

    size_t hdr = 4 + 1 + 1 + 2 + 1 + 1 + 4;
    double lo = 1e-8, hi = 1e10, lambda = 1.0;
    for (int iter = 0; iter < 22; iter++) {
        lambda = sqrt(lo * hi);
        alloc_decisions(&bt, E, MX, mf, nch, lambda, dec, NULL);
        rc = pack_stream(&bt, dec, coef_all, mf, nch, raw, raw_cap, &raw_len);
        if (rc != QTC_OK) {
            free(raw); free(E); free(MX); free(dec); free(coef_all);
            return rc;
        }
        free(lz); lz = NULL;
        rc = qtc_lz_encode(raw, raw_len, &lz, &lz_len);
        if (rc != QTC_OK) {
            free(raw); free(lz); free(E); free(MX); free(dec); free(coef_all);
            return rc;
        }
        double final_bits = (double)(hdr + lz_len) * 8.0;
        if (final_bits > budget_bits)
            lo = lambda;      /* too big: coarsen */
        else
            hi = lambda;      /* fits: try finer  */
    }

    /* Final encode at the finest lambda that fits. */
    lambda = hi;
    alloc_decisions(&bt, E, MX, mf, nch, lambda, dec, NULL);
    rc = pack_stream(&bt, dec, coef_all, mf, nch, raw, raw_cap, &raw_len);
    free(E); free(MX); free(dec); free(coef_all);
    if (rc != QTC_OK) {
        free(raw); free(lz);
        return rc;
    }
    free(lz); lz = NULL;
    rc = qtc_lz_encode(raw, raw_len, &lz, &lz_len);
    free(raw);
    if (rc != QTC_OK) {
        free(lz);
        return rc;
    }

    uint8_t *buf = (uint8_t *)malloc(hdr + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32b(buf, nframes);
    buf[4] = channels;
    buf[5] = bps;
    buf[6] = (uint8_t)(N & 0xFF);
    buf[7] = (uint8_t)(N >> 8);
    buf[8] = qbyte;
    buf[9] = (uint8_t)(ms ? 1 : 0);
    put32b(buf + 10, (uint32_t)raw_len);
    memcpy(buf + hdr, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = hdr + lz_len;
    return QTC_OK;
}

/* ── decoder ────────────────────────────────────────────────────────── */

int qtc_audio_mdct2_decode(const uint8_t *payload, size_t payload_len,
                           uint8_t **out_pcm, uint32_t *out_nframes,
                           uint8_t *out_channels, uint8_t *out_bps)
{
    size_t hdr = 4 + 1 + 1 + 2 + 1 + 1 + 4;
    if (payload_len < hdr)
        return QTC_ERR_CORRUPT;

    uint32_t nframes = get32b(payload);
    uint8_t channels = payload[4];
    uint8_t bps = payload[5];
    int N = (int)payload[6] | ((int)payload[7] << 8);
    uint8_t flags = payload[9];

    if (nframes == 0 || channels == 0 || channels > 8)
        return QTC_ERR_CORRUPT;
    if (bps != 8 && bps != 16)
        return QTC_ERR_CORRUPT;
    if (N < 16 || N > 4096 || (N & (N - 1)) != 0)
        return QTC_ERR_CORRUPT;
    if ((flags & ~1u) != 0)
        return QTC_ERR_CORRUPT;
    if ((flags & 1u) && channels != 2)
        return QTC_ERR_CORRUPT;
    int ms = (flags & 1u) && channels == 2;

    int bps_bytes = bps / 8;
    bands2_t bt;
    if (bands2_build(&bt, N) != QTC_OK)
        return QTC_ERR_CORRUPT;
    int nb = bt.nbands;
    int side_bytes = bands2_side_bytes(&bt);

    size_t mf = ((size_t)nframes + (size_t)N - 1) / (size_t)N + 1;
    size_t total = (mf + 1) * (size_t)N;
    size_t coeff_count = mf * (size_t)N * channels;
    if (coeff_count > ((size_t)1 << 30))
        return QTC_ERR_SIZE;
    if ((size_t)nframes * channels * (size_t)bps_bytes > ((size_t)1 << 31))
        return QTC_ERR_SIZE;

    uint32_t stream_len = get32b(payload + 10);
    /* Worst case: side info + width-15 coefficients for every bin. */
    uint64_t max_raw = (uint64_t)mf * channels *
                       ((uint64_t)side_bytes + (uint64_t)N * 2) + 64;
    if (stream_len == 0 || (uint64_t)stream_len > max_raw)
        return QTC_ERR_CORRUPT;

    uint8_t *raw = (uint8_t *)malloc(stream_len);
    if (!raw)
        return QTC_ERR_ALLOC;
    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + hdr, payload_len - hdr, stream_len,
                           raw, &consumed);
    if (rc != QTC_OK) {
        free(raw);
        return rc;
    }

    /* M/S needs both transformed channels live at once to unmix L/R. */
    double *sig = (double *)calloc(total, sizeof(double));
    double *sig2 = ms ? (double *)calloc(total, sizeof(double)) : NULL;
    double *coef = (double *)malloc((size_t)N * sizeof(double));
    double *rec = (double *)malloc((size_t)2 * N * sizeof(double));
    uint8_t *pcm = (uint8_t *)malloc((size_t)nframes * channels * bps_bytes);
    if (!sig || (ms && !sig2) || !coef || !rec || !pcm) {
        free(raw); free(sig); free(sig2); free(coef); free(rec); free(pcm);
        return QTC_ERR_ALLOC;
    }

    qtc_mdct_kernel_t kern;
    rc = qtc_mdct_kernel_init(&kern, N);
    if (rc != QTC_OK) {
        free(raw); free(sig); free(sig2); free(coef); free(rec); free(pcm);
        return rc;
    }

    br_t br;
    br.buf = raw;
    br.nbits = (size_t)stream_len * 8;
    br.pos = 0;
    br.error = 0;

    for (int c = 0; c < channels && rc == QTC_OK; c++) {
        double *dst_sig = (ms && c == 1) ? sig2 : sig;
        memset(dst_sig, 0, total * sizeof(double));
        for (size_t m = 0; m < mf && rc == QTC_OK; m++) {
            uint8_t sidx[MDCT2_MAX_BANDS];
            uint8_t swid[MDCT2_MAX_BANDS];
            for (int b = 0; b < nb; b++) {
                sidx[b] = (uint8_t)br_get(&br, 7);
                swid[b] = (uint8_t)br_get(&br, 4);
            }
            if (br.error) {
                rc = QTC_ERR_CORRUPT;
                break;
            }
            int got = 0;
            for (int b = 0; b < nb && rc == QTC_OK; b++) {
                int w = bt.edge[b + 1] - bt.edge[b];
                if (sidx[b] == MDCT2_OFF) {
                    if (swid[b] != 0)
                        rc = QTC_ERR_CORRUPT;
                    for (int k = 0; k < w; k++)
                        coef[got + k] = 0.0;
                    got += w;
                    continue;
                }
                if (swid[b] == 0) {
                    for (int k = 0; k < w; k++)
                        coef[got + k] = 0.0;
                    got += w;
                    continue;
                }
                if (swid[b] > MDCT2_MAX_WIDTH) {
                    rc = QTC_ERR_CORRUPT;
                    break;
                }
                double delta = step_of_idx(sidx[b]);
                int half = 1 << (swid[b] - 1);
                for (int k = 0; k < w; k++) {
                    uint32_t v = br_get(&br, swid[b]);
                    if (br.error)
                        break;
                    coef[got + k] = ((double)(int32_t)v - half) * delta;
                }
                if (br.error) {
                    rc = QTC_ERR_CORRUPT;
                    break;
                }
                got += w;
            }
            if (rc != QTC_OK || got != N) {
                rc = QTC_ERR_CORRUPT;
                break;
            }
            qtc_mdct_inverse(&kern, coef, rec);
            for (int n = 0; n < 2 * N; n++)
                dst_sig[m * N + n] += rec[n];
        }
        if (rc == QTC_OK && !ms) {
            for (uint32_t f = 0; f < nframes; f++) {
                uint8_t *q = pcm + ((size_t)f * channels + c) * bps_bytes;
                write_norm(q, sig[N + f], bps);
            }
        }
    }

    if (rc == QTC_OK && ms) {
        for (uint32_t f = 0; f < nframes; f++) {
            double m = sig[N + f], s = sig2[N + f];
            uint8_t *q = pcm + (size_t)f * 2 * bps_bytes;
            write_norm(q, m + s, bps);
            write_norm(q + bps_bytes, m - s, bps);
        }
    }

    qtc_mdct_kernel_free(&kern);
    free(raw); free(sig); free(sig2); free(coef); free(rec);

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
