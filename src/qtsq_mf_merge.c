#include "qtsq_multi_frame.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ── */

static inline int clip_i(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static inline uint8_t rgb_to_luma(const uint8_t *p) {
    return (uint8_t)((19595u * p[0] + 38470u * p[1] + 7471u * p[2]) >> 16);
}

/* ── Context lifecycle ── */

qtsq_mf_context_t* qtsq_mf_init(uint32_t width, uint32_t height, uint32_t max_frames) {
    if (width == 0 || height == 0 || max_frames == 0) return NULL;
    
    qtsq_mf_context_t *ctx = (qtsq_mf_context_t*)calloc(1, sizeof(qtsq_mf_context_t));
    if (!ctx) return NULL;
    
    ctx->width = width;
    ctx->height = height;
    ctx->max_frames = max_frames;
    ctx->frame_count = 0;
    ctx->is_processing = 0;
    
    ctx->rgb_buffers = (uint8_t**)calloc(max_frames, sizeof(uint8_t*));
    if (!ctx->rgb_buffers) { free(ctx); return NULL; }
    
    size_t frame_bytes = (size_t)width * height * 3;
    for (uint32_t i = 0; i < max_frames; i++) {
        ctx->rgb_buffers[i] = (uint8_t*)malloc(frame_bytes);
        if (!ctx->rgb_buffers[i]) {
            /* cleanup on failure */
            for (uint32_t j = 0; j < i; j++) free(ctx->rgb_buffers[j]);
            free(ctx->rgb_buffers);
            free(ctx);
            return NULL;
        }
    }
    
    return ctx;
}

void qtsq_mf_push_frame(qtsq_mf_context_t *ctx, const uint8_t *rgb_data) {
    if (!ctx || !rgb_data || ctx->is_processing) return;
    
    uint32_t idx = ctx->frame_count % ctx->max_frames;
    size_t frame_bytes = (size_t)ctx->width * ctx->height * 3;
    memcpy(ctx->rgb_buffers[idx], rgb_data, frame_bytes);
    ctx->frame_count++;
}

/* ── Alignment: Hierarchical block-matching on luma ── */

typedef struct {
    float dx, dy;
    int valid;
} frame_shift_t;

static uint32_t compute_sad_luma(const uint8_t *ref_rgb, const uint8_t *tar_rgb,
                                  uint32_t width, uint32_t height,
                                  int cx_r, int cy_r, int cx_t, int cy_t, int patch_half) {
    uint32_t sad = 0;
    for (int dy = -patch_half; dy < patch_half; dy++) {
        int ry = cy_r + dy;
        int ty = cy_t + dy;
        if (ry < 0 || ry >= (int)height || ty < 0 || ty >= (int)height) continue;
        for (int dx = -patch_half; dx < patch_half; dx++) {
            int rx = cx_r + dx;
            int tx = cx_t + dx;
            if (rx < 0 || rx >= (int)width || tx < 0 || tx >= (int)width) continue;
            
            uint8_t lr = rgb_to_luma(ref_rgb + ((uint32_t)ry * width + (uint32_t)rx) * 3);
            uint8_t lt = rgb_to_luma(tar_rgb + ((uint32_t)ty * width + (uint32_t)tx) * 3);
            sad += (uint32_t)abs((int)lr - (int)lt);
        }
    }
    return sad;
}

static void align_frame(const uint8_t *ref_rgb, const uint8_t *tar_rgb,
                         uint32_t width, uint32_t height, frame_shift_t *shift) {
    int cx = (int)width / 2;
    int cy = (int)height / 2;
    
    /* Coarse search: range ±8, step 2, 32px patch */
    int best_dx = 0, best_dy = 0;
    uint32_t min_sad = 0xFFFFFFFF;
    
    for (int dy = -8; dy <= 8; dy += 2) {
        for (int dx = -8; dx <= 8; dx += 2) {
            uint32_t sad = compute_sad_luma(ref_rgb, tar_rgb, width, height,
                                            cx, cy, cx + dx, cy + dy, 32);
            if (sad < min_sad) { min_sad = sad; best_dx = dx; best_dy = dy; }
        }
    }
    
    /* Fine search: range ±2 around best, step 1, 64px patch */
    int fine_dx = best_dx, fine_dy = best_dy;
    min_sad = 0xFFFFFFFF;
    uint32_t sads[3][3];
    memset(sads, 0xFF, sizeof(sads));
    
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            uint32_t sad = compute_sad_luma(ref_rgb, tar_rgb, width, height,
                                            cx, cy, cx + fine_dx + dx, cy + fine_dy + dy, 64);
            if (sad < min_sad) { min_sad = sad; best_dx = fine_dx + dx; best_dy = fine_dy + dy; }
            if (dy >= -1 && dy <= 1 && dx >= -1 && dx <= 1)
                sads[dy+1][dx+1] = sad;
        }
    }
    
    /* Sub-pixel parabolic refinement */
    float sub_dx = (float)best_dx;
    float sub_dy = (float)best_dy;
    
    if (best_dx == fine_dx && best_dy == fine_dy) {
        float f1 = (float)sads[1][0], f2 = (float)sads[1][1], f3 = (float)sads[1][2];
        float denom_x = f1 + f3 - 2.0f * f2;
        if (denom_x > 1.0f) sub_dx += (f1 - f3) / (2.0f * denom_x);
        
        f1 = (float)sads[0][1]; f3 = (float)sads[2][1];
        float denom_y = f1 + f3 - 2.0f * f2;
        if (denom_y > 1.0f) sub_dy += (f1 - f3) / (2.0f * denom_y);
    }
    
    shift->dx = sub_dx;
    shift->dy = sub_dy;
    /* Reject outliers with very high SAD */
    shift->valid = (min_sad < 64 * 64 * 40) ? 1 : 0;
}

