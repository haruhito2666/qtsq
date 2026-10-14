/*
 * qtc_delaunay.c — Phase 5: incremental Delaunay triangulation.
 *
 * Bowyer–Watson algorithm. Given a set of 2D points, produces a triangle
 * mesh whose circumcircle property (no point lies inside the circumcircle of
 * any triangle) makes it the natural basis for the lossy image mesh lens:
 * triangles adapt to the point density, and barycentric interpolation over
 * them reconstructs a smooth surface.
 *
 * Implementation notes:
 *   - A single "super-triangle" large enough to contain every point is added
 *     first, then removed at the end (its vertices are indices 0,1,2 and are
 *     never part of the caller's point set).
 *   - Triangles are stored as three point indices. The mesh is returned as a
 *     flat array of index triples.
 *   - v0.9.0 (optimization #12): two quadratic inner loops were removed.
 *     (a) Each triangle's circumcircle is computed ONCE at creation and
 *         cached, so the bad-triangle scan is a cheap point-in-circle test
 *         instead of recomputing circumcircles for every triangle on every
 *         insertion. (b) The boundary of the bad-triangle polygon is found
 *         by collecting the bad triangles' edges, canonicalizing and sorting
 *         them, and keeping the edges that appear exactly once — O(bad log
 *         bad) instead of the old O(bad^2) pairwise shared-edge scan.
 *     Insertion order is unchanged, so the produced triangulation is
 *     bit-identical to the previous implementation.
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int a, b, c;   /* point indices */
} tri_t;

/* Circumcircle of triangle (a,b,c). Returns 0 if degenerate. */
static int circumcircle(const double *px, const double *py,
                        int a, int b, int c,
                        double *cx, double *cy, double *r2)
{
    double ax = px[a], ay = py[a];
    double bx = px[b], by = py[b];
    double cxp = px[c], cyp = py[c];

    double d = 2.0 * (ax * (by - cyp) + bx * (cyp - ay) + cxp * (ay - by));
    if (fabs(d) < 1e-12)
        return 0;

    double a2 = ax * ax + ay * ay;
    double b2 = bx * bx + by * by;
    double c2 = cxp * cxp + cyp * cyp;

    double ux = (a2 * (by - cyp) + b2 * (cyp - ay) + c2 * (ay - by)) / d;
    double uy = (a2 * (cxp - bx) + b2 * (ax - cxp) + c2 * (bx - ax)) / d;

    double dx = ax - ux, dy = ay - uy;
    *cx = ux;
    *cy = uy;
    *r2 = dx * dx + dy * dy;
    return 1;
}

/* A canonical (undirected) edge, for boundary extraction. */
typedef struct {
    int u, v;      /* u < v */
} edge_t;

static int edge_cmp(const void *a, const void *b)
{
    const edge_t *ea = (const edge_t *)a;
    const edge_t *eb = (const edge_t *)b;
    if (ea->u != eb->u) return ea->u < eb->u ? -1 : 1;
    if (ea->v != eb->v) return ea->v < eb->v ? -1 : 1;
    return 0;
}

