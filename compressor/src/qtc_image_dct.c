/*
 * qtc_image_dct.c — Phase 5.5b (v0.12.0): lossy 8x8 block-DCT image lens.
 *
 * The photographic gate. Mesh and accretion are smooth-content coders and
 * lose 13-20 dB to JPEG on photos; the 8x8 DCT is the transform that is
 * actually matched to photographic texture, so this lens fights JPEG on its
 * own ground. The rate-distortion core (DCT + quantization + 4:2:0 chroma)
 * is the proven one; the entropy backend is JPEG's architecture with a
 * strictly better coder: coefficients are serialized as zigzag-scanned
 * (run,size) symbols with sign-magnitude amplitude bits and DC-DPCM, but
 * the symbol stream is entropy-coded by a PER-IMAGE rANS model instead of
 * JPEG's per-image Huffman (rANS reaches fractional-bit costs Huffman
 * cannot), and the amplitude bits are stored raw, exactly as JPEG does.
 *
 * Serialization history (measured, kept honest): v0.12.0 first shipped raw
 * int16 byte planes + LZ (lost 35-48% vs JPEG at matched PSNR), then
 * (run,size,amplitude) + LZ (still ~20-38% behind on texture). The rANS
 * form below is the one that closes the gap.
 *
 * Encode:
 *   1. RGB -> YCbCr (BT.601). For 3-channel input, Cb/Cr are box-averaged
 *      to half resolution in each axis (4:2:0), as JPEG does for photos.
 *   2. Each plane is split into 8x8 blocks (edge blocks replicate border
 *      pixels), level-shifted by -128, forward-DCT'd (orthonormal,
 *      separable), and quantized by a JPEG-Annex-K table scaled by the
 *      quality knob.
 *   3. Per plane: coefficients are scanned in zigzag order; the DC term is
 *      stored as a difference from the previous block's DC (DPCM). Each
 *      non-zero coefficient emits a (run<<4)|size symbol plus `size`
 *      amplitude bits (sign-magnitude, JPEG convention); runs of 16 zeros
 *      emit 0xF0 (ZRL); the block ends with 0x00 (EOB).
 *   4. Luma symbols and chroma symbols are rANS-coded with SEPARATE
 *      per-image static models (chroma is far sparser than luma; JPEG also
 *      uses separate tables), each packed as 512 bytes of u16 freqs. All
 *      amplitude bits are concatenated and stored raw.
 *
 * Decode reverses every step and validates counts exactly. This is LOSSY;
 * quality is measured by PSNR/SSIM in the bench harness.
 *
 * Payload layout:
 *   [u32 w][u32 h][u8 ch][u8 q100][u8 subsample][u8 reserved]
 *   [u32 nsym_y][u32 nsym_c][u32 bits_y_len][u32 nampbytes]
 *   [512-byte luma rANS freq table][512-byte chroma rANS freq table]
 *   [rANS bitstream of nsym_y luma symbols]
 *   [rANS bitstream of nsym_c chroma symbols]
 *   [nampbytes raw amplitude bits]
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Header: [u32 w][u32 h][u8 ch][u8 q100][u8 subsample][u8 reserved]
 *         [u32 nsym_y][u32 nsym_c][u32 bits_y_len][u32 nampbytes]
 *         = 28 bytes. */
#define DCT_HDR 28u
#define DCT_SYM_ALPHA 256u
#define DCT_MODEL_BYTES (DCT_SYM_ALPHA * 2)

/* Worst-case symbol count per block: 64 non-zero coefficients + EOB, plus
 * ZRL expansion cannot exceed the coefficient count, so 70 is generous. */
#define DCT_SYM_PER_BLOCK 70u
/* Worst-case amplitude bits per block: 64 coefficients x 15 bits. */
#define DCT_AMP_BYTES_PER_BLOCK 120u

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

/* ── JPEG Annex K base quantization tables (row-major 8x8) ───────────── */

