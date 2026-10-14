/* demo_text.c — Dictionary compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Text (Horizon Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    const char *text =
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. "
        "A stitch in time saves nine. "
        "A stitch in time saves nine. "
        "All that glitters is not gold. "
        "All that glitters is not gold. ";
    size_t len = strlen(text);

    printf("Original: %zu bytes\n", len);
    printf("Text: \"%.*s...\"\n\n", 60, text);

    int r = qtsq_compress_text(&ctx, text, len);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    /* Decompress and verify */
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_horizon(&ctx, &out, &out_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("\nDecompressed: %zu bytes\n", out_size);
    printf("Match: %s\n", memcmp(text, out, len) == 0 ? "✅ EXACT" : "❌ MISMATCH");

    free(out);
    qtsq_free(&ctx);
    return 0;
}
