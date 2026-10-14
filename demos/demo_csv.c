/* demo_csv.c — CSV dictionary compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"

int main(void) {
    printf("=== QTSQ Demo: CSV (Horizon Engine) ===\n\n");

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    const char *csv =
        "name,age,city,score\n"
        "Alice,30,New York,95.5\n"
        "Bob,25,Los Angeles,87.2\n"
        "Charlie,35,Chicago,92.1\n"
        "Alice,30,New York,95.5\n"
        "Diana,28,Houston,88.9\n"
        "Eve,32,Phoenix,91.3\n"
        "Bob,25,Los Angeles,87.2\n"
        "Frank,29,San Diego,90.0\n"
        "Alice,30,New York,95.5\n"
        "Grace,27,Dallas,93.7\n";
    size_t len = strlen(csv);

    printf("Original CSV: %zu bytes\n\n", len);

    int r = qtsq_compress_csv(&ctx, csv, len);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    qtsq_print_info(&ctx);

    uint8_t *out = NULL; size_t out_size = 0;
    r = qtsq_decompress_horizon(&ctx, &out, &out_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("\nMatch: %s\n", memcmp(csv, out, len) == 0 ? "✅ EXACT" : "❌ MISMATCH");

    free(out);
    qtsq_free(&ctx);
    return 0;
}
