/*
 * QTSQ Pure Splat Image Compression Engine — Implementation
 * Represents entire images as dense 2D Gaussian Splat fields.
 * Completely independent from triangle+splat hybrid engine.
 * Author: Haruhito / QTSQ Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include <time.h>
#include "qtsq_splat_only.h"
#include "qtsq_parallel.h"
#include "qtsq_geocrypt.h"
#include "qtsq_codec.h"
#include "qtsq.h"

#ifdef __ANDROID__
#include <android/log.h>
#define SLOG(...) __android_log_print(ANDROID_LOG_INFO, "QTSQ_SPLAT", __VA_ARGS__)
#else
#define SLOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static double splat_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ══════════════════════════════════════════════════════════════
 *  Internal Sobel Gradient (independent from gravitational.c)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *pixels;
    float *grad;
    uint32_t width, height;
    uint8_t channels;
    uint32_t start_row, end_row;
} sobel_arg_t;

static void *sobel_strip(void *arg) {
    sobel_arg_t *a = (sobel_arg_t *)arg;
    uint32_t w = a->width, h = a->height;
    uint8_t ch = a->channels;
    for (uint32_t y = a->start_row; y < a->end_row; y++) {
        for (uint32_t x = 0; x < w; x++) {
            if (y == 0 || y >= h - 1 || x == 0 || x >= w - 1) {
                a->grad[y * w + x] = 0;
                continue;
            }
            float gx = 0, gy = 0;
            for (uint8_t c = 0; c < ch && c < 3; c++) {
                size_t tl = ((size_t)(y-1)*w+(x-1))*ch+c;
                size_t tc = ((size_t)(y-1)*w+ x   )*ch+c;
                size_t tr = ((size_t)(y-1)*w+(x+1))*ch+c;
                size_t ml = ((size_t) y   *w+(x-1))*ch+c;
                size_t mr = ((size_t) y   *w+(x+1))*ch+c;
                size_t bl = ((size_t)(y+1)*w+(x-1))*ch+c;
                size_t bc = ((size_t)(y+1)*w+ x   )*ch+c;
                size_t br = ((size_t)(y+1)*w+(x+1))*ch+c;
                float sx = -(float)a->pixels[tl] - 2*a->pixels[ml] - a->pixels[bl]
                           +(float)a->pixels[tr] + 2*a->pixels[mr] + a->pixels[br];
                float sy = -(float)a->pixels[tl] - 2*a->pixels[tc] - a->pixels[tr]
                           +(float)a->pixels[bl] + 2*a->pixels[bc] + a->pixels[br];
                gx += sx; gy += sy;
            }
            a->grad[y * w + x] = sqrtf(gx*gx + gy*gy) / (float)ch;
        }
    }
    return NULL;
}

static float *splat_compute_gradient(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch) {
    float *grad = (float *)calloc((size_t)w * h, sizeof(float));
    if (!grad) return NULL;
    int nc = qtsq_num_cores();
    if ((uint32_t)nc > h) nc = (int)h;
    sobel_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].pixels = pixels; args[t].grad = grad;
        args[t].width = w; args[t].height = h; args[t].channels = ch;
        args[t].start_row = (uint32_t)((uint64_t)h * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)h * (t + 1) / nc);
    }
    qtsq_dispatch(sobel_strip, args, sizeof(sobel_arg_t), nc);
    return grad;
}

/* ══════════════════════════════════════════════════════════════
 *  Full-Image Splat Fitting — Multi-Resolution
 *  Coarse (8×8) → Medium (4×4) → Fine (2×2)
 * ══════════════════════════════════════════════════════════════ */

static uint32_t fit_grid_pass(const uint8_t *pixels, const float *grad,
                               uint32_t w, uint32_t h, uint8_t ch,
                               uint32_t block_size, float grad_thresh_lo, float grad_thresh_hi,
                               uint8_t shape_type,
                               splat2d_t *out, uint32_t max_n, uint32_t offset) {
    uint32_t bw = (w + block_size - 1) / block_size;
    uint32_t bh = (h + block_size - 1) / block_size;
    uint32_t ns = 0;

    for (uint32_t by = 0; by < bh && (offset + ns) < max_n; by++) {
        for (uint32_t bx = 0; bx < bw && (offset + ns) < max_n; bx++) {
            uint32_t px0 = bx * block_size, py0 = by * block_size;
            uint32_t px1 = px0 + block_size, py1 = py0 + block_size;
            if (px1 > w) px1 = w;
            if (py1 > h) py1 = h;

            /* Compute block's max gradient to decide if this pass owns it */
            float max_g = 0;
            for (uint32_t py = py0; py < py1; py++)
                for (uint32_t px = px0; px < px1; px++) {
                    float g = grad[py * w + px];
                    if (g > max_g) max_g = g;
                }

            /* Skip if gradient not in this pass's range */
            if (max_g < grad_thresh_lo || max_g >= grad_thresh_hi) continue;

            /* Compute average color */
            double sr = 0, sg = 0, sb = 0;
            uint32_t n = 0;
            for (uint32_t py = py0; py < py1; py++) {
                for (uint32_t px = px0; px < px1; px++) {
                    size_t idx = ((size_t)py * w + px) * ch;
                    sr += pixels[idx];
                    sg += (ch >= 2) ? pixels[idx+1] : pixels[idx];
                    sb += (ch >= 3) ? pixels[idx+2] : pixels[idx];
                    n++;
                }
            }
            if (n == 0) continue;

            uint32_t cx = (px0 + px1) / 2;
            uint32_t cy = (py0 + py1) / 2;
            if (cx >= w) cx = w - 1;
            if (cy >= h) cy = h - 1;

            /* Compute color variance for sigma sizing */
            double var_r = 0;
            float avg_r = (float)(sr / n);
            for (uint32_t py = py0; py < py1; py++)
                for (uint32_t px = px0; px < px1; px++) {
                    float d = (float)pixels[((size_t)py * w + px) * ch] - avg_r;
                    var_r += d * d;
                }
            float stddev = sqrtf((float)(var_r / n));
            /* High variance → tighter splat; low variance → wider splat */
            float sigma_scale = (stddev > 30.0f) ? 0.4f : (stddev > 10.0f) ? 0.6f : 0.8f;

            splat2d_t *sp = &out[offset + ns];
            sp->x = (uint16_t)cx;
            sp->y = (uint16_t)cy;
            float half_bs = (float)block_size * sigma_scale;
            sp->sx = (uint8_t)(half_bs < 1.0f ? 1 : (half_bs > 255.0f ? 255 : half_bs));
            sp->sy = sp->sx;
            sp->theta = 0;
            sp->r = (uint8_t)(sr / n + 0.5);
            sp->g = (uint8_t)(sg / n + 0.5);
            sp->b = (uint8_t)(sb / n + 0.5);
            sp->alpha = 255;
            sp->z = 0;
            SPLAT_SET_SHAPE(sp, shape_type);
            ns++;
        }
    }
    return ns;
}

