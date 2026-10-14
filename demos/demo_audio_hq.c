/* demo_audio_hq.c — Photon Audio HQ codec demo */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Photon Audio HQ (Gabor Atoms) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Generate a 2-second 44100Hz audio: 440Hz + 880Hz + 1320Hz harmonics */
    uint32_t sample_rate = 44100;
    size_t num_samples = 88200;  /* 2 seconds */
    int16_t *samples = (int16_t *)malloc(num_samples * sizeof(int16_t));

    for (size_t i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        double signal = 0.6 * sin(2.0 * 3.14159265 * 440.0 * t)
                      + 0.3 * sin(2.0 * 3.14159265 * 880.0 * t)
                      + 0.1 * sin(2.0 * 3.14159265 * 1320.0 * t);
        samples[i] = (int16_t)(signal * 16000.0);
    }

    printf("Original: %zu samples, %zu bytes (2 seconds @ 44100Hz)\n",
           num_samples, num_samples * 2);
    printf("Content: 440Hz + 880Hz + 1320Hz harmonics\n\n");

    /* Compress with Photon Audio HQ */
    printf("Encoding with Photon Audio HQ...\n");
    int r = qtsq_compress_audio_hq(&ctx, samples, num_samples, sample_rate, 1);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("Strategy: PHOTON (Gabor Atoms)\n");
    printf("Compressed: %zu bytes\n", ctx.singularity_size);
    printf("Ratio: %.1f:1\n\n", (double)(num_samples * 2) / ctx.singularity_size);

    /* Decompress */
    printf("Decoding...\n");
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_audio_hq(&ctx, &out, &out_size, NULL, NULL);
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

    /* Compute SNR */
    double noise_power = 0, signal_power = 0;
    for (size_t i = 0; i < num_samples && i < out_count; i++) {
        double diff = (double)samples[i] - (double)out_samples[i];
        noise_power += diff * diff;
        signal_power += (double)samples[i] * (double)samples[i];
    }
    double snr = (noise_power > 0) ? 10.0 * log10(signal_power / noise_power) : 999.0;

    printf("\nRecovered: %zu samples\n", out_count);
    printf("Correlation: %.6f (1.0 = perfect)\n", correlation);
    printf("SNR: %.1f dB\n", snr);
    printf("Quality: %s\n", correlation > 0.95 ? "✅ Excellent" :
                              correlation > 0.8 ? "✅ Good" :
                              correlation > 0.5 ? "⚠️ Fair" : "❌ Poor");

    free(out);
    free(samples);
    qtsq_free(&ctx);
    return 0;
}
