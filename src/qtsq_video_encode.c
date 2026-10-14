#include "qtsq.h"
#include "qtsq_video_mv.h"
#include "qtsq_video_res.h"
#include "qtc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "qtsq_parallel.h"
#include "qtsq_codec.h"
#include <zlib.h>

#ifdef __APPLE__
#include "qtsq_metal.h"
#endif

#define QTSQ_VIDEO_BATCH_MAX 32

/* ── Pixel Video (mp4-like) Configuration ── */
#define PX_MB_SIZE              16      /* Macroblock size (like H.264) */
#define PX_MV_RANGE             16      /* Motion search range ±pixels */
#define PX_SKIP_SAD_THRESH      8       /* Avg SAD below this → skip block */
#define PX_DEFAULT_GOP          0       /* 0 = use fps (1-second GOP) */

typedef struct {
    qtsq_video_encoder_t *enc;
    uint8_t *raw_pixels;
    int frame_index;
    int ret_code;
    uint8_t *compressed_data;
    size_t compressed_size;
    uint8_t frame_type;
    uint8_t *delta_buffer;
    float change_ratio;
} batch_job_t;

struct qtsq_video_encoder {
    qtsq_context_t *ctx;
    uint32_t width;
    uint32_t height;
    uint8_t channels;
    uint32_t fps;
    int strategy_flags;
    size_t target_payload_per_frame;

    uint32_t frame_count;     /* Total frames encoded so far */

    uint8_t *bitstream;
    size_t bitstream_size;

    /* Optional audio track, Classic Gravitational. Stored in its own
     * container section so old IDX1 readers still work. */
    uint8_t *audio_blob;
    size_t audio_blob_size;
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    size_t bitstream_capacity;

    uint32_t *frame_offsets;
    uint32_t *frame_sizes;
    uint8_t  *frame_types;
    size_t index_capacity;

    batch_job_t batch[QTSQ_VIDEO_BATCH_MAX];
    int batch_count;
    int batch_capacity;

    /* ── Pixel video path state (mp4-like GOP) ── */
    int      is_pixel_mode;       /* 1 when QTSQ_VIDEO_PIXEL flag set */
    uint32_t gop_size;            /* Keyframe interval in frames */
    uint32_t frames_since_keyframe;
    uint8_t *px_ref_frame;        /* Previous frame for motion estimation */
    int      px_has_ref;

    /* ── Lossy pixel path (ctx->quality > 0): custom spatial quant ── */
    int      lossy;               /* 1 when quality > 0 (pixel mode only) */
    float    quality;             /* 0.01..0.95, rate-control may adapt */
    float    quality_boot;        /* quality after bootstrap — RC may not exceed this mid-GOP */
    float    quality_max;         /* user's requested tier quality: the RC's ceiling */
    float    last_psnr;           /* closed-loop recon PSNR of the previous frame */
    int      psnr_hold;           /* 1 → RC must not lower quality (last frame under floor) */
    float    rc_q_good;           /* quality at the last frame that cleared the PSNR floor */
    int      quant;               /* residual step (>=2 for int8 residual) */
    size_t   rc_bytes;            /* cumulative encoded bytes (rate control) */
    size_t   rc_frames;           /* frames fed to rate control */
    /* Sliding window (~2 s). The cumulative ratio is poisoned for the rest of
     * the encode by a single large keyframe, which pins quality at the floor
     * and leaves most frames under the PSNR floor. A window lets the
     * controller recover once the expensive frame has passed. */
    size_t  *rc_win;
    uint32_t rc_win_cap;
    uint32_t rc_win_pos;
    uint32_t rc_win_fill;
    uint8_t *px_recon;            /* closed-loop reconstruction buffer */

    /* ── Reusable P-frame scratch (allocated once, not per-frame) ──
     * 4K: residual alone is ~25 MB; malloc/free 5809× fragments the heap
     * and spikes RSS. Keep them for the encoder lifetime. */
    uint8_t *px_scratch_modes;
    int8_t  *px_scratch_mvs;
    size_t  *px_scratch_res_off;
    uint8_t *px_scratch_residuals;
    size_t   px_scratch_residual_cap; /* bytes in px_scratch_residuals */
    size_t   px_scratch_nmb;

    /* Hard ceiling on the in-memory bitstream (bytes). Exceeding it
     * returns QTSQ_ERR_TOO_LARGE instead of OOM-killing the machine. */
    size_t   bitstream_limit;
};

/* Quality → residual/I-frame quant step. Mid-tread uniform quant, so max
 * sample error is Q/2. Tuned so Studio tiers stay watchable:
 *   High  q=0.70 → Q≈6   (max err ±3, ~near-transparent)
 *   Med   q=0.40 → Q≈17  (max err ±8, mild banding only in flat gradients)
 *   Low   q=0.20 → Q≈36  (usable low tier, not posterized art)
 * Old curve hit Q=21/39/52 at those tiers and rate-control then multiplied
 * quality down to Q≈79–114 (3–4 levels/channel) — the flat-color look. */
static int px_quant_from_quality(float q) {
    if (q <= 0.0f) return 128;
    if (q > 1.0f) q = 1.0f;
    int Q;
    if (q >= 0.60f) {
        /* High: Q 2..8 */
        float t = (0.95f - q) / 0.35f;              /* q=0.95→0, q=0.60→1 */
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        Q = 2 + (int)(t * 6.0f + 0.5f);
    } else if (q >= 0.25f) {
        /* Medium: Q 8..24 */
        float t = (0.60f - q) / 0.35f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        Q = 8 + (int)(t * 16.0f + 0.5f);
    } else if (q >= 0.08f) {
        /* Low: Q 24..64 */
        float t = (0.25f - q) / 0.17f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        Q = 24 + (int)(t * 40.0f + 0.5f);
    } else {
        /* Extreme: Q 64..128 (only if RC is desperate) */
        float t = (0.08f - q) / 0.08f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        Q = 64 + (int)(t * 64.0f + 0.5f);
    }
    if (Q < 2) Q = 2;
    if (Q > 128) Q = 128;
    return Q;
}


/* Hard ceiling on enc->bitstream growth so a long 4K encode cannot exhaust
 * RAM (Mac force-quit). Override: QTSQ_BITSTREAM_MB=NN (1..4096), further
 * capped by the shared 5 GB working-set budget once the per-frame scratch is
 * subtracted — the bitstream is only part of the encoder's footprint. */
static size_t px_bitstream_limit_bytes(size_t working_set) {
    const char *e = getenv("QTSQ_BITSTREAM_MB");
    long mb = 1024; /* default 1 GB — enough for quality-floor 4K near source size */
    if (e && *e) {
        mb = strtol(e, NULL, 10);
        if (mb < 1) mb = 1;
        if (mb > 4096) mb = 4096;
    }
    size_t lim = (size_t)mb * 1024u * 1024u;
    size_t total = qtsq_memory_limit_bytes();
    size_t room = (total > working_set) ? (total - working_set) : 0;
    if (room < (16u << 20)) room = 16u << 20;
    return lim < room ? lim : room;
}

static int append_to_bitstream(qtsq_video_encoder_t *enc, const uint8_t *data, size_t size) {
    if (enc->bitstream_size + size > enc->bitstream_limit)
        return QTSQ_ERR_TOO_LARGE;
    if (enc->bitstream_size + size > enc->bitstream_capacity) {
        size_t new_cap = enc->bitstream_capacity * 2;
        if (new_cap < enc->bitstream_size + size + 1024 * 1024) {
            new_cap = enc->bitstream_size + size + 1024 * 1024;
        }
        if (new_cap > enc->bitstream_limit) new_cap = enc->bitstream_limit;
        if (new_cap < enc->bitstream_size + size) return QTSQ_ERR_TOO_LARGE;
        uint8_t *new_buf = (uint8_t *)realloc(enc->bitstream, new_cap);
        if (!new_buf) return QTSQ_ERR_ALLOC;
        enc->bitstream = new_buf;
        enc->bitstream_capacity = new_cap;
    }
    memcpy(enc->bitstream + enc->bitstream_size, data, size);
    enc->bitstream_size += size;
    return QTSQ_OK;
}

static int append_to_index(qtsq_video_encoder_t *enc, uint32_t offset, uint32_t size, uint8_t type) {
    if (enc->frame_count >= enc->index_capacity) {
        size_t new_cap = enc->index_capacity == 0 ? 600 : enc->index_capacity * 2;
        /* Use temp pointers so a partial failure doesn't leave dangling
         * pointers (a successful realloc frees the old buffer; assigning it
         * back and then failing on the next leaves a freed pointer in enc). */
        uint32_t *new_offsets = (uint32_t *)realloc(enc->frame_offsets, new_cap * sizeof(uint32_t));
        if (!new_offsets) return QTSQ_ERR_ALLOC;
        enc->frame_offsets = new_offsets;
        uint32_t *new_sizes = (uint32_t *)realloc(enc->frame_sizes, new_cap * sizeof(uint32_t));
        if (!new_sizes) return QTSQ_ERR_ALLOC;
        enc->frame_sizes = new_sizes;
        uint8_t *new_types = (uint8_t *)realloc(enc->frame_types, new_cap * sizeof(uint8_t));
        if (!new_types) return QTSQ_ERR_ALLOC;
        enc->frame_types = new_types;
        enc->index_capacity = new_cap;
    }
    enc->frame_offsets[enc->frame_count] = offset;
    enc->frame_sizes[enc->frame_count] = size;
    enc->frame_types[enc->frame_count] = type;
    enc->frame_count++;
    return QTSQ_OK;
}

