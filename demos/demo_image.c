/* demo_image.c — Image fractal compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Image (Accretion Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Generate a 64x64 grayscale test image: circular gradient */
    uint32_t w = 64, h = 64;
    uint8_t *pixels = (uint8_t *)malloc(w * h);

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            double dx = (double)x - 32.0;
            double dy = (double)y - 32.0;
            double dist = sqrt(dx*dx + dy*dy);
            pixels[y * w + x] = (uint8_t)(255.0 * (1.0 - dist / 45.0));
        }
    }

    printf("Original: %ux%u grayscale = %u bytes\n\n", w, h, w * h);

    int r = qtsq_compress_image(&ctx, pixels, w, h, 1);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    /* Decompress */
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_accretion(&ctx, &out, &out_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    /* Compute PSNR */
    double mse = 0;
    for (size_t i = 0; i < w * h && i < out_size; i++) {
        double d = (double)pixels[i] - (double)out[i];
        mse += d * d;
    }
    mse /= (w * h);
    double psnr = (mse > 0) ? 10.0 * log10(255.0 * 255.0 / mse) : 100.0;

    printf("\nRecovered: %zu bytes\n", out_size);
    printf("PSNR: %.1f dB  (MSE: %.1f)\n", psnr, mse);
    printf("Quality: %s\n", psnr > 15.0 ? "✅ Good" : "⚠️ Lossy");

    free(out);
    free(pixels);
    qtsq_free(&ctx);
    return 0;
}
