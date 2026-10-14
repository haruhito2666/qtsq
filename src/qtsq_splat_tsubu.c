/*
 * QTSQ 4D Atom (Tsubu) Splat Engine — Implementation
 * World-space 4D Gaussian splats: pack/unpack + container I/O
 * Author: Haruhito (Tsubu Engine extension)
 */

#include "qtsq_splat_tsubu.h"
#include "qtsq.h"
#include "qtsq_keystore.h"
#include "../crypto/common/randombytes.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

/* ══════════════════════════════════════════════════════════════
 *  Pack / Unpack — Binary Format
 *
 *  Payload layout: [u32 atom_count][splat_tsubu_t × atom_count]
 *  = 4 + 108·N bytes uncompressed.
 * ══════════════════════════════════════════════════════════════ */

int tsubu_pack(const splat_tsubu_t *atoms, uint32_t count,
               uint8_t **out_data, size_t *out_size) {
    if (!atoms || !out_data || !out_size || count == 0) return -1;

    size_t total = 4 + (size_t)count * sizeof(splat_tsubu_t);
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    memcpy(buf, &count, 4);
    memcpy(buf + 4, atoms, (size_t)count * sizeof(splat_tsubu_t));

    *out_data = buf;
    *out_size = total;
    return 0;
}

int tsubu_unpack(const uint8_t *data, size_t size,
                 splat_tsubu_t **out_atoms, uint32_t *out_count) {
    if (!data || !out_atoms || !out_count || size < 4) return -1;

    uint32_t count;
    memcpy(&count, data, 4);

    size_t expected = 4 + (size_t)count * sizeof(splat_tsubu_t);
    if (size < expected) return -1;

    splat_tsubu_t *atoms = (splat_tsubu_t *)malloc((size_t)count * sizeof(splat_tsubu_t));
    if (!atoms) return -1;

    memcpy(atoms, data + 4, (size_t)count * sizeof(splat_tsubu_t));

    *out_atoms = atoms;
    *out_count = count;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Zlib compress / decompress helpers
 * ══════════════════════════════════════════════════════════════ */

static int zlib_compress(const uint8_t *input, size_t input_size,
                          uint8_t **out_compressed, size_t *out_compressed_size) {
    uLongf bound = compressBound((uLong)input_size);
    uint8_t *buf = (uint8_t *)malloc(bound);
    if (!buf) return -1;

    if (compress2(buf, &bound, input, (uLong)input_size, Z_BEST_COMPRESSION) != Z_OK) {
        free(buf);
        return -1;
    }

    *out_compressed = buf;
    *out_compressed_size = (size_t)bound;
    return 0;
}

static int zlib_decompress(const uint8_t *input, size_t input_size,
                            uint8_t **out_decompressed, size_t *out_decompressed_size,
                            size_t known_original_size) {
    uLongf dest_len = (uLongf)known_original_size;
    uint8_t *buf = (uint8_t *)malloc(dest_len);
    if (!buf) return -1;

    if (uncompress(buf, &dest_len, input, (uLong)input_size) != Z_OK) {
        free(buf);
        return -1;
    }

    *out_decompressed = buf;
    *out_decompressed_size = (size_t)dest_len;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Internal: build a sub-context with raw uncompressed data
 * ══════════════════════════════════════════════════════════════ */

static int make_sub_context(qtsq_context_t *sub,
                             uint8_t data_type, uint8_t strategy,
                             uint16_t flags, uint64_t original_size,
                             const uint8_t *data, size_t data_size) {
    qtsq_init(sub);
    sub->header.version = QTSQ_VERSION_3;
    sub->header.strategy = strategy;
    sub->header.data_type = data_type;
    sub->header.flags = flags;
    sub->header.original_size = original_size;

    sub->singularity = (uint8_t *)malloc(data_size);
    if (!sub->singularity) return QTSQ_ERR_ALLOC;
    memcpy(sub->singularity, data, data_size);
    sub->singularity_size = data_size;
    sub->header.singularity_size = data_size;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  tsubu_container_write — Build and write a .tsubu (.qtsq v3) file
 * ══════════════════════════════════════════════════════════════ */

int tsubu_container_write(const char *filename,
                           const uint8_t *asset_id,
                           const splat_tsubu_t *const *lod_atoms,
                           const uint32_t *lod_counts,
                           uint32_t num_lods,
                           const void *bonds,
                           uint32_t bond_count,
                           const void *materials,
                           uint16_t material_count) {
    if (!filename || !lod_atoms || !lod_counts || num_lods == 0) return -1;

    qtsq_context_t container;
    int result = qtsq_container_create(&container);
    if (result != QTSQ_OK) return result;

    /* ── Stream 0: asset_id (16 raw bytes, no compression) ── */
    uint8_t aid[16];
    if (asset_id) {
        memcpy(aid, asset_id, 16);
    } else {
        /* Generate random UUID v4 (M15 fix: was srand(time)+rand() —
         * predictable to the second, not thread-safe. Now uses CSPRNG.) */
        randombytes(aid, 16);
        aid[6] = (aid[6] & 0x0F) | 0x40; /* version 4 */
        aid[8] = (aid[8] & 0x3F) | 0x80; /* variant 1 */
    }

    qtsq_context_t asset_ctx;
    result = make_sub_context(&asset_ctx,
                               QTSQ_TYPE_RAW, QTSQ_STRATEGY_NONE,
                               0, 16, aid, 16);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }
    result = qtsq_container_add_stream(&container, &asset_ctx, "asset_id");
    qtsq_free(&asset_ctx);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    /* ── Streams 1..num_lods: lod0, lod1, ... (zlib-compressed atoms) ── */
    for (uint32_t lod = 0; lod < num_lods; lod++) {
        /* Pack atoms into raw binary */
        uint8_t *packed = NULL;
        size_t packed_size = 0;
        if (tsubu_pack(lod_atoms[lod], lod_counts[lod], &packed, &packed_size) != 0) {
            qtsq_free(&container); return -1;
        }

        /* Zlib compress the packed data */
        uint8_t *compressed = NULL;
        size_t compressed_size = 0;
        int zr = zlib_compress(packed, packed_size, &compressed, &compressed_size);
        free(packed);
        if (zr != 0) { qtsq_free(&container); return -1; }

        char stream_name[32];
        snprintf(stream_name, sizeof(stream_name), "lod%u", lod);

        qtsq_context_t lod_ctx;
        result = make_sub_context(&lod_ctx,
                                   QTSQ_TYPE_ATOM, QTSQ_STRATEGY_TSUBU,
                                   QTSQ_FLAG_SPLAT_ONLY | QTSQ_FLAG_ZLIB,
                                   (uint64_t)packed_size,
                                   compressed, compressed_size);
        free(compressed);
        if (result != QTSQ_OK) { qtsq_free(&container); return result; }

        result = qtsq_container_add_stream(&container, &lod_ctx, stream_name);
        qtsq_free(&lod_ctx);
        if (result != QTSQ_OK) { qtsq_free(&container); return result; }
    }

    /* ── Tail stream: bonds_materials (if provided) ── */
    if (bonds && bond_count > 0 && materials && material_count > 0) {
        /* Estimate bond struct size: assumes 20-byte bonds and 48-byte materials
         * (Tsubu Engine's Bond and TsubuMaterial structs).
         * We store the raw bond/materials data as a contiguous blob. */
        size_t bond_bytes = (size_t)bond_count * 20;
        size_t mat_bytes = (size_t)material_count * 48;
        size_t tail_size = 4 + bond_bytes + 2 + mat_bytes;

        uint8_t *tail = (uint8_t *)malloc(tail_size);
        if (!tail) { qtsq_free(&container); return -1; }
        size_t pos = 0;
        memcpy(tail + pos, &bond_count, 4); pos += 4;
        memcpy(tail + pos, bonds, bond_bytes); pos += bond_bytes;
        memcpy(tail + pos, &material_count, 2); pos += 2;
        memcpy(tail + pos, materials, mat_bytes); pos += mat_bytes;

        uint8_t *compressed = NULL;
        size_t compressed_size = 0;
        int zr = zlib_compress(tail, tail_size, &compressed, &compressed_size);
        free(tail);
        if (zr != 0) { qtsq_free(&container); return -1; }

        qtsq_context_t tail_ctx;
        result = make_sub_context(&tail_ctx,
                                   QTSQ_TYPE_RAW, QTSQ_STRATEGY_TSUBU,
                                   QTSQ_FLAG_ZLIB,
                                   (uint64_t)tail_size,
                                   compressed, compressed_size);
        free(compressed);
        if (result != QTSQ_OK) { qtsq_free(&container); return result; }

        result = qtsq_container_add_stream(&container, &tail_ctx, "bonds_materials");
        qtsq_free(&tail_ctx);
        if (result != QTSQ_OK) { qtsq_free(&container); return result; }
    }

    /* ── Pack container ── */
    result = qtsq_container_pack(&container);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    /* ── Set TSUBU metadata ── */
    container.header.flags |= QTSQ_FLAG_TSUBU_ATOMS;

    memset(&container.schema, 0, sizeof(container.schema));
    strncpy(container.schema.name, "TSUBU", 63);
    container.schema.data_type = QTSQ_TYPE_CONTAINER;
    container.schema.dimensions[0] = (num_lods > 0) ? lod_counts[0] : 0;
    container.schema.dimensions[1] = num_lods;
    container.schema.dimensions[2] = bond_count;
    container.schema.dimensions[3] = material_count;
    container.schema.num_dims = 4;
    snprintf(container.schema.encoding, sizeof(container.schema.encoding),
             "tsubu-v1;codec=zlib");
    container.header.horizon_size = sizeof(qtsq_schema_t);

    /* Phase 1: fail-closed — encrypt before writing. qtsq_container_pack
     * encrypts stream names (Phase 5); the vault preserves the salt. */
    container.is_sanitized = 1;
    container.sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ENCRYPTED;
    if (qtsq_keystore_is_initialized()) {
        result = qtsq_keystore_encrypt(&container);
    } else {
        result = QTSQ_ERR_NOT_ENCRYPTED;
    }
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    /* ── Write to file ── */
    result = qtsq_write(&container, filename);
    qtsq_free(&container);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 *  tsubu_container_read — Read a .tsubu file, extract LOD 0 atoms
 * ══════════════════════════════════════════════════════════════ */

int tsubu_container_read(const char *filename,
                          splat_tsubu_t **out_atoms,
                          uint32_t *out_count,
                          uint8_t *out_asset_id) {
    if (!filename || !out_atoms || !out_count) return -1;

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    int result = qtsq_read(&ctx, filename);
    if (result != QTSQ_OK) { qtsq_free(&ctx); return result; }

    /* Fail-closed round-trip: tsubu_container_write keystore-encrypts the
     * container, so decrypt in place before the stream parser sees the
     * singularity. Without this, an encrypted .tsubu unpacks ciphertext
     * as atom data → corruption. */
    if (ctx.is_encrypted) {
        if (!qtsq_keystore_is_initialized()) { qtsq_free(&ctx); return QTSQ_ERR_ENCRYPTED; }
        result = qtsq_keystore_decrypt(&ctx);
        if (result != QTSQ_OK) { qtsq_free(&ctx); return result; }
    }

    if (!(ctx.header.flags & QTSQ_FLAG_TSUBU_ATOMS)) {
        qtsq_free(&ctx);
        return -1; /* not a Tsubu file */
    }

    uint32_t num_streams = 0;
    result = qtsq_container_get_count(&ctx, &num_streams);
    if (result != QTSQ_OK || num_streams < 2) { qtsq_free(&ctx); return -1; }

    /* Extract asset_id from stream 0 */
    if (out_asset_id) {
        qtsq_context_t stream0;
        result = qtsq_container_get_stream(&ctx, 0, &stream0, NULL, 0);
        if (result == QTSQ_OK && stream0.singularity_size >= 16) {
            memcpy(out_asset_id, stream0.singularity, 16);
        }
        qtsq_free(&stream0);
    }

    /* Find stream named "lod0" */
    splat_tsubu_t *atoms = NULL;
    uint32_t atom_count = 0;

    for (uint32_t i = 0; i < num_streams; i++) {
        char name[256];
        qtsq_context_t stream;
        result = qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name));
        if (result != QTSQ_OK) continue;

        if (strcmp(name, "lod0") == 0 &&
            stream.header.strategy == QTSQ_STRATEGY_TSUBU) {

            uint8_t *raw = NULL;
            size_t raw_size = 0;

            if (stream.header.flags & QTSQ_FLAG_ZLIB) {
                /* Decompress zlib payload */
                result = zlib_decompress(stream.singularity, stream.singularity_size,
                                          &raw, &raw_size,
                                          (size_t)stream.header.original_size);
            } else {
                /* Raw data — copy */
                raw_size = stream.singularity_size;
                raw = (uint8_t *)malloc(raw_size);
                if (raw) memcpy(raw, stream.singularity, raw_size);
            }

            if (raw) {
                tsubu_unpack(raw, raw_size, &atoms, &atom_count);
                free(raw);
            }

            qtsq_free(&stream);
            break;
        }
        qtsq_free(&stream);
    }

    qtsq_free(&ctx);

    if (!atoms) return -1;
    *out_atoms = atoms;
    *out_count = atom_count;
    return 0;
}
