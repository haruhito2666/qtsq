/* Shared motion-vector predictor for the pixel video path.
 *
 * The encoder codes each block's vector as the delta from this prediction and
 * the decoder adds it back, so both sides must compute it identically — hence
 * one inline definition used by qtsq_video_encode.c and qtsq_video_decode.c.
 *
 * Neighbours: A = left, B = top, C = top-right. Skipped blocks take part with
 * their zero vector, so a static or panning region collapses to a stream of
 * near-zero deltas instead of repeating the same vector per block. That is
 * what makes the modes/MV side of a P-frame small: at 1080p a 16x16 grid is
 * ~8k blocks, and two raw bytes each is more than a whole frame's byte budget
 * for a low-bitrate source. */
#ifndef QTSQ_VIDEO_MV_H
#define QTSQ_VIDEO_MV_H

#include <stdint.h>

static inline void qtsq_mv_predict(const int16_t *mv, uint32_t mb_w,
                                   uint32_t mx, uint32_t my,
                                   int *out_x, int *out_y) {
    int ax[3], ay[3];
    int n = 0;
    if (mx > 0) {
        size_t i = ((size_t)my * mb_w + (mx - 1)) * 2;
        ax[n] = mv[i]; ay[n] = mv[i + 1]; n++;
    }
    if (my > 0) {
        size_t i = ((size_t)(my - 1) * mb_w + mx) * 2;
        ax[n] = mv[i]; ay[n] = mv[i + 1]; n++;
    }
    if (my > 0 && mx + 1 < mb_w) {
        size_t i = ((size_t)(my - 1) * mb_w + (mx + 1)) * 2;
        ax[n] = mv[i]; ay[n] = mv[i + 1]; n++;
    }
    if (n == 0) { *out_x = 0; *out_y = 0; return; }
    if (n == 1) { *out_x = ax[0]; *out_y = ay[0]; return; }
    if (n == 2) {
        *out_x = (ax[0] + ax[1]) / 2;
        *out_y = (ay[0] + ay[1]) / 2;
        return;
    }
    int lo = ax[0], hi = ax[0];
    if (ax[1] < lo) lo = ax[1];
    if (ax[2] < lo) lo = ax[2];
    if (ax[1] > hi) hi = ax[1];
    if (ax[2] > hi) hi = ax[2];
    *out_x = ax[0] + ax[1] + ax[2] - lo - hi;
    lo = ay[0]; hi = ay[0];
    if (ay[1] < lo) lo = ay[1];
    if (ay[2] < lo) lo = ay[2];
    if (ay[1] > hi) hi = ay[1];
    if (ay[2] > hi) hi = ay[2];
    *out_y = ay[0] + ay[1] + ay[2] - lo - hi;
}

#endif /* QTSQ_VIDEO_MV_H */
