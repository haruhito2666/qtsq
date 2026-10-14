/* ═══════════════════════════════════════════════════════════════════
 *  QTSQ Triangle Mesh Compression Engine (V3)
 *
 *  Represents images as adaptive Delaunay triangulations:
 *  - Dense triangles along edges (captures detail)
 *  - Sparse triangles in smooth areas (extreme compression)
 *  - GPU-native: triangles can be rasterized with hardware
 *  - Resolution-independent: re-rasterize at any resolution
 *
 *  Algorithm:
 *  1. Sobel edge detection → gradient magnitude map
 *  2. Importance-weighted vertex sampling (more vertices near edges)
 *  3. Bowyer-Watson Delaunay triangulation
 *  4. Vertex color sampling from original image
 *  5. Barycentric rasterization for decompression
 *
 *  Author: Haruhito — Black Hole Disk Project
 * ═══════════════════════════════════════════════════════════════════ */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ── Data Structures ── */

typedef struct {
    float x, y;
    uint8_t r, g, b;
} tri_vertex_t;

typedef struct {
    int v[3];          /* vertex indices */
    float cx, cy, cr2; /* circumcircle center + radius² */
    int alive;         /* 0 = deleted */
} tri_face_t;

typedef struct {
    tri_vertex_t *verts;
    int num_verts;
    int cap_verts;
    tri_face_t *faces;
    int num_faces;
    int cap_faces;
} tri_mesh_t;

/* ── Circumcircle computation ── */

static void compute_circumcircle(tri_face_t *f, const tri_vertex_t *verts) {
    float ax = verts[f->v[0]].x, ay = verts[f->v[0]].y;
    float bx = verts[f->v[1]].x, by = verts[f->v[1]].y;
    float cx = verts[f->v[2]].x, cy = verts[f->v[2]].y;

    float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (fabsf(D) < 1e-10f) {
        f->cx = (ax + bx + cx) / 3.0f;
        f->cy = (ay + by + cy) / 3.0f;
        f->cr2 = 1e18f; /* degenerate — infinite circumcircle */
        return;
    }

    float ax2 = ax * ax + ay * ay;
    float bx2 = bx * bx + by * by;
    float cx2 = cx * cx + cy * cy;

    f->cx = (ax2 * (by - cy) + bx2 * (cy - ay) + cx2 * (ay - by)) / D;
    f->cy = (ax2 * (cx - bx) + bx2 * (ax - cx) + cx2 * (bx - ax)) / D;
    f->cr2 = (f->cx - ax) * (f->cx - ax) + (f->cy - ay) * (f->cy - ay);
}

static int point_in_circumcircle(const tri_face_t *f, float px, float py) {
    float dx = px - f->cx;
    float dy = py - f->cy;
    return (dx * dx + dy * dy) <= f->cr2;
}

/* ── Mesh allocation ── */

static void mesh_init(tri_mesh_t *m, int vcap, int fcap) {
    m->verts = (tri_vertex_t *)calloc(vcap, sizeof(tri_vertex_t));
    m->cap_verts = vcap;
    m->num_verts = 0;
    m->faces = (tri_face_t *)calloc(fcap, sizeof(tri_face_t));
    m->cap_faces = fcap;
    m->num_faces = 0;
}

static void mesh_free(tri_mesh_t *m) {
    free(m->verts); free(m->faces);
    m->verts = NULL; m->faces = NULL;
}

static int mesh_add_vert(tri_mesh_t *m, float x, float y, uint8_t r, uint8_t g, uint8_t b) {
    if (m->num_verts >= m->cap_verts) {
        uint32_t new_cap = m->cap_verts ? m->cap_verts * 2 : 16;
        tri_vertex_t *nv = (tri_vertex_t *)realloc(m->verts, new_cap * sizeof(tri_vertex_t));
        if (!nv) return -1; /* keep old buffer; caller checks */
        m->verts = nv;
        m->cap_verts = new_cap;
    }
    int idx = m->num_verts++;
    m->verts[idx] = (tri_vertex_t){x, y, r, g, b};
    return idx;
}

