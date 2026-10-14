/*
 * QTSQ 2D Gaussian Splat Engine — Implementation
 * Part of QTSQ HD v3 — Hybrid Triangle + Splat Compression
 * Multi-threaded for maximum performance on all CPU cores
 * Author: Haruhito
 */

#include "qtsq_splat2d.h"
#include "qtsq_parallel.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════
 *  Multi-Threaded Splat Renderer
 *  Renders 2D Gaussian splats onto an RGB buffer.
 *  Each thread processes a horizontal strip of the output.
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    const splat2d_t *splats;
    uint32_t count;
    uint8_t *rgb;
    uint32_t width, height;
    const uint8_t *fg_mask;
    uint32_t start_row, end_row;
} splat_render_arg_t;

static void *splat_render_strip(void *arg) {
    splat_render_arg_t *a = (splat_render_arg_t *)arg;
    const splat2d_t *splats = a->splats;
    uint8_t *rgb = a->rgb;
    uint32_t width = a->width;
    const uint8_t *fg_mask = a->fg_mask;
    uint32_t row0 = a->start_row, row1 = a->end_row;

    for (uint32_t s = 0; s < a->count; s++) {
        const splat2d_t *sp = &splats[s];
        if (sp->alpha == 0) continue;

        float theta = (float)sp->theta * (3.14159265f / 255.0f);
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);

        float sx = (float)sp->sx;
        float sy = (float)sp->sy;
        if (sx < 1.0f) sx = 1.0f;
        if (sy < 1.0f) sy = 1.0f;

        float inv_sx2 = 1.0f / (sx * sx);
        float inv_sy2 = 1.0f / (sy * sy);
        float alpha_f = (float)sp->alpha / 255.0f;

        float max_sigma = (sx > sy) ? sx : sy;
        int radius = (int)ceilf(max_sigma * 3.0f);

        /* Clip to this thread's row strip */
        int x0 = (int)sp->x - radius;
        int y0 = (int)sp->y - radius;
        int x1 = (int)sp->x + radius;
        int y1 = (int)sp->y + radius;
        if (x0 < 0) x0 = 0;
        if (x1 >= (int)width) x1 = (int)width - 1;
        if (y0 < (int)row0) y0 = (int)row0;
        if (y1 >= (int)row1) y1 = (int)row1 - 1;
        if (y0 > y1 || x0 > x1) continue; /* splat doesn't touch this strip */

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                size_t idx = (size_t)py * width + px;
                if (fg_mask && fg_mask[idx] >= 2) continue;

                float dx = (float)(px - (int)sp->x);
                float dy = (float)(py - (int)sp->y);
                float lx = dx * cos_t + dy * sin_t;
                float ly = -dx * sin_t + dy * cos_t;

                float exponent = -0.5f * (lx * lx * inv_sx2 + ly * ly * inv_sy2);
                if (exponent < -4.5f) continue;

                float weight = alpha_f * expf(exponent);
                size_t pi = idx * 3;
                float inv_w = 1.0f - weight;
                rgb[pi + 0] = (uint8_t)(rgb[pi + 0] * inv_w + sp->r * weight + 0.5f);
                rgb[pi + 1] = (uint8_t)(rgb[pi + 1] * inv_w + sp->g * weight + 0.5f);
                rgb[pi + 2] = (uint8_t)(rgb[pi + 2] * inv_w + sp->b * weight + 0.5f);
            }
        }
    }
    return NULL;
}

void splat2d_render(const splat2d_t *splats, uint32_t count,
                    uint8_t *rgb, uint32_t width, uint32_t height,
                    const uint8_t *fg_mask) {
    if (!splats || !rgb || count == 0) return;

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > height) nc = (int)height;

    splat_render_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].splats = splats;
        args[t].count = count;
        args[t].rgb = rgb;
        args[t].width = width;
        args[t].height = height;
        args[t].fg_mask = fg_mask;
        args[t].start_row = (uint32_t)((uint64_t)height * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)height * (t + 1) / nc);
    }
    qtsq_dispatch(splat_render_strip, args, sizeof(splat_render_arg_t), nc);
}

/* ═══════════════════════════════════════════════════════════
 *  Background Splat Fitting
 *  Analyzes background blocks (is_fg <= 1, i.e. MEDIUM+LOW tiers)
 *  and fits 2D Gaussian splats. Tier 2+ = triangle territory.
 *  Uses clustering by color similarity.
 * ═══════════════════════════════════════════════════════════ */

/* Helper: compute average color of a rectangular region,
 * only considering background pixels. */
