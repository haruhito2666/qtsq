/*
 * qtc_image_mesh.c — Phase 5: lossy image lens (adaptive mesh).
 *
 * The "mesh compression" idea from Zenodo paper 2, rewritten standalone:
 * represent the image as a Delaunay triangle mesh whose vertices sit at
 * high-detail locations, then reconstruct by barycentric interpolation.
 * Smooth regions need few vertices; edges and texture attract more.
 *
 * Encode (multi-pass, error-driven vertex placement):
 *   1. Seed with the 4 corners + a coarse grid (guarantees full coverage —
 *      the convex hull of the corners is the whole image rectangle).
 *   2. Repeat: triangulate the current vertices, reconstruct, compute the
 *      per-pixel error, and promote the highest-error pixels to vertices.
 *      Stop when the quality-derived vertex budget is spent.
 *   3. Serialize the vertices (position + sampled color); the triangulation
 *      is NOT stored — decode re-runs Delaunay on the same points.
 *
 * Decode: parse vertices → Delaunay → rasterize each triangle with
 * barycentric interpolation.
 *
 * This is LOSSY. Quality is measured by PSNR/SSIM in the bench harness.
 *
 * Payload layout:
 *   [u32 w][u32 h][u8 ch][u8 reserved][u32 nverts]
 *   [LZ payload of nverts * (u16 x, u16 y, ch color bytes)]
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

/* Candidate carries its sort key inline — no file-scope state, so
 * concurrent mesh encodes are safe (audit M2; qsort takes no context). */
typedef struct { double e; size_t i; } mesh_cand_t;

static int mesh_cand_desc(const void *a, const void *b)
{
    double ea = ((const mesh_cand_t *)a)->e;
    double eb = ((const mesh_cand_t *)b)->e;
    if (ea < eb) return 1;
    if (ea > eb) return -1;
    return 0;
}

/* Rasterize the mesh into out (w*h*ch) by barycentric interpolation.
 * Shared with the Phase 5.5 accretion scene codec (triangle layer). */
void qtc_mesh_render(const uint32_t *vx, const uint32_t *vy,
                     const uint8_t *vcol,
                     const int *tris, int ntris,
                     uint32_t w, uint32_t h, uint8_t ch, uint8_t *out)
{
    for (uint32_t i = 0; i < (uint32_t)ntris; i++) {
        int a = tris[3 * i], b = tris[3 * i + 1], c = tris[3 * i + 2];
        double x0 = vx[a], y0 = vy[a];
        double x1 = vx[b], y1 = vy[b];
        double x2 = vx[c], y2 = vy[c];

        double denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (fabs(denom) < 1e-9)
            continue; /* degenerate */

        /* Bounding box, clamped to the image. */
        double minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        double maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        double miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        double maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        int ix0 = (int)floor(minx); if (ix0 < 0) ix0 = 0;
        int ix1 = (int)ceil(maxx);  if (ix1 > (int)w - 1) ix1 = (int)w - 1;
        int iy0 = (int)floor(miny); if (iy0 < 0) iy0 = 0;
        int iy1 = (int)ceil(maxy);  if (iy1 > (int)h - 1) iy1 = (int)h - 1;

        for (int yy = iy0; yy <= iy1; yy++) {
            for (int xx = ix0; xx <= ix1; xx++) {
                /* Sample at integer pixel coordinates (not centers) so every
                 * sample point lies inside the convex hull of the corner
                 * vertices [0,w-1]x[0,h-1]; center sampling would leave the
                 * last row/column outside the hull and unrendered. */
                double px = xx, py = yy;
                double u = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / denom;
                double v = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / denom;
                double ww = 1.0 - u - v;
                /* Small tolerance so shared edges don't leave gaps. */
                if (u < -1e-6 || v < -1e-6 || ww < -1e-6)
                    continue;
                uint8_t *o = out + ((size_t)yy * w + xx) * ch;
                for (uint8_t k = 0; k < ch; k++) {
                    double val = u * vcol[(size_t)a * ch + k] +
                                 v * vcol[(size_t)b * ch + k] +
                                 ww * vcol[(size_t)c * ch + k];
                    int iv = (int)(val + 0.5);
                    if (iv < 0) iv = 0;
                    if (iv > 255) iv = 255;
                    o[k] = (uint8_t)iv;
                }
            }
        }
    }
}

/* Quality [0,1] → vertex budget as a fraction of the pixel count. The mesh's
 * home turf is smooth content at low-to-mid bitrate, so the fraction stays
 * small: each vertex costs (4 + ch) bytes, and it only wins when vertices are
 * far fewer than pixels.
 *
 * The budget is also capped at an absolute maximum because the Bowyer–Watson
 * triangulation is O(n^2) in the vertex count; beyond ~30k vertices it gets
 * slow. For large images this makes the mesh a low-bitrate coder, which is
 * exactly the regime the Phase 5 gate targets. */