static const uint8_t LUMA_BASE[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

static const uint8_t CHROMA_BASE[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Zigzag scan: ZZ[i] = row-major index of the i-th coefficient. */
static const uint8_t ZZ[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Map a 0..100 quality to scaled quantization tables (JPEG's formula). */
static void build_quant(int q100, uint8_t Qy[64], uint8_t Qc[64])
{
    long scale = (q100 < 50) ? (5000L / q100) : (200L - 2L * q100);
    for (int i = 0; i < 64; i++) {
        long ly = ((long)LUMA_BASE[i] * scale + 50) / 100;
        long lc = ((long)CHROMA_BASE[i] * scale + 50) / 100;
        if (ly < 1) ly = 1;
        if (ly > 255) ly = 255;
        if (lc < 1) lc = 1;
        if (lc > 255) lc = 255;
        Qy[i] = (uint8_t)ly;
        Qc[i] = (uint8_t)lc;
    }
}

/* Orthonormal 8-point DCT-II matrix: C[k][n] = a(k) cos((2n+1)k*pi/16),
 * a(0)=sqrt(1/8), a(k>0)=sqrt(2/8). Computed per call (64 doubles, cheap);
 * no shared state, so concurrent encodes are safe (audit M2). */
static void build_dct_matrix(double C[64])
{
    for (int k = 0; k < 8; k++) {
        double a = (k == 0) ? sqrt(1.0 / 8.0) : sqrt(2.0 / 8.0);
        for (int n = 0; n < 8; n++)
            C[k * 8 + n] = a * cos((2.0 * n + 1.0) * k * M_PI / 16.0);
    }
}

/* Separable 2D forward DCT: out = C * in * C^T. in/out are 8x8 row-major. */
static void fdct8x8(const double in[64], double out[64], const double C[64])
{
    double tmp[64];
    for (int y = 0; y < 8; y++)          /* transform each row */
        for (int u = 0; u < 8; u++) {
            double s = 0.0;
            for (int x = 0; x < 8; x++)
                s += C[u * 8 + x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)          /* transform each column */
        for (int v = 0; v < 8; v++) {
            double s = 0.0;
            for (int y = 0; y < 8; y++)
                s += tmp[y * 8 + u] * C[v * 8 + y];
            out[v * 8 + u] = s;
        }
}

/* Separable 2D inverse DCT: out = C^T * in * C.
 * rlast[v] gives the highest nonzero column index of input row v (-1 if the
 * row is all zero). Skipping those lanes in the first pass is bit-exact —
 * adding an exact 0.0 cannot change a sum — and most 8x8 blocks carry only a
 * few nonzero coefficients across a 4K keyframe's ~194k blocks.
 * The second pass must always run the full v range: the row transform
 * spreads energy, so tmp[v][x] can be nonzero even when in[v][x] is zero. */
static void idct8x8(const double in[64], double out[64], const double C[64],
                    const int rlast[8])
{
    double tmp[64];
    for (int v = 0; v < 8; v++) {        /* C^T * in  (over rows u) */
        const int lu = rlast[v];
        if (lu < 0) {
            for (int x = 0; x < 8; x++) tmp[v * 8 + x] = 0.0;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            double s = 0.0;
            for (int u = 0; u <= lu; u++)
                s += C[u * 8 + x] * in[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; x++)          /* ... * C  (over columns v) */
        for (int y = 0; y < 8; y++) {
            double s = 0.0;
            for (int v = 0; v < 8; v++)
                s += tmp[v * 8 + x] * C[v * 8 + y];
            out[y * 8 + x] = s;
        }
}

/* Triangle (bilinear) 4:2:0 chroma upsampling, equivalent to libjpeg's
 * "fancy" upsampler: each output pixel is 9/16 of the nearest chroma
 * sample plus 3/16 + 3/16 + 1/16 of the three next-nearest, with edge
 * samples replicated. Chroma sample (cx,cy) sits at the center of the
 * 2x2 pixel box it was averaged from, so even output rows/cols lean on
 * the sample behind them and odd ones on the sample ahead. */
static void upsample_chroma_row(const double *c, uint32_t cw, uint32_t chh,
                                double *out, uint32_t w, uint32_t y)
{
    uint32_t r0 = y >> 1;
    int32_t r1 = (y & 1) ? (int32_t)r0 + 1 : (int32_t)r0 - 1;
    if (r1 < 0) r1 = 0;
    if ((uint32_t)r1 >= chh) r1 = (int32_t)chh - 1;
    if (r0 >= chh) r0 = chh - 1;
    const double *row0 = c + (size_t)r0 * cw;
    const double *row1 = c + (size_t)r1 * cw;
    for (uint32_t x = 0; x < w; x++) {
        uint32_t c0 = x >> 1;
        int32_t c1 = (x & 1) ? (int32_t)c0 + 1 : (int32_t)c0 - 1;
        if (c1 < 0) c1 = 0;
        if ((uint32_t)c1 >= cw) c1 = (int32_t)cw - 1;
        if (c0 >= cw) c0 = cw - 1;
        out[x] = (9.0 * row0[c0] + 3.0 * row0[c1] +
                  3.0 * row1[c0] + 1.0 * row1[c1]) * (1.0 / 16.0);
    }
}

/* Clamp-sample a plane at (x,y). */
static double getp(const double *p, uint32_t pw, uint32_t ph, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= pw) x = (int)pw - 1;
    if ((uint32_t)y >= ph) y = (int)ph - 1;
    return p[(size_t)y * pw + x];
}

/* Number of bits needed to represent |v| (0 -> 0). */
static int coeff_size(int32_t v)
{
    uint32_t a = (uint32_t)(v < 0 ? -v : v);
    int s = 0;
    while (a) {
        s++;
        a >>= 1;
    }
    return s;
}

/* ── MSB-first amplitude bit writer/reader ───────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   nbits;
} amp_bw_t;

static void amp_put(amp_bw_t *bw, uint32_t val, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        if ((val >> i) & 1u)
            bw->buf[bw->nbits >> 3] |= (uint8_t)(0x80u >> (bw->nbits & 7));
        bw->nbits++;
    }
}

typedef struct {
    const uint8_t *buf;
    size_t         nbits;      /* bits consumed */
    size_t         cap_bits;   /* bits available */
} amp_br_t;

/* Read nbits MSB-first; returns -1 on overrun. */
static int32_t amp_get(amp_br_t *br, int nbits)
{
    if (nbits == 0)
        return 0;
    if (br->nbits + (size_t)nbits > br->cap_bits)
        return -1;
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        val <<= 1;
        if (br->buf[br->nbits >> 3] & (0x80u >> (br->nbits & 7)))
            val |= 1u;
        br->nbits++;
    }
    return (int32_t)val;
}

/* ── per-plane symbol serialization ──────────────────────────────────── */

/* Quantize one plane's blocks and append (run,size) symbols + amplitude
 * bits to the shared buffers. sym_off/amp are advanced.
 *
 * Note: a rate-distortion "trellis" quantizer was prototyped here (the same
 * lever libjpeg-turbo uses) but measured WORSE on our stream: the per-image
 * rANS model prices rare (run,size) symbols near log2(N) bits, so the
 * trellis over-zeroed marginal coefficients and lost more PSNR than it saved
 * in bytes. Naive nearest-rounding is kept. */
static void encode_plane_syms(const double *plane, uint32_t pw, uint32_t ph,
                              uint32_t bx, uint32_t by, const uint8_t Q[64],
                              const double C[64], double *blk, double *dct,
                              uint16_t *sym, size_t *sym_off,
                              amp_bw_t *amp)
{
    int32_t prev_dc = 0;

    for (uint32_t byy = 0; byy < by; byy++)
        for (uint32_t bxx = 0; bxx < bx; bxx++) {
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++)
                    blk[j * 8 + i] = getp(plane, pw, ph,
                                          (int)(bxx * 8 + i),
                                          (int)(byy * 8 + j)) - 128.0;
            fdct8x8(blk, dct, C);

            int16_t q[64];                       /* zigzag order */
            for (int k = 0; k < 64; k++) {
                int rm = ZZ[k];                /* row-major index */
                double val = dct[rm] / (double)Q[rm];
                q[k] = (int16_t)llrint(val);
            }

            /* DC: DPCM against the previous block's DC. */
            int32_t dc = q[0];
            int32_t diff = dc - prev_dc;
            prev_dc = dc;
            int dsz = coeff_size(diff);
            sym[(*sym_off)++] = (uint16_t)dsz;   /* run nibble = 0 for DC */
            if (dsz > 0) {
                uint32_t av = (diff < 0)
                                  ? (uint32_t)(diff + (1 << dsz) - 1)
                                  : (uint32_t)diff;
                amp_put(amp, av, dsz);
            }

            /* AC: (run,size) symbols, ZRL for runs of 16, EOB at end. */
            int run = 0;
            for (int k = 1; k < 64; k++) {
                if (q[k] == 0) {
                    run++;
                    continue;
                }
                while (run >= 16) {
                    sym[(*sym_off)++] = 0xF0;
                    run -= 16;
                }
                int sz = coeff_size(q[k]);
                sym[(*sym_off)++] = (uint16_t)((run << 4) | sz);
                uint32_t av = (q[k] < 0)
                                  ? (uint32_t)(q[k] + (1 << sz) - 1)
                                  : (uint32_t)q[k];
                amp_put(amp, av, sz);
                run = 0;
            }
            sym[(*sym_off)++] = 0x00;            /* EOB */
        }
}

/* Parse one plane's symbols (from the decoded rANS stream) + amplitude
 * bits and reconstruct the plane by dequantizing + inverse DCT. All reads
 * are bounds-checked. */
static int decode_plane_syms(const uint16_t *sym, size_t *si, size_t nsym,
                             amp_br_t *br,
                             uint32_t bx, uint32_t by, const uint8_t Q[64],
                             const double C[64], double *blk, double *dct,
                             double *plane, uint32_t pw, uint32_t ph)
{
    int32_t prev_dc = 0;

    for (uint32_t byy = 0; byy < by; byy++)
        for (uint32_t bxx = 0; bxx < bx; bxx++) {
            int16_t q[64];
            memset(q, 0, sizeof(q));

            /* DC symbol. */
            if (*si >= nsym)
                return QTC_ERR_CORRUPT;
            uint16_t b0 = sym[(*si)++];
            if (b0 > 0x0F)                       /* DC must have run nibble 0 */
                return QTC_ERR_CORRUPT;
            int dsz = (int)(b0 & 0x0F);
            if (dsz > 12)
                return QTC_ERR_CORRUPT;
            int32_t diff = 0;
            if (dsz > 0) {
                int32_t av = amp_get(br, dsz);
                if (av < 0)
                    return QTC_ERR_CORRUPT;
                diff = ((av >> (dsz - 1)) & 1)
                           ? av
                           : av - (int32_t)((1u << dsz) - 1);
            }
            prev_dc += diff;
            q[0] = (int16_t)prev_dc;

            /* AC symbols until EOB. */
            int k = 1;
            for (;;) {
                if (*si >= nsym)
                    return QTC_ERR_CORRUPT;
                uint16_t b = sym[(*si)++];
                if (b == 0x00)
                    break;                     /* EOB */
                if (b == 0xF0) {               /* ZRL: 16 zeros */
                    k += 16;
                    if (k >= 64)
                        return QTC_ERR_CORRUPT;
                    continue;
                }
                int run = b >> 4;
                int sz = b & 0x0F;
                if (sz == 0)
                    return QTC_ERR_CORRUPT;    /* only 0x00 may have size 0 */
                k += run;
                if (k >= 64)
                    return QTC_ERR_CORRUPT;
                int32_t av = amp_get(br, sz);
                if (av < 0)
                    return QTC_ERR_CORRUPT;
                int32_t v = ((av >> (sz - 1)) & 1)
                                ? av
                                : av - (int32_t)((1u << sz) - 1);
                q[k] = (int16_t)v;
                k++;
            }

            /* Dequantize (undo zigzag) + inverse DCT + write pixels. */
            for (int z = 0; z < 64; z++) {
                int rm = ZZ[z];
                dct[(rm / 8) * 8 + (rm % 8)] = (double)q[z] * (double)Q[rm];
            }
            int rlast[8];
            for (int v = 0; v < 8; v++) rlast[v] = -1;
            for (int v = 0; v < 8; v++)
                for (int u = 0; u < 8; u++)
                    if (dct[v * 8 + u] != 0.0) rlast[v] = u;
            idct8x8(dct, blk, C, rlast);
            if (bxx * 8 + 8 <= pw && byy * 8 + 8 <= ph) {
                double *prow = plane + (size_t)(byy * 8) * pw + bxx * 8;
                for (int j = 0; j < 8; j++) {
                    double *dst = prow + (size_t)j * pw;
                    for (int i = 0; i < 8; i++)
                        dst[i] = blk[j * 8 + i] + 128.0;
                }
            } else {
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++) {
                        uint32_t gx = bxx * 8 + i, gy = byy * 8 + j;
                        if (gx < pw && gy < ph)
                            plane[(size_t)gy * pw + gx] = blk[j * 8 + i] + 128.0;
                    }
            }
        }
    return QTC_OK;
}

/* ── encode ──────────────────────────────────────────────────────────── */

int qtc_image_dct_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, float quality,
                         uint8_t **out, size_t *out_len)
{
    if (!pixels || w == 0 || h == 0 || (ch != 1 && ch != 3))
        return QTC_ERR_FORMAT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;

    int q100 = (int)(quality * 100.0f + 0.5f);
    if (q100 < 1) q100 = 1;
    if (q100 > 100) q100 = 100;
    int subsample = (ch == 3) ? 1 : 0;

    uint8_t Qy[64], Qc[64];
    build_quant(q100, Qy, Qc);
    double C[64];
    build_dct_matrix(C);

    uint32_t cw = subsample ? (w + 1) / 2 : w;
    uint32_t chh = subsample ? (h + 1) / 2 : h;

    size_t ynpix = (size_t)w * h;
    size_t cnpix = (size_t)cw * chh;

    double *Y = (double *)malloc(ynpix * sizeof(double));
    double *Cb = subsample ? (double *)malloc(cnpix * sizeof(double)) : NULL;
    double *Cr = subsample ? (double *)malloc(cnpix * sizeof(double)) : NULL;
    if (!Y || (subsample && (!Cb || !Cr))) {
        free(Y); free(Cb); free(Cr);
        return QTC_ERR_ALLOC;
    }

    /* RGB -> YCbCr (+ 4:2:0 box-average for chroma). */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * ch;
            double r, g, b;
            if (ch == 3) {
                r = pixels[i]; g = pixels[i + 1]; b = pixels[i + 2];
            } else {
                r = g = b = pixels[i];
            }
            Y[(size_t)y * w + x] = 0.299 * r + 0.587 * g + 0.114 * b;
        }
    if (subsample) {
        for (uint32_t cy = 0; cy < chh; cy++)
            for (uint32_t cx = 0; cx < cw; cx++) {
                double cb = 0, cr = 0;
                int n = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        uint32_t sx = cx * 2 + dx, sy = cy * 2 + dy;
                        if (sx >= w || sy >= h)
                            continue;
                        size_t i = ((size_t)sy * w + sx) * 3;
                        double r = pixels[i], g = pixels[i + 1],
                               b = pixels[i + 2];
                        cb += -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0;
                        cr +=  0.5 * r - 0.418688 * g - 0.081312 * b + 128.0;
                        n++;
                    }
                Cb[(size_t)cy * cw + cx] = cb / n;
                Cr[(size_t)cy * cw + cx] = cr / n;
            }
    }

    /* Block counts per plane. */
    uint32_t ybx = (w + 7) / 8, yby = (h + 7) / 8;
    uint32_t cbx = (cw + 7) / 8, cby = (chh + 7) / 8;
    size_t yblocks = (size_t)ybx * yby;
    size_t cblocks = subsample ? (size_t)cbx * cby : 0;
    size_t total_blocks = yblocks + 2 * cblocks;
    size_t sym_cap = total_blocks * DCT_SYM_PER_BLOCK;
    size_t amp_cap = total_blocks * DCT_AMP_BYTES_PER_BLOCK;

    double *blk = (double *)malloc(64 * sizeof(double));
    double *dct = (double *)malloc(64 * sizeof(double));
    uint16_t *sym = (uint16_t *)malloc(sym_cap * sizeof(uint16_t));
    uint8_t *amp = (uint8_t *)calloc(amp_cap ? amp_cap : 1, 1);
    if (!blk || !dct || !sym || !amp) {
        free(Y); free(Cb); free(Cr);
        free(blk); free(dct); free(sym); free(amp);
        return QTC_ERR_ALLOC;
    }

    amp_bw_t abw;
    abw.buf = amp;
    abw.nbits = 0;
    size_t nsym_y = 0, nsym_c = 0;

    /* Luma symbols first, then chroma — kept in separate ranges so each
     * gets its own rANS model (chroma is far sparser than luma). */
    encode_plane_syms(Y, w, h, ybx, yby, Qy, C, blk, dct, sym, &nsym_y, &abw);
    nsym_c = nsym_y;
    if (subsample) {
        encode_plane_syms(Cb, cw, chh, cbx, cby, Qc, C, blk, dct,
                          sym, &nsym_c, &abw);
        encode_plane_syms(Cr, cw, chh, cbx, cby, Qc, C, blk, dct,
                          sym, &nsym_c, &abw);
    }
    size_t nampbytes = (abw.nbits + 7) / 8;

    free(Y); free(Cb); free(Cr); free(blk); free(dct);

    /* Build separate per-image rANS models for luma and chroma symbols. */
    uint64_t counts_y[DCT_SYM_ALPHA] = {0};
    uint64_t counts_c[DCT_SYM_ALPHA] = {0};
    for (size_t i = 0; i < nsym_y; i++)
        counts_y[sym[i]]++;
    for (size_t i = nsym_y; i < nsym_c; i++)
        counts_c[sym[i]]++;
    uint16_t freq_y[DCT_SYM_ALPHA], start_y[DCT_SYM_ALPHA];
    uint16_t freq_c[DCT_SYM_ALPHA], start_c[DCT_SYM_ALPHA];
    qtc_model_build_gen(freq_y, counts_y, DCT_SYM_ALPHA);
    qtc_model_derive_starts_gen(start_y, freq_y, DCT_SYM_ALPHA);
    qtc_model_build_gen(freq_c, counts_c, DCT_SYM_ALPHA);
    qtc_model_derive_starts_gen(start_c, freq_c, DCT_SYM_ALPHA);

    uint8_t *bits_y = NULL, *bits_c = NULL;
    size_t bits_y_len = 0, bits_c_len = 0;
    int rc = qtc_ans_encode_gen(sym, nsym_y, DCT_SYM_ALPHA, freq_y, start_y,
                                &bits_y, &bits_y_len);
    if (rc == QTC_OK && nsym_c > nsym_y)
        rc = qtc_ans_encode_gen(sym + nsym_y, nsym_c - nsym_y, DCT_SYM_ALPHA,
                                freq_c, start_c, &bits_c, &bits_c_len);
    free(sym);
    if (rc != QTC_OK) {
        free(bits_y); free(bits_c); free(amp);
        return rc;
    }

    size_t total = DCT_HDR + 2 * DCT_MODEL_BYTES + bits_y_len + bits_c_len +
                   nampbytes;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(bits_y); free(bits_c); free(amp);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = (uint8_t)q100;
    buf[10] = (uint8_t)subsample;
    buf[11] = 0;
    put32(buf + 12, (uint32_t)nsym_y);
    put32(buf + 16, (uint32_t)(nsym_c - nsym_y));
    put32(buf + 20, (uint32_t)bits_y_len);
    put32(buf + 24, (uint32_t)nampbytes);
    qtc_model_pack_gen(freq_y, DCT_SYM_ALPHA, buf + DCT_HDR);
    qtc_model_pack_gen(freq_c, DCT_SYM_ALPHA, buf + DCT_HDR + DCT_MODEL_BYTES);
    size_t off = DCT_HDR + 2 * DCT_MODEL_BYTES;
    memcpy(buf + off, bits_y, bits_y_len);
    off += bits_y_len;
    if (bits_c_len) {
        memcpy(buf + off, bits_c, bits_c_len);
        off += bits_c_len;
    }
    memcpy(buf + off, amp, nampbytes);
    free(bits_y); free(bits_c); free(amp);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

