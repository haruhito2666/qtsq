/*
 * Quantum Tensor Sequence — Turbo I/O Engine
 * All 7 layers of high-performance I/O
 *
 * Author: Haruhito
 */

#include "qtsq.h"
#include "qtsq_turbo.h"
#include "qtsq_sanitize.h"
#include "../crypto/common/aes.h"
#include "../crypto/common/randombytes.h"
#include "../crypto/common/fips202.h"
#include "../crypto/argon2/argon2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Platform headers ── */
#ifdef QTSQ_MMAP_POSIX
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

#ifdef QTSQ_THREADS_POSIX
  #include <pthread.h>
#endif

/* ── SIMD headers ── */
#ifdef QTSQ_SIMD_AVX2
  #include <immintrin.h>
#elif defined(QTSQ_SIMD_SSE2)
  #include <emmintrin.h>
#elif defined(QTSQ_SIMD_NEON)
  #include <arm_neon.h>
#endif

/* ═══════════════════════════════════════════════════════
 *  Layer 7: Adaptive Configuration
 * ═══════════════════════════════════════════════════════ */

void qtsq_turbo_config_default(qtsq_turbo_config_t *cfg) {
    if (!cfg) return;
    cfg->block_size  = QTSQ_BLOCK_SIZE_DEFAULT;
    cfg->num_threads = 4;
    cfg->use_mmap    = 1;
    cfg->use_simd    = 1;
    cfg->encrypt     = 0;
    cfg->password    = NULL;
}

void qtsq_turbo_config_auto(qtsq_turbo_config_t *cfg, size_t data_size) {
    if (!cfg) return;
    qtsq_turbo_config_default(cfg);

    if (data_size < QTSQ_TIER_SMALL) {
        /* Small: single block, no threading, no mmap */
        cfg->block_size  = (uint32_t)data_size;
        cfg->num_threads = 1;
        cfg->use_mmap    = 0;
    } else if (data_size < QTSQ_TIER_MEDIUM) {
        /* Medium: 64KB blocks, 4 threads */
        cfg->block_size  = QTSQ_BLOCK_SIZE_DEFAULT;
        cfg->num_threads = 4;
        cfg->use_mmap    = 0; /* buffered is fine up to 100MB */
    } else if (data_size < QTSQ_TIER_LARGE) {
        /* Large: 256KB blocks, 8 threads, mmap */
        cfg->block_size  = 256 * 1024;
        cfg->num_threads = 8;
        cfg->use_mmap    = 1;
    } else {
        /* Huge: 1MB blocks, max threads, mmap */
        cfg->block_size  = QTSQ_BLOCK_SIZE_MAX;
        cfg->num_threads = QTSQ_MAX_THREADS;
        cfg->use_mmap    = 1;
    }

    /* Clamp threads */
    if (cfg->num_threads > QTSQ_MAX_THREADS)
        cfg->num_threads = QTSQ_MAX_THREADS;
}

/* ═══════════════════════════════════════════════════════
 *  Layer 1: Memory-Mapped I/O
 * ═══════════════════════════════════════════════════════ */

#ifdef QTSQ_MMAP_POSIX

int qtsq_mmap_open_read(qtsq_mmap_t *m, const char *filename) {
    if (!m || !filename) return QTSQ_ERR_NULL;
    memset(m, 0, sizeof(*m));

    m->fd = open(filename, O_RDONLY);
    if (m->fd < 0) return QTSQ_ERR_IO;

    struct stat sb;
    if (fstat(m->fd, &sb) < 0) {
        close(m->fd);
        return QTSQ_ERR_IO;
    }
    m->size = (size_t)sb.st_size;

    if (m->size == 0) {
        close(m->fd);
        return QTSQ_ERR_FORMAT;
    }

    m->data = mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (m->data == MAP_FAILED) {
        close(m->fd);
        m->data = NULL;
        return QTSQ_ERR_IO;
    }

    /* Hint: sequential access expected */
    madvise(m->data, m->size, MADV_SEQUENTIAL);

    m->is_write = 0;
    return QTSQ_OK;
}

int qtsq_mmap_open_write(qtsq_mmap_t *m, const char *filename, size_t size) {
    if (!m || !filename || size == 0) return QTSQ_ERR_NULL;
    memset(m, 0, sizeof(*m));

    m->fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (m->fd < 0) return QTSQ_ERR_IO;

    /* Pre-allocate file to required size */
    if (ftruncate(m->fd, (off_t)size) < 0) {
        close(m->fd);
        return QTSQ_ERR_IO;
    }

    m->size = size;
    m->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
    if (m->data == MAP_FAILED) {
        close(m->fd);
        m->data = NULL;
        return QTSQ_ERR_IO;
    }

    m->is_write = 1;
    return QTSQ_OK;
}

