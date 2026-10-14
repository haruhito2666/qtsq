/*
 * Quantum Tensor Sequence — Accretion Engine (Fractal Compression)
 * Compression: Partitioned IFS (Iterated Function Systems)
 * Best for: Images, textures, video frames
 * Author: Haruhito
 *
 * Optimized: Adaptive block sizes, local search, early termination,
 *            feature pruning. Handles 48MP–200MP images in <1 second.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include "qtsq.h"

#ifdef HAVE_VULKAN_COMPUTE
#include "vulkan_compute.h"
#include <android/log.h>
static vk_compute_ctx_t *g_vk_ctx = NULL;
static int g_vk_init_attempted = 0;
#endif

/* ── Adaptive Block Size Selection ── */

typedef struct {
    int range_size;
    int domain_size;
    int search_radius;   /* pixels radius for local search */
    int search_step;     /* step between domain candidates */
    int num_symmetries;  /* 4 or 8 */
    double mse_threshold;/* early termination threshold */
} accel_params_t;

static accel_params_t select_params(uint32_t width, uint32_t height) {
    size_t pixels = (size_t)width * height;
    accel_params_t p;

    /*
     * Quality-tuned parameters.
     *
     * NEON SIMD + 8 cores gives ~5× per-thread speedup over the
     * original scalar code, so we can afford tighter search params
     * without net speed regression.
     *
     * File size is determined by block_size (not search params):
     *   transforms = (W/RSIZE) × (H/RSIZE) × 8 bytes × channels
     * Tighter search params improve quality without changing file size.
     */

    if (pixels <= 65536) {
        /* ≤256×256: tiny image — exhaustive quality */
        p.range_size = 4;
        p.domain_size = 8;
        p.search_radius = 9999;
        p.search_step = 1;
        p.num_symmetries = 8;
        p.mse_threshold = 1.0;
    } else if (pixels <= 262144) {
        /* ≤512×512: small image — high quality */
        p.range_size = 8;
        p.domain_size = 16;
        p.search_radius = 128;
        p.search_step = 4;
        p.num_symmetries = 8;
        p.mse_threshold = 2.0;
    } else if (pixels <= 2000000) {
        /* ≤2MP: medium image */
        p.range_size = 16;
        p.domain_size = 32;
        p.search_radius = 80;
        p.search_step = 8;
        p.num_symmetries = 8;
        p.mse_threshold = 3.0;
    } else if (pixels <= 16000000) {
        /* ≤16MP: phone cameras (4096×3072, etc.)
         * This is the critical tier — balance quality vs speed.
         * NEON+8-core headroom lets us use tighter params. */
        p.range_size = 16;
        p.domain_size = 32;
        p.search_radius = 64;
        p.search_step = 8;       /* was 16 → 4× more candidates */
        p.num_symmetries = 8;    /* was 4 → enables reflections */
        p.mse_threshold = 4.0;   /* was 8.0 → rejects poor matches */
    } else if (pixels <= 50000000) {
        /* ≤50MP: high-res cameras */
        p.range_size = 16;
        p.domain_size = 32;
        p.search_radius = 48;
        p.search_step = 8;
        p.num_symmetries = 8;
        p.mse_threshold = 5.0;
    } else {
        /* >50MP (108MP, 200MP) */
        p.range_size = 32;
        p.domain_size = 64;
        p.search_radius = 64;
        p.search_step = 16;
        p.num_symmetries = 8;
        p.mse_threshold = 6.0;
    }

    return p;
}

/* ══════════════════════════════════════════════════════════════
 *  V2 ENGINE: YCbCr + Adaptive Quadtree + Perceptual MSE
 * ══════════════════════════════════════════════════════════════ */

#define QTSQ_V2_FLAG_YCBCR    0x01
#define QTSQ_V2_FLAG_QUADTREE 0x02
#define QTSQ_V2_MAX_BLOCK     32
#define QTSQ_V2_MIN_BLOCK     8

/* ── YCbCr Color Space (ITU-R BT.601) ── */

static void rgb_to_ycbcr(const uint8_t *rgb, uint8_t *y_out,
                          uint8_t *cb_out, uint8_t *cr_out,
                          uint32_t width, uint32_t height) {
    size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) {
        int r = rgb[i * 3];
        int g = rgb[i * 3 + 1];
        int b = rgb[i * 3 + 2];
        /* BT.601: Y = 0.299R + 0.587G + 0.114B
         *         Cb= 128 - 0.169R - 0.331G + 0.500B
         *         Cr= 128 + 0.500R - 0.419G - 0.081B
         * Use fixed-point: multiply by 256 for integer math */
        int y  = (  77 * r + 150 * g +  29 * b + 128) >> 8;
        int cb = (- 43 * r -  85 * g + 128 * b + 128) >> 8;
        int cr = ( 128 * r - 107 * g -  21 * b + 128) >> 8;
        y_out[i]  = (uint8_t)(y  < 0 ? 0 : (y  > 255 ? 255 : y));
        cb_out[i] = (uint8_t)((cb + 128) < 0 ? 0 : ((cb + 128) > 255 ? 255 : (cb + 128)));
        cr_out[i] = (uint8_t)((cr + 128) < 0 ? 0 : ((cr + 128) > 255 ? 255 : (cr + 128)));
    }
}

static void ycbcr_to_rgb(const uint8_t *y_in, const uint8_t *cb_in,
                          const uint8_t *cr_in, uint8_t *rgb,
                          uint32_t width, uint32_t height) {
    size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) {
        int y  = y_in[i];
        int cb = (int)cb_in[i] - 128;
        int cr = (int)cr_in[i] - 128;
        /* Inverse BT.601 */
        int r = y + ((  359 * cr + 128) >> 8);
        int g = y + (( - 88 * cb - 183 * cr + 128) >> 8);
        int b = y + ((  454 * cb + 128) >> 8);
        rgb[i * 3]     = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
        rgb[i * 3 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
        rgb[i * 3 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
    }
}

/* ── 4:2:0 Chroma Subsampling ── */

static void downsample_channel_420(const uint8_t *src, uint32_t sw, uint32_t sh,
                                    uint8_t *dst, uint32_t dw, uint32_t dh) {
    /* Box filter 2×2 → 1 */
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t sy = y * 2, sx = x * 2;
            int sum = src[sy * sw + sx];
            if (sx + 1 < sw) sum += src[sy * sw + sx + 1];
            else sum += src[sy * sw + sx];
            if (sy + 1 < sh) {
                sum += src[(sy + 1) * sw + sx];
                if (sx + 1 < sw) sum += src[(sy + 1) * sw + sx + 1];
                else sum += src[(sy + 1) * sw + sx];
            } else {
                sum += src[sy * sw + sx];
                if (sx + 1 < sw) sum += src[sy * sw + sx + 1];
                else sum += src[sy * sw + sx];
            }
            dst[y * dw + x] = (uint8_t)((sum + 2) / 4);
        }
    }
}

static void upsample_channel_420(const uint8_t *src, uint32_t sw, uint32_t sh,
                                  uint8_t *dst, uint32_t dw, uint32_t dh) {
    /* Bilinear interpolation */
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            /* Map to source coordinates */
            float fx = (float)x * (float)sw / (float)dw;
            float fy = (float)y * (float)sh / (float)dh;
            uint32_t x0 = (uint32_t)fx;
            uint32_t y0 = (uint32_t)fy;
            uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t y1 = y0 + 1 < sh ? y0 + 1 : y0;
            float xf = fx - (float)x0;
            float yf = fy - (float)y0;

            float v = (1.0f - xf) * (1.0f - yf) * src[y0 * sw + x0]
                    + xf * (1.0f - yf) * src[y0 * sw + x1]
                    + (1.0f - xf) * yf * src[y1 * sw + x0]
                    + xf * yf * src[y1 * sw + x1];
            dst[y * dw + x] = (uint8_t)(v < 0.0f ? 0 : (v > 255.0f ? 255 : (int)(v + 0.5f)));
        }
    }
}



/* ── Bitstream for Quadtree Structure ── */

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t byte_pos;
    int bit_pos;  /* 7=MSB first, decrements */
    size_t total_bits; /* exact count of bits written */
} bitstream_t;

static void bs_init(bitstream_t *bs, size_t capacity) {
    bs->data = (uint8_t *)calloc(capacity, 1);
    bs->capacity = capacity;
    bs->byte_pos = 0;
    bs->bit_pos = 7;
    bs->total_bits = 0;
}

static void bs_write_bit(bitstream_t *bs, int bit) {
    if (bs->byte_pos >= bs->capacity) {
        size_t old_cap = bs->capacity;
        bs->capacity *= 2;
        bs->data = (uint8_t *)realloc(bs->data, bs->capacity);
        memset(bs->data + old_cap, 0, bs->capacity - old_cap);
    }
    if (bit) bs->data[bs->byte_pos] |= (1 << bs->bit_pos);
    bs->bit_pos--;
    bs->total_bits++;
    if (bs->bit_pos < 0) {
        bs->bit_pos = 7;
        bs->byte_pos++;
    }
}

static size_t bs_size_bytes(const bitstream_t *bs) {
    return (bs->total_bits + 7) / 8;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
} bitreader_t;

static void br_init(bitreader_t *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 7;
}