uint32_t splat_only_fit_image(const uint8_t *pixels,
                               uint32_t width, uint32_t height,
                               uint8_t channels,
                               splat2d_t *out_splats,
                               uint32_t max_splats) {
    if (!pixels || !out_splats || max_splats == 0) return 0;

    double t0 = splat_time_ms();
    float *grad = splat_compute_gradient(pixels, width, height, channels);
    if (!grad) return 0;
    SLOG("[SPLAT] Gradient: %.1f ms\n", splat_time_ms() - t0);

    /* Find gradient statistics for adaptive thresholds */
    float max_grad = 0;
    size_t npx = (size_t)width * height;
    for (size_t i = 0; i < npx; i++)
        if (grad[i] > max_grad) max_grad = grad[i];
    if (max_grad < 1.0f) max_grad = 1.0f;

    float thresh_low  = max_grad * 0.05f;   /* below = smooth (coarse) */
    float thresh_high = max_grad * 0.25f;    /* above = detail (fine) */
    float thresh_extreme = max_grad * 0.50f; /* above = micro-detail (ultra) */

    /* Budget split: 25% coarse, 25% medium, 30% fine */
    uint32_t budget_coarse = max_splats * 25 / 100;
    uint32_t budget_medium = max_splats * 25 / 100;
    uint32_t budget_fine   = max_splats * 30 / 100;

    t0 = splat_time_ms();

    /* Pass 1: Coarse (8×8) — smooth regions */
    uint32_t n_coarse = fit_grid_pass(pixels, grad, width, height, channels,
                                       8, 0.0f, thresh_low, SPLAT_SHAPE_CIRCLE,
                                       out_splats, budget_coarse, 0);

    /* Pass 2: Medium (4×4) — transition regions */
    uint32_t n_medium = fit_grid_pass(pixels, grad, width, height, channels,
                                       4, thresh_low, thresh_high, SPLAT_SHAPE_ELLIPSE,
                                       out_splats, budget_coarse + budget_medium, n_coarse);

    /* Pass 3: Fine (2×2) — detail/edge regions */
    uint32_t n_fine = fit_grid_pass(pixels, grad, width, height, channels,
                                     2, thresh_high, thresh_extreme, SPLAT_SHAPE_TRIANGLE,
                                     out_splats, budget_coarse + budget_medium + budget_fine,
                                     n_coarse + n_medium);

    /* Pass 4: Ultra-fine (1×1) — micro-detail regions (pollen, stamens, thin lines) */
    uint32_t n_ultra = fit_grid_pass(pixels, grad, width, height, channels,
                                      1, thresh_extreme, max_grad + 1.0f, SPLAT_SHAPE_CIRCLE,
                                      out_splats, max_splats,
                                      n_coarse + n_medium + n_fine);

    uint32_t total = n_coarse + n_medium + n_fine + n_ultra;

    /* Guaranteed base coverage: fill any uncovered 8×8 blocks.
     * The gradient-adaptive passes iterate top-to-bottom and may exhaust
     * their budgets before reaching the bottom of the image.  This pass
     * builds a quick coverage bitmap and places large Circle splats in
     * any 8×8 block that has zero splats from the previous passes. */
    {
        uint32_t bw8 = (width  + 7) / 8;
        uint32_t bh8 = (height + 7) / 8;
        size_t map_sz = (size_t)bw8 * bh8;
        uint8_t *covered = (uint8_t *)calloc(1, map_sz);
        if (covered) {
            /* Mark blocks that already have at least one splat */
            for (uint32_t s = 0; s < total; s++) {
                splat2d_t *sp = &out_splats[s];
                if (sp->alpha == 0) continue;
                uint32_t bx = sp->x / 8, by = sp->y / 8;
                if (bx < bw8 && by < bh8)
                    covered[by * bw8 + bx] = 1;
            }
            /* Fill uncovered blocks */
            for (uint32_t by = 0; by < bh8 && total < max_splats; by++) {
                for (uint32_t bx = 0; bx < bw8 && total < max_splats; bx++) {
                    if (covered[by * bw8 + bx]) continue;
                    uint32_t cx = bx * 8 + 4, cy = by * 8 + 4;
                    if (cx >= width) cx = width - 1;
                    if (cy >= height) cy = height - 1;
                    size_t pidx = ((size_t)cy * width + cx) * channels;
                    splat2d_t *sp = &out_splats[total];
                    sp->x = (uint16_t)cx; sp->y = (uint16_t)cy;
                    sp->sx = 6; sp->sy = 6; sp->theta = 0;
                    sp->r = pixels[pidx];
                    sp->g = (channels >= 2) ? pixels[pidx+1] : pixels[pidx];
                    sp->b = (channels >= 3) ? pixels[pidx+2] : pixels[pidx];
                    sp->alpha = 255; sp->z = 0;
                    SPLAT_SET_SHAPE(sp, SPLAT_SHAPE_CIRCLE);
                    total++;
                }
            }
            free(covered);
        }
    }

    free(grad);
    SLOG("[SPLAT] Fit: %u splats (coarse=%u med=%u fine=%u ultra=%u) in %.1f ms\n",
         total, n_coarse, n_medium, n_fine, n_ultra, splat_time_ms() - t0);
    return total;
}