static void region_avg_color(const uint8_t *pixels, uint32_t channels, const uint8_t *is_fg,
                              uint32_t bw, uint32_t block_size,
                              uint32_t bx0, uint32_t by0,
                              uint32_t bx1, uint32_t by1,
                              uint32_t img_w, uint32_t img_h,
                              float *out_r, float *out_g, float *out_b,
                              float *out_cx, float *out_cy,
                              float *out_var_x, float *out_var_y,
                              uint32_t *out_count) {
    double sum_r = 0, sum_g = 0, sum_b = 0;
    double sum_x = 0, sum_y = 0;
    uint32_t n = 0;

    for (uint32_t by = by0; by <= by1; by++) {
        for (uint32_t bx = bx0; bx <= bx1; bx++) {
            if (is_fg[by * bw + bx] >= 2) continue; /* skip triangle territory (HIGH+ULTRA tiers) */

            uint32_t px0 = bx * block_size;
            uint32_t py0 = by * block_size;
            uint32_t px1 = px0 + block_size;
            uint32_t py1 = py0 + block_size;
            if (px1 > img_w) px1 = img_w;
            if (py1 > img_h) py1 = img_h;

            for (uint32_t py = py0; py < py1; py++) {
                for (uint32_t px = px0; px < px1; px++) {
                    size_t idx = ((size_t)py * img_w + px) * channels;
                    sum_r += pixels[idx];
                    sum_g += (channels >= 2) ? pixels[idx + 1] : pixels[idx];
                    sum_b += (channels >= 3) ? pixels[idx + 2] : pixels[idx];
                    sum_x += px;
                    sum_y += py;
                    n++;
                }
            }
        }
    }

    if (n == 0) {
        *out_r = *out_g = *out_b = 128.0f;
        *out_cx = (float)((bx0 + bx1) * block_size) / 2.0f;
        *out_cy = (float)((by0 + by1) * block_size) / 2.0f;
        *out_var_x = (float)block_size;
        *out_var_y = (float)block_size;
        *out_count = 0;
        return;
    }

    *out_r = (float)(sum_r / n);
    *out_g = (float)(sum_g / n);
    *out_b = (float)(sum_b / n);
    *out_cx = (float)(sum_x / n);
    *out_cy = (float)(sum_y / n);
    *out_count = n;

    /* Compute variance for sigma estimation */
    double var_x = 0, var_y = 0;
    float cx = *out_cx, cy = *out_cy;
    for (uint32_t by = by0; by <= by1; by++) {
        for (uint32_t bx = bx0; bx <= bx1; bx++) {
            if (is_fg[by * bw + bx] >= 2) continue;
            uint32_t px0 = bx * block_size;
            uint32_t py0_v = by * block_size;
            uint32_t px1 = px0 + block_size; if (px1 > img_w) px1 = img_w;
            uint32_t py1 = py0_v + block_size; if (py1 > img_h) py1 = img_h;
            for (uint32_t py = py0_v; py < py1; py++) {
                for (uint32_t px = px0; px < px1; px++) {
                    float dx = (float)px - cx;
                    float dy = (float)py - cy;
                    var_x += dx * dx;
                    var_y += dy * dy;
                }
            }
        }
    }
    *out_var_x = sqrtf((float)(var_x / n));
    *out_var_y = sqrtf((float)(var_y / n));
}

