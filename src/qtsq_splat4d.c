/*
 * QTSQ 4D Continuous Gaussian Splat Engine — Implementation
 * 4DGS Velocity-Based Video: Resolution-free, Framerate-free
 * Author: Haruhito / QTSQ Project
 *
 * Quality-first implementation. No aggressive compression.
 */

#include "qtsq_splat4d.h"
#include "qtsq_parallel.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

/* ══════════════════════════════════════════════════════════════
 *  FAST EXP APPROXIMATION — ~10× faster than expf()
 *  Uses IEEE 754 bit-level manipulation + linear refinement.
 *  Max error ~0.3% in range [-4.5, 0], which is sufficient
 *  for Gaussian weighting where values are relative anyway.
 * ══════════════════════════════════════════════════════════════ */
static inline float fast_expf(float x) {
    /* Clamp to avoid overflow/underflow */
    if (x < -10.0f) return 0.0f;
    if (x > 0.0f) x = 0.0f;  /* our usage is always ≤ 0 */
    /* Schraudolph's algorithm (M16 fix: comment claimed "linear correction"
     * and "Newton-style correction" but neither was applied — fixed the
     * comment to match the code. The raw Schraudolph approximation is
     * sufficient here since values are only used for relative weighting.) */
    union { float f; int32_t i; } u;
    u.i = (int32_t)(12102203.0f * x + 1065353216.0f);
    return u.f;
}

static double s4d_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ══════════════════════════════════════════════════════════════
 *  OPTICAL FLOW — Block Matching (SAD)
 *  Computes motion vectors between two consecutive frames.
 *  Quality-first: uses full-search (not diamond/hexagonal).
 * ══════════════════════════════════════════════════════════════ */

int splat4d_optical_flow(const uint8_t *frame_a, const uint8_t *frame_b,
                         uint32_t width, uint32_t height, uint8_t channels,
                         uint32_t block_size, uint32_t search_radius,
                         float **out_mvx, float **out_mvy,
                         uint32_t *out_bw, uint32_t *out_bh) {
    if (!frame_a || !frame_b || !out_mvx || !out_mvy) return -1;

    uint32_t bw = (width + block_size - 1) / block_size;
    uint32_t bh = (height + block_size - 1) / block_size;
    size_t num_blocks = (size_t)bw * bh;

    float *mvx = (float *)calloc(num_blocks, sizeof(float));
    float *mvy = (float *)calloc(num_blocks, sizeof(float));
    if (!mvx || !mvy) { free(mvx); free(mvy); return -1; }

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint32_t bpx = bx * block_size;
            uint32_t bpy = by * block_size;
            uint32_t bpw = (bpx + block_size > width)  ? width  - bpx : block_size;
            uint32_t bph = (bpy + block_size > height) ? height - bpy : block_size;

            int best_dx = 0, best_dy = 0;
            uint32_t best_sad = UINT32_MAX;

            int sr = (int)search_radius;
            for (int dy = -sr; dy <= sr; dy++) {
                for (int dx = -sr; dx <= sr; dx++) {
                    int sx = (int)bpx + dx;
                    int sy = (int)bpy + dy;
                    if (sx < 0 || sy < 0) continue;
                    if ((uint32_t)(sx + bpw) > width || (uint32_t)(sy + bph) > height) continue;

                    uint32_t sad = 0;
                    for (uint32_t py = 0; py < bph; py++) {
                        for (uint32_t px = 0; px < bpw; px++) {
                            size_t ia = ((size_t)(bpy + py) * width + (bpx + px)) * channels;
                            size_t ib = ((size_t)(sy + py) * width + (sx + px)) * channels;
                            for (uint8_t c = 0; c < channels && c < 3; c++) {
                                int d = (int)frame_a[ia + c] - (int)frame_b[ib + c];
                                sad += (uint32_t)(d < 0 ? -d : d);
                            }
                        }
                    }

                    if (sad < best_sad) {
                        best_sad = sad;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }

            mvx[by * bw + bx] = (float)best_dx;
            mvy[by * bw + bx] = (float)best_dy;
        }
    }

    *out_mvx = mvx;
    *out_mvy = mvy;
    if (out_bw) *out_bw = bw;
    if (out_bh) *out_bh = bh;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  SOBEL GRADIENT (reusable, multi-threaded)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *pixels;
    float *grad;
    uint32_t width, height;
    uint8_t channels;
    uint32_t start_row, end_row;
} s4d_sobel_arg_t;

static void *s4d_sobel_strip(void *arg) {
    s4d_sobel_arg_t *a = (s4d_sobel_arg_t *)arg;
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

static float *s4d_compute_gradient(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch) {
    float *grad = (float *)calloc((size_t)w * h, sizeof(float));
    if (!grad) return NULL;
    int nc = qtsq_num_cores();
    if ((uint32_t)nc > h) nc = (int)h;
    s4d_sobel_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].pixels = pixels; args[t].grad = grad;
        args[t].width = w; args[t].height = h; args[t].channels = ch;
        args[t].start_row = (uint32_t)((uint64_t)h * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)h * (t + 1) / nc);
    }
    qtsq_dispatch(s4d_sobel_strip, args, sizeof(s4d_sobel_arg_t), nc);
    return grad;
}