static int br_read_bit(bitreader_t *br) {
    if (br->byte_pos >= br->size) return 0;
    int bit = (br->data[br->byte_pos] >> br->bit_pos) & 1;
    br->bit_pos--;
    if (br->bit_pos < 0) {
        br->bit_pos = 7;
        br->byte_pos++;
    }
    return bit;
}

/* ── IFS Transform (stored per range block) ── */

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct {
    uint16_t domain_x;    /* Domain block origin X */
    uint16_t domain_y;    /* Domain block origin Y */
    uint8_t  symmetry;    /* Symmetry operation (0-7) */
    int8_t   contrast;    /* Contrast scaling (-128 to 127 → /128.0) */
    int16_t  brightness;  /* Brightness offset */
}
#if defined(_MSC_VER)
ifs_transform_t;
#pragma pack(pop)
#else
__attribute__((packed)) ifs_transform_t;
#endif

_Static_assert(sizeof(ifs_transform_t) == 8, "IFS transform must be 8 bytes");

/* ── Quadtree Transform Storage (dynamic array) ── */

typedef struct {
    ifs_transform_t *data;
    size_t count;
    size_t capacity;
} transform_list_t;

static void tl_init(transform_list_t *tl) {
    tl->capacity = 4096;
    tl->data = (ifs_transform_t *)malloc(tl->capacity * sizeof(ifs_transform_t));
    tl->count = 0;
}

static void tl_push(transform_list_t *tl, const ifs_transform_t *t) {
    if (tl->count >= tl->capacity) {
        tl->capacity *= 2;
        tl->data = (ifs_transform_t *)realloc(tl->data,
                    tl->capacity * sizeof(ifs_transform_t));
    }
    tl->data[tl->count++] = *t;
}


/* ── ARM NEON SIMD ── */
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define HAVE_NEON 1
#endif

/* ── Symmetry Operations (8 dihedral group transforms) ── */

static void apply_symmetry(const float *in, float *out,
                            int size, uint8_t sym) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int sx, sy;
            switch (sym) {
                case 0: sx = x;            sy = y;            break;
                case 1: sx = size - 1 - x; sy = y;            break;
                case 2: sx = x;            sy = size - 1 - y; break;
                case 3: sx = size - 1 - x; sy = size - 1 - y; break;
                case 4: sx = y;            sy = x;            break;
                case 5: sx = size - 1 - y; sy = x;            break;
                case 6: sx = y;            sy = size - 1 - x; break;
                case 7: sx = size - 1 - y; sy = size - 1 - x; break;
                default: sx = x; sy = y; break;
            }
            out[y * size + x] = in[sy * size + sx];
        }
    }
}

/* ── Downsample NxN → (N/2)x(N/2) by averaging 2x2 blocks ── */

static void downsample_2x(const float *in, int in_size,
                           float *out, int out_size) {
    (void)in_size;
    for (int y = 0; y < out_size; y++) {
        for (int x = 0; x < out_size; x++) {
            float sum = in[(2*y) * (2*out_size) + (2*x)]
                       + in[(2*y) * (2*out_size) + (2*x+1)]
                       + in[(2*y+1) * (2*out_size) + (2*x)]
                       + in[(2*y+1) * (2*out_size) + (2*x+1)];
            out[y * out_size + x] = sum * 0.25f;
        }
    }
}

/* ── Extract block from image ── */

static void extract_block(const uint8_t *image, uint32_t img_width,
                           uint32_t bx, uint32_t by, int block_size,
                           float *block) {
    for (int y = 0; y < block_size; y++) {
        for (int x = 0; x < block_size; x++) {
            uint32_t px = bx + (uint32_t)x;
            uint32_t py = by + (uint32_t)y;
            block[y * block_size + x] = (float)image[py * img_width + px];
        }
    }
}

/* Helper for summing a float32x4_t vector across all targets */
#ifdef HAVE_NEON
static inline float neon_hsum_f32(float32x4_t v) {
    float temp[4];
    vst1q_f32(temp, v);
    return temp[0] + temp[1] + temp[2] + temp[3];
}
#endif

/* ── Compute block mean ── */

static float block_mean(const float *block, int n) {
#ifdef HAVE_NEON
    float32x4_t vsum = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(block + i);
        vsum = vaddq_f32(vsum, v);
    }
    float sum = neon_hsum_f32(vsum);
    for (; i < n; i++) sum += block[i];
    return sum / (float)n;
#else
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += block[i];
    return sum / (float)n;
#endif
}

/* ── NEON-vectorized: compute affine + algebraic MSE in one call ──
 *
 * Given symmetry-transformed domain and range blocks, computes:
 *   1. Affine params: contrast (s) and brightness (o)
 *   2. MSE using algebraic formula (NO separate MSE pass needed):
 *      MSE = (sum_rr - 2s·sum_dr - 2o·sum_r + s²·sum_dd + 2so·sum_d + o²·n) / n
 *
 * This eliminates the entire domain_mapped buffer and block_mse call.
 */
static float compute_affine_and_mse(const float *domain, const float *range,
                                     int n, float sum_rr,
                                     float *out_contrast, float *out_brightness) {
    float sum_d = 0, sum_r = 0, sum_dd = 0, sum_dr = 0;

#ifdef HAVE_NEON
    float32x4_t vsum_d  = vdupq_n_f32(0.0f);
    float32x4_t vsum_r  = vdupq_n_f32(0.0f);
    float32x4_t vsum_dd = vdupq_n_f32(0.0f);
    float32x4_t vsum_dr = vdupq_n_f32(0.0f);

    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t vd = vld1q_f32(domain + i);
        float32x4_t vr = vld1q_f32(range + i);
        vsum_d  = vaddq_f32(vsum_d, vd);
        vsum_r  = vaddq_f32(vsum_r, vr);
        vsum_dd = vmlaq_f32(vsum_dd, vd, vd);   /* fused multiply-add */
        vsum_dr = vmlaq_f32(vsum_dr, vd, vr);
    }
    sum_d  = neon_hsum_f32(vsum_d);
    sum_r  = neon_hsum_f32(vsum_r);
    sum_dd = neon_hsum_f32(vsum_dd);
    sum_dr = neon_hsum_f32(vsum_dr);

    /* Handle remainder */
    for (; i < n; i++) {
        sum_d  += domain[i];
        sum_r  += range[i];
        sum_dd += domain[i] * domain[i];
        sum_dr += domain[i] * range[i];
    }
#else
    /* Non-NEON fallback */
    for (int i = 0; i < n; i++) {
        sum_d  += domain[i];
        sum_r  += range[i];
        sum_dd += domain[i] * domain[i];
        sum_dr += domain[i] * range[i];
    }
#endif

    float fn = (float)n;

    /* Compute affine parameters:
     * s = (n·sum_dr - sum_d·sum_r) / (n·sum_dd - sum_d²)
     * o = (sum_r - s·sum_d) / n
     */
    float denom = fn * sum_dd - sum_d * sum_d;
    float s, o;

    if (fabsf(denom) < 1e-6f) {
        s = 0.0f;
        o = sum_r / fn;
    } else {
        s = (fn * sum_dr - sum_d * sum_r) / denom;
        o = (sum_r - s * sum_d) / fn;
    }

    /* Clamp to ±0.99 to prevent int8_t overflow (1.0*128=128 wraps to -128!) */
    if (s > 0.99f) s = 0.99f;
    if (s < -0.99f) s = -0.99f;

    *out_contrast = s;
    *out_brightness = o;

    /* Compute final MSE using algebraic expansion */
    float mse = (sum_rr - 2.0f*s*sum_dr - 2.0f*o*sum_r
                 + s*s*sum_dd + 2.0f*s*o*sum_d + o*o*fn) / fn;
    return mse;
}

/* ── Precompute sum of squares for range block (NEON) ── */

static float block_sum_sq(const float *block, int n) {
#ifdef HAVE_NEON
    float32x4_t vsum = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(block + i);
        vsum = vmlaq_f32(vsum, v, v);
    }
    float sum = neon_hsum_f32(vsum);
    for (; i < n; i++) sum += block[i] * block[i];
    return sum;
#else
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += block[i] * block[i];
    return sum;
#endif
}

/* ══════════════════════════════════════════════════════════════
 *  Per-channel fractal compression — NEON optimized, float precision
 *  Compresses range blocks in rows [start_row, end_row).
 *  Uses full channel_data for domain search (read-only).
 *
 *  Key optimizations:
 *   - float (not double) → 4-wide NEON float32x4_t
 *   - Algebraic MSE → no domain_mapped buffer, no separate MSE pass
 *   - Precomputed sum_rr → reused across all domain positions
 *   - NEON fused multiply-add (vfmaq_f32) for inner loops
 * ══════════════════════════════════════════════════════════════ */