uint32_t splat2d_fit_background(const uint8_t *pixels, uint32_t width, uint32_t height,
                                 uint8_t channels, const uint8_t *is_fg,
                                 uint32_t fg_bw, uint32_t fg_bh, uint32_t block_size,
                                 splat2d_t *splats, uint32_t max_splats) {
    if (!pixels || !is_fg || !splats || max_splats == 0) return 0;

    uint32_t ns = 0;
    uint8_t *visited = (uint8_t *)calloc(fg_bw * fg_bh, 1);
    if (!visited) return 0;

    /* Cluster adjacent background blocks with similar color.
     * MAX_CLUSTER_SIZE limits growth so we get many small, evenly-distributed
     * splats instead of 3 giant ones covering sky/ground/shadow. */
    const uint32_t MAX_CLUSTER_W = 4;  /* standard 4x4 agglomeration */
    const uint32_t MAX_CLUSTER_H = 4;
    
    for (uint32_t by = 0; by < fg_bh && ns < max_splats; by++) {
        for (uint32_t bx = 0; bx < fg_bw && ns < max_splats; bx++) {
            if (is_fg[by * fg_bw + bx] >= 2 || visited[by * fg_bw + bx]) continue;
            visited[by * fg_bw + bx] = 1;

            /* Grow cluster: expand right and down while color is similar */
            uint32_t cx1 = bx, cy1 = by;

            /* Seed color from this block */
            float seed_r, seed_g, seed_b, seed_cx, seed_cy, seed_vx, seed_vy;
            uint32_t seed_cnt;
            region_avg_color(pixels, channels, is_fg, fg_bw, block_size,
                            bx, by, bx, by, width, height,
                            &seed_r, &seed_g, &seed_b, &seed_cx, &seed_cy,
                            &seed_vx, &seed_vy, &seed_cnt);

            /* Expand right (capped at MAX_CLUSTER_W blocks) */
            while (cx1 + 1 < fg_bw && (cx1 - bx + 1) < MAX_CLUSTER_W
                   && is_fg[by * fg_bw + cx1 + 1] < 2 && !visited[by * fg_bw + cx1 + 1]) {
                float nr, ng, nb, ncx, ncy, nvx, nvy; uint32_t nc;
                region_avg_color(pixels, channels, is_fg, fg_bw, block_size,
                                cx1+1, by, cx1+1, by, width, height,
                                &nr, &ng, &nb, &ncx, &ncy, &nvx, &nvy, &nc);
                float dist = fabsf(seed_r - nr) + fabsf(seed_g - ng) + fabsf(seed_b - nb);
                if (dist > 25.0f) break;  /* 25.0 luma threshold */
                visited[by * fg_bw + cx1 + 1] = 1;
                cx1++;
            }

            /* Expand down (capped at MAX_CLUSTER_H blocks) */
            while (cy1 + 1 < fg_bh && (cy1 - by + 1) < MAX_CLUSTER_H) {
                int can_expand = 1;
                for (uint32_t xx = bx; xx <= cx1; xx++) {
                    if (is_fg[(cy1+1) * fg_bw + xx] >= 2 || visited[(cy1+1) * fg_bw + xx]) {
                        can_expand = 0; break;
                    }
                }
                if (!can_expand) break;
                float nr, ng, nb, ncx, ncy, nvx, nvy; uint32_t nc;
                region_avg_color(pixels, channels, is_fg, fg_bw, block_size,
                                bx, cy1+1, cx1, cy1+1, width, height,
                                &nr, &ng, &nb, &ncx, &ncy, &nvx, &nvy, &nc);
                float dist = fabsf(seed_r - nr) + fabsf(seed_g - ng) + fabsf(seed_b - nb);
                if (dist > 25.0f) break;
                for (uint32_t xx = bx; xx <= cx1; xx++)
                    visited[(cy1+1) * fg_bw + xx] = 1;
                cy1++;
            }

            /* Create splat from cluster */
            float avg_r, avg_g, avg_b, center_x, center_y, sigma_x, sigma_y;
            uint32_t pcount;
            region_avg_color(pixels, channels, is_fg, fg_bw, block_size,
                            bx, by, cx1, cy1, width, height,
                            &avg_r, &avg_g, &avg_b, &center_x, &center_y,
                            &sigma_x, &sigma_y, &pcount);

            if (pcount == 0) continue;
            if (sigma_x < 1.0f) sigma_x = (float)((cx1 - bx + 1) * block_size) / 2.0f;
            if (sigma_y < 1.0f) sigma_y = (float)((cy1 - by + 1) * block_size) / 2.0f;

            splat2d_t *sp = &splats[ns];
            sp->x = (uint16_t)(center_x + 0.5f);
            sp->y = (uint16_t)(center_y + 0.5f);

            sp->sx = (uint8_t)(sigma_x + 0.5f);
            sp->sy = (uint8_t)(sigma_y + 0.5f);
            sp->theta = 0;  /* no rotation for grid-aligned clusters */
            sp->r = (uint8_t)(avg_r + 0.5f);
            sp->g = (uint8_t)(avg_g + 0.5f);
            sp->b = (uint8_t)(avg_b + 0.5f);
            sp->alpha = 255;  /* fully opaque backgrounds */
            ns++;
        }
    }

    /* ── Grid-based splat fallback ──
     * When clustering produces < 50 splats (e.g. because background blocks
     * are scattered), generate 1 splat per background block for even coverage.
     * This ensures smooth areas (ground, sky, walls) always get filled. */
    if (ns < 50) {
        ns = 0;  /* restart — replace sparse clusters with dense grid */
        for (uint32_t by = 0; by < fg_bh && ns < max_splats; by++) {
            for (uint32_t bx = 0; bx < fg_bw && ns < max_splats; bx++) {
                if (is_fg[by * fg_bw + bx] >= 2) continue;

                /* Sample actual pixel color at block center */
                uint32_t cx = bx * block_size + block_size / 2;
                uint32_t cy = by * block_size + block_size / 2;
                if (cx >= width) cx = width - 1;
                if (cy >= height) cy = height - 1;
                size_t pidx = ((size_t)cy * width + cx) * channels;

                splat2d_t *sp = &splats[ns];
                sp->x = (uint16_t)cx;
                sp->y = (uint16_t)cy;
                sp->sx = (uint8_t)(block_size / 2);  /* sigma = half block */
                sp->sy = (uint8_t)(block_size / 2);
                sp->theta = 0;
                sp->r = pixels[pidx];
                sp->g = (channels >= 2) ? pixels[pidx + 1] : pixels[pidx];
                sp->b = (channels >= 3) ? pixels[pidx + 2] : pixels[pidx];
                sp->alpha = 255;  /* fully opaque grid splats */
                ns++;
            }
        }
        fprintf(stderr, "[QTSQ Splat] Grid fallback: generated %u splats from background blocks\n", ns);
    }

    /* ── Foreground grid splats ──
     * Fill remaining splat capacity with 1 splat per foreground block.
     * This ensures detailed areas (textures, edges) also get splat coverage
     * instead of relying solely on the triangle mesh. */
    if (ns < max_splats) {
        uint32_t fg_added = 0;
        for (uint32_t by = 0; by < fg_bh && ns < max_splats; by++) {
            for (uint32_t bx = 0; bx < fg_bw && ns < max_splats; bx++) {
                if (is_fg[by * fg_bw + bx] < 2) continue;  /* skip background — already covered */

                uint32_t cx = bx * block_size + block_size / 2;
                uint32_t cy = by * block_size + block_size / 2;
                if (cx >= width) cx = width - 1;
                if (cy >= height) cy = height - 1;
                size_t pidx = ((size_t)cy * width + cx) * channels;

                splat2d_t *sp = &splats[ns];
                sp->x = (uint16_t)cx;
                sp->y = (uint16_t)cy;
                sp->sx = (uint8_t)(block_size / 3 + 1);  /* tighter sigma for detail */
                sp->sy = (uint8_t)(block_size / 3 + 1);
                sp->theta = 0;
                sp->r = pixels[pidx];
                sp->g = (channels >= 2) ? pixels[pidx + 1] : pixels[pidx];
                sp->b = (channels >= 3) ? pixels[pidx + 2] : pixels[pidx];
                sp->alpha = 200;  /* slightly transparent — blends with triangle mesh */
                ns++;
                fg_added++;
            }
        }
        if (fg_added > 0)
            fprintf(stderr, "[QTSQ Splat] Foreground grid: added %u splats (total %u)\n", fg_added, ns);
    }

    free(visited);
    return ns;
}