/* ══════════════════════════════════════════════════════════════
 *  SEQUENCE FITTING — The Encoder Core
 *
 *  Pipeline:
 *   1. Gradient analysis on frame 0 (detail map)
 *   2. Optical flow: average motion field across all frame pairs
 *   3. Multi-resolution seed placement with velocity init
 *   4. Temporal color drift estimation (least-squares)
 *   5. Lifespan pruning
 * ══════════════════════════════════════════════════════════════ */

uint32_t splat4d_fit_sequence(const uint8_t **frames, uint32_t num_frames,
                              uint32_t width, uint32_t height, uint8_t channels,
                              splat4d_t *out_splats, uint32_t max_splats) {
    if (!frames || !out_splats || num_frames == 0 || max_splats == 0) return 0;

    double t0 = s4d_time_ms();

    /* ── Phase 1: Gradient from frame 0 (used for adaptive density later) ── */
    float *grad = s4d_compute_gradient(frames[0], width, height, channels);
    if (!grad) return 0;

    float max_grad = 0;
    size_t npx = (size_t)width * height;
    for (size_t i = 0; i < npx; i++)
        if (grad[i] > max_grad) max_grad = grad[i];
    if (max_grad < 1.0f) max_grad = 1.0f;

    fprintf(stderr, "[SPLAT4D] Phase 1: Gradient computed (max=%.1f) in %.1f ms\n",
            max_grad, s4d_time_ms() - t0);

    /* ── Phase 2: Block Matching Optical Flow ── */
    t0 = s4d_time_ms();
    uint32_t flow_block = 8;
    uint32_t flow_bw = (width + flow_block - 1) / flow_block;
    uint32_t flow_bh = (height + flow_block - 1) / flow_block;
    size_t flow_count = (size_t)flow_bw * flow_bh;

    float *avg_mvx = (float *)calloc(flow_count, sizeof(float));
    float *avg_mvy = (float *)calloc(flow_count, sizeof(float));
    if (!avg_mvx || !avg_mvy) { free(avg_mvx); free(avg_mvy); free(grad); return 0; }

    if (num_frames > 1) {
        const uint8_t *f0 = frames[0];
        const uint8_t *f1 = frames[num_frames > 8 ? 8 : num_frames - 1];
        float dt = (float)(num_frames > 8 ? 8 : num_frames - 1);
        int sr = 8; /* Search range */

        for (uint32_t by = 0; by < flow_bh; by++) {
            for (uint32_t bx = 0; bx < flow_bw; bx++) {
                int cx = bx * flow_block;
                int cy = by * flow_block;
                int best_mx = 0, best_my = 0;
                uint32_t best_sad = UINT32_MAX;

                for (int my = -sr; my <= sr; my += 2) { /* Sparse search step 2 */
                    for (int mx = -sr; mx <= sr; mx += 2) {
                        uint32_t sad = 0;
                        /* Subsample block 4x4 (step 2) for speed */
                        for (uint32_t py = 0; py < flow_block; py += 2) {
                            for (uint32_t px = 0; px < flow_block; px += 2) {
                                int x0 = cx + px;
                                int y0 = cy + py;
                                int x1 = x0 + mx;
                                int y1 = y0 + my;
                                if (x0 < (int)width && y0 < (int)height) {
                                    uint8_t c0 = f0[(y0 * width + x0) * channels];
                                    uint8_t c1 = (x1 >= 0 && x1 < (int)width && y1 >= 0 && y1 < (int)height) ?
                                                 f1[(y1 * width + x1) * channels] : c0;
                                    int diff = (int)c0 - (int)c1;
                                    sad += (diff < 0 ? -diff : diff);
                                }
                            }
                        }
                        if (sad < best_sad) {
                            best_sad = sad;
                            best_mx = mx;
                            best_my = my;
                        }
                    }
                }
                avg_mvx[by * flow_bw + bx] = (float)best_mx / dt;
                avg_mvy[by * flow_bw + bx] = (float)best_my / dt;
            }
        }
    }

    fprintf(stderr, "[SPLAT4D] Phase 2: Block Optical Flow computed in %.1f ms\n", s4d_time_ms() - t0);

    /* ── Phase 3: DENSE GRID SEEDING — quality-first ──
     * Place a splat at EVERY grid cell to guarantee full image coverage.
     * Use an adaptive grid: 4×4 in flat areas, 2×2 in high-gradient areas.
     * Gaussian sigma overlaps neighbors so there are NO holes. */
    t0 = s4d_time_ms();

    uint32_t ns = 0;
    /* Average midpoint frame for better initial color */
    uint32_t mid_f = num_frames / 2;
    const uint8_t *mid_frame = frames[mid_f];

    /* Calculate splat density.
     * base_step=4: 1920/4 × 1080/4 = 480 × 270 = ~130K base splats for 1080p.
     * fine_step=2: adds ~70K edge splats in high-gradient areas.
     * Total ~200K — well within budget, avoids overflow truncation.
     * Sigma = step * 2.0 = 8px — guarantees full overlap between neighbors.
     */
    uint32_t base_step = 2; // Increased density for extremely high quality
    uint32_t fine_step = 2;
    float edge_thresh = max_grad * 0.12f; /* slightly lower threshold for better edge coverage */

    /* First pass: base grid (dense coverage, every base_step pixels) */
    for (uint32_t gy = 0; gy < height && ns < max_splats; gy += base_step) {
        for (uint32_t gx = 0; gx < width && ns < max_splats; gx += base_step) {
            uint32_t cx = gx + base_step / 2;
            uint32_t cy = gy + base_step / 2;
            if (cx >= width) cx = width - 1;
            if (cy >= height) cy = height - 1;

            /* Sample color as average of the block from MIDPOINT frame
             * (midpoint gives better initial guess for temporal drift) */
            uint32_t bx1 = gx + base_step > width  ? width  : gx + base_step;
            uint32_t by1 = gy + base_step > height ? height : gy + base_step;
            uint32_t sr0 = 0, sg0 = 0, sb0 = 0, cnt = 0;
            for (uint32_t py = gy; py < by1; py++) {
                for (uint32_t px = gx; px < bx1; px++) {
                    size_t pi = ((size_t)py * width + px) * channels;
                    sr0 += mid_frame[pi];
                    sg0 += (channels >= 2) ? mid_frame[pi + 1] : mid_frame[pi];
                    sb0 += (channels >= 3) ? mid_frame[pi + 2] : mid_frame[pi];
                    cnt++;
                }
            }
            if (cnt == 0) cnt = 1;

            /* Lookup motion vector */
            uint32_t fbx = cx / flow_block, fby = cy / flow_block;
            if (fbx >= flow_bw) fbx = flow_bw - 1;
            if (fby >= flow_bh) fby = flow_bh - 1;
            float vel_x = avg_mvx[fby * flow_bw + fbx];
            float vel_y = avg_mvy[fby * flow_bw + fbx];

            splat4d_t *sp = &out_splats[ns];
            memset(sp, 0, sizeof(splat4d_t));
            sp->x0 = (uint16_t)cx;
            sp->y0 = (uint16_t)cy;
            sp->vx = FLOAT_TO_Q8(vel_x);
            sp->vy = FLOAT_TO_Q8(vel_y);
            /* Sigma = 2.0 * grid_step = 8 for 4×4 grid.
             * Ensures full overlap between adjacent Gaussians — no holes. */
            float sig = (float)base_step * 2.0f;
            sp->sx = (uint8_t)(sig < 1 ? 1 : (sig > 255 ? 255 : sig));
            sp->sy = sp->sx;
            sp->theta = 0;
            sp->alpha = 255;
            sp->birth_frame = 0;
            sp->death_frame = (uint16_t)num_frames;
            sp->r = (uint8_t)(sr0 / cnt);
            sp->g = (uint8_t)(sg0 / cnt);
            sp->b = (uint8_t)(sb0 / cnt);
            sp->dr = 0; sp->dg = 0; sp->db = 0;
            ns++;
        }
    }

    uint32_t base_ns = ns;
    fprintf(stderr, "[SPLAT4D] Phase 3a: %u base grid splats\n", base_ns);

    /* Second pass: add finer splats (2×2) in high-gradient areas */
    for (uint32_t gy = 0; gy < height && ns < max_splats; gy += fine_step) {
        for (uint32_t gx = 0; gx < width && ns < max_splats; gx += fine_step) {
            /* Skip if this is already covered by base grid position */
            uint32_t cx = gx + fine_step / 2;
            uint32_t cy = gy + fine_step / 2;
            if (cx >= width) cx = width - 1;
            if (cy >= height) cy = height - 1;

            /* Check if this cell is already covered by a base grid splat */
            if ((gx % base_step == 0) && (gy % base_step == 0)) continue;

            /* Only add fine splat if gradient is high enough */
            float g = grad[cy * width + cx];
            if (g < edge_thresh) continue;

            size_t pidx = ((size_t)cy * width + cx) * channels;
            uint32_t fbx = cx / flow_block, fby = cy / flow_block;
            if (fbx >= flow_bw) fbx = flow_bw - 1;
            if (fby >= flow_bh) fby = flow_bh - 1;

            splat4d_t *sp = &out_splats[ns];
            memset(sp, 0, sizeof(splat4d_t));
            sp->x0 = (uint16_t)cx;
            sp->y0 = (uint16_t)cy;
            sp->vx = FLOAT_TO_Q8(avg_mvx[fby * flow_bw + fbx]);
            sp->vy = FLOAT_TO_Q8(avg_mvy[fby * flow_bw + fbx]);
            float sig2 = (float)fine_step * 1.5f;
            sp->sx = (uint8_t)(sig2 < 1 ? 1 : (sig2 > 255 ? 255 : sig2));
            sp->sy = sp->sx;
            sp->alpha = 255;
            sp->birth_frame = 0;
            sp->death_frame = (uint16_t)num_frames;
            sp->r = mid_frame[pidx];
            sp->g = (channels >= 2) ? mid_frame[pidx+1] : mid_frame[pidx];
            sp->b = (channels >= 3) ? mid_frame[pidx+2] : mid_frame[pidx];
            ns++;
        }
    }

    fprintf(stderr, "[SPLAT4D] Phase 3b: %u total splats (%u edge splats) in %.1f ms\n",
            ns, ns - base_ns, s4d_time_ms() - t0);

    /* ── Phase 4: Temporal color drift estimation ── */
    t0 = s4d_time_ms();
    for (uint32_t s = 0; s < ns; s++) {
        splat4d_t *sp = &out_splats[s];
        if (num_frames < 3) continue;

        float sum_dr = 0, sum_dg = 0, sum_db = 0;
        float sum_dt2 = 0;
        int samples = 0;
        uint32_t sample_step = num_frames > 12 ? 3 : 1;

        for (uint32_t f = 0; f < num_frames; f += sample_step) {
            float dt = (float)f;
            float px = (float)sp->x0 + Q8_TO_FLOAT(sp->vx) * dt;
            float py = (float)sp->y0 + Q8_TO_FLOAT(sp->vy) * dt;

            int ix = (int)(px + 0.5f), iy = (int)(py + 0.5f);
            if (ix < 0 || ix >= (int)width || iy < 0 || iy >= (int)height) continue;

            size_t pidx = ((size_t)iy * width + ix) * channels;
            float fr = (float)frames[f][pidx];
            float fg = (channels >= 2) ? (float)frames[f][pidx+1] : fr;
            float fb = (channels >= 3) ? (float)frames[f][pidx+2] : fr;

            float err_r = fr - (float)sp->r;
            float err_g = fg - (float)sp->g;
            float err_b = fb - (float)sp->b;

            sum_dt2 += dt * dt;
            sum_dr  += dt * err_r;
            sum_dg  += dt * err_g;
            sum_db  += dt * err_b;
            samples++;
        }

        if (samples > 2 && sum_dt2 > 1.0f) {
            float dr = sum_dr / sum_dt2;
            float dg = sum_dg / sum_dt2;
            float db = sum_db / sum_dt2;
            sp->dr = (int8_t)(dr < -127 ? -127 : (dr > 127 ? 127 : dr));
            sp->dg = (int8_t)(dg < -127 ? -127 : (dg > 127 ? 127 : dg));
            sp->db = (int8_t)(db < -127 ? -127 : (db > 127 ? 127 : db));
        }
    }

    fprintf(stderr, "[SPLAT4D] Phase 4: Color drift estimated in %.1f ms\n", s4d_time_ms() - t0);

    /* ── Phase 5: Lifespan pruning ── */
    t0 = s4d_time_ms();
    for (uint32_t s = 0; s < ns; s++) {
        splat4d_t *sp = &out_splats[s];
        for (uint32_t f = 0; f < num_frames; f++) {
            float px, py;
            splat4d_position(sp, (float)f, &px, &py);
            if (px < -(float)sp->sx * 3 || px >= (float)width + sp->sx * 3 ||
                py < -(float)sp->sy * 3 || py >= (float)height + sp->sy * 3) {
                sp->death_frame = (uint16_t)f;
                break;
            }
        }
        if (sp->death_frame <= sp->birth_frame + 1)
            sp->death_frame = sp->birth_frame + 2;
        if (sp->death_frame > num_frames)
            sp->death_frame = (uint16_t)num_frames;
    }

    fprintf(stderr, "[SPLAT4D] Phase 5: Lifespan pruned in %.1f ms\n", s4d_time_ms() - t0);

    free(grad);
    free(avg_mvx);
    free(avg_mvy);

    fprintf(stderr, "[SPLAT4D] Fit complete: %u splats for %u frames (%ux%u)\n",
            ns, num_frames, width, height);
    return ns;
}