static int compress_single_channel(
    const uint8_t *channel_data,
    uint32_t width, uint32_t height,
    const accel_params_t *params,
    int RSIZE, int DSIZE,
    uint32_t start_row, uint32_t end_row,
    ifs_transform_t **out_transforms,
    uint32_t *out_num_blocks)
{
    uint32_t blocks_x = width / (uint32_t)RSIZE;
    uint32_t row_count = end_row - start_row;
    uint32_t num_blocks = blocks_x * row_count;

    if (num_blocks == 0) { *out_transforms = NULL; *out_num_blocks = 0; return QTSQ_OK; }

    ifs_transform_t *transforms = (ifs_transform_t *)calloc(num_blocks, sizeof(ifs_transform_t));
    if (!transforms) return QTSQ_ERR_ALLOC;

    float *range_block  = (float *)malloc((size_t)RSIZE * RSIZE * sizeof(float));
    float *domain_block = (float *)malloc((size_t)DSIZE * DSIZE * sizeof(float));
    float *domain_down  = (float *)malloc((size_t)RSIZE * RSIZE * sizeof(float));
    float *domain_sym   = (float *)malloc((size_t)RSIZE * RSIZE * sizeof(float));

    if (!range_block || !domain_block || !domain_down || !domain_sym) {
        free(transforms); free(range_block); free(domain_block);
        free(domain_down); free(domain_sym);
        return QTSQ_ERR_ALLOC;
    }

    int block_pixels = RSIZE * RSIZE;

    for (uint32_t by = start_row; by < end_row; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            uint32_t t_idx = (by - start_row) * blocks_x + bx;
            uint32_t range_x = bx * (uint32_t)RSIZE;
            uint32_t range_y = by * (uint32_t)RSIZE;

            extract_block(channel_data, width, range_x, range_y, RSIZE, range_block);
            float range_mean = block_mean(range_block, block_pixels);
            float sum_rr = block_sum_sq(range_block, block_pixels);

            float best_mse = 1e30f;
            ifs_transform_t best_t = {0};

            /* Domain search uses FULL image — not limited to our row range */
            int sr = params->search_radius;
            int dx_start = (int)range_x - sr;
            int dy_start = (int)range_y - sr;
            int dx_end   = (int)range_x + sr;
            int dy_end   = (int)range_y + sr;

            if (dx_start < 0) dx_start = 0;
            if (dy_start < 0) dy_start = 0;
            if (dx_end > (int)width  - DSIZE) dx_end = (int)width  - DSIZE;
            if (dy_end > (int)height - DSIZE) dy_end = (int)height - DSIZE;

            for (int dy = dy_start; dy <= dy_end; dy += params->search_step) {
                for (int dx = dx_start; dx <= dx_end; dx += params->search_step) {
                    extract_block(channel_data, width, (uint32_t)dx, (uint32_t)dy,
                                  DSIZE, domain_block);
                    downsample_2x(domain_block, DSIZE, domain_down, RSIZE);

                    float domain_mean = block_mean(domain_down, block_pixels);
                    if (fabsf(domain_mean - range_mean) > 100.0f) continue;

                    for (int sym = 0; sym < params->num_symmetries; sym++) {
                        apply_symmetry(domain_down, domain_sym, RSIZE, (uint8_t)sym);

                        float s, o;
                        float mse = compute_affine_and_mse(
                            domain_sym, range_block, block_pixels, sum_rr, &s, &o);

                        if (mse < best_mse) {
                            best_mse = mse;
                            best_t.domain_x = (uint16_t)dx;
                            best_t.domain_y = (uint16_t)dy;
                            best_t.symmetry = (uint8_t)sym;
                            best_t.contrast = (int8_t)(s * 128.0f);
                            best_t.brightness = (int16_t)o;
                            if (best_mse < params->mse_threshold) goto found;
                        }
                    }
                }
            }
            found:
            transforms[t_idx] = best_t;
        }
    }

    free(range_block); free(domain_block); free(domain_down);
    free(domain_sym);

    *out_transforms = transforms;
    *out_num_blocks = num_blocks;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  V2: Quadtree Recursive Block Compressor — OPTIMIZED
 *
 *  Key optimizations:
 *   - Pre-allocated work buffers (zero malloc in recursion)
 *   - min_block=8 to limit recursion depth
 *   - Uses existing extract_block/downsample_2x for speed
 *   - Edge strength computed directly from float range block
 * ══════════════════════════════════════════════════════════════ */

/* Pre-allocated work buffers for one thread */
typedef struct {
    float *range_buf;    /* max_block² floats */
    float *domain_buf;   /* (max_block*2)² floats */
    float *domain_down;  /* max_block² floats */
    float *domain_sym;   /* max_block² floats */
} qt_work_t;

static void qt_work_init(qt_work_t *w, int max_block) {
    int max_domain = max_block * 2;
    w->range_buf   = (float *)malloc(max_block * max_block * sizeof(float));
    w->domain_buf  = (float *)malloc(max_domain * max_domain * sizeof(float));
    w->domain_down = (float *)malloc(max_block * max_block * sizeof(float));
    w->domain_sym  = (float *)malloc(max_block * max_block * sizeof(float));
}

static void qt_work_free(qt_work_t *w) {
    free(w->range_buf); free(w->domain_buf);
    free(w->domain_down); free(w->domain_sym);
}

/* Fast edge strength from float block (no uint8_t conversion) */
static float edge_strength_float(const float *block, int size) {
    if (size < 3) return 0.0f;
    float total = 0.0f;
    int count = 0;
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            float gx = -block[(y-1)*size + (x-1)] + block[(y-1)*size + (x+1)]
                       - 2*block[y*size + (x-1)] + 2*block[y*size + (x+1)]
                       - block[(y+1)*size + (x-1)] + block[(y+1)*size + (x+1)];
            float gy = -block[(y-1)*size + (x-1)] - 2*block[(y-1)*size + x]
                       - block[(y-1)*size + (x+1)] + block[(y+1)*size + (x-1)]
                       + 2*block[(y+1)*size + x] + block[(y+1)*size + (x+1)];
            total += sqrtf(gx * gx + gy * gy);
            count++;
        }
    }
    return count > 0 ? total / (float)count : 0.0f;
}

static void qt_compress_block(
    const uint8_t *channel_data, uint32_t width, uint32_t height,
    uint32_t bx, uint32_t by,
    int block_size, int min_block,
    const accel_params_t *params,
    bitstream_t *tree, transform_list_t *tlist,
    qt_work_t *work)
{
    int dsize = block_size * 2;
    int block_pixels = block_size * block_size;

    /* Extract range block into pre-allocated buffer */
    float *range_block = work->range_buf;
    for (int y = 0; y < block_size; y++) {
        for (int x = 0; x < block_size; x++) {
            uint32_t px = bx + (uint32_t)x;
            uint32_t py = by + (uint32_t)y;
            if (px < width && py < height)
                range_block[y * block_size + x] = (float)channel_data[py * width + px];
            else
                range_block[y * block_size + x] = 128.0f;
        }
    }

    /* Edge-aware perceptual threshold */
    float edge = edge_strength_float(range_block, block_size);
    float base_thresh = params->mse_threshold;

    /*
     * V2 Speed Optimization:
     * The quadtree will SPLIT blocks that don't match well, so we can use
     * a fast coarse search at large block sizes. Only leaf-level (8×8)
     * blocks need full quality search. This is the key speedup.
     */
    float threshold;
    int sr, step, nsym;

    if (block_size >= 32) {
        /* 32×32: fast survey — quadtree will split if bad */
        threshold = base_thresh * 2.0f / (1.0f + edge * 0.02f);
        sr = params->search_radius / 2;      /* smaller radius */
        step = params->search_step * 4;      /* coarse grid */
        nsym = 4;                            /* skip reflections */
    } else if (block_size >= 16) {
        /* 16×16: medium search */
        threshold = base_thresh * 1.5f / (1.0f + edge * 0.03f);
        sr = params->search_radius;
        step = params->search_step * 2;
        nsym = 4;
    } else {
        /* 8×8 (leaf): focused local search — quadtree already placed us where detail matters */
        threshold = base_thresh * 1.2f / (1.0f + edge * 0.04f);
        sr = params->search_radius / 2;     /* local search is enough for small blocks */
        step = params->search_step;
        nsym = 4;                            /* rotations only, skip reflections */
    }

    float range_mean = block_mean(range_block, block_pixels);
    float sum_rr = block_sum_sq(range_block, block_pixels);

    float best_mse = 1e30f;
    ifs_transform_t best_t = {0};

    int dx_start = (int)bx - sr;
    int dy_start = (int)by - sr;
    int dx_end   = (int)bx + sr;
    int dy_end   = (int)by + sr;

    if (dx_start < 0) dx_start = 0;
    if (dy_start < 0) dy_start = 0;
    if (dx_end > (int)width  - dsize) dx_end = (int)width  - dsize;
    if (dy_end > (int)height - dsize) dy_end = (int)height - dsize;

    float *domain_block = work->domain_buf;
    float *domain_down = work->domain_down;
    float *domain_sym = work->domain_sym;

    if (dx_start <= dx_end && dy_start <= dy_end) {
        for (int dy = dy_start; dy <= dy_end; dy += step) {
            for (int dx = dx_start; dx <= dx_end; dx += step) {
                for (int y = 0; y < dsize; y++) {
                    for (int x = 0; x < dsize; x++) {
                        uint32_t px = (uint32_t)dx + (uint32_t)x;
                        uint32_t py = (uint32_t)dy + (uint32_t)y;
                        if (px < width && py < height)
                            domain_block[y * dsize + x] = (float)channel_data[py * width + px];
                        else
                            domain_block[y * dsize + x] = 128.0f;
                    }
                }

                downsample_2x(domain_block, dsize, domain_down, block_size);
                float domain_mean = block_mean(domain_down, block_pixels);
                if (fabsf(domain_mean - range_mean) > 100.0f) continue;

                for (int sym = 0; sym < nsym; sym++) {
                    apply_symmetry(domain_down, domain_sym, block_size, (uint8_t)sym);
                    float s, o;
                    float mse = compute_affine_and_mse(
                        domain_sym, range_block, block_pixels, sum_rr, &s, &o);

                    if (mse < best_mse) {
                        best_mse = mse;
                        best_t.domain_x = (uint16_t)dx;
                        best_t.domain_y = (uint16_t)dy;
                        best_t.symmetry = (uint8_t)sym;
                        best_t.contrast = (int8_t)(s * 128.0f);
                        best_t.brightness = (int16_t)o;
                        if (best_mse < threshold) goto qt_found;
                    }
                }
            }
        }
    }
    qt_found:

    /* Decision: split or keep as leaf? */
    if (best_mse > threshold && block_size > min_block) {
        bs_write_bit(tree, 1);
        int half = block_size / 2;
        qt_compress_block(channel_data, width, height,
            bx, by, half, min_block, params, tree, tlist, work);
        qt_compress_block(channel_data, width, height,
            bx + half, by, half, min_block, params, tree, tlist, work);
        qt_compress_block(channel_data, width, height,
            bx, by + half, half, min_block, params, tree, tlist, work);
        qt_compress_block(channel_data, width, height,
            bx + half, by + half, half, min_block, params, tree, tlist, work);
    } else {
        bs_write_bit(tree, 0);
        tl_push(tlist, &best_t);
    }
}