qtsq_video_encoder_t* qtsq_video_encoder_create(qtsq_context_t *ctx,
                                                uint32_t width, uint32_t height,
                                                uint8_t channels, uint32_t fps,
                                                int strategy_flags,
                                                size_t target_payload_per_frame) {
    if (!ctx) return NULL;
    qtsq_video_encoder_t *enc = (qtsq_video_encoder_t *)calloc(1, sizeof(qtsq_video_encoder_t));
    if (!enc) return NULL;

    enc->ctx = ctx;
    enc->width = width;
    enc->height = height;
    enc->channels = channels;
    enc->fps = fps > 0 ? fps : 24;
    enc->strategy_flags = strategy_flags;
    enc->target_payload_per_frame = target_payload_per_frame;

    /* Pixel mode: mp4-like GOP + motion compensation.
     * GOP interval: high byte of strategy_flags may override (e.g.
     * QTSQ_VIDEO_PIXEL | (30u << 24)); default = fps (1-second GOP). */
    enc->is_pixel_mode = (strategy_flags & QTSQ_VIDEO_PIXEL) != 0;
    enc->gop_size = enc->fps; /* default: 1-second GOP, like typical mp4 */
    if (enc->is_pixel_mode) {
        uint32_t gop_override = ((uint32_t)strategy_flags & 0xFF000000u) >> 24;
        if (gop_override > 0) enc->gop_size = gop_override;
        if (enc->gop_size < 1) enc->gop_size = 1;
    }
    enc->frames_since_keyframe = 0;
    enc->px_has_ref = 0;
    /* Lossy pixel mode: ctx->quality > 0 (same convention as the image
     * pixel path). */
    enc->lossy = 0;
    enc->quality = 0.0f;
    enc->quality_boot = 0.0f;
    enc->quality_max = 0.0f;
    enc->last_psnr = 99.0f;
    enc->psnr_hold = 0;
    enc->rc_q_good = 0.0f;
    enc->quant = 1;
    if (enc->is_pixel_mode && ctx->quality > 0.0f) {
        enc->lossy = 1;
        enc->quality = ctx->quality;
        if (enc->quality < 0.15f) enc->quality = 0.15f;
        if (enc->quality > 0.95f) enc->quality = 0.95f;
        enc->quality_boot = enc->quality;
        /* The tier the user picked is the ceiling. quality_boot is only the
         * bootstrap's conservative *starting* point; using it as the ceiling
         * pinned quality low even with byte budget to spare (measured: under
         * budget at 0.91 while stuck at 26 dB). */
        enc->quality_max = enc->quality;
        enc->quant = px_quant_from_quality(enc->quality);
    }
    if (enc->is_pixel_mode) {
        size_t fb = (size_t)width * height * channels;
        enc->px_ref_frame = (uint8_t *)malloc(fb);
        if (!enc->px_ref_frame) { free(enc); return NULL; }
        if (enc->lossy) {
            enc->px_recon = (uint8_t *)malloc(fb);
            if (!enc->px_recon) { free(enc->px_ref_frame); free(enc); return NULL; }
        }
    }

    /* ~2 s of frame costs for the rate-control window. */
    enc->rc_win_cap = (uint32_t)(enc->fps * 2);
    if (enc->rc_win_cap < 8) enc->rc_win_cap = 8;
    if (enc->rc_win_cap > 240) enc->rc_win_cap = 240;
    enc->rc_win = (size_t *)calloc(enc->rc_win_cap, sizeof(size_t));
    if (!enc->rc_win) {
        if (enc->px_ref_frame) free(enc->px_ref_frame);
        if (enc->px_recon) free(enc->px_recon);
        free(enc);
        return NULL;
    }

    enc->batch_capacity = qtsq_num_cores();
    if (enc->batch_capacity > QTSQ_VIDEO_BATCH_MAX) enc->batch_capacity = QTSQ_VIDEO_BATCH_MAX;
    if (enc->batch_capacity < 1) enc->batch_capacity = 1;
    enc->batch_count = 0;

    size_t frame_bytes = (size_t)width * height * channels;

    /* Initialize bitstream with 1MB minimum; hard memory ceiling. The
     * working set covers everything else this encoder holds live: the
     * reference/reconstruction planes and the transient double-precision
     * planes the DCT I-frame decoder allocates
     * (~16 bytes/pixel on top of the raw frame). */
    size_t working = frame_bytes * 2       /* px_ref_frame, px_recon */
                    + frame_bytes * 16;    /* DCT decode transients (Y/Cb/Cr) */
    enc->bitstream_capacity = 1024 * 1024;
    enc->bitstream_limit = px_bitstream_limit_bytes(working);
    if (enc->bitstream_limit < enc->bitstream_capacity)
        enc->bitstream_capacity = enc->bitstream_limit;
    enc->bitstream = (uint8_t *)malloc(enc->bitstream_capacity);
    if (!enc->bitstream) {
        free(enc->rc_win);
        if (enc->px_ref_frame) free(enc->px_ref_frame);
        if (enc->px_recon) free(enc->px_recon);
        free(enc);
        return NULL;
    }
    enc->bitstream_size = 0;

    /* Prep context — pixel is the only video path; the mode is recorded in
     * schema.encoding so decoder/tools can identify it without header bits. */
    ctx->header.data_type = QTSQ_TYPE_VIDEO_FRAMES;
    ctx->header.strategy = QTSQ_STRATEGY_TEMPORAL;
    /* Mask video-mode bits out of header.flags (they collide with security
     * flag space); mode lives in schema.encoding instead. */
    ctx->header.flags = strategy_flags & ~QTSQ_VIDEO_PIXEL;
    if (enc->lossy) ctx->header.flags |= QTSQ_FLAG_LOSSY;
    /* Seed rate control: if caller passed a per-frame budget, start quality
     * mid-range so control can walk toward the target. */

    strcpy(ctx->schema.name, "QTSQ_VIDEO_STREAM");
    ctx->schema.data_type = QTSQ_TYPE_VIDEO_FRAMES;
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;
    ctx->schema.dimensions[2] = channels;
    ctx->schema.num_dims = 3;
    ctx->schema.frame_rate = enc->fps;
    strncpy(ctx->schema.encoding, "PIXEL", sizeof(ctx->schema.encoding) - 1);

    return enc;
}

/* ═══════════════════════════════════════════════════════════
 * PIXEL VIDEO PATH — mp4-like GOP (I/P frames)
 * Targets visual parity with .mp4.
 * Frame types: 'K' = keyframe, 'P' = P-frame (block MVs + residual).
 * Codec: qtc by default (ctx->codec), zlib as Settings opt-out —
 * matches every other QTSQ engine. Decode sniffs stream magic.
 * ═══════════════════════════════════════════════════════════ */


/* Compress the residual blob as N independently-coded chunks, in parallel.
 * Output framing (the decoder appends each decompressed chunk into one
 * contiguous buffer, so the block walk is identical to the single-stream
 * case):
 *   [4B total_raw_len][4B nchunks] then per chunk [4B chunk_raw_len][stream]
 * px_compress_ex already emits the [4B raw_len][stream] pair. */
typedef struct {
    const qtsq_context_t *ctx;
    const uint8_t *src;
    size_t off, len;
    uint8_t *out;      /* this chunk's compressed stream */
    size_t out_len;
    int rc;
} res_chunk_job_t;

/* Defined below (single-stream fallback + qtc/zlib dispatch). Declared here
 * so the chunk worker above it can call it. */
static int px_compress_ex(const qtsq_context_t *ctx,
                          const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len,
                          int speed_prio);

static void *px_res_chunk_thread(void *arg) {
    res_chunk_job_t *j = (res_chunk_job_t *)arg;
    j->rc = px_compress_ex(j->ctx, j->src + j->off, j->len, &j->out, &j->out_len, 1);
    return NULL;
}

