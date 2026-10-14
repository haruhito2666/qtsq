#ifndef QTSQ_MULTI_FRAME_H
#define QTSQ_MULTI_FRAME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t max_frames;
    uint32_t frame_count;
    
    /* Ring buffer of RGB888 frames (w * h * 3 bytes each) */
    uint8_t **rgb_buffers;
    
    int is_processing;
} qtsq_mf_context_t;

/* Initialize the multi-frame context */
qtsq_mf_context_t* qtsq_mf_init(uint32_t width, uint32_t height, uint32_t max_frames);

/* Push a single RGB888 frame to the ring buffer (w*h*3 bytes) */
void qtsq_mf_push_frame(qtsq_mf_context_t *ctx, const uint8_t *rgb_data);

/* Process all buffered frames (alignment + temporal merge).
 * Returns a newly allocated RGB888 buffer of size (width * height * 3).
 * Returns NULL on failure or if 0 frames acquired. */
uint8_t* qtsq_mf_process_and_merge(qtsq_mf_context_t *ctx);

/* Clean up context */
void qtsq_mf_free(qtsq_mf_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* QTSQ_MULTI_FRAME_H */