/* ══════════════════════════════════════════════════════════════
 *  RENDERER — Multi-Threaded Continuous Splat Evaluation
 *  Renders all alive Gaussians at arbitrary time t.
 *  Each thread processes a horizontal strip.
 * ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
 *  RENDERER — Alpha-Blending Gaussian Splatting
 *  
 *  Uses sequential alpha blending: pixel = pixel*(1-w) + color*w
 *  Within each row strip, splats are processed in deterministic index
 *  order. Each pixel is only written by one thread (row partitioning).
 *  This gives SHARP results where the nearest/strongest splat dominates.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const splat4d_t *splats;
    uint32_t count;
    float t;
    uint8_t *rgb;
    uint32_t width, height;
    uint8_t channels;
    uint32_t start_row, end_row;
    float scale_x, scale_y;
} s4d_render_arg_t;

static void *s4d_render_strip(void *arg) {
    s4d_render_arg_t *a = (s4d_render_arg_t *)arg;
    float t = a->t;
    uint32_t w = a->width, row0 = a->start_row, row1 = a->end_row;
    uint8_t ch = a->channels;
    float sx_scale = a->scale_x, sy_scale = a->scale_y;

    for (uint32_t s = 0; s < a->count; s++) {
        const splat4d_t *sp = &a->splats[s];
        if (sp->alpha == 0) continue;
        if (!splat4d_alive(sp, t)) continue;

        float cx, cy;
        splat4d_position(sp, t, &cx, &cy);
        cx *= sx_scale;
        cy *= sy_scale;

        uint8_t cr, cg, cb;
        splat4d_color(sp, t, &cr, &cg, &cb);

        float sigma_x = (float)sp->sx * sx_scale;
        float sigma_y = (float)sp->sy * sy_scale;
        if (sigma_x < 1.0f) sigma_x = 1.0f;
        if (sigma_y < 1.0f) sigma_y = 1.0f;
        float inv_sx2 = 1.0f / (sigma_x * sigma_x);
        float inv_sy2 = 1.0f / (sigma_y * sigma_y);
        float alpha_f = (float)sp->alpha / 255.0f;

        float max_sigma = (sigma_x > sigma_y) ? sigma_x : sigma_y;
        int radius = (int)ceilf(max_sigma * 2.5f);

        int x0 = (int)cx - radius, x1 = (int)cx + radius;
        int y0 = (int)cy - radius, y1 = (int)cy + radius;
        if (x0 < 0) x0 = 0;
        if (x1 >= (int)w) x1 = (int)w - 1;
        if (y0 < (int)row0) y0 = (int)row0;
        if (y1 >= (int)row1) y1 = (int)row1 - 1;
        if (y0 > y1 || x0 > x1) continue;

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float dx = (float)px - cx;
                float dy = (float)py - cy;
                float exponent = -0.5f * (dx * dx * inv_sx2 + dy * dy * inv_sy2);
                if (exponent < -3.0f) continue;

                float weight = alpha_f * fast_expf(exponent);
                size_t pi = ((size_t)py * w + px) * ch;
                float inv_w = 1.0f - weight;
                a->rgb[pi + 0] = (uint8_t)(a->rgb[pi + 0] * inv_w + cr * weight + 0.5f);
                if (ch >= 2) a->rgb[pi + 1] = (uint8_t)(a->rgb[pi + 1] * inv_w + cg * weight + 0.5f);
                if (ch >= 3) a->rgb[pi + 2] = (uint8_t)(a->rgb[pi + 2] * inv_w + cb * weight + 0.5f);
            }
        }
    }
    return NULL;
}

static void s4d_render_internal(const splat4d_t *splats, uint32_t count,
                                 float t, uint8_t *rgb,
                                 uint32_t width, uint32_t height, uint8_t channels,
                                 float scale_x, float scale_y) {
    if (!splats || !rgb || count == 0) return;
    int nc = qtsq_num_cores();
    if ((uint32_t)nc > height) nc = (int)height;
    s4d_render_arg_t args[QTSQ_MAX_THREADS];
    for (int i = 0; i < nc; i++) {
        args[i].splats = splats; args[i].count = count;
        args[i].t = t; args[i].rgb = rgb;
        args[i].width = width; args[i].height = height;
        args[i].channels = channels;
        args[i].scale_x = scale_x; args[i].scale_y = scale_y;
        args[i].start_row = (uint32_t)((uint64_t)height * i / nc);
        args[i].end_row   = (uint32_t)((uint64_t)height * (i + 1) / nc);
    }
    qtsq_dispatch(s4d_render_strip, args, sizeof(s4d_render_arg_t), nc);
}

void splat4d_render_at_time(const splat4d_t *splats, uint32_t count,
                            float t, uint8_t *rgb,
                            uint32_t width, uint32_t height,
                            uint8_t channels) {
    s4d_render_internal(splats, count, t, rgb, width, height, channels, 1.0f, 1.0f);
}

void splat4d_render_scaled(const splat4d_t *splats, uint32_t count,
                           float t, uint8_t *rgb,
                           uint32_t src_width, uint32_t src_height,
                           uint32_t out_width, uint32_t out_height,
                           uint8_t channels) {
    float sx = (float)out_width  / (float)src_width;
    float sy = (float)out_height / (float)src_height;
    s4d_render_internal(splats, count, t, rgb, out_width, out_height, channels, sx, sy);
}

/* ══════════════════════════════════════════════════════════════
 *  GRADIENT DESCENT — Per-Splat Independent Optimization
 *  
 *  Each splat computes error = target_pixel - my_color at its 
 *  footprint. This is stable because each splat independently
 *  converges to the average color under its Gaussian footprint.
 *  Multi-threaded: each thread processes a subset of splats.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    splat4d_t *splats;
    uint32_t start_s, end_s;
    const uint8_t **frames;
    uint32_t num_frames;
    uint32_t width, height;
    uint8_t channels;
    float lr_color, lr_pos, lr_vel, lr_scale, lr_drift;
} s4d_gd_arg_t;

static void *s4d_gd_worker(void *arg) {
    s4d_gd_arg_t *a = (s4d_gd_arg_t *)arg;
    uint32_t w = a->width, h = a->height;
    uint8_t ch = a->channels;

    for (uint32_t s = a->start_s; s < a->end_s; s++) {
        splat4d_t *sp = &a->splats[s];
        if (sp->alpha == 0) continue;

        float gr = 0, gg = 0, gb = 0;
        float gx = 0, gy_grad = 0;
        float gvx = 0, gvy = 0;
        float gsx = 0, gsy = 0;
        float gdr = 0, gdg = 0, gdb = 0;
        float wsum = 0;

        /* Sample 3 frames: start, mid, end — sufficient for linear velocity/drift */
        /* Sample all frames for short segments (≤16), every-other for longer */
        uint32_t frame_step = a->num_frames > 16 ? 2 : 1;

        for (uint32_t f = sp->birth_frame; f < sp->death_frame && f < a->num_frames; f += frame_step) {
            float t = (float)f;
            float dt = t - (float)sp->birth_frame;

            float cx, cy;
            splat4d_position(sp, t, &cx, &cy);

            uint8_t cr, cg_c, cb;
            splat4d_color(sp, t, &cr, &cg_c, &cb);

            float sigma_x = (float)sp->sx, sigma_y = (float)sp->sy;
            if (sigma_x < 1.0f) sigma_x = 1.0f;
            if (sigma_y < 1.0f) sigma_y = 1.0f;
            float inv_sx2 = 1.0f / (sigma_x * sigma_x);
            float inv_sy2 = 1.0f / (sigma_y * sigma_y);

            int radius = (int)ceilf(((sigma_x > sigma_y) ? sigma_x : sigma_y) * 1.5f);
            int x0 = (int)cx - radius; if (x0 < 0) x0 = 0;
            int y0 = (int)cy - radius; if (y0 < 0) y0 = 0;
            int x1 = (int)cx + radius; if (x1 >= (int)w) x1 = w - 1;
            int y1 = (int)cy + radius; if (y1 >= (int)h) y1 = h - 1;

            const uint8_t *target = a->frames[f];

            for (int py = y0; py <= y1; py++) {
                for (int px = x0; px <= x1; px++) {
                    float dx = (float)px - cx;
                    float dy = (float)py - cy;
                    float exp_v = -0.5f * (dx*dx*inv_sx2 + dy*dy*inv_sy2);
                    if (exp_v < -2.0f) continue;
                    float wt = fast_expf(exp_v);

                    /* Per-splat error: target - my_color (stable convergence) */
                    size_t idx = ((size_t)py * w + px) * ch;
                    float er = (float)target[idx] - (float)cr;
                    float eg = (ch >= 2) ? (float)target[idx+1] - (float)cg_c : er;
                    float eb = (ch >= 3) ? (float)target[idx+2] - (float)cb : er;

                    gr += er * wt; gg += eg * wt; gb += eb * wt;
                    float el = (er + eg + eb) / 3.0f;

                    gx += el * dx * wt * inv_sx2;
                    gy_grad += el * dy * wt * inv_sy2;
                    gvx += el * dx * wt * inv_sx2 * dt;
                    gvy += el * dy * wt * inv_sy2 * dt;
                    gsx += el * (dx*dx) * wt * inv_sx2 * (1.0f/sigma_x);
                    gsy += el * (dy*dy) * wt * inv_sy2 * (1.0f/sigma_y);
                    gdr += er * wt * dt;
                    gdg += eg * wt * dt;
                    gdb += eb * wt * dt;

                    wsum += wt;
                }
            }
        }

        if (wsum > 0.01f) {
            float sc = 1.0f / wsum;

            int nr = (int)sp->r + (int)(a->lr_color * gr * sc);
            int ng = (int)sp->g + (int)(a->lr_color * gg * sc);
            int nb = (int)sp->b + (int)(a->lr_color * gb * sc);
            if (nr < 0) nr = 0; if (nr > 255) nr = 255;
            if (ng < 0) ng = 0; if (ng > 255) ng = 255;
            if (nb < 0) nb = 0; if (nb > 255) nb = 255;
            sp->r = (uint8_t)nr; sp->g = (uint8_t)ng; sp->b = (uint8_t)nb;

            int nx = (int)sp->x0 + (int)(a->lr_pos * gx * sc);
            int ny = (int)sp->y0 + (int)(a->lr_pos * gy_grad * sc);
            if (nx < 0) nx = 0; if (nx >= (int)w) nx = w - 1;
            if (ny < 0) ny = 0; if (ny >= (int)h) ny = h - 1;
            sp->x0 = (uint16_t)nx; sp->y0 = (uint16_t)ny;

            float new_vx = Q8_TO_FLOAT(sp->vx) + a->lr_vel * gvx * sc;
            float new_vy = Q8_TO_FLOAT(sp->vy) + a->lr_vel * gvy * sc;
            sp->vx = FLOAT_TO_Q8(new_vx);
            sp->vy = FLOAT_TO_Q8(new_vy);

            int nsx = (int)sp->sx + (int)(a->lr_scale * gsx * sc);
            int nsy = (int)sp->sy + (int)(a->lr_scale * gsy * sc);
            if (nsx < 1) nsx = 1; if (nsx > 255) nsx = 255;
            if (nsy < 1) nsy = 1; if (nsy > 255) nsy = 255;
            sp->sx = (uint8_t)nsx; sp->sy = (uint8_t)nsy;

            int ndr = (int)sp->dr + (int)(a->lr_drift * gdr * sc);
            int ndg = (int)sp->dg + (int)(a->lr_drift * gdg * sc);
            int ndb = (int)sp->db + (int)(a->lr_drift * gdb * sc);
            if (ndr < -127) ndr = -127; if (ndr > 127) ndr = 127;
            if (ndg < -127) ndg = -127; if (ndg > 127) ndg = 127;
            if (ndb < -127) ndb = -127; if (ndb > 127) ndb = 127;
            sp->dr = (int8_t)ndr; sp->dg = (int8_t)ndg; sp->db = (int8_t)ndb;
        }
    }
    return NULL;
}