static int px_res_compress_chunked(qtsq_video_encoder_t *enc,
                                   const uint8_t *res_pack, size_t res_pack_len,
                                   const uint32_t *block_off, const uint32_t *block_cnt,
                                   uint32_t nblocks, int nchunks,
                                   uint8_t **out, size_t *out_len) {
    if (nchunks < 1) nchunks = 1;
    if ((uint32_t)nchunks > nblocks) nchunks = (int)nblocks;

    /* Split the block list into nchunks contiguous, near-equal groups. */
    res_chunk_job_t job[8];
    memset(job, 0, sizeof(job));
    uint32_t next = 0;
    size_t total = 8;   /* header: total_raw_len + nchunks */
    for (int c = 0; c < nchunks; c++) {
        uint32_t lo = (uint32_t)((uint64_t)nblocks * (uint64_t)c / (uint64_t)nchunks);
        uint32_t hi = (uint32_t)((uint64_t)nblocks * (uint64_t)(c + 1) / (uint64_t)nchunks);
        if (hi <= lo) hi = lo + 1;
        if (hi > nblocks) hi = nblocks;
        size_t a = block_off[lo];
        size_t b = (hi < nblocks) ? block_off[hi] : res_pack_len;
        job[c].ctx = enc->ctx;
        job[c].src = res_pack;
        job[c].off = a;
        job[c].len = b > a ? b - a : 0;
        next = hi;
        total += 4 + job[c].len;   /* worst case: raw passthrough */
    }
    (void)next;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return QTSQ_ERR_ALLOC;

    /* Run the chunk encodes concurrently. Each job carries its own thread
     * entry so there is no shared-dispatch indirection to get wrong. */
    pthread_t bt[8];
    int started = 0;
    if (nchunks > 1) {
        for (int c = 0; c < nchunks; c++) {
            if (pthread_create(&bt[c], NULL, px_res_chunk_thread, &job[c]) == 0) {
                started++;
            } else {
                bt[c] = (pthread_t)0;   /* marked: ran inline below */
                px_res_chunk_thread(&job[c]);
            }
        }
        for (int c = 0; c < nchunks; c++) {
            if (bt[c]) pthread_join(bt[c], NULL);
        }
    } else {
        for (int c = 0; c < nchunks; c++) px_res_chunk_thread(&job[c]);
    }
    (void)started;

    int rc = QTSQ_OK;
    for (int c = 0; c < nchunks; c++) {
        if (job[c].rc != QTSQ_OK) { rc = job[c].rc; break; }
    }
    if (rc != QTSQ_OK) {
        for (int c = 0; c < nchunks; c++) free(job[c].out);
        free(buf);
        return rc;
    }

    size_t p = 0;
    uint32_t raw32 = (uint32_t)res_pack_len;
    uint32_t nc32 = (uint32_t)nchunks;
    memcpy(buf + p, &raw32, 4); p += 4;
    memcpy(buf + p, &nc32, 4); p += 4;
    for (int c = 0; c < nchunks; c++) {
        /* px_compress_ex output is already [4B raw_len][stream]. */
        if (job[c].out_len > 0 && job[c].out) {
            memcpy(buf + p, job[c].out, job[c].out_len);
            p += job[c].out_len;
        } else {
            /* Should not happen, but keep the stream well-formed. */
            uint32_t z = 0;
            memcpy(buf + p, &z, 4); p += 4;
        }
        free(job[c].out);
    }
    *out = buf;
    *out_len = p;
    return QTSQ_OK;
}

/* Compress with the context's codec (qtc default, zlib opt-out).
 * On success *out is malloc'd, *out_len set. Returns QTSQ_OK or error.
 * speed_prio: video residual/mm path — skip the multi-lens qtc contest
 * and use zlib level 6 (level 9 was a dominant per-frame cost at 4K). */
static int px_compress_ex(const qtsq_context_t *ctx,
                          const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len,
                          int speed_prio) {
    if (ctx && ctx->codec == QTSQ_CODEC_QTC) {
        if (speed_prio && in_len >= 64) {
            /* Force LZ2 only: residual is sparse int8, LZ wins without
             * paying for RAW0+RAW1+LZ2+audio contest every frame. */
            qtc_options_t o;
            qtc_options_default(&o);
            o.force_lens = QTC_LENS_LZ2;
            int rc = qtc_compress(in, in_len, out, out_len, &o);
            if (rc == QTC_OK && *out) return QTSQ_OK;
            if (rc == QTC_ERR_ALLOC) return QTSQ_ERR_ALLOC;
            /* LZ2 failed or incompressible → full router. */
        }
        return qtsq_codec_compress(in, in_len, out, out_len);
    }
    uLongf zbound = compressBound((uLong)in_len);
    uint8_t *zbuf = (uint8_t *)malloc(zbound);
    if (!zbuf) return QTSQ_ERR_ALLOC;
    uLongf zl = zbound;
    /* Level 6: large speed win vs 9 with ~1–2 % size cost on residuals. */
    int zerr = compress2(zbuf, &zl, in, (uLong)in_len, speed_prio ? 6 : 9);
    if (zerr != Z_OK) {
        free(zbuf);
        return QTSQ_ERR_IO;
    }
    *out = zbuf;
    *out_len = (size_t)zl;
    return QTSQ_OK;
}

static int px_compress(const qtsq_context_t *ctx,
                       const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len) {
    return px_compress_ex(ctx, in, in_len, out, out_len, 0);
}

/* Lossy I-frame: spatial mid-tread quant + existing entropy (qtc/zlib).
 * Fully QTSQ-owned — no H.264/HEVC/VideoToolbox. Faster than full DCT
 * photo coding (which is scalar-double and seconds/frame at 720p) while
 * still killing sensor noise and landing near the rate-control budget.
 * Returns QTSQ_OK; on success *recon_out (optional) receives the
 * reconstruction for closed-loop reference. */
static int px_quantize_frame(const uint8_t *pixels, size_t n,
                             int quant, uint8_t *levels, uint8_t *recon) {
    if (quant < 2) quant = 2;
    int half = quant / 2;
    for (size_t i = 0; i < n; i++) {
        int v = pixels[i];
        int q = (v + half) / quant;
        if (q > 255) q = 255;
        levels[i] = (uint8_t)q;
        if (recon) {
            int r = q * quant;
            if (r > 255) r = 255;
            recon[i] = (uint8_t)r;
        }
    }
    return QTSQ_OK;
}

/* Rate control against the CUMULATIVE byte budget (Studio passes
 * source_size/frame_count). From the 4K music-video case:
 *  - Early P-frames can be ~1 KB while the I-frame already spent 15×
 *    budget — recovery MUST NOT crank quality up and explode late-GOP
 *    (f45 jumped 1 KB → 330 KB that way).
 *  - Once over budget, quality only ratchets down (or stays), except a
 *    mild creep while well under (over < 0.80) and below quality_boot.
 *  - Floor keeps tiers from posterizing. */
static int qtsq_rc_debug(void) {
    static int inited = 0, on = 0;
    if (!inited) { inited = 1; on = getenv("QTSQ_RC_DEBUG") != NULL; }
    return on;
}

static void px_rate_control(qtsq_video_encoder_t *enc, size_t encoded_bytes) {
    if (!enc->lossy || enc->target_payload_per_frame == 0) return;
    size_t target = enc->target_payload_per_frame;
    if (target < 64) return;

    enc->rc_bytes += encoded_bytes;
    enc->rc_frames++;
    size_t expected = target * enc->rc_frames;
    if (expected == 0) return;

    /* Windowed ratio drives the decisions; the cumulative one is kept only
     * for the debug line. */
    double over;
    if (enc->rc_win && enc->rc_win_cap) {
        enc->rc_win[enc->rc_win_pos] = encoded_bytes;
        enc->rc_win_pos = (enc->rc_win_pos + 1) % enc->rc_win_cap;
        if (enc->rc_win_fill < enc->rc_win_cap) enc->rc_win_fill++;
        size_t wb = 0;
        for (uint32_t i = 0; i < enc->rc_win_fill; i++) wb += enc->rc_win[i];
        over = (double)wb / ((double)target * (double)enc->rc_win_fill);
    } else {
        over = (double)enc->rc_bytes / (double)expected;
    }

    float q = enc->quality;
    float qcap  = enc->quality_max > 0.0f ? enc->quality_max : 0.95f;
    float qmin  = 0.15f;
    if (enc->rc_q_good > qmin) qmin = enc->rc_q_good;

    /* The quality floor wins over the byte budget: once a frame lands under
     * the floor, stop shrinking and spend the remaining bytes on recovery.
     * Without this the controller happily sits under budget at 26 dB. */
    int below_floor = (enc->last_psnr < 30.0f);

    /* Proportional control on the log of the windowed ratio. The previous
     * bang-bang factors (x0.70 / x1.02) oscillated hard, which is what showed
     * up as quality pumping between frames. */
    double adj = -0.55 * log(over > 0.02 ? over : 0.02);
    if (below_floor) {
        /* The floor wins, but only up to a bounded overshoot. Unbounded, a
         * target that is simply unreachable (6.7 KB/frame on 1080p) pins
         * quality at the tier maximum and produces 8x the source size. Past
         * the bound the encoder takes the below-floor frames rather than the
         * unbounded file. */
        adj = (over < 2.0) ? 0.10 : 0.0;
    } else if (over < 0.90) {
        adj *= 0.45;                          /* cautious climb */
    }

    double nq = (double)q * exp(adj);
    if (nq < qmin) nq = qmin;
    if (nq > qcap) nq = qcap;
    if (below_floor && nq < q) nq = q;   /* never cut while under the floor */
    if (below_floor && nq < qmin) nq = qmin;

    float q0 = q;
    q = (float)nq;
    enc->quality = q;
    enc->quant = px_quant_from_quality(q);
    if (qtsq_rc_debug()) {
        const char *mode = below_floor ? "HOLD" : (over > 1.0 ? "CUT " : "CREEP");
        fprintf(stderr, "RC f%zu %s bytes=%zu over=%.2f p=%.2f q %.3f->%.3f (cap %.3f min %.3f)\n",
                (size_t)enc->rc_frames, mode, encoded_bytes, over,
                (double)enc->last_psnr, (double)q0, (double)q,
                (double)qcap, (double)qmin);
    }
}

/* Bootstrap: cut quality once from raw/target so the I-frame fits a fair
 * share of the budget. quality_boot is the ceiling for mid-GOP recovery. */
