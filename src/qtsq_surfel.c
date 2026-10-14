/*
 * QTSQ Surfel — captured 2D Gaussian splat geometry (implementation)
 *
 * Mirrors the 4DGS container module (src/qtsq_4dgs.c): pack → zlib → container
 * stream, fail-closed keystore encryption, secure write. The geometry is real
 * surfel parameters (not re-synthesized bytes), which is exactly what the
 * Authorship Seal geometry-mark (remediation 4a) perturbs.
 *
 * Author: Haruhito
 */

#include "qtsq_surfel.h"
#include "qtsq.h"
#include "qtsq_keystore.h"
#include "qtsq_tsdf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <zlib.h>

#define SURFEL_MAGIC0 'S'
#define SURFEL_MAGIC1 'R'
#define SURFEL_MAGIC2 'F'
#define SURFEL_MAGIC3 '1'

/* ══════════════════════════════════════════════════════════════
 *  Model management
 * ══════════════════════════════════════════════════════════════ */

void qtsq_surfel_model_init(qtsq_surfel_model_t *m){
    if (!m) return;
    m->surfels = NULL; m->count = 0; m->capacity = 0;
    for (int i=0;i<3;i++){ m->aabb_min[i]=0.0f; m->aabb_max[i]=0.0f; }
}

void qtsq_surfel_model_free(qtsq_surfel_model_t *m){
    if (!m) return;
    free(m->surfels);
    m->surfels = NULL; m->count = 0; m->capacity = 0;
}

static int surfel_reserve(qtsq_surfel_model_t *m, uint32_t need){
    if (need <= m->capacity) return 0;
    uint32_t nc = m->capacity ? m->capacity*2 : 64;
    if (nc < need) nc = need;
    qtsq_surfel_t *p = (qtsq_surfel_t*)realloc(m->surfels, (size_t)nc*sizeof(qtsq_surfel_t));
    if (!p) return -1;
    m->surfels = p; m->capacity = nc;
    return 0;
}

int qtsq_surfel_model_add(qtsq_surfel_model_t *m, const qtsq_surfel_t *s){
    if (!m || !s) return -1;
    if (surfel_reserve(m, m->count+1) != 0) return -1;
    m->surfels[m->count++] = *s;
    return 0;
}

int qtsq_surfel_model_append_array(qtsq_surfel_model_t *m,
                                   const qtsq_surfel_t *arr, uint32_t count){
    if (!m || !arr || count==0) return -1;
    if (surfel_reserve(m, m->count+count) != 0) return -1;
    memcpy(m->surfels + m->count, arr, (size_t)count*sizeof(qtsq_surfel_t));
    m->count += count;
    return 0;
}