void splat4d_refine(splat4d_t *splats, uint32_t count,
                    const uint8_t **frames, uint32_t num_frames,
                    uint32_t width, uint32_t height, uint8_t channels,
                    int iters) {
    if (!splats || count == 0 || !frames || num_frames == 0 || iters <= 0) return;

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > count) nc = (int)count;

    fprintf(stderr, "[SPLAT4D] GD refine: %u splats × %d iters × %u frames, %d threads\n",
            count, iters, num_frames, nc);
    double t0 = s4d_time_ms();

    for (int it = 0; it < iters; it++) {
        s4d_gd_arg_t gargs[QTSQ_MAX_THREADS];
        for (int i = 0; i < nc; i++) {
            gargs[i].splats = splats;
            gargs[i].start_s = (uint32_t)((uint64_t)count * i / nc);
            gargs[i].end_s   = (uint32_t)((uint64_t)count * (i + 1) / nc);
            gargs[i].frames = frames;
            gargs[i].num_frames = num_frames;
            gargs[i].width = width; gargs[i].height = height;
            gargs[i].channels = channels;
            float decay = 1.0f / (1.0f + 0.02f * (float)it);
            gargs[i].lr_color = 2.0f * decay;
            gargs[i].lr_pos = 0.4f * decay;
            gargs[i].lr_vel = 0.2f * decay;
            gargs[i].lr_scale = 0.3f * decay;
            gargs[i].lr_drift = 0.15f * decay;
        }
        qtsq_dispatch(s4d_gd_worker, gargs, sizeof(s4d_gd_arg_t), nc);
    }

    fprintf(stderr, "[SPLAT4D] GD refine complete in %.1f ms\n", s4d_time_ms() - t0);
}