static void px_rate_bootstrap(qtsq_video_encoder_t *enc) {
    if (!enc->lossy || enc->target_payload_per_frame == 0) {
        enc->quality_boot = enc->quality;
        return;
    }
    size_t raw = (size_t)enc->width * enc->height * enc->channels;
    size_t target = enc->target_payload_per_frame;
    if (target == 0 || raw == 0) {
        enc->quality_boot = enc->quality;
        return;
    }

    double ratio = (double)raw / (double)target;
    float scale;
    if (ratio <= 8.0)         scale = 1.00f;
    else if (ratio <= 40.0)   scale = 0.85f;
    else if (ratio <= 150.0)  scale = 0.70f;
    else if (ratio <= 500.0)  scale = 0.55f;
    else                      scale = 0.45f;

    enc->quality *= scale;
    /* 0.15 static floor: keeps LOW < MEDIUM < HIGH bootstrap ordering.
     * The PSNR hold (rc_q_good) rescues quality if 0.15 posterizes. */
    if (enc->quality < 0.15f) enc->quality = 0.15f;
    if (enc->quality > 0.95f) enc->quality = 0.95f;
    enc->quality_boot = enc->quality;
    enc->quant = px_quant_from_quality(enc->quality);
}

/* SAD for one macroblock (all channels interleaved). */
static uint32_t px_sad_block(const uint8_t *cur, const uint8_t *ref,
                              uint32_t cur_stride, uint32_t ref_stride,
                              uint32_t bw, uint32_t bh, uint8_t ch) {
    uint32_t sad = 0;
    for (uint32_t y = 0; y < bh; y++) {
        const uint8_t *c = cur + y * cur_stride;
        const uint8_t *r = ref + y * ref_stride;
        /* 8-byte chunks via uint64 where possible is overkill for ch=3;
         * plain loop is fine and auto-vectorizes at -O3. */
        for (uint32_t i = 0; i < bw * ch; i++) {
            uint32_t d = c[i] > r[i] ? c[i] - r[i] : r[i] - c[i];
            sad += d;
        }
    }
    return sad;
}


/* Fast motion search: zero-first early-out + diamond (log) search.
 * Replaces the old O(range²) exhaustive scan — ~50-100× fewer SAD calls
 * on typical content where MV≈0 or motion is smooth. */
static uint32_t px_motion_search(const uint8_t *cur, const uint8_t *ref,
                                  uint32_t w, uint32_t h, uint8_t ch,
                                  uint32_t mbx, uint32_t mby,
                                  int *out_mvx, int *out_mvy) {
    uint32_t bw = PX_MB_SIZE, bh = PX_MB_SIZE;
    if (mbx + bw > w) bw = w - mbx;
    if (mby + bh > h) bh = h - mby;
    uint32_t stride = w * ch;
    const uint8_t *cur_blk = cur + ((size_t)mby * w + mbx) * ch;

    /* Pass 1: MV=(0,0) — most blocks in real video match here. */
    uint32_t best_sad = px_sad_block(cur_blk,
                                     ref + ((size_t)mby * w + mbx) * ch,
                                     stride, stride, bw, bh, ch);
    int best_mvx = 0, best_mvy = 0;
    if (best_sad == 0) { *out_mvx = 0; *out_mvy = 0; return 0; }

    /* Pass 2: diamond search — step sizes 8,4,2,1 over ±PX_MV_RANGE. */
    static const int dx4[4] = { -1, 1, 0, 0 };
    static const int dy4[4] = { 0, 0, -1, 1 };
    int step = 8;
    while (step > 0) {
        int improved = 0;
        for (int k = 0; k < 4; k++) {
            int nx = best_mvx + dx4[k] * step;
            int ny = best_mvy + dy4[k] * step;
            if (nx < -PX_MV_RANGE || nx > PX_MV_RANGE ||
                ny < -PX_MV_RANGE || ny > PX_MV_RANGE) continue;
            int sx = (int)mbx + nx, sy = (int)mby + ny;
            if (sx < 0 || sy < 0 ||
                sx + (int)bw > (int)w || sy + (int)bh > (int)h) continue;
            uint32_t sad = px_sad_block(cur_blk,
                                            ref + ((size_t)sy * w + sx) * ch,
                                            stride, stride, bw, bh, ch);
            if (sad < best_sad) {
                best_sad = sad;
                best_mvx = nx;
                best_mvy = ny;
                improved = 1;
                if (sad == 0) break;
            }
        }
        if (best_sad == 0) break;
        if (!improved) step >>= 1;
    }

    /* Pass 3: small exhaustive refine ±2 around diamond result (cheap). */
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = best_mvx + dx, ny = best_mvy + dy;
            if (nx < -PX_MV_RANGE || nx > PX_MV_RANGE ||
                ny < -PX_MV_RANGE || ny > PX_MV_RANGE) continue;
            int sx = (int)mbx + nx, sy = (int)mby + ny;
            if (sx < 0 || sy < 0 ||
                sx + (int)bw > (int)w || sy + (int)bh > (int)h) continue;
            uint32_t sad = px_sad_block(cur_blk,
                                            ref + ((size_t)sy * w + sx) * ch,
                                            stride, stride, bw, bh, ch);
            if (sad < best_sad) {
                best_sad = sad;
                best_mvx = nx;
                best_mvy = ny;
            }
        }
    }

    *out_mvx = best_mvx;
    *out_mvy = best_mvy;
    return best_sad;
}

/* ── Parallel motion-search job (one row of MBs per chunk) ── */
typedef struct {
    const uint8_t *cur;
    const uint8_t *ref;
    uint32_t w, h;
    uint8_t ch;
    uint32_t mb_w, mb_h;
    uint32_t row_start, row_end;   /* MB rows [row_start, row_end) */
    uint8_t *modes;                /* nmb bytes */
    int8_t  *mvs;                  /* nmb * 2 */
    uint8_t *residuals;            /* packed mode-2 residuals */
    size_t  *res_offsets;          /* nmb: offset into residuals, or SIZE_MAX */
    size_t   mb_residual_cap;      /* max bytes per MB residual */
    int      lossy;                /* 1 = quantize residual to int8 levels */
    int      quant;                /* residual step (lossy only) */
} px_search_job_t;

static void px_encode_mb_range(const px_search_job_t *job) {
    uint32_t w = job->w, h = job->h;
    uint8_t ch = job->ch;
    int quant = job->lossy ? job->quant : 0;
    for (uint32_t my = job->row_start; my < job->row_end; my++) {
        for (uint32_t mx = 0; mx < job->mb_w; mx++) {
            uint32_t idx = my * job->mb_w + mx;
            uint32_t bx = mx * PX_MB_SIZE;
            uint32_t by = my * PX_MB_SIZE;
            uint32_t bw = PX_MB_SIZE; if (bx + bw > w) bw = w - bx;
            uint32_t bh = PX_MB_SIZE; if (by + bh > h) bh = h - by;

            int mvx = 0, mvy = 0;
            /* Lossy fast path: if MV=(0,0) already yields all-zero quantized
             * residual, this is mode 0 — skip the search and don't emit an MV. */
            int zero_mv_ok = 0;
            if (job->lossy) {
                const uint8_t *rb0 = job->ref + ((size_t)by * w + bx) * ch;
                const uint8_t *cb0 = job->cur + ((size_t)by * w + bx) * ch;
                int all0 = 1;
                for (uint32_t y = 0; y < bh && all0; y++) {
                    for (uint32_t x = 0; x < bw * ch; x++) {
                        int s = (int)cb0[y * w * ch + x] - (int)rb0[y * w * ch + x];
                        int qv;
                        if (s >= 0) qv = (s + quant / 2) / quant;
                        else qv = -(((-s) + quant / 2) / quant);
                        if (qv != 0) { all0 = 0; break; }
                    }
                }
                if (all0) {
                    zero_mv_ok = 1;
                    job->modes[idx] = 0;
                    job->mvs[idx * 2] = 0;
                    job->mvs[idx * 2 + 1] = 0;
                    job->res_offsets[idx] = SIZE_MAX;
                    continue;
                }
            }
            px_motion_search(job->cur, job->ref, w, h, ch, bx, by, &mvx, &mvy);

            int rx = (int)bx + mvx, ry = (int)by + mvy;
            const uint8_t *ref_blk = job->ref + ((size_t)ry * w + rx) * ch;
            const uint8_t *cur_blk = job->cur + ((size_t)by * w + bx) * ch;

            size_t res_n = 0;
            int residual_nonzero = 0;
            int residual_energy = 0; /* sum |level| — drives high-Q skip */
            for (uint32_t y = 0; y < bh; y++) {
                for (uint32_t x = 0; x < bw * ch; x++) {
                    uint8_t cb = cur_blk[y * w * ch + x];
                    uint8_t rb = ref_blk[y * w * ch + x];
                    if (job->lossy) {
                        /* Signed residual → uniform quant → int8 level.
                         * Encoder reconstructs with level*quant (closed-loop)
                         * so decoder dequant matches exactly. */
                        int s = (int)cb - (int)rb;
                        int qv;
                        if (s >= 0) qv = (s + quant / 2) / quant;
                        else qv = -(((-s) + quant / 2) / quant);
                        if (qv > 127) qv = 127;
                        if (qv < -127) qv = -127;
                        if (qv != 0) residual_nonzero++;
                        if (qv < 0) residual_energy += -qv;
                        else residual_energy += qv;
                        job->residuals[idx * job->mb_residual_cap + res_n++] =
                            (uint8_t)(int8_t)qv;
                    } else {
                        uint8_t d = (uint8_t)(cb - rb);
                        if (d != 0) residual_nonzero = 1;
                        job->residuals[idx * job->mb_residual_cap + res_n++] = d;
                    }
                }
            }

            uint8_t mode;
            if (zero_mv_ok) {
                mode = 0; /* set in early-out above */
            } else if (job->lossy) {
                /* Two complementary skips:
                 *  - Count-based (low/mid Q): <0.5% nonzero → drop. Keeps
                 *    thin edges that carry perceived detail at moderate Q.
                 *  - Energy-based (high Q): mean |level| < ~0.75 → drop.
                 *    At Q≥24 the closed-loop I-ref is blocky so many MBs
                 *    light up; their mean error is still under one quant
                 *    step, so mode 0/1 (MC only) is visually free and is
                 *    what stops LOW exploding past MEDIUM/HIGH in size.
                 *    PSNR hold in RC keeps quality from parking here when
                 *    that would drop under the 28 dB floor. */
                int skip = 0;
                if (quant >= 24) {
                    if (residual_energy * 4 < (int)res_n * 3) skip = 1;
                }
                if (!skip && residual_nonzero * 200 < (int)res_n) skip = 1;
                if (skip) {
                    residual_nonzero = 0;
                    memset(job->residuals + idx * job->mb_residual_cap, 0, res_n);
                }
                if (!residual_nonzero) {
                    mode = (mvx == 0 && mvy == 0) ? 0 : 1;
                } else {
                    /* Mode 3 = sparse run-coded residual. Chosen only when it
                     * is strictly smaller than the dense block, so dense
                     * blocks keep their existing cost.
                     *
                     * The sparse form is at best 1 + nz + runs bytes, and
                     * runs >= 1, so it can only win while nz <= res_n - 3.
                     * Checking that first skips a full read pass over the
                     * block for the dense case, which is the common one at
                     * high quality. Bit-exact: the decision is unchanged. */
                    if ((size_t)residual_nonzero + 2 < (size_t)res_n) {
                        uint32_t sr = 0, sn = 0;
                        size_t ssz = 0;
                        qtsq_res_sparse_scan(job->residuals + idx * job->mb_residual_cap,
                                             res_n, &sr, &sn, &ssz);
                        mode = (ssz > 0 && ssz < (size_t)res_n) ? 3 : 2;
                    } else {
                        mode = 2;
                    }
                }
            } else {
                if (!residual_nonzero) {
                    mode = (mvx == 0 && mvy == 0) ? 0 : 1;
                } else {
                    mode = 2;
                }
            }
            job->modes[idx] = mode;
            job->mvs[idx * 2]     = (int8_t)mvx;
            job->mvs[idx * 2 + 1] = (int8_t)mvy;
            job->res_offsets[idx] = (mode == 2 || mode == 3) ? (idx * job->mb_residual_cap) : SIZE_MAX;
        }
    }
}