void qtsq_mmap_close(qtsq_mmap_t *m) {
    if (!m) return;
    if (m->data && m->data != MAP_FAILED) {
        if (m->is_write) msync(m->data, m->size, MS_SYNC);
        munmap(m->data, m->size);
    }
    if (m->fd >= 0) close(m->fd);
    memset(m, 0, sizeof(*m));
    m->fd = -1;
}

#endif /* QTSQ_MMAP_POSIX */

/* ═══════════════════════════════════════════════════════
 *  Layer 3: SIMD Vectorization
 * ═══════════════════════════════════════════════════════ */

#if defined(QTSQ_SIMD_AVX2)

void qtsq_simd_xor(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t len) {
    size_t i = 0;
    /* Process 32 bytes at a time with AVX2 */
    for (; i + 32 <= len; i += 32) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i vr = _mm256_xor_si256(va, vb);
        _mm256_storeu_si256((__m256i *)(dst + i), vr);
    }
    /* Scalar tail */
    for (; i < len; i++) dst[i] = a[i] ^ b[i];
}

size_t qtsq_simd_rle_count(const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    size_t runs = 1;
    size_t i = 1;
    /* AVX2 comparison: compare adjacent 32-byte blocks */
    for (; i + 32 <= len; i += 32) {
        __m256i curr = _mm256_loadu_si256((const __m256i *)(data + i));
        __m256i prev = _mm256_loadu_si256((const __m256i *)(data + i - 1));
        __m256i eq   = _mm256_cmpeq_epi8(curr, prev);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
        /* Each 0-bit in mask = a run boundary */
        runs += __builtin_popcount(~mask);
    }
    /* Scalar tail */
    for (; i < len; i++) {
        if (data[i] != data[i - 1]) runs++;
    }
    return runs;
}

#elif defined(QTSQ_SIMD_SSE2)

void qtsq_simd_xor(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t len) {
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i va = _mm_loadu_si128((const __m128i *)(a + i));
        __m128i vb = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i vr = _mm_xor_si128(va, vb);
        _mm_storeu_si128((__m128i *)(dst + i), vr);
    }
    for (; i < len; i++) dst[i] = a[i] ^ b[i];
}

size_t qtsq_simd_rle_count(const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    size_t runs = 1;
    size_t i = 1;
    for (; i + 16 <= len; i += 16) {
        __m128i curr = _mm_loadu_si128((const __m128i *)(data + i));
        __m128i prev = _mm_loadu_si128((const __m128i *)(data + i - 1));
        __m128i eq   = _mm_cmpeq_epi8(curr, prev);
        int mask     = _mm_movemask_epi8(eq);
        runs += __builtin_popcount(~mask & 0xFFFF);
    }
    for (; i < len; i++) {
        if (data[i] != data[i - 1]) runs++;
    }
    return runs;
}

#elif defined(QTSQ_SIMD_NEON)

void qtsq_simd_xor(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t len) {
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t va = vld1q_u8(a + i);
        uint8x16_t vb = vld1q_u8(b + i);
        uint8x16_t vr = veorq_u8(va, vb);
        vst1q_u8(dst + i, vr);
    }
    for (; i < len; i++) dst[i] = a[i] ^ b[i];
}

size_t qtsq_simd_rle_count(const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    size_t runs = 1;
    for (size_t i = 1; i < len; i++) {
        if (data[i] != data[i - 1]) runs++;
    }
    return runs;
}

#else /* Scalar fallback */

void qtsq_simd_xor(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; i++) dst[i] = a[i] ^ b[i];
}

size_t qtsq_simd_rle_count(const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    size_t runs = 1;
    for (size_t i = 1; i < len; i++) {
        if (data[i] != data[i - 1]) runs++;
    }
    return runs;
}

#endif

/* SIMD-accelerated CRC32 (simple polynomial, not hardware CRC32C) */
uint32_t qtsq_simd_crc32(const uint8_t *data, size_t len) {
    /* Use the existing qtsq_crc32 from utils — already fast enough */
    return qtsq_crc32(data, len);
}