static int mesh_add_face(tri_mesh_t *m, int v0, int v1, int v2) {
    if (m->num_faces >= m->cap_faces) {
        uint32_t new_cap = m->cap_faces ? m->cap_faces * 2 : 16;
        tri_face_t *nf = (tri_face_t *)realloc(m->faces, new_cap * sizeof(tri_face_t));
        if (!nf) return -1; /* keep old buffer; caller checks */
        m->faces = nf;
        m->cap_faces = new_cap;
    }
    int idx = m->num_faces++;
    m->faces[idx].v[0] = v0;
    m->faces[idx].v[1] = v1;
    m->faces[idx].v[2] = v2;
    m->faces[idx].alive = 1;
    compute_circumcircle(&m->faces[idx], m->verts);
    return idx;
}

/* ── Bowyer-Watson Delaunay triangulation ──
 *
 * Incremental algorithm:
 * 1. Start with a super-triangle containing all points
 * 2. For each point:
 *    a. Find all triangles whose circumcircle contains the point
 *    b. Remove those triangles, creating a polygonal hole
 *    c. Create new triangles from the point to each edge of the hole
 * 3. Remove triangles connected to super-triangle vertices
 */

typedef struct { int a, b; } edge_t;

static void bowyer_watson(tri_mesh_t *m, int start_vert, int end_vert) {
    /* Create super-triangle (vertices at indices 0, 1, 2) */
    /* These should already be placed at extreme positions */

    /* Temporary edge buffer for the polygonal hole */
    int edge_cap = 256;
    edge_t *edges = (edge_t *)malloc(edge_cap * sizeof(edge_t));
    int num_edges;

    /* Insert each point */
    for (int pi = start_vert; pi < end_vert; pi++) {
        float px = m->verts[pi].x;
        float py = m->verts[pi].y;
        num_edges = 0;

        /* Find all bad triangles (point is inside their circumcircle) */
        for (int fi = 0; fi < m->num_faces; fi++) {
            if (!m->faces[fi].alive) continue;
            if (point_in_circumcircle(&m->faces[fi], px, py)) {
                /* Collect edges of this triangle */
                for (int e = 0; e < 3; e++) {
                    int a = m->faces[fi].v[e];
                    int b = m->faces[fi].v[(e + 1) % 3];

                    if (num_edges + 1 >= edge_cap) {
                        edge_cap *= 2;
                        edges = (edge_t *)realloc(edges, edge_cap * sizeof(edge_t));
                    }
                    edges[num_edges++] = (edge_t){a, b};
                }
                m->faces[fi].alive = 0; /* remove this triangle */
            }
        }

        /* Find boundary edges (edges that appear exactly once) */
        for (int i = 0; i < num_edges; i++) {
            if (edges[i].a < 0) continue; /* already marked duplicate */
            for (int j = i + 1; j < num_edges; j++) {
                if (edges[j].a < 0) continue;
                /* Check if edges are the same (in either direction) */
                if ((edges[i].a == edges[j].b && edges[i].b == edges[j].a) ||
                    (edges[i].a == edges[j].a && edges[i].b == edges[j].b)) {
                    edges[i].a = -1; /* mark as shared (not boundary) */
                    edges[j].a = -1;
                    break;
                }
            }
        }

        /* Create new triangles from boundary edges to the new point */
        for (int i = 0; i < num_edges; i++) {
            if (edges[i].a < 0) continue; /* skip shared edges */
            mesh_add_face(m, edges[i].a, edges[i].b, pi);
        }
    }

    free(edges);
}

/* ── Sobel edge detection ── */