/* ══════════════════════════════════════════════════════════════
 *  Pure Splat Renderer — Multi-Threaded
 *  No fg_mask — renders everywhere. Front-to-back alpha compositing.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const splat2d_t *splats;
    uint32_t count;
    uint8_t *rgb;
    uint32_t width, height;
    uint8_t channels;
    uint32_t start_row, end_row;
} so_render_arg_t;

static void *so_render_strip(void *arg) {
    so_render_arg_t *a = (so_render_arg_t *)arg;
    uint32_t w = a->width, row0 = a->start_row, row1 = a->end_row;
    uint8_t ch = a->channels;

    for (uint32_t s = 0; s < a->count; s++) {
        const splat2d_t *sp = &a->splats[s];
        if (sp->alpha == 0) continue;

        int shape = SPLAT_SHAPE(sp);
        float sx = (float)sp->sx;
        if (sx < 1.0f) sx = 1.0f;
        float inv_sx2 = 1.0f / (sx * sx);
        float alpha_f = (float)sp->alpha / 255.0f;

        float max_sigma = sx;
        float theta = 0.0f, cos_t = 1.0f, sin_t = 0.0f, sy = sx, inv_sy2 = inv_sx2;

        if (shape == SPLAT_SHAPE_ELLIPSE || shape == SPLAT_SHAPE_TRIANGLE) {
            theta = (float)sp->theta * (3.14159265f / 255.0f);
            cos_t = cosf(theta);
            sin_t = sinf(theta);
            if (shape == SPLAT_SHAPE_ELLIPSE) {
                sy = (float)sp->sy;
                if (sy < 1.0f) sy = 1.0f;
                inv_sy2 = 1.0f / (sy * sy);
                if (sy > max_sigma) max_sigma = sy;
            }
        }

        int radius = (int)ceilf(max_sigma * 3.0f);

        int x0 = (int)sp->x - radius, x1 = (int)sp->x + radius;
        int y0 = (int)sp->y - radius, y1 = (int)sp->y + radius;
        if (x0 < 0) x0 = 0;
        if (x1 >= (int)w) x1 = (int)w - 1;
        if (y0 < (int)row0) y0 = (int)row0;
        if (y1 >= (int)row1) y1 = (int)row1 - 1;
        if (y0 > y1 || x0 > x1) continue;

        /* Precompute triangle vertices if needed */
        float tv0x = 0, tv0y = 0, tv1x = 0, tv1y = 0, tv2x = 0, tv2y = 0;
        float v0x = 0, v0y = 0, v1x = 0, v1y = 0, v2x = 0, v2y = 0;
        if (shape == SPLAT_SHAPE_TRIANGLE) {
            float TR = sx * 3.0f; /* radius of circumcircle */
            /* +120 deg = 2.0943951f rad, +240 deg = 4.1887902f rad */
            tv0x = (float)sp->x + TR * cos_t;
            tv0y = (float)sp->y + TR * sin_t;
            tv1x = (float)sp->x + TR * cosf(theta + 2.0943951f);
            tv1y = (float)sp->y + TR * sinf(theta + 2.0943951f);
            tv2x = (float)sp->x + TR * cosf(theta + 4.1887902f);
            tv2y = (float)sp->y + TR * sinf(theta + 4.1887902f);
            v0x = tv2x - tv1x; v0y = tv2y - tv1y;
            v1x = tv0x - tv2x; v1y = tv0y - tv2y;
            v2x = tv1x - tv0x; v2y = tv1y - tv0y;
        }

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float dx = (float)(px - (int)sp->x);
                float dy = (float)(py - (int)sp->y);
                float weight = 0.0f;

                if (shape == SPLAT_SHAPE_CIRCLE) {
                    float exponent = -0.5f * (dx*dx + dy*dy) * inv_sx2;
                    if (exponent >= -4.5f) weight = alpha_f * expf(exponent);
                } else if (shape == SPLAT_SHAPE_ELLIPSE) {
                    float lx = dx * cos_t + dy * sin_t;
                    float ly = -dx * sin_t + dy * cos_t;
                    float exponent = -0.5f * (lx*lx*inv_sx2 + ly*ly*inv_sy2);
                    if (exponent >= -4.5f) weight = alpha_f * expf(exponent);
                } else if (shape == SPLAT_SHAPE_TRIANGLE) {
                    /* Barycentric containment test */
                    float px_v1x = (float)px - tv1x, py_v1y = (float)py - tv1y;
                    float px_v2x = (float)px - tv2x, py_v2y = (float)py - tv2y;
                    float px_v0x = (float)px - tv0x, py_v0y = (float)py - tv0y;
                    
                    float d1 = px_v1x * v0y - py_v1y * v0x;
                    float d2 = px_v2x * v1y - py_v2y * v1x;
                    float d3 = px_v0x * v2y - py_v0y * v2x;
                    
                    int has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                    int has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                    
                    if (!(has_neg && has_pos)) { /* Inside boundary */
                        float d2 = (dx*dx + dy*dy) * inv_sx2;
                        float exponent = -0.5f * d2 * sqrtf(d2); /* cubic: sharper than Gaussian, gentler than quartic */
                        if (exponent >= -4.5f) weight = alpha_f * expf(exponent);
                    }
                }

                if (weight == 0.0f) continue;

                size_t pi = ((size_t)py * w + px) * ch;
                float inv_w = 1.0f - weight;
                a->rgb[pi] = (uint8_t)(a->rgb[pi] * inv_w + sp->r * weight + 0.5f);
                if (ch >= 2) a->rgb[pi+1] = (uint8_t)(a->rgb[pi+1] * inv_w + sp->g * weight + 0.5f);
                if (ch >= 3) a->rgb[pi+2] = (uint8_t)(a->rgb[pi+2] * inv_w + sp->b * weight + 0.5f);
            }
        }
    }
    return NULL;
}

void splat_only_render(const splat2d_t *splats, uint32_t count,
                        uint8_t *rgb, uint32_t width, uint32_t height,
                        uint8_t channels) {
    if (!splats || !rgb || count == 0) return;
    int nc = qtsq_num_cores();
    if ((uint32_t)nc > height) nc = (int)height;
    so_render_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].splats = splats; args[t].count = count;
        args[t].rgb = rgb; args[t].width = width; args[t].height = height;
        args[t].channels = channels;
        args[t].start_row = (uint32_t)((uint64_t)height * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)height * (t + 1) / nc);
    }
    qtsq_dispatch(so_render_strip, args, sizeof(so_render_arg_t), nc);
}

