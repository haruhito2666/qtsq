/*
 * Quantum Tensor Sequence — File I/O (.qtsq)
 * Safety: Chunked writes for >4GB, atomic rename for crash safety
 * Privacy: Enforces sanitize (Phase 1) + encrypt-to-self (Phase 2)
 * Author: Haruhito
 */

#include "qtsq.h"
#include "qtsq_keystore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Chunked fwrite — handles buffers >4GB safely on all platforms.
 * Standard fwrite(ptr, huge_size, 1, f) can fail silently on some
 * C runtimes when size exceeds size_t/2 or causes 32-bit overflow. */
static int safe_fwrite(const void *ptr, size_t size, FILE *f) {
    const uint8_t *p = (const uint8_t *)ptr;
    size_t remaining = size;
    const size_t CHUNK = 256ULL * 1024 * 1024; /* 256MB at a time */

    while (remaining > 0) {
        size_t to_write = remaining > CHUNK ? CHUNK : remaining;
        if (fwrite(p, 1, to_write, f) != to_write) return -1;
        p += to_write;
        remaining -= to_write;
    }
    return 0;
}

/* Chunked fread — same safety for huge reads */
static int safe_fread(void *ptr, size_t size, FILE *f) {
    uint8_t *p = (uint8_t *)ptr;
    size_t remaining = size;
    const size_t CHUNK = 256ULL * 1024 * 1024;

    while (remaining > 0) {
        size_t to_read = remaining > CHUNK ? CHUNK : remaining;
        if (fread(p, 1, to_read, f) != to_read) return -1;
        p += to_read;
        remaining -= to_read;
    }
    return 0;
}

