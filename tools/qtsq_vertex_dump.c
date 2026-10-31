/*
 * qtsq_vertex_dump — Extract vertices from .qtsq file for DTCN training.
 *
 * Reads a .qtsq file and outputs raw binary vertex data (x, y, R, G, B)
 * to stdout or a specified output file.
 *
 * Output format (binary):
 *   Bytes 0-3:   uint32_t  num_vertices
 *   Bytes 4-7:   uint32_t  image_width
 *   Bytes 8-11:  uint32_t  image_height
 *   Bytes 12..:  float[num_vertices * 5]  — (x, y, R, G, B) per vertex
 *
 * Usage:
 *   ./bin/qtsq_vertex_dump input.qtsq output.vtx
 *
 * Author: Priyanshu Boruah
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "qtsq.h"
#include "qtsq_vault.h"

#define MAX_VERTICES 100000

/* Extract image dimensions from TRM3 singularity payload */
static int get_trm3_dimensions(const qtsq_context_t *ctx,
                                uint32_t *out_w, uint32_t *out_h) {
    if (!ctx || !ctx->singularity || ctx->singularity_size < 8) return -1;

    /* Decompress zlib */
    uint32_t uncomp_sz;
    memcpy(&uncomp_sz, ctx->singularity, 4);

    uint8_t *packed = (uint8_t *)malloc(uncomp_sz);
    if (!packed) return -1;

    uLongf dl = (uLongf)uncomp_sz;
    if (uncompress(packed, &dl, ctx->singularity + 4,
                   (uLong)(ctx->singularity_size - 4)) != Z_OK) {
        free(packed);
        return -1;
    }

    /* TRM3 header: magic(4) + width(4) + height(4) */
    if (packed[0] != 'T' || packed[1] != 'R' ||
        packed[2] != 'M' || packed[3] != '3') {
        free(packed);
        return -1;
    }

    memcpy(out_w, packed + 4, 4);
    memcpy(out_h, packed + 8, 4);
    free(packed);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *password = NULL;
    const char *input_path = NULL;
    const char *output_path = NULL;

    /* Parse args: support -p <password> flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (!input_path) {
            input_path = argv[i];
        } else {
            output_path = argv[i];
        }
    }

    if (!input_path || !output_path) {
        fprintf(stderr, "Usage: %s [-p password] input.qtsq output.vtx\n", argv[0]);
        return 1;
    }

    /* Initialize context and read .qtsq file */
    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        fprintf(stderr, "Failed to init context\n");
        return 1;
    }

    if (qtsq_read(&ctx, input_path) != 0) {
        fprintf(stderr, "Failed to read: %s\n", input_path);
        qtsq_free(&ctx);
        return 1;
    }

    /* Decrypt if encrypted (H15 fix: previously vertex_dump couldn't open
     * any real .qtsq file since Phase-1 made them all encrypted). */
    if (ctx.is_encrypted) {
        if (!password) {
            fprintf(stderr, "File is encrypted. Use -p <password> to decrypt.\n");
            qtsq_free(&ctx);
            return 1;
        }
        if (qtsq_vault_decrypt_password(&ctx, password) != QTSQ_OK) {
            fprintf(stderr, "Decryption failed (wrong password?)\n");
            qtsq_free(&ctx);
            return 1;
        }
    }

    /* Get image dimensions from TRM3 payload */
    uint32_t width = 0, height = 0;
    if (get_trm3_dimensions(&ctx, &width, &height) != 0) {
        fprintf(stderr, "Failed to read TRM3 dimensions from: %s\n", input_path);
        qtsq_free(&ctx);
        return 1;
    }

    /* Extract vertices: (x, y, R, G, B) per vertex as floats */
    float *verts = (float *)malloc(MAX_VERTICES * 5 * sizeof(float));
    if (!verts) {
        fprintf(stderr, "Out of memory\n");
        qtsq_free(&ctx);
        return 1;
    }

    int nv = qtsq_extract_vertices(&ctx, verts, MAX_VERTICES);
    if (nv <= 0) {
        fprintf(stderr, "No vertices extracted (err=%d)\n", nv);
        free(verts);
        qtsq_free(&ctx);
        return 1;
    }

    /* Write output */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Cannot open output: %s\n", output_path);
        free(verts);
        qtsq_free(&ctx);
        return 1;
    }

    /* Header: num_vertices, width, height */
    uint32_t num = (uint32_t)nv;
    fwrite(&num, sizeof(uint32_t), 1, out);
    fwrite(&width, sizeof(uint32_t), 1, out);
    fwrite(&height, sizeof(uint32_t), 1, out);

    /* Vertex data: float[nv * 5] */
    fwrite(verts, sizeof(float), (size_t)nv * 5, out);

    fclose(out);
    fprintf(stderr, "Extracted %d vertices (%ux%u) → %s\n",
            nv, width, height, output_path);

    free(verts);
    qtsq_free(&ctx);
    return 0;
}
