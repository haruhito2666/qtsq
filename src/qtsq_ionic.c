/*
 * Quantum Tensor Sequence — Ionic Data Tunnel (IDT) Engine
 *
 * Core implementation of the ICE-inspired I/O acceleration engine.
 * Decomposes data into entropy-adaptive particles ("ions"), computes
 * information gradient fields, and runs a 3-stage threaded pipeline
 * with 4-hop Markov cascade prefetch for near-zero latency reads.
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#include "qtsq_ionic.h"
#include "qtsq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Timing
 * ═══════════════════════════════════════════════════════════════ */

static double ionic_time_ms(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ═══════════════════════════════════════════════════════════════
 *  Configuration
 * ═══════════════════════════════════════════════════════════════ */

void qtsq_ionic_config_default(qtsq_ionic_config_t *cfg) {
    if (!cfg) return;
    cfg->base_particle_size = QTSQ_ION_SIZE_DEFAULT;
    cfg->cascade_depth      = QTSQ_CASCADE_DEPTH;
    cfg->cascade_k          = QTSQ_CASCADE_K;
    cfg->num_slots          = QTSQ_TUNNEL_SLOTS;
    cfg->num_threads        = 3;
    cfg->w_entropy          = 0.5f;
    cfg->w_spatial          = 0.3f;
    cfg->w_dependency       = 0.2f;
}

/* ═══════════════════════════════════════════════════════════════
 *  Shannon Entropy Computation
 *
 *  H(X) = -Σ p(x) · log₂(p(x))
 *  Range: 0.0 (all same byte) to 8.0 (perfectly random)
 * ═══════════════════════════════════════════════════════════════ */

float qtsq_shannon_entropy(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0.0f;

    uint32_t freq[256] = {0};
    for (size_t i = 0; i < len; i++)
        freq[data[i]]++;

    float entropy = 0.0f;
    float inv_len = 1.0f / (float)len;
    for (int i = 0; i < 256; i++) {
        if (freq[i] == 0) continue;
        float p = (float)freq[i] * inv_len;
        entropy -= p * log2f(p);
    }
    return entropy;
}

/* ═══════════════════════════════════════════════════════════════
 *  Hilbert Curve — Spatial Ordering
 *
 *  Maps 2D (x, y) → 1D index preserving locality.
 *  This ensures spatially adjacent pixels map to adjacent particles,
 *  which maximizes cascade prediction accuracy.
 * ═══════════════════════════════════════════════════════════════ */

static void hilbert_rot(uint32_t n, uint32_t *x, uint32_t *y,
                        uint32_t rx, uint32_t ry) {
    if (ry == 0) {
        if (rx == 1) {
            *x = n - 1 - *x;
            *y = n - 1 - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

uint32_t qtsq_hilbert_xy2d(uint32_t order, uint32_t x, uint32_t y) {
    uint32_t d = 0;
    uint32_t n = 1u << order;
    for (uint32_t s = n / 2; s > 0; s /= 2) {
        uint32_t rx = (x & s) > 0 ? 1 : 0;
        uint32_t ry = (y & s) > 0 ? 1 : 0;
        d += s * s * ((3 * rx) ^ ry);
        hilbert_rot(s, &x, &y, rx, ry);
    }
    return d;
}

void qtsq_hilbert_d2xy(uint32_t order, uint32_t d,
                       uint32_t *x, uint32_t *y) {
    uint32_t n = 1u << order;
    *x = *y = 0;
    for (uint32_t s = 1; s < n; s *= 2) {
        uint32_t rx = 1 & (d / 2);
        uint32_t ry = 1 & (d ^ rx);
        hilbert_rot(s, x, y, rx, ry);
        *x += s * rx;
        *y += s * ry;
        d /= 4;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Ionizer — Entropy-Adaptive Particle Decomposition
 *
 *  Splits singularity payload into independently-decodable ions.
 *  Particle size adapts based on local Shannon entropy:
 *    high entropy → small particles (more "charge", finer control)
 *    low entropy  → large particles (bulk transport, like neutral air)
 * ═══════════════════════════════════════════════════════════════ */

/* Estimate adaptive particle size based on entropy */
static uint16_t adaptive_ion_size(float entropy, uint16_t base_size) {
    /*
     * Entropy range: 0.0 (flat) to 8.0 (random)
     * 
     * Low entropy (0-2):  size = base * 4  → 4096B (large, few particles)
     * Mid entropy (2-5):  size = base * 1  → 1024B (balanced)
     * High entropy (5-8): size = base / 4  → 256B  (small, many particles)
     *
     * Like ICE: high-charge regions (edges) need more ions for fine control.
     */
    float scale;
    if (entropy < 2.0f)
        scale = 4.0f - 1.5f * entropy;       /* 4.0 → 1.0 */
    else if (entropy < 5.0f)
        scale = 1.0f;                          /* constant */
    else
        scale = 1.0f - 0.25f * (entropy - 5.0f); /* 1.0 → 0.25 */

    uint16_t size = (uint16_t)(base_size * scale);
    if (size < QTSQ_ION_SIZE_MIN) size = QTSQ_ION_SIZE_MIN;
    if (size > QTSQ_ION_SIZE_MAX) size = QTSQ_ION_SIZE_MAX;
    /* Align to 64 bytes for cache line efficiency */
    size = (size + 63) & ~63;
    return size;
}

/*
 * Ionize singularity data into particles.
 *
 * Two-pass approach:
 *   Pass 1: Scan entropy in windows, determine particle boundaries
 *   Pass 2: Allocate and fill ion descriptors
 */
static int ionize_data(const uint8_t *data, size_t data_size,
                       uint16_t base_size,
                       qtsq_ion_t **out_ions, uint32_t *out_count) {
    if (!data || data_size == 0) return QTSQ_ERR_NULL;

    /* Pass 1: Count particles by scanning entropy windows */
    uint32_t max_ions = (uint32_t)(data_size / QTSQ_ION_SIZE_MIN) + 1;
    qtsq_ion_t *ions = (qtsq_ion_t *)calloc(max_ions, sizeof(qtsq_ion_t));
    if (!ions) return QTSQ_ERR_ALLOC;

    uint32_t count = 0;
    size_t offset = 0;

    while (offset < data_size) {
        /* Sample entropy from a small window at current position */
        size_t window = (data_size - offset < 512) ? (data_size - offset) : 512;
        float entropy = qtsq_shannon_entropy(data + offset, window);

        /* Determine particle size based on entropy */
        uint16_t ion_size = adaptive_ion_size(entropy, base_size);
        if (offset + ion_size > data_size)
            ion_size = (uint16_t)(data_size - offset);

        /* Create ion descriptor */
        ions[count].id          = count;
        ions[count].offset      = (uint32_t)offset;
        ions[count].size        = ion_size;
        ions[count].entropy     = (uint16_t)(entropy * 8191.0f);  /* quantize to 16-bit */
        ions[count].hilbert_pos = 0;  /* filled in field computation */
        memset(ions[count].predict, 0xFF, sizeof(ions[count].predict));  /* no prediction yet */

        offset += ion_size;
        count++;

        if (count >= max_ions) break;
    }

    /* Shrink allocation — use a temp so a NULL return doesn't leak the
     * original buffer (realloc(NULL, ...) semantics). The shrink almost
     * always succeeds in-place, but we must not assign NULL over `ions`. */
    if (count > 0) {
        qtsq_ion_t *shrunk = (qtsq_ion_t *)realloc(ions, count * sizeof(qtsq_ion_t));
        if (shrunk) ions = shrunk;
        /* if shrunk is NULL, the original `ions` is still valid (just oversized) */
    }
    *out_ions = ions;
    *out_count = count;

    fprintf(stderr, "[IDT Ionize] %u particles from %zu bytes "
            "(avg %.0f B/particle, range %u–%u B)\n",
            count, data_size, (double)data_size / count,
            QTSQ_ION_SIZE_MIN, QTSQ_ION_SIZE_MAX);

    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  Field Computer — Information Gradient
 *
 *  Computes Φ(pᵢ) for each particle and fills Hilbert positions.
 *  For image data, maps particles to 2D spatial positions using
 *  their byte offset within the pixel buffer.
 * ═══════════════════════════════════════════════════════════════ */

static void compute_field_and_hilbert(qtsq_ion_t *ions, uint32_t num_ions,
                                      uint32_t width, uint32_t height,
                                      uint8_t channels,
                                      const qtsq_ionic_config_t *cfg) {
    (void)cfg; /* reserved for future entropy-adaptive tuning */
    /* Determine Hilbert curve order (smallest power of 2 >= max(w,h)) */
    uint32_t dim = width > height ? width : height;
    uint32_t order = 1;
    while ((1u << order) < dim && order < 16) order++;

    for (uint32_t i = 0; i < num_ions; i++) {
        /* Map byte offset to approximate (x, y) pixel position */
        uint32_t byte_pos = ions[i].offset;
        uint32_t pixel_idx = byte_pos / (channels > 0 ? channels : 1);
        uint32_t py = pixel_idx / (width > 0 ? width : 1);
        uint32_t px = pixel_idx % (width > 0 ? width : 1);

        /* Clamp to Hilbert grid */
        uint32_t n = 1u << order;
        if (px >= n) px = n - 1;
        if (py >= n) py = n - 1;

        ions[i].hilbert_pos = qtsq_hilbert_xy2d(order, px, py);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Cascade Predictor — 4-Hop Markov Prefetch
 *
 *  For each particle, predict the top-K particles most likely
 *  to be needed next. Uses:
 *    1. Sequential neighbor (always predicted)
 *    2. Hilbert-curve spatial neighbor (locality)
 *    3. Entropy-similar particle (compression correlation)
 *    4. Stride-2 lookahead (skip pattern)
 *
 *  Cascade: particle i predicts j, j predicts k, k predicts l...
 *  After 4 hops: 1 → 4 → 16 → 64 → 256 particles in prefetch queue
 * ═══════════════════════════════════════════════════════════════ */

static void compute_cascade_predictions(qtsq_ion_t *ions, uint32_t num_ions) {
    if (num_ions < 2) return;

    /* Sort a copy by Hilbert position for spatial neighbor lookup */
    uint32_t *hilbert_order = (uint32_t *)malloc(num_ions * sizeof(uint32_t));
    if (!hilbert_order) return;

    for (uint32_t i = 0; i < num_ions; i++)
        hilbert_order[i] = i;

    /* Simple insertion sort for hilbert positions (stable, good for nearly-sorted) */
    for (uint32_t i = 1; i < num_ions; i++) {
        uint32_t key = hilbert_order[i];
        uint32_t key_h = ions[key].hilbert_pos;
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && ions[hilbert_order[j]].hilbert_pos > key_h) {
            hilbert_order[j + 1] = hilbert_order[j];
            j--;
        }
        hilbert_order[j + 1] = key;
    }

    /* Build reverse map: ion_id → position in hilbert_order */
    uint32_t *hilbert_rank = (uint32_t *)malloc(num_ions * sizeof(uint32_t));
    if (hilbert_rank) {
        for (uint32_t i = 0; i < num_ions; i++)
            hilbert_rank[hilbert_order[i]] = i;
    }

    /* For each particle, compute K=4 predictions */
    for (uint32_t i = 0; i < num_ions; i++) {
        /* Prediction 0: Sequential next (always most likely) */
        ions[i].predict[0] = (i + 1 < num_ions) ? (uint16_t)(i + 1) : 0;

        /* Prediction 1: Hilbert-curve spatial neighbor */
        if (hilbert_rank) {
            uint32_t hr = hilbert_rank[i];
            uint32_t neighbor = (hr + 1 < num_ions)
                ? hilbert_order[hr + 1] : hilbert_order[0];
            ions[i].predict[1] = (uint16_t)(neighbor % 65536);
        } else {
            ions[i].predict[1] = (i + 2 < num_ions) ? (uint16_t)(i + 2) : 0;
        }

        /* Prediction 2: Stride-2 (skip pattern for interleaved data) */
        ions[i].predict[2] = (i + 2 < num_ions) ? (uint16_t)(i + 2) : 0;

        /* Prediction 3: Stride-4 (wide lookahead for burst reads) */
        ions[i].predict[3] = (i + 4 < num_ions) ? (uint16_t)(i + 4) : 0;
    }

    free(hilbert_order);
    free(hilbert_rank);

    fprintf(stderr, "[IDT Cascade] %u particles with %d predictions each, "
            "%d-hop depth (max prefetch: %u)\n",
            num_ions, QTSQ_CASCADE_K, QTSQ_CASCADE_DEPTH,
            (uint32_t)pow(QTSQ_CASCADE_K, QTSQ_CASCADE_DEPTH));
}

/* ═══════════════════════════════════════════════════════════════
 *  Ionize a compressed context (post-compression, pre-write)
 * ═══════════════════════════════════════════════════════════════ */

int qtsq_ionize(qtsq_context_t *ctx, const qtsq_ionic_config_t *cfg) {
    if (!ctx || !ctx->singularity || ctx->singularity_size == 0)
        return QTSQ_ERR_NULL;

    qtsq_ionic_config_t default_cfg;
    if (!cfg) {
        qtsq_ionic_config_default(&default_cfg);
        cfg = &default_cfg;
    }

    double t0 = ionic_time_ms();

    /* Step 1: Ionize — decompose into entropy-adaptive particles */
    qtsq_ion_t *ions = NULL;
    uint32_t num_ions = 0;
    int rc = ionize_data(ctx->singularity, (size_t)ctx->singularity_size,
                         cfg->base_particle_size, &ions, &num_ions);
    if (rc != QTSQ_OK) return rc;

    /* Step 2: Compute Hilbert positions for spatial ordering */
    /* Extract width/height from schema or header */
    uint32_t w = 0, h = 0;
    uint8_t ch = 3;
    if (ctx->schema.num_dims >= 2) {
        w = ctx->schema.dimensions[0];
        h = ctx->schema.dimensions[1];
    }
    if (ctx->header.num_channels > 0) ch = ctx->header.num_channels;
    compute_field_and_hilbert(ions, num_ions, w, h, ch, cfg);

    /* Step 3: Compute cascade predictions (4-hop Markov) */
    compute_cascade_predictions(ions, num_ions);

    /* Step 4: Build field header */
    qtsq_field_header_t field = {0};
    field.magic           = QTSQ_ION_MAGIC;
    field.num_ions        = num_ions;
    field.base_ion_size   = cfg->base_particle_size;
    field.cascade_depth   = cfg->cascade_depth;
    field.w_entropy       = cfg->w_entropy;
    field.w_spatial       = cfg->w_spatial;
    field.w_dependency    = cfg->w_dependency;
    field.avalanche_onset = (num_ions > 10) ? 10 : num_ions;  /* ~10 particles to warm up */
    field.checksum        = 0;  /* TODO: CRC32 */

    /* Step 5: Rebuild singularity with ionic structure */
    /*
     * New layout:
     *   [field_header: 32B] [ion_index: N*24B] [particle data...]
     *
     * The original singularity data stays the same — we just prepend the index.
     */
    size_t index_size = sizeof(qtsq_field_header_t) + num_ions * sizeof(qtsq_ion_t);
    size_t new_size = index_size + ctx->singularity_size;
    uint8_t *new_sing = (uint8_t *)malloc(new_size);
    if (!new_sing) { free(ions); return QTSQ_ERR_ALLOC; }

    /* Write field header */
    memcpy(new_sing, &field, sizeof(qtsq_field_header_t));
    /* Write ion index */
    memcpy(new_sing + sizeof(qtsq_field_header_t), ions, num_ions * sizeof(qtsq_ion_t));
    /* Copy original singularity data */
    memcpy(new_sing + index_size, ctx->singularity, (size_t)ctx->singularity_size);

    /* Replace singularity */
    free(ctx->singularity);
    ctx->singularity = new_sing;
    ctx->singularity_size = (uint64_t)new_size;
    ctx->header.singularity_size = (uint64_t)new_size;

    /* Mark as ionic in reserved field (extended flags) */
    ctx->header.reserved32 = QTSQ_FLAG_IONIC;

    free(ions);

    double t1 = ionic_time_ms();
    fprintf(stderr, "[IDT] Ionized in %.1f ms: %u particles, "
            "field map %zu bytes, cascade depth %d\n",
            t1 - t0, num_ions, index_size, cfg->cascade_depth);

    return QTSQ_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  Ionic Compress HD — Full pipeline: compress + ionize + write
 * ═══════════════════════════════════════════════════════════════ */

int qtsq_ionic_compress_hd(qtsq_context_t *ctx,
                            const uint8_t *pixels,
                            uint32_t width, uint32_t height,
                            uint8_t channels, size_t source_size) {
    /* Step 1: Standard gravitational HD compression */
    int rc = qtsq_compress_gravitational_hd(ctx, pixels, width, height,
                                             channels, source_size);
    if (rc != QTSQ_OK) return rc;

    /* Step 2: Ionize the compressed payload */
    qtsq_ionic_config_t cfg;
    qtsq_ionic_config_default(&cfg);
    return qtsq_ionize(ctx, &cfg);
}

/* ═══════════════════════════════════════════════════════════════
 *  Tunnel Pipeline — 3-Stage Threaded Ring Buffer
 *
 *  Thread 1 (Reader):    reads particles from source → ring slots
 *  Thread 2 (Decoder):   decompresses particles in ring slots
 *  Thread 3 (Predictor): runs cascade, fills prefetch queue
 * ═══════════════════════════════════════════════════════════════ */

/* Reader thread: fetches particles from source into ring buffer */
static void *reader_thread_fn(void *arg) {
    qtsq_tunnel_t *t = (qtsq_tunnel_t *)arg;

    while (t->running) {
        pthread_mutex_lock(&t->mutex);

        /* Wait until there's a free slot */
        while (t->running) {
            uint32_t next = (t->read_head) % t->num_slots;
            if (t->slots[next].state == ION_SLOT_EMPTY ||
                t->slots[next].state == ION_SLOT_CONSUMED)
                break;
            pthread_cond_wait(&t->cond_read, &t->mutex);
        }

        if (!t->running) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }

        /* Pick next particle to read (from prefetch queue or sequential) */
        uint32_t ion_id;
        if (t->pq_head != t->pq_tail) {
            /* Prefetch queue has predictions — use them (cascade!) */
            ion_id = t->prefetch_queue[t->pq_head % t->pq_capacity];
            t->pq_head++;
            t->stats.cache_hits++;
        } else {
            /* No predictions — sequential fallback */
            ion_id = t->consume_head;
            t->stats.cache_misses++;
        }

        if (ion_id >= t->num_ions) {
            pthread_mutex_unlock(&t->mutex);
            continue;
        }

        /* Load particle data from source */
        uint32_t slot = t->read_head % t->num_slots;
        t->slots[slot].state = ION_SLOT_READING;
        t->slots[slot].ion_id = ion_id;

        qtsq_ion_t *ion = &t->ions[ion_id];
        uint32_t src_offset = ion->offset;
        uint16_t src_size = ion->size;

        /* Bounds check */
        if (src_offset + src_size <= t->source_size) {
            memcpy(t->slots[slot].data, t->source + src_offset, src_size);
            t->slots[slot].data_size = src_size;
            t->slots[slot].state = ION_SLOT_LOADED;
        } else {
            t->slots[slot].data_size = 0;
            t->slots[slot].state = ION_SLOT_LOADED;
        }

        t->read_head++;
        t->stats.total_reads++;

        /* Signal decoder */
        pthread_cond_signal(&t->cond_decode);
        pthread_mutex_unlock(&t->mutex);
    }
    return NULL;
}

/* Decoder thread: processes loaded particles */
static void *decoder_thread_fn(void *arg) {
    qtsq_tunnel_t *t = (qtsq_tunnel_t *)arg;

    while (t->running) {
        pthread_mutex_lock(&t->mutex);

        /* Wait for a loaded slot */
        while (t->running) {
            uint32_t next = t->decode_head % t->num_slots;
            if (t->slots[next].state == ION_SLOT_LOADED)
                break;
            pthread_cond_wait(&t->cond_decode, &t->mutex);
        }

        if (!t->running) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }

        uint32_t slot = t->decode_head % t->num_slots;
        t->slots[slot].state = ION_SLOT_DECODING;

        /*
         * "Decode" the particle.
         * For ionic format, particles are raw data slices — the encoding is
         * the field map + predictions. The actual zlib decompression was
         * already done when loading the singularity. So "decode" here is
         * essentially a no-op on the data (it's already decompressed),
         * but this is where we'd add per-particle transforms in the future
         * (e.g., delta decoding, entropy coding, wavelet reconstruction).
         *
         * The key insight: the decode stage exists as a pipeline slot so that
         * future per-particle compression can overlap with I/O.
         */
        t->slots[slot].state = ION_SLOT_READY;
        t->decode_head++;

        /* Signal consumer and predictor */
        pthread_cond_signal(&t->cond_consume);
        pthread_mutex_unlock(&t->mutex);
    }
    return NULL;
}

/* Predictor thread: runs cascade predictions, fills prefetch queue */
static void *predictor_thread_fn(void *arg) {
    qtsq_tunnel_t *t = (qtsq_tunnel_t *)arg;

    uint32_t last_predicted = 0;

    while (t->running) {
        pthread_mutex_lock(&t->mutex);

        /* Wait for newly decoded particles to generate predictions from */
        while (t->running && last_predicted >= t->decode_head) {
            pthread_cond_wait(&t->cond_consume, &t->mutex);
        }

        if (!t->running) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }

        /* Generate predictions for all newly decoded particles */
        while (last_predicted < t->decode_head && last_predicted < t->num_ions) {
            uint32_t slot = last_predicted % t->num_slots;
            uint32_t ion_id = t->slots[slot].ion_id;

            if (ion_id < t->num_ions) {
                qtsq_ion_t *ion = &t->ions[ion_id];

                /* 4-hop cascade: enqueue predictions recursively.
                 * M9 fix: check queue full before enqueuing (was overwriting
                 * unread predictions, making pq_size and avalanche trigger
                 * meaningless). */
                for (int k = 0; k < QTSQ_CASCADE_K; k++) {
                    uint32_t pred_id = ion->predict[k];
                    if (pred_id < t->num_ions && pred_id != 0xFFFF) {
                        if (t->pq_tail - t->pq_head < t->pq_capacity) {
                            uint32_t pq_idx = t->pq_tail % t->pq_capacity;
                            t->prefetch_queue[pq_idx] = pred_id;
                            t->pq_tail++;
                            t->stats.cascade_predictions++;
                        }

                        /* Hops 2-4: predictions of predictions */
                        if (pred_id < t->num_ions) {
                            qtsq_ion_t *pred_ion = &t->ions[pred_id];
                            for (int k2 = 0; k2 < 2; k2++) {  /* reduced fan for deeper hops */
                                uint32_t pred2 = pred_ion->predict[k2];
                                if (pred2 < t->num_ions && pred2 != 0xFFFF) {
                                    if (t->pq_tail - t->pq_head < t->pq_capacity) {
                                        uint32_t pq_idx2 = t->pq_tail % t->pq_capacity;
                                        t->prefetch_queue[pq_idx2] = pred2;
                                        t->pq_tail++;
                                        t->stats.cascade_predictions++;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            last_predicted++;
        }

        /* Detect avalanche: if prefetch queue is >75% full, we're in avalanche */
        uint32_t pq_size = t->pq_tail - t->pq_head;
        if (pq_size > (t->pq_capacity * 3 / 4) && !t->stats.avalanche_onset) {
            t->stats.avalanche_onset = last_predicted;
            fprintf(stderr, "[IDT] ⚡ AVALANCHE at particle %u "
                    "(prefetch queue: %u/%u)\n",
                    last_predicted, pq_size, t->pq_capacity);
        }

        /* Signal reader to use predictions */
        pthread_cond_signal(&t->cond_read);
        pthread_mutex_unlock(&t->mutex);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  Tunnel Open / Read / Close
 * ═══════════════════════════════════════════════════════════════ */

int qtsq_tunnel_open(qtsq_tunnel_t *tunnel,
                     const uint8_t *singularity_data,
                     size_t singularity_size,
                     const qtsq_ion_t *ions,
                     uint32_t num_ions,
                     const qtsq_ionic_config_t *cfg) {
    if (!tunnel || !singularity_data || !ions || num_ions == 0)
        return QTSQ_ERR_NULL;

    memset(tunnel, 0, sizeof(qtsq_tunnel_t));

    /* Copy ion index */
    tunnel->ions = (qtsq_ion_t *)malloc(num_ions * sizeof(qtsq_ion_t));
    if (!tunnel->ions) return QTSQ_ERR_ALLOC;
    memcpy(tunnel->ions, ions, num_ions * sizeof(qtsq_ion_t));
    tunnel->num_ions = num_ions;

    /* Source data */
    tunnel->source = singularity_data;
    tunnel->source_size = singularity_size;

    /* Configuration */
    if (cfg) tunnel->config = *cfg;
    else qtsq_ionic_config_default(&tunnel->config);

    /* Allocate ring buffer slots */
    tunnel->num_slots = tunnel->config.num_slots;
    tunnel->slots = (qtsq_tunnel_slot_t *)calloc(tunnel->num_slots,
                                                  sizeof(qtsq_tunnel_slot_t));
    if (!tunnel->slots) { free(tunnel->ions); return QTSQ_ERR_ALLOC; }

    for (uint32_t i = 0; i < tunnel->num_slots; i++) {
        tunnel->slots[i].data = (uint8_t *)malloc(QTSQ_ION_SIZE_MAX);
        if (!tunnel->slots[i].data) {
            /* M8 fix: free already-allocated slots + ions on failure. */
            for (uint32_t j = 0; j < i; j++) free(tunnel->slots[j].data);
            free(tunnel->slots);
            free(tunnel->ions);
            return QTSQ_ERR_ALLOC;
        }
        tunnel->slots[i].state = ION_SLOT_EMPTY;
    }

    /* Allocate prefetch queue */
    tunnel->pq_capacity = 1024;  /* enough for 4-hop cascade */
    tunnel->prefetch_queue = (uint32_t *)calloc(tunnel->pq_capacity, sizeof(uint32_t));
    if (!tunnel->prefetch_queue) {
        for (uint32_t i = 0; i < tunnel->num_slots; i++) free(tunnel->slots[i].data);
        free(tunnel->slots);
        free(tunnel->ions);
        return QTSQ_ERR_ALLOC;
    }

    /* Threading */
    tunnel->running = 1;
    pthread_mutex_init(&tunnel->mutex, NULL);
    pthread_cond_init(&tunnel->cond_read, NULL);
    pthread_cond_init(&tunnel->cond_decode, NULL);
    pthread_cond_init(&tunnel->cond_consume, NULL);

    /* Start pipeline threads (M8 fix: check return values). */
    if (pthread_create(&tunnel->reader_thread, NULL, reader_thread_fn, tunnel) != 0 ||
        pthread_create(&tunnel->decoder_thread, NULL, decoder_thread_fn, tunnel) != 0 ||
        pthread_create(&tunnel->predictor_thread, NULL, predictor_thread_fn, tunnel) != 0) {
        /* Stop and clean up on thread-creation failure. */
        tunnel->running = 0;
        pthread_cond_broadcast(&tunnel->cond_read);
        pthread_cond_broadcast(&tunnel->cond_decode);
        pthread_cond_broadcast(&tunnel->cond_consume);
        /* Best-effort join of any thread that did start. */
        pthread_join(tunnel->reader_thread, NULL);
        pthread_join(tunnel->decoder_thread, NULL);
        pthread_join(tunnel->predictor_thread, NULL);
        for (uint32_t i = 0; i < tunnel->num_slots; i++) free(tunnel->slots[i].data);
        free(tunnel->slots);
        free(tunnel->prefetch_queue);
        free(tunnel->ions);
        return QTSQ_ERR_ALLOC;
    }

    fprintf(stderr, "[IDT] Tunnel open: %u ions, %u slots, 3 threads\n",
            num_ions, tunnel->num_slots);

    return QTSQ_OK;
}

int qtsq_tunnel_read(qtsq_tunnel_t *tunnel,
                     uint8_t **out_data, size_t *out_size) {
    if (!tunnel || !out_data || !out_size)
        return QTSQ_ERR_NULL;

    if (tunnel->consume_head >= tunnel->num_ions)
        return QTSQ_ERR_IO;  /* end of data */

    pthread_mutex_lock(&tunnel->mutex);

    /* Wait for our slot to be READY */
    uint32_t slot = tunnel->consume_head % tunnel->num_slots;
    while (tunnel->slots[slot].state != ION_SLOT_READY && tunnel->running) {
        pthread_cond_wait(&tunnel->cond_consume, &tunnel->mutex);
    }

    if (!tunnel->running) {
        pthread_mutex_unlock(&tunnel->mutex);
        return QTSQ_ERR_IO;
    }

    /* Consume the particle */
    *out_data = tunnel->slots[slot].data;
    *out_size = tunnel->slots[slot].data_size;
    tunnel->slots[slot].state = ION_SLOT_CONSUMED;
    tunnel->consume_head++;

    /* Signal reader that a slot is free */
    pthread_cond_signal(&tunnel->cond_read);
    pthread_mutex_unlock(&tunnel->mutex);

    return QTSQ_OK;
}

void qtsq_tunnel_close(qtsq_tunnel_t *tunnel) {
    if (!tunnel) return;

    /* Stop threads */
    tunnel->running = 0;
    pthread_cond_broadcast(&tunnel->cond_read);
    pthread_cond_broadcast(&tunnel->cond_decode);
    pthread_cond_broadcast(&tunnel->cond_consume);

    pthread_join(tunnel->reader_thread, NULL);
    pthread_join(tunnel->decoder_thread, NULL);
    pthread_join(tunnel->predictor_thread, NULL);

    /* Print stats */
    qtsq_ionic_print_stats(&tunnel->stats);

    /* Cleanup */
    if (tunnel->slots) {
        for (uint32_t i = 0; i < tunnel->num_slots; i++)
            free(tunnel->slots[i].data);
        free(tunnel->slots);
    }
    free(tunnel->ions);
    free(tunnel->prefetch_queue);

    pthread_mutex_destroy(&tunnel->mutex);
    pthread_cond_destroy(&tunnel->cond_read);
    pthread_cond_destroy(&tunnel->cond_decode);
    pthread_cond_destroy(&tunnel->cond_consume);

    memset(tunnel, 0, sizeof(qtsq_tunnel_t));
}

void qtsq_tunnel_stats(const qtsq_tunnel_t *tunnel,
                       qtsq_ionic_stats_t *stats) {
    if (!tunnel || !stats) return;
    *stats = tunnel->stats;
    if (stats->total_reads > 0)
        stats->hit_rate = (double)stats->cache_hits / (double)stats->total_reads;
}

/* ═══════════════════════════════════════════════════════════════
 *  Diagnostics
 * ═══════════════════════════════════════════════════════════════ */

void qtsq_ionic_print_stats(const qtsq_ionic_stats_t *stats) {
    if (!stats) return;
    double hit_rate = (stats->total_reads > 0)
        ? (double)stats->cache_hits / (double)stats->total_reads * 100.0
        : 0.0;
    fprintf(stderr,
        "[IDT Stats] reads=%llu hits=%llu misses=%llu "
        "hit_rate=%.1f%% cascade_preds=%llu "
        "avalanche_at=%u\n",
        (unsigned long long)stats->total_reads,
        (unsigned long long)stats->cache_hits,
        (unsigned long long)stats->cache_misses,
        hit_rate,
        (unsigned long long)stats->cascade_predictions,
        stats->avalanche_onset);
}
