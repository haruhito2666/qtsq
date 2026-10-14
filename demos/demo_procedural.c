/* demo_procedural.c — Raw binary seed compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Raw Binary (Singularity Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Generate test data: repeating pattern */
    uint8_t data[4096];
    for (int i = 0; i < 4096; i++) {
        data[i] = (uint8_t)((i * 7 + 13) % 256);
    }

    printf("Original: %zu bytes\n", sizeof(data));

    int r = qtsq_compress_raw(&ctx, data, sizeof(data), 0);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    /* Decompress and verify */
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_singularity(&ctx, &out, &out_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("\nDecompressed: %zu bytes\n", out_size);
    printf("Match: %s\n", memcmp(data, out, sizeof(data)) == 0 ? "✅ EXACT (lossless)" : "❌ MISMATCH");

    free(out);
    qtsq_free(&ctx);
    return 0;
}