static float *compute_gradient_magnitude(const uint8_t *rgb,
                                          uint32_t w, uint32_t h) {
    float *grad = (float *)calloc((size_t)w * h, sizeof(float));
    if (!grad) return NULL;

    for (uint32_t y = 1; y < h - 1; y++) {
        for (uint32_t x = 1; x < w - 1; x++) {
            /* Compute luminance-based Sobel gradient */
            float lum[9];
            int k = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    size_t idx = ((y + dy) * w + (x + dx)) * 3;
                    lum[k++] = 0.299f * rgb[idx] + 0.587f * rgb[idx+1] + 0.114f * rgb[idx+2];
                }
            }
            /* Sobel kernels:
             * Gx = [-1 0 1; -2 0 2; -1 0 1]
             * Gy = [-1 -2 -1; 0 0 0; 1 2 1] */
            float gx = -lum[0] + lum[2] - 2*lum[3] + 2*lum[5] - lum[6] + lum[8];
            float gy = -lum[0] - 2*lum[1] - lum[2] + lum[6] + 2*lum[7] + lum[8];
            grad[y * w + x] = sqrtf(gx * gx + gy * gy);
        }
    }
    return grad;
}

/* ── Importance-weighted vertex placement ── */

static void place_vertices(tri_mesh_t *m, const uint8_t *rgb,
                            const float *grad, uint32_t w, uint32_t h,
                            int target_verts) {
    /* Phase 1: Add image corners and boundary vertices */
    /* (Super-triangle vertices are at 0, 1, 2 — skip those) */

    /* Corner vertices */
    mesh_add_vert(m, 0, 0, rgb[0], rgb[1], rgb[2]);
    mesh_add_vert(m, (float)(w-1), 0, rgb[((w-1))*3], rgb[((w-1))*3+1], rgb[((w-1))*3+2]);
    mesh_add_vert(m, 0, (float)(h-1), rgb[((h-1)*w)*3], rgb[((h-1)*w)*3+1], rgb[((h-1)*w)*3+2]);
    mesh_add_vert(m, (float)(w-1), (float)(h-1),
                  rgb[((h-1)*w+(w-1))*3], rgb[((h-1)*w+(w-1))*3+1], rgb[((h-1)*w+(w-1))*3+2]);

    /* Phase 2: Add boundary vertices along edges */
    int border_step = (int)fmaxf(8.0f, (float)(w + h) / 200.0f);
    for (uint32_t x = border_step; x < w - 1; x += border_step) {
        size_t i = x * 3;
        mesh_add_vert(m, (float)x, 0, rgb[i], rgb[i+1], rgb[i+2]);
        i = ((h-1) * w + x) * 3;
        mesh_add_vert(m, (float)x, (float)(h-1), rgb[i], rgb[i+1], rgb[i+2]);
    }
    for (uint32_t y = border_step; y < h - 1; y += border_step) {
        size_t i = (y * w) * 3;
        mesh_add_vert(m, 0, (float)y, rgb[i], rgb[i+1], rgb[i+2]);
        i = (y * w + (w-1)) * 3;
        mesh_add_vert(m, (float)(w-1), (float)y, rgb[i], rgb[i+1], rgb[i+2]);
    }

    /* Phase 3: Interior vertices — importance sampling */
    /* Divide image into cells, allocate vertices proportional to max gradient */
    int remaining = target_verts - m->num_verts + 3; /* +3 for super-triangle */
    if (remaining < 100) remaining = 100;

    /* Cell grid: ~32x32 pixels per cell */
    int cell_size = 32;
    int cols = ((int)w + cell_size - 1) / cell_size;
    int rows = ((int)h + cell_size - 1) / cell_size;
    int ncells = cols * rows;

    float *cell_importance = (float *)calloc(ncells, sizeof(float));
    float total_importance = 0.0f;

    /* Compute importance per cell (sum of gradients) */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int cx = (int)x / cell_size;
            int cy = (int)y / cell_size;
            if (cx >= cols) cx = cols - 1;
            if (cy >= rows) cy = rows - 1;
            float g = grad[y * w + x];
            cell_importance[cy * cols + cx] += g;
        }
    }

    /* Add a minimum baseline so smooth areas still get some vertices */
    for (int i = 0; i < ncells; i++) {
        cell_importance[i] += 1.0f; /* baseline importance */
        total_importance += cell_importance[i];
    }

    /* Allocate vertices proportional to importance */
    /* Use deterministic grid-jittered placement within each cell */
    uint32_t seed = 42;
    for (int ci = 0; ci < ncells; ci++) {
        int num_cell_verts = (int)(remaining * cell_importance[ci] / total_importance);
        if (num_cell_verts < 1) num_cell_verts = 1;

        int cell_x = (ci % cols) * cell_size;
        int cell_y = (ci / cols) * cell_size;
        int cell_w = cell_size;
        int cell_h = cell_size;
        if (cell_x + cell_w > (int)w) cell_w = (int)w - cell_x;
        if (cell_y + cell_h > (int)h) cell_h = (int)h - cell_y;

        for (int v = 0; v < num_cell_verts && m->num_verts < m->cap_verts - 3; v++) {
            /* Simple LCG random for deterministic jitter */
            seed = seed * 1664525u + 1013904223u;
            int lx = (int)(seed % (uint32_t)cell_w);
            seed = seed * 1664525u + 1013904223u;
            int ly = (int)(seed % (uint32_t)cell_h);

            int px = cell_x + lx;
            int py = cell_y + ly;
            if (px >= (int)w) px = (int)w - 1;
            if (py >= (int)h) py = (int)h - 1;

            size_t idx = ((uint32_t)py * w + (uint32_t)px) * 3;
            mesh_add_vert(m, (float)px, (float)py, rgb[idx], rgb[idx+1], rgb[idx+2]);
        }
    }

    free(cell_importance);
}