static int qtsq_write_impl(const qtsq_context_t *ctx, const char *filename,
                           int allow_plaintext) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;

    /* ══ Phase 1 privacy enforcement: fail-closed on both guarantees ══
     *
     * PRODUCT §1 promises these are "unconditional on every file, every type."
     * They are gates, not settings, for the normal qtsq_write() API:
     *
     *   1. Sanitized — the context must have run through a qtsq_secure_* path
     *      (real LSB scrub / zero-meta / names-in-envelope). qtsq_init() no
     *      longer defaults is_sanitized=1, so a raw qtsq_compress_* context
     *      cannot satisfy this gate without the scrub actually happening.
     *
     *   2. Encrypted — "No plaintext .qtsq." If the caller supplied neither a
     *      recipient public key nor a password (and no keystore auto-encrypt
     *      applied), the write is rejected with QTSQ_ERR_NOT_ENCRYPTED rather
     *      than silently emitting an unencrypted file.
     *
     * The caller is responsible for encrypting (via a qtsq_secure_* wrapper,
     * qtsq_vault_encrypt_password, qtsq_vault_encrypt_pq, or keystore
     * auto-encrypt) before calling qtsq_write. */
    if (!allow_plaintext) {
        if (!ctx->is_sanitized) {
            return QTSQ_ERR_NOT_SANITIZED;
        }
        if (!ctx->is_encrypted) {
            return QTSQ_ERR_NOT_ENCRYPTED;
        }
    } else if (ctx->is_encrypted || ctx->has_security ||
               ctx->header.security_block_size != 0 ||
               (ctx->header.flags & (QTSQ_FLAG_ENCRYPTED |
                                     QTSQ_FLAG_PQ_ENCRYPTED |
                                     QTSQ_FLAG_STREAM_ENCRYPTED |
                                     QTSQ_FLAG_GEOCRYPT |
                                     QTSQ_FLAG_REGION_CRYPT |
                                     QTSQ_FLAG_DENIABLE))) {
        /* Do not let an explicitly plaintext write silently discard or
         * mislabel security state that is already present in the context. */
        return QTSQ_ERR_ENCRYPTED;
    }

    /* Write to temporary file, then atomic rename.
     * This prevents corruption if the process crashes mid-write. */
    size_t name_len = strlen(filename);
    char *tmp_name = (char *)malloc(name_len + 8);
    if (!tmp_name) return QTSQ_ERR_ALLOC;
    snprintf(tmp_name, name_len + 8, "%s.tmp", filename);

    FILE *f = fopen(tmp_name, "wb");
    if (!f) { free(tmp_name); return QTSQ_ERR_IO; }
    /* 1 MiB stdio buffer: large sequential singularity transfers should
     * not syscall per 4 KiB. */
    setvbuf(f, NULL, _IOFBF, (size_t)1 << 20);

    if (ctx->header.version == QTSQ_VERSION_3 || ctx->header.version == 0) {
        /* Default to V3 if version is not set explicitly */
        qtsq_header_t h = ctx->header;
        h.version = QTSQ_VERSION_3;
        h.security_block_size = ctx->has_security ? sizeof(qtsq_security_block_t) : 0;
        if (fwrite(&h, sizeof(qtsq_header_t), 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    } else if (ctx->header.version == QTSQ_VERSION_2) {
        qtsq_header_v2_t h2;
        memset(&h2, 0, sizeof(h2));
        memcpy(h2.magic, ctx->header.magic, 4);
        h2.version = QTSQ_VERSION_2;
        h2.strategy = ctx->header.strategy;
        h2.flags = ctx->header.flags;
        h2.data_type = ctx->header.data_type;
        h2.num_channels = ctx->header.num_channels;
        h2.sample_depth = ctx->header.sample_depth;
        h2.original_size = ctx->header.original_size;
        h2.timestamp = ctx->header.timestamp;
        memcpy(h2.checksum, ctx->header.checksum, 32);
        h2.horizon_size = ctx->header.horizon_size;
        h2.accretion_size = ctx->header.accretion_size;
        h2.singularity_size = ctx->header.singularity_size;
        h2.radiation_size = ctx->header.radiation_size;
        if (fwrite(&h2, sizeof(qtsq_header_v2_t), 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    if (ctx->header.horizon_size > 0) {
        if (fwrite(&ctx->schema, ctx->header.horizon_size, 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    for (uint32_t i = 0; i < ctx->num_transforms; i++) {
        if (fwrite(&ctx->transforms[i].type, 1, 1, f) != 1 ||
            fwrite(&ctx->transforms[i].param_size, 4, 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
        if (ctx->transforms[i].param_size > 0 && ctx->transforms[i].params) {
            if (fwrite(ctx->transforms[i].params, ctx->transforms[i].param_size, 1, f) != 1) {
                fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
            }
        }
    }

    /* Chunked write for singularity — handles >4GB safely */
    if (ctx->singularity && ctx->singularity_size > 0) {
        if (safe_fwrite(ctx->singularity, ctx->singularity_size, f) != 0) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    if (ctx->radiation_index && ctx->num_radiation > 0) {
        if (fwrite(ctx->radiation_index, sizeof(qtsq_index_t), ctx->num_radiation, f) != ctx->num_radiation) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    /* Write censorship (encryption) metadata if encrypted (Legacy v2) */
    if (!ctx->has_security && (ctx->is_encrypted || (ctx->header.flags & QTSQ_FLAG_ENCRYPTED))) {
        if (fwrite(&ctx->censorship, sizeof(qtsq_censorship_t), 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    /* Write V3 security block */
    if (ctx->has_security) {
        if (fwrite(&ctx->security, sizeof(qtsq_security_block_t), 1, f) != 1) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
    }

    /* Write recovery (ECC) data if present */
    if (ctx->has_recovery && ctx->recovery.num_chunks > 0) {
        if (fwrite(&ctx->recovery.num_chunks, 4, 1, f) != 1 ||
            fwrite(&ctx->recovery.parity_size, 4, 1, f) != 1 ||
            fwrite(ctx->recovery.chunk_checksums, 4, ctx->recovery.num_chunks, f) != ctx->recovery.num_chunks) {
            fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
        }
        if (ctx->recovery.reed_solomon_parity && ctx->recovery.parity_size > 0) {
            if (fwrite(ctx->recovery.reed_solomon_parity, ctx->recovery.parity_size, 1, f) != 1) {
                fclose(f); unlink(tmp_name); free(tmp_name); return QTSQ_ERR_IO;
            }
        }
    }

    /* Flush to disk before rename */
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Atomic rename: old file is replaced only after new file is fully written */
    if (rename(tmp_name, filename) != 0) {
        unlink(tmp_name);
        free(tmp_name);
        return QTSQ_ERR_IO;
    }

    free(tmp_name);
    return QTSQ_OK;
}

int qtsq_write(const qtsq_context_t *ctx, const char *filename) {
    return qtsq_write_impl(ctx, filename, 0);
}

int qtsq_write_plaintext(const qtsq_context_t *ctx, const char *filename) {
    return qtsq_write_impl(ctx, filename, 1);
}

int qtsq_read(qtsq_context_t *ctx, const char *filename) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;

    FILE *f = fopen(filename, "rb");
    if (!f) return QTSQ_ERR_IO;
    setvbuf(f, NULL, _IOFBF, (size_t)1 << 20);

    /* Peek at version byte (offset 4) to determine header size */
    uint8_t peek_buf[5];
    if (fread(peek_buf, 1, 5, f) != 5) {
        fclose(f); return QTSQ_ERR_IO;
    }
    rewind(f);

    /* Magic check */
    if (peek_buf[0] != QTSQ_MAGIC_0 ||
        peek_buf[1] != QTSQ_MAGIC_1 ||
        peek_buf[2] != QTSQ_MAGIC_2 ||
        peek_buf[3] != QTSQ_MAGIC_3) {
        fclose(f); return QTSQ_ERR_FORMAT;
    }

    uint8_t file_version = peek_buf[4];

    if (file_version == QTSQ_VERSION_1) {
        /* Read v1 header (80 bytes) and upgrade to v2 struct */
        qtsq_header_v1_t v1;
        if (fread(&v1, sizeof(qtsq_header_v1_t), 1, f) != 1) {
            fclose(f); return QTSQ_ERR_IO;
        }
        /* Copy common fields */
        memcpy(ctx->header.magic, v1.magic, 4);
        ctx->header.version         = QTSQ_VERSION_2; /* upgrade in memory */
        ctx->header.strategy        = v1.strategy;
        ctx->header.flags           = v1.flags;
        ctx->header.data_type       = v1.data_type;
        ctx->header.num_channels    = v1.num_channels;
        ctx->header.sample_depth    = v1.sample_depth;
        ctx->header.original_size   = v1.original_size;
        ctx->header.timestamp       = v1.timestamp;
        memcpy(ctx->header.checksum, v1.checksum, 32);
        /* Zero-extend 32-bit sizes to 64-bit */
        ctx->header.horizon_size     = (uint64_t)v1.horizon_size;
        ctx->header.accretion_size   = (uint64_t)v1.accretion_size;
        ctx->header.singularity_size = (uint64_t)v1.singularity_size;
        ctx->header.radiation_size   = (uint64_t)v1.radiation_size;
        ctx->header.security_block_size = 0;
    } else if (file_version == QTSQ_VERSION_2) {
        /* Read v2 header (96 bytes) */
        qtsq_header_v2_t v2;
        if (fread(&v2, sizeof(qtsq_header_v2_t), 1, f) != 1) {
            fclose(f); return QTSQ_ERR_IO;
        }
        memcpy(ctx->header.magic, v2.magic, 4);
        ctx->header.version         = QTSQ_VERSION_3; /* upgrade */
        ctx->header.strategy        = v2.strategy;
        ctx->header.flags           = v2.flags;
        ctx->header.data_type       = v2.data_type;
        ctx->header.num_channels    = v2.num_channels;
        ctx->header.sample_depth    = v2.sample_depth;
        ctx->header.original_size   = v2.original_size;
        ctx->header.timestamp       = v2.timestamp;
        memcpy(ctx->header.checksum, v2.checksum, 32);
        ctx->header.horizon_size     = v2.horizon_size;
        ctx->header.accretion_size   = v2.accretion_size;
        ctx->header.singularity_size = v2.singularity_size;
        ctx->header.radiation_size   = v2.radiation_size;
        ctx->header.security_block_size = 0;
    } else if (file_version == QTSQ_VERSION_3) {
        /* Read v3 header (112 bytes) directly */
        if (fread(&ctx->header, sizeof(qtsq_header_t), 1, f) != 1) {
            fclose(f); return QTSQ_ERR_IO;
        }
    } else {
        /* A version byte above 3 is a future (or corrupt) header. It
         * used to fall through and be parsed as v3, which misreads any
         * future layout that moved or grew a field — silent corruption
         * instead of a clean refusal. Reject it. */
        fclose(f);
        return QTSQ_ERR_FORMAT;
    }

    if (ctx->header.horizon_size > 0) {
        size_t to_read = ctx->header.horizon_size;
        if (to_read > sizeof(qtsq_schema_t)) to_read = sizeof(qtsq_schema_t);
        if (fread(&ctx->schema, to_read, 1, f) != 1) {
            fclose(f); return QTSQ_ERR_IO;
        }
        if (ctx->header.horizon_size > to_read)
            fseek(f, ctx->header.horizon_size - to_read, SEEK_CUR);
    }

    if (ctx->header.accretion_size > 0) {
        long acc_start = ftell(f);
        long acc_end = acc_start + ctx->header.accretion_size;
        uint32_t count = 0;

        while (ftell(f) < acc_end) {
            uint8_t type; uint32_t psize;
            if (fread(&type, 1, 1, f) != 1) break;
            if (fread(&psize, 4, 1, f) != 1) break;
            /* IO-4 FIX: zero-sized params with zero type = padding/sentinel, stop scanning
             * Without this check, fseek(f, 0, SEEK_CUR) is a no-op → infinite loop */
            if (psize == 0 && type == 0) break;
            if (psize > 65536) { fclose(f); return QTSQ_ERR_FORMAT; } /* IO-2 guard */
            fseek(f, psize, SEEK_CUR);
            count++;
        }

        ctx->num_transforms = count;
        ctx->transforms = calloc(count, sizeof(qtsq_transform_t));
        if (!ctx->transforms) { fclose(f); return QTSQ_ERR_ALLOC; }

        fseek(f, acc_start, SEEK_SET);
        for (uint32_t i = 0; i < count; i++) {
            if (fread(&ctx->transforms[i].type, 1, 1, f) != 1 ||
                fread(&ctx->transforms[i].param_size, 4, 1, f) != 1) {
                fclose(f); return QTSQ_ERR_FORMAT;
            }
            if (ctx->transforms[i].param_size > 0) {
                /* IO-2 FIX: param_size comes from file — bound before malloc */
                if (ctx->transforms[i].param_size > 65536) { fclose(f); return QTSQ_ERR_FORMAT; }
                ctx->transforms[i].params = malloc(ctx->transforms[i].param_size);
                if (!ctx->transforms[i].params) { fclose(f); return QTSQ_ERR_ALLOC; }
                if (fread(ctx->transforms[i].params, ctx->transforms[i].param_size, 1, f) != 1) {
                    fclose(f); return QTSQ_ERR_FORMAT;
                }
            }
        }
    }

    if (ctx->header.singularity_size > 0) {
        /* IO-1 FIX: singularity_size is read from file — bound it to prevent DoS malloc.
         * 4 GB is a generous cap for any real payload; malicious files can set this to UINT64_MAX. */
        if (ctx->header.singularity_size > 4000000000ULL) {
            fclose(f); return QTSQ_ERR_FORMAT;
        }
        ctx->singularity_size = ctx->header.singularity_size;
        ctx->singularity = malloc(ctx->singularity_size);
        if (!ctx->singularity) { fclose(f); return QTSQ_ERR_ALLOC; }
        /* Chunked read for >4GB safety */
        if (safe_fread(ctx->singularity, ctx->singularity_size, f) != 0) {
            fclose(f); return QTSQ_ERR_IO;
        }
    }

    if (ctx->header.radiation_size > 0) {
        /* singularity_size and the recovery block are both capped; this
         * one was not, so a 4-byte header could request a multi-GB
         * calloc with no relationship to the file it came from. Bound
         * it against what the file can actually hold, and against the
         * count a uint32_t can express without truncating. */
        long cur = ftell(f);
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return QTSQ_ERR_IO; }
        long fsz = ftell(f);
        if (fsz < 0 || cur < 0) { fclose(f); return QTSQ_ERR_IO; }
        if (fseek(f, cur, SEEK_SET) != 0) { fclose(f); return QTSQ_ERR_IO; }
        if (ctx->header.radiation_size > (uint64_t)(fsz - cur)) {
            fclose(f); return QTSQ_ERR_FORMAT;
        }
        uint64_t nrad = ctx->header.radiation_size / sizeof(qtsq_index_t);
        if (nrad == 0 || nrad > UINT32_MAX) { fclose(f); return QTSQ_ERR_FORMAT; }
        ctx->num_radiation = (uint32_t)nrad;
        ctx->radiation_index = calloc(ctx->num_radiation, sizeof(qtsq_index_t));
        if (!ctx->radiation_index) { fclose(f); return QTSQ_ERR_ALLOC; }
        if (fread(ctx->radiation_index, sizeof(qtsq_index_t), ctx->num_radiation, f) != ctx->num_radiation) {
            fclose(f); return QTSQ_ERR_FORMAT;
        }
    }

    ctx->is_encrypted = (ctx->header.flags & QTSQ_FLAG_ENCRYPTED) || 
                        (ctx->header.flags & QTSQ_FLAG_PQ_ENCRYPTED) ? 1 : 0;
    ctx->has_recovery = (ctx->header.flags & QTSQ_FLAG_HAS_ECC) ? 1 : 0;

    /* Reconstruct sanitize state from header flags */
    if ((ctx->header.flags & QTSQ_FLAG_ANTI_STEGO) ||
        (ctx->header.flags & QTSQ_FLAG_ZERO_META)) {
        ctx->is_sanitized = 1;
        ctx->sanitize_report = 0;
        if (ctx->header.flags & QTSQ_FLAG_ANTI_STEGO)  ctx->sanitize_report |= QTSQ_SANITIZE_IMAGE_LSB;
        if (ctx->header.flags & QTSQ_FLAG_ZERO_META)   ctx->sanitize_report |= QTSQ_SANITIZE_ZERO_META;
        if (ctx->is_encrypted)                          ctx->sanitize_report |= QTSQ_SANITIZE_ENCRYPTED;
    }

    /* Read V3 security block if present (M19 fix: zero the struct first
     * so a partial read doesn't leave uninitialized tail fields). */
    if (ctx->header.security_block_size > 0 && ctx->header.security_block_size <= sizeof(qtsq_security_block_t)) {
        memset(&ctx->security, 0, sizeof(qtsq_security_block_t));
        if (fread(&ctx->security, ctx->header.security_block_size, 1, f) == 1) {
            ctx->has_security = 1;
        }
    } else if (ctx->is_encrypted) {
        /* Read legacy censorship metadata if encrypted (V1/V2/V3 legacy path) */
        if (fread(&ctx->censorship, sizeof(qtsq_censorship_t), 1, f) != 1) {
            ctx->is_encrypted = 0;
        }
    }

    /* Read recovery (ECC) data if present */
    if (ctx->has_recovery) {
        uint32_t num_chunks = 0, parity_size = 0;
        if (fread(&num_chunks, 4, 1, f) == 1 && fread(&parity_size, 4, 1, f) == 1) {
            /* IO-3 FIX: bound recovery block sizes from file before allocating */
            if (num_chunks > 1000000 || parity_size > 100000000UL) {
                fclose(f); return QTSQ_ERR_FORMAT;
            }
            ctx->recovery.num_chunks = num_chunks;
            ctx->recovery.parity_size = parity_size;
            ctx->recovery.chunk_checksums = (uint32_t *)calloc(num_chunks, sizeof(uint32_t));
            if (ctx->recovery.chunk_checksums) {
                if (fread(ctx->recovery.chunk_checksums, 4, num_chunks, f) != num_chunks) {
                    fclose(f); return QTSQ_ERR_FORMAT;
                }
            }
            if (parity_size > 0) {
                ctx->recovery.reed_solomon_parity = (uint8_t *)malloc(parity_size);
                if (ctx->recovery.reed_solomon_parity) {
                    if (fread(ctx->recovery.reed_solomon_parity, parity_size, 1, f) != 1) {
                        fclose(f); return QTSQ_ERR_FORMAT;
                    }
                }
            }
        }
    }

    /* GEN-1 FIX (Phase 6b): checksum verification now happens after decode
     * in qtsq_decompress (qtsq_verify_decoded_checksum, qtsq_radiation.c).
     * Engines write sha256(original input) into header.checksum; only the
     * compressed singularity exists at read time, so it cannot be verified
     * here. (The original read-time check hashed the singularity and
     * rejected every healthy checksummed-unencrypted file.) */

    fclose(f);
    return QTSQ_OK;
}
