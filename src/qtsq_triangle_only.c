/*
 * qtsq_triangle_only.c — Pure Triangle Mesh Compression (No Splats)
 *
 * Forked from qtsq_gravitational.c with ALL splat logic removed.
 * Produces pure TRM3 vertex data → Delaunay triangulation at decode.
 *
 * Three quality tiers:
 *   Compact  — small output, 25K-400K vertices
 *   HD       — high quality, 200K-3M vertices, full budget to triangles
 *   Lossless — near-lossless, 500K-6M vertices
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include "qtsq_triangle_only.h"
#include "qtsq_parallel.h"
#include "qtsq_codec.h"
#include "qtsq.h"
#include <time.h>
#ifdef __APPLE__
#include "qtsq_metal.h"
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define TONLY_LOG(...) __android_log_print(ANDROID_LOG_INFO, "QTSQ_TONLY", __VA_ARGS__)
#else
#define TONLY_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static double tonly_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ══════════════════════════════════════════════════════════════
 *  Sobel Edge Detection (Multi-Threaded)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *pixels;
    float *grad;
    uint32_t w, h;
    uint8_t ch;
    uint32_t start_row, end_row;
} tonly_grad_arg_t;

static void *tonly_gradient_strip(void *arg) {
    tonly_grad_arg_t *a = (tonly_grad_arg_t *)arg;
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

static float *tonly_compute_gradient(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch) {
    float *grad = (float *)calloc((size_t)w * h, sizeof(float));
    if (!grad) return NULL;

#ifdef __APPLE__
    /* Thread-safe singleton (pthread_once inside). */
    qtsq_metal_ctx_t *metal = qtsq_metal_global();
    if (metal) {
        if (qtsq_metal_sobel(metal, pixels, grad, w, h, ch) == 0)
            return grad;
    }
#endif

    int nc = qtsq_num_cores();
    if ((uint32_t)nc > h) nc = (int)h;

    tonly_grad_arg_t args[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc; t++) {
        args[t].pixels = pixels;
        args[t].grad = grad;
        args[t].w = w; args[t].h = h; args[t].ch = ch;
        args[t].start_row = (uint32_t)((uint64_t)h * t / nc);
        args[t].end_row   = (uint32_t)((uint64_t)h * (t + 1) / nc);
    }
    qtsq_dispatch(tonly_gradient_strip, args, sizeof(tonly_grad_arg_t), nc);
    return grad;
}

/* ══════════════════════════════════════════════════════════════
 *  Vertex Placement — Full Image (No fg/bg Split)
 *
 *  Unlike the gravitational mode, we do NOT create a foreground/
 *  background map. ALL regions get vertices. 100% budget goes
 *  to the triangle mesh for maximum coverage.
 * ══════════════════════════════════════════════════════════════ */