static void *px_search_thread(void *arg) {
    px_encode_mb_range((const px_search_job_t *)arg);
    return NULL;
}

/* Encode one P-frame.
 * Layout (all little-endian), LOSSLESS:
 *   [1B mb_size] [2B mb_cols] [2B mb_rows] [4B modes_mvs_len] [4B res_comp_len]
 *   [modes_mvs stream]  — per MB: [1B mode][optional 2B mvx/mvy]
 *   [compressed residual blob] — concatenation of all mode-2 residuals
 * LOSSY (QTSQ_FLAG_LOSSY) inserts quant after mb_size (14-byte header):
 *   [1B mb_size] [1B quant] [2B mb_cols] [2B mb_rows] [4B mm_len] [4B res_len]
 *   residual samples are int8 quant levels; decoder dequantizes with quant.
 * Residual is compressed ONCE per frame with qtc (default) or zlib.
 * Returns QTSQ_OK on success. */
static int px_encode_pframe(qtsq_video_encoder_t *enc, const uint8_t *cur) {
    uint32_t w = enc->width, h = enc->height;
    uint8_t ch = enc->channels;
    uint32_t mb_w = (w + PX_MB_SIZE - 1) / PX_MB_SIZE;
    uint32_t mb_h = (h + PX_MB_SIZE - 1) / PX_MB_SIZE;
    uint32_t nmb = mb_w * mb_h;
    size_t mb_res_cap = (size_t)PX_MB_SIZE * PX_MB_SIZE * ch;

    /* Reuse per-encoder scratch — first frame allocates, rest reuse.
     * (Old path: malloc+free of ~25 MB residual every P-frame at 4K.) */
    if (!enc->px_scratch_modes || enc->px_scratch_nmb != nmb ||
        enc->px_scratch_residual_cap < (size_t)nmb * mb_res_cap) {
        free(enc->px_scratch_modes);
        free(enc->px_scratch_mvs);
        free(enc->px_scratch_res_off);
        free(enc->px_scratch_residuals);
        enc->px_scratch_modes = NULL;
        enc->px_scratch_mvs = NULL;
        enc->px_scratch_res_off = NULL;
        enc->px_scratch_residuals = NULL;
        enc->px_scratch_residual_cap = 0;
        enc->px_scratch_nmb = 0;
        enc->px_scratch_modes = (uint8_t *)malloc(nmb);
        enc->px_scratch_mvs   = (int8_t  *)malloc((size_t)nmb * 2);
        enc->px_scratch_res_off = (size_t *)malloc(nmb * sizeof(size_t));
        enc->px_scratch_residuals = (uint8_t *)malloc((size_t)nmb * mb_res_cap);
        if (!enc->px_scratch_modes || !enc->px_scratch_mvs ||
            !enc->px_scratch_res_off || !enc->px_scratch_residuals) {
            free(enc->px_scratch_modes);
            free(enc->px_scratch_mvs);
            free(enc->px_scratch_res_off);
            free(enc->px_scratch_residuals);
            enc->px_scratch_modes = NULL;
            enc->px_scratch_mvs = NULL;
            enc->px_scratch_res_off = NULL;
            enc->px_scratch_residuals = NULL;
            return QTSQ_ERR_ALLOC;
        }
        enc->px_scratch_nmb = nmb;
        enc->px_scratch_residual_cap = (size_t)nmb * mb_res_cap;
    }

    uint8_t *modes = enc->px_scratch_modes;
    int8_t  *mvs   = enc->px_scratch_mvs;
    size_t  *res_off = enc->px_scratch_res_off;
    uint8_t *residuals = enc->px_scratch_residuals;

    /* Parallelize motion search across MB rows */
    px_search_job_t job = {
        .cur = cur, .ref = enc->px_ref_frame,
        .w = w, .h = h, .ch = ch,
        .mb_w = mb_w, .mb_h = mb_h,
        .modes = modes, .mvs = mvs,
        .residuals = residuals, .res_offsets = res_off,
        .mb_residual_cap = mb_res_cap,
        .lossy = enc->lossy, .quant = enc->quant,
    };

    int nthreads = qtsq_num_cores();
    if (nthreads > (int)mb_h) nthreads = (int)mb_h;
    if (nthreads < 1) nthreads = 1;

    if (nthreads <= 1) {
        job.row_start = 0;
        job.row_end = mb_h;
        px_encode_mb_range(&job);
    } else {
        /* One job struct per thread with its own row slice */
        px_search_job_t jobs[QTSQ_MAX_THREADS];
        uint32_t rows_per = (mb_h + (uint32_t)nthreads - 1) / (uint32_t)nthreads;
        for (int t = 0; t < nthreads; t++) {
            jobs[t] = job;
            jobs[t].row_start = (uint32_t)t * rows_per;
            jobs[t].row_end = jobs[t].row_start + rows_per;
            if (jobs[t].row_end > mb_h) jobs[t].row_end = mb_h;
            if (jobs[t].row_start > mb_h) jobs[t].row_start = mb_h;
        }
        qtsq_dispatch(px_search_thread, jobs, sizeof(jobs[0]), nthreads);
    }

    /* Closed-loop reconstruction (lossy only): MC + dequant residual →
     * next reference must be what the decoder will build, not the
     * original pixels, or quality drifts every P-frame. */
    if (enc->lossy && enc->px_recon) {
        const uint8_t *ref = enc->px_ref_frame;
        uint8_t *recon = enc->px_recon;
        int quant = enc->quant;
        for (uint32_t my = 0; my < mb_h; my++) {
            for (uint32_t mx = 0; mx < mb_w; mx++) {
                uint32_t idx = my * mb_w + mx;
                uint32_t bx = mx * PX_MB_SIZE;
                uint32_t by = my * PX_MB_SIZE;
                uint32_t bw = PX_MB_SIZE; if (bx + bw > w) bw = w - bx;
                uint32_t bh = PX_MB_SIZE; if (by + bh > h) bh = h - by;
                int mvx = mvs[idx * 2], mvy = mvs[idx * 2 + 1];
                int rx = (int)bx + mvx, ry = (int)by + mvy;
                if (rx < 0) rx = 0;
                if (ry < 0) ry = 0;
                if (rx + (int)bw > (int)w) rx = (int)w - (int)bw;
                if (ry + (int)bh > (int)h) ry = (int)h - (int)bh;
                if (rx < 0 || ry < 0) continue;
                for (uint32_t y = 0; y < bh; y++) {
                    memcpy(recon + ((size_t)(by + y) * w + bx) * ch,
                           ref + ((size_t)(ry + y) * w + rx) * ch,
                           (size_t)bw * ch);
                }
                if (modes[idx] == 2 || modes[idx] == 3) {
                    const uint8_t *res_base = residuals + res_off[idx];
                    int qmb = quant;
                    for (uint32_t y = 0; y < bh; y++) {
                        uint8_t *out_row = recon + ((size_t)(by + y) * w + bx) * ch;
                        const uint8_t *res_row = res_base + (size_t)y * bw * ch;
                        for (uint32_t i = 0; i < bw * ch; i++) {
                            int v = (int)out_row[i] + (int)(int8_t)res_row[i] * quant;
                            if (v < 0) v = 0;
                            if (v > 255) v = 255;
                            out_row[i] = (uint8_t)v;
                        }
                    }
                }
            }
        }
        memcpy(enc->px_ref_frame, recon, (size_t)w * h * ch);
    }

    /* Build modes_mvs stream — bitpack modes (2 bits) + MV pairs only for
     * non-zero modes. Raw byte-per-MB format was ~14–43 KB/frame uncompressed
     * before entropy; this is ~4× smaller before compression too.
     * Layout: [mode bits, MSB-first, padded to byte]
     *         [for each mode!=0 in raster order: 1B mvx, 1B mvy] */
    size_t mode_bits = (size_t)nmb * 2;
    size_t mode_bytes = (mode_bits + 7) / 8;
    size_t nonzero_mv = 0;
    for (uint32_t i = 0; i < nmb; i++) if (modes[i] != 0) nonzero_mv++;
    size_t mm_raw_len = mode_bytes + nonzero_mv * 2;
    uint8_t *mm = (uint8_t *)calloc(1, mm_raw_len ? mm_raw_len : 1);
    if (!mm) {
        return QTSQ_ERR_ALLOC;
    }
    for (uint32_t i = 0; i < nmb; i++) {
        size_t bit = i * 2;
        uint8_t m = modes[i] & 3;
        mm[bit / 8] |= (uint8_t)(m << (6 - (bit % 8)));
        /* when bit%8 == 6, both bits fit in this byte (m uses bits 1..0);
         * when bit%8 == 0, m spans bits 7..6 of this byte — covered above
         * only if m fits entirely; for bit%8==6, m<<0 would be wrong for
         * the low bit of a split — modes are 2-bit so alignment is always
         * on even bit offsets: bit = i*2 ⇒ bit%8 ∈ {0,2,4,6}.
         * For bit%8==2: shift = 6-2=4 → bits 5..4. For 4: shift=2 → 3..2.
         * For 6: shift=0 → 1..0. Correct. */
    }
    {
        uint8_t *p = mm + mode_bytes;
        /* 'p' frames code the median-predicted MV delta instead of the raw
         * vector. Neighbour MVs are already known in raster order, so the
         * predictor only ever reads coded values. */
        int16_t *mvp = (int16_t *)malloc((size_t)nmb * 2 * sizeof(int16_t));
        if (!mvp) { free(mm); return QTSQ_ERR_ALLOC; }
        for (uint32_t i = 0; i < nmb; i++) {
            mvp[i * 2] = mvs[i * 2];
            mvp[i * 2 + 1] = mvs[i * 2 + 1];
        }
        for (uint32_t i = 0; i < nmb; i++) {
            if (modes[i] == 0) continue;
            uint32_t mx = i % mb_w, my = i / mb_w;
            int pvx, pvy;
            qtsq_mv_predict(mvp, mb_w, mx, my, &pvx, &pvy);
            int dx = (int)mvs[i * 2] - pvx;
            int dy = (int)mvs[i * 2 + 1] - pvy;
            /* MV range is +/-16 and the predictor is a median of those, so
             * deltas always fit a signed byte. Clamp defensively anyway. */
            if (dx < -128) dx = -128; if (dx > 127) dx = 127;
            if (dy < -128) dy = -128; if (dy > 127) dy = 127;
            *p++ = (uint8_t)(int8_t)dx;
            *p++ = (uint8_t)(int8_t)dy;
        }
        free(mvp);
    }
    size_t mm_pos = mm_raw_len;

    /* Pack residuals into one contiguous buffer (raster order). Mode 3 blocks
     * use the sparse run form, mode 2 the dense form, so this is two passes:
     * size, then write. */
    size_t res_total = 0;
    for (uint32_t i = 0; i < nmb; i++) {
        if (modes[i] == 2 || modes[i] == 3) {
            uint32_t mx = i % mb_w, my = i / mb_w;
            uint32_t bx = mx * PX_MB_SIZE, by = my * PX_MB_SIZE;
            uint32_t bw = PX_MB_SIZE; if (bx + bw > w) bw = w - bx;
            uint32_t bh = PX_MB_SIZE; if (by + bh > h) bh = h - by;
            size_t blen = (size_t)bw * bh * ch;
            if (modes[i] == 3) {
                uint32_t sr = 0, sn = 0; size_t ssz = 0;
                qtsq_res_sparse_scan(residuals + res_off[i], blen, &sr, &sn, &ssz);
                res_total += ssz;
            } else {
                res_total += blen;
            }
        }
    }

    /* Per-block (offset, length) so the blob can be split at block edges. */
    uint32_t *res_block_off = NULL, *res_block_cnt = NULL;
    uint32_t res_nblocks = 0;
    if (nmb > 0) {
        res_block_off = (uint32_t *)malloc(nmb * sizeof(uint32_t));
        res_block_cnt = (uint32_t *)malloc(nmb * sizeof(uint32_t));
    }

    uint8_t *res_pack = NULL;
    size_t res_pack_len = 0;
    if (res_total > 0) {
        res_pack = (uint8_t *)malloc(res_total);
        if (!res_pack) {
            free(mm);
            return QTSQ_ERR_ALLOC;
        }
        size_t rp = 0;
        for (uint32_t i = 0; i < nmb; i++) {
            if (modes[i] != 2 && modes[i] != 3) continue;
            uint32_t mx = i % mb_w, my = i / mb_w;
            uint32_t bx = mx * PX_MB_SIZE, by = my * PX_MB_SIZE;
            uint32_t bw = PX_MB_SIZE; if (bx + bw > w) bw = w - bx;
            uint32_t bh = PX_MB_SIZE; if (by + bh > h) bh = h - by;
            size_t blen = (size_t)bw * bh * ch;
            uint32_t before = (uint32_t)rp;
            if (modes[i] == 3) {
                size_t n = qtsq_res_sparse_write(res_pack + rp,
                                                 residuals + res_off[i], blen);
                if (n == 0) {   /* cannot happen: mode 3 implies a fit */
                    free(res_pack); free(mm);
                    free(res_block_off); free(res_block_cnt);
                    return QTSQ_ERR_FORMAT;
                }
                rp += n;
            } else {
                memcpy(res_pack + rp, residuals + res_off[i], blen);
                rp += blen;
            }
            if (res_block_off && res_block_cnt && res_nblocks < nmb) {
                res_block_off[res_nblocks] = before;
                res_block_cnt[res_nblocks] = (uint32_t)(rp - before);
                res_nblocks++;
            }
        }
        res_pack_len = rp;
    }

    uint8_t *res_comp = NULL;
    size_t res_comp_len = 0;
    if (res_pack_len > 0) {
        int crc;
        /* Entropy coding the whole residual blob is the single biggest cost in
         * the frame (~61% measured at 1080p) and it is serial, which is why
         * the encoder only reached ~3.3x on 10 cores. Compressing the blob in
         * independent per-block-range chunks lets those run in parallel.
         *
         * Chunks are cut on macroblock boundaries and the decoder simply
         * appends each decompressed chunk into one contiguous buffer, so the
         * block walk downstream is unchanged. */
        int nchunks = 1;
        if (res_pack_len >= (1u << 18) && enc->batch_capacity > 1) {
            nchunks = enc->batch_capacity;
            if (nchunks > 8) nchunks = 8;
            /* Keep each chunk large enough for LZ to find matches. */
            if (res_pack_len / (size_t)nchunks < (1u << 15)) nchunks = 1;
        }
        if (nchunks <= 1) {
            crc = px_compress_ex(enc->ctx, res_pack, res_pack_len,
                                 &res_comp, &res_comp_len, 1);
        } else {
            crc = px_res_compress_chunked(enc, res_pack, res_pack_len,
                                           res_block_off, res_block_cnt,
                                           res_nblocks, nchunks,
                                           &res_comp, &res_comp_len);
        }
        free(res_pack);
        if (crc != QTSQ_OK) { free(mm); return crc; }
    } else {
        free(res_pack);
    }

    /* Compress modes+mvs (highly structured — large win vs raw). Stored as
     * [4B raw_len][qtc-or-zlib] so the decoder can qtsq_uncompress_bounded. */
    uint8_t *mm_comp = NULL;
    size_t mm_comp_len = 0;
    if (mm_pos > 0) {
        int mrc = px_compress_ex(enc->ctx, mm, mm_pos, &mm_comp, &mm_comp_len, 1);
        free(mm);
        if (mrc != QTSQ_OK) { free(res_comp); return mrc; }
    } else {
        free(mm);
    }

    /* Assemble payload. Lossless: 13-byte header. Lossy: 14-byte header
     * with quant at byte 1 so the decoder can dequantize.
     * mm section on disk: [4B raw_mm_len][compressed stream] when mm_comp
     * present; empty when there were no MBs (mm_pos==0). */
    size_t mm_disk = mm_comp_len > 0 ? (4 + mm_comp_len) : 0;
    size_t res_blob_disk = res_comp_len > 0 ? (4 + res_comp_len) : 0;
    size_t hdr = enc->lossy ? 14 : 13;
    size_t payload_size = hdr + mm_disk + res_blob_disk;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) { free(mm_comp); free(res_comp); return QTSQ_ERR_ALLOC; }
    size_t pos = 0;
    payload[pos++] = (uint8_t)PX_MB_SIZE;
    if (enc->lossy) payload[pos++] = (uint8_t)enc->quant;
    payload[pos++] = (uint8_t)(mb_w & 0xFF);
    payload[pos++] = (uint8_t)(mb_w >> 8);
    payload[pos++] = (uint8_t)(mb_h & 0xFF);
    payload[pos++] = (uint8_t)(mb_h >> 8);
    uint32_t mm_len32 = (uint32_t)mm_disk;
    uint32_t rc_len32 = (uint32_t)res_blob_disk;
    memcpy(payload + pos, &mm_len32, 4); pos += 4;
    memcpy(payload + pos, &rc_len32, 4); pos += 4;
    if (mm_comp_len > 0) {
        uint32_t mm_raw = (uint32_t)mm_pos; /* original uncompressed size */
        memcpy(payload + pos, &mm_raw, 4); pos += 4;
        memcpy(payload + pos, mm_comp, mm_comp_len); pos += mm_comp_len;
        free(mm_comp);
    }
    if (res_comp_len > 0) {
        uint32_t raw_total = (uint32_t)res_pack_len;
        memcpy(payload + pos, &raw_total, 4); pos += 4;
        memcpy(payload + pos, res_comp, res_comp_len); pos += res_comp_len;
        free(res_comp);
    }

    uint32_t offset = (uint32_t)enc->bitstream_size;
    int arc = append_to_bitstream(enc, payload, payload_size);
    free(payload);
    if (arc != QTSQ_OK) return arc;
    return append_to_index(enc, offset, (uint32_t)payload_size, 'q');
}

