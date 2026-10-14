/*
 * qtc_accretion.c — Phase 5.5: multi-primitive lossy image scene codec.
 *
 * Phase 5 proved that a single primitive (the Delaunay triangle mesh) cannot
 * represent photographic content: piecewise-linear interpolation smooths away
 * broadband texture, and no vertex budget fixes a basis mismatch. This codec
 * replaces the single basis with a SCENE of three primitive kinds, each
 * owning the image structure it fits:
 *
 *   splats    2D Gaussians (position, sigma, color) rendered as a
 *             normalized-weight field (partition of unity) — the soft base
 *             layer for gradients, glow, bokeh, skies.
 *   vertices  Delaunay mesh points with barycentric interpolation — the
 *             sharp layer for edges and linear structure (reuses the Phase 5
 *             mesh machinery).
 *   blocks    direct quantized pixel rectangles — the exact layer for
 *             texture and noise, where every smooth basis fails.
 *
 * Physics framing (CONCEPT.md lineage): error maxima are gravity wells and
 * primitives are matter. Encoding is accretion — each pass renders the
 * scene, finds the deepest error wells, classifies the local structure
 * (variance / gradient coherence of the original), and drops the primitive
 * type that belongs there: blocks onto texture, vertices onto coherent
 * edges, splats onto everything smooth. A quality-derived BYTE budget (not a
 * vertex count) bounds the total, so the rate-distortion knob is honest.
 *
 * The encoder renders only what the decoder will reconstruct: splat sigma is
 * quantized to its stored byte before use, block samples are quantized to
 * the global depth shift at insert time. No encode/decode drift.
 *
 * Render order: splats (base) → triangles (overwrite) → blocks (overwrite).
 * Splat coverage is guaranteed by an overlapping seed grid (spacing ≤ 96,
 * sigma ≥ spacing*0.75 clamped to 48, so every pixel sees nonzero weight).
 *
 * Payload layout:
 *   [u32 w][u32 h][u8 ch][u8 depth-shift]
 *   [u32 n_splat][u32 n_vert][u32 n_block][u32 body_len]
 *   [LZ of body:  splat records  (u16 cx, u16 cy, u8 sigma, u8 rot0, ch col)
 *                 vertex records (u16 x, u16 y, ch col)
 *                 block records  (u16 x, u16 y, u8 bw, u8 bh, bw*bh*ch bytes)]
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ACC_MAX_SPLAT 60000u
#define ACC_MAX_VERT  30000u
#define ACC_MAX_BLOCK 20000u
#define ACC_HDR       31u

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

/* Splat sigma: u8 <-> [0.5, 48] on a log scale. */
static double splat_sigma(uint8_t sq)
{
    return 0.5 * pow(96.0, (double)sq / 255.0);
}

static uint8_t sigma_byte(double s)
{
    if (s < 0.5) s = 0.5;
    if (s > 48.0) s = 48.0;
    return (uint8_t)((log(s / 0.5) / log(96.0)) * 255.0 + 0.5);
}

/* Sort candidates by error, descending (no globals: struct carries the key). */
typedef struct { double e; size_t i; } acc_cand_t;

static int acc_cand_desc(const void *a, const void *b)
{
    double ea = ((const acc_cand_t *)a)->e;
    double eb = ((const acc_cand_t *)b)->e;
    if (ea < eb) return 1;
    if (ea > eb) return -1;
    return 0;
}

/* Quickselect (descending by error): rearrange cand so the k highest-error
 * entries occupy cand[0..k-1], unordered. O(n) average; the caller sorts
 * just that prefix. Part of optimization #11: a pass can consume only a
 * few hundred primitives, so selecting them replaces a full sort of ~npix
 * candidates every pass.
 * Invariant: positions [0, lo) hold the lo largest entries seen so far. */
static void acc_select_top(acc_cand_t *c, size_t n, size_t k)
{
    if (k >= n)
        return;
    long lo = 0, hi = (long)n - 1;
    long K = (long)k;

    while (hi - lo > 24) {
        /* Median-of-three: arrange so c[lo] >= c[mid] >= c[hi]; pivot c[lo]. */
        long mid = lo + (hi - lo) / 2;
        if (c[mid].e > c[lo].e) { acc_cand_t t = c[mid]; c[mid] = c[lo]; c[lo] = t; }
        if (c[hi].e > c[lo].e)  { acc_cand_t t = c[hi];  c[hi]  = c[lo]; c[lo] = t; }
        if (c[mid].e > c[hi].e) { acc_cand_t t = c[mid]; c[mid] = c[hi]; c[hi] = t; }
        double pv = c[lo].e;

        /* Lomuto partition, descending: [lo, store) > pv, store == pv,
         * (store, hi] <= pv. */
        long store = lo;
        for (long i = lo + 1; i <= hi; i++) {
            if (c[i].e > pv) {
                store++;
                acc_cand_t t = c[store]; c[store] = c[i]; c[i] = t;
            }
        }
        { acc_cand_t t = c[lo]; c[lo] = c[store]; c[store] = t; }

        long have = store - lo + 1;      /* |A| + pivot */
        long need = K - lo;
        if (need < have) {
            hi = store - 1;              /* top K all inside A */
        } else if (need == have) {
            return;                      /* [lo, store] is exactly the rest */
        } else {
            lo = store + 1;              /* A + pivot kept; continue in C */
        }
    }

    /* Small tail: insertion sort descending; the invariant then places the
     * top K at [0, K). */
    for (long i = lo + 1; i <= hi; i++) {
        acc_cand_t t = c[i];
        long j = i;
        while (j > lo && c[j - 1].e < t.e) { c[j] = c[j - 1]; j--; }
        c[j] = t;
    }
}