/* ═══════════════════════════════════════════════════════════════
 *  PUBLIC API: Triangle mesh compress/decompress
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Compress RGB image to triangle mesh.
 * Returns malloc'd buffer with encoded mesh data.
 *
 * Format:
 *   [4] width
 *   [4] height
 *   [4] num_verts (excluding super-triangle)
 *   [4] num_faces (alive, excluding super-triangle)
 *   For each vertex: [2] x_u16 [2] y_u16 [1] r [1] g [1] b  = 7 bytes
 *   For each face:   [4] v0 [4] v1 [4] v2                    = 12 bytes
 */
int qtsq_triangle_compress(const uint8_t *rgb, uint32_t width, uint32_t height,
                            uint8_t **out_data, size_t *out_size) {
    /* 1. Edge detection */
    float *grad = compute_gradient_magnitude(rgb, width, height);
    if (!grad) return -1;

    /* 2. Determine vertex budget based on image size */
    size_t pixel_count = (size_t)width * height;
    /* ~1 vertex per 256 pixels for good quality, max 100K */
    int target_verts = (int)(pixel_count / 256);
    if (target_verts > 100000) target_verts = 100000;
    if (target_verts < 500) target_verts = 500;

    /* 3. Build mesh */
    tri_mesh_t mesh;
    mesh_init(&mesh, target_verts + 100, target_verts * 3);

    /* Add super-triangle (3 vertices far outside the image) */
    float sw = (float)width, sh = (float)height;
    float margin = fmaxf(sw, sh) * 3.0f;
    mesh_add_vert(&mesh, sw / 2.0f, -margin,     0, 0, 0);  /* 0: top */
    mesh_add_vert(&mesh, -margin,   sh + margin,  0, 0, 0);  /* 1: bottom-left */
    mesh_add_vert(&mesh, sw + margin, sh + margin, 0, 0, 0); /* 2: bottom-right */
    mesh_add_face(&mesh, 0, 1, 2);

    /* 4. Place vertices with importance sampling */
    int first_real_vert = mesh.num_verts; /* = 3 (after super-triangle verts) */
    place_vertices(&mesh, rgb, grad, width, height, target_verts);
    free(grad);

    /* 5. Delaunay triangulation */
    bowyer_watson(&mesh, first_real_vert, mesh.num_verts);

    /* 6. Count alive faces (excluding super-triangle connections) */
    int alive_faces = 0;
    for (int fi = 0; fi < mesh.num_faces; fi++) {
        if (!mesh.faces[fi].alive) continue;
        if (mesh.faces[fi].v[0] < 3 || mesh.faces[fi].v[1] < 3 || mesh.faces[fi].v[2] < 3)
            continue;
        alive_faces++;
    }

    int real_verts = mesh.num_verts - 3; /* exclude super-triangle */

    /* 7. Encode */
    size_t header = 16; /* width + height + num_verts + num_faces */
    size_t vert_size = (size_t)real_verts * 7;  /* x:2 + y:2 + rgb:3 */
    size_t face_size = (size_t)alive_faces * 6; /* 3 × uint16 indices (relative to real verts) */
    size_t total = header + vert_size + face_size;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { mesh_free(&mesh); return -1; }

    size_t pos = 0;
    memcpy(buf + pos, &width, 4);  pos += 4;
    memcpy(buf + pos, &height, 4); pos += 4;
    uint32_t nv = (uint32_t)real_verts;
    uint32_t nf = (uint32_t)alive_faces;
    memcpy(buf + pos, &nv, 4); pos += 4;
    memcpy(buf + pos, &nf, 4); pos += 4;

    /* Write vertices (skip super-triangle verts 0,1,2) */
    for (int i = 3; i < mesh.num_verts; i++) {
        uint16_t x = (uint16_t)(mesh.verts[i].x + 0.5f);
        uint16_t y = (uint16_t)(mesh.verts[i].y + 0.5f);
        memcpy(buf + pos, &x, 2); pos += 2;
        memcpy(buf + pos, &y, 2); pos += 2;
        buf[pos++] = mesh.verts[i].r;
        buf[pos++] = mesh.verts[i].g;
        buf[pos++] = mesh.verts[i].b;
    }

    /* Write faces (with indices offset by -3) */
    for (int fi = 0; fi < mesh.num_faces; fi++) {
        if (!mesh.faces[fi].alive) continue;
        if (mesh.faces[fi].v[0] < 3 || mesh.faces[fi].v[1] < 3 || mesh.faces[fi].v[2] < 3)
            continue;
        uint16_t i0 = (uint16_t)(mesh.faces[fi].v[0] - 3);
        uint16_t i1 = (uint16_t)(mesh.faces[fi].v[1] - 3);
        uint16_t i2 = (uint16_t)(mesh.faces[fi].v[2] - 3);
        memcpy(buf + pos, &i0, 2); pos += 2;
        memcpy(buf + pos, &i1, 2); pos += 2;
        memcpy(buf + pos, &i2, 2); pos += 2;
    }

    mesh_free(&mesh);
    *out_data = buf;
    *out_size = pos;
    return 0;
}