/* ── Bilinear sampling from RGB buffer ── */

static inline void bilinear_sample_rgb(const uint8_t *rgb, int w, int h,
                                        float fx, float fy, float out[3]) {
    int x0 = (int)fx, y0 = (int)fy;
    float tx = fx - x0, ty = fy - y0;
    
    x0 = clip_i(x0, 0, w - 2);
    y0 = clip_i(y0, 0, h - 2);
    
    const uint8_t *p00 = rgb + ((size_t)y0 * w + x0) * 3;
    const uint8_t *p01 = p00 + 3;
    const uint8_t *p10 = p00 + (size_t)w * 3;
    const uint8_t *p11 = p10 + 3;
    
    float omtx = 1.0f - tx, omty = 1.0f - ty;
    for (int c = 0; c < 3; c++) {
        float top = p00[c] * omtx + p01[c] * tx;
        float bot = p10[c] * omtx + p11[c] * tx;
        out[c] = top * omty + bot * ty;
    }
}

/* ── Main merge ── */

uint8_t* qtsq_mf_process_and_merge(qtsq_mf_context_t *ctx) {
    if (!ctx || ctx->frame_count == 0) return NULL;
    ctx->is_processing = 1;
    
    uint32_t width = ctx->width;
    uint32_t height = ctx->height;
    uint32_t actual = ctx->frame_count > ctx->max_frames ? ctx->max_frames : ctx->frame_count;
    
    /* Reference = most recent frame */
    uint32_t ref_idx = (ctx->frame_count - 1) % ctx->max_frames;
    uint8_t *ref_rgb = ctx->rgb_buffers[ref_idx];
    
    /* Align all frames to reference */
    frame_shift_t *shifts = (frame_shift_t*)calloc(actual, sizeof(frame_shift_t));
    if (!shifts) { ctx->is_processing = 0; return NULL; }
    
    for (uint32_t i = 0; i < actual; i++) {
        if (i == ref_idx) {
            shifts[i].dx = 0; shifts[i].dy = 0; shifts[i].valid = 1;
            continue;
        }
        align_frame(ref_rgb, ctx->rgb_buffers[i], width, height, &shifts[i]);
    }
    
    /* Allocate output */
    size_t out_bytes = (size_t)width * height * 3;
    uint8_t *rgb_out = (uint8_t*)malloc(out_bytes);
    if (!rgb_out) { free(shifts); ctx->is_processing = 0; return NULL; }
    
    /* Temporal merge with bilinear-shifted averaging */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            float sum[3] = {0, 0, 0};
            int count = 0;
            
            for (uint32_t i = 0; i < actual; i++) {
                if (!shifts[i].valid) continue;
                float sx = (float)x + shifts[i].dx;
                float sy = (float)y + shifts[i].dy;
                
                float sample[3];
                bilinear_sample_rgb(ctx->rgb_buffers[i], (int)width, (int)height, sx, sy, sample);
                sum[0] += sample[0];
                sum[1] += sample[1];
                sum[2] += sample[2];
                count++;
            }
            
            size_t idx = ((size_t)y * width + x) * 3;
            if (count > 0) {
                float inv = 1.0f / count;
                rgb_out[idx + 0] = (uint8_t)clip_i((int)(sum[0] * inv + 0.5f), 0, 255);
                rgb_out[idx + 1] = (uint8_t)clip_i((int)(sum[1] * inv + 0.5f), 0, 255);
                rgb_out[idx + 2] = (uint8_t)clip_i((int)(sum[2] * inv + 0.5f), 0, 255);
            } else {
                /* fallback: copy reference */
                rgb_out[idx + 0] = ref_rgb[((size_t)y * width + x) * 3 + 0];
                rgb_out[idx + 1] = ref_rgb[((size_t)y * width + x) * 3 + 1];
                rgb_out[idx + 2] = ref_rgb[((size_t)y * width + x) * 3 + 2];
            }
        }
    }
    
    free(shifts);
    ctx->is_processing = 0;
    ctx->frame_count = 0;
    
    return rgb_out;
}

void qtsq_mf_free(qtsq_mf_context_t *ctx) {
    if (!ctx) return;
    if (ctx->rgb_buffers) {
        for (uint32_t i = 0; i < ctx->max_frames; i++) {
            free(ctx->rgb_buffers[i]);
        }
        free(ctx->rgb_buffers);
    }
    free(ctx);
}