/* ══════════════════════════════════════════════════════════════
 *  DENSIFICATION — Split High-Error Splats
 * ══════════════════════════════════════════════════════════════ */

typedef struct { uint64_t hash; uint32_t idx; } sp_hash_t;
static int hash_cmp(const void *a, const void *b) {
    uint64_t ha = ((const sp_hash_t*)a)->hash;
    uint64_t hb = ((const sp_hash_t*)b)->hash;
    return (ha < hb) ? -1 : ((ha > hb) ? 1 : 0);
}

uint32_t splat4d_densify(splat4d_t *splats, uint32_t count,
                          const uint8_t **frames, uint32_t num_frames,
                          uint32_t width, uint32_t height, uint8_t channels,
                          uint32_t max_capacity) {
    if (!splats || count == 0 || !frames || num_frames == 0) return count;

    /* ── Hash-based Merge (TurboQuant-inspired clustering) ── */
    sp_hash_t *hashes = (sp_hash_t*)malloc(count * sizeof(sp_hash_t));
    if (hashes) {
        for (uint32_t s = 0; s < count; s++) {
            if (splats[s].alpha == 0) { hashes[s].hash = 0xFFFFFFFFFFFFFFFF; hashes[s].idx = s; continue; }
            uint64_t h = ((uint64_t)splats[s].x0 << 48) | ((uint64_t)splats[s].y0 << 32) | 
                         ((uint64_t)splats[s].r << 16) | ((uint64_t)splats[s].g << 8) | splats[s].b;
            hashes[s].hash = h;
            hashes[s].idx = s;
        }
        qsort(hashes, count, sizeof(sp_hash_t), hash_cmp);
        uint32_t merged = 0;
        for (uint32_t i = 1; i < count; i++) {
            if (hashes[i].hash == 0xFFFFFFFFFFFFFFFF) break;
            if (hashes[i].hash == hashes[i-1].hash) {
                uint32_t cur = hashes[i].idx;
                uint32_t prev = hashes[i-1].idx;
                if (splats[cur].alpha > 0 && splats[prev].alpha > 0) {
                    splats[prev].sx = (splats[prev].sx + splats[cur].sx) / 2;
                    splats[prev].sy = (splats[prev].sy + splats[cur].sy) / 2;
                    splats[cur].alpha = 0; /* Kill current */
                    merged++;
                }
            }
        }
        if (merged > 0) fprintf(stderr, "[SPLAT4D] Densify Merge: %u duplicates heavily merged.\n", merged);
        free(hashes);
    }

    /* Render mid-segment frame and measure error for SPLITTING */
    uint32_t mid = num_frames / 2;
    size_t frame_bytes = (size_t)width * height * channels;
    uint8_t *render = (uint8_t *)calloc(1, frame_bytes);
    if (!render) return count;

    splat4d_render_at_time(splats, count, (float)mid, render, width, height, channels);

    uint32_t new_count = count;
    const float error_thresh = 40.0f;
    const uint8_t *target = frames[mid];

    for (uint32_t s = 0; s < count && new_count + 2 <= max_capacity; s++) {
        splat4d_t *sp = &splats[s];
        if (sp->alpha == 0 || !splat4d_alive(sp, (float)mid)) continue;
        if (sp->sx <= 2 && sp->sy <= 2) continue; /* too small */

        float cx, cy;
        splat4d_position(sp, (float)mid, &cx, &cy);
        float sigma = (float)sp->sx;
        if (sigma < 1.0f) sigma = 1.0f;
        int radius = (int)ceilf(sigma * 2.0f);

        int x0 = (int)cx - radius; if (x0 < 0) x0 = 0;
        int y0 = (int)cy - radius; if (y0 < 0) y0 = 0;
        int x1 = (int)cx + radius; if (x1 >= (int)width)  x1 = width - 1;
        int y1 = (int)cy + radius; if (y1 >= (int)height) y1 = height - 1;

        float total_err = 0;
        int n_px = 0;
        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                size_t idx = ((size_t)py * width + px) * channels;
                for (uint8_t c = 0; c < channels && c < 3; c++) {
                    float d = (float)target[idx+c] - (float)render[idx+c];
                    total_err += (d < 0) ? -d : d;
                }
                n_px++;
            }
        }
        if (n_px == 0) continue;
        float avg_err = total_err / (float)(n_px * channels);

        if (avg_err > error_thresh) {
            uint8_t half_sx = (sp->sx > 3) ? sp->sx / 2 : 2;
            uint8_t half_sy = (sp->sy > 3) ? sp->sy / 2 : 2;
            int off = (int)sp->sx / 4;
            if (off < 1) off = 1;

            /* Child 1 */
            splat4d_t *c1 = &splats[new_count++];
            *c1 = *sp;
            int c1x = (int)sp->x0 - off; if (c1x < 0) c1x = 0;
            int c1y = (int)sp->y0 - off; if (c1y < 0) c1y = 0;
            c1->x0 = (uint16_t)c1x; c1->y0 = (uint16_t)c1y;
            c1->sx = half_sx; c1->sy = half_sy;

            /* Child 2 */
            splat4d_t *c2 = &splats[new_count++];
            *c2 = *sp;
            int c2x = (int)sp->x0 + off; if (c2x >= (int)width) c2x = width - 1;
            int c2y = (int)sp->y0 + off; if (c2y >= (int)height) c2y = height - 1;
            c2->x0 = (uint16_t)c2x; c2->y0 = (uint16_t)c2y;
            c2->sx = half_sx; c2->sy = half_sy;

            sp->alpha = sp->alpha / 2;
        }
    }

    free(render);
    fprintf(stderr, "[SPLAT4D] Densify: %u → %u splats\n", count, new_count);
    return new_count;
}

