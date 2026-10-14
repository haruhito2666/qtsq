/*
 * Quantum Tensor Sequence — Parked video-encoder stubs.
 *
 * PRODUCT.md §7: Video / 4DGS is parked (not deleted), excluded from all
 * current work. src/qtsq_video_encode.c does not currently compile
 * (pre-existing break: implicit px_compress_ex, batch_job_t misuse,
 * undeclared px_res_batch_thread) and is excluded from the build until
 * video is unparked. These stubs satisfy the linker so the library,
 * CLI, and Python extension build green; any call returns
 * QTSQ_ERR_STRATEGY ("parked"). The decoder (qtsq_video_decode.c) still
 * builds, so old files remain readable. The encoder source itself is
 * left untouched.
 */

#include <stddef.h>
#include <stdint.h>
#include "qtsq.h"

qtsq_video_encoder_t* qtsq_video_encoder_create(qtsq_context_t *ctx,
    uint32_t width, uint32_t height, uint8_t channels, uint32_t fps,
    int strategy_flags, size_t target_payload_per_frame) {
    (void)ctx; (void)width; (void)height; (void)channels; (void)fps;
    (void)strategy_flags; (void)target_payload_per_frame;
    return NULL;
}
int qtsq_video_encoder_add_frame(qtsq_video_encoder_t *enc, const uint8_t *pixels) {
    (void)enc; (void)pixels;
    return QTSQ_ERR_STRATEGY;
}
int qtsq_video_encoder_set_audio(qtsq_video_encoder_t *enc,
    const int16_t *pcm, uint32_t total_samples, uint8_t channels, uint32_t sample_rate) {
    (void)enc; (void)pcm; (void)total_samples; (void)channels; (void)sample_rate;
    return QTSQ_ERR_STRATEGY;
}
int qtsq_video_encoder_finish(qtsq_video_encoder_t *enc) {
    (void)enc;
    return QTSQ_ERR_STRATEGY;
}
qtsq_context_t* qtsq_video_encoder_get_context(qtsq_video_encoder_t *enc) {
    (void)enc;
    return NULL;
}
void qtsq_video_encoder_free(qtsq_video_encoder_t *enc) {
    (void)enc;
}