void qtsq_surfel_model_recompute_aabb(qtsq_surfel_model_t *m){
    if (!m || m->count==0){ return; }
    float lo[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (uint32_t i=0;i<m->count;i++){
        for (int k=0;k<3;k++){
            float v = m->surfels[i].center[k];
            if (v<lo[k]) lo[k]=v;
            if (v>hi[k]) hi[k]=v;
        }
    }
    for (int k=0;k<3;k++){ m->aabb_min[k]=lo[k]; m->aabb_max[k]=hi[k]; }
}

/* ══════════════════════════════════════════════════════════════
 *  Binary packing
 * ══════════════════════════════════════════════════════════════ */

int qtsq_surfel_pack(const qtsq_surfel_t *surfels, uint32_t count,
                     uint8_t **out_data, size_t *out_size){
    if (!surfels || !out_data || !out_size || count==0) return -1;
    size_t total = 8 + (size_t)count*sizeof(qtsq_surfel_t);  /* 4 magic + 4 count */
    uint8_t *buf = (uint8_t*)malloc(total);
    if (!buf) return -1;
    buf[0]=SURFEL_MAGIC0; buf[1]=SURFEL_MAGIC1; buf[2]=SURFEL_MAGIC2; buf[3]=SURFEL_MAGIC3;
    memcpy(buf+4, &count, 4);
    memcpy(buf+8, surfels, (size_t)count*sizeof(qtsq_surfel_t));
    *out_data = buf; *out_size = total;
    return 0;
}

int qtsq_surfel_unpack(const uint8_t *data, size_t size,
                       qtsq_surfel_t **out_surfels, uint32_t *out_count){
    if (!data || !out_surfels || !out_count || size < 8) return -1;
    if (data[0]!=SURFEL_MAGIC0 || data[1]!=SURFEL_MAGIC1 ||
        data[2]!=SURFEL_MAGIC2 || data[3]!=SURFEL_MAGIC3) return -1;
    uint32_t count;
    memcpy(&count, data+4, 4);
    size_t expected = 8 + (size_t)count*sizeof(qtsq_surfel_t);
    if (count==0 || size < expected) return -1;
    qtsq_surfel_t *s = (qtsq_surfel_t*)malloc((size_t)count*sizeof(qtsq_surfel_t));
    if (!s) return -1;
    memcpy(s, data+8, (size_t)count*sizeof(qtsq_surfel_t));
    *out_surfels = s; *out_count = count;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Zlib helpers (local, mirror qtsq_4dgs.c)
 * ══════════════════════════════════════════════════════════════ */

static int surfel_zlib_compress(const uint8_t *in, size_t in_size,
                                uint8_t **out_c, size_t *out_cs){
    uLongf bound = compressBound((uLong)in_size);
    uint8_t *buf = (uint8_t*)malloc(bound);
    if (!buf) return -1;
    if (compress2(buf, &bound, in, (uLong)in_size, Z_BEST_COMPRESSION) != Z_OK){
        free(buf); return -1;
    }
    *out_c = buf; *out_cs = (size_t)bound;
    return 0;
}

static int surfel_zlib_decompress(const uint8_t *in, size_t in_size,
                                  uint8_t **out_d, size_t *out_ds, size_t orig){
    uLongf dest = (uLongf)orig;
    uint8_t *buf = (uint8_t*)malloc(dest ? dest : 1);
    if (!buf) return -1;
    if (uncompress(buf, &dest, in, (uLong)in_size) != Z_OK){
        free(buf); return -1;
    }
    *out_d = buf; *out_ds = (size_t)dest;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Surfel extract/apply — Phase F Authorship Seal geometry domain
 *  Mirrors qtsq_extract_splats / qtsq_apply_splats (qtsq_gravitational.c)
 *  for the surfel stream payload: a bare zlib stream decompressing to the
 *  ['S','R','F','1'][u32 count][surfels] blob. The surfel container stores
 *  the stream as bare zlib (no size prefix — unlike the splat [u32][zlib]
 *  layout), so this extractor tries bare-zlib first and tolerates a
 *  [u32 uncompressed_size]-prefixed stream as a legacy variant. Trailing
 *  bytes after the zlib stream (a seal's SEALTOKEN/share markers) are
 *  ignored by uncompress.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_extract_surfels(const qtsq_context_t *ctx, qtsq_surfel_t *out_surfels,
                         uint32_t max_surfels, uint32_t *out_count){
    if (!ctx || !out_surfels || !out_count) return QTSQ_ERR_NULL;
    if (ctx->header.data_type != QTSQ_TYPE_SURFEL) return QTSQ_ERR_FORMAT;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    uint32_t orig = ctx->header.original_size;
    if (orig == 0) return QTSQ_ERR_FORMAT;
    uint8_t *raw = (uint8_t *)malloc(orig);
    if (!raw) return QTSQ_ERR_ALLOC;
    uLongf dl = (uLongf)orig;
    int zok = (uncompress(raw, &dl, ctx->singularity,
                          (uLong)ctx->singularity_size) == Z_OK);
    if (!zok && ctx->singularity_size >= 8) {
        /* Legacy variant: [u32 uncompressed_size][zlib] (splat convention). */
        uint32_t legacy_sz;
        memcpy(&legacy_sz, ctx->singularity, 4);
        uLongf dl2 = (uLongf)legacy_sz;
        if (uncompress(raw, &dl2, ctx->singularity + 4,
                       (uLong)(ctx->singularity_size - 4)) == Z_OK) {
            dl = dl2;
            zok = 1;
        }
    }
    if (!zok){ free(raw); return QTSQ_ERR_FORMAT; }

    /* Unpacked size must match what the container recorded, otherwise the
     * payload is truncated/tampered. */
    if ((uint32_t)dl > orig){ free(raw); return QTSQ_ERR_FORMAT; }

    qtsq_surfel_t *s = NULL;
    uint32_t sc = 0;
    if (qtsq_surfel_unpack(raw, (size_t)dl, &s, &sc) != 0 || !s){
        free(raw); return QTSQ_ERR_FORMAT;
    }
    uint32_t copy_n = (sc > max_surfels) ? max_surfels : sc;
    memcpy(out_surfels, s, (size_t)copy_n * sizeof(qtsq_surfel_t));
    free(s);
    free(raw);
    *out_count = copy_n;
    return QTSQ_OK;
}

int qtsq_apply_surfels(qtsq_context_t *ctx, const qtsq_surfel_t *surfels,
                       uint32_t count){
    if (!ctx || !surfels) return QTSQ_ERR_NULL;
    if (ctx->header.data_type != QTSQ_TYPE_SURFEL) return QTSQ_ERR_FORMAT;
    if (count == 0) return QTSQ_ERR_FORMAT;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Pack, zlib-compress, and rebuild the bare-zlib singularity — the
     * layout qtsq_surfel_container_write stores in the container stream.
     * Mirrors qtsq_apply_splats: singularity + sizes are replaced; any
     * bytes trailing the original payload (SEALTOKEN etc.) are dropped —
     * callers must re-append them after a re-apply. */
    uint8_t *packed = NULL;
    size_t packed_size = 0;
    if (qtsq_surfel_pack(surfels, count, &packed, &packed_size) != 0)
        return QTSQ_ERR_ALLOC;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    int zr = surfel_zlib_compress(packed, packed_size, &compressed, &compressed_size);
    free(packed);
    if (zr != 0) return QTSQ_ERR_ALLOC;

    free(ctx->singularity);
    ctx->singularity = compressed;
    ctx->singularity_size = (uint64_t)compressed_size;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.original_size = packed_size;
    ctx->header.flags |= QTSQ_FLAG_ZLIB;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Container write — fail-closed secure path
 *  surfel_container_write_internal: author_name == NULL → unsealed
 *  (identical to the historical qtsq_surfel_container_write behavior);
 *  author_name != NULL → additionally runs the Phase F Authorship Seal
 *  geometry mark on the surfels stream before keystore encryption.
 * ══════════════════════════════════════════════════════════════ */

static int surfel_container_write_internal(const char *filename,
                                           const qtsq_surfel_model_t *model,
                                           const qtsq_mesh_t *mesh,
                                           const char *author_name,
                                           const char *license,
                                           uint32_t consent_flags){
    if (!filename || !model || model->count==0) return QTSQ_ERR_NULL;
    if (author_name && !qtsq_keystore_is_initialized())
        return QTSQ_ERR_NOT_ENCRYPTED; /* fail-closed, same as unsealed */

    qtsq_context_t container;
    int result = qtsq_container_create(&container);
    if (result != QTSQ_OK) return result;

    /* Stream 0: metadata (JSON) */
    char meta[640];
    snprintf(meta, sizeof(meta),
             "{\"count\":%u,\"aabb\":[%f,%f,%f,%f,%f,%f],\"kind\":\"surfel2dgs\",\"mesh\":%s}",
             model->count,
             model->aabb_min[0], model->aabb_min[1], model->aabb_min[2],
             model->aabb_max[0], model->aabb_max[1], model->aabb_max[2],
             mesh ? "true" : "false");
    size_t meta_len = strlen(meta);

    qtsq_context_t meta_ctx;
    qtsq_init(&meta_ctx);
    meta_ctx.header.version = QTSQ_VERSION_3;
    meta_ctx.header.strategy = QTSQ_STRATEGY_NONE;
    meta_ctx.header.data_type = QTSQ_TYPE_JSON;
    meta_ctx.singularity = (uint8_t*)malloc(meta_len);
    if (!meta_ctx.singularity){ qtsq_free(&container); return QTSQ_ERR_ALLOC; }
    memcpy(meta_ctx.singularity, meta, meta_len);
    meta_ctx.singularity_size = meta_len;
    meta_ctx.header.singularity_size = meta_len;
    meta_ctx.header.original_size = meta_len;
    result = qtsq_container_add_stream(&container, &meta_ctx, "metadata");
    qtsq_free(&meta_ctx);
    if (result != QTSQ_OK){ qtsq_free(&container); return result; }

    /* Stream 1: surfels (zlib-compressed) */
    uint8_t *packed = NULL; size_t packed_size = 0;
    if (qtsq_surfel_pack(model->surfels, model->count, &packed, &packed_size) != 0){
        qtsq_free(&container); return QTSQ_ERR_ALLOC;
    }
    uint8_t *compressed = NULL; size_t compressed_size = 0;
    int zr = surfel_zlib_compress(packed, packed_size, &compressed, &compressed_size);
    free(packed);
    if (zr != 0){ qtsq_free(&container); return QTSQ_ERR_ALLOC; }

    qtsq_context_t s_ctx;
    qtsq_init(&s_ctx);
    s_ctx.header.version  = QTSQ_VERSION_3;
    s_ctx.header.strategy = QTSQ_STRATEGY_SURFEL;
    s_ctx.header.data_type = QTSQ_TYPE_SURFEL;
    s_ctx.header.flags = QTSQ_FLAG_SPLAT_ONLY | QTSQ_FLAG_ZLIB;
    s_ctx.singularity = compressed;
    s_ctx.singularity_size = compressed_size;
    s_ctx.header.singularity_size = compressed_size;
    s_ctx.header.original_size = packed_size;
    result = qtsq_container_add_stream(&container, &s_ctx, "surfels");
    qtsq_free(&s_ctx);
    if (result != QTSQ_OK){ qtsq_free(&container); return result; }

    /* Stream 2: TSDF mesh (zlib-compressed 'MESH' pack), optional (Phase F) */
    if (mesh && mesh->nv > 0 && mesh->nt > 0){
        uint8_t *mpack = NULL; size_t mpack_size = 0;
        if (qtsq_mesh_pack(mesh, &mpack, &mpack_size) != 0 || !mpack){
            qtsq_free(&container); return QTSQ_ERR_ALLOC;
        }
        uint8_t *mcomp = NULL; size_t mcomp_size = 0;
        int mz = surfel_zlib_compress(mpack, mpack_size, &mcomp, &mcomp_size);
        free(mpack);
        if (mz != 0){ qtsq_free(&container); return QTSQ_ERR_ALLOC; }

        qtsq_context_t m_ctx;
        qtsq_init(&m_ctx);
        m_ctx.header.version = QTSQ_VERSION_3;
        m_ctx.header.strategy = QTSQ_STRATEGY_SURFEL;
        m_ctx.header.data_type = QTSQ_TYPE_MESH;
        m_ctx.header.flags = QTSQ_FLAG_SPLAT_ONLY | QTSQ_FLAG_ZLIB;
        m_ctx.singularity = mcomp;
        m_ctx.singularity_size = mcomp_size;
        m_ctx.header.singularity_size = mcomp_size;
        m_ctx.header.original_size = mpack_size;
        result = qtsq_container_add_stream(&container, &m_ctx, "mesh");
        qtsq_free(&m_ctx);
        if (result != QTSQ_OK){ qtsq_free(&container); return result; }
    }

    /* Pack + (sealed path) seal the surfels stream + rebuild */
    result = qtsq_container_pack(&container);
    if (result != QTSQ_OK){ qtsq_free(&container); return result; }

    if (author_name){
        /* Phase F: seal the surfels stream. The container API has no
         * in-place qtsq_container_set_stream — qtsq_container_pack consumes
         * its builder and add_stream only copies the singularity — so we
         * pull the packed surfels stream (index 1; "metadata" is index 0, as
         * built above) back out, seal it, and rebuild the container with the
         * sealed stream in its place. Fail-closed: without a keystore the
         * seal keys are NULL and the write is refused below (identical
         * error semantics to the unsealed path). */
        char name[256];
        qtsq_context_t s_ctx;
        qtsq_init(&s_ctx); /* get_stream only inits on success — pre-init so
                              the error path can qtsq_free safely */
        result = qtsq_container_get_stream(&container, 1, &s_ctx, name, sizeof(name));
        if (result == QTSQ_OK){
            result = qtsq_seal_authorship(&s_ctx, author_name, license,
                                          consent_flags,
                                          qtsq_keystore_get_sign_sk(),
                                          qtsq_keystore_get_sign_pk());
        }
        if (result != QTSQ_OK){ qtsq_free(&s_ctx); qtsq_free(&container); return result; }

        qtsq_context_t rebuilt;
        result = qtsq_container_create(&rebuilt);
        if (result == QTSQ_OK){
            /* Copy the metadata stream (index 0) verbatim. */
            qtsq_context_t meta_copy;
            qtsq_init(&meta_copy);
            char meta_name[256];
            result = qtsq_container_get_stream(&container, 0, &meta_copy,
                                               meta_name, sizeof(meta_name));
            if (result == QTSQ_OK){
                result = qtsq_container_add_stream(&rebuilt, &meta_copy, meta_name);
                qtsq_free(&meta_copy);
            }
        }
        if (result == QTSQ_OK){
            /* Replace stream 1 with the sealed surfels stream. */
            result = qtsq_container_add_stream(&rebuilt, &s_ctx, "surfels");
        }
        qtsq_free(&s_ctx);
        if (result == QTSQ_OK && mesh && mesh->nv > 0 && mesh->nt > 0){
            /* Preserve the Phase F mesh stream (index 2). */
            qtsq_context_t m_copy;
            qtsq_init(&m_copy);
            char m_name[256];
            int mrc = qtsq_container_get_stream(&container, 2, &m_copy,
                                                m_name, sizeof(m_name));
            if (mrc == QTSQ_OK){
                result = qtsq_container_add_stream(&rebuilt, &m_copy, m_name);
                qtsq_free(&m_copy);
            } else {
                qtsq_free(&m_copy);
            }
        }
        if (result != QTSQ_OK){ qtsq_free(&rebuilt); qtsq_free(&container); return result; }

        result = qtsq_container_pack(&rebuilt);
        if (result != QTSQ_OK){ qtsq_free(&rebuilt); qtsq_free(&container); return result; }

        /* Move the rebuilt container into place. qtsq_container_pack has
         * consumed the builder (it lives inside the ctx's singularity, which
         * pack replaced), so both structs own plain heap buffers and the
         * move cannot double-free. */
        qtsq_free(&container);
        container = rebuilt;
    }

    memset(&container.schema, 0, sizeof(container.schema));
    strncpy(container.schema.name, "SURFEL", 63);
    container.schema.data_type = QTSQ_TYPE_CONTAINER;
    container.schema.dimensions[0] = model->count;
    container.schema.num_dims = 1;
    snprintf(container.schema.encoding, sizeof(container.schema.encoding),
             "surfel2dgs-v1;codec=zlib");
    container.header.horizon_size = sizeof(qtsq_schema_t);

    /* Phase 1: fail-closed — sanitize + encrypt before writing.
     * qtsq_container_pack encrypts stream names (Phase 5); the vault
     * preserves the salt. */
    container.is_sanitized = 1;
    container.sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ENCRYPTED;
    if (qtsq_keystore_is_initialized()){
        result = qtsq_keystore_encrypt(&container);
    } else {
        result = QTSQ_ERR_NOT_ENCRYPTED;
    }
    if (result != QTSQ_OK){ qtsq_free(&container); return result; }

    result = qtsq_write(&container, filename);
    qtsq_free(&container);
    return result;
}

/* Public unsealed write — historical behavior, unchanged. */
int qtsq_surfel_container_write(const char *filename,
                                const qtsq_surfel_model_t *model){
    return surfel_container_write_internal(filename, model, NULL, NULL, NULL, 0);
}
int qtsq_surfel_container_write_sealed(const char *filename,
                                       const qtsq_surfel_model_t *model,
                                       const char *author_name,
                                       const char *license,
                                       uint32_t consent_flags){
    return surfel_container_write_internal(filename, model, NULL, author_name,
                                           license, consent_flags);
}
int qtsq_surfel_container_write_mesh(const char *filename,
                                     const qtsq_surfel_model_t *model,
                                     const qtsq_mesh_t *mesh){
    return surfel_container_write_internal(filename, model, mesh, NULL, NULL, 0);
}

/* ══════════════════════════════════════════════════════════════
 *  Container read
 * ══════════════════════════════════════════════════════════════ */

static int surfel_container_read_internal(const char *filename,
                                          qtsq_surfel_model_t *model,
                                          qtsq_mesh_t *mesh_out){
    if (!filename || !model) return QTSQ_ERR_NULL;

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int result = qtsq_read(&ctx, filename);
    if (result != QTSQ_OK){ qtsq_free(&ctx); return result; }

    /* Fail-closed round-trip: the container was keystore-encrypted on write,
     * so decrypt in place before the stream parser sees the singularity. */
    if (ctx.is_encrypted){
        if (!qtsq_keystore_is_initialized()){ qtsq_free(&ctx); return QTSQ_ERR_ENCRYPTED; }
        result = qtsq_keystore_decrypt(&ctx);
        if (result != QTSQ_OK){ qtsq_free(&ctx); return result; }
    }

    if (ctx.header.data_type != QTSQ_TYPE_CONTAINER){
        qtsq_free(&ctx); return QTSQ_ERR_FORMAT;
    }

    uint32_t num_streams = 0;
    qtsq_container_get_count(&ctx, &num_streams);

    qtsq_surfel_model_init(model);

    for (uint32_t i=0;i<num_streams;i++){
        char name[256];
        qtsq_context_t stream;
        result = qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name));
        if (result != QTSQ_OK) continue;

        if (strcmp(name, "surfels") == 0){
            uint8_t *raw = NULL; size_t raw_size = 0;
            if (stream.header.flags & QTSQ_FLAG_ZLIB){
                surfel_zlib_decompress(stream.singularity, stream.singularity_size,
                                       &raw, &raw_size, (size_t)stream.header.original_size);
            } else {
                raw_size = stream.singularity_size;
                raw = (uint8_t*)malloc(raw_size ? raw_size : 1);
                if (raw) memcpy(raw, stream.singularity, raw_size);
            }
            if (raw){
                qtsq_surfel_t *s = NULL; uint32_t sc = 0;
                if (qtsq_surfel_unpack(raw, raw_size, &s, &sc) == 0 && s){
                    qtsq_surfel_model_append_array(model, s, sc);
                    free(s);
                }
                free(raw);
            }
        } else if (strcmp(name, "mesh") == 0 && mesh_out){
            /* Phase F: TSDF mesh stream (decompress → mesh_unpack). */
            uint8_t *raw = NULL; size_t raw_size = 0;
            if (stream.header.flags & QTSQ_FLAG_ZLIB){
                surfel_zlib_decompress(stream.singularity, stream.singularity_size,
                                       &raw, &raw_size, (size_t)stream.header.original_size);
            } else {
                raw_size = stream.singularity_size;
                raw = (uint8_t*)malloc(raw_size ? raw_size : 1);
                if (raw) memcpy(raw, stream.singularity, raw_size);
            }
            if (raw){
                qtsq_mesh_unpack(raw, raw_size, mesh_out);
                free(raw);
            }
        }
        qtsq_free(&stream);
    }

    qtsq_free(&ctx);
    if (model->count > 0){
        qtsq_surfel_model_recompute_aabb(model);
        return QTSQ_OK;
    }
    return QTSQ_ERR_FORMAT;
}

int qtsq_surfel_container_read(const char *filename, qtsq_surfel_model_t *model){
    return surfel_container_read_internal(filename, model, NULL);
}

int qtsq_surfel_container_read_with_mesh(const char *filename,
                                         qtsq_surfel_model_t *model,
                                         qtsq_mesh_t *mesh){
    if (mesh) memset(mesh, 0, sizeof(*mesh));   /* caller-owned, may be partial */
    return surfel_container_read_internal(filename, model, mesh);
}