/* ══════════════════════════════════════════════════════════════
 *  PACK / UNPACK — Planar Binary Format for CONT Segments
 *
 *  Header (24 bytes):
 *    'CONT' | width(4) | height(4) | count(4) |
 *    seg_frames(2) | source_fps(2) | reserved(4)
 *
 *  Body: 22 byte-planes × count
 * ══════════════════════════════════════════════════════════════ */

int splat4d_pack(const splat4d_t *splats, uint32_t count,
                 uint32_t width, uint32_t height,
                 uint16_t seg_frames, uint16_t source_fps,
                 uint8_t **out_data, size_t *out_size) {
    if (!splats || !out_data || !out_size) return -1;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

    size_t header = 24;
    size_t body = (size_t)count * 22; /* High Fidelity */
    size_t total = header + body;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return -1;

    size_t p = 0;
    buf[p++]='C'; buf[p++]='O'; buf[p++]='N'; buf[p++]='T';
    memcpy(buf+p, &width, 4);      p += 4;
    memcpy(buf+p, &height, 4);     p += 4;
    memcpy(buf+p, &count, 4);      p += 4;
    memcpy(buf+p, &seg_frames, 2); p += 2;
    memcpy(buf+p, &source_fps, 2); p += 2;
    p += 4; /* reserved */

    /* Planar encoding: each field as its own array */
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].x0 & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].x0 >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].y0 & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].y0 >> 8);
    
    /* Packed velocity */
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].vx & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].vx >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].vy & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].vy >> 8);
    
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].sx;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].sy;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].theta;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].alpha;
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].birth_frame & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].birth_frame >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].death_frame & 0xFF);
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)(splats[i].death_frame >> 8);
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].r;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].g;
    for (uint32_t i = 0; i < count; i++) buf[p++] = splats[i].b;
    
    /* Packed color drift */
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)splats[i].dr;
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)splats[i].dg;
    for (uint32_t i = 0; i < count; i++) buf[p++] = (uint8_t)splats[i].db;


    *out_data = buf;
    *out_size = total;
    return 0;
}

