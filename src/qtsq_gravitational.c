/*
 * qtsq_gravitational.c — Triangle RGB Mesh Image Compression
 *
 * Novel image compression using adaptive Delaunay triangulation:
 *   1. Detect edges (Sobel gradient) → place vertices densely at detail
 *   2. Grid vertices for baseline coverage
 *   3. Bowyer-Watson Delaunay triangulation
 *   4. Per-vertex RGB colors
 *   5. Iterative refinement: split high-error triangles
 *   6. Decode: rasterize triangles with barycentric RGB interpolation
 *
 * The triangle mesh naturally handles:
 *   - Smooth gradients (large triangles = efficient)
 *   - Sharp edges (small triangles = precise)
 *   - Compact storage (7 bytes per vertex, 6 bytes per triangle)
 *
 * Author: Haruhito / QTSQ Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include "qtsq_splat2d.h"
#include "qtsq_parallel.h"
#include "qtsq_codec.h"
#include "qtsq.h"
#include <time.h>
#ifdef __APPLE__
#include "qtsq_metal.h"
#endif
#ifdef HAVE_VULKAN_COMPUTE
#include "vulkan_compute.h"
#endif
#ifdef __ANDROID__
#include <android/log.h>
#define QTSQ_LOG_TAG "QTSQ"

/* Global GPU/CPU toggle — set from JNI. 1=GPU (Vulkan), 0=CPU (rasterize_strip) */
int qtsq_use_gpu = 1;
#define QTSQ_TLOG(...) __android_log_print(ANDROID_LOG_INFO, "QTSQ_HD", __VA_ARGS__)
#else
/* No Vulkan on macOS/iOS — always use CPU path */
int qtsq_use_gpu = 0;
#define QTSQ_TLOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static double qtsq_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ══════════════════════════════════════════════════════════════
 *  Triangle Mesh Structures (Exposed in qtsq.h)
 * ══════════════════════════════════════════════════════════════ */

/* For Bowyer-Watson: circumcircle */
typedef struct {
    double cx, cy, cr2; /* center x, y, radius² */
    int valid;
} circumcircle_t;

/* ══════════════════════════════════════════════════════════════
 *  Wendland C² Kernel — kept for audio
 * ══════════════════════════════════════════════════════════════ */

static inline double wendland_c2(double r) {
    if (r >= 1.0) return 0.0;
    double t = 1.0 - r;
    double t2 = t * t;
    return t2 * t2 * (4.0 * r + 1.0);
}

/* ══════════════════════════════════════════════════════════════
 *  Multi-Threaded Sobel Edge Detection
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *pixels;
    float *grad;
    uint32_t w, h;
    uint8_t ch;
    uint32_t start_row, end_row;
} gradient_thread_arg_t;

static void *compute_gradient_strip(void *arg) {
    gradient_thread_arg_t *a = (gradient_thread_arg_t *)arg;
    const uint8_t *pixels = a->pixels;
    float *grad = a->grad;
    uint32_t w = a->w, h = a->h;
    uint8_t ch = a->ch;
    uint32_t y0 = a->start_row < 1 ? 1 : a->start_row;
    uint32_t y1 = a->end_row > h - 1 ? h - 1 : a->end_row;

    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 1; x < w - 1; x++) {
            float gx = 0, gy = 0;
            for (uint8_t c = 0; c < ch; c++) {
                float dx = -(float)pixels[((y-1)*w + (x-1))*ch + c]
                           + (float)pixels[((y-1)*w + (x+1))*ch + c]
                           - 2.0f*(float)pixels[(y*w + (x-1))*ch + c]
                           + 2.0f*(float)pixels[(y*w + (x+1))*ch + c]
                           - (float)pixels[((y+1)*w + (x-1))*ch + c]
                           + (float)pixels[((y+1)*w + (x+1))*ch + c];
                float dy = -(float)pixels[((y-1)*w + (x-1))*ch + c]
                           - 2.0f*(float)pixels[((y-1)*w + x)*ch + c]
                           - (float)pixels[((y-1)*w + (x+1))*ch + c]
                           + (float)pixels[((y+1)*w + (x-1))*ch + c]
                           + 2.0f*(float)pixels[((y+1)*w + x)*ch + c]
                           + (float)pixels[((y+1)*w + (x+1))*ch + c];
                gx += dx * dx;
                gy += dy * dy;
            }
            grad[y * w + x] = sqrtf(gx + gy);
        }
    }
    return NULL;
}

static float *compute_gradient(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch) {
    float *grad = (float *)calloc((size_t)w * h, sizeof(float));
    if (!grad) return NULL;

#ifdef __APPLE__
    /* Thread-safe singleton (pthread_once inside). */
    qtsq_metal_ctx_t *metal = qtsq_metal_global();
    if (metal) {
        if (qtsq_metal_sobel(metal, pixels, grad, w, h, ch) == 0) {
            return grad;
        }
    }
#endif

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > h) nc = (int)h;

    gradient_thread_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].pixels = pixels;
        args[t].grad = grad;
        args[t].w = w;
        args[t].h = h;
        args[t].ch = ch;
        args[t].start_row = (uint32_t)((uint64_t)h * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)h * (t + 1) / nc);
    }
    qtsq_dispatch(compute_gradient_strip, args, sizeof(gradient_thread_arg_t), nc);

    return grad;
}

/* ══════════════════════════════════════════════════════════════
 *  Content-Aware Saliency-Based Vertex Placement
 *  Foreground (faces/objects) gets dense vertices,
 *  Background gets sparse vertices + extreme smoothing later
 * ══════════════════════════════════════════════════════════════ */

/* Helper: add a vertex at (x,y) if not already used */
static inline int add_vertex(const uint8_t *pixels, uint32_t w, uint8_t ch,
                             tri_vertex_t *verts, uint8_t *used,
                             uint32_t *nv, uint32_t max_verts,
                             uint32_t x, uint32_t y) {
    if (*nv >= max_verts) return 0;
    if (used[y * w + x]) return 0;
    used[y * w + x] = 1;
    size_t idx = ((size_t)y * w + x) * ch;
    verts[*nv].x = (float)x;
    verts[*nv].y = (float)y;
    verts[*nv].r = pixels[idx];
    verts[*nv].g = (ch >= 2) ? pixels[idx + 1] : pixels[idx];
    verts[*nv].b = (ch >= 3) ? pixels[idx + 2] : pixels[idx];
    (*nv)++;
    return 1;
}

/* qsort comparator for floats (descending) */
static int cmp_float_desc(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa < fb) - (fa > fb);
}

static uint32_t place_vertices(const uint8_t *pixels, const float *grad,
                                uint32_t w, uint32_t h, uint8_t ch,
                                tri_vertex_t *verts, uint32_t max_verts, uint8_t *used,
                                uint8_t **out_fg_map, uint32_t *out_bw,
                                uint32_t *out_bh, uint32_t *out_block_size) {
    uint32_t nv = 0;

    /* ══════════════════════════════════════════════════════════════
     *  4-TIER ADAPTIVE DENSITY CLASSIFICATION
     *
     *  Tier 3 (ULTRA):  Top 10%  — Dense objects, fine texture  → 1px grid
     *  Tier 2 (HIGH):   10-30%   — Moderate detail, edges       → 2px grid
     *  Tier 1 (MEDIUM): 30-60%   — Light texture                → SPLAT ONLY
     *  Tier 0 (LOW):    Bottom 40% — Smooth/sky/flat            → SPLAT ONLY
     *
     *  Triangle mesh ONLY places vertices in ULTRA + HIGH (tier >= 2).
     *  Gaussian Splats ONLY operate in MEDIUM + LOW  (tier <= 1).
     *  NO OVERLAP between the two systems.
     * ══════════════════════════════════════════════════════════════ */

    const int BLOCK = 16;
    uint32_t bw = (w + BLOCK - 1) / BLOCK;
    uint32_t bh = (h + BLOCK - 1) / BLOCK;
    uint32_t num_blocks = bw * bh;

    /* ── Step 1: Build Block Saliency + Color Variance Map ── */
    float *block_sal = (float *)calloc(num_blocks, sizeof(float));
    float *block_var = (float *)calloc(num_blocks, sizeof(float));
    if (!block_sal || !block_var) { free(block_sal); free(block_var); return 0; }

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            float grad_sum = 0;
            int count = 0;
            uint32_t y0 = by * BLOCK, y1 = (y0 + BLOCK < h) ? y0 + BLOCK : h;
            uint32_t x0 = bx * BLOCK, x1 = (x0 + BLOCK < w) ? x0 + BLOCK : w;

            /* Gradient saliency */
            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                    grad_sum += grad[y * w + x];
                    count++;
                }
            }
            block_sal[by * bw + bx] = (count > 0) ? (grad_sum / count) : 0.0f;

            /* Local color variance — catches high-frequency textures like grass
             * blades that have small individual gradients but many different hues */
            double cr_sum = 0, cg_sum = 0, cb_sum = 0;
            double cr2_sum = 0, cg2_sum = 0, cb2_sum = 0;
            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                    size_t idx = ((size_t)y * w + x) * ch;
                    double r = pixels[idx];
                    double g_c = (ch >= 2) ? pixels[idx + 1] : r;
                    double b = (ch >= 3) ? pixels[idx + 2] : r;
                    cr_sum += r; cg_sum += g_c; cb_sum += b;
                    cr2_sum += r * r; cg2_sum += g_c * g_c; cb2_sum += b * b;
                }
            }
            if (count > 1) {
                double rv = (cr2_sum - cr_sum * cr_sum / count) / count;
                double gv = (cg2_sum - cg_sum * cg_sum / count) / count;
                double bv = (cb2_sum - cb_sum * cb_sum / count) / count;
                block_var[by * bw + bx] = (float)(sqrt(rv + gv + bv));
            }
        }
    }

    /* ── Step 2: Compute combined score = gradient + variance boost ── */
    float *block_score = (float *)malloc(num_blocks * sizeof(float));
    if (!block_score) { free(block_sal); free(block_var); return 0; }

    /* Normalize and combine into a single score */
    float max_sal = 0, max_var = 0;
    for (uint32_t i = 0; i < num_blocks; i++) {
        if (block_sal[i] > max_sal) max_sal = block_sal[i];
        if (block_var[i] > max_var) max_var = block_var[i];
    }
    float sal_scale = (max_sal > 0.001f) ? (1.0f / max_sal) : 1.0f;
    float var_scale = (max_var > 0.001f) ? (1.0f / max_var) : 1.0f;
    for (uint32_t i = 0; i < num_blocks; i++) {
        block_score[i] = block_sal[i] * sal_scale * 0.7f
                       + block_var[i] * var_scale * 0.3f;
    }
    free(block_sal);
    free(block_var);

    /* Simple binary fg/bg split for splat fitting (NO 4-tier system).
     * Top 40% gradient blocks = foreground (value=2) → triangles already cover these well
     * Bottom 60% gradient blocks = background (value=0) → splats enhance these areas */
    float *sorted_scores = (float *)malloc(num_blocks * sizeof(float));
    uint8_t *fg_map_local = NULL;
    if (sorted_scores) {
        memcpy(sorted_scores, block_score, num_blocks * sizeof(float));
        qsort(sorted_scores, num_blocks, sizeof(float), cmp_float_desc);
        uint32_t fg_idx = (uint32_t)(num_blocks * 0.40f);
        if (fg_idx < 1) fg_idx = 1;
        float fg_thresh = sorted_scores[fg_idx - 1];
        free(sorted_scores);

        fg_map_local = (uint8_t *)calloc(num_blocks, sizeof(uint8_t));
        if (fg_map_local) {
            uint32_t fg_count = 0, bg_count = 0;
            for (uint32_t i = 0; i < num_blocks; i++) {
                if (block_score[i] >= fg_thresh) {
                    fg_map_local[i] = 2;  /* foreground */
                    fg_count++;
                } else {
                    fg_map_local[i] = 0;  /* background → splat territory */
                    bg_count++;
                }
            }
            fprintf(stderr, "[QTSQ HD] fg/bg split: foreground=%u background=%u (splat territory)\n", fg_count, bg_count);
        }
    }
    free(block_score);

    /* ── Smart placement: 40% Uniform Grid, 60% Error Diffusion Edge Bias ── */

    uint32_t grid_budget = (uint32_t)(max_verts * 0.40f);
    uint32_t edge_budget = max_verts - grid_budget;

    size_t npx = (size_t)w * h;
    /* Determine grid span such that exactly `grid_budget` vertices are placed uniformly */
    double grid_target = (double)grid_budget * 1.0;  
    int grid_step = (int)ceil(sqrt((double)npx / grid_target));
    if (grid_step < 1) grid_step = 1;

    /* Place baseline solid uniform grid to structurally prevent massive macro-triangles */
    uint32_t grid_placed = 0;
    for (uint32_t y = 0; y < h && grid_placed < grid_budget; y += grid_step) {
        for (uint32_t x = 0; x < w && grid_placed < grid_budget; x += grid_step) {
            if (add_vertex(pixels, w, ch, verts, used, &nv, max_verts, x, y))
                grid_placed++;
        }
    }

    /* Image boundary vertices (anchors the mesh to the frame) */
    int bnd_step = grid_step;
    for (uint32_t y = 0; y < h && nv < max_verts; y += bnd_step)
        add_vertex(pixels, w, ch, verts, used, &nv, max_verts, 0, y);
    for (uint32_t y = 0; y < h && nv < max_verts; y += bnd_step)
        add_vertex(pixels, w, ch, verts, used, &nv, max_verts, w - 1, y);
    for (uint32_t x = 0; x < w && nv < max_verts; x += bnd_step)
        add_vertex(pixels, w, ch, verts, used, &nv, max_verts, x, 0);
    for (uint32_t x = 0; x < w && nv < max_verts; x += bnd_step)
        add_vertex(pixels, w, ch, verts, used, &nv, max_verts, x, h - 1);
    add_vertex(pixels, w, ch, verts, used, &nv, max_verts, w - 1, h - 1);

    /* Edge placement — purely based on Error Diffusion of the continuous gradient field */
    double sum_grad = 0.0;
    for (uint32_t y = 2; y < h - 2; y++) {
        for (uint32_t x = 2; x < w - 2; x++) {
            if (!used[y * w + x]) sum_grad += (double)grad[y * w + x];
        }
    }

    double grad_per_vert = sum_grad / (double)(edge_budget > 0 ? edge_budget : 1);
    if (grad_per_vert < 0.0001) grad_per_vert = 1.0;  /* safety against pure solid colors */

    double error_acc = 0.0;
    for (uint32_t y = 2; y < h - 2 && nv < max_verts; y++) {
        for (uint32_t x = 2; x < w - 2 && nv < max_verts; x++) {
            if (used[y * w + x]) continue;
            error_acc += (double)grad[y * w + x];
            if (error_acc >= grad_per_vert) {
                add_vertex(pixels, w, ch, verts, used, &nv, max_verts, x, y);
                error_acc -= grad_per_vert;
            }
        }
    }

    /* Guaranteed Backfill: Any precision loss remaining in `max_verts` budget is dropped globally 
     * to eliminate final huge macro-triangles. */
    if (nv < max_verts) {
        uint32_t remaining = max_verts - nv;
        if (remaining > 10) {
            double r_step_f = sqrt((double)npx / (double)remaining);
            int r_step = (int)ceil(r_step_f);
            if (r_step < 1) r_step = 1;
            
            for (uint32_t y = r_step / 2; y < h && nv < max_verts; y += r_step) {
                for (uint32_t x = r_step / 2; x < w && nv < max_verts; x += r_step) {
                    if (!used[y * w + x]) {
                        add_vertex(pixels, w, ch, verts, used, &nv, max_verts, x, y);
                    }
                }
            }
        }
    }

    fprintf(stderr, "[QTSQ HD] Total vertices placed: %u (grid=%u, edges+boundary=%u)\n",
            nv, grid_placed, nv - grid_placed);

    /* ── Export fg_map — binary gradient-based split ── */
    if (out_fg_map && out_bw && out_bh && out_block_size) {
        if (fg_map_local) {
            *out_fg_map = fg_map_local;  /* caller takes ownership */
        } else {
            /* Fallback: all foreground if allocation failed */
            *out_fg_map = NULL;
        }
        *out_bw = bw;
        *out_bh = bh;
        *out_block_size = BLOCK;
    } else {
        if (fg_map_local) free(fg_map_local);
    }

    return nv;
}

/* ══════════════════════════════════════════════════════════════
 *  Circumcircle computation for Bowyer-Watson
 * ══════════════════════════════════════════════════════════════ */

static void compute_circumcircle(const tri_vertex_t *verts,
                                   uint32_t a, uint32_t b, uint32_t c,
                                   circumcircle_t *cc) {
    double ax = verts[a].x, ay = verts[a].y;
    double bx = verts[b].x, by = verts[b].y;
    double cx_v = verts[c].x, cy_v = verts[c].y;

    double D = 2.0 * (ax * (by - cy_v) + bx * (cy_v - ay) + cx_v * (ay - by));
    if (fabs(D) < 1e-10) {
        cc->cx = (ax + bx + cx_v) / 3.0;
        cc->cy = (ay + by + cy_v) / 3.0;
        cc->cr2 = 1e18;
        cc->valid = 1;
        return;
    }

    double a2 = ax * ax + ay * ay;
    double b2 = bx * bx + by * by;
    double c2 = cx_v * cx_v + cy_v * cy_v;

    cc->cx = (a2 * (by - cy_v) + b2 * (cy_v - ay) + c2 * (ay - by)) / D;
    cc->cy = (a2 * (cx_v - bx) + b2 * (ax - cx_v) + c2 * (bx - ax)) / D;
    double dx = ax - cc->cx;
    double dy = ay - cc->cy;
    cc->cr2 = dx * dx + dy * dy;
    cc->valid = 1;
}

