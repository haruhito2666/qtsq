/* demo_universal.c — Universal auto-detect compression demo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qtsq.h"
#include "../tests/test_util.h"

int main(int argc, char **argv) {
    printf("=== QTSQ Demo: Universal Auto-Detect ===\n\n");

    if (argc < 2) {
        printf("Usage: %s <file>\n", argv[0]);
        printf("  Compresses any file using auto-detected optimal strategy.\n\n");

        /* Run with built-in demo data */
        printf("Running with built-in demo data...\n\n");

        qtsq_context_t ctx;
        qtsq_init(&ctx);

        /* Test 1: Raw data */
        {
            uint8_t data[512];
            for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i * 3);

            uint8_t detected = qtsq_detect_type(data, 512);
            printf("Raw bytes   → detected: %s (0x%02X)\n", qtsq_type_name(detected), detected);

            qtsq_compress_raw(&ctx, data, 512, 0);
            printf("  Compressed: %u bytes (ratio %.1f:1)\n\n",
                   ctx.singularity_size, qtsq_compression_ratio(&ctx));
            qtsq_free(&ctx);
        }

        /* Test 2: Text */
        {
            qtsq_init(&ctx);
            const char *text = "Hello, world! This is a text document for testing.";
            uint8_t detected = qtsq_detect_type((const uint8_t *)text, strlen(text));
            printf("Text string → detected: %s (0x%02X)\n", qtsq_type_name(detected), detected);

            qtsq_compress_text(&ctx, text, strlen(text));
            printf("  Compressed: %u bytes (ratio %.1f:1)\n\n",
                   ctx.singularity_size, qtsq_compression_ratio(&ctx));
            qtsq_free(&ctx);
        }

        /* Test 3: JSON */
        {
            qtsq_init(&ctx);
            const char *json = "{\"name\":\"Alice\",\"age\":30,\"city\":\"NYC\"}";
            uint8_t detected = qtsq_detect_type((const uint8_t *)json, strlen(json));
            printf("JSON data   → detected: %s (0x%02X)\n", qtsq_type_name(detected), detected);

            qtsq_compress_text(&ctx, json, strlen(json));
            printf("  Compressed: %u bytes (ratio %.1f:1)\n\n",
                   ctx.singularity_size, qtsq_compression_ratio(&ctx));
            qtsq_free(&ctx);
        }

        /* Test 4: CSV */
        {
            qtsq_init(&ctx);
            const char *csv = "name,age\nAlice,30\nBob,25\n";
            uint8_t detected = qtsq_detect_type((const uint8_t *)csv, strlen(csv));
            printf("CSV data    → detected: %s (0x%02X)\n", qtsq_type_name(detected), detected);

            qtsq_compress_csv(&ctx, csv, strlen(csv));
            printf("  Compressed: %u bytes (ratio %.1f:1)\n",
                   ctx.singularity_size, qtsq_compression_ratio(&ctx));
            qtsq_free(&ctx);
        }

        return 0;
    }

    /* File mode: read and compress any file */
    const char *filename = argv[1];
    FILE *fp = fopen(filename, "rb");
    if (!fp) { printf("Cannot open: %s\n", filename); return 1; }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(file_size);
    fread(data, 1, file_size, fp);
    fclose(fp);

    printf("File: %s (%ld bytes)\n", filename, file_size);

    /* Auto-detect type */
    uint8_t detected = qtsq_detect_type(data, file_size);
    printf("Detected type: %s (0x%02X)\n\n", qtsq_type_name(detected), detected);

    /* Compress with appropriate engine */
    qtsq_context_t ctx;
    qtsq_init(&ctx);

    int r;
    if (qtsq_type_is_text(detected)) {
        r = qtsq_compress_text(&ctx, (const char *)data, file_size);
    } else {
        r = qtsq_compress_raw(&ctx, data, file_size, 0);
    }

    if (r != QTSQ_OK) {
        printf("ERROR: %s\n", qtsq_error_string(r));
        free(data); return 1;
    }

    qtsq_print_info(&ctx);

    /* Save (Phase 1: encrypt before write) */
    char outname[512];
    snprintf(outname, sizeof(outname), "%s.qtsq", filename);
    r = qtsq_test_encrypt_for_write(&ctx);
    if (r != QTSQ_OK) {
        printf("ERROR encrypt: %s\n", qtsq_error_string(r));
        free(data); qtsq_free(&ctx); return 1;
    }
    r = qtsq_write(&ctx, outname);
    printf("\nSaved: %s (%u bytes)\n", outname, ctx.singularity_size);

    free(data);
    qtsq_free(&ctx);
    return 0;
}