/* ═══════════════════════════════════════════════════════════
 *  Multi-Threaded Gradient Descent Splat Refinement
 *  The gradient computation per-splat is independent.
 *  Each thread processes a subset of splats.
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    splat2d_t *splats;
    uint32_t start_splat, end_splat;
    const uint8_t *target;
    const uint8_t *render;
    uint32_t width, height;
    uint8_t channels;
    const uint8_t *is_fg;
    float lr_color, lr_pos, lr_scale;
} gd_thread_arg_t;

static void *gd_update_splats(void *arg) {
    gd_thread_arg_t *a = (gd_thread_arg_t *)arg;
    uint32_t width = a->width, height = a->height;
    uint8_t channels = a->channels;
    const uint8_t *target = a->target;
    const uint8_t *render = a->render;
    const uint8_t *is_fg = a->is_fg;

    for (uint32_t s = a->start_splat; s < a->end_splat; s++) {
        splat2d_t *sp = &a->splats[s];
        if (sp->alpha == 0) continue;

        float sx = (float)sp->sx;
        float sy = (float)sp->sy;
        if (sx < 1.0f) sx = 1.0f;
        if (sy < 1.0f) sy = 1.0f;
        float inv_sx2 = 1.0f / (sx * sx);
        float inv_sy2 = 1.0f / (sy * sy);

        float max_sig = (sx > sy) ? sx : sy;
        int radius = (int)ceilf(max_sig * 2.5f);

        int x0 = (int)sp->x - radius; if (x0 < 0) x0 = 0;
        int y0 = (int)sp->y - radius; if (y0 < 0) y0 = 0;
        int x1 = (int)sp->x + radius; if (x1 >= (int)width) x1 = width - 1;
        int y1 = (int)sp->y + radius; if (y1 >= (int)height) y1 = height - 1;

        float grad_r = 0, grad_g = 0, grad_b = 0;
        float grad_x = 0, grad_y = 0;
        float grad_sx = 0, grad_sy = 0;
        float weight_sum = 0;

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                size_t idx = (size_t)py * width + px;
                if (is_fg && is_fg[idx] >= 2) continue;

                float dx = (float)(px - (int)sp->x);
                float dy = (float)(py - (int)sp->y);
                float exponent = -0.5f * (dx*dx*inv_sx2 + dy*dy*inv_sy2);
                if (exponent < -4.5f) continue;

                float w = expf(exponent);
                size_t rp = idx * 3;
                size_t tp = idx * channels;

                float err_r = (float)target[tp] - render[rp];
                float err_g = (float)(channels >= 2 ? target[tp+1] : target[tp]) - render[rp+1];
                float err_b = (float)(channels >= 3 ? target[tp+2] : target[tp]) - render[rp+2];

                grad_r += err_r * w;
                grad_g += err_g * w;
                grad_b += err_b * w;

                float err_luma = (err_r + err_g + err_b) / 3.0f;
                grad_x += err_luma * dx * w * inv_sx2;
                grad_y += err_luma * dy * w * inv_sy2;
                grad_sx += err_luma * (dx*dx) * w * inv_sx2 * (1.0f/sx);
                grad_sy += err_luma * (dy*dy) * w * inv_sy2 * (1.0f/sy);
                weight_sum += w;
            }
        }

        if (weight_sum > 0.01f) {
            float scale = 1.0f / weight_sum;
            int nr = (int)sp->r + (int)(a->lr_color * grad_r * scale);
            int ng = (int)sp->g + (int)(a->lr_color * grad_g * scale);
            int nb = (int)sp->b + (int)(a->lr_color * grad_b * scale);
            int nx = (int)sp->x + (int)(a->lr_pos * grad_x * scale);
            int ny = (int)sp->y + (int)(a->lr_pos * grad_y * scale);
            int nsx = (int)sp->sx + (int)(a->lr_scale * grad_sx * scale);
            int nsy = (int)sp->sy + (int)(a->lr_scale * grad_sy * scale);

            if (nr < 0) nr = 0; if (nr > 255) nr = 255;
            if (ng < 0) ng = 0; if (ng > 255) ng = 255;
            if (nb < 0) nb = 0; if (nb > 255) nb = 255;
            if (nx < 0) nx = 0; if (nx >= (int)width) nx = width - 1;
            if (ny < 0) ny = 0; if (ny >= (int)height) ny = height - 1;
            if (nsx < 1) nsx = 1; if (nsx > 255) nsx = 255;
            if (nsy < 1) nsy = 1; if (nsy > 255) nsy = 255;

            sp->r = (uint8_t)nr;
            sp->g = (uint8_t)ng;
            sp->b = (uint8_t)nb;
            sp->x = (uint16_t)nx;
            sp->y = (uint16_t)ny;
            sp->sx = (uint8_t)nsx;
            sp->sy = (uint8_t)nsy;
        }
    }
    return NULL;
}

void splat2d_refine_gradient_descent(splat2d_t *splats, uint32_t count,
                                      const uint8_t *target, uint32_t width, uint32_t height,
                                      uint8_t channels, const uint8_t *is_fg,
                                      int iters) {
    if (!splats || count == 0 || !target || iters <= 0) return;

    uint8_t *render = (uint8_t *)calloc((size_t)width * height, 3);
    if (!render) return;

    float lr_color = 0.6f;
    float lr_pos = 0.2f;
    float lr_scale = 0.2f;

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > count) nc = (int)count;

    for (int it = 0; it < iters; it++) {
        /* Render pass (multi-threaded via splat2d_render) */
        memset(render, 0, (size_t)width * height * 3);
        splat2d_render(splats, count, render, width, height, is_fg);

        /* Gradient + update pass (multi-threaded: split splats across cores) */
        gd_thread_arg_t gargs[QTSQ_MAX_THREADS];
        for (int t = 0; t < nc; t++) {
            gargs[t].splats = splats;
            gargs[t].start_splat = (uint32_t)((uint64_t)count * t / nc);
            gargs[t].end_splat   = (uint32_t)((uint64_t)count * (t + 1) / nc);
            gargs[t].target = target;
            gargs[t].render = render;
            gargs[t].width = width;
            gargs[t].height = height;
            gargs[t].channels = channels;
            gargs[t].is_fg = is_fg;
            gargs[t].lr_color = lr_color;
            gargs[t].lr_pos = lr_pos;
            gargs[t].lr_scale = lr_scale;
        }
        qtsq_dispatch(gd_update_splats, gargs, sizeof(gd_thread_arg_t), nc);
    }
    free(render);
}