/* Thread argument for v2 channel compression */
typedef struct {
    const uint8_t *channel_data;
    uint32_t width, height;
    const accel_params_t *params;
    int max_block, min_block;
    uint32_t start_y, end_y;  /* pixel row range */
    bitstream_t tree;
    transform_list_t tlist;
} qt_thread_arg_t;

static void *qt_compress_thread(void *arg) {
    qt_thread_arg_t *a = (qt_thread_arg_t *)arg;
    qt_work_t work;
    qt_work_init(&work, a->max_block);

    for (uint32_t by = a->start_y; by + (uint32_t)a->max_block <= a->end_y; by += a->max_block) {
        for (uint32_t bx = 0; bx + (uint32_t)a->max_block <= a->width; bx += a->max_block) {
            qt_compress_block(a->channel_data, a->width, a->height,
                bx, by, a->max_block, a->min_block, a->params,
                &a->tree, &a->tlist, &work);
        }
    }

    qt_work_free(&work);
    return NULL;
}


/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_image
 * ══════════════════════════════════════════════════════════════
 *
 * 6-thread parallel compression:
 *   3 channels × 2 halves (top/bottom) = 6 threads on 6 cores.
 *   Each thread processes half the range blocks of one channel
 *   but has full channel data for domain search.
 *
 * Packed format in singularity:
 *   [4B width] [4B height] [1B channels] [1B block_size]
 *   [4B num_blocks_per_channel]
 *   For each channel:
 *     [num_blocks_per_channel * sizeof(ifs_transform_t)]
 */

/* Thread argument for half-channel compression */
typedef struct {
    const uint8_t *channel_data;    /* full single-channel buffer (read-only) */
    uint32_t width, height;
    const accel_params_t *params;
    int RSIZE, DSIZE;
    uint32_t start_row, end_row;    /* block row range [start, end) */
    ifs_transform_t *out_transforms;
    uint32_t out_num_blocks;
    int result;
} half_channel_arg_t;

static void *compress_half_thread(void *arg) {
    half_channel_arg_t *a = (half_channel_arg_t *)arg;
    a->result = compress_single_channel(
        a->channel_data, a->width, a->height, a->params,
        a->RSIZE, a->DSIZE, a->start_row, a->end_row,
        &a->out_transforms, &a->out_num_blocks);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  V3 Triangle Mesh Compression Wrapper
 * ═══════════════════════════════════════════════════════════════ */

int qtsq_compress_image_triangle(qtsq_context_t *ctx,
                                  const uint8_t *pixels,
                                  uint32_t width, uint32_t height) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width == 0 || height == 0) return QTSQ_ERR_TOO_SMALL;

    extern int qtsq_triangle_compress(const uint8_t *rgb, uint32_t width, uint32_t height,
                                       uint8_t **out_data, size_t *out_size);

    uint8_t *tri_data = NULL;
    size_t tri_size = 0;
    int res = qtsq_triangle_compress(pixels, width, height, &tri_data, &tri_size);
    if (res != 0 || !tri_data) return QTSQ_ERR_ALLOC;

    /* Store in context */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = tri_data;
    ctx->singularity_size = tri_size;

    /* Set header */
    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = (uint64_t)width * height * 3;
    ctx->header.num_channels = 3;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = tri_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;
    qtsq_sha256(pixels, (size_t)width * height * 3, ctx->header.checksum);

    /* Schema */
    memset(&ctx->schema, 0, sizeof(ctx->schema));
    strncpy(ctx->schema.name, "triangle_mesh", sizeof(ctx->schema.name) - 1);
    ctx->schema.data_type = QTSQ_TYPE_IMAGE_RGB;
    ctx->schema.num_dims = 2;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;

    return QTSQ_OK;
}

