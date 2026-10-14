/* demo_video.c — Video temporal+fractal compression demo */
#include <stdio.h>
#include <stdlib.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Video (Accretion Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Generate 4 frames of 32x32 grayscale "video" */
    uint32_t w = 32, h = 32, nf = 4;
    size_t frame_size = w * h;
    uint8_t *frames = (uint8_t *)malloc(frame_size * nf);

    for (uint32_t f = 0; f < nf; f++) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                /* Moving gradient per frame */
                frames[f * frame_size + y * w + x] =
                    (uint8_t)((x + y + f * 8) * 4);
            }
        }
    }

    printf("Original: %u frames of %ux%u = %zu bytes\n\n",
           nf, w, h, frame_size * nf);

    int r = qtsq_compress_video(&ctx, frames, w, h, nf, 1);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    printf("\nKeyframe compressed: %u bytes (ratio %.1f:1)\n",
           ctx.singularity_size, qtsq_compression_ratio(&ctx));

    free(frames);
    qtsq_free(&ctx);
    return 0;
}
