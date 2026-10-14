/* demo_audio.c — Audio Fourier compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Audio (Lensing Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Generate a 1-second 44100Hz audio: 440Hz + 880Hz harmonics */
    uint32_t sample_rate = 44100;
    size_t num_samples = 44100;
    int16_t *samples = (int16_t *)malloc(num_samples * sizeof(int16_t));

    for (size_t i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        double signal = 0.6 * sin(2.0 * 3.14159 * 440.0 * t)
                      + 0.3 * sin(2.0 * 3.14159 * 880.0 * t)
                      + 0.1 * sin(2.0 * 3.14159 * 1320.0 * t);
        samples[i] = (int16_t)(signal * 16000.0);
    }

    printf("Original: %zu samples, %zu bytes (1 second @ 44100Hz)\n",
           num_samples, num_samples * 2);
    printf("Content: 440Hz + 880Hz + 1320Hz harmonics\n\n");

    int r = qtsq_compress_audio(&ctx, samples, num_samples, sample_rate, 1);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    /* Decompress */
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_lensing(&ctx, &out, &out_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    /* Compute correlation */
    int16_t *out_samples = (int16_t *)out;
    size_t out_count = out_size / sizeof(int16_t);
    double corr_num = 0, corr_a = 0, corr_b = 0;
    for (size_t i = 0; i < num_samples && i < out_count; i++) {
        corr_num += (double)samples[i] * out_samples[i];
        corr_a += (double)samples[i] * samples[i];
        corr_b += (double)out_samples[i] * out_samples[i];
    }
    double correlation = (corr_a > 0 && corr_b > 0)
        ? corr_num / sqrt(corr_a * corr_b) : 0.0;

    printf("\nRecovered: %zu samples\n", out_count);
    printf("Correlation: %.4f (1.0 = perfect)\n", correlation);
    printf("Quality: %s\n", correlation > 0.5 ? "✅ Good" : "⚠️ Lossy");

    free(out);
    free(samples);
    qtsq_free(&ctx);
    return 0;
}