int qtsq_compress_image(qtsq_context_t *ctx,
                        const uint8_t *pixels,
                        uint32_t width, uint32_t height,
                        uint8_t channels) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width == 0 || height == 0) return QTSQ_ERR_TOO_SMALL;
    if (channels == 0) channels = 1;

    size_t pixel_count = (size_t)width * height;
    accel_params_t params = select_params(width, height);

    /* ═══════════════════════════════════════════
     *  V2 PATH: YCbCr + Quadtree (for RGB images)
     * ═══════════════════════════════════════════ */
    if (channels == 3) {
        int max_block = QTSQ_V2_MAX_BLOCK;  /* 32 */
        int min_block = QTSQ_V2_MIN_BLOCK;  /* 4 */

        /* Align dimensions to max_block for clean tiling */
        uint32_t aligned_w = (width / (uint32_t)max_block) * (uint32_t)max_block;
        uint32_t aligned_h = (height / (uint32_t)max_block) * (uint32_t)max_block;
        if (aligned_w == 0 || aligned_h == 0) {
            /* Image too small for v2 — fall through to v1 */
            goto v1_path;
        }

        /* 1) Convert RGB → YCbCr */
        uint8_t *y_buf  = (uint8_t *)malloc(pixel_count);
        uint8_t *cb_buf = (uint8_t *)malloc(pixel_count);
        uint8_t *cr_buf = (uint8_t *)malloc(pixel_count);
        if (!y_buf || !cb_buf || !cr_buf) {
            free(y_buf); free(cb_buf); free(cr_buf);
            return QTSQ_ERR_ALLOC;
        }
        rgb_to_ycbcr(pixels, y_buf, cb_buf, cr_buf, width, height);

        /* 2) Downsample Cb/Cr (4:2:0) */
        uint32_t chroma_w = width / 2;
        uint32_t chroma_h = height / 2;
        uint8_t *cb_sub = (uint8_t *)malloc(chroma_w * chroma_h);
        uint8_t *cr_sub = (uint8_t *)malloc(chroma_w * chroma_h);
        if (!cb_sub || !cr_sub) {
            free(y_buf); free(cb_buf); free(cr_buf);
            free(cb_sub); free(cr_sub);
            return QTSQ_ERR_ALLOC;
        }
        downsample_channel_420(cb_buf, width, height, cb_sub, chroma_w, chroma_h);
        downsample_channel_420(cr_buf, width, height, cr_sub, chroma_w, chroma_h);
        free(cb_buf); free(cr_buf);

        /* 3) Compress all channels in parallel — 8 threads total
         *    Y:  6 threads (full resolution, biggest workload)
         *    Cb: 1 thread  (1/4 resolution)
         *    Cr: 1 thread  (1/4 resolution)
         */

        /* Compute chroma aligned dimensions before launching threads */
        uint32_t chroma_aw = (chroma_w / (uint32_t)max_block) * (uint32_t)max_block;
        uint32_t chroma_ah = (chroma_h / (uint32_t)max_block) * (uint32_t)max_block;
        int chroma_max = max_block;
        if (chroma_aw < (uint32_t)max_block || chroma_ah < (uint32_t)max_block) {
            chroma_max = 16;
            chroma_aw = (chroma_w / (uint32_t)chroma_max) * (uint32_t)chroma_max;
            chroma_ah = (chroma_h / (uint32_t)chroma_max) * (uint32_t)chroma_max;
        }
        if (chroma_aw < (uint32_t)chroma_max || chroma_ah < (uint32_t)chroma_max) {
            chroma_max = 8;
            chroma_aw = (chroma_w / (uint32_t)chroma_max) * (uint32_t)chroma_max;
            chroma_ah = (chroma_h / (uint32_t)chroma_max) * (uint32_t)chroma_max;
        }

        accel_params_t chroma_params = params;
        chroma_params.mse_threshold *= 2.0f;  /* relaxed for chroma */

        /* Launch all 8 threads: Y[0..5], Cb[6], Cr[7] */
        qt_thread_arg_t all_args[8];
        pthread_t all_threads[8];
        uint32_t y_blocks_total = aligned_h / (uint32_t)max_block;
        int y_nthreads = 6;
        if (y_blocks_total < 6) y_nthreads = (int)y_blocks_total;
        if (y_nthreads < 1) y_nthreads = 1;

        /* Y threads (0..y_nthreads-1) */
        for (int t = 0; t < y_nthreads; t++) {
            uint32_t row_start = (y_blocks_total * t / y_nthreads) * (uint32_t)max_block;
            uint32_t row_end   = (y_blocks_total * (t + 1) / y_nthreads) * (uint32_t)max_block;
            all_args[t].channel_data = y_buf;
            all_args[t].width = aligned_w;
            all_args[t].height = aligned_h;
            all_args[t].params = &params;
            all_args[t].max_block = max_block;
            all_args[t].min_block = min_block;
            all_args[t].start_y = row_start;
            all_args[t].end_y = row_end;
            bs_init(&all_args[t].tree, 4096);
            tl_init(&all_args[t].tlist);
        }

        /* Cb thread (index y_nthreads) */
        int cb_idx = y_nthreads;
        all_args[cb_idx].channel_data = cb_sub;
        all_args[cb_idx].width = chroma_aw;
        all_args[cb_idx].height = chroma_ah;
        all_args[cb_idx].params = &chroma_params;
        all_args[cb_idx].max_block = chroma_max;
        all_args[cb_idx].min_block = min_block;
        all_args[cb_idx].start_y = 0;
        all_args[cb_idx].end_y = chroma_ah;
        bs_init(&all_args[cb_idx].tree, 2048);
        tl_init(&all_args[cb_idx].tlist);

        /* Cr thread (index y_nthreads+1) */
        int cr_idx = y_nthreads + 1;
        all_args[cr_idx].channel_data = cr_sub;
        all_args[cr_idx].width = chroma_aw;
        all_args[cr_idx].height = chroma_ah;
        all_args[cr_idx].params = &chroma_params;
        all_args[cr_idx].max_block = chroma_max;
        all_args[cr_idx].min_block = min_block;
        all_args[cr_idx].start_y = 0;
        all_args[cr_idx].end_y = chroma_ah;
        bs_init(&all_args[cr_idx].tree, 2048);
        tl_init(&all_args[cr_idx].tlist);

        int total_threads = cr_idx + 1;

        /* Launch: thread 0 runs on caller, others spawn */
        for (int t = 1; t < total_threads; t++)
            pthread_create(&all_threads[t], NULL, qt_compress_thread, &all_args[t]);
        qt_compress_thread(&all_args[0]);  /* caller does thread 0 */
        for (int t = 1; t < total_threads; t++)
            pthread_join(all_threads[t], NULL);

        /* Free channel buffers — safe now as all threads done */
        free(y_buf);
        free(cb_sub);
        free(cr_sub);

        /* Merge Y threads into one bitstream+transform list */
        bitstream_t y_tree; bs_init(&y_tree, 16384);
        transform_list_t y_tl; tl_init(&y_tl);
        for (int t = 0; t < y_nthreads; t++) {
            /* Copy exactly total_bits from this thread's bitstream */
            size_t nbits = all_args[t].tree.total_bits;
            for (size_t b = 0; b < nbits; b++) {
                size_t byte_idx = b / 8;
                int bit_offset = 7 - (int)(b % 8);
                int val = (all_args[t].tree.data[byte_idx] >> bit_offset) & 1;
                bs_write_bit(&y_tree, val);
            }
            for (size_t i = 0; i < all_args[t].tlist.count; i++)
                tl_push(&y_tl, &all_args[t].tlist.data[i]);
            free(all_args[t].tree.data);
            free(all_args[t].tlist.data);
        }

        /* Cb and Cr are single-thread — use directly */
        bitstream_t cb_tree = all_args[cb_idx].tree;
        transform_list_t cb_tl = all_args[cb_idx].tlist;
        bitstream_t cr_tree = all_args[cr_idx].tree;
        transform_list_t cr_tl = all_args[cr_idx].tlist;

        /* 4) Pack v2 format */
        size_t y_tree_bytes = bs_size_bytes(&y_tree);
        size_t cb_tree_bytes = bs_size_bytes(&cb_tree);
        size_t cr_tree_bytes = bs_size_bytes(&cr_tree);
        size_t y_tf_bytes  = y_tl.count  * sizeof(ifs_transform_t);
        size_t cb_tf_bytes = cb_tl.count * sizeof(ifs_transform_t);
        size_t cr_tf_bytes = cr_tl.count * sizeof(ifs_transform_t);

        /*
         * V2 Header:
         *  [4B width] [4B height] [1B channels=3] [1B flags]
         *  [1B max_block] [1B min_block]
         *  [4B aligned_w] [4B aligned_h]
         *  [4B chroma_aw] [4B chroma_ah]
         *
         * Per channel (Y, Cb, Cr):
         *  [4B quadtree_bytes] [4B num_transforms]
         *  [quadtree bitstream]
         *  [transforms]
         */
        size_t header_size = 4 + 4 + 1 + 1 + 1 + 1 + 4 + 4 + 4 + 4;  /* 28 bytes */
        size_t per_ch_header = 4 + 4;  /* 8 bytes each */
        size_t total = header_size
                     + per_ch_header + y_tree_bytes + y_tf_bytes
                     + per_ch_header + cb_tree_bytes + cb_tf_bytes
                     + per_ch_header + cr_tree_bytes + cr_tf_bytes;

        uint8_t *output = (uint8_t *)malloc(total);
        if (!output) {
            free(y_tree.data); free(y_tl.data);
            free(cb_tree.data); free(cb_tl.data);
            free(cr_tree.data); free(cr_tl.data);
            return QTSQ_ERR_ALLOC;
        }

        size_t pos = 0;
        memcpy(output + pos, &width, 4);  pos += 4;
        memcpy(output + pos, &height, 4); pos += 4;
        output[pos++] = 3;  /* channels */
        output[pos++] = QTSQ_V2_FLAG_YCBCR | QTSQ_V2_FLAG_QUADTREE;  /* flags */
        output[pos++] = (uint8_t)max_block;
        output[pos++] = (uint8_t)min_block;
        memcpy(output + pos, &aligned_w, 4);  pos += 4;
        memcpy(output + pos, &aligned_h, 4);  pos += 4;
        memcpy(output + pos, &chroma_aw, 4);  pos += 4;
        memcpy(output + pos, &chroma_ah, 4);  pos += 4;

        /* Y channel data */
        uint32_t y_tb = (uint32_t)y_tree_bytes;
        uint32_t y_tc = (uint32_t)y_tl.count;
        memcpy(output + pos, &y_tb, 4); pos += 4;
        memcpy(output + pos, &y_tc, 4); pos += 4;
        memcpy(output + pos, y_tree.data, y_tree_bytes); pos += y_tree_bytes;
        memcpy(output + pos, y_tl.data, y_tf_bytes); pos += y_tf_bytes;

        /* Cb channel data */
        uint32_t cb_tb = (uint32_t)cb_tree_bytes;
        uint32_t cb_tc = (uint32_t)cb_tl.count;
        memcpy(output + pos, &cb_tb, 4); pos += 4;
        memcpy(output + pos, &cb_tc, 4); pos += 4;
        memcpy(output + pos, cb_tree.data, cb_tree_bytes); pos += cb_tree_bytes;
        memcpy(output + pos, cb_tl.data, cb_tf_bytes); pos += cb_tf_bytes;

        /* Cr channel data */
        uint32_t cr_tb = (uint32_t)cr_tree_bytes;
        uint32_t cr_tc = (uint32_t)cr_tl.count;
        memcpy(output + pos, &cr_tb, 4); pos += 4;
        memcpy(output + pos, &cr_tc, 4); pos += 4;
        memcpy(output + pos, cr_tree.data, cr_tree_bytes); pos += cr_tree_bytes;
        memcpy(output + pos, cr_tl.data, cr_tf_bytes); pos += cr_tf_bytes;

        free(y_tree.data); free(y_tl.data);
        free(cb_tree.data); free(cb_tl.data);
        free(cr_tree.data); free(cr_tl.data);

        /* Store in context */
        if (ctx->singularity) free(ctx->singularity);
        ctx->singularity = output;
        ctx->singularity_size = pos;

        /* Set header fields */
        ctx->header.strategy = QTSQ_STRATEGY_FRACTAL;
        ctx->header.data_type = QTSQ_TYPE_IMAGE_RGB;
        ctx->header.original_size = pixel_count * channels;
        ctx->header.num_channels = channels;
        ctx->header.sample_depth = 8;
        ctx->header.singularity_size = ctx->singularity_size;
        ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;

        memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
        strncpy(ctx->schema.name, "image", 63);
        ctx->schema.data_type = ctx->header.data_type;
        ctx->schema.dimensions[0] = width;
        ctx->schema.dimensions[1] = height;
        ctx->schema.dimensions[2] = 0;  /* Splat count for UI (0 for Accretion) */
        ctx->schema.num_dims = 3;
        strncpy(ctx->schema.encoding, "RAW_UINT8", 63);
        ctx->header.horizon_size = sizeof(qtsq_schema_t);

        qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

        return QTSQ_OK;
    }

    /* ═══════════════════════════════════════════
     *  V1 PATH: Original fixed-block (for grayscale, RGBA)
     * ═══════════════════════════════════════════ */
    v1_path:;

    int RSIZE = params.range_size;
    int DSIZE = params.domain_size;

    /* Validate block size */
    uint32_t blocks_x = width / (uint32_t)RSIZE;
    uint32_t blocks_y = height / (uint32_t)RSIZE;
    if (blocks_x == 0 || blocks_y == 0) {
        RSIZE = 4; DSIZE = 8;
        blocks_x = width / (uint32_t)RSIZE;
        blocks_y = height / (uint32_t)RSIZE;
        if (blocks_x == 0 || blocks_y == 0) return QTSQ_ERR_TOO_SMALL;
    }

    uint32_t num_blocks = blocks_x * blocks_y;  /* per channel total */

    /* ── Extract each channel into a separate buffer ── */
    uint8_t *ch_bufs[4] = {NULL};
    for (uint8_t c = 0; c < channels; c++) {
        ch_bufs[c] = (uint8_t *)malloc(pixel_count);
        if (!ch_bufs[c]) {
            for (uint8_t j = 0; j < c; j++) free(ch_bufs[j]);
            return QTSQ_ERR_ALLOC;
        }
        if (channels == 1) {
            memcpy(ch_bufs[c], pixels, pixel_count);
        } else {
            for (size_t i = 0; i < pixel_count; i++)
                ch_bufs[c][i] = pixels[i * channels + c];
        }
    }

    /* GPU is used for decompression only. Compression uses NEON SIMD CPU. */

    /* ── 8-thread parallel compression (all CPU cores) ──
     *
     * For RGB (3 channels): R=3 slices, G=3 slices, B=2 slices = 8 threads
     * For grayscale: 8 slices of the single channel
     * For RGBA: 2 slices per channel = 8 threads
     *
     * Each slice covers a range of block rows, so all slices have
     * full read access to the channel data for domain search.
     */
    uint32_t slices_per_ch[4] = {0};
    int num_threads = 0;

    if (channels == 1) {
        /* Grayscale: 8 slices */
        slices_per_ch[0] = 8;
        num_threads = 8;
    } else if (channels == 3) {
        /* RGB: 3+3+2 = 8 */
        slices_per_ch[0] = 3;  /* R */
        slices_per_ch[1] = 3;  /* G */
        slices_per_ch[2] = 2;  /* B */
        num_threads = 8;
    } else if (channels == 4) {
        /* RGBA: 2+2+2+2 = 8 */
        slices_per_ch[0] = 2;
        slices_per_ch[1] = 2;
        slices_per_ch[2] = 2;
        slices_per_ch[3] = 2;
        num_threads = 8;
    } else {
        for (uint8_t c = 0; c < channels; c++) slices_per_ch[c] = 2;
        num_threads = channels * 2;
    }

    half_channel_arg_t args[16];
    pthread_t threads[16];
    int thread_idx = 0;

    for (uint8_t c = 0; c < channels; c++) {
        uint32_t nslices = slices_per_ch[c];
        for (uint32_t s = 0; s < nslices; s++) {
            uint32_t row_start = (blocks_y * s) / nslices;
            uint32_t row_end   = (blocks_y * (s + 1)) / nslices;

            args[thread_idx].channel_data = ch_bufs[c];
            args[thread_idx].width = width;
            args[thread_idx].height = height;
            args[thread_idx].params = &params;
            args[thread_idx].RSIZE = RSIZE;
            args[thread_idx].DSIZE = DSIZE;
            args[thread_idx].start_row = row_start;
            args[thread_idx].end_row = row_end;
            args[thread_idx].out_transforms = NULL;
            args[thread_idx].out_num_blocks = 0;
            args[thread_idx].result = QTSQ_OK;
            thread_idx++;
        }
    }

    /* Thread 0 runs on current thread, rest get their own thread */
    for (int t = 1; t < num_threads; t++) {
        pthread_create(&threads[t], NULL, compress_half_thread, &args[t]);
    }
    compress_half_thread(&args[0]);  /* current thread */

    for (int t = 1; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    /* Free channel buffers */
    for (uint8_t c = 0; c < channels; c++) free(ch_bufs[c]);

    /* Check results */
    int result = QTSQ_OK;
    for (int t = 0; t < num_threads; t++) {
        if (args[t].result != QTSQ_OK) { result = args[t].result; break; }
    }

    if (result != QTSQ_OK) {
        for (int t = 0; t < num_threads; t++) free(args[t].out_transforms);
        return result;
    }

    /* ── Merge all slices per channel into contiguous arrays ── */
    ifs_transform_t *merged[4] = {NULL};
    thread_idx = 0;
    for (uint8_t c = 0; c < channels; c++) {
        uint32_t nslices = slices_per_ch[c];

        /* Count total blocks for this channel */
        uint32_t total = 0;
        for (uint32_t s = 0; s < nslices; s++)
            total += args[thread_idx + s].out_num_blocks;

        merged[c] = (ifs_transform_t *)malloc(total * sizeof(ifs_transform_t));
        if (!merged[c]) {
            for (uint8_t j = 0; j < c; j++) free(merged[j]);
            for (int t = 0; t < num_threads; t++) free(args[t].out_transforms);
            return QTSQ_ERR_ALLOC;
        }

        /* Copy all slices contiguously */
        uint32_t offset = 0;
        for (uint32_t s = 0; s < nslices; s++) {
            uint32_t count = args[thread_idx + s].out_num_blocks;
            if (args[thread_idx + s].out_transforms && count > 0)
                memcpy(merged[c] + offset, args[thread_idx + s].out_transforms,
                       count * sizeof(ifs_transform_t));
            free(args[thread_idx + s].out_transforms);
            offset += count;
        }
        thread_idx += nslices;
    }

    /* Pack output: header + transforms for all channels */
    size_t header_size = 4 + 4 + 1 + 1 + 4; /* 14 bytes */
    size_t per_ch_size = (size_t)num_blocks * sizeof(ifs_transform_t);
    size_t total_output = header_size + per_ch_size * channels;

    uint8_t *output = (uint8_t *)malloc(total_output);
    if (!output) {
        for (uint8_t c = 0; c < channels; c++) free(merged[c]);
        return QTSQ_ERR_ALLOC;
    }

    size_t pos = 0;
    memcpy(output + pos, &width, 4);  pos += 4;
    memcpy(output + pos, &height, 4); pos += 4;
    output[pos++] = channels;
    output[pos++] = (uint8_t)RSIZE;
    memcpy(output + pos, &num_blocks, 4); pos += 4;

    for (uint8_t c = 0; c < channels; c++) {
        memcpy(output + pos, merged[c], per_ch_size);
        pos += per_ch_size;
        free(merged[c]);
    }

    /* Store in context */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = output;
    ctx->singularity_size = pos;

    /* Set header fields */
    ctx->header.strategy = QTSQ_STRATEGY_FRACTAL;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 3) ? QTSQ_TYPE_IMAGE_RGB :
                            QTSQ_TYPE_IMAGE_RGBA;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;

    /* Store schema */
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "image", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = 0;  /* Splat count for UI (0 for Accretion) */
    ctx->schema.num_dims = 3;
    strncpy(ctx->schema.encoding, "RAW_UINT8", 63);

    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_accretion
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_accretion(const qtsq_context_t *ctx,
                              uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 14) return QTSQ_ERR_FORMAT;

    /* ═══════ V3 Triangle Mesh Decompress ═══════ */
    if (ctx->header.strategy == QTSQ_STRATEGY_TRIANGLE) {
        /* Forward-declare triangle decompress (from qtsq_triangle.c) */
        extern int qtsq_triangle_decompress(const uint8_t *data, size_t size,
                                             uint8_t **out_rgb, uint32_t *out_w, uint32_t *out_h);

        uint32_t tw, th;
        uint8_t *tri_rgb = NULL;
        int tres = qtsq_triangle_decompress(ctx->singularity, ctx->singularity_size,
                                             &tri_rgb, &tw, &th);
        if (tres != 0 || !tri_rgb) return QTSQ_ERR_FORMAT;

        *out_data = tri_rgb;
        *out_size = (size_t)tw * th * 3;
        return QTSQ_OK;
    }

    const uint8_t *buf = ctx->singularity;
    size_t pos = 0;

    /* Read common header */
    uint32_t width, height;
    uint8_t channels;

    memcpy(&width, buf + pos, 4); pos += 4;
    memcpy(&height, buf + pos, 4); pos += 4;
    channels = buf[pos]; /* peek at byte 8: channels */
    uint8_t flags = buf[pos + 1]; /* peek at byte 9: flags or block_size */

    /* SEC: width/height come from the file. Cap total pixel count to
     * prevent a crafted file from forcing a huge malloc (DoS guard). */
    if ((uint64_t)width * height > (1ULL << 28)) return QTSQ_ERR_FORMAT;
    if (width == 0 || height == 0) return QTSQ_ERR_FORMAT;

    /* Detect v2 format: if flags has QUADTREE bit set */
    if (channels == 3 && (flags & QTSQ_V2_FLAG_QUADTREE)) {
        /* ═══════════════ V2 DECOMPRESS ═══════════════ */
        pos++;  /* skip channels byte */
        pos++;  /* skip flags byte */
        uint8_t max_block = buf[pos++];
        uint8_t min_block = buf[pos++];

        /* Validate block sizes — a crafted file with max_block==0 makes the
         * (by += mb) loop never advance (infinite loop / DoS); values that
         * aren't powers of two in [MIN, MAX] corrupt the quadtree split math. */
        if (max_block < QTSQ_V2_MIN_BLOCK || max_block > QTSQ_V2_MAX_BLOCK ||
            (max_block & (max_block - 1)) != 0 ||
            min_block < QTSQ_V2_MIN_BLOCK || min_block > max_block ||
            (min_block & (min_block - 1)) != 0) {
            return QTSQ_ERR_FORMAT;
        }

        uint32_t aligned_w, aligned_h, chroma_aw, chroma_ah;
        memcpy(&aligned_w, buf + pos, 4); pos += 4;
        memcpy(&aligned_h, buf + pos, 4); pos += 4;
        memcpy(&chroma_aw, buf + pos, 4); pos += 4;
        memcpy(&chroma_ah, buf + pos, 4); pos += 4;

        size_t pixel_count = (size_t)width * height;

        /* Allocate output RGB */
        size_t output_size = pixel_count * 3;
        uint8_t *output = (uint8_t *)malloc(output_size);
        if (!output) return QTSQ_ERR_ALLOC;

        /* Helper: decompress one channel using quadtree */
        /* Read channel: quadtree_bytes, num_transforms, bitstream, transforms */
        uint8_t *ch_buffers[3] = {NULL};
        uint32_t ch_widths[3]  = {aligned_w, chroma_aw, chroma_aw};
        uint32_t ch_heights[3] = {aligned_h, chroma_ah, chroma_ah};

        for (int c = 0; c < 3; c++) {
            uint32_t qt_bytes, num_tf;
            memcpy(&qt_bytes, buf + pos, 4); pos += 4;
            memcpy(&num_tf, buf + pos, 4); pos += 4;

            /* Bounds-check file-controlled sizes before advancing pos past
             * qt_bytes and the transform array. Without this a crafted file
             * makes qt_data / transforms point past the singularity buffer
             * and the quadtree/transform reads dereference OOB. */
            if ((uint64_t)qt_bytes > ctx->singularity_size - pos) {
                free(output);
                return QTSQ_ERR_FORMAT;
            }
            const uint8_t *qt_data = buf + pos; pos += qt_bytes;
            if ((uint64_t)num_tf * sizeof(ifs_transform_t) > ctx->singularity_size - pos) {
                free(output);
                return QTSQ_ERR_FORMAT;
            }
            const ifs_transform_t *transforms = (const ifs_transform_t *)(buf + pos);
            pos += num_tf * sizeof(ifs_transform_t);

            uint32_t cw = ch_widths[c];
            uint32_t ch = ch_heights[c];
            size_t cpx = (size_t)cw * ch;

            float *current = (float *)calloc(cpx, sizeof(float));
            float *next    = (float *)calloc(cpx, sizeof(float));
            /* Pre-allocate work buffers at max size (max_block*2 for domain) */
            int max_dsize = (int)max_block * 2;
            float *dblk = (float *)malloc(max_dsize * max_dsize * sizeof(float));
            float *ddn  = (float *)malloc((int)max_block * (int)max_block * sizeof(float));
            float *dsym = (float *)malloc((int)max_block * (int)max_block * sizeof(float));
            if (!current || !next || !dblk || !ddn || !dsym) {
                free(current); free(next); free(dblk); free(ddn); free(dsym);
                free(output);
                for (int j = 0; j < c; j++) free(ch_buffers[j]);
                return QTSQ_ERR_ALLOC;
            }

            /* Initialize with mid-gray */
            for (size_t i = 0; i < cpx; i++) current[i] = 128.0f;

            int num_iterations = 6;
            int mb = (int)max_block;
            int mnb = (int)min_block;

            for (int iter = 0; iter < num_iterations; iter++) {
                memcpy(next, current, cpx * sizeof(float));

                /* Walk the quadtree and apply transforms */
                bitreader_t br;
                br_init(&br, qt_data, qt_bytes);
                uint32_t tf_idx = 0;

                /* Process in max_block tiles */
                for (uint32_t by = 0; by + (uint32_t)mb <= ch; by += mb) {
                    for (uint32_t bx = 0; bx + (uint32_t)mb <= cw; bx += mb) {
                        /* Recursive quadtree decompress */
                        /* Use a stack-based approach to avoid deep recursion */
                        typedef struct { uint32_t x, y; int size; } qt_stack_t;
                        qt_stack_t stack[256];
                        int sp = 0;
                        stack[sp++] = (qt_stack_t){bx, by, mb};

                        while (sp > 0) {
                            qt_stack_t node = stack[--sp];
                            int bit = br_read_bit(&br);

                            if (bit == 1 && node.size > mnb) {
                                /* Split: push 4 children (reverse order for correct traversal) */
                                int half = node.size / 2;
                                stack[sp++] = (qt_stack_t){node.x + half, node.y + half, half};
                                stack[sp++] = (qt_stack_t){node.x,        node.y + half, half};
                                stack[sp++] = (qt_stack_t){node.x + half, node.y,        half};
                                stack[sp++] = (qt_stack_t){node.x,        node.y,        half};
                            } else {
                                /* Leaf: apply transform */
                                if (tf_idx >= num_tf) continue;
                                const ifs_transform_t *t = &transforms[tf_idx++];
                                int rsize = node.size;
                                int dsize = rsize * 2;

                                float s = (float)t->contrast / 128.0f;
                                float o = (float)t->brightness;
                                uint32_t dx = t->domain_x;
                                uint32_t dy = t->domain_y;

                                /* Extract domain block (reuse pre-allocated buffers) */
                                for (int y = 0; y < dsize; y++)
                                    for (int x = 0; x < dsize; x++) {
                                        uint32_t px = dx + (uint32_t)x;
                                        uint32_t py = dy + (uint32_t)y;
                                        if (px < cw && py < ch)
                                            dblk[y * dsize + x] = current[py * cw + px];
                                        else
                                            dblk[y * dsize + x] = 128.0f;
                                    }

                                downsample_2x(dblk, dsize, ddn, rsize);
                                apply_symmetry(ddn, dsym, rsize, t->symmetry);

                                for (int y = 0; y < rsize; y++) {
                                    for (int x = 0; x < rsize; x++) {
                                        uint32_t px = node.x + (uint32_t)x;
                                        uint32_t py = node.y + (uint32_t)y;
                                        if (px < cw && py < ch) {
                                            float val = s * dsym[y * rsize + x] + o;
                                            if (val < 0.0f) val = 0.0f;
                                            if (val > 255.0f) val = 255.0f;
                                            next[py * cw + px] = val;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                float *tmp = current;
                current = next;
                next = tmp;
            }

            /* Convert to uint8 */
            ch_buffers[c] = (uint8_t *)malloc(cpx);
            if (!ch_buffers[c]) {
                free(current); free(next); free(output);
                for (int j = 0; j < c; j++) free(ch_buffers[j]);
                return QTSQ_ERR_ALLOC;
            }
            for (size_t i = 0; i < cpx; i++) {
                float v = current[i];
                ch_buffers[c][i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : (int)(v + 0.5f)));
            }
            free(current); free(next);
            free(dblk); free(ddn); free(dsym);
        }

        /* Upsample Cb/Cr from chroma to full resolution */
        uint8_t *y_full = ch_buffers[0];
        uint8_t *cb_full = (uint8_t *)calloc(pixel_count, 1);
        uint8_t *cr_full = (uint8_t *)calloc(pixel_count, 1);
        if (!cb_full || !cr_full) {
            free(y_full); free(ch_buffers[1]); free(ch_buffers[2]);
            free(cb_full); free(cr_full); free(output);
            return QTSQ_ERR_ALLOC;
        }

        upsample_channel_420(ch_buffers[1], chroma_aw, chroma_ah,
                             cb_full, width, height);
        upsample_channel_420(ch_buffers[2], chroma_aw, chroma_ah,
                             cr_full, width, height);
        free(ch_buffers[1]); free(ch_buffers[2]);

        /* Handle Y channel: copy to full-size if needed */
        uint8_t *y_out;
        if (aligned_w == width && aligned_h == height) {
            y_out = y_full;
        } else {
            y_out = (uint8_t *)calloc(pixel_count, 1);
            if (!y_out) {
                free(y_full); free(cb_full); free(cr_full); free(output);
                return QTSQ_ERR_ALLOC;
            }
            /* Copy the decoded aligned rows, then edge-replicate to fill
             * rows/columns beyond the aligned region. Previously the tail
             * rows/columns stayed black — data loss for any dimension not
             * a multiple of the block size. */
            uint32_t copy_w = aligned_w < width ? aligned_w : width;
            for (uint32_t row = 0; row < aligned_h && row < height; row++) {
                memcpy(y_out + row * width, y_full + row * aligned_w, copy_w);
                /* Right-edge columns: replicate the last valid column */
                for (uint32_t col = aligned_w; col < width; col++)
                    y_out[row * width + col] = y_full[row * aligned_w + (copy_w - 1)];
            }
            for (uint32_t row = aligned_h; row < height; row++) {
                uint32_t src_row = (aligned_h > 0) ? aligned_h - 1 : 0;
                memcpy(y_out + row * width, y_full + src_row * aligned_w, copy_w);
                for (uint32_t col = aligned_w; col < width; col++)
                    y_out[row * width + col] = y_full[src_row * aligned_w + (copy_w - 1)];
            }
            free(y_full);
        }

        /* Convert YCbCr → RGB */
        ycbcr_to_rgb(y_out, cb_full, cr_full, output, width, height);
        free(y_out); free(cb_full); free(cr_full);

        *out_data = output;
        *out_size = output_size;
        return QTSQ_OK;
    }

    /* ═══════════════ V1 DECOMPRESS ═══════════════ */
    channels = buf[pos++];
    uint8_t block_size = buf[pos++];
    uint32_t num_blocks;
    memcpy(&num_blocks, buf + pos, 4); pos += 4;

    if (block_size == 0) block_size = 8;
    if (channels == 0) channels = 1;

    int RSIZE = (int)block_size;
    int DSIZE = RSIZE * 2;

    uint32_t blocks_x = width / (uint32_t)RSIZE;
    uint32_t blocks_y = height / (uint32_t)RSIZE;

    size_t pixel_count = (size_t)width * height;
    size_t transforms_per_ch = (size_t)num_blocks * sizeof(ifs_transform_t);

    /* Validate data size: header + channels * transforms */
    if (pos + transforms_per_ch * channels > ctx->singularity_size)
        return QTSQ_ERR_FORMAT;

    /* Allocate interleaved output: width * height * channels */
    size_t output_size = pixel_count * channels;
    uint8_t *output = (uint8_t *)malloc(output_size);
    if (!output) return QTSQ_ERR_ALLOC;

    /* Decompress each channel independently */
    int num_iterations = 8;  /* more iterations = better IFS convergence */

    /* ── Try GPU-accelerated decompression ── */
#ifdef HAVE_VULKAN_COMPUTE
    if (!g_vk_init_attempted) {
        g_vk_init_attempted = 1;
        g_vk_ctx = vk_compute_init();
    }

    if (g_vk_ctx) {
        /* GPU path: use float buffers */
        float *cur_f = (float *)malloc(pixel_count * sizeof(float));
        float *nxt_f = (float *)malloc(pixel_count * sizeof(float));

        if (cur_f && nxt_f) {
            int gpu_ok = 1;

            vk_decompress_params_t dp;
            dp.width = width;
            dp.height = height;
            dp.range_size = (uint32_t)RSIZE;
            dp.domain_size = (uint32_t)DSIZE;
            dp.blocks_x = blocks_x;
            dp.blocks_y = blocks_y;

            for (uint8_t c = 0; c < channels && gpu_ok; c++) {
                const void *ch_transforms = buf + pos + transforms_per_ch * c;

                /* Initialize with mid-gray */
                for (size_t i = 0; i < pixel_count; i++)
                    cur_f[i] = 128.0f;

                /* IFS iterations on GPU */
                for (int iter = 0; iter < num_iterations; iter++) {
                    if (vk_compute_decompress_iteration(
                            g_vk_ctx, cur_f, ch_transforms, nxt_f,
                            num_blocks, &dp) != 0) {
                        gpu_ok = 0;
                        break;
                    }
                    /* Swap */
                    float *tmp = cur_f;
                    cur_f = nxt_f;
                    nxt_f = tmp;
                }

                if (gpu_ok) {
                    /* Write channel to interleaved output */
                    for (size_t i = 0; i < pixel_count; i++) {
                        float v = cur_f[i];
                        if (v < 0.0f) v = 0.0f;
                        if (v > 255.0f) v = 255.0f;
                        output[i * channels + c] = (uint8_t)v;
                    }
                }
            }

            free(cur_f); free(nxt_f);

            if (gpu_ok) {
                *out_data = output;
                *out_size = output_size;
                return QTSQ_OK;
            }
        } else {
            free(cur_f); free(nxt_f);
        }
        /* GPU failed — fall through to CPU */
    }
#endif

    /* ── CPU fallback decompression ── */
    {
        float *current = (float *)calloc(pixel_count, sizeof(float));
        float *next    = (float *)calloc(pixel_count, sizeof(float));
        float *domain_block = (float *)malloc((size_t)DSIZE * DSIZE * sizeof(float));
        float *domain_down  = (float *)malloc((size_t)RSIZE * RSIZE * sizeof(float));
        float *domain_sym   = (float *)malloc((size_t)RSIZE * RSIZE * sizeof(float));

        if (!current || !next || !domain_block || !domain_down || !domain_sym) {
            free(current); free(next);
            free(domain_block); free(domain_down); free(domain_sym);
            free(output);
            return QTSQ_ERR_ALLOC;
        }

        for (uint8_t c = 0; c < channels; c++) {
            const ifs_transform_t *transforms =
                (const ifs_transform_t *)(buf + pos + transforms_per_ch * c);

            for (size_t i = 0; i < pixel_count; i++)
                current[i] = 128.0f;

            for (int iter = 0; iter < num_iterations; iter++) {
                memcpy(next, current, pixel_count * sizeof(float));

                for (uint32_t by = 0; by < blocks_y; by++) {
                    for (uint32_t bx = 0; bx < blocks_x; bx++) {
                        uint32_t t_idx = by * blocks_x + bx;
                        const ifs_transform_t *t = &transforms[t_idx];

                        uint32_t dx = t->domain_x;
                        uint32_t dy = t->domain_y;

                        if (dx + (uint32_t)DSIZE > width || dy + (uint32_t)DSIZE > height)
                            continue;

                        for (int y = 0; y < DSIZE; y++)
                            for (int x = 0; x < DSIZE; x++)
                                domain_block[y * DSIZE + x] =
                                    current[(dy + (uint32_t)y) * width + (dx + (uint32_t)x)];

                        downsample_2x(domain_block, DSIZE, domain_down, RSIZE);
                        apply_symmetry(domain_down, domain_sym, RSIZE, t->symmetry);

                        float s = (float)t->contrast / 128.0f;
                        float o = (float)t->brightness;

                        for (int y = 0; y < RSIZE; y++) {
                            for (int x = 0; x < RSIZE; x++) {
                                uint32_t px = bx * (uint32_t)RSIZE + (uint32_t)x;
                                uint32_t py = by * (uint32_t)RSIZE + (uint32_t)y;
                                if (px < width && py < height) {
                                    float val = s * domain_sym[y * RSIZE + x] + o;
                                    if (val < 0.0f) val = 0.0f;
                                    if (val > 255.0f) val = 255.0f;
                                    next[py * width + px] = val;
                                }
                            }
                        }
                    }
                }

                float *tmp = current;
                current = next;
                next = tmp;
            }

            for (size_t i = 0; i < pixel_count; i++) {
                float v = current[i];
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                output[i * channels + c] = (uint8_t)v;
            }
        }

        free(current); free(next);
        free(domain_block); free(domain_down); free(domain_sym);
    }

    *out_data = output;
    *out_size = output_size;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_video (keyframe + temporal delta)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_video(qtsq_context_t *ctx,
                        const uint8_t *frames,
                        uint32_t width, uint32_t height,
                        uint32_t num_frames, uint8_t channels) {
    if (!ctx || !frames) return QTSQ_ERR_NULL;
    if (width == 0 || height == 0 || num_frames == 0) return QTSQ_ERR_TOO_SMALL;

    /* Delegate to the real video encoder (qtsq_video_encode.c) so the
     * singularity is an IDX1 stream that qtsq_video_decoder_open and
     * qtsq_decompress can actually read. Previously this function wrote a
     * private [keyframe + XOR deltas] layout that NO decoder understood —
     * compressed video could not be decompressed (format dead-end). */
    qtsq_video_encoder_t *enc = qtsq_video_encoder_create(
        ctx, width, height, channels, 24, QTSQ_VIDEO_PIXEL, 0);
    if (!enc) return QTSQ_ERR_ALLOC;

    size_t frame_size = (size_t)width * height * channels;
    for (uint32_t f = 0; f < num_frames; f++) {
        int rc = qtsq_video_encoder_add_frame(enc, frames + (size_t)f * frame_size);
        if (rc != QTSQ_OK) {
            qtsq_video_encoder_free(enc);
            return rc;
        }
    }
    int result = qtsq_video_encoder_finish(enc);
    qtsq_video_encoder_free(enc);
    if (result != QTSQ_OK) return result;

    ctx->header.data_type = QTSQ_TYPE_VIDEO_FRAMES;
    ctx->header.original_size = frame_size * num_frames;
    ctx->header.singularity_size = ctx->singularity_size;

    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = channels;
    ctx->schema.dimensions[3] = num_frames;
    ctx->schema.num_dims = 4;
    strncpy(ctx->schema.name, "video", 63);

    return QTSQ_OK;
}