int qtc_delaunay(const double *xs, const double *ys, int npts,
                 int **out_tris, int *out_ntris)
{
    if (npts < 3 || !xs || !ys || !out_tris || !out_ntris)
        return QTC_ERR_FORMAT;

    /* Work on a copy of the points so we can prepend the super-triangle. */
    int total = npts + 3;
    double *px = (double *)malloc((size_t)total * sizeof(double));
    double *py = (double *)malloc((size_t)total * sizeof(double));
    if (!px || !py) {
        free(px); free(py);
        return QTC_ERR_ALLOC;
    }

    /* Bounding box of the input. */
    double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < npts; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    double dx = maxx - minx, dy = maxy - miny;
    double dmax = dx > dy ? dx : dy;
    if (dmax < 1e-9) dmax = 1.0;
    double midx = (minx + maxx) * 0.5, midy = (miny + maxy) * 0.5;

    /* Super-triangle vertices at indices 0,1,2. */
    px[0] = midx - 20 * dmax; py[0] = midy - dmax;
    px[1] = midx;             py[1] = midy + 20 * dmax;
    px[2] = midx + 20 * dmax; py[2] = midy - dmax;
    for (int i = 0; i < npts; i++) {
        px[3 + i] = xs[i];
        py[3 + i] = ys[i];
    }

    /* Triangle list (dynamic) with a parallel circumcircle cache. */
    int cap = 256;
    int ntri = 0;
    tri_t *tris = (tri_t *)malloc((size_t)cap * sizeof(tri_t));
    double *ccx = (double *)malloc((size_t)cap * sizeof(double));
    double *ccy = (double *)malloc((size_t)cap * sizeof(double));
    double *cr2 = (double *)malloc((size_t)cap * sizeof(double));
    if (!tris || !ccx || !ccy || !cr2) {
        free(px); free(py); free(tris); free(ccx); free(ccy); free(cr2);
        return QTC_ERR_ALLOC;
    }
    tris[ntri].a = 0; tris[ntri].b = 1; tris[ntri].c = 2;
    if (!circumcircle(px, py, 0, 1, 2, &ccx[ntri], &ccy[ntri], &cr2[ntri]))
        cr2[ntri] = -1.0;            /* degenerate: never contains a point */
    ntri++;

    /* Scratch for bad-triangle marking and the boundary edge list. */
    uint8_t *bad = (uint8_t *)malloc((size_t)cap * sizeof(uint8_t));
    edge_t *edges = NULL;            /* sized per insertion below */
    int *edge = (int *)malloc((size_t)cap * 3 * 2 * sizeof(int));
    if (!bad || !edge) {
        free(px); free(py); free(tris); free(ccx); free(ccy); free(cr2);
        free(bad); free(edge);
        return QTC_ERR_ALLOC;
    }

    for (int p = 3; p < total; p++) {
        double x = px[p], y = py[p];

        /* Mark triangles whose CACHED circumcircle contains the point. */
        int nbad = 0;
        for (int t = 0; t < ntri; t++) {
            bad[t] = 0;
            if (cr2[t] < 0.0)
                continue;
            double ddx = x - ccx[t], ddy = y - ccy[t];
            if (ddx * ddx + ddy * ddy < cr2[t]) {
                bad[t] = 1;
                nbad++;
            }
        }

        /* Collect the bad triangles' edges, canonicalized (u < v). An edge
         * shared by two bad triangles appears twice; a boundary edge appears
         * exactly once. Sorting groups duplicates so a single linear scan
         * extracts the boundary — O(nbad log nbad), replacing the old
         * O(nbad^2) pairwise shared-edge test. */
        if (nbad > 0) {
            edge_t *ne = (edge_t *)realloc(edges,
                                           (size_t)nbad * 3 * sizeof(edge_t));
            if (!ne) {
                free(px); free(py); free(tris); free(ccx); free(ccy);
                free(cr2); free(bad); free(edge); free(edges);
                return QTC_ERR_ALLOC;
            }
            edges = ne;
        }
        int nedge_raw = 0;
        for (int t = 0; t < ntri; t++) {
            if (!bad[t])
                continue;
            int e[3][2] = {
                {tris[t].a, tris[t].b},
                {tris[t].b, tris[t].c},
                {tris[t].c, tris[t].a},
            };
            for (int k = 0; k < 3; k++) {
                int u = e[k][0], v = e[k][1];
                if (u > v) { int tmp = u; u = v; v = tmp; }
                edges[nedge_raw].u = u;
                edges[nedge_raw].v = v;
                nedge_raw++;
            }
        }
        qsort(edges, (size_t)nedge_raw, sizeof(edge_t), edge_cmp);

        int nedge = 0;
        for (int i = 0; i < nedge_raw; ) {
            int j = i + 1;
            while (j < nedge_raw && edges[j].u == edges[i].u &&
                   edges[j].v == edges[i].v)
                j++;
            if (j - i == 1) {        /* appears exactly once → boundary */
                edge[nedge * 2] = edges[i].u;
                edge[nedge * 2 + 1] = edges[i].v;
                nedge++;
            }
            i = j;
        }

        /* Remove bad triangles (compact in place, cache in lockstep). */
        int w = 0;
        for (int t = 0; t < ntri; t++) {
            if (!bad[t]) {
                tris[w] = tris[t];
                ccx[w] = ccx[t]; ccy[w] = ccy[t]; cr2[w] = cr2[t];
                w++;
            }
        }
        ntri = w;

        /* Grow capacity for the new triangles. On failure, free each
         * buffer EXACTLY once: a successful realloc has already consumed
         * the old block, so free the new pointer; a failed one leaves the
         * old pointer owning the memory (audit M1 — freeing both was a
         * double-free). */
        if (ntri + nedge > cap) {
            int ncap = cap * 2;
            while (ncap < ntri + nedge)
                ncap *= 2;
            tri_t *nt = (tri_t *)realloc(tris, (size_t)ncap * sizeof(tri_t));
            if (nt)
                tris = nt;
            double *nx = (double *)realloc(ccx, (size_t)ncap * sizeof(double));
            if (nx)
                ccx = nx;
            double *ny = (double *)realloc(ccy, (size_t)ncap * sizeof(double));
            if (ny)
                ccy = ny;
            double *nr = (double *)realloc(cr2, (size_t)ncap * sizeof(double));
            if (nr)
                cr2 = nr;
            uint8_t *nb = (uint8_t *)realloc(bad, (size_t)ncap * sizeof(uint8_t));
            if (nb)
                bad = nb;
            int *ned = (int *)realloc(edge, (size_t)ncap * 3 * 2 * sizeof(int));
            if (ned)
                edge = ned;
            if (!nt || !nx || !ny || !nr || !nb || !ned) {
                free(px); free(py); free(tris); free(ccx); free(ccy);
                free(cr2); free(bad); free(edge); free(edges);
                return QTC_ERR_ALLOC;
            }
            cap = ncap;
        }

        /* Re-triangulate the hole with the new point, caching each new
         * triangle's circumcircle as it is created. */
        for (int k = 0; k < nedge; k++) {
            tris[ntri].a = edge[k * 2];
            tris[ntri].b = edge[k * 2 + 1];
            tris[ntri].c = p;
            if (!circumcircle(px, py, tris[ntri].a, tris[ntri].b, tris[ntri].c,
                              &ccx[ntri], &ccy[ntri], &cr2[ntri]))
                cr2[ntri] = -1.0;
            ntri++;
        }
    }

    /* Drop triangles that touch the super-triangle (any index < 3). */
    int w = 0;
    for (int t = 0; t < ntri; t++) {
        if (tris[t].a < 3 || tris[t].b < 3 || tris[t].c < 3)
            continue;
        tris[w].a = tris[t].a - 3;
        tris[w].b = tris[t].b - 3;
        tris[w].c = tris[t].c - 3;
        w++;
    }
    ntri = w;

    free(px); free(py); free(ccx); free(ccy); free(cr2);
    free(bad); free(edge); free(edges);

    *out_tris = (int *)tris;
    *out_ntris = ntri;
    return QTC_OK;
}
