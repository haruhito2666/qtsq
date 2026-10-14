/*
 * Quantum Tensor Sequence — Gravitational Waves Engine (Diff/Patch)
 * Feature: Generate compact diffs between two .qtsq files (.qtsw format)
 * Apply patches to update a .qtsq file without full recompression
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "qtsq.h"

/* ── Wave magic bytes: "QTW\x01" ── */
#define QTW_MAGIC_0 'Q'
#define QTW_MAGIC_1 'T'
#define QTW_MAGIC_2 'W'
#define QTW_MAGIC_3 '\x01'

/* ══════════════════════════════════════════════════════════════
 *  qtsq_emit_wave — Generate a diff between old and new contexts
 * ══════════════════════════════════════════════════════════════
 *
 * Compares the singularity buffers of two contexts and generates
 * a compact patch file (.qtsw). Only stores the changed bytes.
 *
 * Wave file format:
 *   [qtsq_wave_header_t]  — 76 bytes
 *   For each patch:
 *     [4B offset] [4B old_size] [4B new_size] [new_size bytes data]
 */

int qtsq_emit_wave(const qtsq_context_t *old_ctx,
                  const qtsq_context_t *new_ctx,
                  const char *wave_filename) {
    if (!old_ctx || !new_ctx || !wave_filename) return QTSQ_ERR_NULL;

    /* Decompress both contexts to compare raw data */
    uint8_t *old_data = NULL, *new_data = NULL;
    size_t old_size = 0, new_size = 0;

    int result = qtsq_decompress((qtsq_context_t *)old_ctx, &old_data, &old_size);
    if (result != QTSQ_OK) return result;

    result = qtsq_decompress((qtsq_context_t *)new_ctx, &new_data, &new_size);
    if (result != QTSQ_OK) { free(old_data); return result; }

    /* Find patches: scan for differing regions */
    size_t max_patches = 1024;
    qtsq_patch_t *patches = (qtsq_patch_t *)calloc(max_patches, sizeof(qtsq_patch_t));
    if (!patches) { free(old_data); free(new_data); return QTSQ_ERR_ALLOC; }

    uint32_t num_patches = 0;
    size_t min_size = old_size < new_size ? old_size : new_size;
    size_t i = 0;

    while (i < min_size) {
        /* Skip matching bytes */
        if (old_data[i] == new_data[i]) { i++; continue; }

        /* Found a difference — scan to find the end of this diff region */
        size_t diff_start = i;
        while (i < min_size && old_data[i] != new_data[i]) i++;

        /* Also include a few bytes of matching context for robustness */
        size_t diff_end = i;

        if (num_patches >= max_patches) {
            max_patches *= 2;
            qtsq_patch_t *tmp = (qtsq_patch_t *)realloc(patches,
                max_patches * sizeof(qtsq_patch_t));
            if (!tmp) { free(patches); free(old_data); free(new_data); return QTSQ_ERR_ALLOC; }
            patches = tmp;
        }

        patches[num_patches].offset = (uint32_t)diff_start;
        patches[num_patches].old_size = (uint32_t)(diff_end - diff_start);
        patches[num_patches].new_size = (uint32_t)(diff_end - diff_start);
        patches[num_patches].new_data = (uint8_t *)malloc(diff_end - diff_start);
        if (!patches[num_patches].new_data) {
            for (uint32_t p = 0; p < num_patches; p++) free(patches[p].new_data);
            free(patches); free(old_data); free(new_data);
            return QTSQ_ERR_ALLOC;
        }
        memcpy(patches[num_patches].new_data, new_data + diff_start,
               diff_end - diff_start);
        num_patches++;
    }

    /* Handle size difference as a final patch */
    if (new_size > old_size) {
        if (num_patches >= max_patches) {
            max_patches++;
            qtsq_patch_t *tmp = (qtsq_patch_t *)realloc(patches,
                max_patches * sizeof(qtsq_patch_t));
            if (!tmp) {
                for (uint32_t p = 0; p < num_patches; p++) free(patches[p].new_data);
                free(patches); free(old_data); free(new_data); return QTSQ_ERR_ALLOC;
            }
            patches = tmp;
        }
        size_t extra = new_size - old_size;
        patches[num_patches].offset = (uint32_t)old_size;
        patches[num_patches].old_size = 0;
        patches[num_patches].new_size = (uint32_t)extra;
        patches[num_patches].new_data = (uint8_t *)malloc(extra);
        if (patches[num_patches].new_data) {
            memcpy(patches[num_patches].new_data, new_data + old_size, extra);
            num_patches++;
        }
    }

    /* Build wave header */
    qtsq_wave_header_t wave_hdr;
    memset(&wave_hdr, 0, sizeof(wave_hdr));
    wave_hdr.magic[0] = QTW_MAGIC_0;
    wave_hdr.magic[1] = QTW_MAGIC_1;
    wave_hdr.magic[2] = QTW_MAGIC_2;
    wave_hdr.magic[3] = QTW_MAGIC_3;
    wave_hdr.num_patches = num_patches;
    wave_hdr.timestamp = new_ctx->header.timestamp;

    /* Compute source and result hashes */
    qtsq_sha256(old_data, old_size, wave_hdr.source_hash);
    qtsq_sha256(new_data, new_size, wave_hdr.result_hash);

    free(old_data);
    free(new_data);

    /* Write wave file */
    FILE *fp = fopen(wave_filename, "wb");
    if (!fp) {
        for (uint32_t p = 0; p < num_patches; p++) free(patches[p].new_data);
        free(patches);
        return QTSQ_ERR_IO;
    }

    if (fwrite(&wave_hdr, sizeof(wave_hdr), 1, fp) != 1) {
        fclose(fp);
        for (uint32_t p = 0; p < num_patches; p++) free(patches[p].new_data);
        free(patches);
        return QTSQ_ERR_IO;
    }

    for (uint32_t p = 0; p < num_patches; p++) {
        if (fwrite(&patches[p].offset, 4, 1, fp) != 1 ||
            fwrite(&patches[p].old_size, 4, 1, fp) != 1 ||
            fwrite(&patches[p].new_size, 4, 1, fp) != 1) {
            fclose(fp);
            for (uint32_t q = p; q < num_patches; q++) free(patches[q].new_data);
            free(patches);
            return QTSQ_ERR_IO;
        }
        if (patches[p].new_size > 0) {
            if (fwrite(patches[p].new_data, patches[p].new_size, 1, fp) != 1) {
                fclose(fp);
                for (uint32_t q = p; q < num_patches; q++) free(patches[q].new_data);
                free(patches);
                return QTSQ_ERR_IO;
            }
        }
        free(patches[p].new_data);
    }

    fclose(fp);
    free(patches);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_apply_wave — Apply a .qtsw patch to a context
 * ══════════════════════════════════════════════════════════════ */

int qtsq_apply_wave(qtsq_context_t *ctx, const char *wave_filename) {
    if (!ctx || !wave_filename) return QTSQ_ERR_NULL;

    FILE *fp = fopen(wave_filename, "rb");
    if (!fp) return QTSQ_ERR_IO;

    /* Read wave header */
    qtsq_wave_header_t wave_hdr;
    if (fread(&wave_hdr, sizeof(wave_hdr), 1, fp) != 1) {
        fclose(fp);
        return QTSQ_ERR_FORMAT;
    }

    /* Verify magic */
    if (wave_hdr.magic[0] != QTW_MAGIC_0 || wave_hdr.magic[1] != QTW_MAGIC_1 ||
        wave_hdr.magic[2] != QTW_MAGIC_2 || wave_hdr.magic[3] != QTW_MAGIC_3) {
        fclose(fp);
        return QTSQ_ERR_FORMAT;
    }

    /* Decompress current data */
    uint8_t *data = NULL;
    size_t data_size = 0;
    int result = qtsq_decompress(ctx, &data, &data_size);
    if (result != QTSQ_OK) { fclose(fp); return result; }

    /* Verify source hash matches */
    uint8_t actual_hash[32];
    qtsq_sha256(data, data_size, actual_hash);
    if (memcmp(actual_hash, wave_hdr.source_hash, 32) != 0) {
        free(data);
        fclose(fp);
        return QTSQ_ERR_CHECKSUM; /* data doesn't match expected source */
    }

    /* Apply patches sequentially */
    for (uint32_t p = 0; p < wave_hdr.num_patches; p++) {
        uint32_t offset, old_sz, new_sz;
        if (fread(&offset, 4, 1, fp) != 1 ||
            fread(&old_sz, 4, 1, fp) != 1 ||
            fread(&new_sz, 4, 1, fp) != 1) {
            free(data);
            fclose(fp);
            return QTSQ_ERR_FORMAT;
        }

        uint8_t *new_data_patch = NULL;
        if (new_sz > 0) {
            new_data_patch = (uint8_t *)malloc(new_sz);
            if (!new_data_patch) { free(data); fclose(fp); return QTSQ_ERR_ALLOC; }
            if (fread(new_data_patch, new_sz, 1, fp) != 1) {
                free(new_data_patch); free(data); fclose(fp);
                return QTSQ_ERR_FORMAT;
            }
        }

        /* Apply this patch */
        if (new_sz != old_sz) {
            /* Validate patch offsets before rebuilding — a crafted .bhw with
             * offset+old_sz > data_size would underflow the (data_size -
             * offset - old_sz) tail computation (size_t) and trigger a huge
             * memcpy. */
            if ((uint64_t)offset + old_sz > data_size) {
                free(new_data_patch); free(data); fclose(fp);
                return QTSQ_ERR_FORMAT;
            }
            /* Size change: rebuild buffer */
            size_t new_total = data_size - old_sz + new_sz;
            uint8_t *new_buf = (uint8_t *)malloc(new_total);
            if (!new_buf) {
                free(new_data_patch); free(data); fclose(fp);
                return QTSQ_ERR_ALLOC;
            }

            /* Copy before patch */
            if (offset > 0) memcpy(new_buf, data, offset);
            /* Copy new data */
            if (new_sz > 0) memcpy(new_buf + offset, new_data_patch, new_sz);
            /* Copy after patch */
            size_t tail = data_size - offset - old_sz;
            if (tail > 0) memcpy(new_buf + offset + new_sz,
                                  data + offset + old_sz, tail);

            free(data);
            data = new_buf;
            data_size = new_total;
        } else {
            /* Same size: replace in-place */
            if (new_sz > 0 && offset + new_sz <= data_size) {
                memcpy(data + offset, new_data_patch, new_sz);
            }
        }

        free(new_data_patch);
    }

    fclose(fp);

    /* Re-compress the patched data, preserving the original strategy.
     * Previously this discarded the strategy and always stored as raw
     * delta-seed ("For simplicity"), which broke round-trips for non-raw
     * data types. Now we select the compress function based on the
     * original data_type, falling back to raw for unknown types. */
    uint8_t dt = ctx->header.data_type;
    if (dt >= QTSQ_TYPE_TEXT_ASCII && dt <= QTSQ_TYPE_CSV) {
        result = qtsq_compress_text(ctx, (const char *)data, data_size);
    } else {
        result = qtsq_compress_raw(ctx, data, data_size, 0);
    }
    free(data);

    return result;
}
