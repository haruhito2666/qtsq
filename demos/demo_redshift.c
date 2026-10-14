/* demo_redshift.c — Quality scaling demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Redshift (Quality Scaling) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Compress a 64x64 image */
    uint32_t w = 64, h = 64;
    uint8_t *pixels = (uint8_t *)malloc(w * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            pixels[y * w + x] = (uint8_t)((x * 4 + y * 2) % 256);

    qtsq_compress_image(&ctx, pixels, w, h, 1);
    printf("Original: %ux%u = %u bytes\n", w, h, w * h);
    printf("Compressed: %u bytes\n\n", ctx.singularity_size);

    /* Decompress at different quality levels */
    double levels[] = {1.0, 0.75, 0.5, 0.25, 0.1};
    for (int i = 0; i < 5; i++) {
        uint8_t *out = NULL; size_t out_size = 0;
        int r = qtsq_decompress_redshift(&ctx, levels[i], &out, &out_size);
        if (r == QTSQ_OK) {
            printf("  Redshift %.2f → %5zu bytes", levels[i], out_size);
            if (levels[i] >= 0.999 && out_size == w * h)
                printf("  (full quality)");
            else
                printf("  (%.0f%% of original)", 100.0 * out_size / (w * h));
            printf("\n");
            free(out);
        }
    }

    free(pixels);
    qtsq_free(&ctx);
    return 0;
}
