/* demo_recovery.c — ECC corruption recovery demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Recovery (Information Paradox) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    /* Compress some data */
    uint8_t data[2048];
    for (int i = 0; i < 2048; i++) data[i] = (uint8_t)(i % 251);

    qtsq_compress_raw(&ctx, data, 2048, 1234);
    printf("Compressed: %u bytes\n", ctx.singularity_size);

    /* Add ECC recovery data */
    int r = qtsq_add_recovery(&ctx);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("ECC added: %u chunks, %u bytes parity\n\n",
           ctx.recovery.num_chunks, ctx.recovery.parity_size);

    /* Health check — clean */
    r = qtsq_health_check(&ctx);
    printf("Health check (clean):   %s\n",
           r == QTSQ_OK ? "✅ HEALTHY" : "❌ CORRUPT");

    /* Simulate corruption: flip bits in singularity */
    printf("\n🔨 Corrupting byte at offset 20...\n");
    ctx.singularity[20] ^= 0xFF;

    r = qtsq_health_check(&ctx);
    printf("Health check (corrupt): %s\n",
           r == QTSQ_ERR_CORRUPT ? "⚠️ CORRUPTION DETECTED" : "unexpected");

    /* Recover */
    r = qtsq_recover(&ctx);
    printf("Recovery attempt:       %s\n",
           r == QTSQ_OK ? "✅ RECOVERED" : "❌ FAILED");

    /* Verify recovery */
    r = qtsq_health_check(&ctx);
    printf("Health check (fixed):   %s\n",
           r == QTSQ_OK ? "✅ HEALTHY" : "❌ STILL CORRUPT");

    /* Full roundtrip */
    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_singularity(&ctx, &out, &out_size);
    if (r == QTSQ_OK && out_size == 2048) {
        printf("\nData after recovery: %s\n",
               memcmp(data, out, 2048) == 0 ? "✅ EXACT MATCH" : "❌ MISMATCH");
    }

    free(out);
    qtsq_free(&ctx);
    return 0;
}