typedef struct {
    uint32_t a, b;
} edge_t;

static int compare_verts_x(const void *a, const void *b) {
    const tri_vertex_t *va = (const tri_vertex_t *)a;
    const tri_vertex_t *vb = (const tri_vertex_t *)b;
    if (va->x < vb->x) return -1;
    if (va->x > vb->x) return 1;
    if (va->y < vb->y) return -1;
    if (va->y > vb->y) return 1;
    return 0;
}

uint32_t delaunay_triangulate(tri_vertex_t *verts, uint32_t nv,
                                      tri_face_t *faces, uint32_t max_faces,
                                      uint32_t img_w, uint32_t img_h) {

    if (nv < 3) return 0;
    double t_dt = qtsq_time_ms();

    /* Sort vertices by X ascending for sweep-line */
    qsort(verts, nv, sizeof(tri_vertex_t), compare_verts_x);

    float W = (float)img_w + 10.0f;
    float H = (float)img_h + 10.0f;
    float margin = 10.0f;

    /* Working buffers */
    uint32_t active_nf = 0;
    tri_face_t *active_faces = faces; /* Reuse the start of faces array for active */
    circumcircle_t *active_ccs = (circumcircle_t *)malloc(max_faces * sizeof(circumcircle_t));
    uint32_t *bad = (uint32_t *)malloc(16384 * sizeof(uint32_t)); /* Small buffer for nbad */
    edge_t *polygon = (edge_t *)malloc(16384 * sizeof(edge_t));   /* Small buffer for polygon */

    if (!active_ccs || !bad || !polygon) {
        free(active_ccs); free(bad); free(polygon);
        return 0;
    }

    uint32_t final_start = max_faces;

    /* Super-triangle */
    uint32_t sv0 = nv, sv1 = nv + 1, sv2 = nv + 2;
    verts[sv0].x = -W;       verts[sv0].y = -margin;
    verts[sv1].x = W * 2.0f; verts[sv1].y = -margin;
    verts[sv2].x = W * 0.5f; verts[sv2].y = H * 3.0f;
    verts[sv0].r = verts[sv0].g = verts[sv0].b = 128;
    verts[sv1].r = verts[sv1].g = verts[sv1].b = 128;
    verts[sv2].r = verts[sv2].g = verts[sv2].b = 128;

    active_faces[0].v0 = sv0; active_faces[0].v1 = sv1; active_faces[0].v2 = sv2;
    compute_circumcircle(verts, sv0, sv1, sv2, &active_ccs[0]);
    active_nf = 1;

    for (uint32_t vi = 0; vi < nv; vi++) {
        double px = verts[vi].x, py = verts[vi].y;
        uint32_t nbad = 0;

        /* Single ultra-tight loop: process active triangles */
        for (uint32_t i = 0; i < active_nf; ) {
            double cx = active_ccs[i].cx;
            double cr2 = active_ccs[i].cr2;
            double dx = px - cx;
            double dx2 = dx * dx;

            /* Check if triangle is bad or finalized */
            if (dx2 > cr2) {
                if (dx > 0) {
                    /* Finalized: sweep-line passed the right edge of circumcircle */
                    if (final_start > active_nf) {
                        final_start--;
                        faces[final_start] = active_faces[i];
                    }
                    /* Swap-and-pop from active array */
                    active_nf--;
                    if (i < active_nf) {
                        active_faces[i] = active_faces[active_nf];
                        active_ccs[i]   = active_ccs[active_nf];
                    }
                    /* Do NOT increment i, re-check swapped-in element */
                    continue;
                }
                /* Else: dx <= 0, too far right, but sweep line hasn't passed it yet.
                 * Just skip it. */
            } else {
                double dy = py - active_ccs[i].cy;
                if (dx2 + dy * dy <= cr2) {
                    /* Triangle circumcircle contains (px,py) */
                    if (nbad < 16384) bad[nbad++] = i;
                }
            }
            i++;
        }

        if (nbad == 0) continue;

        /* Build polygon from bad triangles */
        uint32_t npoly = 0;
        for (uint32_t bi = 0; bi < nbad; bi++) {
            uint32_t fi = bad[bi];
            uint32_t tv[3] = { active_faces[fi].v0, active_faces[fi].v1, active_faces[fi].v2 };
            edge_t tri_edges[3] = {
                { tv[0], tv[1] }, { tv[1], tv[2] }, { tv[2], tv[0] }
            };

            for (int ei = 0; ei < 3; ei++) {
                int shared = 0;
                /* Check if edge is shared with another bad triangle */
                for (uint32_t bj = 0; bj < nbad; bj++) {
                    if (bj == bi) continue;
                    uint32_t fj = bad[bj];
                    uint32_t ov[3] = { active_faces[fj].v0, active_faces[fj].v1, active_faces[fj].v2 };
                    for (int ej = 0; ej < 3; ej++) {
                        uint32_t oa = ov[ej], ob = ov[(ej + 1) % 3];
                        if ((tri_edges[ei].a == oa && tri_edges[ei].b == ob) ||
                            (tri_edges[ei].a == ob && tri_edges[ei].b == oa)) {
                            shared = 1; break;
                        }
                    }
                    if (shared) break;
                }
                if (!shared) {
                    if (npoly < 16384) polygon[npoly++] = tri_edges[ei];
                }
            }
        }

        /* Remove bad triangles safely using swap-and-pop.
         * Must process in perfectly descending index order so subsequent 
         * removals don't invalidate pending indices. */
        for (uint32_t i = 0; i < nbad; i++) {
            for (uint32_t j = i + 1; j < nbad; j++) {
                if (bad[j] > bad[i]) {
                    uint32_t t = bad[i]; bad[i] = bad[j]; bad[j] = t;
                }
            }
        }
        for (uint32_t i = 0; i < nbad; i++) {
            uint32_t fi = bad[i];
            active_nf--;
            if (fi < active_nf) {
                active_faces[fi] = active_faces[active_nf];
                active_ccs[fi]   = active_ccs[active_nf];
            }
        }

        /* Form new triangles from polygon edges */
        for (uint32_t i = 0; i < npoly && active_nf < final_start; i++) {
             uint32_t a = polygon[i].a;
             uint32_t b = polygon[i].b;
             uint32_t c = vi;
             active_faces[active_nf].v0 = a;
             active_faces[active_nf].v1 = b;
             active_faces[active_nf].v2 = c;
             compute_circumcircle(verts, a, b, c, &active_ccs[active_nf]);
             active_nf++;
        }
    }

    /* Consolidate: active + finalized, excluding any connected to super-triangle */
    uint32_t total_nf = 0;

    for (uint32_t i = 0; i < active_nf; i++) {
        if (active_faces[i].v0 >= nv || active_faces[i].v1 >= nv || active_faces[i].v2 >= nv)
            continue;
        faces[total_nf++] = active_faces[i];
    }

    for (uint32_t i = final_start; i < max_faces; i++) {
        if (faces[i].v0 >= nv || faces[i].v1 >= nv || faces[i].v2 >= nv)
            continue;
        faces[total_nf++] = faces[i];
    }

    free(active_ccs);
    free(bad);
    free(polygon);

    QTSQ_TLOG("[QTSQ] Delaunay: %u verts \u2192 %u faces in %.1f ms\n",
              nv, total_nf, qtsq_time_ms() - t_dt);

    return total_nf;
}



/* ══════════════════════════════════════════════════════════════
 *  Simple Delta+RLE (kept for audio codec)
 * ══════════════════════════════════════════════════════════════ */

static size_t delta_rle_encode(const uint8_t *src, size_t len,
                                uint8_t *dst, size_t cap) {
    if (len == 0 || cap < 2) return 0;
    size_t out = 0;

    uint8_t *delta = (uint8_t *)malloc(len);
    if (!delta) return 0;
    delta[0] = src[0];
    for (size_t i = 1; i < len; i++) delta[i] = src[i] - src[i - 1];

    size_t i = 0;
    while (i < len && out + 2 < cap) {
        uint8_t val = delta[i];
        size_t run = 1;
        while (i + run < len && delta[i + run] == val && run < 255) run++;

        if (run >= 3 || val == 0) {
            if (out + 3 > cap) break;
            dst[out++] = 0x00;
            dst[out++] = (uint8_t)run;
            dst[out++] = val;
            i += run;
        } else {
            if (val == 0x00) {
                if (out + 3 > cap) break;
                dst[out++] = 0x00; dst[out++] = 1; dst[out++] = 0x00;
                i++;
            } else {
                dst[out++] = val;
                i++;
            }
        }
    }

    free(delta);
    return (i == len) ? out : 0;
}

static size_t delta_rle_decode(const uint8_t *src, size_t len,
                                uint8_t *dst, size_t expected_len) {
    size_t in = 0, out = 0;
    while (in < len && out < expected_len) {
        if (src[in] == 0x00 && in + 2 < len) {
            uint8_t count = src[in + 1];
            uint8_t val = src[in + 2];
            for (uint8_t j = 0; j < count && out < expected_len; j++)
                dst[out++] = val;
            in += 3;
        } else {
            dst[out++] = src[in++];
        }
    }
    for (size_t i = 1; i < out; i++) dst[i] = dst[i] + dst[i - 1];
    return out;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_gravitational
 *  Triangle RGB Mesh Compression
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_gravitational(qtsq_context_t *ctx,
                                 const uint8_t *pixels,
                                 uint32_t width, uint32_t height,
                                 uint8_t channels,
                                 size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "gravitational");
    if (guard != QTSQ_OK) return guard;

    double t_total = qtsq_time_ms();

    /* Compact mode — adaptive vertex budget based on input size.
     * Empirical: zlib-9 on planar vertex data ≈ 3.6 bytes/vertex.
     *
     *   Input ≤ 2 MB  →  output = 30-35% of original  (target 32.5%)
     *   Input > 2 MB  →  output = 40-50% of original  (target 45%)
     */
    size_t pixel_count = (size_t)width * height;
    
    /* Use actual file size if provided, otherwise estimate a typical JPEG size (raw / 10) */
    size_t original_bytes = source_size > 0 ? source_size : (pixel_count * channels / 10);
    size_t two_mb = 2 * 1024 * 1024;

    double target_ratio;
    if (original_bytes <= two_mb) {
        target_ratio = 0.65;   /* Goal: 30-35% less -> 65% of original */
    } else {
        target_ratio = 0.75;    /* Goal: 20-30% less -> 70-75% of original */
    }

    double target_bytes = (double)original_bytes * target_ratio;
    uint32_t max_verts = (uint32_t)(target_bytes / 3.6);
    if (max_verts < 25000)   max_verts = 25000;
    if (max_verts > 2000000) max_verts = 2000000;

    QTSQ_TLOG("[QTSQ Compact] Input: %zu bytes (%s 2MB), target ratio: %.0f%%, max_verts: %u\n",
              original_bytes, original_bytes <= two_mb ? "<=" : ">", target_ratio * 100, max_verts);

    /* Allocate working buffers */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Step 1: Edge detection */
    double t0 = qtsq_time_ms();
    float *grad = compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }
    QTSQ_TLOG("[QTSQ Compact] Gradient: %.1f ms\n", qtsq_time_ms() - t0);

    /* Step 2: Place ALL vertices — no face budget needed */
    t0 = qtsq_time_ms();
    uint32_t nv = place_vertices(pixels, grad, width, height, channels,
                                  verts, max_verts, used,
                                  NULL, NULL, NULL, NULL);
    QTSQ_TLOG("[QTSQ Compact] Place vertices: %.1f ms (%u verts)\n", qtsq_time_ms() - t0, nv);
    free(grad);
    free(used);

    /* Step 3: Pack in PLANAR vertex-only layout */
    size_t header_sz = 4 + 4 + 4 + 1 + 4;
    size_t vert_data = (size_t)nv * 7;
    size_t total_sz = header_sz + vert_data;

    uint8_t *output = (uint8_t *)malloc(total_sz);
    if (!output) { free(verts); return QTSQ_ERR_ALLOC; }

    size_t pos = 0;
    output[pos++] = 'T'; output[pos++] = 'R';
    output[pos++] = 'M'; output[pos++] = '3';
    memcpy(output + pos, &width, 4); pos += 4;
    memcpy(output + pos, &height, 4); pos += 4;
    output[pos++] = channels;
    memcpy(output + pos, &nv, 4); pos += 4;

    for (uint32_t i = 0; i < nv; i++) {
        uint16_t vx = (uint16_t)(verts[i].x + 0.5f);
        memcpy(output + pos, &vx, 2); pos += 2;
    }
    for (uint32_t i = 0; i < nv; i++) {
        uint16_t vy = (uint16_t)(verts[i].y + 0.5f);
        memcpy(output + pos, &vy, 2); pos += 2;
    }
    for (uint32_t i = 0; i < nv; i++) output[pos++] = verts[i].r;
    for (uint32_t i = 0; i < nv; i++) output[pos++] = verts[i].g;
    for (uint32_t i = 0; i < nv; i++) output[pos++] = verts[i].b;

    free(verts);

    /* Compress: qtc (opt-in) or zlib (default) */
    t0 = qtsq_time_ms();
    uint8_t *zd = NULL;
    size_t zs = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(output, pos, &zd, &zs) != QTSQ_OK) {
            free(output); return QTSQ_ERR_ALLOC;
        }
    } else {
        uLongf zb = compressBound((uLong)pos);
        zd = (uint8_t *)malloc(zb);
        if (!zd) { free(output); return QTSQ_ERR_ALLOC; }
        uLongf zl = zb;
        if (compress2(zd, &zl, output, (uLong)pos, 9) != Z_OK) {  /* Level 9: max compression for compact */
            free(output); free(zd); return QTSQ_ERR_ALLOC;
        }
        zs = (size_t)zl;
    }
    QTSQ_TLOG("[QTSQ Compact] Zlib compress: %.1f ms\n", qtsq_time_ms() - t0);

    /* Store: [4B uncompressed_size] [zlib data] */
    size_t final_sz = 4 + (size_t)zs;
    uint8_t *final_out = (uint8_t *)malloc(final_sz);
    if (!final_out) { free(output); free(zd); return QTSQ_ERR_ALLOC; }
    uint32_t uncomp_sz = (uint32_t)pos;
    memcpy(final_out, &uncomp_sz, 4);
    memcpy(final_out + 4, zd, zs);
    free(output); free(zd);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = final_out;
    ctx->singularity_size = (uint32_t)final_sz;

    /* Set header */
    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.reserved32 = nv;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "gravitational", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = channels;
    ctx->schema.num_dims = 3;
    strncpy(ctx->schema.encoding, "TRI_RGB_MESH", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    QTSQ_TLOG("[QTSQ Compact] Total compress: %.1f ms\n", qtsq_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_gravitational_hd
 *  QTSQ HD — High-Quality Triangle RGB Mesh Compression
 *
 *  Same pipeline as Compact but with HD-specific parameters:
 *   - Up to 3M vertices (vs 400K)
 *   - Finer edge detection (lower Sobel thresholds)
 *   - Denser grid sampling in flat areas
 *   - Sets QTSQ_FLAG_HD so decoders/AI models know the tier
 *
 *  Goal: Beat JPEG/WebP visual quality at 20-30% smaller size.
 * ══════════════════════════════════════════════════════════════ */

static int compare_verts_y_x_pack(const void *a, const void *b) {
    const tri_vertex_t *va = (const tri_vertex_t *)a;
    const tri_vertex_t *vb = (const tri_vertex_t *)b;
    if (va->y < vb->y) return -1;
    if (va->y > vb->y) return 1;
    if (va->x < vb->x) return -1;
    if (va->x > vb->x) return 1;
    return 0;
}

/* Helper: pack nv vertices into TRM3 format and compress with zlib.
 * Returns the compressed payload and its size via out_data / out_size.
 * Caller owns the returned buffer. Returns 0 on success. */
static int pack_and_compress_trm3(const tri_vertex_t *verts, uint32_t nv,
                                   uint32_t width, uint32_t height, uint8_t channels,
                                   uint8_t **out_data, size_t *out_size, int comp_level,
                                   uint8_t codec) {
    /* Sort a COPY by Y then X for zlib delta efficiency.
     * We must not sort the caller's array because the binary search
     * trims from the end — if sorted by Y, that cuts off the bottom of the image. */
    tri_vertex_t *sorted = (tri_vertex_t *)malloc(nv * sizeof(tri_vertex_t));
    if (!sorted) return -1;
    memcpy(sorted, verts, nv * sizeof(tri_vertex_t));
    qsort(sorted, nv, sizeof(tri_vertex_t), compare_verts_y_x_pack);

    size_t hsz = 4 + 4 + 4 + 1 + 4;  /* TRM3 header: 17 bytes */
    size_t vd = (size_t)nv * 7;       /* 2+2+1+1+1 per vertex */
    size_t raw_sz = hsz + vd;

    uint8_t *raw = (uint8_t *)malloc(raw_sz);
    if (!raw) { free(sorted); return -1; }

    size_t p = 0;
    raw[p++] = 'T'; raw[p++] = 'R'; raw[p++] = 'M'; raw[p++] = '3';
    memcpy(raw + p, &width, 4);  p += 4;
    memcpy(raw + p, &height, 4); p += 4;
    raw[p++] = channels;
    memcpy(raw + p, &nv, 4);    p += 4;

    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(sorted[i].x + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(sorted[i].y + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].r;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].g;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].b;
    free(sorted);

    uint8_t *zd = NULL;
    size_t zs = 0;
    if (codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(raw, p, &zd, &zs) != QTSQ_OK) {
            free(raw); return -1;
        }
    } else {
        uLongf zb = compressBound((uLong)p);
        zd = (uint8_t *)malloc(zb);
        if (!zd) { free(raw); return -1; }
        uLongf zl = zb;
        if (compress2(zd, &zl, raw, (uLong)p, comp_level) != Z_OK) {
            free(raw); free(zd); return -1;
        }
        zs = (size_t)zl;
    }

    size_t final_sz = 4 + (size_t)zs;
    uint8_t *out = (uint8_t *)malloc(final_sz);
    if (!out) { free(raw); free(zd); return -1; }
    uint32_t uncomp = (uint32_t)p;
    memcpy(out, &uncomp, 4);
    memcpy(out + 4, zd, zs);
    free(raw); free(zd);

    *out_data = out;
    *out_size = final_sz;
    return 0;
}