/* ── scene state ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t w, h;
    uint8_t ch;
    size_t npix;

    uint16_t *scx, *scy;        /* splat centers (quantized px)      */
    uint8_t *ssx, *ssy;          /* anisotropic sigma bytes (v0.8.0)   */
    uint8_t *scol;              /* splat colors (ns * ch)            */
    size_t ns, cs;

    uint32_t *vx, *vy;          /* mesh vertices                    */
    uint8_t *vcol;              /* exact vertex colors              */
    size_t nv, cv;
    uint8_t *isvert;            /* per-pixel vertex bitmap          */

    uint32_t *bx, *by, *bw, *bh;
    size_t *boff;               /* offset of each block in bdata    */
    uint8_t *bshift;            /* per-block depth shift (v0.8.0)   */
    size_t nb, cb;
    uint8_t *bdata;             /* quantized block samples          */
    size_t bdlen, cbd;
    uint8_t *bmark;             /* pixels already owned by a block  */

    uint8_t *smark;             /* 3x3 marks around splat centers   */
    size_t nseed;               /* seed count: bg = Voronoi of these */
    int shift;                  /* block depth shift (0..3)         */
} acc_scene_t;

static void acc_scene_free(acc_scene_t *s)
{
    free(s->scx); free(s->scy); free(s->ssx); free(s->ssy); free(s->scol);
    free(s->vx); free(s->vy); free(s->vcol); free(s->isvert);
    free(s->bx); free(s->by); free(s->bw); free(s->bh); free(s->boff);
    free(s->bshift);
    free(s->bdata); free(s->bmark); free(s->smark);
}

/* Paint one splat as an alpha-composited disc. Record order matters: the
 * seed grid paints first, later error-driven splats (exact colors, small
 * sigma) overwrite locally instead of being averaged against the base — a
 * normalized-weight field cannot hold a step edge (kernels from the far
 * side always bleed in), a painter can. */
static void acc_splat_paint(const acc_scene_t *s, uint8_t *out, size_t k)
{
    /* Anisotropic kernel (optimization #4, v0.8.0): the record carries
     * independent x/y sigmas, so refinement splats can elongate ALONG an
     * edge (thin across it). The seed grid stays isotropic for coverage. */
    double sx = splat_sigma(s->ssx[k]);
    double sy = splat_sigma(s->ssy[k]);
    double cx = (double)s->scx[k], cy = (double)s->scy[k];
    int extx = (int)(3.0 * sx) + 1;
    int exty = (int)(3.0 * sy) + 1;
    int x0 = (int)cx - extx; if (x0 < 0) x0 = 0;
    int x1 = (int)cx + extx; if (x1 > (int)s->w - 1) x1 = (int)s->w - 1;
    int y0 = (int)cy - exty; if (y0 < 0) y0 = 0;
    int y1 = (int)cy + exty; if (y1 > (int)s->h - 1) y1 = (int)s->h - 1;
    double invx = 1.0 / (2.0 * sx * sx);
    double invy = 1.0 / (2.0 * sy * sy);
    const uint8_t *col = s->scol + k * s->ch;

    for (int y = y0; y <= y1; y++) {
        double dy = (double)y - cy;
        double wy = dy * dy * invy;
        for (int x = x0; x <= x1; x++) {
            double dx = (double)x - cx;
            double a = exp(-(dx * dx * invx + wy));
            if (a < 0.01)
                continue;
            uint8_t *o = out + ((size_t)y * s->w + x) * s->ch;
            for (uint8_t c = 0; c < s->ch; c++) {
                double v = (double)o[c] + a * ((double)col[c] - (double)o[c]);
                o[c] = (uint8_t)(v + 0.5);
            }
        }
    }
}

static int acc_add_splat(acc_scene_t *s, uint32_t x, uint32_t y,
                         double sx, double sy, const uint8_t *colsrc)
{
    if (s->ns >= ACC_MAX_SPLAT)
        return QTC_ERR_SIZE;
    if (s->ns == s->cs) {
        size_t nc = s->cs ? s->cs * 2 : 1024;
        if (nc > ACC_MAX_SPLAT) nc = ACC_MAX_SPLAT;
        uint16_t *ncx = (uint16_t *)realloc(s->scx, nc * sizeof(uint16_t));
        if (!ncx) return QTC_ERR_ALLOC;
        s->scx = ncx;
        uint16_t *ncy = (uint16_t *)realloc(s->scy, nc * sizeof(uint16_t));
        if (!ncy) return QTC_ERR_ALLOC;
        s->scy = ncy;
        uint8_t *ngx = (uint8_t *)realloc(s->ssx, nc);
        if (!ngx) return QTC_ERR_ALLOC;
        s->ssx = ngx;
        uint8_t *ngy = (uint8_t *)realloc(s->ssy, nc);
        if (!ngy) return QTC_ERR_ALLOC;
        s->ssy = ngy;
        uint8_t *nco = (uint8_t *)realloc(s->scol, nc * s->ch);
        if (!nco) return QTC_ERR_ALLOC;
        s->scol = nco;
        s->cs = nc;
    }
    size_t k = s->ns++;
    s->scx[k] = (uint16_t)x;
    s->scy[k] = (uint16_t)y;
    /* Quantize BOTH sigmas to their stored bytes: the renderer uses the
     * dequantized values, so encode and decode stay identical. */
    s->ssx[k] = sigma_byte(sx);
    s->ssy[k] = sigma_byte(sy);
    memcpy(s->scol + k * s->ch, colsrc, s->ch);

    /* Painting happens at render time in record order (later = on top). */

    /* 3x3 mark so the candidate walk spreads samples while density builds. */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int xx = (int)x + dx, yy = (int)y + dy;
            if (xx < 0 || yy < 0 || xx >= (int)s->w || yy >= (int)s->h)
                continue;
            s->smark[(size_t)yy * s->w + xx] = 1;
        }
    return QTC_OK;
}