/* ══════════════════════════════════════════════════════════════
 *  Gradient Descent Refinement — Multi-Threaded
 *  Jittered Multi-View for sub-pixel accuracy
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    splat2d_t *splats;
    uint32_t start_s, end_s;
    const uint8_t *target;
    const uint8_t *render;
    uint32_t width, height;
    uint8_t channels;
    float lr_color, lr_pos, lr_scale, lr_theta;
} so_gd_arg_t;

static void *so_gd_worker(void *arg) {
    so_gd_arg_t *a = (so_gd_arg_t *)arg;
    uint32_t w = a->width, h = a->height;
    uint8_t ch = a->channels;

    for (uint32_t s = a->start_s; s < a->end_s; s++) {
        splat2d_t *sp = &a->splats[s];
        if (sp->alpha == 0) continue;

        int shape = SPLAT_SHAPE(sp);
        float sx = (float)sp->sx, sy = (float)sp->sy;
        if (sx < 1.0f) sx = 1.0f;
        if (sy < 1.0f) sy = 1.0f;
        float inv_sx2 = 1.0f / (sx * sx), inv_sy2 = 1.0f / (sy * sy);
        float max_sig = (sx > sy) ? sx : sy;
        int radius = (int)ceilf(max_sig * 2.5f);

        /* Rotation for Ellipse/Triangle shapes */
        float theta = (float)sp->theta * (3.14159265f / 255.0f);
        float cos_t = cosf(theta), sin_t = sinf(theta);

        int x0 = (int)sp->x - radius; if (x0 < 0) x0 = 0;
        int y0 = (int)sp->y - radius; if (y0 < 0) y0 = 0;
        int x1 = (int)sp->x + radius; if (x1 >= (int)w) x1 = w - 1;
        int y1 = (int)sp->y + radius; if (y1 >= (int)h) y1 = h - 1;

        float gr = 0, gg = 0, gb = 0, gx = 0, gy = 0;
        float gsx = 0, gsy = 0, g_theta = 0, wsum = 0;

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float dx = (float)(px - (int)sp->x);
                float dy = (float)(py - (int)sp->y);

                float exp_v, lx, ly;
                if (shape == SPLAT_SHAPE_CIRCLE) {
                    exp_v = -0.5f * (dx*dx + dy*dy) * inv_sx2;
                    lx = dx; ly = dy;
                } else if (shape == SPLAT_SHAPE_TRIANGLE) {
                    lx = dx * cos_t + dy * sin_t;
                    ly = -dx * sin_t + dy * cos_t;
                    float d2 = (lx*lx + ly*ly) * inv_sx2;
                    exp_v = -0.5f * d2 * sqrtf(d2); /* cubic: match renderer */
                } else {
                    lx = dx * cos_t + dy * sin_t;
                    ly = -dx * sin_t + dy * cos_t;
                    exp_v = -0.5f * (lx*lx*inv_sx2 + ly*ly*inv_sy2);
                }
                if (exp_v < -4.5f) continue;
                float wt = expf(exp_v);

                size_t idx = (size_t)py * w + px;
                size_t tp = idx * ch, rp = idx * ch;

                float er = (float)a->target[tp]   - (float)a->render[rp];
                float eg = (ch >= 2) ? (float)a->target[tp+1] - (float)a->render[rp+1] : er;
                float eb = (ch >= 3) ? (float)a->target[tp+2] - (float)a->render[rp+2] : er;

                gr += er * wt; gg += eg * wt; gb += eb * wt;
                float el = (er + eg + eb) / 3.0f;
                gx += el * dx * wt * inv_sx2;
                gy += el * dy * wt * inv_sy2;
                gsx += el * (dx*dx) * wt * inv_sx2 * (1.0f/sx);
                gsy += el * (dy*dy) * wt * inv_sy2 * (1.0f/sy);

                /* θ gradient: cross-term derivative for Ellipse/Triangle */
                if (shape != SPLAT_SHAPE_CIRCLE) {
                    g_theta += el * lx * ly * (inv_sx2 - inv_sy2) * wt;
                }

                wsum += wt;
            }
        }

        if (wsum > 0.01f) {
            float sc = 1.0f / wsum;
            int nr = (int)sp->r + (int)(a->lr_color * gr * sc);
            int ng = (int)sp->g + (int)(a->lr_color * gg * sc);
            int nb = (int)sp->b + (int)(a->lr_color * gb * sc);
            int nx = (int)sp->x + (int)(a->lr_pos * gx * sc);
            int ny = (int)sp->y + (int)(a->lr_pos * gy * sc);
            int nsx = (int)sp->sx + (int)(a->lr_scale * gsx * sc);
            int nsy = (int)sp->sy + (int)(a->lr_scale * gsy * sc);

            /* Shape constraints */
            if (shape == SPLAT_SHAPE_CIRCLE || shape == SPLAT_SHAPE_TRIANGLE) {
                int avg_ns = (nsx + nsy) / 2;
                nsx = avg_ns;
                nsy = avg_ns;
            }

            /* θ update for non-circle shapes */
            int ntheta = (int)sp->theta;
            if (shape != SPLAT_SHAPE_CIRCLE) {
                ntheta += (int)(a->lr_theta * g_theta * sc);
                if (ntheta < 0) ntheta = 0;
                if (ntheta > 255) ntheta = 255;
            }

            if (nr < 0) nr = 0; if (nr > 255) nr = 255;
            if (ng < 0) ng = 0; if (ng > 255) ng = 255;
            if (nb < 0) nb = 0; if (nb > 255) nb = 255;
            if (nx < 0) nx = 0; if (nx >= (int)w) nx = w - 1;
            if (ny < 0) ny = 0; if (ny >= (int)h) ny = h - 1;
            if (nsx < 1) nsx = 1; if (nsx > 255) nsx = 255;
            if (nsy < 1) nsy = 1; if (nsy > 255) nsy = 255;

            sp->r = (uint8_t)nr; sp->g = (uint8_t)ng; sp->b = (uint8_t)nb;
            sp->x = (uint16_t)nx; sp->y = (uint16_t)ny;
            sp->sx = (uint8_t)nsx; sp->sy = (uint8_t)nsy;
            sp->theta = (uint8_t)ntheta;
        }
    }
    return NULL;
}