static int compare_splats_area(const void *a, const void *b) {
    const splat2d_t *sa = (const splat2d_t *)a;
    const splat2d_t *sb = (const splat2d_t *)b;
    uint32_t area_a = (uint32_t)sa->sx * sa->sy;
    uint32_t area_b = (uint32_t)sb->sx * sb->sy;
    /* Sort descending: keep largest/most important background splats first */
    if (area_a > area_b) return -1;
    if (area_a < area_b) return 1;
    return 0;
}

int qtsq_compress_gravitational_hd(qtsq_context_t *ctx,
                                    const uint8_t *pixels,
                                    uint32_t width, uint32_t height,
                                    uint8_t channels,
                                    size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "gravitational_hd");
    if (guard != QTSQ_OK) return guard;

    double t_hd_total = qtsq_time_ms();
    double t_step;

    size_t pixel_count = (size_t)width * height;

    /* QTSQ HD target: preserve quality with only 20-30% size reduction.
     * HD mode is the HIGH QUALITY tier — output should be close to the original.
     * Heavy compression (tiny output) belongs to Compact mode, not here.
     *
     * IMPORTANT: The budget controls raw vertex data, which then gets ZLib
     * compressed at ~4-5:1 ratio. So to achieve a final output of ~75% of
     * source_size, we need a raw budget of ~3.5x source_size.
     * Example: 2.8MB JPEG → raw budget 9.8MB → ZLib to ~2.1MB output. */
    size_t hdr_overhead = sizeof(qtsq_header_t) + sizeof(qtsq_schema_t);
    size_t raw_pixels = pixel_count * channels;
    size_t target_payload;
    if (source_size > 0) {
        /* Raw budget = 3.5x source → ZLib compresses to ~75% of source */
        target_payload = (size_t)(source_size * 3.5);
        if (target_payload > raw_pixels * 2) target_payload = raw_pixels * 2;
    } else {
        /* Fallback: generous budget from raw pixel data */
        target_payload = (size_t)(pixel_count * channels * 2.0);
    }
    if (target_payload > hdr_overhead) target_payload -= hdr_overhead;
    
    /* Budget split: 85% vertices, 15% splats.
     * Dense vertex coverage is key for HD quality; splats fill subtle gaps. */
    size_t target_payload_verts = (size_t)(target_payload * 0.85);
    size_t target_payload_splats = target_payload - target_payload_verts;

    /* Derive max_verts from vertex budget. Each vertex is ~7 bytes raw,
     * ~1.5-2 bytes after ZLib. Use /2 as estimate for budget planning. */
    uint32_t max_verts = (uint32_t)(target_payload_verts / 2);
    if (max_verts < 200000) max_verts = 200000;
    if (max_verts > 1500000) max_verts = 1500000;

    /* Allocate working buffers */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Step 1: Edge detection (same Sobel, but we use more of its output) */
    t_step = qtsq_time_ms();
    float *grad = compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }
    QTSQ_TLOG("[QTSQ HD] Gradient: %.1f ms\n", qtsq_time_ms() - t_step);

    /* Step 2: Place vertices across ALL 4 tiers for full coverage. */
    t_step = qtsq_time_ms();
    uint8_t *fg_map = NULL;
    uint32_t fg_bw = 0, fg_bh = 0, fg_block_size = 0;
    uint32_t nv = place_vertices(pixels, grad, width, height, channels,
                                  verts, max_verts, used,
                                  &fg_map, &fg_bw, &fg_bh, &fg_block_size);
    QTSQ_TLOG("[QTSQ HD] Place vertices: %.1f ms (%u verts)\n", qtsq_time_ms() - t_step, nv);

    free(grad);
    free(used);

    uint8_t *best_payload = NULL;
    size_t best_payload_size = 0;
    uint32_t best_nv = nv;

    /* Compress vertices with ZLib level 1 for speed — mobile can't afford level 6+ */
    if (pack_and_compress_trm3(verts, nv, width, height, channels, &best_payload, &best_payload_size, 1, ctx->codec) != 0) {
        free(verts);
        if (fg_map) free(fg_map);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    /* ── Step 4: Fit 2D Gaussian Splats to background regions ── */
    t_step = qtsq_time_ms();
    int is_3d = (ctx && (ctx->header.flags & QTSQ_FLAG_3D));
    uint32_t max_splats = (uint32_t)(target_payload_splats / (is_3d ? 12 : 11));
    if (max_splats < 100) max_splats = 100;
    if (max_splats > 80000) max_splats = 80000;
    fprintf(stderr, "[QTSQ HD] Budget: payload=%zu verts_budget=%zu splats_budget=%zu max_verts=%u max_splats=%u\n",
            target_payload, target_payload_verts, target_payload_splats, max_verts, max_splats);
    splat2d_t *splats = NULL;
    uint32_t ns = 0;
    uint8_t *gsp2_data = NULL;

    if (fg_map) {
        splats = (splat2d_t *)malloc(max_splats * sizeof(splat2d_t));
        if (splats) {
            ns = splat2d_fit_background(pixels, width, height, channels,
                                         fg_map, fg_bw, fg_bh, fg_block_size,
                                         splats, max_splats);
            if (ns > 0) {                /* Refine splat parameters via Gradient Descent */
                uint8_t *fg_pixel_mask = (uint8_t *)calloc((size_t)width * height, sizeof(uint8_t));
                if (fg_pixel_mask) {
                    for (uint32_t by = 0; by < fg_bh; by++) {
                        for (uint32_t bx = 0; bx < fg_bw; bx++) {
                            uint8_t tier = fg_map[by * fg_bw + bx];
                            if (tier >= 2) {
                                uint32_t py0 = by * fg_block_size;
                                uint32_t py1 = py0 + fg_block_size;
                                if (py1 > height) py1 = height;
                                uint32_t px0 = bx * fg_block_size;
                                uint32_t px1 = px0 + fg_block_size;
                                if (px1 > width) px1 = width;
                                for (uint32_t py = py0; py < py1; py++) {
                                    for (uint32_t px = px0; px < px1; px++) {
                                        fg_pixel_mask[py * width + px] = tier;
                                    }
                                }
                            }
                        }
                    }
#ifdef __ANDROID__
                    fprintf(stderr, "[QTSQ HD] Running refinement on %u splats (2 views, Android)...\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 2, 2);
#else
                    fprintf(stderr, "[QTSQ HD] Running jittered multi-view refinement on %u splats (8 views)...\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 5, 8);
#endif
                    free(fg_pixel_mask);
                } else {
#ifdef __ANDROID__
                    fprintf(stderr, "[QTSQ HD] Running refinement on %u splats (2 views, no fg, Android)...\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 2, 2);
#else
                    fprintf(stderr, "[QTSQ HD] Running jittered multi-view refinement on %u splats (8 views, no fg mask)...\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 5, 8);
#endif
                }

                if (is_3d) {
                    fprintf(stderr, "[QTSQ HD] Estimating 3D depth for %u splats (parallax mode)...\n", ns);
                    splat2d_estimate_depth(splats, ns, pixels, width, height, channels);
                }

                qsort(splats, ns, sizeof(splat2d_t), compare_splats_area);
            }
        }
    }

    if (fg_map) { free(fg_map); fg_map = NULL; }

    /* ── Step 5: Combine TRM3 + GSP2 into single compressed payload ── */
    if (ns > 0) {
        uint8_t *trm3_raw = NULL;
        size_t trm3_raw_n = 0;
        if (qtsq_uncompress_bounded(best_payload, best_payload_size, 256,
                                    1ULL << 30, &trm3_raw, &trm3_raw_n) == QTSQ_OK) {
            uLongf trm3_dl = (uLongf)trm3_raw_n;
            if (trm3_raw) {
                
                uint32_t lo = 0;
                uint32_t hi = ns;
                uint32_t best_ns = 0;
                uint8_t *best_combined = NULL;
                size_t best_combined_size = 0;

                int max_search_iters = 6;
                while (lo <= hi && max_search_iters-- > 0) {
                    uint32_t mid = lo + (hi - lo) / 2;
                    if (mid == 0) { lo = mid + 1; continue; }

                    uint8_t *test_gsp2 = NULL;
                    size_t test_gsp2_sz = 0;
                    if (is_3d) {
                        splat2d_pack_gsp3(splats, mid, width, height, &test_gsp2, &test_gsp2_sz);
                    } else {
                        splat2d_pack_gsp2(splats, mid, width, height, &test_gsp2, &test_gsp2_sz);
                    }

                    if (!test_gsp2 || test_gsp2_sz == 0) {
                        if (test_gsp2) free(test_gsp2);
                        hi = mid - 1; continue;
                    }

                    size_t comb_raw_size = trm3_dl + test_gsp2_sz;
                    uint8_t *comb_raw = (uint8_t *)malloc(comb_raw_size);
                    if (!comb_raw) { free(test_gsp2); hi = mid - 1; continue; }
                    memcpy(comb_raw, trm3_raw, trm3_dl);
                    memcpy(comb_raw + trm3_dl, test_gsp2, test_gsp2_sz);

                    uint8_t *zd = NULL;
                    uLongf zb = 0;
                    int z_res;
                    if (ctx->codec == QTSQ_CODEC_QTC) {
                        size_t zsz = 0;
                        int qrc = qtsq_codec_compress(comb_raw, comb_raw_size, &zd, &zsz);
                        zb = (uLongf)zsz;
                        z_res = (qrc == QTSQ_OK) ? Z_OK : Z_BUF_ERROR;
                    } else {
                        zb = compressBound((uLong)comb_raw_size);
                        zd = (uint8_t *)malloc(zb);
                        if (!zd) { free(comb_raw); free(test_gsp2); hi = mid - 1; continue; }
                        z_res = compress2(zd, &zb, comb_raw, (uLong)comb_raw_size, 1);
                    }

                    if (z_res == Z_OK) {
                        size_t test_payload_size = 4 + (size_t)zb;
                        int fits = 0;
                        size_t splat_headroom = best_payload_size / 7 + 20480;
                        if (test_payload_size <= target_payload) fits = 1;
                        if (test_payload_size <= best_payload_size + splat_headroom) fits = 1;

                        if (fits || mid == 1) {
                            best_ns = mid;
                            if (best_combined) free(best_combined);
                            best_combined = (uint8_t *)malloc(test_payload_size);
                            if (best_combined) {
                                uint32_t new_uncomp = (uint32_t)comb_raw_size;
                                memcpy(best_combined, &new_uncomp, 4);
                                memcpy(best_combined + 4, zd, zb);
                                best_combined_size = test_payload_size;
                            }
                            lo = mid + 1;
                        } else {
                            hi = mid - 1;
                        }
                    } else {
                        hi = mid - 1;
                    }
                    free(zd);
                    free(test_gsp2);
                    free(comb_raw);
                }

                if (best_combined) {
                    free(best_payload);
                    best_payload = best_combined;
                    best_payload_size = best_combined_size;
                    fprintf(stderr, "[QTSQ HD] Splat budget: kept %u/%u splats (Payload %zu bytes)\n", 
                            best_ns, ns, best_payload_size);
                    ns = best_ns;
                } else {
                    fprintf(stderr, "[QTSQ HD] Splat budget: discarded all %u splats\n", ns);
                    ns = 0;
                }
            }
            free(trm3_raw);
        }
        if (gsp2_data) free(gsp2_data);
    }

    if (splats) free(splats);

    /* Store compressed payload */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = best_payload;
    ctx->singularity_size = (uint32_t)best_payload_size;

    ctx->header.reserved32 = best_nv;

    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_HD;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "gravitational_hd", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = ns;
    ctx->schema.num_dims = 3;
    if (ns > 0) {
        strncpy(ctx->schema.encoding, "TRI_SPLAT_HYBRID_HD", 63);
    } else {
        strncpy(ctx->schema.encoding, "TRI_RGB_MESH_HD", 63);
    }
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    QTSQ_TLOG("[QTSQ HD] Total compress: %.1f ms\n", qtsq_time_ms() - t_hd_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_gravitational_upscale
 *  QTSQ Upscale — Mathematical Quality Enhancement for Lossy Inputs
 *
 *  For JPEG/WebP inputs, the triangle mesh acts as a reconstruction
 *  filter: Sobel edge detection finds REAL structural edges (not
 *  JPEG block boundaries), Delaunay interpolation replaces blocky
 *  8×8 artifacts with smooth, continuous color fields, and Gaussian
 *  splats fill backgrounds with clean gradients.
 *
 *  Output is LARGER than input but HIGHER QUALITY.
 *
 *  Key differences from HD mode:
 *   - Budget = 8x input size (not 20-30% smaller)
 *   - 65% vertices / 35% splats (more splats for clean backgrounds)
 *   - Ultra-low edge threshold (1.5%) to capture fine structure
 *   - 80% foreground budget to maximize edge vertex density
 *   - 8 gradient descent iterations for precise splat color fitting
 *   - ZLib level 9 for maximum compression efficiency
 * ══════════════════════════════════════════════════════════════ */
int qtsq_compress_gravitational_upscale(qtsq_context_t *ctx,
                                         const uint8_t *pixels,
                                         uint32_t width, uint32_t height,
                                         uint8_t channels,
                                         size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "gravitational_upscale");
    if (guard != QTSQ_OK) return guard;

    double t_total = qtsq_time_ms();
    double t_step;
    size_t pixel_count = (size_t)width * height;

    /* ── Upscale budget tiers based on input size ──
     * <1MB       : output = input * 1.35  (upscale, +30-40% size)
     * 1MB-2MB    : output = input * 1.06  (gentle, +5-7%)
     * 2MB-5MB    : output ≈ 4.4MB fixed   (~4.3-4.5MB sweet spot)
     * >5MB       : output = input * 0.85  (compress, 10-20% smaller)
     *
     * Raw budget = desired_output * ~3.5 (ZLib compression ratio estimate) */
    size_t raw_pixels = pixel_count * channels;
    size_t target_payload;
    if (source_size < 1000000) {
        /* <1MB: upscale quality, output 30-40% larger */
        target_payload = (size_t)(source_size * 1.35 * 3.5);
    } else if (source_size < 2000000) {
        /* 1-2MB: gentle touch, output +5-7% */
        target_payload = (size_t)(source_size * 1.06 * 3.5);
    } else if (source_size < 5000000) {
        /* 2-5MB: fixed output around 4.3-4.5MB */
        target_payload = (size_t)(4400000.0 * 3.5);
    } else {
        /* >5MB: quality-preserving compression, 10-20% smaller */
        target_payload = (size_t)(source_size * 0.85 * 3.5);
    }
    if (target_payload > raw_pixels * 2) target_payload = raw_pixels * 2;
    if (target_payload < 300000) target_payload = 300000;  /* min 300KB output */

    /* Budget split: 65% vertices, 35% splats */
    size_t target_payload_verts = (size_t)(target_payload * 0.65);
    size_t target_payload_splats = target_payload - target_payload_verts;

    /* Derive max_verts */
    uint32_t max_verts = (uint32_t)(target_payload_verts / 2);
    if (max_verts < 100000) max_verts = 100000;
    if (max_verts > 2500000) max_verts = 2500000;

    QTSQ_TLOG("[QTSQ UPSCALE] Input %zu bytes → budget %zu (verts %zu / splats %zu)\n",
              source_size, target_payload, target_payload_verts, target_payload_splats);

    /* Allocate working buffers */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Step 1: Edge detection */
    t_step = qtsq_time_ms();
    float *grad = compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }
    QTSQ_TLOG("[QTSQ UPSCALE] Gradient: %.1f ms\n", qtsq_time_ms() - t_step);

    /* Step 2: Place vertices with upscale-optimized parameters */
    t_step = qtsq_time_ms();
    uint8_t *fg_map = NULL;
    uint32_t fg_bw = 0, fg_bh = 0, fg_block_size = 0;
    uint32_t nv = place_vertices(pixels, grad, width, height, channels,
                                  verts, max_verts, used,
                                  &fg_map, &fg_bw, &fg_bh, &fg_block_size);
    QTSQ_TLOG("[QTSQ UPSCALE] Place vertices: %.1f ms (%u verts)\n", qtsq_time_ms() - t_step, nv);

    free(grad);
    free(used);

    uint8_t *best_payload = NULL;
    size_t best_payload_size = 0;
    uint32_t best_nv = nv;

    /* Compress vertices with ZLib level 1 for speed — level 9 is way too slow on mobile */
    if (pack_and_compress_trm3(verts, nv, width, height, channels,
                                &best_payload, &best_payload_size, 1, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    /* ── Step 3: Fit Gaussian Splats — 35% of budget for clean backgrounds ── */
    t_step = qtsq_time_ms();
    uint32_t max_splats = (uint32_t)(target_payload_splats / 11);
    if (max_splats < 500) max_splats = 500;
    if (max_splats > 80000) max_splats = 80000;
    QTSQ_TLOG("[QTSQ UPSCALE] Max splats: %u\n", max_splats);

    splat2d_t *splats = NULL;
    uint32_t ns = 0;

    if (fg_map) {
        splats = (splat2d_t *)malloc(max_splats * sizeof(splat2d_t));
        if (splats) {
            ns = splat2d_fit_background(pixels, width, height, channels,
                                         fg_map, fg_bw, fg_bh, fg_block_size,
                                         splats, max_splats);
            if (ns > 0) {
                /* Expand fg_map from block-level to pixel-level for gradient descent */
                uint8_t *fg_pixel_mask = (uint8_t *)calloc((size_t)width * height, sizeof(uint8_t));
                if (fg_pixel_mask) {
                    for (uint32_t by = 0; by < fg_bh; by++) {
                        for (uint32_t bx = 0; bx < fg_bw; bx++) {
                            uint8_t tier = fg_map[by * fg_bw + bx];
                            if (tier >= 2) {
                                uint32_t py0 = by * fg_block_size;
                                uint32_t py1 = py0 + fg_block_size;
                                if (py1 > height) py1 = height;
                                uint32_t px0 = bx * fg_block_size;
                                uint32_t px1 = px0 + fg_block_size;
                                if (px1 > width) px1 = width;
                                for (uint32_t py = py0; py < py1; py++) {
                                    for (uint32_t px = px0; px < px1; px++) {
                                        fg_pixel_mask[py * width + px] = tier;
                                    }
                                }
                            }
                        }
                    }
#ifdef __ANDROID__
                    QTSQ_TLOG("[QTSQ UPSCALE] Refinement: 2 iters, 2 views on %u splats (Android)\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 2, 2);
#else
                    QTSQ_TLOG("[QTSQ UPSCALE] Jittered multi-view: 5 iterations, 8 views on %u splats\n", ns);
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 5, 8);
#endif
                    free(fg_pixel_mask);
                } else {
#ifdef __ANDROID__
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 2, 2);
#else
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 5, 8);
#endif
                }

                int is_3d = (ctx && (ctx->header.flags & QTSQ_FLAG_3D));
                if (is_3d) {
                    QTSQ_TLOG("[QTSQ UPSCALE] Estimating 3D depth for %u splats\n", ns);
                    splat2d_estimate_depth(splats, ns, pixels, width, height, channels);
                }

                qsort(splats, ns, sizeof(splat2d_t), compare_splats_area);
            }
        }
        free(fg_map);
    }

    /* ── Step 4: Combine TRM3 + GSP2 into single compressed payload ── */
    if (ns > 0) {
        uint8_t *trm3_raw = NULL;
        size_t trm3_raw_n = 0;
        if (qtsq_uncompress_bounded(best_payload, best_payload_size, 256,
                                    1ULL << 30, &trm3_raw, &trm3_raw_n) == QTSQ_OK) {
            uLongf trm3_dl = (uLongf)trm3_raw_n;
            if (trm3_raw) {
                /* Pack splats */
                uint8_t *gsp2_data = NULL;
                size_t gsp2_sz = 0;
                int is_3d = (ctx && (ctx->header.flags & QTSQ_FLAG_3D));
                if (is_3d) {
                    splat2d_pack_gsp3(splats, ns, width, height, &gsp2_data, &gsp2_sz);
                } else {
                    splat2d_pack_gsp2(splats, ns, width, height, &gsp2_data, &gsp2_sz);
                }

                if (gsp2_data && gsp2_sz > 0) {
                    /* Binary search to find max splats that fit budget */
                    uint32_t best_ns = 0;
                    uint8_t *best_combined = NULL;
                    size_t best_combined_size = 0;

                    uint32_t lo = 1, hi = ns;
                    while (lo <= hi) {
                        uint32_t mid = lo + (hi - lo) / 2;
                        if (mid == 0) { lo = mid + 1; continue; }

                        uint8_t *test_gsp2 = NULL;
                        size_t test_gsp2_sz = 0;
                        if (is_3d) {
                            splat2d_pack_gsp3(splats, mid, width, height, &test_gsp2, &test_gsp2_sz);
                        } else {
                            splat2d_pack_gsp2(splats, mid, width, height, &test_gsp2, &test_gsp2_sz);
                        }

                        if (!test_gsp2 || test_gsp2_sz == 0) {
                            if (test_gsp2) free(test_gsp2);
                            hi = mid - 1;
                            continue;
                        }

                        size_t comb_raw_size = trm3_dl + test_gsp2_sz;
                        uint8_t *comb_raw = (uint8_t *)malloc(comb_raw_size);
                        if (!comb_raw) {
                            free(test_gsp2);
                            hi = mid - 1;
                            continue;
                        }
                        memcpy(comb_raw, trm3_raw, trm3_dl);
                        memcpy(comb_raw + trm3_dl, test_gsp2, test_gsp2_sz);

                        uint8_t *zd = NULL;
                        uLongf zb = 0;
                        int z_res;
                        if (ctx->codec == QTSQ_CODEC_QTC) {
                            size_t zsz = 0;
                            int qrc = qtsq_codec_compress(comb_raw, comb_raw_size, &zd, &zsz);
                            zb = (uLongf)zsz;
                            z_res = (qrc == QTSQ_OK) ? Z_OK : Z_BUF_ERROR;
                        } else {
                            zb = compressBound((uLong)comb_raw_size);
                            zd = (uint8_t *)malloc(zb);
                            if (!zd) {
                                free(comb_raw); free(test_gsp2);
                                hi = mid - 1;
                                continue;
                            }
                            z_res = compress2(zd, &zb, comb_raw, (uLong)comb_raw_size, 1);
                        }
                        if (z_res == Z_OK) {
                            size_t test_payload_size = 4 + (size_t)zb;
                            if (test_payload_size <= target_payload) {
                                best_ns = mid;
                                if (best_combined) free(best_combined);
                                best_combined = (uint8_t *)malloc(test_payload_size);
                                if (best_combined) {
                                    uint32_t new_uncomp = (uint32_t)comb_raw_size;
                                    memcpy(best_combined, &new_uncomp, 4);
                                    memcpy(best_combined + 4, zd, zb);
                                    best_combined_size = test_payload_size;
                                }
                                lo = mid + 1;
                            } else {
                                hi = mid - 1;
                            }
                        } else {
                            hi = mid - 1;
                        }
                        free(zd);
                        free(test_gsp2);
                        free(comb_raw);
                    }

                    if (best_combined) {
                        free(best_payload);
                        best_payload = best_combined;
                        best_payload_size = best_combined_size;
                        QTSQ_TLOG("[QTSQ UPSCALE] Splats: kept %u/%u (payload %zu bytes)\n",
                                  best_ns, ns, best_payload_size);
                        ns = best_ns;
                    } else {
                        QTSQ_TLOG("[QTSQ UPSCALE] Splats: discarded all %u (budget reached)\n", ns);
                        ns = 0;
                    }
                }
                if (gsp2_data) free(gsp2_data);
            }
            free(trm3_raw);
        }
    }
    if (splats) free(splats);

    /* Store compressed payload */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = best_payload;
    ctx->singularity_size = (uint32_t)best_payload_size;
    ctx->header.reserved32 = best_nv;

    /* Set header */
    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_HD;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "gravitational_upscale", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = ns;  /* Splat count for UI (not channels) */
    ctx->schema.num_dims = 3;
    if (ns > 0) {
        strncpy(ctx->schema.encoding, "TRI_SPLAT_UPSCALE_HD", 63);
        QTSQ_TLOG("[QTSQ UPSCALE] Hybrid: %u vertices + %u splats\n", best_nv, ns);
    } else {
        strncpy(ctx->schema.encoding, "TRI_RGB_UPSCALE_HD", 63);
    }
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    QTSQ_TLOG("[QTSQ UPSCALE] Total: %.1f ms | Input: %zu → Output: %zu bytes\n",
              qtsq_time_ms() - t_total, source_size, best_payload_size);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  GRAVITATIONAL LOSSLESS HD — Maximum Fidelity (6M vertices)
 *
 *  This mode prioritizes absolute quality using the max vertex budget.
 *  Output caps prevent unreasonable file sizes:
 *    - Input < 5MB  →  Output ≤ 10MB
 *    - Input ≥ 5MB  →  Output ≤ 30MB
 * ══════════════════════════════════════════════════════════════ */
int qtsq_compress_gravitational_lossless(qtsq_context_t *ctx,
                                          const uint8_t *pixels,
                                          uint32_t width, uint32_t height,
                                          uint8_t channels,
                                          size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    int guard = qtsq_require_creative(ctx, "gravitational_lossless");
    if (guard != QTSQ_OK) return guard;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;

    double t_total = qtsq_time_ms();
    double t_step;
    size_t pixel_count = (size_t)width * height;

    /* Budget: fill output cap via ZLib ratio estimate (~3.5:1) */
    size_t raw_pixels = pixel_count * channels;
    size_t max_output = (source_size < 5000000) ? 10000000 : 30000000;
    size_t target_payload = (size_t)(max_output * 3.5);
    if (target_payload > raw_pixels * 3) target_payload = raw_pixels * 3;

    size_t target_payload_verts = (size_t)(target_payload * 0.85);
    size_t target_payload_splats = target_payload - target_payload_verts;

    /* 6M vertex hard cap */
    uint32_t max_verts = (uint32_t)(target_payload_verts / 2);
    if (max_verts > 6000000) max_verts = 6000000;
    if (max_verts < 500000) max_verts = 500000;

    fprintf(stderr, "[QTSQ LOSSLESS] Input %zu bytes | max_output %zu | max_verts %u\n",
            source_size, max_output, max_verts);

    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    t_step = qtsq_time_ms();
    float *grad = compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }
    QTSQ_TLOG("[QTSQ LOSSLESS] Gradient: %.1f ms\n", qtsq_time_ms() - t_step);

    t_step = qtsq_time_ms();
    uint8_t *fg_map = NULL;
    uint32_t fg_bw = 0, fg_bh = 0, fg_block_size = 0;
    uint32_t nv = place_vertices(pixels, grad, width, height, channels,
                                  verts, max_verts, used,
                                  &fg_map, &fg_bw, &fg_bh, &fg_block_size);
    QTSQ_TLOG("[QTSQ LOSSLESS] Place vertices: %.1f ms (%u verts)\n", qtsq_time_ms() - t_step, nv);
    free(grad);
    free(used);

    uint8_t *best_payload = NULL;
    size_t best_payload_size = 0;
    uint32_t best_nv = nv;

#ifdef __ANDROID__
    if (pack_and_compress_trm3(verts, nv, width, height, channels,
                                &best_payload, &best_payload_size, 1, ctx->codec) != 0) {
#else
    if (pack_and_compress_trm3(verts, nv, width, height, channels,
                                &best_payload, &best_payload_size, 6, ctx->codec) != 0) {
#endif
        free(verts);
        if (fg_map) free(fg_map);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    /* Splats */
    t_step = qtsq_time_ms();
    int is_3d = (ctx && (ctx->header.flags & QTSQ_FLAG_3D));
    uint32_t max_splats = (uint32_t)(target_payload_splats / (is_3d ? 12 : 11));
    if (max_splats < 500) max_splats = 500;
    if (max_splats > 150000) max_splats = 150000;  /* Lossless: 150K splats for max fidelity */
    splat2d_t *splats = NULL;
    uint32_t ns = 0;

    if (fg_map) {
        splats = (splat2d_t *)malloc(max_splats * sizeof(splat2d_t));
        if (splats) {
            ns = splat2d_fit_background(pixels, width, height, channels,
                                         fg_map, fg_bw, fg_bh, fg_block_size,
                                         splats, max_splats);
            if (ns > 0) {
                uint8_t *fg_pixel_mask = (uint8_t *)calloc((size_t)width * height, sizeof(uint8_t));
                if (fg_pixel_mask) {
                    for (uint32_t by = 0; by < fg_bh; by++) {
                        for (uint32_t bx = 0; bx < fg_bw; bx++) {
                            uint8_t tier = fg_map[by * fg_bw + bx];
                            if (tier >= 2) {
                                uint32_t py0 = by * fg_block_size, py1 = py0 + fg_block_size;
                                if (py1 > height) py1 = height;
                                uint32_t px0 = bx * fg_block_size, px1 = px0 + fg_block_size;
                                if (px1 > width) px1 = width;
                                for (uint32_t py = py0; py < py1; py++)
                                    for (uint32_t px = px0; px < px1; px++)
                                        fg_pixel_mask[py * width + px] = tier;
                            }
                        }
                    }
#ifdef __ANDROID__
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 2, 2);
#else
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, fg_pixel_mask, 5, 8);
#endif
                    free(fg_pixel_mask);
                } else {
#ifdef __ANDROID__
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 2, 2);
#else
                    splat2d_refine_jittered_multiview(splats, ns, pixels, width, height, channels, NULL, 5, 8);
#endif
                }
                if (is_3d) splat2d_estimate_depth(splats, ns, pixels, width, height, channels);
                qsort(splats, ns, sizeof(splat2d_t), compare_splats_area);
            }
        }
        free(fg_map);
    }

    /* Combine TRM3 + GSP2 */
    if (ns > 0) {
        uint8_t *trm3_raw = NULL;
        size_t trm3_raw_n = 0;
        if (qtsq_uncompress_bounded(best_payload, best_payload_size, 256,
                                    1ULL << 30, &trm3_raw, &trm3_raw_n) == QTSQ_OK) {
            uLongf trm3_dl = (uLongf)trm3_raw_n;
            if (trm3_raw) {
                uint8_t *gsp2_data = NULL;  size_t gsp2_sz = 0;
                if (is_3d) splat2d_pack_gsp3(splats, ns, width, height, &gsp2_data, &gsp2_sz);
                else       splat2d_pack_gsp2(splats, ns, width, height, &gsp2_data, &gsp2_sz);
                if (gsp2_data && gsp2_sz > 0) {
                    size_t comb_raw_size = trm3_dl + gsp2_sz;
                    uint8_t *comb_raw = (uint8_t *)malloc(comb_raw_size);
                    if (comb_raw) {
                        memcpy(comb_raw, trm3_raw, trm3_dl);
                        memcpy(comb_raw + trm3_dl, gsp2_data, gsp2_sz);
                        uint8_t *zd = NULL;
                        uLongf zb = 0;
                        int z_res;
                        if (ctx->codec == QTSQ_CODEC_QTC) {
                            size_t zsz = 0;
                            int qrc = qtsq_codec_compress(comb_raw, comb_raw_size, &zd, &zsz);
                            zb = (uLongf)zsz;
                            z_res = (qrc == QTSQ_OK) ? Z_OK : Z_BUF_ERROR;
                        } else {
                            zb = compressBound((uLong)comb_raw_size);
                            zd = (uint8_t *)malloc(zb);
                            if (!zd) { z_res = Z_BUF_ERROR; }
                            else {
#ifdef __ANDROID__
                                z_res = compress2(zd, &zb, comb_raw, (uLong)comb_raw_size, 1);
#else
                                z_res = compress2(zd, &zb, comb_raw, (uLong)comb_raw_size, 6);
#endif
                            }
                        }
                        if (z_res == Z_OK) {
                            size_t new_size = 4 + zb;
                            uint8_t *np = (uint8_t *)malloc(new_size);
                            if (np) {
                                uint32_t nu = (uint32_t)comb_raw_size;
                                memcpy(np, &nu, 4);
                                memcpy(np + 4, zd, zb);
                                free(best_payload);
                                best_payload = np;
                                best_payload_size = new_size;
                            }
                        }
                        free(zd); free(comb_raw);
                    }
                }
                if (gsp2_data) free(gsp2_data);
            }
            free(trm3_raw);
        }
    }
    if (splats) free(splats);

    /* Store result */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = best_payload;
    ctx->singularity_size = (uint32_t)best_payload_size;
    ctx->header.reserved32 = best_nv;
    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA : QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_HD;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "gravitational_lossless", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = ns;
    ctx->schema.num_dims = 3;
    strncpy(ctx->schema.encoding, ns > 0 ? "TRI_SPLAT_LOSSLESS_HD" : "TRI_RGB_LOSSLESS_HD", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);

    QTSQ_TLOG("[QTSQ LOSSLESS] Total: %.1f ms | Input: %zu → Output: %zu bytes\n",
              qtsq_time_ms() - t_total, source_size, best_payload_size);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Parallel Triangle Rasterization Thread Worker
 *  Reconstructs a horizontal strip of rows via Barycentric interps
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const tri_vertex_t *verts;
    const tri_face_t *faces;
    uint32_t nf;
    uint8_t *output;
    uint32_t orig_w;
    uint32_t orig_h;
    uint8_t ch;
    uint8_t *blur_map;
    const uint8_t *tier_mask;  /* pixel-level tier ownership: skip write if tier < 2 */
    uint32_t start_row, end_row;  /* [start_row, end_row) */
    uint8_t *filled_map;       /* explicit coverage mask to preserve true black pixels */
} rasterize_thread_arg_t;

static inline void barycentric_inline(float px, float py,
                                      float x0, float y0, float x1, float y1,
                                      float x2, float y2,
                                      float *u, float *v, float *w) {
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-6f) { *u = *v = *w = 1.0f / 3.0f; return; }
    *u = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d;
    *v = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d;
    *w = 1.0f - *u - *v;
}

static void *rasterize_strip(void *arg) {
    rasterize_thread_arg_t *a = (rasterize_thread_arg_t *)arg;

    /* Initialize strip with black to ensure full coverage cleanly */
    size_t strip_bytes = (size_t)(a->end_row - a->start_row) * a->orig_w * a->ch;
    size_t start_idx = (size_t)a->start_row * a->orig_w * a->ch;
    memset(a->output + start_idx, 0, strip_bytes);

    for (uint32_t fi = 0; fi < a->nf; fi++) {
        const tri_vertex_t *v0 = &a->verts[a->faces[fi].v0];
        const tri_vertex_t *v1 = &a->verts[a->faces[fi].v1];
        const tri_vertex_t *v2 = &a->verts[a->faces[fi].v2];

        /* Calculate triangle area to determine blur strength */
        float area = 0.5f * fabsf(v0->x * (v1->y - v2->y) +
                                  v1->x * (v2->y - v0->y) +
                                  v2->x * (v0->y - v1->y));
        uint8_t passes = 0;
        if (area > 200.0f) passes = 2;          /* Background triangle: splats will paint over */
        else if (area > 50.0f) passes = 1;      /* Medium triangle: transition zone */
        /* Smaller triangles: passes stays 0, protected from splats */

        /* Bounding box */
        float minx = v0->x, maxx = v0->x;
        float miny = v0->y, maxy = v0->y;
        if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x;
        if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
        if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y;
        if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;

        int x0 = (int)floorf(minx); if (x0 < 0) x0 = 0;
        int y0i = (int)floorf(miny);
        int x1 = (int)ceilf(maxx); if (x1 >= (int)a->orig_w) x1 = (int)a->orig_w - 1;
        int y1i = (int)ceilf(maxy);

        /* Clip to this thread's designated row strip */
        if (y0i < (int)a->start_row) y0i = (int)a->start_row;
        if (y1i >= (int)a->end_row) y1i = (int)a->end_row - 1;
        if (y0i > y1i) continue; /* totally outside strip */

        for (int py = y0i; py <= y1i; py++) {
            for (int px = x0; px <= x1; px++) {
                /* tier_mask is used for blur_map (splat blending decisions) but
                 * NEVER to skip triangle rasterization — triangles are the base layer. */
                float cpx = (float)px + 0.5f;
                float cpy = (float)py + 0.5f;
                if (px == 0) cpx = 0.0f;
                else if (px == (int)a->orig_w - 1) cpx = (float)px;
                
                if (py == 0) cpy = 0.0f;
                else if (py == (int)a->orig_h - 1) cpy = (float)py;

                float u, v_b, wb;
                barycentric_inline(cpx, cpy,
                                   v0->x, v0->y, v1->x, v1->y, v2->x, v2->y,
                                   &u, &v_b, &wb);

                /* Inside triangle? */
                if (u >= -0.001f && v_b >= -0.001f && wb >= -0.001f) {
                    if (u < 0) u = 0; if (v_b < 0) v_b = 0; if (wb < 0) wb = 0;
                    float sum = u + v_b + wb;
                    if (sum > 0) { u /= sum; v_b /= sum; wb /= sum; }

                    float r = u * v0->r + v_b * v1->r + wb * v2->r;
                    float g = u * v0->g + v_b * v1->g + wb * v2->g;
                    float b = u * v0->b + v_b * v1->b + wb * v2->b;

                    /* Phase 1: Pure geometry interpolation */


                    size_t idx = ((size_t)py * a->orig_w + px) * a->ch;
                    a->output[idx] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r + 0.5f));
                    if (a->ch >= 2) a->output[idx+1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g + 0.5f));
                    if (a->ch >= 3) a->output[idx+2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b + 0.5f));
                    if (a->blur_map) a->blur_map[py * a->orig_w + px] = passes;
                    if (a->filled_map) a->filled_map[py * a->orig_w + px] = 1;
                }
            }
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Parallel Box Blur Smoothing Pass
 *  Flattens Mach bands & geometric artifacts as requested
 * ══════════════════════════════════════════════════════════════ */


/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_gravitational
 *  Triangle Mesh → Barycentric Rasterization
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_gravitational(qtsq_context_t *ctx,
                                   uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;

    /* SEC: decompressed size is file-declared — bound it (DoS guard).
     * Image payloads at 256:1 ratio cap; 1 GB absolute cap. */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(buf, ctx->singularity_size, 256, 1ULL << 30,
                                     &packed, &dl);
    if (rc != QTSQ_OK) return rc;

    size_t pos = 0;

    /* Check magic — support TRM1, TRM2, TRM3, TRM4, TRM5 */
    if (packed[0] != 'T' || packed[1] != 'R' || packed[2] != 'M') {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    uint8_t fmt_ver = packed[3]; /* '1'=interleaved, '2'=planar+faces, '3'=vertex-only, '4'=vertex+faces, '5'=adaptive */
    if (fmt_ver != '1' && fmt_ver != '2' && fmt_ver != '3' && fmt_ver != '4' && fmt_ver != '5') {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    pos = 4;

    uint32_t orig_w, orig_h;
    memcpy(&orig_w, packed + pos, 4); pos += 4;
    memcpy(&orig_h, packed + pos, 4); pos += 4;
    uint8_t ch_raw = packed[pos++];
    /* TRM4/TRM5 use bit 7 of channels byte as 16-bit face index flag */
    int face_idx_16 = ((fmt_ver == '4' || fmt_ver == '5') && (ch_raw & 0x80)) ? 1 : 0;
    uint8_t ch = ch_raw & 0x7F;
    uint32_t nv;
    memcpy(&nv, packed + pos, 4); pos += 4;

    uint32_t nf = 0;
    uint8_t trm5_max_depth = 0;
    if (fmt_ver == '1' || fmt_ver == '2' || fmt_ver == '4' || fmt_ver == '5') {
        memcpy(&nf, packed + pos, 4); pos += 4;
    }
    if (fmt_ver == '5') {
        trm5_max_depth = packed[pos++];  /* max subdivision depth (informational) */
    }
    
    /* Expose dimensions for UI to read (especially when decoding inside containers) */
    if (ctx) {
        qtsq_context_t *rw_ctx = (qtsq_context_t *)ctx;
        rw_ctx->schema.dimensions[0] = orig_w;
        rw_ctx->schema.dimensions[1] = orig_h;
        rw_ctx->schema.dimensions[2] = ch;
        rw_ctx->schema.dimensions[3] = nf;
    }

    if (ch < 1 || ch > 4) { free(packed); return QTSQ_ERR_FORMAT; }

    /* SEC: nv/nf come from the file — validate the minimum bytes the
     * vertex/face readers will consume against the actual payload size
     * before allocating or looping. Prevents heap over-read on crafted
     * files. (TRM1/TRM2: 7B/vertex + 6B/face; TRM3: 7B/vertex, no faces;
     * TRM4/5: 7B/vertex + 2B or 4B/face index.) */
    {
        uint64_t min_need = (uint64_t)nv * 7;
        switch (fmt_ver) {
            case '1':
            case '2':
                min_need += (uint64_t)nf * 6;
                break;
            case '4':
            case '5':
                min_need += (uint64_t)nf * (face_idx_16 ? 2u : 4u);
                break;
            default: /* TRM3 — no face data */
                break;
        }
        if ((uint64_t)pos + min_need > (uint64_t)dl) {
            free(packed);
            return QTSQ_ERR_FORMAT;
        }
    }

    /* Read vertices */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(nv * sizeof(tri_vertex_t));
    if (!verts) { free(packed); return QTSQ_ERR_ALLOC; }

    if (fmt_ver == '1') {
        /* TRM1: Interleaved layout with faces */
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vx, vy;
            memcpy(&vx, packed + pos, 2); pos += 2;
            memcpy(&vy, packed + pos, 2); pos += 2;
            verts[i].x = (float)vx;
            verts[i].y = (float)vy;
            verts[i].r = packed[pos++];
            verts[i].g = packed[pos++];
            verts[i].b = packed[pos++];
        }
        /* Skip face data */
        pos += (size_t)nf * 6;
    } else if (fmt_ver == '2') {
        /* TRM2: Planar layout with faces */
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vx; memcpy(&vx, packed + pos, 2); pos += 2;
            verts[i].x = (float)vx;
        }
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vy; memcpy(&vy, packed + pos, 2); pos += 2;
            verts[i].y = (float)vy;
        }
        for (uint32_t i = 0; i < nv; i++) verts[i].r = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].g = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].b = packed[pos++];
        /* Skip face data */
        pos += (size_t)nf * 6;
    } else if (fmt_ver == '4' || fmt_ver == '5') {
        /* TRM4/TRM5: Planar layout with stored face indices */
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vx; memcpy(&vx, packed + pos, 2); pos += 2;
            verts[i].x = (float)vx;
        }
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vy; memcpy(&vy, packed + pos, 2); pos += 2;
            verts[i].y = (float)vy;
        }
        for (uint32_t i = 0; i < nv; i++) verts[i].r = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].g = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].b = packed[pos++];
        /* Face indices are read below in the geometry section */
    } else {
        /* TRM3: Vertex-only planar layout — NO face data */
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vx; memcpy(&vx, packed + pos, 2); pos += 2;
            verts[i].x = (float)vx;
        }
        for (uint32_t i = 0; i < nv; i++) {
            uint16_t vy; memcpy(&vy, packed + pos, 2); pos += 2;
            verts[i].y = (float)vy;
        }
        for (uint32_t i = 0; i < nv; i++) verts[i].r = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].g = packed[pos++];
        for (uint32_t i = 0; i < nv; i++) verts[i].b = packed[pos++];
    }

    /* Save potential GSP2/GSP3 data before freeing packed buffer.
     * NOTE: For TRM4, we must NOT free packed yet — face indices are read below. */
    uint8_t *gsp2_buf = NULL;
    size_t gsp2_buf_size = 0;
    /* For TRM3, GSP starts right after vertex data. */
    if (fmt_ver != '4' && fmt_ver != '5' && fmt_ver != '6') {
        if (pos + 4 <= dl && packed[pos] == 'G' && packed[pos+1] == 'S' &&
            packed[pos+2] == 'P' && (packed[pos+3] == '2' || packed[pos+3] == '3')) {
            gsp2_buf_size = dl - pos;
            gsp2_buf = (uint8_t *)malloc(gsp2_buf_size);
            if (gsp2_buf) memcpy(gsp2_buf, packed + pos, gsp2_buf_size);
        }
        free(packed);
        packed = NULL;
    }

    /* Allocate output */
    size_t out_px = (size_t)orig_w * orig_h * ch;
    /* SEC: orig_w/orig_h come from the file — cap output size (DoS guard). */
    if (out_px > (1ULL << 30)) { free(packed); if (gsp2_buf) free(gsp2_buf); free(verts); return QTSQ_ERR_FORMAT; }
    uint8_t *output = NULL;
    if (*out_data != NULL) {
        output = *out_data;
    } else {
        output = (uint8_t *)malloc(out_px);
        if (!output) {
            free(packed);
            if (gsp2_buf) free(gsp2_buf);
            free(verts);
            return QTSQ_ERR_ALLOC;
        }
        *out_data = output;
    }
    memset(output, 0, out_px);
    /* ══════════════════════════════════════════════════════════════
     *  TRM3 Geometry Reconstruction & Rasterization
     *  Multi-threaded, top-to-bottom linear reconstruction
     * ══════════════════════════════════════════════════════════════ */

    tri_face_t *faces = NULL;
    
    if (fmt_ver == '3') {
        /* ── PIXEL-PERFECT FAST PATH ──
         * If vertex count == pixel count (1:1 mapping from triangle_pixel mode),
         * skip Delaunay triangulation entirely. Just write each vertex's color
         * directly to its (x,y) position. O(N) bit-exact reconstruction. */
        if (nv == (uint32_t)((uint64_t)orig_w * orig_h)) {
            fprintf(stderr, "[QTSQ] Pixel-perfect decode: %u vertices → %u×%u direct write\n",
                    nv, orig_w, orig_h);
            for (uint32_t i = 0; i < nv; i++) {
                uint32_t px = (uint32_t)(verts[i].x + 0.5f);
                uint32_t py = (uint32_t)(verts[i].y + 0.5f);
                if (px < orig_w && py < orig_h) {
                    size_t idx = ((size_t)py * orig_w + px) * ch;
                    output[idx] = verts[i].r;
                    if (ch >= 2) output[idx + 1] = verts[i].g;
                    if (ch >= 3) output[idx + 2] = verts[i].b;
                    if (ch >= 4) output[idx + 3] = 255;
                }
            }
            free(verts);
            if (gsp2_buf) free(gsp2_buf);
            *out_size = out_px;
            return QTSQ_OK;
        }

        /* TRM3: Local Delaunay Triangulation to reconstruct missing faces */
        uint32_t max_faces = nv * 2 + 100;
        tri_vertex_t *new_verts = (tri_vertex_t *)realloc(verts, (nv + 3) * sizeof(tri_vertex_t));
        faces = (tri_face_t *)malloc(max_faces * sizeof(tri_face_t));
        
        if (!new_verts || !faces) {
            /* realloc failure keeps the ORIGINAL verts block valid — free it,
             * plus faces (may be NULL), output, and gsp2_buf. */
            free(verts);
            if (faces) free(faces);
            if (gsp2_buf) free(gsp2_buf);
            free(output);
            return QTSQ_ERR_ALLOC;
        }
        verts = new_verts;
        
        /* Add sub-pixel jitter to prevent Bowyer-Watson co-circularity timeout on phone */
        for (uint32_t i = 0; i < nv; i++) {
            verts[i].x += ((float)(rand() % 1000) * 0.0001f - 0.05f);
            verts[i].y += ((float)(rand() % 1000) * 0.0001f - 0.05f);
        }
        
        nf = delaunay_triangulate(verts, nv, faces, max_faces, orig_w, orig_h);
        
        /* VERY IMPORTANT: remove the jitter before rendering! The random subpixel noise 
         * creates a "wobbly" heat-haze distortion that ruins sharp edges and text clarity.
         * TRM3 vertices are integers originally, so roundf() perfectly restores them. */
        for (uint32_t i = 0; i < nv; i++) {
            verts[i].x = roundf(verts[i].x);
            verts[i].y = roundf(verts[i].y);
        }
    } else if (fmt_ver == '4') {
        /* TRM4: Read pre-computed face indices directly — NO Delaunay needed! */
        faces = (tri_face_t *)malloc(nf * sizeof(tri_face_t));
        if (!faces) { free(verts); free(output); free(packed); return QTSQ_ERR_ALLOC; }

        if (face_idx_16) {
            /* 16-bit face indices */
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v0 = v; }
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v1 = v; }
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v2 = v; }
        } else {
            /* 32-bit face indices */
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v0, packed + pos, 4); pos += 4; }
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v1, packed + pos, 4); pos += 4; }
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v2, packed + pos, 4); pos += 4; }
        }
        fprintf(stderr, "[QTSQ HD] TRM4 fast decode: %u vertices + %u faces (no Delaunay)\n", nv, nf);

        /* Now check for GSP2/GSP3 data after face indices */
        if (pos + 4 <= dl && packed[pos] == 'G' && packed[pos+1] == 'S' &&
            packed[pos+2] == 'P' && (packed[pos+3] == '2' || packed[pos+3] == '3')) {
            gsp2_buf_size = dl - pos;
            gsp2_buf = (uint8_t *)malloc(gsp2_buf_size);
            if (gsp2_buf) memcpy(gsp2_buf, packed + pos, gsp2_buf_size);
        }
        free(packed);
        packed = NULL;
    } else if (fmt_ver == '5') {
        /* TRM5: Adaptive subdivision — pre-computed faces (NO Delaunay) */
        faces = (tri_face_t *)malloc(nf * sizeof(tri_face_t));
        if (!faces) { free(verts); free(output); free(packed); return QTSQ_ERR_ALLOC; }

        if (face_idx_16) {
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v0 = v; }
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v1 = v; }
            for (uint32_t i = 0; i < nf; i++) { uint16_t v; memcpy(&v, packed + pos, 2); pos += 2; faces[i].v2 = v; }
        } else {
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v0, packed + pos, 4); pos += 4; }
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v1, packed + pos, 4); pos += 4; }
            for (uint32_t i = 0; i < nf; i++) { memcpy(&faces[i].v2, packed + pos, 4); pos += 4; }
        }
        /* Skip per-face depth bytes — rasterizer doesn't need them */
        pos += nf;
        (void)trm5_max_depth;

        fprintf(stderr, "[QTSQ Phase 1] TRM%c decode: %u vertices + %u faces\n",
                fmt_ver, nv, nf);

        free(packed);
        packed = NULL;
    } else {
        /* TRM1/2 not supported for geometry */
        free(verts); free(output);
        return QTSQ_ERR_FORMAT;
    }


    /* ══════════════════════════════════════════════════════════════
     *  Parse TMB1 Tier Bitmap (if present after GSP2 in gsp2_buf)
     *  Used to build fg_mask for splats: tier >= 2 → foreground (triangle)
     * ══════════════════════════════════════════════════════════════ */
    uint8_t *tier_mask = NULL;  /* pixel-level: tier_mask[py * orig_w + px] */
    if (gsp2_buf && gsp2_buf_size > 0) {
        for (size_t scan = 0; scan + 16 <= gsp2_buf_size; scan++) {
            if (gsp2_buf[scan] == 'T' && gsp2_buf[scan+1] == 'M' &&
                gsp2_buf[scan+2] == 'B' && gsp2_buf[scan+3] == '1') {
                uint32_t tmb_bw, tmb_bh, tmb_bs;
                memcpy(&tmb_bw, gsp2_buf + scan + 4, 4);
                memcpy(&tmb_bh, gsp2_buf + scan + 8, 4);
                memcpy(&tmb_bs, gsp2_buf + scan + 12, 4);
                size_t tier_data_size = (size_t)tmb_bw * tmb_bh;
                if (scan + 16 + tier_data_size <= gsp2_buf_size && tmb_bs > 0) {
                    const uint8_t *tier_data = gsp2_buf + scan + 16;
                    tier_mask = (uint8_t *)calloc((size_t)orig_w * orig_h, sizeof(uint8_t));
                    if (tier_mask) {
                        for (uint32_t by = 0; by < tmb_bh; by++) {
                            for (uint32_t bx = 0; bx < tmb_bw; bx++) {
                                uint8_t tier = tier_data[by * tmb_bw + bx];
                                uint32_t py0 = by * tmb_bs;
                                uint32_t py1 = py0 + tmb_bs;
                                if (py1 > orig_h) py1 = orig_h;
                                uint32_t px0 = bx * tmb_bs;
                                uint32_t px1 = px0 + tmb_bs;
                                if (px1 > orig_w) px1 = orig_w;
                                for (uint32_t py = py0; py < py1; py++) {
                                    for (uint32_t px = px0; px < px1; px++) {
                                        tier_mask[py * orig_w + px] = tier;
                                    }
                                }
                            }
                        }
                        fprintf(stderr, "[QTSQ HD] TMB1 tier bitmap: %u×%u blocks (bs=%u)\n", tmb_bw, tmb_bh, tmb_bs);
                    }
                }
                break;
            }
        }
    }

    /* ══════════════════════════════════════════════════════════════
     *  Render Triangles FIRST (original order — rasterize_strip is unchanged)
     * ══════════════════════════════════════════════════════════════ */
    if (ctx) ((qtsq_context_t *)ctx)->schema.dimensions[3] = nf;

    double t_rast_start = qtsq_time_ms();
    uint8_t *blur_map = (uint8_t *)malloc((size_t)orig_w * orig_h);
    if (blur_map) memset(blur_map, 255, (size_t)orig_w * orig_h);

    int gpu_rast_ok = -1;
    extern int qtsq_use_gpu;