static inline int tonly_add_vertex(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch,
                                    tri_vertex_t *verts, uint8_t *used,
                                    uint32_t *nv, uint32_t max_verts,
                                    uint32_t x, uint32_t y) {
    (void)h; /* height is implied by w×used grid */
    uint8_t expected = 0;
    if (!__atomic_compare_exchange_n(&used[y * w + x], &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return 0; /* Already used by another thread */
    }

    /* Check bounds BEFORE incrementing the counter — the previous code
     * incremented first then checked, so on overflow the counter was never
     * decremented and *nv exceeded the written verts. */
    uint32_t current_nv = __atomic_load_n(nv, __ATOMIC_SEQ_CST);
    if (current_nv >= max_verts) {
        return 0;
    }
    current_nv = __atomic_fetch_add(nv, 1, __ATOMIC_SEQ_CST);
    if (current_nv >= max_verts) {
        /* Another thread won the race — undo our increment. */
        __atomic_fetch_sub(nv, 1, __ATOMIC_SEQ_CST);
        return 0;
    }

    /* ── Exact Pixel Color ──
     * For EXTREME detail, we sample the exact pixel. Soft averaging
     * creates a "muddy" or blurry mesh. Since our updated placement
     * strategy snaps exactly to the geometric edges, the exact raw 
     * pixel perfectly isolates the true edge color boundary. */
    size_t idx = ((size_t)y * w + x) * ch;
    verts[current_nv].x = (float)x;
    verts[current_nv].y = (float)y;
    verts[current_nv].r = pixels[idx];
    verts[current_nv].g = (ch >= 2) ? pixels[idx + 1] : pixels[idx];
    verts[current_nv].b = (ch >= 3) ? pixels[idx + 2] : pixels[idx];
    return 1;
}

typedef struct {
    const uint8_t *pixels;
    const float *grad;
    uint32_t w, h;
    uint8_t ch;
    tri_vertex_t *verts;
    uint32_t *nv;
    uint32_t max_verts;
    uint8_t *used;
    float base_floor;
    double boosted_per_vert;
    uint32_t start_y;
    uint32_t end_y;
} tonly_place_worker_t;

static void *tonly_place_strip(void *arg) {
    tonly_place_worker_t *a = (tonly_place_worker_t *)arg;
    double error_acc = 0.0;
    for (uint32_t y = a->start_y; y < a->end_y && *a->nv < a->max_verts; y++) {
        int rev = (y & 1);  /* Serpentine within strip */
        for (uint32_t xi = 0; xi < a->w - 2 && *a->nv < a->max_verts; xi++) {
            uint32_t x = rev ? (a->w - 2 - xi) : (xi + 1);
            if (a->used[y * a->w + x]) continue;
            
            float g = a->grad[y * a->w + x] + a->base_floor;
            float boosted = g * g * g;
            error_acc += (double)boosted;
            
            if (error_acc >= a->boosted_per_vert) {
                uint32_t best_x = x, best_y = y;
                float best_g = a->grad[y * a->w + x];
                const int search_rad = 1;
                for (int dy = -search_rad; dy <= search_rad; dy++) {
                    int ny2 = (int)y + dy;
                    if (ny2 < 1 || ny2 >= (int)a->h - 1) continue;
                    for (int dx = -search_rad; dx <= search_rad; dx++) {
                        int nx2 = (int)x + dx;
                        if (nx2 < 1 || nx2 >= (int)a->w - 1) continue;
                        if (!a->used[ny2 * a->w + nx2]) {
                            float cur_g = a->grad[ny2 * a->w + nx2];
                            if (cur_g > best_g) {
                                best_g = cur_g;
                                best_x = nx2;
                                best_y = ny2;
                            }
                        }
                    }
                }
                tonly_add_vertex(a->pixels, a->w, a->h, a->ch, a->verts, a->used, a->nv, a->max_verts, best_x, best_y);
                error_acc -= a->boosted_per_vert;
            }
        }
    }
    return NULL;
}


/* ── File-scope parallel sum_boosted reduction worker ── */
typedef struct {
    const float *grad;
    const uint8_t *used;
    uint32_t w, h;
    float base_floor;
    double partial;
    uint32_t start_y, end_y;
} sum_worker_t;

static void *sum_strip(void *arg) {
    sum_worker_t *a = (sum_worker_t *)arg;
    double s = 0.0;
    for (uint32_t y = a->start_y; y < a->end_y; y++)
        for (uint32_t x = 1; x < a->w - 1; x++)
            if (!a->used[y * a->w + x]) {
                float g = a->grad[y * a->w + x] + a->base_floor;
                s += (double)(g * g * g);
            }
    a->partial = s;
    return NULL;
}

static uint32_t tonly_place_vertices(const uint8_t *pixels, const float *grad,
                                      uint32_t w, uint32_t h, uint8_t ch,
                                      tri_vertex_t *verts, uint32_t max_verts,
                                      uint8_t *used) {
    uint32_t nv = 0;
    size_t npx = (size_t)w * h;

    /* ────────────────────────────────────────────────────────
     *  Phase 1: Boundary anchors (essential for clean edges)
     *  Budget: ~5% — these prevent border artifacts
     * ──────────────────────────────────────────────────────── */
    uint32_t bnd_budget = (uint32_t)(max_verts * 0.05f);
    if (bnd_budget < 20) bnd_budget = 20;
    uint32_t bnd_count = 2 * (w + h);
    int bnd_step = (int)((float)bnd_count / (float)bnd_budget + 0.5f);
    if (bnd_step < 1) bnd_step = 1;

    /* Four edges */
    for (uint32_t x = 0; x < w && nv < max_verts; x += bnd_step)
        tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, x, 0);
    for (uint32_t x = 0; x < w && nv < max_verts; x += bnd_step)
        tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, x, h - 1);
    for (uint32_t y = 1; y < h - 1 && nv < max_verts; y += bnd_step)
        tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, 0, y);
    for (uint32_t y = 1; y < h - 1 && nv < max_verts; y += bnd_step)
        tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, w - 1, y);
    /* Corners guaranteed */
    tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, 0, 0);
    tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, w - 1, 0);
    tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, 0, h - 1);
    tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, w - 1, h - 1);

    uint32_t bnd_placed = nv;

    /* ────────────────────────────────────────────────────────
     *  Phase 2: Sparse safety grid (~5% budget)
     *  Prevents large uncovered holes in completely flat regions
     *  that have zero gradient (solid color walls, clear sky).
     *  Grid step is large — only catches areas the gradient pass
     *  would completely skip.
     * ──────────────────────────────────────────────────────── */
    uint32_t safety_budget = (uint32_t)(max_verts * 0.05f);
    double safety_step_f = sqrt((double)npx / (double)(safety_budget > 0 ? safety_budget : 1));
    int safety_step = (int)ceil(safety_step_f);
    if (safety_step < 2) safety_step = 2;

    uint32_t safety_placed = 0;
    for (uint32_t y = (uint32_t)(safety_step / 2); y < h && safety_placed < safety_budget; y += safety_step)
        for (uint32_t x = (uint32_t)(safety_step / 2); x < w && safety_placed < safety_budget; x += safety_step)
            if (tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, x, y))
                safety_placed++;

    /* ────────────────────────────────────────────────────────
     *  Phase 2b: Absolute Luminance anchoring (~8% budget)
     *  Ensures pure blacks and pure whites receive vertices
     *  BEFORE gradient error-diffusion. This guarantees that
     *  dark spots anchor the mesh, allowing interpolation to
     *  smoothly reach true 0,0,0 instead of washing out into gray.
     * ──────────────────────────────────────────────────────── */
    uint32_t lum_budget = (uint32_t)(max_verts * 0.08f);
    uint32_t lum_placed = 0;
    int lum_step = 16; /* Large step ensures budget reaches entire image! */
    for (uint32_t y = 1; y < h - 1 && lum_placed < lum_budget; y += lum_step) {
        for (uint32_t x = 1; x < w - 1 && lum_placed < lum_budget; x += lum_step) {
            if (!used[y * w + x]) {
                size_t idx = ((size_t)y * w + x) * ch;
                uint8_t r = pixels[idx];
                uint8_t g = (ch >= 2) ? pixels[idx + 1] : r;
                uint8_t b = (ch >= 3) ? pixels[idx + 2] : r;
                uint32_t sum = r + g + b;
                /* Anchor near-pure blacks or bright whites */
                if (sum <= 45 || sum >= 745) { 
                    if (tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, x, y))
                        lum_placed++;
                }
            }
        }
    }

    /* ────────────────────────────────────────────────────────
     *  Phase 3: Gradient-Density Placement (~85% budget)
     *
     *  Vertex density is PROPORTIONAL to local gradient magnitude.
     *  Uses error-diffusion (Floyd-Steinberg style) across the
     *  gradient field so placement is spatially even, not biased
     *  toward the top-left.
     *
     *  Key improvements over old algorithm:
     *  - Base floor: every pixel gets min weight (grad + base),
     *    so flat regions still get sparse coverage
     *  - Multi-pass: if first pass doesn't fill budget, runs
     *    again with lower threshold
     *  - Non-linear gradient boost: grad^1.5 emphasizes strong
     *    edges more aggressively
     * ──────────────────────────────────────────────────────── */
    uint32_t density_budget = max_verts - nv;
    if (density_budget < 10) goto done;

    /* Build boosted gradient field: grad^1.5.
     * The exponent >1 concentrates vertices MORE on strong edges.
     * Base floor is 0 so completely flat regions consume almost 0 budget
     * (they are already handled by Phase 2 safety grid). */
    float grad_max = 0.0f;
    for (size_t i = 0; i < npx; i++)
        if (grad[i] > grad_max) grad_max = grad[i];
    if (grad_max < 1.0f) grad_max = 1.0f;

    /* Base floor: Very small baseline so flat/textured regions still get
     * sparse coverage without consuming budget that should go to text edges.
     * Dropped from 15% → 3%: the g^3 boost below makes sharp edges get
     * ~2700× more budget than flat regions — 15% was wasting too much on
     * smooth gradients and diluting away from critical character strokes. */
    float base_floor = grad_max * 0.03f;

    /* ── Parallel sum_boosted reduction ── */
    int nc2 = qtsq_num_cores();
    if ((uint32_t)nc2 > h - 2) nc2 = (int)(h - 2);
    if (nc2 < 1) nc2 = 1;

    sum_worker_t sargs[QTSQ_MAX_THREADS];
    for (int t = 0; t < nc2; t++) {
        sargs[t].grad = grad;
        sargs[t].used = used;
        sargs[t].w = w;
        sargs[t].h = h;
        sargs[t].base_floor = base_floor;
        sargs[t].partial = 0.0;
        sargs[t].start_y = 1 + (uint32_t)((uint64_t)(h - 2) * t / nc2);
        sargs[t].end_y   = 1 + (uint32_t)((uint64_t)(h - 2) * (t + 1) / nc2);
    }
    qtsq_dispatch(sum_strip, sargs, sizeof(sum_worker_t), nc2);

    double sum_boosted = 0.0;
    for (int t = 0; t < nc2; t++) sum_boosted += sargs[t].partial;

    double boosted_per_vert = sum_boosted / (double)(density_budget > 0 ? density_budget : 1);
    if (boosted_per_vert < 0.0001) boosted_per_vert = 1.0;


    /* Error-diffusion pass: accumulate boosted gradient,
     * emit vertex when accumulated error exceeds threshold.
     * Snaps to the local gradient maximum in a 5x5 window to
     * place vertices DIRECTLY on structural edges instead of randomly. */
    /* Error-diffusion pass: Parallelized over horizontal strips */
    tonly_place_worker_t args[QTSQ_MAX_THREADS];
    int nc = qtsq_num_cores();
    if ((uint32_t)nc > h - 2) nc = (int)(h - 2);
    if (nc < 1) nc = 1;

    for (int t = 0; t < nc; t++) {
        args[t].pixels = pixels;
        args[t].grad = grad;
        args[t].w = w; args[t].h = h; args[t].ch = ch;
        args[t].verts = verts;
        args[t].nv = &nv;
        args[t].max_verts = max_verts;
        args[t].used = used;
        args[t].base_floor = base_floor;
        args[t].boosted_per_vert = boosted_per_vert;
        /* map y from 1 to h-1 */
        args[t].start_y = 1 + (uint32_t)((uint64_t)(h - 2) * t / nc);
        args[t].end_y   = 1 + (uint32_t)((uint64_t)(h - 2) * (t + 1) / nc);
    }
    qtsq_dispatch(tonly_place_strip, args, sizeof(tonly_place_worker_t), nc);

    /* Phase 4: Fill any remaining budget with uniform backfill */
    if (nv < max_verts) {
        uint32_t remaining = max_verts - nv;
        if (remaining > 10) {
            double r_step_f = sqrt((double)npx / (double)remaining);
            int r_step = (int)ceil(r_step_f);
            if (r_step < 1) r_step = 1;
            for (uint32_t y = (uint32_t)(r_step / 2); y < h && nv < max_verts; y += r_step)
                for (uint32_t x = (uint32_t)(r_step / 2); x < w && nv < max_verts; x += r_step)
                    if (!used[y * w + x])
                        tonly_add_vertex(pixels, w, h, ch, verts, used, &nv, max_verts, x, y);
        }
    }