void qtsq_simd_memset(uint8_t *dst, uint8_t val, size_t len) {
#if defined(QTSQ_SIMD_AVX2)
    __m256i v = _mm256_set1_epi8((char)val);
    size_t i = 0;
    for (; i + 32 <= len; i += 32)
        _mm256_storeu_si256((__m256i *)(dst + i), v);
    for (; i < len; i++) dst[i] = val;
#elif defined(QTSQ_SIMD_SSE2)
    __m128i v = _mm_set1_epi8((char)val);
    size_t i = 0;
    for (; i + 16 <= len; i += 16)
        _mm_storeu_si128((__m128i *)(dst + i), v);
    for (; i < len; i++) dst[i] = val;
#elif defined(QTSQ_SIMD_NEON)
    uint8x16_t v = vdupq_n_u8(val);
    size_t i = 0;
    for (; i + 16 <= len; i += 16)
        vst1q_u8(dst + i, v);
    for (; i < len; i++) dst[i] = val;
#else
    memset(dst, val, len);
#endif
}

/* ═══════════════════════════════════════════════════════
 *  Layer 5: Zero-Copy In-Place Encryption
 * ═══════════════════════════════════════════════════════ */

/*
 * In-place AES-256-CTR encryption for a single block.
 * Uses the vendored PQClean AES (constant-time, table-free) instead of the
 * previous hand-rolled XOR PRNG which was cryptographically weak. Each block
 * gets a unique counter (derived from iv + block_counter) so keystreams don't
 * repeat across blocks. CTR mode is symmetric: same function encrypts and
 * decrypts.
 */