#ifdef HAVE_VULKAN_COMPUTE
    /* vkctx used later for GPU splat rendering (line ~2409) and cleanup (line ~2592).
     * Triangle rasterization does NOT use Vulkan here — vk_compute_rasterize()
     * needs a rasterize_spv pipeline but vk_compute_init() creates an IFS
     * decompress pipeline, causing a fatal device-corruption crash on destroy.
     * The CPU rasterize_strip is already parallelized across all cores. */
    vk_compute_ctx_t *vkctx = qtsq_use_gpu ? vk_compute_init() : NULL;
#else
    void *vkctx = NULL;
    (void)qtsq_use_gpu;
#endif


    uint8_t *filled = (uint8_t *)calloc(orig_w * orig_h, 1);
    if (gpu_rast_ok != 0) {
        /* ── CPU Path: rasterize_strip (unchanged — zeros and renders normally) ── */
        int num_cores = qtsq_num_cores();
        if ((uint32_t)num_cores > orig_h) num_cores = (int)orig_h;
        rasterize_thread_arg_t rargs[QTSQ_MAX_THREADS];
        for (int t = 0; t < num_cores; t++) {
            rargs[t].faces = faces;
            rargs[t].nf = nf;
            rargs[t].verts = verts;
            rargs[t].output = output;
            rargs[t].orig_w = orig_w;
            rargs[t].orig_h = orig_h;
            rargs[t].ch = ch;
            rargs[t].blur_map = blur_map;
            rargs[t].tier_mask = tier_mask;
            rargs[t].filled_map = filled;
            rargs[t].start_row = (uint32_t)((uint64_t)orig_h * t / num_cores);
            rargs[t].end_row   = (uint32_t)((uint64_t)orig_h * (t + 1) / num_cores);
        }
        qtsq_dispatch(rasterize_strip, rargs, sizeof(rasterize_thread_arg_t), num_cores);
        gpu_rast_ok = 0;
        fprintf(stderr, "[QTSQ HD] CPU rasterization (%d threads)\n", num_cores);
    }
    QTSQ_TLOG("[QTSQ HD] Rasterization: %.1f ms\n", qtsq_time_ms() - t_rast_start);

    free(faces);
    free(verts);

    /* Post-Process: Multi-pass iterative flood fill for uncovered pixels.
     * The Delaunay triangulation may only cover 30-60% of pixels on dense
     * upscale meshes. Each pass propagates color from any adjacent non-zero
     * neighbor (up/down/left/right). Repeats until 100% coverage. */
    {
        size_t total_px = (size_t)orig_w * orig_h;
        if (filled) {
            /* Count currently covered pixels directly from explicit map */
            size_t covered = 0;
            for (size_t i = 0; i < total_px; i++) {
                if (filled[i]) covered++;
            }
            QTSQ_TLOG("[QTSQ HD] Gap-fill: %zu/%zu pixels covered (%.1f%%)\n",
                      covered, total_px, 100.0 * covered / total_px);

            /* Iterative flood fill — max 200 passes (image diagonal) */
            for (int pass = 0; pass < 200 && covered < total_px; pass++) {
                size_t new_filled = 0;
                for (uint32_t py = 0; py < orig_h; py++) {
                    for (uint32_t px = 0; px < orig_w; px++) {
                        size_t pi = (size_t)py * orig_w + px;
                        if (filled[pi]) continue;

                        /* Try 4 neighbors: up, down, left, right */
                        size_t src = 0;
                        int found = 0;
                        if (py > 0 && filled[pi - orig_w]) { src = (pi - orig_w) * ch; found = 1; }
                        else if (py + 1 < orig_h && filled[pi + orig_w]) { src = (pi + orig_w) * ch; found = 1; }
                        else if (px > 0 && filled[pi - 1]) { src = (pi - 1) * ch; found = 1; }
                        else if (px + 1 < orig_w && filled[pi + 1]) { src = (pi + 1) * ch; found = 1; }

                        if (found) {
                            size_t dst = pi * ch;
                            for (uint8_t c = 0; c < ch; c++) output[dst + c] = output[src + c];
                            filled[pi] = 1;
                            new_filled++;
                        }
                    }
                }
                covered += new_filled;
                if (new_filled == 0) break;
            }
            QTSQ_TLOG("[QTSQ HD] Gap-fill complete: %zu/%zu covered\n", covered, total_px);
            free(filled);
        }
    }

    /* ══════════════════════════════════════════════════════════════
     *  Render Splats SECOND — on top of triangles.
     *  Uses tier_mask as fg_mask: splats only paint where tier < 2.
     *  If no TMB1, falls back to blur_map-based fg_mask (original behavior).
     * ══════════════════════════════════════════════════════════════ */
    if (gsp2_buf && gsp2_buf_size > 0) {
        double t_splat_start = qtsq_time_ms();
        splat2d_t *splats = NULL;
        uint32_t splat_count = 0;
        uint32_t sw = 0, sh = 0;
        int unpack_ok = -1;
        if (gsp2_buf_size >= 4 && gsp2_buf[3] == '3') {
            unpack_ok = splat2d_unpack_gsp3(gsp2_buf, gsp2_buf_size, &splats, &splat_count, &sw, &sh);
        } else {
            unpack_ok = splat2d_unpack_gsp2(gsp2_buf, gsp2_buf_size, &splats, &splat_count, &sw, &sh);
        }
        if (unpack_ok == 0 && splats && splat_count > 0) {
            /* Store splat count in [4] — do NOT overwrite [2] which holds channel count! */
            if (ctx) ((qtsq_context_t *)ctx)->schema.dimensions[4] = splat_count;

            /* Build fg_mask from blur_map: mark foreground (small/dense triangles)
             * so splats only paint over large/sparse background triangles. 
             * Then erode 2px to create clean buffer zone at boundaries. */
            uint8_t *fg_mask = (uint8_t *)calloc((size_t)orig_w * orig_h, sizeof(uint8_t));
            if (fg_mask && blur_map) {
                for (uint32_t y = 0; y < orig_h; y++) {
                    for (uint32_t x = 0; x < orig_w; x++) {
                        if (blur_map[y * orig_w + x] == 0) {
                            fg_mask[y * orig_w + x] = 1;
                        }
                    }
                }
                /* Erode: expand foreground mask by 2px to prevent splat bleed */
                uint8_t *eroded = (uint8_t *)malloc((size_t)orig_w * orig_h);
                if (eroded) {
                    memcpy(eroded, fg_mask, (size_t)orig_w * orig_h);
                    for (uint32_t y = 0; y < orig_h; y++) {
                        for (uint32_t x = 0; x < orig_w; x++) {
                            if (fg_mask[y * orig_w + x] == 0) continue;
                            for (int dy = -2; dy <= 2; dy++) {
                                for (int dx = -2; dx <= 2; dx++) {
                                    int ny = (int)y + dy, nx = (int)x + dx;
                                    if (ny < 0 || ny >= (int)orig_h || nx < 0 || nx >= (int)orig_w) continue;
                                    eroded[ny * orig_w + nx] = 1;
                                }
                            }
                        }
                    }
                    memcpy(fg_mask, eroded, (size_t)orig_w * orig_h);
                    free(eroded);
                }
            }

            int gpu_splat_ok = -1;
#ifdef __APPLE__
            /* Thread-safe singleton (pthread_once inside). */
            qtsq_metal_ctx_t *metal = qtsq_metal_global();
            if (metal) {
                gpu_splat_ok = qtsq_metal_render_splats(metal, splats, splat_count, orig_w, orig_h, ch, output, fg_mask);
            }
#else
#ifdef HAVE_VULKAN_COMPUTE
            if (vkctx) {
                vk_splat_t *vk_splats = (vk_splat_t *)malloc(splat_count * sizeof(vk_splat_t));
                if (vk_splats) {
                    for (uint32_t i = 0; i < splat_count; i++) {
                        vk_splats[i].x = splats[i].x; vk_splats[i].y = splats[i].y;
                        vk_splats[i].sx = splats[i].sx; vk_splats[i].sy = splats[i].sy;
                        vk_splats[i].theta = splats[i].theta; vk_splats[i].alpha = splats[i].alpha;
                        vk_splats[i].r = splats[i].r; vk_splats[i].g = splats[i].g;
                        vk_splats[i].b = splats[i].b; vk_splats[i].pad = 0;
                    }
                    gpu_splat_ok = vk_compute_render_splats(vkctx, vk_splats, splat_count,
                                                             orig_w, orig_h, output, fg_mask);
                    free(vk_splats);
                }
            }
#endif
#endif
            if (gpu_splat_ok != 0) {
                splat2d_render(splats, splat_count, output, orig_w, orig_h, fg_mask);
            }
            if (fg_mask) free(fg_mask);
            free(splats);
            QTSQ_TLOG("[QTSQ HD] Splat render: %.1f ms (%u splats)\n",
                      qtsq_time_ms() - t_splat_start, splat_count);
        }
        free(gsp2_buf);
    }

    /* ── Post-Processing Filters (TRM1, TRM2, TRM3 ONLY) ──
     * These algorithms were designed to hide structural noise and block artifacts
     * in older formats. For TRM4/TRM5 (Adaptive Subdivision), they destroy high-fidelity
     * details and wash out absolute blacks. We skip them to preserve raw sharpness. */
    if (fmt_ver < '4') {
        /* ── Edge-preserving bilateral filter (2-pass, 5×5 kernel) ── */
        {
            uint8_t *bilateral_out = (uint8_t *)malloc(out_px);
            if (bilateral_out) {
                const int radius = 2;
                const float sigma_s = 2.0f;
                const float sigma_c = 20.0f;
                const float inv_2sigma_s2 = 1.0f / (2.0f * sigma_s * sigma_s);
                const float inv_2sigma_c2 = 1.0f / (2.0f * sigma_c * sigma_c);

                /* Precompute spatial weights */
                float spatial_w[5][5];
                for (int dy = -radius; dy <= radius; dy++)
                    for (int dx = -radius; dx <= radius; dx++)
                        spatial_w[dy + radius][dx + radius] = expf(-(float)(dx*dx + dy*dy) * inv_2sigma_s2);

                for (int pass = 0; pass < 2; pass++) {
                    const uint8_t *src = (pass == 0) ? output : bilateral_out;
                    uint8_t *dst = (pass == 0) ? bilateral_out : output;

                    for (uint32_t y = 0; y < orig_h; y++) {
                        for (uint32_t x = 0; x < orig_w; x++) {
                            size_t idx = ((size_t)y * orig_w + x) * ch;
                            if (y < (uint32_t)radius || y >= orig_h - radius ||
                                x < (uint32_t)radius || x >= orig_w - radius) {
                                for (uint8_t c = 0; c < ch; c++) dst[idx + c] = src[idx + c];
                                continue;
                            }
                            for (uint8_t c = 0; c < ch; c++) {
                                float center_val = (float)src[idx + c];
                                float weighted_sum = 0.0f;
                                float weight_total = 0.0f;

                                for (int dy = -radius; dy <= radius; dy++) {
                                    for (int dx = -radius; dx <= radius; dx++) {
                                        size_t nidx = ((size_t)(y + dy) * orig_w + (x + dx)) * ch + c;
                                        float neighbor = (float)src[nidx];
                                        float color_diff = center_val - neighbor;
                                        float w = spatial_w[dy + radius][dx + radius] *
                                                expf(-color_diff * color_diff * inv_2sigma_c2);
                                        weighted_sum += w * neighbor;
                                        weight_total += w;
                                    }
                                }
                                dst[idx + c] = (uint8_t)(weighted_sum / weight_total + 0.5f);
                            }
                        }
                    }
                }
                free(bilateral_out);
            }
        }

        /* ── Boundary-aware unsharp mask ── */
        {
            uint8_t *usmb = (uint8_t *)malloc(out_px);
            if (usmb) {
                for (uint32_t y = 0; y < orig_h; y++) {
                    for (uint32_t x = 0; x < orig_w; x++) {
                        size_t idx = ((size_t)y * orig_w + x) * ch;
                        if (y == 0 || y == orig_h - 1 || x == 0 || x == orig_w - 1) {
                            for (uint8_t c = 0; c < ch; c++) usmb[idx + c] = output[idx + c];
                            continue;
                        }
                        for (uint8_t c = 0; c < ch; c++) {
                            int sum = 0;
                            for (int dy = -1; dy <= 1; dy++) {
                                for (int dx = -1; dx <= 1; dx++) {
                                    sum += output[((size_t)(y + dy) * orig_w + (x + dx)) * ch + c];
                                }
                            }
                            usmb[idx + c] = (uint8_t)(sum / 9);
                        }
                    }
                }

                for (uint32_t y = 1; y < orig_h - 1; y++) {
                    for (uint32_t x = 1; x < orig_w - 1; x++) {
                        size_t idx = ((size_t)y * orig_w + x) * ch;
                        float local_grad = 0.0f;
                        for (uint8_t c = 0; c < ch; c++) {
                            float gx = (float)output[((size_t)y * orig_w + x + 1) * ch + c] -
                                    (float)output[((size_t)y * orig_w + x - 1) * ch + c];
                            float gy = (float)output[((size_t)(y + 1) * orig_w + x) * ch + c] -
                                    (float)output[((size_t)(y - 1) * orig_w + x) * ch + c];
                            local_grad += gx * gx + gy * gy;
                        }
                        local_grad = sqrtf(local_grad / (float)ch);

                        float usm_strength;
                        if (local_grad > 60.0f) usm_strength = 0.1f;
                        else if (local_grad > 30.0f) usm_strength = 0.25f;
                        else usm_strength = 0.4f;

                        for (uint8_t c = 0; c < ch; c++) {
                            float orig_v = (float)output[idx + c];
                            float blur_v = (float)usmb[idx + c];
                            float sharp = orig_v + usm_strength * (orig_v - blur_v);
                            if (sharp < 0.0f) sharp = 0.0f;
                            if (sharp > 255.0f) sharp = 255.0f;
                            output[idx + c] = (uint8_t)(sharp + 0.5f);
                        }
                    }
                }
                free(usmb);
            }
        }

        /* ── Contrast stretch ── */
        {
            for (uint8_t c = 0; c < ch; c++) {
                uint32_t hist[256] = {0};
                size_t npx_total = (size_t)orig_w * orig_h;
                for (size_t i = 0; i < npx_total; i++) {
                    hist[output[i * ch + c]]++;
                }
                uint32_t lo_target = (uint32_t)(npx_total * 0.01);
                uint32_t hi_target = (uint32_t)(npx_total * 0.99);
                uint32_t lo_val = 0, hi_val = 255;
                uint32_t acc = 0;
                for (int v = 0; v < 256; v++) {
                    acc += hist[v];
                    if (acc >= lo_target) { lo_val = v; break; }
                }
                acc = 0;
                for (int v = 255; v >= 0; v--) {
                    acc += hist[v];
                    if (acc >= (npx_total - hi_target)) { hi_val = v; break; }
                }
                if (hi_val <= lo_val) continue;
                uint8_t lut[256];
                float scale = 255.0f / (float)(hi_val - lo_val);
                for (int v = 0; v < 256; v++) {
                    int stretched = (int)((float)(v - (int)lo_val) * scale + 0.5f);
                    if (stretched < 0) stretched = 0;
                    if (stretched > 255) stretched = 255;
                    lut[v] = (uint8_t)stretched;
                }
                for (size_t i = 0; i < npx_total; i++) {
                    output[i * ch + c] = lut[output[i * ch + c]];
                }
            }
        }
    }


    if (blur_map) free(blur_map);
    if (tier_mask) free(tier_mask);