/* ═══════════════════════════════════════════════════════════
 *  Jittered Multi-View Gradient Descent
 *
 *  Takes a SINGLE captured image and generates N jittered copies
 *  in RAM with random sub-pixel shifts (bilinear interpolation).
 *  Each GD iteration uses a different jittered view, giving the
 *  optimizer sub-pixel accuracy — equivalent to multi-frame
 *  capture from N slightly different camera positions, but from
 *  one photo with zero motion blur risk.
 *
 *  RAM cost: N × W × H × 3 bytes (25 views of 12MP ≈ 900MB)
 * ═══════════════════════════════════════════════════════════ */

/* Bilinear interpolation for sub-pixel shifted target */
static void generate_jittered_view(const uint8_t *src, uint32_t w, uint32_t h,
                                    uint8_t channels, float shift_x, float shift_y,
                                    uint8_t *dst) {
    for (uint32_t y = 0; y < h; y++) {
        float fy = (float)y + shift_y;
        int y0 = (int)fy;
        float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        /* Guard against h < 2: (int)h - 2 underflows to -1 → OOB read */
        if (h < 2) { y0 = 0; ty = 0; }
        else if (y0 >= (int)h - 1) { y0 = (int)h - 2; ty = 1.0f; }

        for (uint32_t x = 0; x < w; x++) {
            float fx = (float)x + shift_x;
            int x0 = (int)fx;
            float tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            if (w < 2) { x0 = 0; tx = 0; }
            else if (x0 >= (int)w - 1) { x0 = (int)w - 2; tx = 1.0f; }

            float omtx = 1.0f - tx, omty = 1.0f - ty;
            size_t p00 = ((size_t)y0 * w + x0) * channels;
            size_t p01 = p00 + channels;
            size_t p10 = p00 + (size_t)w * channels;
            size_t p11 = p10 + channels;
            size_t di = ((size_t)y * w + x) * channels;

            for (uint8_t c = 0; c < channels && c < 3; c++) {
                float val = src[p00+c] * omtx * omty +
                           src[p01+c] * tx   * omty +
                           src[p10+c] * omtx * ty   +
                           src[p11+c] * tx   * ty;
                int iv = (int)(val + 0.5f);
                if (iv < 0) iv = 0; if (iv > 255) iv = 255;
                dst[di + c] = (uint8_t)iv;
            }
        }
    }
}