static int acc_add_vert(acc_scene_t *s, uint32_t x, uint32_t y,
                        const uint8_t *pixels)
{
    if (s->nv >= ACC_MAX_VERT)
        return QTC_ERR_SIZE;
    if (s->nv == s->cv) {
        size_t nc = s->cv ? s->cv * 2 : 1024;
        if (nc > ACC_MAX_VERT) nc = ACC_MAX_VERT;
        uint32_t *nx = (uint32_t *)realloc(s->vx, nc * sizeof(uint32_t));
        if (!nx) return QTC_ERR_ALLOC;
        s->vx = nx;
        uint32_t *ny = (uint32_t *)realloc(s->vy, nc * sizeof(uint32_t));
        if (!ny) return QTC_ERR_ALLOC;
        s->vy = ny;
        uint8_t *nco = (uint8_t *)realloc(s->vcol, nc * s->ch);
        if (!nco) return QTC_ERR_ALLOC;
        s->vcol = nco;
        s->cv = nc;
    }
    size_t pi = (size_t)y * s->w + x;
    if (s->isvert[pi])
        return QTC_OK;
    s->isvert[pi] = 1;
    size_t k = s->nv++;
    s->vx[k] = x;
    s->vy[k] = y;
    memcpy(s->vcol + k * s->ch, pixels + pi * s->ch, s->ch);
    return QTC_OK;
}

/* Blocks quantize the ORIGINAL at insert time, so what renders is exactly
 * what decodes. Optimization #3 (v0.8.0): the shift is chosen PER BLOCK —
 * the coarsest shift ≤ the global one whose quantization MSE stays within
 * a quarter of the error that made this pixel a candidate, so the worst
 * regions get finer samples without spending exactness where it buys
 * nothing. The chosen shift is serialized in the record. */
static int acc_add_block(acc_scene_t *s, uint32_t x, uint32_t y,
                         const uint8_t *pixels, double cand_err)
{
    if (s->nb >= ACC_MAX_BLOCK)
        return QTC_ERR_SIZE;
    uint32_t bw = 8, bh = 8;
    if (x + bw > s->w) bw = s->w - x;
    if (y + bh > s->h) bh = s->h - y;
    if (bw == 0 || bh == 0)
        return QTC_ERR_FORMAT;

    /* Pick the block's depth shift before storing anything. */
    double budget = 0.25 * cand_err;
    int shift = s->shift;
    while (shift > 0) {
        int half = 1 << (shift - 1);
        double mse = 0.0;
        for (uint32_t r = 0; r < bh; r++)
            for (uint32_t c = 0; c < bw; c++) {
                size_t pi = ((size_t)(y + r) * s->w + (x + c)) * s->ch;
                for (uint8_t q = 0; q < s->ch; q++) {
                    int v = pixels[pi + q];
                    int vq = (v + half) >> shift;
                    vq <<= shift;
                    double d = (double)v - (double)vq;
                    mse += d * d;
                }
            }
        if (mse <= budget)
            break;
        shift--;
    }

    if (s->nb == s->cb) {
        size_t nc = s->cb ? s->cb * 2 : 256;
        if (nc > ACC_MAX_BLOCK) nc = ACC_MAX_BLOCK;
        uint32_t *na = (uint32_t *)realloc(s->bx, nc * sizeof(uint32_t));
        if (!na) return QTC_ERR_ALLOC;
        s->bx = na;
        uint32_t *nb2 = (uint32_t *)realloc(s->by, nc * sizeof(uint32_t));
        if (!nb2) return QTC_ERR_ALLOC;
        s->by = nb2;
        uint32_t *nw = (uint32_t *)realloc(s->bw, nc * sizeof(uint32_t));
        if (!nw) return QTC_ERR_ALLOC;
        s->bw = nw;
        uint32_t *nh = (uint32_t *)realloc(s->bh, nc * sizeof(uint32_t));
        if (!nh) return QTC_ERR_ALLOC;
        s->bh = nh;
        size_t *no = (size_t *)realloc(s->boff, nc * sizeof(size_t));
        if (!no) return QTC_ERR_ALLOC;
        s->boff = no;
        uint8_t *nsh = (uint8_t *)realloc(s->bshift, nc);
        if (!nsh) return QTC_ERR_ALLOC;
        s->bshift = nsh;
        s->cb = nc;
    }
    size_t need = s->bdlen + (size_t)bw * bh * s->ch;
    if (need > s->cbd) {
        size_t nc = s->cbd ? s->cbd : 8192;
        while (nc < need) nc *= 2;
        uint8_t *nd = (uint8_t *)realloc(s->bdata, nc);
        if (!nd) return QTC_ERR_ALLOC;
        s->bdata = nd;
        s->cbd = nc;
    }

    size_t k = s->nb++;
    s->bx[k] = x; s->by[k] = y; s->bw[k] = bw; s->bh[k] = bh;
    s->boff[k] = s->bdlen;
    s->bshift[k] = (uint8_t)shift;

    int half = shift ? (1 << (shift - 1)) : 0;
    for (uint32_t r = 0; r < bh; r++)
        for (uint32_t c = 0; c < bw; c++) {
            size_t pi = ((size_t)(y + r) * s->w + (x + c)) * s->ch;
            for (uint8_t q = 0; q < s->ch; q++) {
                int v = pixels[pi + q];
                if (shift)
                    v = (v + half) >> shift;
                s->bdata[s->bdlen++] = (uint8_t)v;
            }
        }

    for (uint32_t r = 0; r < bh; r++)
        for (uint32_t c = 0; c < bw; c++)
            s->bmark[(size_t)(y + r) * s->w + (x + c)] = 1;
    return QTC_OK;
}

/* Triangles are a LOCAL sharpening primitive: only triangles whose minimum
 * altitude is ≤ this many pixels are rendered (edge-scale slivers). Large-
 * in-every-dimension triangles would chord across curved or distant regions
 * the splat field models better, so they are skipped identically at encode
 * and decode. */
#define ACC_TRI_MAX_W 8.0

/* Render the triangle and block layers ON TOP of an already-prepared
 * buffer (which already holds the background + refinement splats). The
 * encoder's incremental path prepares that base separately so it does not
 * repaint every splat each pass; the decoder still uses acc_render below,
 * which paints splats first and then calls this. */