#ifdef HAVE_VULKAN_COMPUTE
    if (vkctx) vk_compute_destroy(vkctx);
#else
    (void)vkctx;
#endif

    *out_data = output;
    *out_size = out_px;
    return QTSQ_OK;
}


/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_render_mesh
 *  Directly render a constructed mesh (used by AI optimization to
 *  bypass redundant Delaunay triangulation during decompression).
 * ══════════════════════════════════════════════════════════════ */
int qtsq_render_mesh(const tri_vertex_t *verts, uint32_t nv,
                     const tri_face_t *faces, uint32_t nf,
                     uint32_t orig_w, uint32_t orig_h, uint8_t ch,
                     uint8_t *output) {
    if (!verts || !faces || !output) return QTSQ_ERR_NULL;
    if (nv == 0 || nf == 0 || orig_w == 0 || orig_h == 0) return QTSQ_ERR_FORMAT;

    int num_cores;
#ifdef __APPLE__
    int mib[2] = { CTL_HW, HW_NCPU };
    size_t len = sizeof(num_cores);
    if (sysctl(mib, 2, &num_cores, &len, NULL, 0) != 0) num_cores = 4;
#else
    num_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 4;
#endif
    if (num_cores > 16) num_cores = 16;
    if ((uint32_t)num_cores > orig_h) num_cores = (int)orig_h;

    rasterize_thread_arg_t targs[16];
    pthread_t tids[16];

    /* Blur passes are DISABLED (they cause foggy halos around sharp text).
     * Don't allocate/pass blur_map — it was pure wasted work. */
    uint8_t *blur_map = NULL;

    for (int t = 0; t < num_cores; t++) {
        targs[t].verts = verts;
        targs[t].faces = faces;
        targs[t].nf = nf;
        targs[t].output = output;
        targs[t].blur_map = blur_map;
        targs[t].tier_mask = NULL;  /* No tier mask in direct render path */
        targs[t].orig_w = orig_w;
        targs[t].orig_h = orig_h;
        targs[t].ch = ch;
        targs[t].start_row = (uint32_t)((uint64_t)orig_h * t / num_cores);
        targs[t].end_row   = (uint32_t)((uint64_t)orig_h * (t + 1) / num_cores);
    }

    for (int t = 1; t < num_cores; t++) {
        if (pthread_create(&tids[t], NULL, rasterize_strip, &targs[t]) != 0) {
            /* Thread creation failed — run remaining strips on caller */
            for (int r = t; r < num_cores; r++)
                rasterize_strip(&targs[r]);
            num_cores = 1; /* skip joins below */
            break;
        }
    }
    if (num_cores > 1) {
        rasterize_strip(&targs[0]);
        for (int t = 1; t < num_cores; t++)
            pthread_join(tids[t], NULL);
    }

    /* Post-Process: Copy-from-above/left for extreme edge unrendered pixels (rare) */
    for (uint32_t py = 0; py < orig_h; py++) {
        for (uint32_t px = 0; px < orig_w; px++) {
            size_t idx = ((size_t)py * orig_w + px) * ch;
            if (output[idx] == 0 && (ch < 2 || output[idx+1] == 0) && (ch < 3 || output[idx+2] == 0)) {
                if (py > 0) {
                    size_t src = ((size_t)(py - 1) * orig_w + px) * ch;
                    for (uint8_t c = 0; c < ch; c++) output[idx + c] = output[src + c];
                } else if (px > 0) {
                    size_t src = ((size_t)py * orig_w + px - 1) * ch;
                    for (uint8_t c = 0; c < ch; c++) output[idx + c] = output[src + c];
                }
            }
        }
    }

    /* blur_map is NULL (disabled) — nothing to free. */
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  1D GRAVITATIONAL AUDIO COMPRESSION
 *
 *  Adapts the 2D GFC concept for audio signals:
 *    1. Mix stereo → mono
 *    2. Downsample by adaptive factor (2-6x)
 *    3. Quantize base to 8-bit, delta+RLE encode
 *    4. Select mass points at transients (high residual energy)
 *    5. Reconstruct via Wendland C² kernel in 1D
 *
 *  Packed format (26-byte header):
 *    [4] mono_samples  [1] orig_channels  [4] sample_rate
 *    [1] ds_factor     [4] base_samples
 *    [2] base_min      [2] base_max
 *    [4] base_comp_size [4] num_mass_points
 *    [base_comp_size bytes] delta+RLE encoded 8-bit base
 *    [num_mass_points * 6 bytes] (uint32 pos + int16 delta)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t pos;
    int16_t  delta;
} audio_mp_t;