/* ── decode ──────────────────────────────────────────────────────────── */

int qtc_image_dct_decode(const uint8_t *payload, size_t payload_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch)
{
    if (!payload || !out_pixels || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_pixels = NULL;
    /* Header is 28 bytes: w,h (8) + ch,q100,subsample,reserved (4) +
     * nsym_y,nsym_c,bits_y_len,nampbytes (16). */
    if (payload_len < DCT_HDR + 2 * DCT_MODEL_BYTES)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];
    int q100 = payload[9];
    int subsample = payload[10];
    uint32_t nsym_y = get32(payload + 12);
    uint32_t nsym_c = get32(payload + 16);
    uint32_t bits_y_len = get32(payload + 20);
    uint32_t nampbytes = get32(payload + 24);

    if (w == 0 || h == 0 || (ch != 1 && ch != 3))
        return QTC_ERR_CORRUPT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_CORRUPT;
    if (q100 < 1 || q100 > 100)
        return QTC_ERR_CORRUPT;
    if (subsample != ((ch == 3) ? 1 : 0))
        return QTC_ERR_CORRUPT;
    if (!subsample && nsym_c != 0)
        return QTC_ERR_CORRUPT;

    uint32_t cw = subsample ? (w + 1) / 2 : w;
    uint32_t chh = subsample ? (h + 1) / 2 : h;
    uint32_t ybx = (w + 7) / 8, yby = (h + 7) / 8;
    uint32_t cbx = (cw + 7) / 8, cby = (chh + 7) / 8;
    size_t yblocks = (size_t)ybx * yby;
    size_t cblocks = subsample ? (size_t)cbx * cby : 0;

    /* Sanity-cap the declared counts against the worst case for this
     * geometry (guards every downstream allocation). */
    uint64_t max_sym_y = (uint64_t)yblocks * DCT_SYM_PER_BLOCK;
    uint64_t max_sym_c = (uint64_t)2 * cblocks * DCT_SYM_PER_BLOCK;
    uint64_t max_amp =
        (uint64_t)(yblocks + 2 * cblocks) * DCT_AMP_BYTES_PER_BLOCK;
    if ((uint64_t)nsym_y > max_sym_y || (uint64_t)nsym_c > max_sym_c ||
        (uint64_t)nampbytes > max_amp)
        return QTC_ERR_CORRUPT;

    /* Unpack + validate both rANS models. */
    uint16_t freq_y[DCT_SYM_ALPHA], start_y[DCT_SYM_ALPHA];
    uint16_t freq_c[DCT_SYM_ALPHA], start_c[DCT_SYM_ALPHA];
    int rc = qtc_model_unpack_gen(freq_y, DCT_SYM_ALPHA, payload + DCT_HDR);
    if (rc != QTC_OK)
        return rc;
    rc = qtc_model_unpack_gen(freq_c, DCT_SYM_ALPHA,
                              payload + DCT_HDR + DCT_MODEL_BYTES);
    if (rc != QTC_OK)
        return rc;
    qtc_model_derive_starts_gen(start_y, freq_y, DCT_SYM_ALPHA);
    qtc_model_derive_starts_gen(start_c, freq_c, DCT_SYM_ALPHA);

    /* Section layout after the two models: [luma bits][chroma bits][amp].
     * bits_y_len is stored; the chroma stream fills the rest before amp. */
    size_t bits_off = DCT_HDR + 2 * DCT_MODEL_BYTES;
    if (payload_len < bits_off + (size_t)bits_y_len + nampbytes)
        return QTC_ERR_CORRUPT;
    size_t bits_c_len = payload_len - bits_off - (size_t)bits_y_len -
                        nampbytes;
    const uint8_t *bits_y = payload + bits_off;
    const uint8_t *bits_c = bits_y + bits_y_len;
    const uint8_t *amp = bits_c + bits_c_len;

    uint16_t *sym_y = NULL, *sym_c = NULL;
    if (nsym_y > 0) {
        sym_y = (uint16_t *)malloc(nsym_y * sizeof(uint16_t));
        if (!sym_y)
            return QTC_ERR_ALLOC;
        rc = qtc_ans_decode_gen(bits_y, bits_y_len, nsym_y, DCT_SYM_ALPHA,
                                freq_y, start_y, sym_y);
        if (rc != QTC_OK) {
            free(sym_y);
            return rc;
        }
    }
    if (nsym_c > 0) {
        sym_c = (uint16_t *)malloc(nsym_c * sizeof(uint16_t));
        if (!sym_c) {
            free(sym_y);
            return QTC_ERR_ALLOC;
        }
        rc = qtc_ans_decode_gen(bits_c, bits_c_len, nsym_c, DCT_SYM_ALPHA,
                                freq_c, start_c, sym_c);
        if (rc != QTC_OK) {
            free(sym_y); free(sym_c);
            return rc;
        }
    }

    uint8_t Qy[64], Qc[64];
    build_quant(q100, Qy, Qc);
    double C[64];
    build_dct_matrix(C);

    size_t ynpix = (size_t)w * h;
    size_t cnpix = (size_t)cw * chh;
    double *Y = (double *)malloc(ynpix * sizeof(double));
    double *Cb = subsample ? (double *)malloc(cnpix * sizeof(double)) : NULL;
    double *Cr = subsample ? (double *)malloc(cnpix * sizeof(double)) : NULL;
    double *blk = (double *)malloc(64 * sizeof(double));
    double *dct = (double *)malloc(64 * sizeof(double));
    if (!Y || (subsample && (!Cb || !Cr)) || !blk || !dct) {
        free(Y); free(Cb); free(Cr); free(blk); free(dct);
        free(sym_y); free(sym_c);
        return QTC_ERR_ALLOC;
    }

    amp_br_t br;
    br.buf = amp;
    br.nbits = 0;
    br.cap_bits = (size_t)nampbytes * 8;
    size_t si_y = 0, si_c = 0;

    rc = decode_plane_syms(sym_y, &si_y, nsym_y, &br, ybx, yby, Qy, C,
                           blk, dct, Y, w, h);
    if (rc == QTC_OK && subsample) {
        rc = decode_plane_syms(sym_c, &si_c, nsym_c, &br, cbx, cby, Qc, C,
                               blk, dct, Cb, cw, chh);
        if (rc == QTC_OK)
            rc = decode_plane_syms(sym_c, &si_c, nsym_c, &br, cbx, cby, Qc, C,
                                   blk, dct, Cr, cw, chh);
    }
    free(sym_y);
    free(sym_c);
    if (rc != QTC_OK) {
        free(Y); free(Cb); free(Cr); free(blk); free(dct);
        return rc;
    }
    if (si_y != nsym_y || si_c != nsym_c) {    /* leftover symbols */
        free(Y); free(Cb); free(Cr); free(blk); free(dct);
        return QTC_ERR_CORRUPT;
    }
    /* Amplitude bits: the writer pads the final byte with zeros, so allow
     * up to 7 unconsumed pad bits in the last byte. */
    if (br.nbits + 7 < br.cap_bits) {
        free(Y); free(Cb); free(Cr); free(blk); free(dct);
        return QTC_ERR_CORRUPT;
    }
    free(blk); free(dct);

    /* YCbCr -> RGB (chroma bilinear-upsampled to full resolution).
     * Chroma is upsampled one row at a time and consumed immediately: two
     * full-resolution double planes would be 132 MB of fresh allocation at
     * 4K, and that page-faulting dominated keyframe decode. */
    uint8_t *px = (uint8_t *)malloc(ynpix * ch);
    double *cbRow = NULL, *crRow = NULL;
    if (!px) {
        free(Y); free(Cb); free(Cr);
        return QTC_ERR_ALLOC;
    }
    if (subsample) {
        cbRow = (double *)malloc((size_t)w * sizeof(double));
        crRow = (double *)malloc((size_t)w * sizeof(double));
        if (!cbRow || !crRow) {
            free(Y); free(Cb); free(Cr); free(px); free(cbRow); free(crRow);
            return QTC_ERR_ALLOC;
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        if (subsample) {
            upsample_chroma_row(Cb, cw, chh, cbRow, w, y);
            upsample_chroma_row(Cr, cw, chh, crRow, w, y);
        }
        const double *Yrow = Y + (size_t)y * w;
        uint8_t *out = px + (size_t)y * w * ch;
        for (uint32_t x = 0; x < w; x++) {
            double yy = Yrow[x];
            double cb, cr;
            if (subsample) {
                cb = cbRow[x];
                cr = crRow[x];
            } else {
                cb = cr = 128.0;
            }
            double rr = yy + 1.402 * (cr - 128.0);
            double gg = yy - 0.344136 * (cb - 128.0)
                           - 0.714136 * (cr - 128.0);
            double bb = yy + 1.772 * (cb - 128.0);
            size_t o = (size_t)x * ch;
            if (ch == 3) {
                out[o]     = (uint8_t)(rr < 0 ? 0 :
                                      (rr > 255 ? 255 : (int)rr));
                out[o + 1] = (uint8_t)(gg < 0 ? 0 :
                                      (gg > 255 ? 255 : (int)gg));
                out[o + 2] = (uint8_t)(bb < 0 ? 0 :
                                      (bb > 255 ? 255 : (int)bb));
            } else {
                out[o] = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : (int)yy));
            }
        }
    }

    free(Y); free(Cb); free(Cr); free(cbRow); free(crRow);
    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}