static void acc_render_over(const acc_scene_t *s, int *tris, int ntris,
                            uint8_t *out)
{
    if (ntris > 0 && tris) {
        /* Keep only LOCAL triangles: minimum altitude ≤ ACC_TRI_MAX_W.
         * A long thin sliver along an edge is locally exact and wanted; a
         * triangle large in every dimension would chord across curved or
         * distant regions the splat field models better. min_alt =
         * 2*Area / longest_edge = |cross| / longest_edge. */
        int nsmall = 0;
        for (int t = 0; t < ntris; t++) {
            int a = tris[3 * t], b = tris[3 * t + 1], c3 = tris[3 * t + 2];
            double x0 = s->vx[a], y0 = s->vy[a];
            double x1 = s->vx[b], y1 = s->vy[b];
            double x2 = s->vx[c3], y2 = s->vy[c3];
            double cross = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (fabs(cross) < 1e-9)
                continue;
            double e0 = hypot(x1 - x0, y1 - y0);
            double e1 = hypot(x2 - x0, y2 - y0);
            double e2 = hypot(x2 - x1, y2 - y1);
            double longest = e0 > e1 ? (e0 > e2 ? e0 : e2)
                                     : (e1 > e2 ? e1 : e2);
            if (fabs(cross) / longest > ACC_TRI_MAX_W)
                continue;
            tris[3 * nsmall] = a;
            tris[3 * nsmall + 1] = b;
            tris[3 * nsmall + 2] = c3;
            nsmall++;
        }
        qtc_mesh_render(s->vx, s->vy, s->vcol, tris, nsmall,
                        s->w, s->h, s->ch, out);
    }
    for (size_t k = 0; k < s->nb; k++) {
        const uint8_t *src = s->bdata + s->boff[k];
        for (uint32_t r = 0; r < s->bh[k]; r++) {
            uint8_t *dst = out + (((size_t)(s->by[k] + r) * s->w +
                                   s->bx[k]) * s->ch);
            for (uint32_t c = 0; c < s->bw[k]; c++)
                for (uint8_t q = 0; q < s->ch; q++) {
                    int v = *src++ << s->bshift[k];
                    if (v > 255) v = 255;
                    dst[(size_t)c * s->ch + q] = (uint8_t)v;
                }
        }
    }
}

/* Compose the current scene into out (npix*ch). The caller must pre-fill
 * out with the seed background (acc_seed_bg) or zeroes. Refinement splats
 * paint on top in record order; then local triangles; then blocks. Used by
 * the decoder; the encoder uses the incremental splat-layer path instead. */
static void acc_render(const acc_scene_t *s, int *tris, int ntris,
                       uint8_t *out)
{
    for (size_t k = s->nseed; k < s->ns; k++)
        acc_splat_paint(s, out, k);
    acc_render_over(s, tris, ntris, out);
}

/* Mean color of a (2r+1)^2 window of the original, rounded. */
static void window_color(const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, uint32_t x, uint32_t y, int r,
                         uint8_t *col)
{
    uint32_t x0 = x < (uint32_t)r ? 0 : x - r;
    uint32_t y0 = y < (uint32_t)r ? 0 : y - r;
    uint32_t x1 = x + r >= w ? w - 1 : x + r;
    uint32_t y1 = y + r >= h ? h - 1 : y + r;
    double sum[4] = {0, 0, 0, 0};
    size_t n = 0;
    for (uint32_t yy = y0; yy <= y1; yy++)
        for (uint32_t xx = x0; xx <= x1; xx++) {
            const uint8_t *p = pixels + ((size_t)yy * w + xx) * ch;
            for (uint8_t q = 0; q < ch; q++)
                sum[q] += p[q];
            n++;
        }
    for (uint8_t q = 0; q < ch; q++) {
        int v = (int)(sum[q] / (double)n + 0.5);
        col[q] = (uint8_t)(v > 255 ? 255 : v);
    }
}

/* Classify a 7x7 neighborhood of the ORIGINAL: per-channel variance and
 * per-channel gradient coherence decide which primitive belongs here.
 * Per-channel, NOT luminance — a chroma edge (r/b stepping in opposite
 * directions) is invisible in luminance and must still attract vertices.
 * Returns 0=splat, 1=vertex, 2=block; sx and sy are outputs for splats
 * (anisotropic since v0.8.0: elongate ALONG structure — a vertical edge
 * wants a splat thin in x, long in y). */