static void audio_downsample(const int16_t *in, uint32_t n,
                              int16_t *out, uint32_t factor) {
    uint32_t m = n / factor;
    for (uint32_t i = 0; i < m; i++) {
        int32_t s = 0;
        for (uint32_t j = 0; j < factor; j++) s += in[i * factor + j];
        out[i] = (int16_t)(s / (int32_t)factor);
    }
}

static void audio_upsample(const int16_t *in, uint32_t in_n,
                             int16_t *out, uint32_t out_n) {
    if (in_n < 2) { for (uint32_t i = 0; i < out_n; i++) out[i] = in[0]; return; }
    for (uint32_t i = 0; i < out_n; i++) {
        float t = (float)i * (float)(in_n - 1) / (float)(out_n - 1);
        uint32_t idx = (uint32_t)t;
        float f = t - (float)idx;
        if (idx >= in_n - 1) out[i] = in[in_n - 1];
        else out[i] = (int16_t)((1.0f - f) * in[idx] + f * in[idx + 1]);
    }
}

static void audio_quantize(const int16_t *in, uint32_t n,
                             uint8_t *out, int16_t *mn, int16_t *mx) {
    *mn = in[0]; *mx = in[0];
    for (uint32_t i = 1; i < n; i++) {
        if (in[i] < *mn) *mn = in[i];
        if (in[i] > *mx) *mx = in[i];
    }
    int32_t range = (int32_t)*mx - (int32_t)*mn;
    if (range == 0) range = 1;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (uint8_t)(((int32_t)in[i] - (int32_t)*mn) * 255 / range);
}