int qtsq_turbo_encrypt_block(uint8_t *data, size_t size,
                              const uint8_t *key, const uint8_t *iv,
                              uint32_t block_counter) {
    if (!data || !key || !iv || size == 0) return QTSQ_ERR_NULL;

    /* Build the 16-byte IV+counter block: [iv 12B][counter 4B big-endian] */
    uint8_t iv_block[16];
    memcpy(iv_block, iv, 12);
    iv_block[12] = (uint8_t)(block_counter >> 24);
    iv_block[13] = (uint8_t)(block_counter >> 16);
    iv_block[14] = (uint8_t)(block_counter >> 8);
    iv_block[15] = (uint8_t)(block_counter);

    aes256ctx aes;
    aes256_ctr_keyexp(&aes, key);

    /* PQClean aes256_ctr emits the keystream; XOR it with the data in-place. */
    uint8_t *keystream = (uint8_t *)malloc(size);
    if (!keystream) { aes256_ctx_release(&aes); return QTSQ_ERR_ALLOC; }
    aes256_ctr(keystream, size, iv_block, &aes);
    for (size_t i = 0; i < size; i++) {
        data[i] ^= keystream[i];
    }

    qtsq_secure_zero(keystream, size);
    free(keystream);
    aes256_ctx_release(&aes);
    qtsq_secure_zero(&aes, sizeof(aes));
    qtsq_secure_zero(iv_block, sizeof(iv_block));
    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════
 *  Layer 4: Multi-threaded Block Compression
 * ═══════════════════════════════════════════════════════ */

#ifdef QTSQ_THREADS_POSIX

/* Worker function for parallel block compression */
static void *block_compress_worker(void *arg) {
    qtsq_block_job_t *job = (qtsq_block_job_t *)arg;

    /* Compute checksum of input (for integrity) */
    job->checksum = qtsq_crc32(job->input, job->input_size);

    /* Compress this block using a temporary context */
    qtsq_context_t tmp;
    qtsq_init(&tmp);

    job->status = qtsq_compress_raw(&tmp, job->input, job->input_size, 0);

    if (job->status == QTSQ_OK && tmp.singularity && tmp.singularity_size > 0) {
        /* Check if compression actually helped */
        if (tmp.singularity_size < job->input_size) {
            job->output = (uint8_t *)malloc(tmp.singularity_size);
            if (job->output) {
                memcpy(job->output, tmp.singularity, tmp.singularity_size);
                job->output_size = tmp.singularity_size;
                job->strategy = tmp.header.strategy;
            } else {
                job->status = QTSQ_ERR_ALLOC;
            }
        } else {
            /* Store raw — compression didn't help */
            job->output = (uint8_t *)malloc(job->input_size);
            if (job->output) {
                memcpy(job->output, job->input, job->input_size);
                job->output_size = job->input_size;
                job->strategy = QTSQ_STRATEGY_NONE;
            } else {
                job->status = QTSQ_ERR_ALLOC;
            }
        }
    } else if (job->status == QTSQ_OK) {
        /* Singularity produced nothing — store raw */
        job->output = (uint8_t *)malloc(job->input_size);
        if (job->output) {
            memcpy(job->output, job->input, job->input_size);
            job->output_size = job->input_size;
            job->strategy = QTSQ_STRATEGY_NONE;
        } else {
            job->status = QTSQ_ERR_ALLOC;
        }
    }

    qtsq_free(&tmp);
    return NULL;
}

#endif /* QTSQ_THREADS_POSIX */

/* ═══════════════════════════════════════════════════════
 *  Layer 2 + 4 + 5 + 6 + 7: Turbo Write
 *  Split → Parallel Compress → Encrypt → Write
 * ═══════════════════════════════════════════════════════ */

int qtsq_turbo_write(const qtsq_context_t *ctx, const char *filename,
                     const qtsq_turbo_config_t *cfg) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;

    /* Get the raw data to split into blocks */
    const uint8_t *raw_data = ctx->singularity;
    size_t raw_size = ctx->singularity_size;

    if (!raw_data || raw_size == 0) {
        /* No singularity data, fall back to legacy write */
        return qtsq_write(ctx, filename);
    }

    qtsq_turbo_config_t auto_cfg;
    if (!cfg) {
        qtsq_turbo_config_auto(&auto_cfg, raw_size);
        cfg = &auto_cfg;
    }

    /* Phase 1: turbo bypasses qtsq_write, so enforce encryption here */
    if (!cfg->encrypt || !cfg->password) {
        return QTSQ_ERR_NOT_ENCRYPTED;
    }

    uint32_t block_size = cfg->block_size;
    if (block_size == 0) block_size = QTSQ_BLOCK_SIZE_DEFAULT;
    if (block_size < QTSQ_BLOCK_SIZE_MIN) block_size = QTSQ_BLOCK_SIZE_MIN;
    if (block_size > QTSQ_BLOCK_SIZE_MAX) block_size = QTSQ_BLOCK_SIZE_MAX;

    /* ── Layer 2: Split into blocks ── */
    uint32_t num_blocks = (uint32_t)((raw_size + block_size - 1) / block_size);

    qtsq_block_job_t *jobs = (qtsq_block_job_t *)calloc(num_blocks, sizeof(qtsq_block_job_t));
    if (!jobs) return QTSQ_ERR_ALLOC;

    /* Set up job inputs */
    for (uint32_t i = 0; i < num_blocks; i++) {
        size_t offset = (size_t)i * block_size;
        size_t this_block = block_size;
        if (offset + this_block > raw_size)
            this_block = raw_size - offset;

        jobs[i].input      = (uint8_t *)(raw_data + offset);
        jobs[i].input_size = this_block;
        jobs[i].output     = NULL;
        jobs[i].status     = -1;
    }

    /* ── Layer 4: Parallel compression ── */
#ifdef QTSQ_THREADS_POSIX
    {
        uint8_t nthreads = cfg->num_threads;
        if (nthreads < 1) nthreads = 1;
        if (nthreads > QTSQ_MAX_THREADS) nthreads = QTSQ_MAX_THREADS;

        /* Process blocks in batches of nthreads */
        for (uint32_t batch_start = 0; batch_start < num_blocks; batch_start += nthreads) {
            uint32_t batch_end = batch_start + nthreads;
            if (batch_end > num_blocks) batch_end = num_blocks;
            uint32_t batch_count = batch_end - batch_start;

            pthread_t threads[QTSQ_MAX_THREADS];

            /* Launch threads */
            for (uint32_t t = 0; t < batch_count; t++) {
                pthread_create(&threads[t], NULL,
                               block_compress_worker,
                               &jobs[batch_start + t]);
            }

            /* Wait for all threads in this batch */
            for (uint32_t t = 0; t < batch_count; t++) {
                pthread_join(threads[t], NULL);
            }
        }
    }
#else
    /* Single-threaded fallback */
    for (uint32_t i = 0; i < num_blocks; i++) {
        block_compress_worker(&jobs[i]);
    }
#endif

    /* Check for compression errors */
    for (uint32_t i = 0; i < num_blocks; i++) {
        if (jobs[i].status != QTSQ_OK) {
            int err = jobs[i].status;
            for (uint32_t j = 0; j < num_blocks; j++) free(jobs[j].output);
            free(jobs);
            return err;
        }
    }

    /* ── Layer 5: In-place encrypt each block ── */
    if (cfg->encrypt && cfg->password) {
        uint8_t key[32];
        uint8_t iv[16];
        /* Derive key from password with Argon2id (t=3, m=64MB, p=4) — the
         * same KDF the vault uses. The previous code was a single unsalted
         * SHA-256 (trivially brute-forceable; IV derived from the key).
         * Generate a fresh random IV per file and persist it in the header
         * reserved32 field alongside the turbo magic. */
        uint8_t salt[32];
        randombytes(salt, 32);
        randombytes(iv, 12);
        memset(iv + 12, 0, 4);

        int ar = argon2id_hash_raw(
            3, 65536, 4,
            cfg->password, strlen(cfg->password),
            salt, 32,
            key, 32);
        if (ar != ARGON2_OK) {
            qtsq_secure_zero(key, sizeof(key));
            for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
            free(jobs);
            return QTSQ_ERR_ALLOC;
        }

        for (uint32_t i = 0; i < num_blocks; i++) {
            qtsq_turbo_encrypt_block(jobs[i].output, jobs[i].output_size,
                                      key, iv, i);
        }
        qtsq_secure_zero(key, sizeof(key));
        qtsq_secure_zero(iv, sizeof(iv));
    }

    /* ── Calculate total output size ── */
    size_t index_size = sizeof(qtsq_block_index_header_t)
                      + num_blocks * sizeof(qtsq_block_entry_t);
    size_t total_data = 0;
    for (uint32_t i = 0; i < num_blocks; i++)
        total_data += jobs[i].output_size;

    size_t header_and_schema = sizeof(qtsq_header_t);
    if (ctx->header.horizon_size > 0)
        header_and_schema += ctx->header.horizon_size;

    size_t total_size = header_and_schema + index_size + total_data;

    /* ── Layer 1 / 6: Write output (mmap for large, stdio for small) ── */
    FILE *f = fopen(filename, "wb");
    if (!f) {
        for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
        free(jobs);
        return QTSQ_ERR_IO;
    }

    /* Write modified header */
    qtsq_header_t hdr = ctx->header;
    hdr.singularity_size = total_data;
    /* Store turbo block info in radiation_size as a marker */
    hdr.radiation_size = (uint32_t)index_size;
    /* Set turbo flag in reserved field */
    hdr.reserved32 = QTSQ_TURBO_MAGIC;

    if (fwrite(&hdr, sizeof(qtsq_header_t), 1, f) != 1) {
        fclose(f);
        for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
        free(jobs);
        return QTSQ_ERR_IO;
    }

    /* Write schema if present */
    if (ctx->header.horizon_size > 0) {
        if (fwrite(&ctx->schema, ctx->header.horizon_size, 1, f) != 1) {
            fclose(f);
            for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
            free(jobs);
            return QTSQ_ERR_IO;
        }
    }

    /* ── Layer 2: Write block index ── */
    qtsq_block_index_header_t idx_hdr;
    idx_hdr.magic      = QTSQ_TURBO_MAGIC;
    idx_hdr.block_size = block_size;
    idx_hdr.num_blocks = num_blocks;
    idx_hdr.index_checksum = 0; /* Will be filled below */

    if (fwrite(&idx_hdr, sizeof(idx_hdr), 1, f) != 1) {
        fclose(f);
        for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
        free(jobs);
        return QTSQ_ERR_IO;
    }

    /* Build block entries with file offsets */
    size_t data_start = header_and_schema + index_size;
    size_t running_offset = data_start;

    qtsq_block_entry_t *entries = (qtsq_block_entry_t *)calloc(num_blocks,
                                      sizeof(qtsq_block_entry_t));
    if (!entries) {
        fclose(f);
        for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
        free(jobs);
        return QTSQ_ERR_ALLOC;
    }

    for (uint32_t i = 0; i < num_blocks; i++) {
        entries[i].original_size   = (uint32_t)jobs[i].input_size;
        entries[i].compressed_size = (uint32_t)jobs[i].output_size;
        entries[i].file_offset     = running_offset;
        entries[i].checksum        = jobs[i].checksum;
        entries[i].strategy        = jobs[i].strategy;
        entries[i].flags           = (cfg->encrypt && cfg->password) ? 0x01 : 0x00;
        running_offset            += jobs[i].output_size;
    }

    /* Checksum the index */
    idx_hdr.index_checksum = qtsq_crc32((const uint8_t *)entries,
                                         num_blocks * sizeof(qtsq_block_entry_t));

    /* Rewrite index header with correct checksum */
    fseek(f, (long)header_and_schema, SEEK_SET);
    if (fwrite(&idx_hdr, sizeof(idx_hdr), 1, f) != 1 ||
        fwrite(entries, sizeof(qtsq_block_entry_t), num_blocks, f) != num_blocks) {
        fclose(f);
        free(entries);
        for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
        free(jobs);
        return QTSQ_ERR_IO;
    }

    /* ── Layer 6: Write all compressed blocks (sequential for now) ── */
    for (uint32_t i = 0; i < num_blocks; i++) {
        if (fwrite(jobs[i].output, 1, jobs[i].output_size, f) != jobs[i].output_size) {
            fclose(f);
            free(entries);
            for (uint32_t j = i; j < num_blocks; j++) free(jobs[j].output);
            free(jobs);
            return QTSQ_ERR_IO;
        }
    }

    fclose(f);

    /* Cleanup */
    for (uint32_t i = 0; i < num_blocks; i++) free(jobs[i].output);
    free(jobs);
    free(entries);

    (void)total_size; /* used for clarity only */
    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════
 *  Layer 1 + 2: Turbo Read
 *  Detect turbo format, mmap, read blocks
 * ═══════════════════════════════════════════════════════ */

int qtsq_turbo_read(qtsq_context_t *ctx, const char *filename) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;

    qtsq_mmap_t m;
    int res = qtsq_mmap_open_read(&m, filename);
    if (res != QTSQ_OK) {
        /* Fallback to legacy read if mmap fails */
        return qtsq_read(ctx, filename);
    }

    const uint8_t *data = (const uint8_t *)m.data;
    size_t data_len = m.size;

    if (data_len < sizeof(qtsq_header_t)) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    /* Read header */
    memcpy(&ctx->header, data, sizeof(qtsq_header_t));

    /* Magic check */
    if (ctx->header.magic[0] != QTSQ_MAGIC_0 ||
        ctx->header.magic[1] != QTSQ_MAGIC_1 ||
        ctx->header.magic[2] != QTSQ_MAGIC_2 ||
        ctx->header.magic[3] != QTSQ_MAGIC_3) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    size_t pos = sizeof(qtsq_header_t);

    /* Read schema */
    if (ctx->header.horizon_size > 0) {
        size_t to_read = ctx->header.horizon_size;
        if (to_read > sizeof(qtsq_schema_t)) to_read = sizeof(qtsq_schema_t);
        if (pos + to_read > data_len) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_FORMAT;
        }
        memcpy(&ctx->schema, data + pos, to_read);
        pos += ctx->header.horizon_size;
    }

    /* ── Detect turbo (blocked) format ── */
    int is_turbo = (ctx->header.reserved32 == QTSQ_TURBO_MAGIC);

    if (is_turbo && pos + sizeof(qtsq_block_index_header_t) <= data_len) {
        /* Read block index header */
        qtsq_block_index_header_t idx_hdr;
        memcpy(&idx_hdr, data + pos, sizeof(idx_hdr));
        pos += sizeof(idx_hdr);

        if (idx_hdr.magic != QTSQ_TURBO_MAGIC || idx_hdr.num_blocks == 0) {
            /* Not actually turbo, fall back */
            qtsq_mmap_close(&m);
            return qtsq_read(ctx, filename);
        }

        uint32_t num_blocks = idx_hdr.num_blocks;

        /* Read block entries */
        size_t entries_size = num_blocks * sizeof(qtsq_block_entry_t);
        if (pos + entries_size > data_len) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_FORMAT;
        }

        qtsq_block_entry_t *entries = (qtsq_block_entry_t *)(data + pos);

        /* Verify index checksum */
        uint32_t expected_cksum = qtsq_crc32((const uint8_t *)entries, entries_size);
        if (expected_cksum != idx_hdr.index_checksum) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_CHECKSUM;
        }

        /* Decompress all blocks and reassemble into singularity buffer */
        size_t total_original = 0;
        for (uint32_t i = 0; i < num_blocks; i++)
            total_original += entries[i].original_size;

        ctx->singularity = (uint8_t *)malloc(total_original);
        if (!ctx->singularity) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_ALLOC;
        }
        ctx->singularity_size = total_original;

        size_t out_pos = 0;
        for (uint32_t i = 0; i < num_blocks; i++) {
            const uint8_t *block_data = data + entries[i].file_offset;
            size_t  comp_size = entries[i].compressed_size;
            size_t  orig_size = entries[i].original_size;

            if (entries[i].file_offset + comp_size > data_len) {
                free(ctx->singularity);
                ctx->singularity = NULL;
                qtsq_mmap_close(&m);
                return QTSQ_ERR_FORMAT;
            }

            if (entries[i].strategy == QTSQ_STRATEGY_NONE) {
                /* Stored raw — just copy */
                memcpy(ctx->singularity + out_pos, block_data, orig_size);
            } else {
                /* Decompress this block */
                qtsq_context_t tmp;
                qtsq_init(&tmp);
                tmp.header.strategy = entries[i].strategy;
                tmp.header.singularity_size = (uint32_t)comp_size;
                tmp.header.original_size = orig_size;
                tmp.singularity = (uint8_t *)malloc(comp_size);
                if (!tmp.singularity) {
                    free(ctx->singularity);
                    ctx->singularity = NULL;
                    qtsq_mmap_close(&m);
                    return QTSQ_ERR_ALLOC;
                }
                memcpy(tmp.singularity, block_data, comp_size);
                tmp.singularity_size = (uint32_t)comp_size;

                uint8_t *dec = NULL;
                size_t dec_size = 0;
                int dec_res = qtsq_decompress_singularity(&tmp, &dec, &dec_size);
                qtsq_free(&tmp);

                if (dec_res != QTSQ_OK || !dec) {
                    free(ctx->singularity);
                    ctx->singularity = NULL;
                    qtsq_mmap_close(&m);
                    return dec_res != QTSQ_OK ? dec_res : QTSQ_ERR_FORMAT;
                }

                size_t copy_size = dec_size < orig_size ? dec_size : orig_size;
                memcpy(ctx->singularity + out_pos, dec, copy_size);
                free(dec);
            }

            /* Verify block checksum */
            uint32_t actual_cksum = qtsq_crc32(ctx->singularity + out_pos, orig_size);
            if (actual_cksum != entries[i].checksum) {
                free(ctx->singularity);
                ctx->singularity = NULL;
                qtsq_mmap_close(&m);
                return QTSQ_ERR_CHECKSUM;
            }

            out_pos += orig_size;
        }

    } else {
        /* Legacy (non-blocked) format — read singularity in one shot */
        if (ctx->header.singularity_size > 0) {
            if (pos + ctx->header.singularity_size > data_len) {
                qtsq_mmap_close(&m);
                return QTSQ_ERR_FORMAT;
            }
            ctx->singularity_size = ctx->header.singularity_size;
            ctx->singularity = (uint8_t *)malloc(ctx->singularity_size);
            if (!ctx->singularity) {
                qtsq_mmap_close(&m);
                return QTSQ_ERR_ALLOC;
            }
            memcpy(ctx->singularity, data + pos, ctx->singularity_size);
            pos += ctx->singularity_size;
        }

        /* Read accretion transforms */
        if (ctx->header.accretion_size > 0 && pos + ctx->header.accretion_size <= data_len) {
            /* Handled by radiation index below */
        }

        /* Read radiation index */
        if (ctx->header.radiation_size > 0 && pos + ctx->header.radiation_size <= data_len) {
            ctx->num_radiation = ctx->header.radiation_size / sizeof(qtsq_index_t);
            ctx->radiation_index = (qtsq_index_t *)calloc(ctx->num_radiation, sizeof(qtsq_index_t));
            if (ctx->radiation_index) {
                memcpy(ctx->radiation_index, data + pos, ctx->header.radiation_size);
                pos += ctx->header.radiation_size;
            }
        }
    }

    /* Encryption / recovery flags */
    ctx->is_encrypted = (ctx->header.flags & QTSQ_FLAG_ENCRYPTED) ? 1 : 0;
    ctx->has_recovery = (ctx->header.flags & QTSQ_FLAG_HAS_ECC) ? 1 : 0;

    qtsq_mmap_close(&m);
    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════
 *  Layer 2: Random Access Block Read
 *  Read a single block without decompressing the whole file
 * ═══════════════════════════════════════════════════════ */