/* Encode one I-frame (keyframe), type 'K'.
 * Lossless: [2B w][2B h][4B raw_size][qtc-or-zlib of raw pixels]
 * Lossy DCT (quant byte = 0 sentinel):
 *   [2B w][2B h][1B 0][4B raw_size][qtc image-DCT stream]
 *   The stream is self-describing (w/h/ch inside); decoder runs
 *   qtc_decompress → pixels (no level scaling). Closed-loop recon is
 *   the DCT decode, so P-frame MC matches the reference exactly.
 * Lossy spatial (legacy, quant ≥ 2):
 *   [2B w][2B h][1B quant][4B raw_size][qtc-or-zlib of LEVELS] */
static int px_encode_iframe(qtsq_video_encoder_t *enc, const uint8_t *pixels) {
    size_t raw_size = (size_t)enc->width * enc->height * enc->channels;
    uint8_t *cbuf = NULL;
    size_t clen = 0;
    int crc;
    int iq = enc->quant;
    int use_dct = 0;

    if (enc->lossy) {
        /* Prefer the custom 8×8 DCT photo coder for I-frames: ~2–4× smaller
         * than spatial levels at matched PSNR, and self-contained so the
         * decoder needs no per-pixel dequant loop. Uses qtc_compress_image
         * (mode=LOSSY) — the generic qtc_compress contest path never runs
         * the DCT lens. Falls back to spatial quant if DCT rejects the
         * input (e.g. ch not in {1,3}). */
        if (enc->channels == 1 || enc->channels == 3) {
            qtc_options_t opts;
            qtc_options_default(&opts);
            opts.mode = QTC_MODE_LOSSY;
            opts.quality = enc->quality;
            opts.force_lens = QTC_LENS_IMAGE_DCT;
            int drc = qtc_compress_image(pixels,
                                         (uint32_t)enc->width,
                                         (uint32_t)enc->height,
                                         enc->channels,
                                         &cbuf, &clen, &opts);
            if (drc == QTC_OK && cbuf && clen > 0) {
                /* Closed-loop: recon = DCT decode of what we just wrote.
                 * If recon PSNR is below the I-frame floor, fall back to
                 * spatial quant — some high-frequency content (XOR patterns,
                 * hard edges) codes worse under 4:2:0 DCT than mid-tread. */
                uint8_t *recon = NULL;
                uint32_t rw = 0, rh = 0;
                uint8_t rch = 0;
                if (qtc_decompress_image(cbuf, clen, &recon, &rw, &rh, &rch) == QTC_OK &&
                    recon && (size_t)rw * rh * rch == raw_size && rch == enc->channels) {
                    double mse = 0.0;
                    for (size_t i = 0; i < raw_size; i++) {
                        double d = (double)pixels[i] - (double)recon[i];
                        mse += d * d;
                    }
                    mse /= (double)raw_size;
                    double ipsnr = (mse < 1e-12) ? 99.0
                                 : 10.0 * log10(255.0 * 255.0 / mse);
                    /* The DCT coder reaches ~28-29 dB at medium quality. A
                     * 30 dB gate rejected it there, so every I-frame fell
                     * back to the spatial coder — visibly washed out and
                     * larger. Gate at the same 28 dB floor the format
                     * guarantees, so the better coder is actually used. */
                    if (ipsnr >= 28.0) {
                        memcpy(enc->px_ref_frame, recon, raw_size);
                        free(recon);
                        use_dct = 1;
                        iq = 0; /* sentinel: DCT stream, no level scaling */
                    } else {
                        free(recon);
                        free(cbuf);
                        cbuf = NULL;
                        clen = 0;
                    }
                } else {
                    free(recon);
                    free(cbuf);
                    cbuf = NULL;
                    clen = 0;
                }
            } else {
                free(cbuf);
                cbuf = NULL;
                clen = 0;
            }
        }
        if (!use_dct) {
            /* Spatial mid-tread quant fallback (levels + entropy). */
            if (iq < 2) iq = 2;
            if (!enc->px_recon) return QTSQ_ERR_ALLOC;
            for (size_t i = 0; i < raw_size; i++) {
                int v = pixels[i];
                int q = (v + iq / 2) / iq;
                if (q > 255) q = 255;
                enc->px_recon[i] = (uint8_t)q;
            }
            if (!enc->px_ref_frame) return QTSQ_ERR_ALLOC;
            for (size_t i = 0; i < raw_size; i++) {
                int r = (int)enc->px_recon[i] * iq;
                if (r > 255) r = 255;
                enc->px_ref_frame[i] = (uint8_t)r;
            }
            crc = px_compress(enc->ctx, enc->px_recon, raw_size, &cbuf, &clen);
            if (crc != QTSQ_OK) return crc;
        }
    } else {
        crc = px_compress(enc->ctx, pixels, raw_size, &cbuf, &clen);
        if (crc != QTSQ_OK) return crc;
    }

    size_t hdr = enc->lossy ? 9 : 8;
    size_t payload_size = hdr + clen;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) { free(cbuf); return QTSQ_ERR_ALLOC; }
    payload[0] = (uint8_t)(enc->width & 0xFF);
    payload[1] = (uint8_t)(enc->width >> 8);
    payload[2] = (uint8_t)(enc->height & 0xFF);
    payload[3] = (uint8_t)(enc->height >> 8);
    size_t qoff = 4;
    if (enc->lossy) {
        payload[4] = (uint8_t)iq;
        qoff = 5;
    }
    uint32_t rs = (uint32_t)raw_size;
    memcpy(payload + qoff, &rs, 4);
    memcpy(payload + qoff + 4, cbuf, clen);
    free(cbuf);

    uint32_t offset = (uint32_t)enc->bitstream_size;
    int arc = append_to_bitstream(enc, payload, payload_size);
    free(payload);
    if (arc != QTSQ_OK) return arc;
    return append_to_index(enc, offset, (uint32_t)payload_size, 'K');
}