/* Bilinear interpolation for jittered views */
static void so_jitter_view(const uint8_t *src, uint32_t w, uint32_t h,
                            uint8_t ch, float shift_x, float shift_y, uint8_t *dst) {
    for (uint32_t y = 0; y < h; y++) {
        float fy = (float)y + shift_y;
        int y0 = (int)fy; float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        if (h < 2) { y0 = 0; ty = 0; }
        else if (y0 >= (int)h - 1) { y0 = (int)h - 2; ty = 1.0f; }
        for (uint32_t x = 0; x < w; x++) {
            float fx = (float)x + shift_x;
            int x0 = (int)fx; float tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            if (w < 2) { x0 = 0; tx = 0; }
            else if (x0 >= (int)w - 1) { x0 = (int)w - 2; tx = 1.0f; }
            float omtx = 1.0f - tx, omty = 1.0f - ty;
            size_t p00 = ((size_t)y0*w+x0)*ch, p01 = p00+ch;
            size_t p10 = p00+(size_t)w*ch, p11 = p10+ch;
            size_t di = ((size_t)y*w+x)*ch;
            for (uint8_t c = 0; c < ch && c < 3; c++) {
                float v = src[p00+c]*omtx*omty + src[p01+c]*tx*omty +
                          src[p10+c]*omtx*ty + src[p11+c]*tx*ty;
                int iv = (int)(v + 0.5f);
                dst[di+c] = (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
            }
        }
    }
}

void splat_only_refine(splat2d_t *splats, uint32_t count,
                        const uint8_t *target,
                        uint32_t width, uint32_t height,
                        uint8_t channels,
                        int iters, int num_views) {
    if (!splats || count == 0 || !target || iters <= 0) return;
    if (num_views < 1) num_views = 1;
    if (num_views > 30) num_views = 30;

    size_t frame_bytes = (size_t)width * height * channels;

    /* Allocate jittered views */
    uint8_t **views = (uint8_t **)calloc(num_views, sizeof(uint8_t *));
    if (!views) return;
    views[0] = (uint8_t *)malloc(frame_bytes);
    if (!views[0]) { free(views); return; }
    memcpy(views[0], target, frame_bytes);

    uint32_t seed = 0xCAFEBABE;
    int actual_views = 1;
    for (int v = 1; v < num_views; v++) {
        views[v] = (uint8_t *)malloc(frame_bytes);
        if (!views[v]) break;
        seed = seed * 1664525u + 1013904223u;
        float sx = ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 1.8f;
        seed = seed * 1664525u + 1013904223u;
        float sy = ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 1.8f;
        so_jitter_view(target, width, height, channels, sx, sy, views[v]);
        actual_views++;
    }

    uint8_t *render = (uint8_t *)calloc((size_t)width * height, channels);
    if (!render) goto cleanup;

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > count) nc = (int)count;

    for (int it = 0; it < iters; it++) {
        const uint8_t *cur = views[it % actual_views];
        memset(render, 0, (size_t)width * height * channels);
        splat_only_render(splats, count, render, width, height, channels);

        so_gd_arg_t gargs[QTSQ_MAX_THREADS];
        for (int t = 0; t < nc; t++) {
            gargs[t].splats = splats;
            gargs[t].start_s = (uint32_t)((uint64_t)count * t / nc);
            gargs[t].end_s   = (uint32_t)((uint64_t)count * (t + 1) / nc);
            gargs[t].target = cur; gargs[t].render = render;
            gargs[t].width = width; gargs[t].height = height;
            gargs[t].channels = channels;
            gargs[t].lr_color = 0.6f; gargs[t].lr_pos = 0.2f; gargs[t].lr_scale = 0.2f; gargs[t].lr_theta = 0.15f;
        }
        qtsq_dispatch(so_gd_worker, gargs, sizeof(so_gd_arg_t), nc);
    }
    free(render);

cleanup:
    for (int v = 0; v < num_views; v++) if (views[v]) free(views[v]);
    free(views);
}

/* ══════════════════════════════════════════════════════════════
 *  Adaptive Densification — Split High-Error Splats
 *  Re-renders, measures per-splat error, splits overloaded splats
 *  into 2 smaller children for better detail coverage.
 * ══════════════════════════════════════════════════════════════ */

