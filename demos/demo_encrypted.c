/* demo_encrypted.c — AES-256 encryption demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: Encryption (Cosmic Censorship) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    const char *secret = "TOP SECRET: The quantum tensor sequence is complete.";
    size_t len = strlen(secret);

    printf("Original: \"%s\" (%zu bytes)\n\n", secret, len);

    qtsq_compress_text(&ctx, secret, len);
    printf("Compressed: %u bytes\n", ctx.singularity_size);

    /* Encrypt */
    int r = qtsq_encrypt(&ctx, "hunter2");
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("Encrypted: ✅ (flags: 0x%04X)\n", ctx.header.flags);
    printf("Is encrypted: %s\n\n", qtsq_is_encrypted(&ctx) ? "YES" : "NO");

    /* Try wrong password */
    r = qtsq_decrypt(&ctx, "wrongpass");
    printf("Wrong password: %s ✅\n", qtsq_error_string(r));

    /* Correct password */
    r = qtsq_decrypt(&ctx, "hunter2");
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }
    printf("Correct password: Decrypted ✅\n");

    /* Verify */
    uint8_t *out = NULL; size_t out_size = 0;
    qtsq_decompress_horizon(&ctx, &out, &out_size);
    printf("\nRecovered: \"%.*s\"\n", (int)out_size, out);
    printf("Match: %s\n", memcmp(secret, out, len) == 0 ? "✅ EXACT" : "❌ MISMATCH");

    free(out);
    qtsq_free(&ctx);
    return 0;
}