static int classify_patch(const uint8_t *pixels, uint32_t w, uint32_t h,
                          uint8_t ch, uint32_t x, uint32_t y,
                          double *sx, double *sy)
{
    int r = 3;
    uint32_t x0 = x < (uint32_t)r ? 0 : x - r;
    uint32_t y0 = y < (uint32_t)r ? 0 : y - r;
    uint32_t x1 = x + r >= w ? w - 1 : x + r;
    uint32_t y1 = y + r >= h ? h - 1 : y + r;

    double sum[4] = {0, 0, 0, 0}, sumsq[4] = {0, 0, 0, 0};
    double gxx = 0.0, gyy = 0.0, gxy = 0.0;
    size_t n = 0, ng = 0;
    for (uint32_t yy = y0; yy <= y1; yy++)
        for (uint32_t xx = x0; xx <= x1; xx++) {
            const uint8_t *p = pixels + ((size_t)yy * w + xx) * ch;
            for (uint8_t q = 0; q < ch; q++) {
                sum[q] += p[q];
                sumsq[q] += (double)p[q] * p[q];
            }
            n++;
            if (xx > 0 && xx + 1 < w && yy > 0 && yy + 1 < h) {
                const uint8_t *pr = pixels + ((size_t)yy * w + xx + 1) * ch;
                const uint8_t *pl = pixels + ((size_t)yy * w + xx - 1) * ch;
                const uint8_t *pd = pixels + ((size_t)(yy + 1) * w + xx) * ch;
                const uint8_t *pu = pixels + ((size_t)(yy - 1) * w + xx) * ch;
                for (uint8_t q = 0; q < ch; q++) {
                    double dx = (double)pr[q] - pl[q];
                    double dy = (double)pd[q] - pu[q];
                    gxx += dx * dx;
                    gyy += dy * dy;
                    gxy += dx * dy;
                }
                ng++;
            }
        }

    double std_max = 0.0;
    for (uint8_t q = 0; q < ch; q++) {
        double mean = sum[q] / (double)n;
        double var = sumsq[q] / (double)n - mean * mean;
        if (var < 0) var = 0;
        double std = sqrt(var);
        if (std > std_max) std_max = std;
    }

    /* Anisotropy helper: aspect from the gradient direction, applied so
     * the splat is NARROW across the gradient and LONG along it. */
    #define ACC_SPLAT_ASPECT(base) do {                                   \
        double gx_ = sqrt(gxx > 0 ? gxx : 0), gy_ = sqrt(gyy > 0 ? gyy : 0); \
        double ratio = 1.0;                                               \
        if (gx_ > gy_) ratio = 1.0 + (gx_ - gy_) / (gy_ + 8.0);           \
        else           ratio = 1.0 + (gy_ - gx_) / (gx_ + 8.0);           \
        if (ratio > 4.0) ratio = 4.0;                                     \
        double r_ = sqrt(ratio);                                          \
        if (gx_ > gy_) { *sx = (base) / r_; *sy = (base) * r_; }          \
        else           { *sx = (base) * r_; *sy = (base) / r_; }          \
    } while (0)

    if (std_max < 5.0) {
        /* flat → big soft splat (aspect ≈ 1 automatically: gradients ~0) */
        ACC_SPLAT_ASPECT(32.0);
        return 0;
    }
    /* Structure tensor across channels: a step edge has all gradients
     * parallel (lambda_max dominates); isotropic texture does not. This
     * separates "strong edge" from "texture", which plain variance cannot
     * (a straddled edge is bimodal and inflates std like noise does). */
    double gsum = gxx + gyy;
    if (gsum > 1e-9) {
        double lam = 0.5 * (gsum + sqrt((gxx - gyy) * (gxx - gyy) +
                                        4.0 * gxy * gxy));
        double strength = sqrt(gsum / (double)(ng ? ng : 1) / (double)ch);
        if (lam > 0.75 * gsum && strength > 4.0) {
            /* Coherent edge: sharp elongated σ3 beads (exact colors,
             * painted on top) rather than mesh vertices — experience here
             * shows vertex slivers can wedge into regions the splat field
             * already models and streak them. */
            ACC_SPLAT_ASPECT(3.0);
            return 0;
        }
    }
    if (std_max > 20.0)
        return 2;                          /* broadband texture → block */
    /* Everything else is the splats' domain: curvature and gentle ramps get
     * SMALL exact repaints. In painter compositing a big soft disc would
     * plateau the region (last-writer-wins, not averaging), so scale is
     * always local and density does the smoothing; vertices on structure
     * would instead grow a full-coverage Delaunay hull over this layer. */
    ACC_SPLAT_ASPECT(5.0);
    return 0;
    #undef ACC_SPLAT_ASPECT
}

/* Opaque background: each pixel takes the color of its NEAREST seed splat
 * (Voronoi). Full coverage with no opacity bias; refinement splats then
 * paint soft corrections on top. Deterministic from the stored seeds. */
static void acc_seed_bg(const acc_scene_t *s, uint8_t *out)
{
    for (size_t i = 0; i < s->npix; i++) {
        uint32_t x = (uint32_t)(i % s->w), y = (uint32_t)(i / s->w);
        size_t best = 0;
        double bd = 1e30;
        for (size_t k = 0; k < s->nseed; k++) {
            double dx = (double)x - (double)s->scx[k];
            double dy = (double)y - (double)s->scy[k];
            double d2 = dx * dx + dy * dy;
            if (d2 < bd) { bd = d2; best = k; }
        }
        memcpy(out + i * s->ch, s->scol + best * s->ch, s->ch);
    }
}

/* ── encode ──────────────────────────────────────────────────────────── */