/* Scene-cut probe: mean absolute difference over an evenly spaced sample of
 * the frame. Cheap (a few thousand samples) and enough to spot a hard cut,
 * which the pixel path must not predict across — doing so produces a burst
 * of huge residuals (blowing the byte budget) and a badly smeared frame. */
static int px_scene_cut(const qtsq_video_encoder_t *enc, const uint8_t *pixels) {
    if (!enc->px_has_ref || !enc->px_ref_frame) return 0;
    size_t n = (size_t)enc->width * enc->height * enc->channels;
    if (n < 1024) return 0;
    size_t want = 4096;
    size_t step = n / want;
    if (step == 0) step = 1;
    const uint8_t *ref = enc->px_ref_frame;
    double sad = 0.0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i += step) {
        int d = (int)pixels[i] - (int)ref[i];
        sad += (d < 0 ? -d : d);
        cnt++;
    }
    if (cnt == 0) return 0;
    double mean = sad / (double)cnt;
    /* 42/255 (~16%) separates a cut from pan/flash. Require a few frames
     * since the last keyframe so a stutter can't force a GOP of 1. */
    return (mean > 42.0 && enc->frames_since_keyframe >= 4) ? 1 : 0;
}

/* Add one frame on the pixel path. Encodes immediately (no segment buffering). */
static int px_add_frame(qtsq_video_encoder_t *enc, const uint8_t *pixels) {
    if (enc->frame_count == 0) px_rate_bootstrap(enc);
    int is_key = (enc->frame_count == 0) ||
                 (enc->frames_since_keyframe >= enc->gop_size) ||
                 px_scene_cut(enc, pixels);
    size_t before = enc->bitstream_size;
    int rc;
    if (is_key) {
        rc = px_encode_iframe(enc, pixels);
        /* Count the keyframe itself so the next keyframe lands exactly
         * gop_size frames later (0, gop, 2*gop, …). Resetting to 0 here
         * made every GOP gop_size+1 frames long. */
        enc->frames_since_keyframe = 1;
    } else {
        rc = px_encode_pframe(enc, pixels);
        enc->frames_since_keyframe++;
    }
    if (rc != QTSQ_OK) return rc;

    /* Lossless: next ref = original pixels. Lossy: iframe/pframe already
     * wrote the closed-loop reconstruction into px_ref_frame. */
    if (!enc->lossy) {
        size_t fb = (size_t)enc->width * enc->height * enc->channels;
        memcpy(enc->px_ref_frame, pixels, fb);
    }
    enc->px_has_ref = 1;

    /* Sampled closed-loop PSNR (every 4th pixel): full-frame MSE at 4K
     * is ~25 M ops/frame; 1/16th is plenty for the hold threshold and
     * saves a visible slice of encode time over 5809 frames. */
    if (enc->lossy && pixels && enc->px_ref_frame) {
        size_t fb = (size_t)enc->width * enc->height * enc->channels;
        double mse = 0.0;
        size_t n = 0;
        for (size_t i = 0; i < fb; i += 4) {
            double d = (double)pixels[i] - (double)enc->px_ref_frame[i];
            mse += d * d;
            n++;
        }
        mse /= (double)(n ? n : 1);
        float p = (mse < 1e-12) ? 99.0f
                : (float)(10.0 * log10(255.0 * 255.0 / mse));
        enc->last_psnr = p;
        /* Tier-aware floor: HIGH keeps more margin; LOW must protect the
         * first P-frame — its I-frame is DCT (passes at q=0.15) but the
         * spatial P at the same q fell to 25.9 dB. Threshold 30 for LOW
         * forces a bump after f0 before any weak P-frame. */
        float hold_thresh = 29.0f;
        if (enc->quality_boot >= 0.25f)      hold_thresh = 30.0f;
        else if (enc->quality_boot <= 0.16f) hold_thresh = 30.0f;
        else                                 hold_thresh = 29.5f;
        if (p < hold_thresh) {
            enc->psnr_hold = 1;
        } else {
            if (enc->psnr_hold) {
                float locked = enc->quality * 0.95f;
                /* Tier-aware lock: HIGH floors near its boot so it stays
                 * larger; LOW locks to whatever cleared the floor. */
                float tier_floor = enc->quality_boot * 0.75f;
                if (locked < tier_floor) locked = tier_floor;
                if (locked < 0.15f) locked = 0.15f;
                if (locked > 0.45f) locked = 0.45f;
                enc->rc_q_good = locked;
            }
            if (enc->rc_q_good > 0.15f) {
                float rate;
                if (p >= 35.0f)      rate = 0.90f;
                else if (p >= 31.0f) rate = 0.95f;
                else                 rate = 0.985f;
                enc->rc_q_good *= rate;
                if (enc->rc_q_good < 0.15f) enc->rc_q_good = 0.15f;
            }
            enc->psnr_hold = 0;
        }
    }

    px_rate_control(enc, enc->bitstream_size - before);
    /* frame_count is incremented inside append_to_index — do not bump it here. */
    return QTSQ_OK;
}

