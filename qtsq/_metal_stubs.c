/*
 * Metal stubs for Python package builds.
 * Exact signatures matching qtsq_metal.h — return -1 (not available).
 * The read path (qtsq_extract_vertices) never calls these.
 */

#include "qtsq_metal.h"

qtsq_metal_ctx_t* qtsq_metal_init(void) { return NULL; }
qtsq_metal_ctx_t* qtsq_metal_global(void) { return NULL; }
void qtsq_metal_destroy(qtsq_metal_ctx_t *ctx) { (void)ctx; }

int qtsq_metal_rasterize(qtsq_metal_ctx_t *ctx,
                         const tri_vertex_t *verts, uint32_t nv,
                         const tri_face_t *faces, uint32_t nf,
                         uint32_t w, uint32_t h, uint8_t ch,
                         uint8_t *output, uint8_t *blur_map, const uint8_t *tier_mask) {
    (void)ctx;(void)verts;(void)nv;(void)faces;(void)nf;
    (void)w;(void)h;(void)ch;(void)output;(void)blur_map;(void)tier_mask;
    return -1;
}

int qtsq_metal_blur(qtsq_metal_ctx_t *ctx,
                    const uint8_t *input, uint8_t *output,
                    uint32_t w, uint32_t h, uint8_t ch,
                    const uint8_t *blur_map) {
    (void)ctx;(void)input;(void)output;(void)w;(void)h;(void)ch;(void)blur_map;
    return -1;
}

int qtsq_metal_render_splats(qtsq_metal_ctx_t *ctx,
                             const splat2d_t *splats, uint32_t count,
                             uint32_t w, uint32_t h, uint8_t ch,
                             uint8_t *output, const uint8_t *fg_mask) {
    (void)ctx;(void)splats;(void)count;(void)w;(void)h;(void)ch;(void)output;(void)fg_mask;
    return -1;
}

int qtsq_metal_sobel(qtsq_metal_ctx_t *ctx,
                     const uint8_t *pixels, float *gradient,
                     uint32_t w, uint32_t h, uint8_t ch) {
    (void)ctx;(void)pixels;(void)gradient;(void)w;(void)h;(void)ch;
    return -1;
}

int qtsq_metal_bgra_to_rgb(qtsq_metal_ctx_t *ctx,
                           const uint8_t *bgra, uint8_t *rgb,
                           uint32_t w, uint32_t h, uint32_t stride) {
    (void)ctx;(void)bgra;(void)rgb;(void)w;(void)h;(void)stride;
    return -1;
}

int qtsq_metal_frame_diff(qtsq_metal_ctx_t *ctx,
                           const uint8_t *current, const uint8_t *reference,
                           uint8_t *delta, float *change_ratio,
                           uint32_t w, uint32_t h, uint8_t ch, uint32_t threshold) {
    (void)ctx;(void)current;(void)reference;(void)delta;(void)change_ratio;
    (void)w;(void)h;(void)ch;(void)threshold;
    return -1;
}

int qtsq_metal_classify_regions(qtsq_metal_ctx_t *ctx,
                                 const float *gradient, uint8_t *mask,
                                 uint32_t w, uint32_t h, float threshold) {
    (void)ctx;(void)gradient;(void)mask;(void)w;(void)h;(void)threshold;
    return -1;
}

int qtsq_metal_gpu_delaunay(qtsq_metal_ctx_t *ctx,
                             const float *vertex_pos, uint32_t num_vertices,
                             uint32_t *tris_out, uint32_t *num_tris_out,
                             uint32_t max_tris, uint32_t w, uint32_t h) {
    (void)ctx;(void)vertex_pos;(void)num_vertices;
    (void)tris_out;(void)num_tris_out;(void)max_tris;(void)w;(void)h;
    return -1;
}

int qtsq_metal_fit_triangle_colors(qtsq_metal_ctx_t *ctx,
                                    const float *vertex_pos, uint32_t num_vertices,
                                    const uint8_t *source, uint8_t *colors_out,
                                    uint32_t w, uint32_t h, uint8_t ch) {
    (void)ctx;(void)vertex_pos;(void)num_vertices;
    (void)source;(void)colors_out;(void)w;(void)h;(void)ch;
    return -1;
}

int qtsq_metal_fit_splats(qtsq_metal_ctx_t *ctx,
                           const float *splat_pos, uint32_t num_splats,
                           const uint8_t *source, const uint8_t *mask,
                           const float *gradient,
                           uint8_t *splats_out, uint32_t *num_active_out,
                           uint32_t w, uint32_t h, uint8_t ch) {
    (void)ctx;(void)splat_pos;(void)num_splats;(void)source;(void)mask;
    (void)gradient;(void)splats_out;(void)num_active_out;(void)w;(void)h;(void)ch;
    return -1;
}

int qtsq_metal_splat_color_sample(qtsq_metal_ctx_t *ctx,
                                   const uint8_t *splat_packed, uint32_t num_splats,
                                   const uint8_t *source, uint8_t *colors_out,
                                   uint32_t w, uint32_t h, uint8_t ch) {
    (void)ctx;(void)splat_packed;(void)num_splats;
    (void)source;(void)colors_out;(void)w;(void)h;(void)ch;
    return -1;
}

int qtsq_metal_render_splats_fast(qtsq_metal_ctx_t *ctx,
                                   const splat2d_t *splats, uint32_t count,
                                   uint32_t w, uint32_t h, uint8_t ch,
                                   uint8_t *output) {
    (void)ctx;(void)splats;(void)count;(void)w;(void)h;(void)ch;(void)output;
    return -1;
}

int qtsq_metal_render_hybrid(qtsq_metal_ctx_t *ctx,
                              const tri_vertex_t *verts, uint32_t num_verts,
                              const tri_face_t *faces, uint32_t num_faces,
                              const splat2d_t *splats, uint32_t num_splats,
                              uint32_t w, uint32_t h, uint8_t ch,
                              uint8_t *output) {
    (void)ctx;(void)verts;(void)num_verts;(void)faces;(void)num_faces;
    (void)splats;(void)num_splats;(void)w;(void)h;(void)ch;(void)output;
    return -1;
}