int qtc_accretion_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, float quality,
                         uint8_t **out, size_t *out_len)
{
    if (!pixels || w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;

    size_t npix = (size_t)w * h;
    size_t rawsz = npix * ch;

    acc_scene_t s;
    memset(&s, 0, sizeof(s));
    s.w = w; s.h = h; s.ch = ch; s.npix = npix;
    s.isvert = (uint8_t *)calloc(npix, 1);
    s.bmark = (uint8_t *)calloc(npix, 1);
    s.smark = (uint8_t *)calloc(npix, 1);
    uint8_t *render = (uint8_t *)malloc(rawsz);
    double *err = (double *)malloc(npix * sizeof(double));
    acc_cand_t *cand = (acc_cand_t *)malloc(npix * sizeof(acc_cand_t));
    int *tris = NULL;
    int ntris = 0;
    if (!s.isvert || !s.bmark || !s.smark ||
        !render || !err || !cand) {
        acc_scene_free(&s);
        free(render); free(err); free(cand); free(tris);
        return QTC_ERR_ALLOC;
    }

    int shift = (int)(((1.0 - (double)quality) * 3.0) + 0.5);
    if (shift < 0) shift = 0;
    if (shift > 3) shift = 3;
    s.shift = shift;

    size_t budget = (size_t)((double)rawsz *
                             (0.001 + pow((double)quality, 2.5) * 0.09));
    if (budget < 64) budget = 64;
    if (budget > rawsz) budget = rawsz;

    size_t splat_b = 6 + ch, vert_b = 4 + ch;

    /* Splat seed grid: spacing adapts to the budget (seeds ≤ ~half of it),
     * clamped so adjacent kernels always overlap (coverage guarantee). */
    int sp = 32;
    double min_sp = sqrt((double)npix * (double)(6 + ch) * 2.0 /
                         (double)budget);
    if (min_sp > (double)sp)
        sp = (int)min_sp;
    if (sp > 96) sp = 96;
    if ((uint32_t)sp > w || (uint32_t)sp > h)
        sp = (int)(w < h ? w : h);
    if (sp < 1) sp = 1;
    double sig0 = (double)sp * 0.75;
    if (sig0 < 2.0) sig0 = 2.0;
    if (sig0 > 48.0) sig0 = 48.0;

    uint8_t col[4];
    for (uint32_t y = 0; y < h; y += (uint32_t)sp)
        for (uint32_t x = 0; x < w; x += (uint32_t)sp) {
            window_color(pixels, w, h, ch, x, y, 1, col);
            acc_add_splat(&s, x, y, sig0, sig0, col);
        }

    /* No vertex seeds by design: the splat grid is the coverage base, and
     * vertices accrete only where the classifier finds coherent structure.
     * Seeding corners would make the Delaunay hull span the whole image and
     * the triangle layer would overwrite (and waste) the splat base. */

    /* Seeds are fixed from here on: they define the Voronoi background that
     * refinement splats paint over. */
    s.nseed = s.ns;
    uint8_t *bg = (uint8_t *)malloc(rawsz);
    /* Incremental splat layer (optimization #11, v0.9.0): background +
     * every refinement splat painted ONCE, at insertion time. Each pass
     * then only adds the triangle/block layers on top, instead of
     * re-rendering the whole growing splat field from scratch (the old
     * hot spot: O(passes × splats × kernel) exp() calls). */
    uint8_t *splat_layer = (uint8_t *)malloc(rawsz);
    if (!bg || !splat_layer) {
        acc_scene_free(&s);
        free(render); free(err); free(cand); free(tris);
        free(bg); free(splat_layer);
        return QTC_ERR_ALLOC;
    }
    acc_seed_bg(&s, bg);
    memcpy(splat_layer, bg, rawsz);

    size_t bytes_used = s.ns * splat_b + s.nv * vert_b;

    double *dxs = (double *)malloc((ACC_MAX_VERT + 4) * sizeof(double));
    double *dys = (double *)malloc((ACC_MAX_VERT + 4) * sizeof(double));
    if (!dxs || !dys) {
        acc_scene_free(&s);
        free(render); free(err); free(cand); free(tris); free(dxs); free(dys);
        free(bg); free(splat_layer);
        return QTC_ERR_ALLOC;
    }

    size_t tri_nv = 0;   /* vertex count the current triangulation covers */

    for (int pass = 0; pass < 48 && bytes_used < budget; pass++) {
        /* Triangulate only when the vertex set actually grew: points are
         * never moved, so the Delaunay result depends only on the set. */
        if (s.nv != tri_nv) {
            free(tris); tris = NULL; ntris = 0;
            if (s.nv >= 3) {
                for (size_t i = 0; i < s.nv; i++) {
                    dxs[i] = (double)s.vx[i];
                    dys[i] = (double)s.vy[i];
                }
                int rc = qtc_delaunay(dxs, dys, (int)s.nv, &tris, &ntris);
                if (rc != QTC_OK) {
                    acc_scene_free(&s);
                    free(render); free(err); free(cand); free(tris);
                    free(dxs); free(dys); free(bg); free(splat_layer);
                    return rc;
                }
            }
            tri_nv = s.nv;
        }
        memcpy(render, splat_layer, rawsz);
        acc_render_over(&s, tris, ntris, render);

        for (size_t i = 0; i < npix; i++) {
            double e = 0.0;
            for (uint8_t q = 0; q < ch; q++) {
                double d = (double)pixels[i * ch + q] -
                           (double)render[i * ch + q];
                e += d * d;
            }
            err[i] = e;
        }

        size_t ncand = 0;
        for (size_t i = 0; i < npix; i++)
            if (!s.bmark[i] && !s.isvert[i] && err[i] > 0.5) {
                cand[ncand].e = err[i];
                cand[ncand].i = i;
                ncand++;
            }
        if (ncand == 0)
            break;

        /* Only the top `allowance/4 + 4096` candidates can possibly be
         * consumed this pass (the cheapest primitive is a mono vertex at
         * 5 bytes; 4 is a safe cost lower bound, and the slack covers
         * candidates skipped because an earlier primitive covered them),
         * so select just that many instead of sorting all ~npix
         * candidates — the second profile hot spot. Quickselect
         * partitions in O(ncand); the selected subset is then sorted so
         * within-pass processing stays highest-error-first, exactly as
         * before. Unspent budget simply carries into the next pass,
         * which is cheap now that rendering is incremental. */
        size_t allowance = (budget - bytes_used) / 6 + 512;
        size_t want = allowance / 4 + 4096;
        if (want > ncand)
            want = ncand;
        if (want < ncand)
            acc_select_top(cand, ncand, want);
        qsort(cand, want, sizeof(acc_cand_t), acc_cand_desc);

        size_t pass_bytes = 0;
        size_t added = 0;

        for (size_t k = 0; k < want; k++) {
            if (bytes_used >= budget || pass_bytes >= allowance)
                break;
            size_t i = cand[k].i;
            if (s.bmark[i] || s.isvert[i] || s.smark[i])
                continue;
            uint32_t x = (uint32_t)(i % w), y = (uint32_t)(i / w);

            double sig_x = 5.0, sig_y = 5.0;
            int kind = classify_patch(pixels, w, h, ch, x, y, &sig_x, &sig_y);
            if (kind == 1 && s.nv >= ACC_MAX_VERT)
                kind = 0;                  /* vert pool full → splat */
            if (kind == 0 && s.ns >= ACC_MAX_SPLAT)
                continue;
            if (kind == 2 && s.nb >= ACC_MAX_BLOCK)
                continue;

            size_t cost;
            int arc;
            if (kind == 2) {
                arc = acc_add_block(&s, x, y, pixels, cand[k].e);
                if (arc == QTC_ERR_SIZE)
                    continue;
                if (arc != QTC_OK)
                    goto fail_encode;
                cost = 7;                  /* rect header incl. shift byte */
                uint32_t bw = 8, bh = 8;
                if (x + bw > w) bw = w - x;
                if (y + bh > h) bh = h - y;
                cost += (size_t)bw * bh * ch;
            } else if (kind == 1) {
                arc = acc_add_vert(&s, x, y, pixels);
                if (arc == QTC_ERR_SIZE)
                    continue;
                if (arc != QTC_OK)
                    goto fail_encode;
                cost = vert_b;
            } else {
                /* Exact center color, not a window mean: overlapping
                 * normalized kernels of exact samples converge to the true
                 * surface as density grows; window means would stay blurry. */
                memcpy(col, pixels + i * ch, ch);
                arc = acc_add_splat(&s, x, y, sig_x, sig_y, col);
                if (arc == QTC_ERR_SIZE)
                    continue;
                if (arc != QTC_OK)
                    goto fail_encode;
                /* Paint the new splat into the persistent layer once. */
                acc_splat_paint(&s, splat_layer, s.ns - 1);
                cost = splat_b;
            }
            bytes_used += cost;
            pass_bytes += cost;
            added++;
        }
        if (added == 0)
            break;
    }

    free(tris);
    free(dxs);
    free(dys);
    free(bg);
    free(splat_layer);
    goto serialize;

fail_encode:
    acc_scene_free(&s);
    free(render);
    free(err);
    free(cand);
    free(tris);
    free(dxs);
    free(dys);
    free(bg);
    return QTC_ERR_ALLOC;   /* only allocation failure reaches here */

serialize:
    ;
    /* Serialize: header + [splat|vert|block] records, LZ over the body. */
    size_t body = s.ns * splat_b + s.nv * vert_b;
    for (size_t k = 0; k < s.nb; k++)
        body += 7 + (size_t)s.bw[k] * s.bh[k] * ch;

    uint8_t *raw = (uint8_t *)malloc(body ? body : 1);
    if (!raw) {
        acc_scene_free(&s);
        free(render); free(err); free(cand);
        return QTC_ERR_ALLOC;
    }    size_t o = 0;
    for (size_t k = 0; k < s.ns; k++) {
        raw[o++] = (uint8_t)(s.scx[k] & 0xFF);
        raw[o++] = (uint8_t)(s.scx[k] >> 8);
        raw[o++] = (uint8_t)(s.scy[k] & 0xFF);
        raw[o++] = (uint8_t)(s.scy[k] >> 8);
        raw[o++] = s.ssx[k];                 /* x sigma (was: iso + rsv) */
        raw[o++] = s.ssy[k];                 /* y sigma (v0.8.0) */
        memcpy(raw + o, s.scol + k * ch, ch);
        o += ch;
    }
    for (size_t k = 0; k < s.nv; k++) {
        raw[o++] = (uint8_t)(s.vx[k] & 0xFF);
        raw[o++] = (uint8_t)(s.vx[k] >> 8);
        raw[o++] = (uint8_t)(s.vy[k] & 0xFF);
        raw[o++] = (uint8_t)(s.vy[k] >> 8);
        memcpy(raw + o, s.vcol + k * ch, ch);
        o += ch;
    }
    for (size_t k = 0; k < s.nb; k++) {
        raw[o++] = (uint8_t)(s.bx[k] & 0xFF);
        raw[o++] = (uint8_t)(s.bx[k] >> 8);
        raw[o++] = (uint8_t)(s.by[k] & 0xFF);
        raw[o++] = (uint8_t)(s.by[k] >> 8);
        raw[o++] = (uint8_t)s.bw[k];
        raw[o++] = (uint8_t)s.bh[k];
        raw[o++] = s.bshift[k];              /* per-block shift (v0.8.0) */
        size_t n = (size_t)s.bw[k] * s.bh[k] * ch;
        memcpy(raw + o, s.bdata + s.boff[k], n);
        o += n;
    }

    acc_scene_free(&s);
    free(render);
    free(err);
    free(cand);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(raw, body, &lz, &lz_len);
    free(raw);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(ACC_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = (uint8_t)shift;
    put32(buf + 10, (uint32_t)s.ns);
    put32(buf + 14, (uint32_t)s.nv);
    put32(buf + 18, (uint32_t)s.nb);
    put32(buf + 22, (uint32_t)body);
    put32(buf + 26, (uint32_t)s.nseed);
    memcpy(buf + ACC_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = ACC_HDR + lz_len;
    return QTC_OK;
}

/* ── decode ──────────────────────────────────────────────────────────── */

int qtc_accretion_decode(const uint8_t *payload, size_t payload_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch)
{
    if (!payload || !out_pixels || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_pixels = NULL;

    if (payload_len < ACC_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];
    int shift = payload[9];
    uint32_t ns = get32(payload + 10);
    uint32_t nv = get32(payload + 14);
    uint32_t nbk = get32(payload + 18);
    uint32_t body_len = get32(payload + 22);
    uint32_t nseed = get32(payload + 26);

    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_CORRUPT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_CORRUPT;
    if (shift > 7)
        return QTC_ERR_CORRUPT;

    size_t npix = (size_t)w * h;
    if (npix > ((size_t)1 << 30))
        return QTC_ERR_SIZE;
    size_t rawsz = npix * ch;

    /* A scene never carries more primitives than pixels, and blocks never
     * exceed one record minimum each; reject impossible geometry before
     * allocating anything derived from these counts. */
    if ((uint64_t)ns > (uint64_t)npix + 1 || (uint64_t)nv > (uint64_t)npix + 1)
        return QTC_ERR_CORRUPT;
    if ((uint64_t)nbk > (uint64_t)npix + 1)
        return QTC_ERR_CORRUPT;
    if ((uint64_t)body_len > (uint64_t)rawsz + (uint64_t)npix * 6 + 65536u)
        return QTC_ERR_SIZE;

    uint8_t *raw = (uint8_t *)malloc(body_len ? body_len : 1);
    if (!raw)
        return QTC_ERR_ALLOC;
    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + ACC_HDR, payload_len - ACC_HDR,
                           body_len, raw, &consumed);
    if (rc != QTC_OK) {
        free(raw);
        return rc;
    }

    acc_scene_t s;
    memset(&s, 0, sizeof(s));
    s.w = w; s.h = h; s.ch = ch; s.npix = npix; s.shift = shift;
    s.nseed = nseed;
    uint8_t *px = (uint8_t *)malloc(rawsz);
    if (!px) {
        free(raw);
        acc_scene_free(&s);
        return QTC_ERR_ALLOC;
    }

    size_t o = 0;
    int bad = 0;
    for (uint32_t k = 0; k < ns && !bad; k++) {
        if (o + 6 + ch > body_len) { bad = 1; break; }
        uint32_t cx = (uint32_t)raw[o] | ((uint32_t)raw[o + 1] << 8);
        uint32_t cy = (uint32_t)raw[o + 2] | ((uint32_t)raw[o + 3] << 8);
        uint8_t sqx = raw[o + 4];
        uint8_t sqy = raw[o + 5];
        o += 6;
        if (cx >= w || cy >= h) { bad = 1; break; }
        if (s.ns == s.cs) {
            size_t nc = s.cs ? s.cs * 2 : 1024;
            s.scx = (uint16_t *)realloc(s.scx, nc * sizeof(uint16_t));
            s.scy = (uint16_t *)realloc(s.scy, nc * sizeof(uint16_t));
            s.ssx = (uint8_t *)realloc(s.ssx, nc);
            s.ssy = (uint8_t *)realloc(s.ssy, nc);
            s.scol = (uint8_t *)realloc(s.scol, nc * ch);
            if (!s.scx || !s.scy || !s.ssx || !s.ssy || !s.scol) {
                free(raw);
                acc_scene_free(&s);
                free(px);
                return QTC_ERR_ALLOC;
            }
            s.cs = nc;
        }
        s.scx[s.ns] = (uint16_t)cx;
        s.scy[s.ns] = (uint16_t)cy;
        s.ssx[s.ns] = sqx;
        s.ssy[s.ns] = sqy;
        memcpy(s.scol + s.ns * ch, raw + o, ch);
        o += ch;
        s.ns++;
    }
    if (!bad)
        for (uint32_t k = 0; k < nv && !bad; k++) {
            if (o + 4 + ch > body_len) { bad = 1; break; }
            uint32_t x = (uint32_t)raw[o] | ((uint32_t)raw[o + 1] << 8);
            uint32_t y = (uint32_t)raw[o + 2] | ((uint32_t)raw[o + 3] << 8);
            o += 4;
            if (x >= w || y >= h) { bad = 1; break; }
            if (s.nv == s.cv) {
                size_t nc = s.cv ? s.cv * 2 : 1024;
                s.vx = (uint32_t *)realloc(s.vx, nc * sizeof(uint32_t));
                s.vy = (uint32_t *)realloc(s.vy, nc * sizeof(uint32_t));
                s.vcol = (uint8_t *)realloc(s.vcol, nc * ch);
                if (!s.vx || !s.vy || !s.vcol) {
                    free(raw);
                    acc_scene_free(&s);
                    free(px);
                    return QTC_ERR_ALLOC;
                }
                s.cv = nc;
            }
            s.vx[s.nv] = x;
            s.vy[s.nv] = y;
            memcpy(s.vcol + s.nv * ch, raw + o, ch);
            o += ch;
            s.nv++;
        }
    if (!bad)
        for (uint32_t k = 0; k < nbk && !bad; k++) {
            if (o + 7 > body_len) { bad = 1; break; }
            uint32_t x = (uint32_t)raw[o] | ((uint32_t)raw[o + 1] << 8);
            uint32_t y = (uint32_t)raw[o + 2] | ((uint32_t)raw[o + 3] << 8);
            uint32_t bw = raw[o + 4], bh = raw[o + 5];
            uint32_t bsh = raw[o + 6];
            o += 7;
            if (bw == 0 || bh == 0 || bw > 16 || bh > 16) { bad = 1; break; }
            if (bsh > 3) { bad = 1; break; }   /* per-block shift range */
            if (x >= w || y >= h || x + bw > w || y + bh > h) { bad = 1; break; }
            size_t n = (size_t)bw * bh * ch;
            if (o + n > body_len) { bad = 1; break; }
            if (s.nb == s.cb) {
                size_t nc = s.cb ? s.cb * 2 : 256;
                s.bx = (uint32_t *)realloc(s.bx, nc * sizeof(uint32_t));
                s.by = (uint32_t *)realloc(s.by, nc * sizeof(uint32_t));
                s.bw = (uint32_t *)realloc(s.bw, nc * sizeof(uint32_t));
                s.bh = (uint32_t *)realloc(s.bh, nc * sizeof(uint32_t));
                s.boff = (size_t *)realloc(s.boff, nc * sizeof(size_t));
                s.bshift = (uint8_t *)realloc(s.bshift, nc);
                if (!s.bx || !s.by || !s.bw || !s.bh || !s.boff || !s.bshift) {
                    free(raw);
                    acc_scene_free(&s);
                    free(px);
                    return QTC_ERR_ALLOC;
                }
                s.cb = nc;
            }
            s.bx[s.nb] = x; s.by[s.nb] = y;
            s.bw[s.nb] = bw; s.bh[s.nb] = bh;
            s.boff[s.nb] = o;
            s.bshift[s.nb] = (uint8_t)bsh;
            s.nb++;
            o += n;
        }
    if (bad || o != body_len) {
        free(raw);
        acc_scene_free(&s);
        free(px);
        return QTC_ERR_CORRUPT;
    }

    /* Point block data at the raw buffer (not copied; raw stays alive). */
    s.bdata = raw;
    s.bdlen = body_len;
    s.cbd = body_len;

    int *tris = NULL;
    int ntris = 0;
    if (s.nv >= 3) {
        double *dxs = (double *)malloc(s.nv * sizeof(double));
        double *dys = (double *)malloc(s.nv * sizeof(double));
        if (!dxs || !dys) {
            free(dxs); free(dys);
            acc_scene_free(&s);
            free(px);
            return QTC_ERR_ALLOC;
        }
        for (size_t i = 0; i < s.nv; i++) {
            dxs[i] = (double)s.vx[i];
            dys[i] = (double)s.vy[i];
        }
        rc = qtc_delaunay(dxs, dys, (int)s.nv, &tris, &ntris);
        free(dxs); free(dys);
        if (rc != QTC_OK) {
            acc_scene_free(&s);
            free(px);
            return rc;
        }
    }

    /* Background: Voronoi of the seed splats (validated below), then the
     * refinement layers paint on top — mirrors the encoder exactly. */
    if (s.nseed == 0 || s.nseed > s.ns) {
        free(tris);
        acc_scene_free(&s);
        free(px);
        return QTC_ERR_CORRUPT;
    }
    acc_seed_bg(&s, px);
    acc_render(&s, tris, ntris, px);
    free(tris);
    acc_scene_free(&s);                    /* frees raw via bdata alias */

    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}