void splat2d_refine_jittered_multiview(splat2d_t *splats, uint32_t count,
                                        const uint8_t *target, uint32_t width, uint32_t height,
                                        uint8_t channels, const uint8_t *is_fg,
                                        int iters, int num_views) {
    if (!splats || count == 0 || !target || iters <= 0) return;
    if (num_views < 1) num_views = 1;
    if (num_views > 30) num_views = 30;

    size_t frame_bytes = (size_t)width * height * channels;

    fprintf(stderr, "[QTSQ Splat] Generating %d jittered views (%.1f MB RAM)...\n",
            num_views, (float)(num_views * frame_bytes) / (1024.0f * 1024.0f));

    /* Allocate N jittered views in RAM */
    uint8_t **views = (uint8_t **)calloc(num_views, sizeof(uint8_t *));
    if (!views) {
        /* Fallback: run plain gradient descent */
        splat2d_refine_gradient_descent(splats, count, target, width, height, channels, is_fg, iters);
        return;
    }

    /* View 0 = original (no shift) */
    views[0] = (uint8_t *)malloc(frame_bytes);
    if (!views[0]) {
        free(views);
        splat2d_refine_gradient_descent(splats, count, target, width, height, channels, is_fg, iters);
        return;
    }
    memcpy(views[0], target, frame_bytes);

    /* Generate jittered views with deterministic pseudo-random shifts */
    uint32_t seed = 0xDEADBEEF;
    int alloc_ok = 1;
    for (int v = 1; v < num_views; v++) {
        views[v] = (uint8_t *)malloc(frame_bytes);
        if (!views[v]) { alloc_ok = 0; break; }

        /* Generate sub-pixel shift ±0.3 to ±0.9 px using LCG */
        seed = seed * 1664525u + 1013904223u;
        float sx = ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 1.8f;  /* ±0.9 */
        seed = seed * 1664525u + 1013904223u;
        float sy = ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 1.8f;

        generate_jittered_view(target, width, height, channels, sx, sy, views[v]);
    }

    int actual_views = alloc_ok ? num_views : 1;
    fprintf(stderr, "[QTSQ Splat] %d/%d views allocated. Running %d GD iterations...\n",
            actual_views, num_views, iters);

    /* Run gradient descent — each iteration uses a different jittered view */
    uint8_t *render = (uint8_t *)calloc((size_t)width * height, 3);
    if (!render) goto cleanup;

    float lr_color = 0.6f;
    float lr_pos = 0.2f;
    float lr_scale = 0.2f;

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > count) nc = (int)count;

    for (int it = 0; it < iters; it++) {
        /* Pick a jittered view for this iteration (round-robin) */
        const uint8_t *cur_target = views[it % actual_views];

        memset(render, 0, (size_t)width * height * 3);
        splat2d_render(splats, count, render, width, height, is_fg);

        gd_thread_arg_t gargs[QTSQ_MAX_THREADS];
        for (int t = 0; t < nc; t++) {
            gargs[t].splats = splats;
            gargs[t].start_splat = (uint32_t)((uint64_t)count * t / nc);
            gargs[t].end_splat   = (uint32_t)((uint64_t)count * (t + 1) / nc);
            gargs[t].target = cur_target;
            gargs[t].render = render;
            gargs[t].width = width;
            gargs[t].height = height;
            gargs[t].channels = channels;
            gargs[t].is_fg = is_fg;
            gargs[t].lr_color = lr_color;
            gargs[t].lr_pos = lr_pos;
            gargs[t].lr_scale = lr_scale;
        }
        qtsq_dispatch(gd_update_splats, gargs, sizeof(gd_thread_arg_t), nc);
    }

    free(render);

cleanup:
    for (int v = 0; v < num_views; v++) {
        if (views[v]) free(views[v]);
    }
    free(views);
    fprintf(stderr, "[QTSQ Splat] Jittered multi-view refinement complete.\n");
}

/* ═══════════════════════════════════════════════════════════
 *  GSP2 Binary Format Packing
 *  Planar layout for better zlib compression:
 *  "GSP2" | width(4) | height(4) | count(4) |
 *  x_lo[] | x_hi[] | y_lo[] | y_hi[] | sx[] | sy[] |
 *  theta[] | r[] | g[] | b[] | alpha[]
 * ═══════════════════════════════════════════════════════════ */