/*
 * Decompress triangle mesh back to RGB pixels.
 * Returns 0 on success.
 */
int qtsq_triangle_decompress(const uint8_t *data, size_t size,
                              uint8_t **out_rgb, uint32_t *out_w, uint32_t *out_h) {
    if (size < 16) return -1;

    size_t pos = 0;
    uint32_t width, height, nv, nf;
    memcpy(&width, data + pos, 4);  pos += 4;
    memcpy(&height, data + pos, 4); pos += 4;
    memcpy(&nv, data + pos, 4);     pos += 4;
    memcpy(&nf, data + pos, 4);     pos += 4;

    /* Rebuild mesh */
    tri_mesh_t mesh;
    mesh_init(&mesh, (int)nv + 4, (int)nf + 4);

    /* Read vertices */
    for (uint32_t i = 0; i < nv; i++) {
        uint16_t x, y;
        memcpy(&x, data + pos, 2); pos += 2;
        memcpy(&y, data + pos, 2); pos += 2;
        uint8_t r = data[pos++], g = data[pos++], b = data[pos++];
        mesh_add_vert(&mesh, (float)x, (float)y, r, g, b);
    }

    /* Read faces — add directly, set alive, compute circumcircle (not needed for raster) */
    for (uint32_t i = 0; i < nf; i++) {
        uint16_t i0, i1, i2;
        memcpy(&i0, data + pos, 2); pos += 2;
        memcpy(&i1, data + pos, 2); pos += 2;
        memcpy(&i2, data + pos, 2); pos += 2;
        /* Don't need circumcircle for rasterization, just store face */
        if (mesh.num_faces >= mesh.cap_faces) {
            mesh.cap_faces *= 2;
            mesh.faces = (tri_face_t *)realloc(mesh.faces, mesh.cap_faces * sizeof(tri_face_t));
        }
        int fi = mesh.num_faces++;
        mesh.faces[fi].v[0] = (int)i0;
        mesh.faces[fi].v[1] = (int)i1;
        mesh.faces[fi].v[2] = (int)i2;
        mesh.faces[fi].alive = 1;
        mesh.faces[fi].cx = mesh.faces[fi].cy = mesh.faces[fi].cr2 = 0; /* unused */
    }

    /* Allocate output */
    size_t pixel_count = (size_t)width * height;
    uint8_t *rgb = (uint8_t *)malloc(pixel_count * 3);
    if (!rgb) { mesh_free(&mesh); return -1; }

    /* Rasterize — note: no super-triangle verts, so all indices are valid */
    /* Temporarily set the super-triangle skip threshold to 0 */
    /* (The decompressed mesh has no super-triangle vertices) */

    /* Clear output */
    memset(rgb, 0, pixel_count * 3);

    /* Rasterize each triangle */
    for (int fi = 0; fi < mesh.num_faces; fi++) {
        if (!mesh.faces[fi].alive) continue;

        const tri_vertex_t *va = &mesh.verts[mesh.faces[fi].v[0]];
        const tri_vertex_t *vb = &mesh.verts[mesh.faces[fi].v[1]];
        const tri_vertex_t *vc = &mesh.verts[mesh.faces[fi].v[2]];

        int minx = (int)fmaxf(0, floorf(fminf(fminf(va->x, vb->x), vc->x)));
        int maxx = (int)fminf((float)(width - 1), ceilf(fmaxf(fmaxf(va->x, vb->x), vc->x)));
        int miny = (int)fmaxf(0, floorf(fminf(fminf(va->y, vb->y), vc->y)));
        int maxy = (int)fminf((float)(height - 1), ceilf(fmaxf(fmaxf(va->y, vb->y), vc->y)));

        float denom = (vb->y - vc->y) * (va->x - vc->x) +
                      (vc->x - vb->x) * (va->y - vc->y);
        if (fabsf(denom) < 1e-6f) continue;
        float inv = 1.0f / denom;

        for (int py = miny; py <= maxy; py++) {
            for (int px = minx; px <= maxx; px++) {
                float fpx = (float)px, fpy = (float)py;
                float w0 = ((vb->y - vc->y) * (fpx - vc->x) + (vc->x - vb->x) * (fpy - vc->y)) * inv;
                float w1 = ((vc->y - va->y) * (fpx - vc->x) + (va->x - vc->x) * (fpy - vc->y)) * inv;
                float w2 = 1.0f - w0 - w1;

                if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
                    float r = w0 * va->r + w1 * vb->r + w2 * vc->r;
                    float g = w0 * va->g + w1 * vb->g + w2 * vc->g;
                    float b = w0 * va->b + w1 * vb->b + w2 * vc->b;

                    size_t oi = ((uint32_t)py * width + (uint32_t)px) * 3;
                    rgb[oi]     = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : (int)(r + 0.5f)));
                    rgb[oi + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : (int)(g + 0.5f)));
                    rgb[oi + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : (int)(b + 0.5f)));
                }
            }
        }
    }

    mesh_free(&mesh);
    *out_rgb = rgb;
    *out_w = width;
    *out_h = height;
    return 0;
}