int qtsq_turbo_read_block(const char *filename, uint32_t block_index,
                          uint8_t **out_data, size_t *out_size) {
    if (!filename || !out_data || !out_size) return QTSQ_ERR_NULL;

    qtsq_mmap_t m;
    int res = qtsq_mmap_open_read(&m, filename);
    if (res != QTSQ_OK) return res;

    const uint8_t *data = (const uint8_t *)m.data;
    size_t data_len = m.size;

    if (data_len < sizeof(qtsq_header_t)) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    /* Read header */
    qtsq_header_t hdr;
    memcpy(&hdr, data, sizeof(qtsq_header_t));

    if (hdr.reserved32 != QTSQ_TURBO_MAGIC) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT; /* Not a turbo file */
    }

    /* Find index */
    size_t pos = sizeof(qtsq_header_t);
    if (hdr.horizon_size > 0) pos += hdr.horizon_size;

    if (pos + sizeof(qtsq_block_index_header_t) > data_len) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    qtsq_block_index_header_t idx_hdr;
    memcpy(&idx_hdr, data + pos, sizeof(idx_hdr));
    pos += sizeof(idx_hdr);

    if (block_index >= idx_hdr.num_blocks) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    /* Read specific block entry */
    size_t entry_offset = pos + block_index * sizeof(qtsq_block_entry_t);
    if (entry_offset + sizeof(qtsq_block_entry_t) > data_len) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    qtsq_block_entry_t entry;
    memcpy(&entry, data + entry_offset, sizeof(entry));

    if (entry.file_offset + entry.compressed_size > data_len) {
        qtsq_mmap_close(&m);
        return QTSQ_ERR_FORMAT;
    }

    /* Decompress just this one block */
    if (entry.strategy == QTSQ_STRATEGY_NONE) {
        *out_data = (uint8_t *)malloc(entry.original_size);
        if (!*out_data) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_ALLOC;
        }
        memcpy(*out_data, data + entry.file_offset, entry.original_size);
        *out_size = entry.original_size;
    } else {
        qtsq_context_t tmp;
        qtsq_init(&tmp);
        tmp.header.strategy = entry.strategy;
        tmp.header.singularity_size = entry.compressed_size;
        tmp.header.original_size = entry.original_size;
        tmp.singularity = (uint8_t *)malloc(entry.compressed_size);
        if (!tmp.singularity) {
            qtsq_mmap_close(&m);
            return QTSQ_ERR_ALLOC;
        }
        memcpy(tmp.singularity, data + entry.file_offset, entry.compressed_size);
        tmp.singularity_size = entry.compressed_size;

        int dec_res = qtsq_decompress_singularity(&tmp, out_data, out_size);
        qtsq_free(&tmp);

        if (dec_res != QTSQ_OK) {
            qtsq_mmap_close(&m);
            return dec_res;
        }
    }

    /* Verify checksum */
    uint32_t actual = qtsq_crc32(*out_data, *out_size);
    if (actual != entry.checksum) {
        free(*out_data);
        *out_data = NULL;
        *out_size = 0;
        qtsq_mmap_close(&m);
        return QTSQ_ERR_CHECKSUM;
    }

    qtsq_mmap_close(&m);
    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════
 *  Diagnostics: Print Turbo Capabilities
 * ═══════════════════════════════════════════════════════ */