int splat2d_pack_gsp2(const splat2d_t *splats, uint32_t count,
                       uint32_t width, uint32_t height,
                       uint8_t **out_data, size_t *out_size) {
    if (!splats || !out_data || !out_size) return -1;

    /* Header: "GSP2" + width + height + count = 16 bytes */
    size_t header_sz = 4 + 4 + 4 + 4;
    /* Planar data: 2+2+1+1+1+1+1+1+1 = 11 bytes per splat */
    size_t data_sz = (size_t)count * 11;
    size_t total = header_sz + data_sz;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t p = 0;
    buf[p++] = 'G'; buf[p++] = 'S'; buf[p++] = 'P'; buf[p++] = '2';
    memcpy(buf + p, &width, 4);  p += 4;
    memcpy(buf + p, &height, 4); p += 4;
    memcpy(buf + p, &count, 4);  p += 4;

    /* Planar: x low bytes, x high bytes, y low bytes, y high bytes */
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

    *out_data = buf;
    *out_size = total;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  GSP2 Binary Format Unpacking
 * ═══════════════════════════════════════════════════════════ */

int splat2d_unpack_gsp2(const uint8_t *data, size_t size,
                         splat2d_t **out_splats, uint32_t *out_count,
                         uint32_t *out_width, uint32_t *out_height) {
    if (!data || !out_splats || !out_count) return -1;
    if (size < 16) return -1;

    /* Check magic */
    if (data[0] != 'G' || data[1] != 'S' || data[2] != 'P' || data[3] != '2')
        return -1;

    uint32_t w, h, count;
    memcpy(&w, data + 4, 4);
    memcpy(&h, data + 8, 4);
    memcpy(&count, data + 12, 4);

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;

    size_t expected = 16 + (size_t)count * 11;
    if (size < expected) return -1;

    splat2d_t *splats = (splat2d_t *)malloc(count * sizeof(splat2d_t));
    if (!splats) return -1;

    const uint8_t *p = data + 16;
    size_t stride = count;

    for (uint32_t i = 0; i < count; i++) {
        splats[i].x = (uint16_t)(p[i] | (p[stride + i] << 8));
    }
    p += stride * 2;

    for (uint32_t i = 0; i < count; i++) {
        splats[i].y = (uint16_t)(p[i] | (p[stride + i] << 8));
    }
    p += stride * 2;

    for (uint32_t i = 0; i < count; i++) splats[i].sx    = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].sy    = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].theta = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].r     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].g     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].b     = p[i]; p += stride;
    for (uint32_t i = 0; i < count; i++) splats[i].alpha = p[i]; p += stride;

    *out_splats = splats;
    *out_count = count;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  GSP3 Binary Format Packing (with depth 'z')
 *  Planar layout: x_lo, x_hi, y_lo, y_hi, sx, sy, theta, r, g, b, alpha, z
 * ═══════════════════════════════════════════════════════════ */