static uint32_t splat_densify(splat2d_t *splats, uint32_t count,
                               const uint8_t *target,
                               uint32_t width, uint32_t height,
                               uint8_t channels,
                               uint32_t max_capacity) {
    if (!splats || count == 0 || !target) return count;

    size_t frame_bytes = (size_t)width * height * channels;
    uint8_t *render = (uint8_t *)calloc(1, frame_bytes);
    if (!render) return count;

    /* Render current state */
    splat_only_render(splats, count, render, width, height, channels);

    uint32_t new_count = count;
    const float error_threshold = 45.0f;

    for (uint32_t s = 0; s < count && new_count + 2 <= max_capacity; s++) {
        splat2d_t *sp = &splats[s];
        if (sp->alpha == 0) continue;
        if (sp->sx <= 3 && sp->sy <= 3) continue; /* Too small to split */
        if (sp->sx > 6) continue; /* Don't split large coarse/background splats */

        /* Compute average absolute error within this splat's footprint */
        float sx = (float)sp->sx;
        if (sx < 1.0f) sx = 1.0f;
        int radius = (int)ceilf(sx * 2.0f);
        int x0 = (int)sp->x - radius; if (x0 < 0) x0 = 0;
        int y0 = (int)sp->y - radius; if (y0 < 0) y0 = 0;
        int x1 = (int)sp->x + radius; if (x1 >= (int)width)  x1 = width - 1;
        int y1 = (int)sp->y + radius; if (y1 >= (int)height) y1 = height - 1;

        float total_err = 0;
        int n_px = 0;
        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                size_t idx = ((size_t)py * width + px) * channels;
                for (uint8_t c = 0; c < channels && c < 3; c++) {
                    float diff = (float)target[idx + c] - (float)render[idx + c];
                    total_err += (diff < 0) ? -diff : diff;
                }
                n_px++;
            }
        }
        if (n_px == 0) continue;
        float avg_err = total_err / (float)(n_px * channels);

        if (avg_err > error_threshold) {
            /* Split: reduce parent alpha, create 2 children at 2D offsets */
            uint8_t half_sx = (sp->sx > 3) ? sp->sx / 2 : 2;
            uint8_t half_sy = (sp->sy > 3) ? sp->sy / 2 : 2;
            int off_x = (int)sp->sx / 4;
            int off_y = (int)sp->sy / 4;
            if (off_x < 1) off_x = 1;
            if (off_y < 1) off_y = 1;

            int shape = SPLAT_SHAPE(sp);

            /* Child 1: shifted upper-left */
            int c1x = (int)sp->x - off_x; if (c1x < 0) c1x = 0;
            int c1y = (int)sp->y - off_y; if (c1y < 0) c1y = 0;
            size_t c1_pidx = ((size_t)c1y * width + c1x) * channels;
            splat2d_t *c1 = &splats[new_count++];
            *c1 = *sp;
            c1->x = (uint16_t)c1x; c1->y = (uint16_t)c1y;
            c1->sx = half_sx; c1->sy = half_sy;
            /* Sample actual target pixel color */
            c1->r = target[c1_pidx];
            c1->g = (channels >= 2) ? target[c1_pidx+1] : target[c1_pidx];
            c1->b = (channels >= 3) ? target[c1_pidx+2] : target[c1_pidx];
            SPLAT_SET_SHAPE(c1, shape);

            /* Child 2: shifted lower-right */
            int c2x = (int)sp->x + off_x; if (c2x >= (int)width) c2x = width - 1;
            int c2y = (int)sp->y + off_y; if (c2y >= (int)height) c2y = height - 1;
            size_t c2_pidx = ((size_t)c2y * width + c2x) * channels;
            splat2d_t *c2 = &splats[new_count++];
            *c2 = *sp;
            c2->x = (uint16_t)c2x; c2->y = (uint16_t)c2y;
            c2->sx = half_sx; c2->sy = half_sy;
            c2->r = target[c2_pidx];
            c2->g = (channels >= 2) ? target[c2_pidx+1] : target[c2_pidx];
            c2->b = (channels >= 3) ? target[c2_pidx+2] : target[c2_pidx];
            SPLAT_SET_SHAPE(c2, shape);

            /* Reduce parent alpha instead of disabling — keeps background coverage */
            sp->alpha = sp->alpha / 2;
        }
    }

    free(render);
    SLOG("[SPLAT] Densify: %u → %u splats (split %u)\n",
         count, new_count, new_count - count);
    return new_count;
}

/* ══════════════════════════════════════════════════════════════
 *  GSP5 Binary Format — Standalone Splat-Only Container
 *  "GSP5" | width(4) | height(4) | count(4) | channels(1) | planar data
 * ══════════════════════════════════════════════════════════════ */

int splat_only_pack(const splat2d_t *splats, uint32_t count,
                     uint32_t width, uint32_t height,
                     uint8_t **out_data, size_t *out_size) {
    if (!splats || !out_data || !out_size) return -1;

    size_t header_sz = 17; /* GSP5(4) + w(4) + h(4) + count(4) + ch(1) */
    size_t data_sz = (size_t)count * 12;
    size_t total = header_sz + data_sz;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t p = 0;
    buf[p++] = 'G'; buf[p++] = 'S'; buf[p++] = 'P'; buf[p++] = '5';
    memcpy(buf + p, &width, 4);  p += 4;
    memcpy(buf + p, &height, 4); p += 4;
    memcpy(buf + p, &count, 4);  p += 4;
    buf[p++] = 3; /* channels (always RGB for now) */

    /* Planar layout for ZLib efficiency */
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].x & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].x >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].y & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].y >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].sx;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].sy;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].theta;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].r;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].g;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].b;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].alpha;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].z;

    *out_data = buf;
    *out_size = total;
    return 0;
}

/* Public singleton for use outside this TU (e.g. qtsq_seal.c geometry embed).
 * Thin forwarder to the file-local splat_only_unpack. */
int qtsq_splat_only_unpack(const uint8_t *data, size_t size,
                           splat2d_t **out_splats, uint32_t *out_count,
                           uint32_t *out_width, uint32_t *out_height) {
    return splat_only_unpack(data, size, out_splats, out_count, out_width, out_height);
}

int splat_only_unpack(const uint8_t *data, size_t size,
                       splat2d_t **out_splats, uint32_t *out_count,
                       uint32_t *out_width, uint32_t *out_height) {
    if (!data || !out_splats || !out_count) return -1;
    if (size < 17) return -1;
    if (data[0] != 'G' || data[1] != 'S' || data[2] != 'P' || 
        (data[3] != '4' && data[3] != '5')) return -1;

    uint32_t w, h, count;
    memcpy(&w, data + 4, 4);
    memcpy(&h, data + 8, 4);
    memcpy(&count, data + 12, 4);
    /* uint8_t ch = data[16]; — reserved for future use */

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;

    size_t expected = 17 + (size_t)count * 12;
    if (size < expected) return -1;

    splat2d_t *splats = (splat2d_t *)malloc(count * sizeof(splat2d_t));
    if (!splats) return -1;

    const uint8_t *p = data + 17;
    size_t stride = count;

    for (uint32_t i = 0; i < count; i++)
        splats[i].x = (uint16_t)(p[i] | (p[stride + i] << 8));
    p += stride * 2;
    for (uint32_t i = 0; i < count; i++)
        splats[i].y = (uint16_t)(p[i] | (p[stride + i] << 8));
    p += stride * 2;
    for (uint32_t i = 0; i < count; i++) splats[i].sx    = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].sy    = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].theta = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].r     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].g     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].b     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].alpha = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].z     = p[i]; p += stride;

    *out_splats = splats;
    *out_count = count;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Internal: Compress + store splat payload into context
 * ══════════════════════════════════════════════════════════════ */