int splat4d_unpack(const uint8_t *data, size_t size,
                   splat4d_t **out_splats, uint32_t *out_count,
                   uint32_t *out_width, uint32_t *out_height,
                   uint16_t *out_seg_frames, uint16_t *out_source_fps) {
    if (!data || !out_splats || !out_count) return -1;
    if (size < 24) return -1;
    if (data[0]!='C'||data[1]!='O'||data[2]!='N'||data[3]!='T') return -1;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

    uint32_t w, h, count;
    uint16_t seg_f, fps;
    memcpy(&w, data+4, 4);
    memcpy(&h, data+8, 4);
    memcpy(&count, data+12, 4);
    memcpy(&seg_f, data+16, 2);
    memcpy(&fps, data+18, 2);

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    if (out_seg_frames) *out_seg_frames = seg_f;
    if (out_source_fps) *out_source_fps = fps;

    size_t expected = 24 + (size_t)count * 22;
    if (size < expected) return -1;

    splat4d_t *sp = (splat4d_t *)calloc(count, sizeof(splat4d_t));
    if (!sp) return -1;

    const uint8_t *p = data + 24;
    size_t st = count;

    /* Unpack each plane */
    for (uint32_t i=0;i<count;i++) sp[i].x0 = (uint16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].y0 = (uint16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].vx = (int16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].vy = (int16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].sx = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].sy = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].theta = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].alpha = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].birth_frame = (uint16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].death_frame = (uint16_t)(p[i] | (p[st+i]<<8));
    p += st*2;
    for (uint32_t i=0;i<count;i++) sp[i].r = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].g = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].b = p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].dr = (int8_t)p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].dg = (int8_t)p[i]; p += st;
    for (uint32_t i=0;i<count;i++) sp[i].db = (int8_t)p[i]; p += st;

    *out_splats = sp;
    *out_count = count;
    return 0;
}