int qtsq_video_encoder_add_frame(qtsq_video_encoder_t *enc, const uint8_t *pixels) {
    if (!enc || !pixels) return QTSQ_ERR_NULL;

    /* Pixel path: encode immediately (GOP I/P, no segment buffering) */
    return px_add_frame(enc, pixels);
}


int qtsq_video_encoder_set_audio(qtsq_video_encoder_t *enc,
                                 const int16_t *pcm, uint32_t total_samples,
                                 uint8_t channels, uint32_t sample_rate) {
    if (!enc || !pcm) return QTSQ_ERR_NULL;
    if (total_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (enc->audio_blob) return QTSQ_ERR_FORMAT; /* one track only */

    /* The gravitational coder writes straight into a context singularity, so
     * give it a scratch context and adopt the result. */
    qtsq_context_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    qtsq_init(&tmp);
    int rc = qtsq_compress_gravitational_audio(&tmp, pcm, total_samples,
                                               channels, sample_rate);
    if (rc != QTSQ_OK) {
        qtsq_free(&tmp);
        return rc;
    }
    if (!tmp.singularity || tmp.singularity_size == 0) {
        qtsq_free(&tmp);
        return QTSQ_ERR_FORMAT;
    }
    enc->audio_blob = tmp.singularity;
    enc->audio_blob_size = tmp.singularity_size;
    enc->audio_sample_rate = sample_rate;
    enc->audio_channels = channels ? channels : 1;
    /* tmp.singularity is now owned by the encoder; release the rest only. */
    tmp.singularity = NULL;
    tmp.singularity_size = 0;
    qtsq_free(&tmp);
    return QTSQ_OK;
}

int qtsq_video_encoder_finish(qtsq_video_encoder_t *enc) {
    if (!enc) return QTSQ_ERR_NULL;
    /* Pixel path encodes each frame immediately — nothing buffered to flush. */
    
    /* IDX1 has no audio section. IDX2 adds audio metadata + blob; readers that
     * only know IDX1 still parse the leading IDX2 magic and bail cleanly. */
    const int has_audio = (enc->audio_blob && enc->audio_blob_size > 0);
    const size_t idx_tail = (size_t)enc->frame_count * 4 * 2 + enc->frame_count;
    /* IDX1 = magic+count (8) + table. IDX2 = magic+count+audio_size+
     * audio_rate+channels/reserved (20) + table. */
    const size_t index_size = (has_audio ? 20 : 8) + idx_tail;
    const size_t total_payload = index_size +
                                 enc->bitstream_size +
                                 (has_audio ? enc->audio_blob_size : 0);
    /* Reusing a context that already holds a payload would orphan the old
     * buffer (qtsq_init only memsets), so release it before replacing. */
    if (enc->ctx->singularity) {
        free(enc->ctx->singularity);
        enc->ctx->singularity = NULL;
        enc->ctx->singularity_size = 0;
    }
    enc->ctx->singularity = (uint8_t *)malloc(total_payload);
    if (!enc->ctx->singularity) return QTSQ_ERR_ALLOC;
    enc->ctx->singularity_size = total_payload;

    uint8_t *p = enc->ctx->singularity;
    if (has_audio) {
        uint32_t a_sz = (uint32_t)enc->audio_blob_size;
        uint32_t a_rate = enc->audio_sample_rate;
        memcpy(p, "IDX2", 4); p += 4;
        memcpy(p, &enc->frame_count, 4); p += 4;
        memcpy(p, &a_sz, 4); p += 4;
        memcpy(p, &a_rate, 4); p += 4;
        p[0] = enc->audio_channels;
        p[1] = 0; p[2] = 0; p[3] = 0;   /* reserved, keeps 4-byte alignment */
        p += 4;
    } else {
        memcpy(p, "IDX1", 4); p += 4;
        memcpy(p, &enc->frame_count, 4); p += 4;
    }
    
    if (enc->frame_count > 0) {
        memcpy(p, enc->frame_offsets, enc->frame_count * 4); p += enc->frame_count * 4;
        memcpy(p, enc->frame_sizes, enc->frame_count * 4); p += enc->frame_count * 4;
        memcpy(p, enc->frame_types, enc->frame_count); p += enc->frame_count;
    }
    
    if (enc->bitstream_size > 0 && enc->bitstream) {
        memcpy(p, enc->bitstream, enc->bitstream_size);
        p += enc->bitstream_size;
    }
    if (has_audio) {
        memcpy(p, enc->audio_blob, enc->audio_blob_size);
    }
    /* Free the bitstream immediately after copying into singularity —
     * holding both for the rest of the process doubles peak RSS. */
    free(enc->bitstream);
    enc->bitstream = NULL;
    enc->bitstream_size = 0;
    enc->bitstream_capacity = 0;

    enc->ctx->header.original_size = (uint64_t)enc->width * enc->height * enc->channels * enc->frame_count;
    enc->ctx->header.singularity_size = total_payload;
    enc->ctx->header.horizon_size = sizeof(qtsq_schema_t);
    return QTSQ_OK;
}

qtsq_context_t* qtsq_video_encoder_get_context(qtsq_video_encoder_t *enc) {
    return enc ? enc->ctx : NULL;
}

void qtsq_video_encoder_free(qtsq_video_encoder_t *enc) {
    if (!enc) return;
    if (enc->bitstream) free(enc->bitstream);
    if (enc->audio_blob) free(enc->audio_blob);
    if (enc->frame_offsets) free(enc->frame_offsets);
    if (enc->frame_sizes) free(enc->frame_sizes);
    if (enc->frame_types) free(enc->frame_types);
    if (enc->px_ref_frame) free(enc->px_ref_frame);
    if (enc->px_recon) free(enc->px_recon);
    free(enc->px_scratch_modes);
    free(enc->px_scratch_mvs);
    free(enc->px_scratch_res_off);
    free(enc->px_scratch_residuals);
    free(enc->rc_win);
    free(enc);
}