static void audio_dequantize(const uint8_t *in, uint32_t n,
                               int16_t *out, int16_t mn, int16_t mx) {
    int32_t range = (int32_t)mx - (int32_t)mn;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (int16_t)((int32_t)mn + (int32_t)in[i] * range / 255);
}

static uint32_t audio_select_mp(const int16_t *orig, const int16_t *up,
                                 uint32_t n, audio_mp_t *pts, uint32_t max_pts) {
    if (max_pts == 0 || n == 0) return 0;
    uint32_t window = n / max_pts;
    if (window < 2) window = 2;
    uint32_t count = 0;

    for (uint32_t w = 0; w < n && count < max_pts; w += window) {
        uint32_t end = w + window;
        if (end > n) end = n;
        uint32_t bp = w;
        int32_t ba = 0, bd = 0;
        for (uint32_t i = w; i < end; i++) {
            int32_t d = (int32_t)orig[i] - (int32_t)up[i];
            int32_t ad = d < 0 ? -d : d;
            if (ad > ba) { ba = ad; bp = i; bd = d; }
        }
        if (ba > 32) {  /* lower threshold = more correction points = smoother */
            pts[count].pos = bp;
            if (bd > 32767) bd = 32767;
            if (bd < -32768) bd = -32768;
            pts[count].delta = (int16_t)bd;
            count++;
        }
    }
    return count;
}

int qtsq_compress_gravitational_audio(
    qtsq_context_t *ctx, const int16_t *pcm,
    uint32_t total_samples, uint8_t channels, uint32_t sample_rate) {

    if (!ctx || !pcm) return QTSQ_ERR_NULL;
    if (total_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (channels == 0) channels = 1;

    /* Mix to mono */
    uint32_t mono_n = total_samples / channels;
    int16_t *mono = (int16_t *)malloc(mono_n * sizeof(int16_t));
    if (!mono) return QTSQ_ERR_ALLOC;

    if (channels >= 2) {
        for (uint32_t i = 0; i < mono_n; i++)
            mono[i] = (int16_t)(((int32_t)pcm[i * channels] +
                                  (int32_t)pcm[i * channels + 1]) / 2);
    } else {
        for (uint32_t i = 0; i < mono_n; i++) mono[i] = pcm[i];
    }

    /* Adaptive downsample factor */
    uint32_t ds_factor;
    uint32_t max_mp;
    if (mono_n <= 44100) {          /* ≤1 sec */
        ds_factor = 2; max_mp = 2000;
    } else if (mono_n <= 441000) {  /* ≤10 sec */
        ds_factor = 3; max_mp = 5000;
    } else {
        ds_factor = 4; max_mp = 8000;
    }

    uint32_t base_n = mono_n / ds_factor;
    int16_t *base = (int16_t *)malloc(base_n * sizeof(int16_t));
    if (!base) { free(mono); return QTSQ_ERR_ALLOC; }

    audio_downsample(mono, mono_n, base, ds_factor);

    /* Quantize base to 8-bit */
    uint8_t *base_q = (uint8_t *)malloc(base_n);
    if (!base_q) { free(mono); free(base); return QTSQ_ERR_ALLOC; }

    int16_t base_min, base_max;
    audio_quantize(base, base_n, base_q, &base_min, &base_max);

    /* Upsample back for residual */
    int16_t *base_deq = (int16_t *)malloc(base_n * sizeof(int16_t));
    int16_t *up = (int16_t *)malloc(mono_n * sizeof(int16_t));
    if (!base_deq || !up) {
        free(mono); free(base); free(base_q);
        free(base_deq); free(up); return QTSQ_ERR_ALLOC;
    }

    audio_dequantize(base_q, base_n, base_deq, base_min, base_max);
    audio_upsample(base_deq, base_n, up, mono_n);
    free(base); free(base_deq);

    /* Select mass points from residual */
    audio_mp_t *mps = (audio_mp_t *)malloc(max_mp * sizeof(audio_mp_t));
    if (!mps) { free(mono); free(base_q); free(up); return QTSQ_ERR_ALLOC; }

    uint32_t num_mp = audio_select_mp(mono, up, mono_n, mps, max_mp);
    free(up); free(mono);

    /* Delta+RLE compress base */
    size_t base_comp_cap = base_n * 2 + 64;
    uint8_t *base_comp = (uint8_t *)malloc(base_comp_cap);
    if (!base_comp) { free(base_q); free(mps); return QTSQ_ERR_ALLOC; }

    size_t base_comp_sz = delta_rle_encode(base_q, base_n, base_comp, base_comp_cap);
    int base_raw = 0;
    if (base_comp_sz == 0 || base_comp_sz >= base_n) {
        memcpy(base_comp, base_q, base_n);
        base_comp_sz = base_n;
        base_raw = 1;
    }
    free(base_q);

    /* Pack:
     * [4] mono_n  [1] orig_channels  [4] sample_rate
     * [1] ds_factor  [4] base_n  [1] base_raw
     * [2] base_min  [2] base_max
     * [4] base_comp_sz  [4] num_mp
     * [base_comp_sz bytes]
     * [num_mp * 6 bytes]  (uint32 pos + int16 delta)
     */
    size_t hdr = 4 + 1 + 4 + 1 + 4 + 1 + 2 + 2 + 4 + 4;  /* 27 bytes */
    size_t mp_data_sz = (size_t)num_mp * sizeof(audio_mp_t);
    size_t total = hdr + base_comp_sz + mp_data_sz;

    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(base_comp); free(mps); return QTSQ_ERR_ALLOC; }

    size_t p = 0;
    memcpy(out + p, &mono_n, 4); p += 4;
    out[p++] = channels;
    memcpy(out + p, &sample_rate, 4); p += 4;
    out[p++] = (uint8_t)ds_factor;
    memcpy(out + p, &base_n, 4); p += 4;
    out[p++] = (uint8_t)base_raw;
    memcpy(out + p, &base_min, 2); p += 2;
    memcpy(out + p, &base_max, 2); p += 2;
    uint32_t bcs32 = (uint32_t)base_comp_sz;
    memcpy(out + p, &bcs32, 4); p += 4;
    memcpy(out + p, &num_mp, 4); p += 4;
    memcpy(out + p, base_comp, base_comp_sz); p += base_comp_sz;
    memcpy(out + p, mps, mp_data_sz); p += mp_data_sz;
    free(base_comp); free(mps);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = (uint32_t)p;

    ctx->header.strategy = QTSQ_STRATEGY_LAYERED;
    ctx->header.data_type = QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = total_samples * sizeof(int16_t);
    ctx->header.num_channels = 1; /* mono output */
    ctx->header.sample_depth = 16;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "audio_gravitational", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = mono_n;
    ctx->schema.num_dims = 1;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "1D_GFC_WENDLAND", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256((const uint8_t *)pcm, total_samples * sizeof(int16_t), ctx->header.checksum);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Decompress Gravitational Audio — 1D Wendland C² reconstruction
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_gravitational_audio(const qtsq_context_t *ctx,
                                          uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 27) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t p = 0;

    uint32_t mono_n; memcpy(&mono_n, buf + p, 4); p += 4;
    uint8_t orig_ch = buf[p++];
    uint32_t sr; memcpy(&sr, buf + p, 4); p += 4;
    uint8_t ds_factor = buf[p++];
    uint32_t base_n; memcpy(&base_n, buf + p, 4); p += 4;
    uint8_t base_raw = buf[p++];
    int16_t base_min, base_max;
    memcpy(&base_min, buf + p, 2); p += 2;
    memcpy(&base_max, buf + p, 2); p += 2;
    uint32_t base_comp_sz; memcpy(&base_comp_sz, buf + p, 4); p += 4;
    uint32_t num_mp; memcpy(&num_mp, buf + p, 4); p += 4;

    /* Every count below comes straight out of the blob, so validate them
     * against the bytes actually present before allocating or reading. A
     * hostile or truncated file must not steer an out-of-bounds access. */
    size_t avail = ctx->singularity_size - p;
    if (base_n > mono_n) return QTSQ_ERR_FORMAT;
    if ((uint64_t)base_comp_sz > (uint64_t)avail) return QTSQ_ERR_FORMAT;
    if ((uint64_t)num_mp * sizeof(audio_mp_t) > (uint64_t)(avail - base_comp_sz))
        return QTSQ_ERR_FORMAT;
    if (mono_n == 0 || mono_n > (1u << 30)) return QTSQ_ERR_FORMAT;

    (void)ds_factor; (void)orig_ch;

    /* Decode base */
    uint8_t *base_q = (uint8_t *)malloc(base_n);
    if (!base_q) return QTSQ_ERR_ALLOC;

    if (base_raw) {
        memcpy(base_q, buf + p, base_n);
    } else {
        size_t dec = delta_rle_decode(buf + p, base_comp_sz, base_q, base_n);
        if (dec != base_n) { free(base_q); return QTSQ_ERR_FORMAT; }
    }
    p += base_comp_sz;

    /* Dequantize + upsample */
    int16_t *base_pcm = (int16_t *)malloc(base_n * sizeof(int16_t));
    int16_t *mono = (int16_t *)calloc(mono_n, sizeof(int16_t));
    if (!base_pcm || !mono) {
        free(base_q); free(base_pcm); free(mono); return QTSQ_ERR_ALLOC;
    }

    audio_dequantize(base_q, base_n, base_pcm, base_min, base_max);
    free(base_q);

    audio_upsample(base_pcm, base_n, mono, mono_n);
    free(base_pcm);

    /* Apply mass point corrections (1D Wendland) */
    const audio_mp_t *mps = (const audio_mp_t *)(buf + p);
    int radius = 256;

    for (uint32_t i = 0; i < num_mp; i++) {
        uint32_t center = mps[i].pos;
        int16_t delta = mps[i].delta;
        uint32_t start = (center > (uint32_t)radius) ? center - radius : 0;
        uint32_t end = (center + radius < mono_n) ? center + radius : mono_n;

        for (uint32_t j = start; j < end; j++) {
            double dist = fabs((double)j - (double)center) / (double)radius;
            double w = wendland_c2(dist);
            int32_t v = (int32_t)mono[j] + (int32_t)(delta * w);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mono[j] = (int16_t)v;
        }
    }

    /* Build mono WAV */
    uint32_t pcm_bytes = mono_n * 2;
    uint32_t wav_sz = 44 + pcm_bytes;
    uint8_t *wav = (uint8_t *)malloc(wav_sz);
    if (!wav) { free(mono); return QTSQ_ERR_ALLOC; }

    uint32_t fsz = wav_sz - 8, brate = sr * 2, fc = 16;
    uint16_t af = 1, out_ch = 1, ba = 2, bits = 16;
    memcpy(wav, "RIFF", 4);       memcpy(wav + 4, &fsz, 4);
    memcpy(wav + 8, "WAVE", 4);   memcpy(wav + 12, "fmt ", 4);
    memcpy(wav + 16, &fc, 4);     memcpy(wav + 20, &af, 2);
    memcpy(wav + 22, &out_ch, 2); memcpy(wav + 24, &sr, 4);
    memcpy(wav + 28, &brate, 4);  memcpy(wav + 32, &ba, 2);
    memcpy(wav + 34, &bits, 2);   memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &pcm_bytes, 4);
    memcpy(wav + 44, mono, pcm_bytes);
    free(mono);

    *out_data = wav;
    *out_size = wav_sz;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  AI Optimization API
 *  Modifies existing mesh vertex COLORS from the AI model's
 *  corrected RGB output. Vertex positions are preserved.
 *  Future: Train ColonyNet to also predict position offsets.
 * ══════════════════════════════════════════════════════════════ */