static size_t quality_to_budget(float quality, uint32_t w, uint32_t h)
{
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    double frac = 0.0002 + pow((double)quality, 2.5) * 0.03;
    size_t budget = (size_t)((double)w * h * frac);
    if (budget < 16) budget = 16;
    if (budget > (size_t)w * h / 4) budget = (size_t)w * h / 4;
    if (budget > 30000) budget = 30000;   /* keep O(n^2) Delaunay tractable */
    return budget;
}

int qtc_image_mesh_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                          uint8_t ch, float quality,
                          uint8_t **out, size_t *out_len)
{
    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_FORMAT;

    size_t npix = (size_t)w * h;
    size_t budget = quality_to_budget(quality, w, h);

    /* Vertex storage. */
    size_t vcap = budget + 64;
    uint32_t *vx = (uint32_t *)malloc(vcap * sizeof(uint32_t));
    uint32_t *vy = (uint32_t *)malloc(vcap * sizeof(uint32_t));
    uint8_t *vcol = (uint8_t *)malloc(vcap * ch);
    uint8_t *isvert = (uint8_t *)calloc(npix, 1);
    double *dxs = (double *)malloc(vcap * sizeof(double));
    double *dys = (double *)malloc(vcap * sizeof(double));
    uint8_t *render = (uint8_t *)malloc(npix * ch);
    double *err = (double *)malloc(npix * sizeof(double));
    if (!vx || !vy || !vcol || !isvert || !dxs || !dys || !render || !err) {
        free(vx); free(vy); free(vcol); free(isvert);
        free(dxs); free(dys); free(render); free(err);
        return QTC_ERR_ALLOC;
    }

    int nverts = 0;

    /* Add a vertex at pixel (x,y), sampling its true color. */
    #define ADD_VERT(XX, YY) do {                                       \
        size_t pi = (size_t)(YY) * w + (XX);                            \
        if (!isvert[pi] && nverts < (int)vcap) {                        \
            isvert[pi] = 1;                                             \
            vx[nverts] = (XX); vy[nverts] = (YY);                       \
            memcpy(vcol + (size_t)nverts * ch, pixels + pi * ch, ch);   \
            nverts++;                                                   \
        }                                                               \
    } while (0)

    /* Seeds: corners. */
    ADD_VERT(0, 0);
    ADD_VERT(w - 1, 0);
    ADD_VERT(0, h - 1);
    ADD_VERT(w - 1, h - 1);

    /* Seeds: coarse grid so the initial mesh isn't 2 giant triangles. */
    uint32_t gstep = (w > h ? w : h) / 8;
    if (gstep < 1) gstep = 1;
    for (uint32_t yy = 0; yy < h; yy += gstep)
        for (uint32_t xx = 0; xx < w; xx += gstep)
            ADD_VERT(xx, yy);

    int *tris = NULL;
    int ntris = 0;

    /* Multi-pass error-driven refinement. */
    mesh_cand_t *cand = (mesh_cand_t *)malloc(npix * sizeof(mesh_cand_t));
    if (!cand) {
        free(vx); free(vy); free(vcol); free(isvert);
        free(dxs); free(dys); free(render); free(err);
        return QTC_ERR_ALLOC;
    }
    for (int pass = 0; pass < 64 && (size_t)nverts < budget; pass++) {
        free(tris); tris = NULL;
        for (int i = 0; i < nverts; i++) {
            dxs[i] = (double)vx[i];
            dys[i] = (double)vy[i];
        }
        int rc = qtc_delaunay(dxs, dys, nverts, &tris, &ntris);
        if (rc != QTC_OK) {
            free(vx); free(vy); free(vcol); free(isvert);
            free(dxs); free(dys); free(render); free(err); free(cand);
            return rc;
        }

        memset(render, 0, npix * ch);
        qtc_mesh_render(vx, vy, vcol, tris, ntris, w, h, ch, render);

        /* Per-pixel squared error. */
        for (size_t i = 0; i < npix; i++) {
            double e = 0.0;
            for (uint8_t k = 0; k < ch; k++) {
                double d = (double)pixels[i * ch + k] -
                           (double)render[i * ch + k];
                e += d * d;
            }
            err[i] = e;
        }

        /* Promote the highest-error pixels, up to the remaining budget.
         * Batch size shrinks as we approach the budget for finer tuning. */
        size_t remain = budget - (size_t)nverts;
        size_t batch = remain / 4;
        if (batch < 8) batch = remain;
        if (batch > 1024) batch = 1024;

        /* Collect non-vertex candidates with meaningful error, then take the
         * top `batch` by error in one sort (O(npix log npix), not
         * O(batch * npix)). */
        size_t ncand = 0;
        for (size_t i = 0; i < npix; i++) {
            if (!isvert[i] && err[i] > 0.5) {
                cand[ncand].e = err[i];
                cand[ncand].i = i;
                ncand++;
            }
        }
        if (ncand == 0)
            break;   /* error floor reached */
        if (batch > ncand)
            batch = ncand;

        qsort(cand, ncand, sizeof(mesh_cand_t), mesh_cand_desc);

        for (size_t added = 0; added < batch; added++) {
            size_t best = cand[added].i;
            ADD_VERT((uint32_t)(best % w), (uint32_t)(best / w));
        }
    }
    #undef ADD_VERT

    free(tris);
    free(isvert);
    free(render);
    free(err);
    free(cand);

    /* Images with fewer than 3 distinct pixels (1x1, 1x2, 2x1) cannot form
     * a triangle; the decoder rejects nverts < 3, so refuse at encode rather
     * than emit a payload our own decoder would decline. */
    if (nverts < 3) {
        free(vx); free(vy); free(vcol); free(dxs); free(dys);
        return QTC_ERR_FORMAT;
    }

    /* Serialize vertices: (u16 x, u16 y, ch color bytes) each. */
    size_t per = 2 + 2 + ch;
    size_t raw_len = (size_t)nverts * per;
    uint8_t *raw = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!raw) {
        free(vx); free(vy); free(vcol); free(dxs); free(dys);
        return QTC_ERR_ALLOC;
    }
    for (int i = 0; i < nverts; i++) {
        uint8_t *p = raw + (size_t)i * per;
        p[0] = (uint8_t)(vx[i] & 0xFF);
        p[1] = (uint8_t)(vx[i] >> 8);
        p[2] = (uint8_t)(vy[i] & 0xFF);
        p[3] = (uint8_t)(vy[i] >> 8);
        memcpy(p + 4, vcol + (size_t)i * ch, ch);
    }
    free(vx); free(vy); free(vcol); free(dxs); free(dys);

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(raw, raw_len, &lz, &lz_len);
    free(raw);
    if (rc != QTC_OK)
        return rc;

    size_t hdr = 4 + 4 + 1 + 1 + 4;
    uint8_t *buf = (uint8_t *)malloc(hdr + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = 0;
    put32(buf + 10, (uint32_t)nverts);
    memcpy(buf + hdr, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = hdr + lz_len;
    return QTC_OK;
}

int qtc_image_mesh_decode(const uint8_t *payload, size_t payload_len,
                          uint8_t **out_pixels, uint32_t *out_w,
                          uint32_t *out_h, uint8_t *out_ch)
{
    size_t hdr = 4 + 4 + 1 + 1 + 4;
    if (payload_len < hdr)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];
    uint32_t nverts = get32(payload + 10);

    if (w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_CORRUPT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_CORRUPT;
    if (nverts < 3)
        return QTC_ERR_CORRUPT;
    /* Every vertex sits on a distinct pixel, so a count above w*h is a
     * crafted header — reject before trusting it for allocations. */
    if ((uint64_t)nverts > (uint64_t)w * h)
        return QTC_ERR_CORRUPT;

    size_t npix = (size_t)w * h;
    if (npix > ((size_t)1 << 30))
        return QTC_ERR_SIZE;

    size_t per = 2 + 2 + ch;
    size_t raw_len = (size_t)nverts * per;
    uint8_t *raw = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!raw)
        return QTC_ERR_ALLOC;

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + hdr, payload_len - hdr, raw_len, raw,
                           &consumed);
    if (rc != QTC_OK) {
        free(raw);
        return rc;
    }

    uint32_t *vx = (uint32_t *)malloc((size_t)nverts * sizeof(uint32_t));
    uint32_t *vy = (uint32_t *)malloc((size_t)nverts * sizeof(uint32_t));
    uint8_t *vcol = (uint8_t *)malloc((size_t)nverts * ch);
    double *dxs = (double *)malloc((size_t)nverts * sizeof(double));
    double *dys = (double *)malloc((size_t)nverts * sizeof(double));
    uint8_t *px = (uint8_t *)malloc(npix * ch);
    if (!vx || !vy || !vcol || !dxs || !dys || !px) {
        free(raw); free(vx); free(vy); free(vcol);
        free(dxs); free(dys); free(px);
        return QTC_ERR_ALLOC;
    }

    for (uint32_t i = 0; i < nverts; i++) {
        const uint8_t *p = raw + (size_t)i * per;
        vx[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        vy[i] = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
        if (vx[i] >= w || vy[i] >= h) {
            free(raw); free(vx); free(vy); free(vcol);
            free(dxs); free(dys); free(px);
            return QTC_ERR_CORRUPT;
        }
        memcpy(vcol + (size_t)i * ch, p + 4, ch);
        dxs[i] = (double)vx[i];
        dys[i] = (double)vy[i];
    }
    free(raw);

    int *tris = NULL;
    int ntris = 0;
    rc = qtc_delaunay(dxs, dys, (int)nverts, &tris, &ntris);
    if (rc != QTC_OK) {
        free(vx); free(vy); free(vcol); free(dxs); free(dys); free(px);
        return rc;
    }

    memset(px, 0, npix * ch);
    qtc_mesh_render(vx, vy, vcol, tris, ntris, w, h, ch, px);

    free(tris); free(vx); free(vy); free(vcol); free(dxs); free(dys);

    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}