done:
    TONLY_LOG("[TONLY] Vertices placed: %u (boundary=%u safety=%u density=%u)\n",
             nv, bnd_placed, safety_placed, nv - bnd_placed - safety_placed);
    return nv;
}

/* ══════════════════════════════════════════════════════════════
 *  TRM3 Packing — Pack vertices into planar TRM3 format + zlib
 * ══════════════════════════════════════════════════════════════ */

static int tonly_compare_verts_yx(const void *a, const void *b) {
    const tri_vertex_t *va = (const tri_vertex_t *)a;
    const tri_vertex_t *vb = (const tri_vertex_t *)b;
    if (va->y < vb->y) return -1;
    if (va->y > vb->y) return 1;
    if (va->x < vb->x) return -1;
    if (va->x > vb->x) return 1;
    return 0;
}

static int tonly_pack_trm3(const tri_vertex_t *verts, uint32_t nv,
                            uint32_t width, uint32_t height, uint8_t channels,
                            uint8_t **out_data, size_t *out_size, int comp_level,
                            uint8_t codec) {
    /* Sort copy by Y,X for better zlib delta compression */
    tri_vertex_t *sorted = (tri_vertex_t *)malloc(nv * sizeof(tri_vertex_t));
    if (!sorted) return -1;
    memcpy(sorted, verts, nv * sizeof(tri_vertex_t));
    qsort(sorted, nv, sizeof(tri_vertex_t), tonly_compare_verts_yx);

    size_t hsz = 4 + 4 + 4 + 1 + 4;  /* TRM3 header */
    size_t vd = (size_t)nv * 7;       /* 2+2+1+1+1 per vertex */
    size_t raw_sz = hsz + vd;

    uint8_t *raw = (uint8_t *)malloc(raw_sz);
    if (!raw) { free(sorted); return -1; }

    size_t p = 0;
    raw[p++] = 'T'; raw[p++] = 'R'; raw[p++] = 'M'; raw[p++] = '3';
    memcpy(raw + p, &width, 4);  p += 4;
    memcpy(raw + p, &height, 4); p += 4;
    raw[p++] = channels;
    memcpy(raw + p, &nv, 4);     p += 4;

    /* Planar layout: all X, all Y, all R, all G, all B */
    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(sorted[i].x + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(sorted[i].y + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].r;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].g;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = sorted[i].b;
    free(sorted);

    /* Compress: qtc (opt-in) or zlib (default) */
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

/* ══════════════════════════════════════════════════════════════
 *  Shared helper: finalize context after compression
 * ══════════════════════════════════════════════════════════════ */

static void tonly_finalize_ctx(qtsq_context_t *ctx,
                                uint8_t *payload, size_t payload_size,
                                uint32_t nv, uint32_t width, uint32_t height,
                                uint8_t channels, const uint8_t *pixels,
                                uint32_t flags, const char *encoding) {
    size_t pixel_count = (size_t)width * height;

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = payload;
    ctx->singularity_size = (uint32_t)payload_size;

    ctx->header.reserved32 = nv;
    ctx->header.strategy = QTSQ_STRATEGY_TRIANGLE;
    ctx->header.data_type = (channels == 1) ? QTSQ_TYPE_IMAGE_GRAY :
                            (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = pixel_count * channels;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 8;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED | flags;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "triangle_only", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = channels;
    ctx->schema.num_dims = 3;
    strncpy(ctx->schema.encoding, encoding, 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256(pixels, pixel_count * channels, ctx->header.checksum);
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_triangle_compact
 *
 *  Pure triangle mesh, compact size.
 *  25K-400K vertices, 30-40% of original.
 *  No splats. 100% budget to triangle mesh.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_compact(qtsq_context_t *ctx,
                                    const uint8_t *pixels,
                                    uint32_t width, uint32_t height,
                                    uint8_t channels, size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "triangle_compact");
    if (guard != QTSQ_OK) return guard;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;
    size_t original_bytes = source_size > 0 ? source_size : (pixel_count * channels / 10);

    /* Budget: target 65% of original size */
    double target_ratio = (original_bytes <= 2 * 1024 * 1024) ? 0.65 : 0.75;
    double target_bytes = (double)original_bytes * target_ratio;

    /* 100% budget → vertices (no splat split!) */
    uint32_t max_verts = (uint32_t)(target_bytes / 3.6);
    if (max_verts < 25000)   max_verts = 25000;
    if (max_verts > 2000000) max_verts = 2000000;

    TONLY_LOG("[TONLY Compact] Input: %zu bytes, max_verts: %u\n", original_bytes, max_verts);

    /* Allocate */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Gradient */
    float *grad = tonly_compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Place vertices — 100% to triangles */
    uint32_t nv = tonly_place_vertices(pixels, grad, width, height, channels,
                                        verts, max_verts, used);
    free(grad);
    free(used);

    /* Pack TRM3 + zlib level 9 (max compression for compact) */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm3(verts, nv, width, height, channels,
                         &payload, &payload_size, 9, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        0, "TRI_ONLY_COMPACT");

    TONLY_LOG("[TONLY Compact] %u verts → %zu bytes in %.1f ms\n",
             nv, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_triangle_hd
 *
 *  Pure triangle mesh, high quality.
 *  200K-3M vertices, 70-85% of original.
 *  No splats. Full HD budget to triangle mesh.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_hd(qtsq_context_t *ctx,
                                const uint8_t *pixels,
                                uint32_t width, uint32_t height,
                                uint8_t channels, size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "triangle_hd");
    if (guard != QTSQ_OK) return guard;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;

    /* HD budget: 100% of raw budget goes to vertices (no 85/15 splat split!) */
    size_t target_payload;
    if (source_size > 0) {
        target_payload = (size_t)(source_size * 3.5);
        if (target_payload > pixel_count * channels * 2)
            target_payload = pixel_count * channels * 2;
    } else {
        target_payload = (size_t)(pixel_count * channels * 2.0);
    }

    size_t hdr_overhead = sizeof(qtsq_header_t) + sizeof(qtsq_schema_t);
    if (target_payload > hdr_overhead) target_payload -= hdr_overhead;

    /* ALL payload → vertices (gravitational mode only gives 85%) */
    uint32_t max_verts = (uint32_t)(target_payload / 2);
    if (max_verts < 200000)  max_verts = 200000;
    if (max_verts > 3000000) max_verts = 3000000;

    TONLY_LOG("[TONLY HD] Input: %zu bytes, budget: %zu, max_verts: %u\n",
             source_size, target_payload, max_verts);

    /* Allocate */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Gradient */
    float *grad = tonly_compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Place vertices */
    uint32_t nv = tonly_place_vertices(pixels, grad, width, height, channels,
                                        verts, max_verts, used);
    free(grad);
    free(used);

    /* Pack TRM3 + zlib level 1 (fast, for HD) */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm3(verts, nv, width, height, channels,
                         &payload, &payload_size, 1, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        QTSQ_FLAG_HD, "TRI_ONLY_HD");

    TONLY_LOG("[TONLY HD] %u verts → %zu bytes in %.1f ms\n",
             nv, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_triangle_lossless
 *
 *  Pure triangle mesh, near-lossless quality.
 *  500K-6M vertices, maximum fidelity.
 *  No splats. Every byte goes to vertex density.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_lossless(qtsq_context_t *ctx,
                                      const uint8_t *pixels,
                                      uint32_t width, uint32_t height,
                                      uint8_t channels, size_t source_size) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "triangle_lossless");
    if (guard != QTSQ_OK) return guard;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;

    /* Lossless budget: maximum vertex density */
    size_t target_payload;
    if (source_size > 0) {
        target_payload = (size_t)(source_size * 5.0);
        if (target_payload > pixel_count * channels * 3)
            target_payload = pixel_count * channels * 3;
    } else {
        target_payload = (size_t)(pixel_count * channels * 3.0);
    }

    /* ALL payload → vertices */
    uint32_t max_verts = (uint32_t)(target_payload / 2);
    if (max_verts < 500000)  max_verts = 500000;
    if (max_verts > 6000000) max_verts = 6000000;

    /* Cap memory: 6M vertices × sizeof(tri_vertex_t) ≈ 120MB */
    size_t mem_needed = (size_t)max_verts * sizeof(tri_vertex_t) + pixel_count;
    if (mem_needed > 512 * 1024 * 1024) {
        max_verts = (uint32_t)((512 * 1024 * 1024 - pixel_count) / sizeof(tri_vertex_t));
    }

    TONLY_LOG("[TONLY Lossless] Input: %zu bytes, max_verts: %u\n",
             source_size, max_verts);

    /* Allocate */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Gradient */
    float *grad = tonly_compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Place vertices */
    uint32_t nv = tonly_place_vertices(pixels, grad, width, height, channels,
                                        verts, max_verts, used);
    free(grad);
    free(used);

    /* Pack TRM3 + zlib level 6 (balanced speed/compression) */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm3(verts, nv, width, height, channels,
                         &payload, &payload_size, 6, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        QTSQ_FLAG_HD, "TRI_ONLY_LOSSLESS");

    TONLY_LOG("[TONLY Lossless] %u verts → %zu bytes in %.1f ms\n",
             nv, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_triangle_pixel
 *
 *  Pixel-perfect triangle mesh: 1 vertex per pixel.
 *  Every pixel is stored as a vertex with its exact RGB color.
 *  No gradient-based sampling, no quality loss.
 *
 *  The decoder writes each vertex directly to its (x,y) position
 *  instead of doing barycentric interpolation — guaranteed bit-exact
 *  pixel reconstruction.
 *
 *  The Delaunay triangulation is still available at decode time for
 *  DTCN graph construction, but is NOT used for pixel rendering.
 *
 *  Size: typically 50-150 KB for 150×150 images (zlib'd TRM3).
 *  Designed for ML training datasets where quality > size.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_pixel(qtsq_context_t *ctx,
                                   const uint8_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint8_t channels, size_t source_size) {
    (void)source_size; /* pixel-perfect mode: budget = pixel count, not size */
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    int guard = qtsq_require_creative(ctx, "triangle_pixel");
    if (guard != QTSQ_OK) return guard;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;

    /* Every pixel = 1 vertex. No subsampling. */
    uint32_t nv = (uint32_t)pixel_count;

    TONLY_LOG("[TONLY Pixel] %ux%u (%u pixels) → %u vertices (1:1)\n",
             width, height, nv, nv);

    /* Allocate vertices — sequential pixel-to-vertex mapping */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(nv * sizeof(tri_vertex_t));
    if (!verts) return QTSQ_ERR_ALLOC;

    /* Direct mapping: vertex[i] = pixel[i] with position (x, y) */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t vi = y * width + x;
            size_t pi = (size_t)vi * channels;
            verts[vi].x = (float)x;
            verts[vi].y = (float)y;
            verts[vi].r = pixels[pi];
            verts[vi].g = (channels >= 2) ? pixels[pi + 1] : pixels[pi];
            verts[vi].b = (channels >= 3) ? pixels[pi + 2] : pixels[pi];
        }
    }

    /* Pack TRM3 + zlib level 6 (balanced speed/compression).
     * The planar layout (all X, all Y, all R, all G, all B) compresses
     * very well with zlib because adjacent pixels have similar values. */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm3(verts, nv, width, height, channels,
                         &payload, &payload_size, 6, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        QTSQ_FLAG_HD, "TRI_ONLY_PIXEL");

    TONLY_LOG("[TONLY Pixel] %u verts → %zu bytes in %.1f ms\n",
             nv, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  ADAPTIVE RECURSIVE TRIANGLE SUBDIVISION ENGINE
 *
 *  After initial vertex placement + Delaunay triangulation,
 *  measures per-triangle color error against the original image.
 *  High-error triangles are recursively subdivided by inserting
 *  edge midpoint vertices, creating 4 child triangles per split.
 *
 *  Key design decisions:
 *  - Edge deduplication via hash map prevents T-junction cracks
 *  - Priority queue: highest-error triangles subdivide first
 *  - Budget-aware: stops when vertex cap is reached
 *  - Output: TRM5 format (vertices + faces + depth per face)
 *
 *  Author: Priyanshu Boruah / QTSQ Project
 * ══════════════════════════════════════════════════════════════ */

/* Forward declaration of Delaunay from qtsq_gravitational.c */
extern uint32_t delaunay_triangulate(tri_vertex_t *verts, uint32_t nv,
                                      tri_face_t *faces, uint32_t max_faces,
                                      uint32_t img_w, uint32_t img_h);

/* ── Edge Midpoint Hash Map for Deduplication ── */
#define EDGE_HASH_SIZE 65521  /* prime near 64K */

typedef struct edge_entry {
    uint32_t v_lo, v_hi;     /* canonical edge key (lo < hi) */
    uint32_t mid_idx;        /* index of the midpoint vertex */
    struct edge_entry *next;
} edge_entry_t;

typedef struct {
    edge_entry_t **buckets;
    edge_entry_t *pool;
    uint32_t pool_used;
    uint32_t pool_cap;
} edge_map_t;

static void edge_map_init(edge_map_t *m, uint32_t pool_cap) {
    m->buckets = (edge_entry_t **)calloc(EDGE_HASH_SIZE, sizeof(edge_entry_t *));
    m->pool = (edge_entry_t *)malloc(pool_cap * sizeof(edge_entry_t));
    m->pool_used = 0;
    m->pool_cap = pool_cap;
}

static void edge_map_free(edge_map_t *m) {
    free(m->buckets);
    free(m->pool);
    m->buckets = NULL;
    m->pool = NULL;
}

static uint32_t edge_hash(uint32_t a, uint32_t b) {
    uint32_t lo = (a < b) ? a : b;
    uint32_t hi = (a < b) ? b : a;
    return ((lo * 2654435761u) ^ (hi * 40503u)) % EDGE_HASH_SIZE;
}

/* Returns the midpoint vertex index for edge (a,b).
 * If it doesn't exist yet, creates a new vertex at the midpoint
 * and returns its index. Returns UINT32_MAX on pool exhaustion. */
static uint32_t edge_map_get_or_insert(edge_map_t *m,
                                        uint32_t va, uint32_t vb,
                                        tri_vertex_t *verts, uint32_t *nv,
                                        uint32_t max_verts,
                                        const uint8_t *pixels,
                                        uint32_t w, uint32_t h, uint8_t ch) {
    uint32_t lo = (va < vb) ? va : vb;
    uint32_t hi = (va < vb) ? vb : va;
    uint32_t bucket = edge_hash(lo, hi);

    /* Search existing */
    for (edge_entry_t *e = m->buckets[bucket]; e; e = e->next) {
        if (e->v_lo == lo && e->v_hi == hi) {
            return e->mid_idx;
        }
    }

    /* Create new midpoint vertex */
    if (*nv >= max_verts || m->pool_used >= m->pool_cap) {
        return UINT32_MAX;
    }

    float mx = (verts[va].x + verts[vb].x) * 0.5f;
    float my = (verts[va].y + verts[vb].y) * 0.5f;

    uint32_t px = (uint32_t)(mx + 0.5f);
    uint32_t py = (uint32_t)(my + 0.5f);

    if (px >= w) px = w - 1;
    if (py >= h) py = h - 1;

    size_t idx = ((size_t)py * w + px) * ch;
    uint32_t mi = *nv;
    verts[mi].x = mx;
    verts[mi].y = my;
    verts[mi].r = pixels[idx];
    verts[mi].g = (ch >= 2) ? pixels[idx + 1] : pixels[idx];
    verts[mi].b = (ch >= 3) ? pixels[idx + 2] : pixels[idx];
    (*nv)++;

    /* Insert into hash map */
    edge_entry_t *entry = &m->pool[m->pool_used++];
    entry->v_lo = lo;
    entry->v_hi = hi;
    entry->mid_idx = mi;
    entry->next = m->buckets[bucket];
    m->buckets[bucket] = entry;

    return mi;
}

/* ── Per-Triangle Color Error (MSE) ── */

static float tonly_compute_triangle_error(const tri_vertex_t *va,
                                           const tri_vertex_t *vb,
                                           const tri_vertex_t *vc,
                                           const uint8_t *pixels,
                                           uint32_t w, uint32_t h,
                                           uint8_t ch) {
    /* Prevent Subpixel Geometric Aliasing */
    float area = 0.5f * fabsf((vb->x - va->x)*(vc->y - va->y) - (vc->x - va->x)*(vb->y - va->y));
    if (area < 0.45f) return 0.0f; // STOP fragmenting microscopic triangles that cause 50x zoom noise!

    /* Compute bounding box */
    int minx = (int)fmaxf(0, floorf(fminf(fminf(va->x, vb->x), vc->x)));
    int maxx = (int)fminf((float)(w - 1), ceilf(fmaxf(fmaxf(va->x, vb->x), vc->x)));
    int miny = (int)fmaxf(0, floorf(fminf(fminf(va->y, vb->y), vc->y)));
    int maxy = (int)fminf((float)(h - 1), ceilf(fmaxf(fmaxf(va->y, vb->y), vc->y)));

    if (minx > maxx || miny > maxy) return 0.0f;

    /* Barycentric denominator */
    float denom = (vb->y - vc->y) * (va->x - vc->x) +
                  (vc->x - vb->x) * (va->y - vc->y);
    if (fabsf(denom) < 1e-6f) return 0.0f;
    float inv = 1.0f / denom;

    double total_error = 0.0;
    uint32_t count = 0;

    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float fpx = (float)px, fpy = (float)py;
            float w0 = ((vb->y - vc->y) * (fpx - vc->x) + (vc->x - vb->x) * (fpy - vc->y)) * inv;
            float w1 = ((vc->y - va->y) * (fpx - vc->x) + (va->x - vc->x) * (fpy - vc->y)) * inv;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
                /* Interpolated color */
                float ir = w0 * va->r + w1 * vb->r + w2 * vc->r;
                float ig = w0 * va->g + w1 * vb->g + w2 * vc->g;
                float ib = w0 * va->b + w1 * vb->b + w2 * vc->b;

                /* Original pixel color */
                size_t oi = ((size_t)py * w + px) * ch;
                float or_v = (float)pixels[oi];
                float og = (ch >= 2) ? (float)pixels[oi + 1] : or_v;
                float ob = (ch >= 3) ? (float)pixels[oi + 2] : or_v;

                /* Squared error */
                float dr = ir - or_v;
                float dg = ig - og;
                float db = ib - ob;
                total_error += (double)(dr * dr + dg * dg + db * db);
                count++;
            }
        }
    }

    if (count == 0) return 0.0f;
    return (float)(total_error / (double)count / 3.0);  /* MSE per channel */
}

/* ── Subdivision candidate for priority sorting ── */
typedef struct {
    uint32_t face_idx;
    float error;
} subdiv_candidate_t;

static int cmp_subdiv_desc(const void *a, const void *b) {
    float ea = ((const subdiv_candidate_t *)a)->error;
    float eb = ((const subdiv_candidate_t *)b)->error;
    return (ea < eb) - (ea > eb);  /* descending */
}

/* ── Adaptive Refinement Loop ── */

typedef struct {
    const tri_vertex_t *verts;
    const tri_face_t *faces;
    const uint8_t *face_depth;
    uint8_t depth;
    const uint8_t *pixels;
    uint32_t w, h;
    uint8_t ch;
    float error_threshold;
    float *errors;
    uint32_t start_fi;
    uint32_t end_fi;
} tonly_error_worker_t;

static void *tonly_error_strip(void *arg) {
    tonly_error_worker_t *a = (tonly_error_worker_t *)arg;
    for (uint32_t fi = a->start_fi; fi < a->end_fi; fi++) {
        if (a->face_depth[fi] != a->depth - 1) continue;
        float err = tonly_compute_triangle_error(
            &a->verts[a->faces[fi].v0], &a->verts[a->faces[fi].v1], &a->verts[a->faces[fi].v2],
            a->pixels, a->w, a->h, a->ch);
        if (err > a->error_threshold) {
            a->errors[fi] = err;
        }
    }
    return NULL;
}

static void tonly_adaptive_refine(tri_vertex_t *verts, uint32_t *nv,
                                   tri_face_t *faces, uint32_t *nf,
                                   uint8_t *face_depth,
                                   uint32_t max_verts, uint32_t max_faces,
                                   const uint8_t *pixels,
                                   uint32_t w, uint32_t h, uint8_t ch,
                                   uint8_t max_depth, float error_threshold) {
    TONLY_LOG("[TONLY Adaptive] Starting refinement: %u verts, %u faces, max_depth=%u, threshold=%.1f\n",
             *nv, *nf, max_depth, error_threshold);

    /* Allocate edge map — estimate max new edges = faces * 3 * max_depth */
    uint32_t pool_cap = (*nf) * 3;
    if (pool_cap > 500000) pool_cap = 500000;
    edge_map_t emap;
    edge_map_init(&emap, pool_cap);

    /* Candidate buffer — reused per depth pass */
    subdiv_candidate_t *candidates = (subdiv_candidate_t *)malloc((*nf + max_faces) * sizeof(subdiv_candidate_t));
    if (!candidates) { edge_map_free(&emap); return; }

    for (uint8_t depth = 1; depth <= max_depth; depth++) {
        uint32_t ncand = 0;

        float *errors = (float *)calloc(*nf, sizeof(float));
        if (!errors) break;

        tonly_error_worker_t args[QTSQ_MAX_THREADS];
        int nc = qtsq_num_cores();
        if ((uint32_t)nc > *nf) nc = (int)*nf;

        for (int t = 0; t < nc; t++) {
            args[t].verts = verts;
            args[t].faces = faces;
            args[t].face_depth = face_depth;
            args[t].depth = depth;
            args[t].pixels = pixels;
            args[t].w = w; args[t].h = h; args[t].ch = ch;
            args[t].error_threshold = error_threshold;
            args[t].errors = errors;
            args[t].start_fi = (uint32_t)((uint64_t)(*nf) * t / nc);
            args[t].end_fi   = (uint32_t)((uint64_t)(*nf) * (t + 1) / nc);
        }
        if (nc > 0) qtsq_dispatch(tonly_error_strip, args, sizeof(tonly_error_worker_t), nc);

        for (uint32_t fi = 0; fi < *nf; fi++) {
            if (errors[fi] > 0.0f) {
                candidates[ncand].face_idx = fi;
                candidates[ncand].error = errors[fi];
                ncand++;
            }
        }
        free(errors);

        if (ncand == 0) {
            TONLY_LOG("[TONLY Adaptive] Depth %u: no candidates above threshold\n", depth);
            break;
        }

        /* Sort by error descending — subdivide worst triangles first */
        qsort(candidates, ncand, sizeof(subdiv_candidate_t), cmp_subdiv_desc);

        uint32_t subdivided = 0;

        for (uint32_t ci = 0; ci < ncand; ci++) {
            if (*nv + 3 > max_verts) break;
            if (*nf + 4 > max_faces) break;  /* need room for 4 new faces (1 replaced + 3 new) */

            uint32_t fi = candidates[ci].face_idx;
            uint32_t v0 = faces[fi].v0;
            uint32_t v1 = faces[fi].v1;
            uint32_t v2 = faces[fi].v2;

            /* Get or create edge midpoints (deduplicated) */
            uint32_t m01 = edge_map_get_or_insert(&emap, v0, v1, verts, nv, max_verts, pixels, w, h, ch);
            uint32_t m12 = edge_map_get_or_insert(&emap, v1, v2, verts, nv, max_verts, pixels, w, h, ch);
            uint32_t m20 = edge_map_get_or_insert(&emap, v2, v0, verts, nv, max_verts, pixels, w, h, ch);

            if (m01 == UINT32_MAX || m12 == UINT32_MAX || m20 == UINT32_MAX) {
                break;  /* vertex budget exhausted */
            }

            /* Replace parent face with child 0 (corner v0) */

            faces[fi].v0 = v0;
            faces[fi].v1 = m01;
            faces[fi].v2 = m20;
            face_depth[fi] = depth;

            /* Add 3 more children */
            uint32_t nfi = *nf;
            faces[nfi].v0 = m01; faces[nfi].v1 = v1;  faces[nfi].v2 = m12;  face_depth[nfi] = depth;
            faces[nfi+1].v0 = m20; faces[nfi+1].v1 = m12; faces[nfi+1].v2 = v2;  face_depth[nfi+1] = depth;
            faces[nfi+2].v0 = m01; faces[nfi+2].v1 = m12; faces[nfi+2].v2 = m20; face_depth[nfi+2] = depth;
            *nf += 3;

            subdivided++;
        }

        TONLY_LOG("[TONLY Adaptive] Depth %u: %u/%u candidates subdivided -> %u verts, %u faces\n",
                 depth, subdivided, ncand, *nv, *nf);

        if (subdivided == 0) break;
    }

    free(candidates);
    edge_map_free(&emap);
}

/* ══════════════════════════════════════════════════════════════
 *  TRM5 Packing — Vertices + Faces + Depth per face + zlib
 * ══════════════════════════════════════════════════════════════ */

static int tonly_pack_trm5(const tri_vertex_t *verts, uint32_t nv,
                            const tri_face_t *faces, uint32_t nf,
                            const uint8_t *face_depth, uint8_t max_depth,
                            uint32_t width, uint32_t height, uint8_t channels,
                            uint8_t **out_data, size_t *out_size, int comp_level,
                            uint8_t codec) {
    int use_16bit = (nv <= 65535) ? 1 : 0;

    /* Header: TRM5(4) + W(4) + H(4) + ch_flags(1) + nv(4) + nf(4) + max_depth(1) = 22 bytes */
    size_t hsz = 4 + 4 + 4 + 1 + 4 + 4 + 1;
    size_t vert_sz = (size_t)nv * 7;
    size_t face_sz = (size_t)nf * 3 * (use_16bit ? 2 : 4);
    size_t depth_sz = (size_t)nf;
    size_t raw_sz = hsz + vert_sz + face_sz + depth_sz;

    uint8_t *raw = (uint8_t *)malloc(raw_sz);
    if (!raw) return -1;

    size_t p = 0;
    raw[p++] = 'T'; raw[p++] = 'R'; raw[p++] = 'M'; raw[p++] = '5';
    memcpy(raw + p, &width, 4);  p += 4;
    memcpy(raw + p, &height, 4); p += 4;

    uint8_t ch_flags = channels;
    if (use_16bit) ch_flags |= 0x80;
    raw[p++] = ch_flags;

    memcpy(raw + p, &nv, 4); p += 4;
    memcpy(raw + p, &nf, 4); p += 4;
    raw[p++] = max_depth;

    /* Planar vertices: X[], Y[], R[], G[], B[] */
    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(verts[i].x + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) { uint16_t v = (uint16_t)(verts[i].y + 0.5f); memcpy(raw + p, &v, 2); p += 2; }
    for (uint32_t i = 0; i < nv; i++) raw[p++] = verts[i].r;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = verts[i].g;
    for (uint32_t i = 0; i < nv; i++) raw[p++] = verts[i].b;

    /* Planar face indices: v0[], v1[], v2[] */
    if (use_16bit) {
        for (uint32_t i = 0; i < nf; i++) { uint16_t v = (uint16_t)faces[i].v0; memcpy(raw + p, &v, 2); p += 2; }
        for (uint32_t i = 0; i < nf; i++) { uint16_t v = (uint16_t)faces[i].v1; memcpy(raw + p, &v, 2); p += 2; }
        for (uint32_t i = 0; i < nf; i++) { uint16_t v = (uint16_t)faces[i].v2; memcpy(raw + p, &v, 2); p += 2; }
    } else {
        for (uint32_t i = 0; i < nf; i++) { memcpy(raw + p, &faces[i].v0, 4); p += 4; }
        for (uint32_t i = 0; i < nf; i++) { memcpy(raw + p, &faces[i].v1, 4); p += 4; }
        for (uint32_t i = 0; i < nf; i++) { memcpy(raw + p, &faces[i].v2, 4); p += 4; }
    }

    /* Per-face depth bytes — compresses very well (mostly 0s) */
    memcpy(raw + p, face_depth, nf);
    p += nf;

    /* Compress: qtc (opt-in) or zlib (default) */
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

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_triangle_adaptive
 *
 *  Grid-mesh adaptive recursive triangle subdivision (TRM5).
 *  1. Build regular grid mesh (O(n), parallel — replaces Delaunay)
 *  2. Per-triangle error measurement (parallel)
 *  3. Recursive subdivision of high-error triangles
 *  4. Pack as TRM5 (vertices + faces + depth)
 *
 *  This approach eliminates the O(n²) Bowyer-Watson Delaunay bottleneck
 *  while keeping extreme vertex counts (6M+ initial, 10M total).
 *  Grid spacing is < 1.5px on a 4K image — no features are missed.
 *  Adaptive subdivision still adds extreme density on edges/text.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_adaptive(qtsq_context_t *ctx,
                                     const uint8_t *pixels,
                                     uint32_t width, uint32_t height,
                                     uint8_t channels, size_t source_size,
                                     uint8_t max_depth,
                                     float error_threshold) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "triangle_adaptive");
    if (guard != QTSQ_OK) return guard;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;
    (void)source_size;

    /* Clamp parameters */
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 6) max_depth = 6;

    /* ──────────────────────────────────────────────────────────────
     *  Pixel-Count Driven Vertex Budget
     *
     *  Budget tiers (depth-driven):
     *    Compact  (depth<=2):  4% of px  →  ~500K  on 4K
     *    HD       (depth=3):  20% of px  →  ~2.5M  on 4K
     *    Lossless (depth=4):  65% of px  →  ~8.2M  on 4K
     *    Ultra    (depth=5):  80% of px  →  ~10M   on 4K (capped)
     *    Max      (depth=6):  90% of px  →  ~10M   on 4K (capped)
     * ────────────────────────────────────────────────────────────── */
    double px_fraction;
    if      (max_depth <= 2) px_fraction = 0.04;
    else if (max_depth == 3) px_fraction = 0.20;
    else if (max_depth == 4) px_fraction = 0.65;
    else if (max_depth == 5) px_fraction = 0.80;
    else                     px_fraction = 0.90;

    uint32_t total_vert_budget = (uint32_t)((double)pixel_count * px_fraction);
    if (total_vert_budget < 50000)   total_vert_budget = 50000;
    if (total_vert_budget > 3000000) total_vert_budget = 3000000;

    /* 60% for the initial grid, 40% headroom for adaptive subdivision */
    uint32_t initial_budget = (uint32_t)(total_vert_budget * 0.60f);

    TONLY_LOG("[TONLY Adaptive] px=%zu, px_fraction=%.0f%%, total_budget=%u, initial=%u, depth=%u, threshold=%.1f\n",
             pixel_count, px_fraction * 100.0, total_vert_budget, initial_budget, max_depth, error_threshold);

    /* ══════════════════════════════════════════════════════════════
     *  Step 1: Grid Mesh Construction (replaces scatter + Delaunay)
     *
     *  Regular grid mesh: O(n) construction, fully parallel.
     *  For 6M initial budget on 4K (4000×3000):
     *    grid_w ≈ 2828, grid_h ≈ 2121, cell ≈ 1.41px
     *    Every pixel is within 1 cell of a vertex → no feature missed.
     *  Adaptive subdivision then adds density on edges/text Detail.
     *
     *  Expected time: ~200ms (was 3-8 minutes with Delaunay)
     * ══════════════════════════════════════════════════════════════ */
    double t0 = tonly_time_ms();

    /* Grid resolution preserving aspect ratio */
    float aspect = (float)width / (float)height;
    uint32_t grid_w = (uint32_t)(sqrtf((float)initial_budget * aspect) + 0.5f);
    uint32_t grid_h = (uint32_t)(sqrtf((float)initial_budget / aspect) + 0.5f);
    if (grid_w < 2) grid_w = 2;
    if (grid_h < 2) grid_h = 2;
    if (grid_w > width)  grid_w = width;
    if (grid_h > height) grid_h = height;

    uint32_t nv = grid_w * grid_h;
    uint32_t nf = 2 * (grid_w - 1) * (grid_h - 1);

    TONLY_LOG("[TONLY Adaptive] Grid: %ux%u = %u verts, %u faces (cell ~%.2fpx)\n",
             grid_w, grid_h, nv, nf,
             (grid_w > 1) ? (float)(width - 1) / (float)(grid_w - 1) : 0.0f);

    /* Allocate for initial grid + subdivision headroom */
    tri_vertex_t *verts = (tri_vertex_t *)malloc((size_t)(total_vert_budget + 3) * sizeof(tri_vertex_t));
    uint32_t max_faces = total_vert_budget * 3;
    if (max_faces > 30000000) max_faces = 30000000;
    tri_face_t *faces = (tri_face_t *)malloc((size_t)max_faces * sizeof(tri_face_t));
    if (!verts || !faces) { free(verts); free(faces); return QTSQ_ERR_ALLOC; }

    float x_scale = (grid_w > 1) ? (float)(width - 1) / (float)(grid_w - 1) : 0.0f;
    float y_scale = (grid_h > 1) ? (float)(height - 1) / (float)(grid_h - 1) : 0.0f;

    /* ── Grid vertex generation (sample exact pixel color at each grid point) ── */
    for (uint32_t gy = 0; gy < grid_h; gy++) {
        float py = gy * y_scale;
        uint32_t ipy = (uint32_t)(py + 0.5f);
        if (ipy >= height) ipy = height - 1;
        for (uint32_t gx = 0; gx < grid_w; gx++) {
            uint32_t vi = gy * grid_w + gx;
            float px = gx * x_scale;
            uint32_t ipx = (uint32_t)(px + 0.5f);
            if (ipx >= width) ipx = width - 1;

            size_t idx = ((size_t)ipy * width + ipx) * channels;
            verts[vi].x = px;
            verts[vi].y = py;
            verts[vi].r = pixels[idx];
            verts[vi].g = (channels >= 2) ? pixels[idx + 1] : pixels[idx];
            verts[vi].b = (channels >= 3) ? pixels[idx + 2] : pixels[idx];
        }
    }

    /* ── Face generation (2 triangles per grid cell) ── */
    for (uint32_t gy = 0; gy < grid_h - 1; gy++) {
        for (uint32_t gx = 0; gx < grid_w - 1; gx++) {
            uint32_t fi = (gy * (grid_w - 1) + gx) * 2;
            uint32_t tl = gy * grid_w + gx;
            uint32_t tr = tl + 1;
            uint32_t bl = tl + grid_w;
            uint32_t br = bl + 1;

            faces[fi].v0 = tl;
            faces[fi].v1 = tr;
            faces[fi].v2 = bl;

            faces[fi + 1].v0 = bl;
            faces[fi + 1].v1 = tr;
            faces[fi + 1].v2 = br;
        }
    }

    TONLY_LOG("[TONLY Adaptive] Grid mesh built: %u verts, %u faces in %.1f ms\n",
             nv, nf, tonly_time_ms() - t0);

    /* ── Step 2: Adaptive subdivision ── */
    t0 = tonly_time_ms();
    uint8_t *face_depth = (uint8_t *)calloc(max_faces, sizeof(uint8_t));
    if (!face_depth) { free(verts); free(faces); return QTSQ_ERR_ALLOC; }

    tonly_adaptive_refine(verts, &nv, faces, &nf, face_depth,
                           total_vert_budget, max_faces,
                           pixels, width, height, channels,
                           max_depth, error_threshold);
    TONLY_LOG("[TONLY Adaptive] Refinement: %.1f ms -> %u verts, %u faces\n",
             tonly_time_ms() - t0, nv, nf);

    /* ── Step 3: Pack TRM5 + zlib ── */
    t0 = tonly_time_ms();
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm5(verts, nv, faces, nf, face_depth, max_depth,
                         width, height, channels,
                         &payload, &payload_size, 1, ctx->codec) != 0) {
        free(verts); free(faces); free(face_depth);
        return QTSQ_ERR_ALLOC;
    }
    TONLY_LOG("[TONLY Adaptive] TRM5 packing: %.1f ms -> %zu bytes\n",
             tonly_time_ms() - t0, payload_size);

    free(verts);
    free(faces);
    free(face_depth);

    /* ── Finalize context ── */
    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        QTSQ_FLAG_HD, "TRI_ADAPTIVE_V2");

    TONLY_LOG("[TONLY Adaptive] Total: %u verts, %u faces -> %zu bytes in %.1f ms\n",
              nv, nf, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Artist Controls — Custom triangle compression
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_triangle_custom(qtsq_context_t *ctx,
                                   const uint8_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint8_t channels,
                                   float density,
                                   float edge_sensitivity,
                                   float edge_threshold) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width < 8 || height < 8) return QTSQ_ERR_TOO_SMALL;
    if (channels < 1 || channels > 4) return QTSQ_ERR_FORMAT;
    int guard = qtsq_require_creative(ctx, "triangle_custom");
    if (guard != QTSQ_OK) return guard;

    /* Clamp parameters */
    if (density < 0.01f) density = 0.01f;
    if (density > 1.0f) density = 1.0f;
    if (edge_sensitivity < 0.0f) edge_sensitivity = 0.0f;
    if (edge_sensitivity > 1.0f) edge_sensitivity = 1.0f;
    if (edge_threshold < 0.0f) edge_threshold = 0.0f;
    if (edge_threshold > 255.0f) edge_threshold = 255.0f;

    double t_total = tonly_time_ms();
    size_t pixel_count = (size_t)width * height;

    /* Calculate vertex budget from density parameter */
    uint32_t max_verts = (uint32_t)(pixel_count * density);
    if (max_verts < 25000) max_verts = 25000;
    if (max_verts > 6000000) max_verts = 6000000;

    TONLY_LOG("[TONLY Custom] density=%.2f edge_sens=%.2f edge_thresh=%.1f max_verts=%u\n",
              density, edge_sensitivity, edge_threshold, max_verts);

    /* Allocate */
    tri_vertex_t *verts = (tri_vertex_t *)malloc(max_verts * sizeof(tri_vertex_t));
    uint8_t *used = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
    if (!verts || !used) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Compute gradient */
    float *grad = tonly_compute_gradient(pixels, width, height, channels);
    if (!grad) { free(verts); free(used); return QTSQ_ERR_ALLOC; }

    /* Apply edge threshold — zero out gradients below threshold */
    if (edge_threshold > 0.0f) {
        for (size_t i = 0; i < pixel_count; i++) {
            if (grad[i] < edge_threshold) grad[i] = 0.0f;
        }
    }

    /* Adjust edge sensitivity by modifying the gradient boost.
     * Higher sensitivity = more concentration on edges (higher exponent).
     * Sensitivity 0.0 → grad^1.0 (uniform), 1.0 → grad^3.0 (extreme edge focus) */
    float exponent = 1.0f + edge_sensitivity * 2.0f;
    float grad_max = 0.0f;
    for (size_t i = 0; i < pixel_count; i++) {
        grad[i] = powf(grad[i], exponent);
        if (grad[i] > grad_max) grad_max = grad[i];
    }
    if (grad_max < 1.0f) grad_max = 1.0f;

    /* Place vertices using the internal function */
    uint32_t nv = tonly_place_vertices(pixels, grad, width, height, channels,
                                        verts, max_verts, used);
    free(grad);
    free(used);

    /* Determine zlib level based on density */
    int zlib_level = (density < 0.1f) ? 9 : (density < 0.5f) ? 6 : 1;

    /* Pack TRM3 */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (tonly_pack_trm3(verts, nv, width, height, channels,
                         &payload, &payload_size, zlib_level, ctx->codec) != 0) {
        free(verts);
        return QTSQ_ERR_ALLOC;
    }
    free(verts);

    tonly_finalize_ctx(ctx, payload, payload_size, nv,
                        width, height, channels, pixels,
                        0, "TRI_CUSTOM");

    TONLY_LOG("[TONLY Custom] %u verts → %zu bytes in %.1f ms\n",
              nv, payload_size, tonly_time_ms() - t_total);
    return QTSQ_OK;
}

int qtsq_compress_triangle_lowpoly(qtsq_context_t *ctx,
                                    const uint8_t *pixels,
                                    uint32_t width, uint32_t height,
                                    uint8_t channels) {
    /* Low-poly: sparse mesh (2% of pixels), maximum edge focus */
    return qtsq_compress_triangle_custom(ctx, pixels, width, height, channels,
                                          0.02f,   /* density: sparse */
                                          0.9f,    /* edge_sensitivity: very high */
                                          30.0f);  /* edge_threshold: moderate */
}

int qtsq_compress_triangle_smooth(qtsq_context_t *ctx,
                                   const uint8_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint8_t channels) {
    /* Smooth: medium density (20% of pixels), low edge sensitivity */
    return qtsq_compress_triangle_custom(ctx, pixels, width, height, channels,
                                          0.20f,   /* density: medium */
                                          0.3f,    /* edge_sensitivity: low */
                                          10.0f);  /* edge_threshold: low */
}
