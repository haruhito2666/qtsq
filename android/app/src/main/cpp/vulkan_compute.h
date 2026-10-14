#ifndef QTSQ_VULKAN_COMPUTE_H
#define QTSQ_VULKAN_COMPUTE_H

#include <stdint.h>

/*
 * QTSQ Vulkan Compute — GPU-accelerated processing
 *
 * Provides GPU paths for:
 * 1. IFS decompression iteration (original)
 * 2. Barycentric triangle rasterization (gravitational decompress)
 * 3. 2D Gaussian splat rendering (gravitational decompress)
 */

/* Opaque GPU context */
typedef struct vk_compute_ctx vk_compute_ctx_t;

/* Initialize Vulkan compute context. Returns NULL if GPU not available. */
vk_compute_ctx_t *vk_compute_init(void);

/* Destroy Vulkan compute context */
void vk_compute_destroy(vk_compute_ctx_t *ctx);

/* GPU decompression parameters (IFS) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t range_size;
    uint32_t domain_size;
    uint32_t blocks_x;
    uint32_t blocks_y;
} vk_decompress_params_t;

/* Run one IFS iteration on GPU for a single channel. */
int vk_compute_decompress_iteration(
    vk_compute_ctx_t *ctx,
    const float *current,
    const void *transforms,
    float *next,
    uint32_t num_blocks,
    const vk_decompress_params_t *params
);

/* ═══════════════════════════════════════════════════════════
 *  GPU Triangle Rasterization
 *  Replaces CPU rasterize_strip for gravitational decompress.
 *  Each pixel runs as a parallel GPU thread.
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    float x, y;
    uint8_t r, g, b;
} vk_vertex_t;

typedef struct {
    uint32_t v0, v1, v2;
} vk_face_t;

/*
 * GPU-accelerate triangle rasterization.
 * verts: array of nv vertices with position + color
 * faces: array of nf triangles with vertex indices
 * output_rgb: width×height×3 byte buffer (written by GPU)
 * blur_map: width×height blur pass count (written by GPU), can be NULL
 * Returns 0 on success, -1 on failure (fall back to CPU).
 */
int vk_compute_rasterize(
    vk_compute_ctx_t *ctx,
    const vk_vertex_t *verts, uint32_t nv,
    const vk_face_t *faces, uint32_t nf,
    uint32_t width, uint32_t height,
    uint8_t *output_rgb,
    uint8_t *blur_map,
    const uint8_t *tier_mask
);

/* ═══════════════════════════════════════════════════════════
 *  GPU Splat Rendering
 *  Replaces CPU splat2d_render for gravitational decompress.
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t x, y;
    uint8_t sx, sy, theta, alpha;
    uint8_t r, g, b, pad;
    uint32_t pad2;
} vk_splat_t;

/*
 * GPU-accelerate splat rendering.
 * image_rgb: width×height×3 byte buffer (read+written by GPU)
 * fg_mask: width×height mask (1=foreground, skip), can be NULL
 * Returns 0 on success, -1 on failure (fall back to CPU).
 */
int vk_compute_render_splats(
    vk_compute_ctx_t *ctx,
    const vk_splat_t *splats, uint32_t num_splats,
    uint32_t width, uint32_t height,
    uint8_t *image_rgb,
    const uint8_t *fg_mask
);

#endif /* QTSQ_VULKAN_COMPUTE_H */