int qtsq_optimize_mesh(qtsq_context_t *ctx, const uint8_t *rgb_target, 
                       uint32_t width, uint32_t height) {
    if (!ctx || !ctx->singularity || !rgb_target) return QTSQ_ERR_NULL;
    if (ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;
    
    const uint8_t *buf = ctx->singularity;
    
    /* SEC: bound file-declared decompressed size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(buf, ctx->singularity_size, 256, 1ULL << 30,
                                     &packed, &dl);
    if (rc != QTSQ_OK) return rc;
    
    // Check TRM3 magic
    if (packed[0] != 'T' || packed[1] != 'R' || packed[2] != 'M' || packed[3] != '3') {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    
    size_t pos = 4;
    pos += 8; // skip w, h
    uint8_t ch = packed[pos++];
    uint32_t nv;
    memcpy(&nv, packed + pos, 4); pos += 4;
    
    if (ch < 3) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    
    size_t x_offset = pos;
    size_t y_offset = pos + nv * 2;
    size_t r_offset = y_offset + nv * 2;
    size_t g_offset = r_offset + nv;
    size_t b_offset = g_offset + nv;
    
    if (b_offset + nv > dl) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    
    for (uint32_t i = 0; i < nv; i++) {
        uint16_t vx, vy;
        memcpy(&vx, packed + x_offset + i * 2, 2);
        memcpy(&vy, packed + y_offset + i * 2, 2);
        
        uint32_t ix = vx >= width ? width - 1 : vx;
        uint32_t iy = vy >= height ? height - 1 : vy;
        
        size_t t_idx = ((size_t)iy * width + ix) * 3;
        
        packed[r_offset + i] = rgb_target[t_idx];
        packed[g_offset + i] = rgb_target[t_idx + 1];
        packed[b_offset + i] = rgb_target[t_idx + 2];
    }
    
    // Recompress into new singularity buffer: qtc (opt-in) or zlib (default)
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(packed, dl, &stream, &stream_len) != QTSQ_OK) {
            free(packed); return QTSQ_ERR_FORMAT;
        }
    } else {
        uLongf new_dl = compressBound((uLong)dl);
        stream = (uint8_t *)malloc(new_dl);
        if (!stream) { free(packed); return QTSQ_ERR_ALLOC; }
        if (compress(stream, &new_dl, packed, (uLong)dl) != Z_OK) {
            free(packed); free(stream); return QTSQ_ERR_FORMAT;
        }
        stream_len = (size_t)new_dl;
    }

    uint8_t *new_payload = (uint8_t *)malloc(4 + stream_len);
    if (!new_payload) { free(packed); free(stream); return QTSQ_ERR_ALLOC; }
    uint32_t new_uncomp = (uint32_t)dl;
    memcpy(new_payload, &new_uncomp, 4);
    memcpy(new_payload + 4, stream, stream_len);
    free(stream);

    free(packed);
    free(ctx->singularity);

    ctx->singularity = new_payload;
    ctx->singularity_size = (uint64_t)(4 + stream_len);
    ctx->header.singularity_size = ctx->singularity_size;
    
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  AI V3 Hooks
 * ══════════════════════════════════════════════════════════════ */
int qtsq_extract_vertices(const qtsq_context_t *ctx, float *flat_verts, uint32_t max_verts) {
    if (!ctx || !ctx->singularity || !flat_verts) return -1;
    if (ctx->singularity_size < 8) return -1;
    
    /* SEC: bound file-declared decompressed size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    if (qtsq_uncompress_bounded(ctx->singularity, ctx->singularity_size,
                                256, 1ULL << 30, &packed, &dl) != QTSQ_OK) return -1;
    
    // Check TRM3 magic
    if (packed[0] != 'T' || packed[1] != 'R' || packed[2] != 'M' || packed[3] != '3') {
        free(packed); return -1;
    }
    
    size_t pos = 4;
    pos += 8; // skip w, h
    uint8_t ch = packed[pos++];
    uint32_t nv;
    memcpy(&nv, packed + pos, 4); pos += 4;
    
    if (ch < 3) {
        free(packed); return -1;
    }
    
    size_t x_offset = pos;
    size_t y_offset = pos + nv * 2;
    size_t r_offset = y_offset + nv * 2;
    size_t g_offset = r_offset + nv;
    size_t b_offset = g_offset + nv;
    
    if (b_offset + nv > dl) {
        free(packed); return -1;
    }
    
    uint32_t count = nv > max_verts ? max_verts : nv;
    
    for (uint32_t i = 0; i < count; i++) {
        uint16_t vx, vy;
        memcpy(&vx, packed + x_offset + i * 2, 2);
        memcpy(&vy, packed + y_offset + i * 2, 2);
        
        flat_verts[i * 5 + 0] = (float)vx;
        flat_verts[i * 5 + 1] = (float)vy;
        flat_verts[i * 5 + 2] = (float)packed[r_offset + i];
        flat_verts[i * 5 + 3] = (float)packed[g_offset + i];
        flat_verts[i * 5 + 4] = (float)packed[b_offset + i];
    }
    
    free(packed);
    return (int)count;
}

int qtsq_apply_vertices(qtsq_context_t *ctx, const float *new_verts, uint32_t num_verts) {
    if (!ctx || !ctx->singularity || !new_verts) return QTSQ_ERR_NULL;
    if (ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;
    
    const uint8_t *buf = ctx->singularity;
    
    /* SEC: bound file-declared decompressed size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(buf, ctx->singularity_size, 256, 1ULL << 30,
                                     &packed, &dl);
    if (rc != QTSQ_OK) return rc;
    
    // Check TRM magic — support TRM3 and TRM4
    if (packed[0] != 'T' || packed[1] != 'R' || packed[2] != 'M' ||
        (packed[3] != '3' && packed[3] != '4')) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    uint8_t fmt_ver = packed[3];
    
    size_t pos = 4;
    pos += 8; // skip w, h
    pos++;    // skip channels byte
    uint32_t nv;
    memcpy(&nv, packed + pos, 4); pos += 4;
    
    // TRM4 has a face count field after vertex count — skip it
    if (fmt_ver == '4') {
        pos += 4; // skip nf
    }
    
    size_t x_offset = pos;
    size_t y_offset = pos + nv * 2;
    size_t r_offset = y_offset + nv * 2;
    size_t g_offset = r_offset + nv;
    size_t b_offset = g_offset + nv;
    
    /* SEC: the write loop below writes count*7 bytes starting at x_offset.
     * Validate against the decompressed payload before writing. */
    if (b_offset + nv > dl) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    
    uint32_t count = nv > num_verts ? num_verts : nv;
    
    for (uint32_t i = 0; i < count; i++) {
        uint16_t vx = (uint16_t)(new_verts[i * 5 + 0] + 0.5f);
        uint16_t vy = (uint16_t)(new_verts[i * 5 + 1] + 0.5f);
        uint8_t r = (uint8_t)(new_verts[i * 5 + 2] + 0.5f);
        uint8_t g = (uint8_t)(new_verts[i * 5 + 3] + 0.5f);
        uint8_t b = (uint8_t)(new_verts[i * 5 + 4] + 0.5f);
        
        memcpy(packed + x_offset + i * 2, &vx, 2);
        memcpy(packed + y_offset + i * 2, &vy, 2);
        packed[r_offset + i] = r;
        packed[g_offset + i] = g;
        packed[b_offset + i] = b;
    }
    
    // Recompress into new singularity buffer: qtc (opt-in) or zlib (default)
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(packed, dl, &stream, &stream_len) != QTSQ_OK) {
            free(packed); return QTSQ_ERR_FORMAT;
        }
    } else {
        uLongf new_dl = compressBound((uLong)dl);
        stream = (uint8_t *)malloc(new_dl);
        if (!stream) { free(packed); return QTSQ_ERR_ALLOC; }
        if (compress(stream, &new_dl, packed, (uLong)dl) != Z_OK) {
            free(packed); free(stream); return QTSQ_ERR_FORMAT;
        }
        stream_len = (size_t)new_dl;
    }

    uint8_t *new_payload = (uint8_t *)malloc(4 + stream_len);
    if (!new_payload) { free(packed); free(stream); return QTSQ_ERR_ALLOC; }
    uint32_t new_uncomp = (uint32_t)dl;
    memcpy(new_payload, &new_uncomp, 4);
    memcpy(new_payload + 4, stream, stream_len);
    free(stream);

    free(packed);
    free(ctx->singularity);

    ctx->singularity = new_payload;
    ctx->singularity_size = (uint64_t)(4 + stream_len);
    ctx->header.singularity_size = ctx->singularity_size;
    
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Mesh Editor: Extract Gaussian Splats from TRM3 payload
 *  Returns number of splats extracted, or negative error code.
 * ══════════════════════════════════════════════════════════════ */
int qtsq_extract_splats(const qtsq_context_t *ctx, void *out_splats, uint32_t max_splats,
                         uint32_t *out_width, uint32_t *out_height) {
    if (!ctx || !ctx->singularity || !out_splats) return -1;
    if (ctx->singularity_size < 8) return -1;

    /* Forward decl for cross-TU hook (defined in qtsq_splat_only.c). */
    extern int qtsq_splat_only_unpack(const uint8_t *data, size_t size,
                                       splat2d_t **out_splats, uint32_t *out_count,
                                       uint32_t *out_width, uint32_t *out_height);

    /* Decompress zlib — SEC: bound file-declared size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    if (qtsq_uncompress_bounded(ctx->singularity, ctx->singularity_size,
                                256, 1ULL << 30, &packed, &dl) != QTSQ_OK) return -1;

    /* Try TRM3 mixed (triangle + GSP3) layout first. */
    if (dl >= 4 &&
        packed[0] == 'T' && packed[1] == 'R' && packed[2] == 'M' && packed[3] == '3') {
        /* Skip header to find GSP3 block */
        size_t pos = 4;
        pos += 4; /* orig_w */
        pos += 4; /* orig_h */
        uint8_t ch = packed[pos++];
        uint32_t nv;
        memcpy(&nv, packed + pos, 4); pos += 4;
        /* Skip vertex data: x(2*nv) + y(2*nv) + r(nv) + g(nv) + b(nv) */
        if (ch >= 3 && (size_t)pos + (size_t)nv * 7 <= dl) {
            pos += (size_t)nv * 2 + (size_t)nv * 2 + (size_t)nv * 3;
            /* Find GSP3 magic */
            int found = 0;
            for (size_t scan = pos; scan + 4 <= dl; scan++) {
                if (packed[scan] == 'G' && packed[scan+1] == 'S' && packed[scan+2] == 'P' &&
                    (packed[scan+3] == '3' || packed[scan+3] == '2')) {
                    size_t gsp_size = dl - scan;
                    splat2d_t *splats = NULL;
                    uint32_t count = 0, sw = 0, sh = 0;
                    int ok;
                    if (packed[scan+3] == '3') {
                        ok = splat2d_unpack_gsp3(packed + scan, gsp_size, &splats, &count, &sw, &sh);
                    } else {
                        ok = splat2d_unpack_gsp2(packed + scan, gsp_size, &splats, &count, &sw, &sh);
                    }
                    if (ok == 0 && splats && count > 0) {
                        uint32_t copy_n = count > max_splats ? max_splats : count;
                        memcpy(out_splats, splats, copy_n * sizeof(splat2d_t));
                        if (out_width)  *out_width  = sw;
                        if (out_height) *out_height = sh;
                        free(splats);
                        free(packed);
                        return (int)copy_n;
                    }
                    if (splats) free(splats);
                    found = 1;
                    break;
                }
            }
            free(packed);
            return found ? 0 : -1;
        }
        /* fall through if TRM3 but malformed */
        free(packed);
        return -1;
    }

    /* Phase 4a: standalone GSP4 / GSP5 splat-only payload (used by
     * qtsq_compress_splat_compact / splat_hd). Detect at start of decompressed
     * payload. */
    if (dl >= 17 &&
        packed[0] == 'G' && packed[1] == 'S' && packed[2] == 'P' &&
        (packed[3] == '4' || packed[3] == '5')) {
        splat2d_t *splats = NULL;
        uint32_t count = 0, sw = 0, sh = 0;
        if (qtsq_splat_only_unpack(packed, dl, &splats, &count, &sw, &sh) == 0 && splats && count > 0) {
            uint32_t copy_n = count > max_splats ? max_splats : count;
            memcpy(out_splats, splats, copy_n * sizeof(splat2d_t));
            if (out_width)  *out_width  = sw;
            if (out_height) *out_height = sh;
            free(splats);
            free(packed);
            return (int)copy_n;
        }
        if (splats) free(splats);
        free(packed);
        return -1;
    }

    free(packed);
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 *  Mesh Editor: Apply modified Gaussian Splats back into TRM3 payload
 *  Replaces the existing GSP3 block with new splat data.
 * ══════════════════════════════════════════════════════════════ */
int qtsq_apply_splats(qtsq_context_t *ctx, const void *splats, uint32_t count,
                       uint32_t width, uint32_t height) {
    if (!ctx || !ctx->singularity || !splats) return QTSQ_ERR_NULL;
    if (ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;

    /* Decompress zlib — SEC: bound file-declared size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(ctx->singularity, ctx->singularity_size,
                                     256, 1ULL << 30, &packed, &dl);
    if (rc != QTSQ_OK) return rc;

    /* Check TRM3 */
    if (packed[0] != 'T' || packed[1] != 'R' || packed[2] != 'M' || packed[3] != '3') {
        free(packed); return QTSQ_ERR_FORMAT;
    }

    /* Find GSP start offset */
    size_t pos = 4;
    pos += 4 + 4; /* w, h */
    uint8_t ch = packed[pos++];
    uint32_t nv;
    memcpy(&nv, packed + pos, 4); pos += 4;
    if (ch < 3) { free(packed); return QTSQ_ERR_FORMAT; }
    /* SEC: nv comes from the file — validate the vertex region before
     * advancing pos past it (prevents over-read below). */
    if ((uint64_t)pos + (uint64_t)nv * 7 > (uint64_t)dl) {
        free(packed); return QTSQ_ERR_FORMAT;
    }
    pos += (size_t)nv * 2 + (size_t)nv * 2 + (size_t)nv * 3;

    size_t gsp_start = 0;
    for (size_t scan = pos; scan + 4 <= dl; scan++) {
        if (packed[scan] == 'G' && packed[scan+1] == 'S' && packed[scan+2] == 'P') {
            gsp_start = scan;
            break;
        }
    }

    /* Pack new splats as GSP3 */
    uint8_t *new_gsp = NULL;
    size_t new_gsp_size = 0;
    int pack_rc = splat2d_pack_gsp3((const splat2d_t *)splats, count, width, height,
                                     &new_gsp, &new_gsp_size);
    if (pack_rc != 0 || !new_gsp) { free(packed); return QTSQ_ERR_ALLOC; }

    /* Build new payload: vertex data (up to gsp_start or pos) + new GSP3 */
    size_t prefix_end = gsp_start > 0 ? gsp_start : pos;
    size_t new_total = prefix_end + new_gsp_size;
    uint8_t *new_packed = (uint8_t *)malloc(new_total);
    if (!new_packed) { free(packed); free(new_gsp); return QTSQ_ERR_ALLOC; }

    memcpy(new_packed, packed, prefix_end);
    memcpy(new_packed + prefix_end, new_gsp, new_gsp_size);
    free(new_gsp);
    free(packed);

    /* Recompress: qtc (opt-in) or zlib (default) */
    uint32_t new_uncomp = (uint32_t)new_total;
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(new_packed, new_total, &stream, &stream_len) != QTSQ_OK) {
            free(new_packed); return QTSQ_ERR_FORMAT;
        }
    } else {
        uLongf comp_bound = compressBound((uLong)new_total);
        stream = (uint8_t *)malloc(comp_bound);
        if (!stream) { free(new_packed); return QTSQ_ERR_ALLOC; }
        uLongf comp_dl = comp_bound;
        if (compress(stream, &comp_dl, new_packed, (uLong)new_total) != Z_OK) {
            free(new_packed); free(stream); return QTSQ_ERR_FORMAT;
        }
        stream_len = (size_t)comp_dl;
    }

    uint8_t *new_payload = (uint8_t *)malloc(4 + stream_len);
    if (!new_payload) { free(new_packed); free(stream); return QTSQ_ERR_ALLOC; }
    memcpy(new_payload, &new_uncomp, 4);
    memcpy(new_payload + 4, stream, stream_len);
    free(stream);
    free(new_packed);

    free(ctx->singularity);
    ctx->singularity = new_payload;
    ctx->singularity_size = (uint64_t)(4 + stream_len);
    ctx->header.singularity_size = ctx->singularity_size;

    return QTSQ_OK;
}