int splat2d_pack_gsp3(const splat2d_t *splats, uint32_t count,
                       uint32_t width, uint32_t height,
                       uint8_t **out_data, size_t *out_size) {
    if (!splats || !out_data || !out_size) return -1;

    size_t header_sz = 16;
    size_t data_sz = (size_t)count * 12; /* 12 bytes per splat now */
    size_t total = header_sz + data_sz;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t p = 0;
    buf[p++] = 'G'; buf[p++] = 'S'; buf[p++] = 'P'; buf[p++] = '3';
    memcpy(buf + p, &width, 4);  p += 4;
    memcpy(buf + p, &height, 4); p += 4;
    memcpy(buf + p, &count, 4);  p += 4;

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

/* ═══════════════════════════════════════════════════════════
 *  GSP3 Binary Format Unpacking (with depth 'z')
 * ═══════════════════════════════════════════════════════════ */

int  splat2d_unpack_gsp3(const uint8_t *data, size_t size,
                         splat2d_t **out_splats, uint32_t *out_count,
                         uint32_t *out_width, uint32_t *out_height) {
    if (!data || !out_splats || !out_count) return -1;
    if (size < 16) return -1;

    if (data[0] != 'G' || data[1] != 'S' || data[2] != 'P' || data[3] != '3')
        return -1;

    uint32_t w, h, count;
    memcpy(&w, data + 4, 4);
    memcpy(&h, data + 8, 4);
    memcpy(&count, data + 12, 4);

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;

    size_t expected = 16 + (size_t)count * 12;
    if (size < expected) return -1;

    splat2d_t *splats = (splat2d_t *)malloc(count * sizeof(splat2d_t));
    if (!splats) return -1;

    const uint8_t *p = data + 16;
    size_t stride = count;

    for (uint32_t i = 0; i < count; i++) {
        splats[i].x = (uint16_t)(p[i] | (p[stride + i] << 8));
    }
    p += stride * 2;

    for (uint32_t i = 0; i < count; i++) {
        splats[i].y = (uint16_t)(p[i] | (p[stride + i] << 8));
    }
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

/* ═══════════════════════════════════════════════════════════
 *  Estimate Splat Depth (z) from local image contrast/focus
 *  Simulates a 3D depth map from a single 2D monocular image.
 * ═══════════════════════════════════════════════════════════ */

void splat2d_estimate_depth(splat2d_t *splats, uint32_t count,
                            const uint8_t *target, uint32_t width, uint32_t height,
                            uint8_t channels) {
    if (!splats || !target) return;
    /* Strategy: Estimate local variance/contrast.
       Sharp/high-contrast = foreground (closer to 255).
       Blurry/low-contrast = background (closer to 0). */
    
    for (uint32_t i = 0; i < count; i++) {
        int cx = splats[i].x;
        int cy = splats[i].y;
        int r = splats[i].sx > splats[i].sy ? splats[i].sx : splats[i].sy;
        r = r > 10 ? 10 : r; /* limit radius for variance check */

        int min_luma = 255;
        int max_luma = 0;
        int x0 = cx - r; if (x0 < 0) x0 = 0;
        int y0 = cy - r; if (y0 < 0) y0 = 0;
        int x1 = cx + r; if (x1 >= (int)width) x1 = width - 1;
        int y1 = cy + r; if (y1 >= (int)height) y1 = height - 1;

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                size_t idx = ((size_t)y * width + x) * channels;
                int luma = target[idx];
                if (channels >= 3) {
                    luma = (target[idx]*77 + target[idx+1]*150 + target[idx+2]*29) >> 8;
                }
                if (luma < min_luma) min_luma = luma;
                if (luma > max_luma) max_luma = luma;
            }
        }
        int contrast = max_luma - min_luma;
        /* Map contrast (approx 0-100) to z depth (0-255) */
        int z = contrast * 3;
        if (z > 255) z = 255;
        
        /* Larger splats are generally blurrier -> naturally pushes them to background */
        splats[i].z = (uint8_t)z;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Resolution-Independent Render
 *
 *  Scales splat positions and sigma values from the original
 *  capture resolution to an arbitrary target resolution.
 *  Enables rendering at any DPI without re-fitting.
 * ══════════════════════════════════════════════════════════════ */

void splat2d_render_scaled(const splat2d_t *splats, uint32_t count,
                           uint32_t orig_w, uint32_t orig_h,
                           uint8_t *output, uint32_t target_w, uint32_t target_h) {
    if (!splats || !output || count == 0) return;
    if (orig_w == 0 || orig_h == 0 || target_w == 0 || target_h == 0) return;

    memset(output, 0, (size_t)target_w * target_h * 3);

    float scale_x = (float)target_w / (float)orig_w;
    float scale_y = (float)target_h / (float)orig_h;
    float scale_avg = (scale_x + scale_y) * 0.5f;

    for (uint32_t i = 0; i < count; i++) {
        const splat2d_t *sp = &splats[i];

        float cx = (float)sp->x * scale_x;
        float cy = (float)sp->y * scale_y;
        float sx = (float)sp->sx * scale_avg;
        float sy = (float)sp->sy * scale_avg;

        if (sx < 0.5f) sx = 0.5f;
        if (sy < 0.5f) sy = 0.5f;
        if (sx > 512.0f) sx = 512.0f;
        if (sy > 512.0f) sy = 512.0f;

        float alpha = (float)sp->alpha / 255.0f;
        if (alpha < 0.01f) continue;

        /* Rotation — same conversion as splat2d_render() */
        float theta = (float)sp->theta * (3.14159265f / 255.0f);
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);

        float max_r = (sx > sy) ? sx : sy;
        int32_t min_x = (int32_t)(cx - max_r * 3.0f);
        int32_t max_x = (int32_t)(cx + max_r * 3.0f);
        int32_t min_y = (int32_t)(cy - max_r * 3.0f);
        int32_t max_y = (int32_t)(cy + max_r * 3.0f);

        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x >= (int32_t)target_w) max_x = (int32_t)target_w - 1;
        if (max_y >= (int32_t)target_h) max_y = (int32_t)target_h - 1;

        float inv_sx2 = 1.0f / (sx * sx);
        float inv_sy2 = 1.0f / (sy * sy);

        for (int32_t py = min_y; py <= max_y; py++) {
            for (int32_t px = min_x; px <= max_x; px++) {
                float dx = (float)px - cx;
                float dy = (float)py - cy;

                float rx = cos_t * dx + sin_t * dy;
                float ry = -sin_t * dx + cos_t * dy;

                float exponent = -0.5f * (rx * rx * inv_sx2 + ry * ry * inv_sy2);
                if (exponent < -8.0f) continue;

                float weight = alpha * expf(exponent);
                if (weight < 0.004f) continue;

                size_t pi = ((size_t)py * target_w + px) * 3;
                float w_r = weight * (float)sp->r;
                float w_g = weight * (float)sp->g;
                float w_b = weight * (float)sp->b;

                output[pi]     = (uint8_t)((float)output[pi]     * (1.0f - weight) + w_r);
                output[pi + 1] = (uint8_t)((float)output[pi + 1] * (1.0f - weight) + w_g);
                output[pi + 2] = (uint8_t)((float)output[pi + 2] * (1.0f - weight) + w_b);
            }
        }
    }
}