static int splat_only_compress_core(qtsq_context_t *ctx,
                                     const uint8_t *pixels,
                                     uint32_t width, uint32_t height,
                                     uint8_t channels,
                                     size_t source_size,
                                     uint32_t max_splats,
                                     int refine_iters, int refine_views,
                                     int zlib_level,
                                     const char *schema_name,
                                     const char *encoding_name) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "splat_only");
    if (guard != QTSQ_OK) return guard;

    double t_total = splat_time_ms();
    size_t pixel_count = (size_t)width * height;

    /* Allocate splat buffer with +15% headroom for densification */
    uint32_t capacity = max_splats + max_splats / 6;
    splat2d_t *splats = (splat2d_t *)malloc(capacity * sizeof(splat2d_t));
    if (!splats) return QTSQ_ERR_ALLOC;

    /* Fit splats to entire image */
    uint32_t ns = splat_only_fit_image(pixels, width, height, channels,
                                        splats, max_splats);
    if (ns == 0) { free(splats); return QTSQ_ERR_FORMAT; }

    /* Refine via gradient descent */
    double t_refine = splat_time_ms();
    splat_only_refine(splats, ns, pixels, width, height, channels,
                       refine_iters, refine_views);
    SLOG("[SPLAT %s] Refine: %.1f ms (%d iters, %d views)\n",
         schema_name, splat_time_ms() - t_refine, refine_iters, refine_views);

    /* Adaptive densification: split high-error splats */
    double t_dense = splat_time_ms();
    ns = splat_densify(splats, ns, pixels, width, height, channels, capacity);
    /* Quick refinement pass on new children */
    if (ns > max_splats) {
        splat_only_refine(splats, ns, pixels, width, height, channels, 2, 1);
    }
    SLOG("[SPLAT %s] Densify+refit: %.1f ms (%u splats)\n",
         schema_name, splat_time_ms() - t_dense, ns);

    /* ── Geometric Cryptography: encrypt splats BEFORE packing ── */
    if (ctx->has_geocrypt && ctx->geocrypt_mode != 0) {
        qtsq_geocrypt_encrypt_splats(splats, ns, ctx->geocrypt_key, ctx->geocrypt_mode);
        ctx->header.flags |= QTSQ_FLAG_GEOCRYPT;
        ctx->security.geocrypt_mode = ctx->geocrypt_mode;
        SLOG("[SPLAT %s] GeoCrypt applied (mode=0x%02X, %u splats)\n",
             schema_name, ctx->geocrypt_mode, ns);
    }

    /* Pack splats into GSP5 format */
    uint8_t *gsp4_data = NULL;
    size_t gsp4_size = 0;
    if (splat_only_pack(splats, ns, width, height, &gsp4_data, &gsp4_size) != 0) {
        free(splats); return QTSQ_ERR_ALLOC;
    }
    free(splats);

    /* Compress: qtc (opt-in) or zlib (default) */
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(gsp4_data, gsp4_size, &stream, &stream_len) != QTSQ_OK) {
            free(gsp4_data); return QTSQ_ERR_ALLOC;
        }
    } else {
        uLongf zb = compressBound((uLong)gsp4_size);
        stream = (uint8_t *)malloc(zb);
        if (!stream) { free(gsp4_data); return QTSQ_ERR_ALLOC; }
        uLongf zs = zb;
        if (compress2(stream, &zs, gsp4_data, (uLong)gsp4_size, zlib_level) != Z_OK) {
            free(gsp4_data); free(stream); return QTSQ_ERR_ALLOC;
        }
        stream_len = (size_t)zs;
    }

    uint8_t *payload = (uint8_t *)malloc(4 + stream_len);
    if (!payload) { free(gsp4_data); free(stream); return QTSQ_ERR_ALLOC; }
    uint32_t uncomp = (uint32_t)gsp4_size;
    memcpy(payload, &uncomp, 4);
    memcpy(payload + 4, stream, stream_len);
    free(stream);
    free(gsp4_data);

    size_t payload_size = 4 + stream_len;

    /* Store in context */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = payload;
    ctx->singularity_size = (uint32_t)payload_size;

    ctx->header.strategy = QTSQ_STRATEGY_SPLAT;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.reserved32 = ns;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_SPLAT_ONLY;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, schema_name, 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = ns;
    ctx->schema.num_dims = 3;
    strncpy(ctx->schema.encoding, encoding_name, 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    SLOG("[SPLAT %s] Total: %.1f ms | %u splats | %zu → %zu bytes\n",
         schema_name, splat_time_ms() - t_total, ns, source_size, payload_size);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Three Compression Tiers
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_splat_compact(qtsq_context_t *ctx,
                                 const uint8_t *pixels,
                                 uint32_t width, uint32_t height,
                                 uint8_t channels,
                                 size_t source_size) {
    size_t pixel_count = (size_t)width * height;
    /* Budget: ~65-75% of source, each splat ~12 bytes raw, ~3 bytes after ZLib */
    uint32_t max_splats = (uint32_t)(source_size > 0 ?
                           (source_size * 0.75) / 3 : pixel_count / 4);
    if (max_splats < 50000) max_splats = 50000;
    if (max_splats > 500000) max_splats = 500000;

#ifdef __ANDROID__
    /* Mobile: 1 jitter view instead of 2 — each view allocates a full
     * frame copy (37 MB at 13MP) and views are only an anti-overfit
     * regularizer. Halves refine memory with negligible quality cost. */
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 2, 1, 1,
                                     "splat_compact", "SPLAT_COMPACT");
#else
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 5, 8, 1,
                                     "splat_compact", "SPLAT_COMPACT");
#endif
}

int qtsq_compress_splat_hd(qtsq_context_t *ctx,
                             const uint8_t *pixels,
                             uint32_t width, uint32_t height,
                             uint8_t channels,
                             size_t source_size) {
    size_t pixel_count = (size_t)width * height;
    uint32_t max_splats = (uint32_t)(source_size > 0 ?
                           (source_size * 1.2) / 3 : pixel_count / 2);
    if (max_splats < 400000) max_splats = 400000;
    if (max_splats > 1000000) max_splats = 1000000;

#ifdef __ANDROID__
    /* Mobile: 1 jitter view (see splat_compact note). */
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 3, 1, 1,
                                     "splat_hd", "SPLAT_HD");
#else
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 5, 8, 1,
                                     "splat_hd", "SPLAT_HD");
#endif
}

