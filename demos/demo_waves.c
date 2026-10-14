/* demo_waves.c — Gravitational wave diff/patch demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Gravitational Waves (Diff/Patch) ===\n\n");

    /* Create "old" version */
    qtsq_context_t old_ctx;
    qtsq_init(&old_ctx);
    const char *old_text = "Version 1: The quick brown fox jumps over the lazy dog.";
    qtsq_compress_text(&old_ctx, old_text, strlen(old_text));
    printf("Old version: \"%s\" (%zu bytes)\n", old_text, strlen(old_text));

    /* Create "new" version (slightly different) */
    qtsq_context_t new_ctx;
    qtsq_init(&new_ctx);
    const char *new_text = "Version 2: The quick brown fox leaps over the lazy cat.";
    qtsq_compress_text(&new_ctx, new_text, strlen(new_text));
    printf("New version: \"%s\" (%zu bytes)\n\n", new_text, strlen(new_text));

    /* Generate wave diff */
    int r = qtsq_emit_wave(&old_ctx, &new_ctx, "/tmp/test.qtsw");
    if (r != QTSQ_OK) {
        printf("emit_wave: %s\n", qtsq_error_string(r));
    } else {
        printf("Wave diff: ✅ Generated /tmp/test.qtsw\n");
    }

    /* Apply wave to old context */
    r = qtsq_apply_wave(&old_ctx, "/tmp/test.qtsw");
    if (r != QTSQ_OK) {
        printf("apply_wave: %s\n", qtsq_error_string(r));
    } else {
        printf("Wave apply: ✅ Patched old → new\n");

        /* Verify the patched data matches new */
        uint8_t *out = NULL; size_t out_size = 0;
        qtsq_decompress(&old_ctx, &out, &out_size);
        if (out) {
            printf("\nPatched result: \"%.*s\"\n", (int)out_size, out);
            printf("Match new text: %s\n",
                   out_size == strlen(new_text) && memcmp(new_text, out, out_size) == 0
                   ? "✅ EXACT" : "⚠️ Different (re-compressed)");
            free(out);
        }
    }

    /* Cleanup */
    remove("/tmp/test.qtsw");
    qtsq_free(&old_ctx);
    qtsq_free(&new_ctx);
    return 0;
}