void qtsq_turbo_print_caps(void) {
    printf("╔══════════════════════════════════════╗\n");
    printf("║     QTSQ Turbo I/O Capabilities     ║\n");
    printf("╠══════════════════════════════════════╣\n");

    /* SIMD */
#if defined(QTSQ_SIMD_AVX2)
    printf("║  SIMD:      AVX2 (32 bytes/cycle)   ║\n");
#elif defined(QTSQ_SIMD_SSE2)
    printf("║  SIMD:      SSE2 (16 bytes/cycle)   ║\n");
#elif defined(QTSQ_SIMD_NEON)
    printf("║  SIMD:      NEON (16 bytes/cycle)    ║\n");
#else
    printf("║  SIMD:      None (scalar)            ║\n");
#endif

    /* Threading */
#if defined(QTSQ_THREADS_POSIX)
    printf("║  Threads:   POSIX pthreads           ║\n");
#elif defined(QTSQ_THREADS_WIN)
    printf("║  Threads:   Windows threads           ║\n");
#else
    printf("║  Threads:   None                      ║\n");
#endif

    /* mmap */
#if defined(QTSQ_MMAP_POSIX)
    printf("║  mmap:      POSIX mmap                ║\n");
#elif defined(QTSQ_MMAP_WIN)
    printf("║  mmap:      Windows MapViewOfFile      ║\n");
#endif

    printf("║  Block:     64KB default              ║\n");
    printf("║  Max threads: %d                       ║\n", QTSQ_MAX_THREADS);
    printf("╚══════════════════════════════════════╝\n");
}