int qtsq_compress_splat_lossless(qtsq_context_t *ctx,
                                   const uint8_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint8_t channels,
                                   size_t source_size) {
    size_t pixel_count = (size_t)width * height;
    uint32_t max_splats = (uint32_t)(source_size > 0 ?
                           (source_size * 2.5) / 3 : pixel_count);
    if (max_splats < 1000000) max_splats = 1000000;
    if (max_splats > 2000000) max_splats = 2000000;

#ifdef __ANDROID__
    /* Mobile: 1 jitter view (see splat_compact note). */
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 3, 1, 1,
                                     "splat_lossless", "SPLAT_LOSSLESS");
#else
    return splat_only_compress_core(ctx, pixels, width, height, channels,
                                     source_size, max_splats, 8, 8, 6,
                                     "splat_lossless", "SPLAT_LOSSLESS");
#endif
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Decompressor — Pure Splat Only
 *  Completely independent from qtsq_decompress_gravitational.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_splat_only(qtsq_context_t *ctx,
                                 uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;

    double t_total = splat_time_ms();

    /* Decompress ZLib — SEC: bound file-declared size (DoS guard).
     * GSP payloads are compact splat arrays; 256:1 ratio, 512 MB cap. */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(ctx->singularity, ctx->singularity_size,
                                     256, 1ULL << 29, &packed, &dl);
    if (rc != QTSQ_OK) return rc;

    /* Verify GSP4/GSP5 magic */
    if (dl < 17 || packed[0] != 'G' || packed[1] != 'S' ||
        packed[2] != 'P' || (packed[3] != '4' && packed[3] != '5')) {
        free(packed); return QTSQ_ERR_FORMAT;
    }

    /* Unpack splats */
    splat2d_t *splats = NULL;
    uint32_t count = 0, orig_w = 0, orig_h = 0;
    if (splat_only_unpack(packed, dl, &splats, &count, &orig_w, &orig_h) != 0) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    free(packed);

    /* ── Geometric Cryptography: decrypt splats BEFORE rendering ── */
    if ((ctx->header.flags & QTSQ_FLAG_GEOCRYPT) || ctx->has_geocrypt) {
        uint8_t mode = ctx->has_geocrypt ? ctx->geocrypt_mode : ctx->security.geocrypt_mode;
        if (mode == 0) mode = QTSQ_GEOCRYPT_FULL; /* legacy fallback */
        qtsq_geocrypt_decrypt_splats(splats, count, ctx->geocrypt_key, mode);
        SLOG("[SPLAT] GeoCrypt decrypt applied (mode=0x%02X, %u splats)\n", mode, count);
    }

    uint8_t ch = ctx->header.num_channels;
    if (ch < 1) ch = 3;
    if (ch > 4) ch = 3;

    /* Expose dimensions */
    ctx->schema.dimensions[0] = orig_w;
    ctx->schema.dimensions[1] = orig_h;
    ctx->schema.dimensions[2] = ch;

    /* Allocate output */
    size_t out_px = (size_t)orig_w * orig_h * ch;
    /* SEC: orig_w/orig_h come from the file — a tiny splat payload could
     * declare absurd dimensions and force a huge malloc. Cap output at
     * 512 MB (generous for any real image). */
    if (out_px > (1ULL << 29)) { free(splats); return QTSQ_ERR_FORMAT; }
    uint8_t *output = NULL;
    if (*out_data != NULL) {
        output = *out_data;
    } else {
        output = (uint8_t *)malloc(out_px);
        if (!output) { free(splats); return QTSQ_ERR_ALLOC; }
        *out_data = output;
    }
    memset(output, 0, out_px);

    /* Render splats */
    double t_render = splat_time_ms();
    splat_only_render(splats, count, output, orig_w, orig_h, ch);
    SLOG("[SPLAT] Render: %.1f ms (%u splats)\n", splat_time_ms() - t_render, count);
    free(splats);

    /* ── Post-processing: Bilateral filter (2-pass, 5×5) ── */
    {
        uint8_t *bilateral = (uint8_t *)malloc(out_px);
        if (bilateral) {
            const int radius = 2;
            const float inv_2ss = 1.0f / (2.0f * 2.0f * 2.0f);
            const float inv_2sc = 1.0f / (2.0f * 20.0f * 20.0f);
            float sw[5][5];
            for (int dy = -radius; dy <= radius; dy++)
                for (int dx = -radius; dx <= radius; dx++)
                    sw[dy+radius][dx+radius] = expf(-(float)(dx*dx+dy*dy)*inv_2ss);

            for (int pass = 0; pass < 2; pass++) {
                const uint8_t *src = (pass == 0) ? output : bilateral;
                uint8_t *dst = (pass == 0) ? bilateral : output;
                for (uint32_t y = 0; y < orig_h; y++) {
                    for (uint32_t x = 0; x < orig_w; x++) {
                        size_t idx = ((size_t)y * orig_w + x) * ch;
                        if (y < (uint32_t)radius || y >= orig_h - radius ||
                            x < (uint32_t)radius || x >= orig_w - radius) {
                            for (uint8_t c = 0; c < ch; c++) dst[idx+c] = src[idx+c];
                            continue;
                        }
                        for (uint8_t c = 0; c < ch; c++) {
                            float cv = (float)src[idx+c], ws = 0, wt = 0;
                            for (int dy = -radius; dy <= radius; dy++) {
                                for (int dx = -radius; dx <= radius; dx++) {
                                    size_t ni = ((size_t)(y+dy)*orig_w+(x+dx))*ch+c;
                                    float nb = (float)src[ni];
                                    float cd = cv - nb;
                                    float w = sw[dy+radius][dx+radius] * expf(-cd*cd*inv_2sc);
                                    ws += w * nb; wt += w;
                                }
                            }
                            dst[idx+c] = (uint8_t)(ws / wt + 0.5f);
                        }
                    }
                }
            }
            free(bilateral);
        }
    }

    /* Contrast stretch removed — splat colors are already fitted to
     * the original pixel values, so histogram stretching blows out
     * highlights (sky, specular) and crushes shadows unnecessarily. */

    *out_data = output;
    *out_size = out_px;
    SLOG("[SPLAT] Decompress total: %.1f ms\n", splat_time_ms() - t_total);
    return QTSQ_OK;
}
