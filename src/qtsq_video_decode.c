#include "qtsq.h"
#include "qtsq_video_mv.h"
#include "qtsq_video_res.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

#include "qtsq_codec.h"

struct qtsq_video_decoder {
    const qtsq_context_t *ctx;
    uint32_t width;
    uint32_t height;
    uint8_t channels;
    
    uint32_t frame_count;
    uint32_t *frame_offsets;
    uint32_t *frame_sizes;
    uint8_t  *frame_types;
    
    const uint8_t *bitstream;
    size_t bitstream_size;

    /* Optional Classic Gravitational audio section (IDX2). */
    const uint8_t *audio_blob;
    size_t audio_blob_size;
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    
    /* Ring buffer (capacity 3) for output frames */
    uint8_t *ring_buffers[3];
    uint32_t ring_head;
    size_t   ring_buffer_size;
    
    /* Reference frame: last decoded I-frame for P-frame reconstruction */
    uint8_t *ref_frame;
    int has_ref_frame;
    uint32_t last_iframe_index; /* track which I-frame is loaded */
    int lossy;                  /* QTSQ_FLAG_LOSSY: residual = int8 quant levels */
};

/* ── RLE decoder for P-frame delta ──
 * Format matches the encoder:
 * [0xFF][high][low] = skip N unchanged pixels (all channels = 128)
 * [0xFE][ch0][ch1][ch2] = one changed pixel with biased delta values
 */

/* Decode a pixel-path P-frame ('P'): block motion compensation + residual.
 * Layout (matches px_encode_pframe), LOSSLESS:
 *   [1B mb_size][2B mb_cols][2B mb_rows][4B modes_mvs_len][4B res_blob_len]
 *   [modes_mvs stream] — per MB: [1B mode][optional 2B mvx/mvy]
 *   [residual blob] = [4B raw_total][qtc-or-zlib stream]
 * LOSSY (header flag QTSQ_FLAG_LOSSY): 14-byte header with quant at [1],
 * residual samples are int8 levels → dequant = level * quant.
 * Requires dec->ref_frame valid. Writes full frame and refreshes ref_frame. */
static int decode_pframe(qtsq_video_decoder_t *dec, uint32_t frame_index,
                         uint8_t *output, int pred_mvs, int sparse_residual) {
    uint32_t offset = dec->frame_offsets[frame_index];
    uint32_t size = dec->frame_sizes[frame_index];
    int lossy = dec->lossy;
    size_t hdr = lossy ? 14 : 13;
    if ((uint64_t)offset + size > dec->bitstream_size || size < hdr) return QTSQ_ERR_CORRUPT;
    if (!dec->has_ref_frame) return QTSQ_ERR_FORMAT;

    const uint8_t *data = dec->bitstream + offset;
    uint32_t w = dec->width, h = dec->height;
    uint8_t ch = dec->channels;
    size_t frame_bytes = (size_t)w * h * ch;

    uint8_t mb_size = data[0];
    if (mb_size == 0) return QTSQ_ERR_CORRUPT;
    int quant = 1;
    size_t p = 1;
    if (lossy) {
        quant = data[1];
        if (quant < 1) quant = 1;
        p = 2;
    }
    uint32_t mb_cols = data[p] | (data[p + 1] << 8);
    uint32_t mb_rows = data[p + 2] | (data[p + 3] << 8);
    p += 4;
    if (mb_cols == 0 || mb_rows == 0 || mb_cols > 65535 || mb_rows > 65535)
        return QTSQ_ERR_CORRUPT;
    if ((uint64_t)mb_cols * mb_rows > 10000000ULL) return QTSQ_ERR_CORRUPT;

    uint32_t mm_len, res_blob_len;
    memcpy(&mm_len, data + p, 4); p += 4;
    memcpy(&res_blob_len, data + p, 4); p += 4;
    size_t mm_pos = p;
    size_t res_pos = mm_pos + mm_len;
    if ((uint64_t)res_pos + res_blob_len > size) return QTSQ_ERR_CORRUPT;
    size_t mm_end = res_pos;

    /* Decompress modes+mvs once ([4B raw_len][qtc-or-zlib], same framing
     * as the residual blob). Empty mm_len → no MB side data (should not
     * happen for a real frame, but tolerate it). */
    uint8_t *mm_raw = NULL;
    size_t mm_raw_len = 0;
    if (mm_len > 0) {
        int mrc = qtsq_uncompress_bounded(data + mm_pos, mm_len,
                                          4096, (uint64_t)mb_cols * mb_rows * 8 + 64,
                                          &mm_raw, &mm_raw_len);
        if (mrc != QTSQ_OK) return QTSQ_ERR_CORRUPT;
    }

    /* Decompress residual blob once (qtc or zlib — sniffed inside helper). */
    uint8_t *res_raw = NULL;
    size_t res_raw_len = 0;
    if (res_blob_len > 0) {
        /* max_ratio high: post-quant residuals are zero-heavy and zlib
         * often exceeds the default 256:1 bound on synthetic/noisy clips. */
        int drc = qtsq_uncompress_bounded(data + res_pos, res_blob_len,
                                          8192, frame_bytes + 64,
                                          &res_raw, &res_raw_len);
        if (drc != QTSQ_OK) { free(mm_raw); return QTSQ_ERR_CORRUPT; }
    }

    const uint8_t *ref = dec->ref_frame;
    const uint8_t *mm_src = mm_raw;
    /* Scratch for one expanded sparse block. mb_size comes from the file, so
     * this is sized and bounds-checked rather than assumed. */
    size_t mb_res_cap = (size_t)mb_size * mb_size * ch;
    if (mb_res_cap == 0 || mb_res_cap > frame_bytes + 64) {
        free(mm_raw); free(res_raw);
        return QTSQ_ERR_CORRUPT;
    }
    uint8_t *sparse_blk = (uint8_t *)malloc(mb_res_cap);
    if (!sparse_blk) { free(mm_raw); free(res_raw); return QTSQ_ERR_ALLOC; }
    /* Bitpacked modes: 2 bits/MB MSB-first, then mv pairs for mode!=0. */
    size_t mode_bytes = ((size_t)mb_cols * mb_rows * 2 + 7) / 8;
    if (mm_src && mm_raw_len < mode_bytes) {
        free(mm_raw); free(res_raw);
        return QTSQ_ERR_CORRUPT;
    }
    size_t bit_pos = 0;
    size_t mv_pos = mode_bytes;
    size_t mm_end_pos = mm_raw_len;
    size_t res_off = 0;
    /* Reconstructed vectors, in raster order, so the median predictor sees
     * exactly what the encoder predicted from. */
    size_t nmb = (size_t)mb_cols * mb_rows;
    int16_t *mvrecon = (int16_t *)calloc(nmb * 2, sizeof(int16_t));
    if (!mvrecon) { free(mm_raw); free(res_raw); return QTSQ_ERR_ALLOC; }

    for (uint32_t my = 0; my < mb_rows; my++) {
        for (uint32_t mx = 0; mx < mb_cols; mx++) {
            if (!mm_src) {
                free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                return QTSQ_ERR_CORRUPT;
            }
            size_t byte_i = bit_pos / 8;
            int shift = 6 - (int)(bit_pos % 8);
            uint8_t mode = (mm_src[byte_i] >> shift) & 3;
            bit_pos += 2;

            size_t midx = (size_t)my * mb_cols + mx;
            int mvx = 0, mvy = 0;
            if (mode != 0) {
                if (mv_pos + 2 > mm_end_pos) {
                    free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                    return QTSQ_ERR_CORRUPT;
                }
                int dx = (int8_t)mm_src[mv_pos++];
                int dy = (int8_t)mm_src[mv_pos++];
                if (pred_mvs) {
                    int pvx, pvy;
                    qtsq_mv_predict(mvrecon, mb_cols, mx, my, &pvx, &pvy);
                    mvx = pvx + dx;
                    mvy = pvy + dy;
                } else {
                    mvx = dx;
                    mvy = dy;
                }
            } else if (mode != 0) {
                free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                return QTSQ_ERR_CORRUPT;
            }
            mvrecon[midx * 2] = (int16_t)mvx;
            mvrecon[midx * 2 + 1] = (int16_t)mvy;

            uint32_t bx = mx * mb_size;
            uint32_t by = my * mb_size;
            if (bx >= w || by >= h) continue;
            uint32_t bw = mb_size; if (bx + bw > w) bw = w - bx;
            uint32_t bh = mb_size; if (by + bh > h) bh = h - by;

            /* Clamp MV so source block stays in-bounds */
            int rx = (int)bx + mvx;
            int ry = (int)by + mvy;
            if (rx < 0) rx = 0;
            if (ry < 0) ry = 0;
            if (rx + (int)bw > (int)w) rx = (int)w - (int)bw;
            if (ry + (int)bh > (int)h) ry = (int)h - (int)bh;
            if (rx < 0 || ry < 0) { free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk); return QTSQ_ERR_CORRUPT; }

            /* Copy predicted block from reference */
            for (uint32_t y = 0; y < bh; y++) {
                memcpy(output + ((size_t)(by + y) * w + bx) * ch,
                       ref + ((size_t)(ry + y) * w + rx) * ch,
                       (size_t)bw * ch);
            }

            if (mode > 3 || (mode == 3 && !sparse_residual)) {
                free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                return QTSQ_ERR_CORRUPT;
            }
            if (mode == 2 || mode == 3) {
                size_t blen = (size_t)bw * bh * ch;
                const uint8_t *res_base;
                if (mode == 3) {
                    /* Sparse run form: expand one block into a dense scratch. */
                    if (!res_raw || res_off >= res_raw_len) {
                        free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                        return QTSQ_ERR_CORRUPT;
                    }
                    size_t used = qtsq_res_sparse_read(res_raw + res_off,
                                                       res_raw_len - res_off,
                                                       sparse_blk, blen);
                    if (used == 0) {
                        free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                        return QTSQ_ERR_CORRUPT;
                    }
                    res_off += used;
                    res_base = sparse_blk;
                } else {
                    if (!res_raw || res_off + blen > res_raw_len) {
                        free(mvrecon); free(mm_raw); free(res_raw); free(sparse_blk);
                        return QTSQ_ERR_CORRUPT;
                    }
                    res_base = res_raw + res_off;
                    res_off += blen;
                }
                for (uint32_t y = 0; y < bh; y++) {
                    uint8_t *out_row = output + ((size_t)(by + y) * w + bx) * ch;
                    const uint8_t *res_row = res_base + (size_t)y * bw * ch;
                    for (uint32_t i = 0; i < bw * ch; i++) {
                        if (lossy) {
                            int v = (int)out_row[i] + (int)(int8_t)res_row[i] * quant;
                            if (v < 0) v = 0;
                            if (v > 255) v = 255;
                            out_row[i] = (uint8_t)v;
                        } else {
                            out_row[i] = (uint8_t)(out_row[i] + res_row[i]);
                        }
                    }
                }
            }
        }
    }
    free(mm_raw);
    free(res_raw);
    free(sparse_blk);

    /* Refresh reference frame for next P-frame */
    memcpy(dec->ref_frame, output, frame_bytes);
    dec->has_ref_frame = 1;
    return QTSQ_OK;
}

qtsq_video_decoder_t* qtsq_video_decoder_open(const qtsq_context_t *ctx) {
    if (!ctx) { fprintf(stderr, "[QTSQ Decoder] ctx is NULL\n"); return NULL; }
    if (!ctx->singularity) { fprintf(stderr, "[QTSQ Decoder] singularity is NULL\n"); return NULL; }
    if (ctx->singularity_size < 8) { fprintf(stderr, "[QTSQ Decoder] singularity_size too small: %llu\n", (unsigned long long)ctx->singularity_size); return NULL; }
    if (ctx->header.data_type != QTSQ_TYPE_VIDEO_FRAMES) {
        fprintf(stderr, "[QTSQ Decoder] data_type mismatch: %d (expected %d)\n", ctx->header.data_type, QTSQ_TYPE_VIDEO_FRAMES);
        return NULL;
    }

    const uint8_t *p = ctx->singularity;
    int is_idx2 = (p[0] == 'I' && p[1] == 'D' && p[2] == 'X' && p[3] == '2');
    if (!is_idx2 && (p[0] != 'I' || p[1] != 'D' || p[2] != 'X' || p[3] != '1')) {
        fprintf(stderr, "[QTSQ Decoder] Error: Missing IDX1/IDX2 chunk. Got: %c%c%c%c\n", p[0], p[1], p[2], p[3]);
        return NULL;
    }
    /* IDX2 adds [4] audio_size [4] audio_rate [4] (channels|reserved<<8)
     * before the frame index. */
    uint32_t audio_size = 0, audio_rate = 0;
    uint8_t audio_channels = 0;
    if (is_idx2) {
        if (ctx->singularity_size < 24) {
            fprintf(stderr, "[QTSQ Decoder] IDX2 header truncated\n");
            return NULL;
        }
        memcpy(&audio_size, p + 8, 4);
        memcpy(&audio_rate, p + 12, 4);
        audio_channels = p[16];
    }
    fprintf(stderr, "[QTSQ Decoder] Opening: %u×%u, singularity=%llu bytes\n",
            ctx->schema.dimensions[0], ctx->schema.dimensions[1], (unsigned long long)ctx->singularity_size);

    qtsq_video_decoder_t *dec = (qtsq_video_decoder_t *)calloc(1, sizeof(qtsq_video_decoder_t));
    if (!dec) return NULL;

    dec->ctx = ctx;
    dec->width = ctx->schema.dimensions[0];
    dec->height = ctx->schema.dimensions[1];
    dec->channels = ctx->schema.dimensions[2];

    p += 4;
    memcpy(&dec->frame_count, p, 4); p += 4;
    if (is_idx2) p += 12;   /* audio_size, audio_rate, channels/reserved */

    /* SEC: frame_count comes from the file. The index tables are 9 bytes
     * per frame (4+4+1); validate the count against the remaining payload
     * before allocating or copying. Also cap absolute count. */
    {
        uint64_t remaining = (uint64_t)(ctx->singularity_size - (p - ctx->singularity));
        uint64_t index_bytes = (uint64_t)dec->frame_count * 9;
        if (dec->frame_count > 1000000 || index_bytes > remaining) {
            free(dec);
            return NULL;
        }
    }

    if (dec->frame_count > 0) {
        dec->frame_offsets = (uint32_t *)malloc(dec->frame_count * sizeof(uint32_t));
        dec->frame_sizes = (uint32_t *)malloc(dec->frame_count * sizeof(uint32_t));
        dec->frame_types = (uint8_t *)malloc(dec->frame_count * sizeof(uint8_t));

        if (!dec->frame_offsets || !dec->frame_sizes || !dec->frame_types) {
            qtsq_video_decoder_free(dec);
            return NULL;
        }

        memcpy(dec->frame_offsets, p, dec->frame_count * 4); p += dec->frame_count * 4;
        memcpy(dec->frame_sizes, p, dec->frame_count * 4); p += dec->frame_count * 4;
        memcpy(dec->frame_types, p, dec->frame_count); p += dec->frame_count;
    }

    dec->bitstream = p;
    dec->bitstream_size = ctx->singularity_size - (p - ctx->singularity);
    /* The audio blob trails the bitstream. Clamp it against what is actually
     * present so a corrupt size field cannot point outside the payload. */
    if (audio_size > dec->bitstream_size) {
        dec->audio_blob = NULL;
        dec->audio_blob_size = 0;
    } else if (audio_size > 0) {
        dec->audio_blob_size = audio_size;
        dec->bitstream_size -= audio_size;
        dec->audio_blob = (uint8_t *)p + dec->bitstream_size;
        dec->audio_sample_rate = audio_rate;
        dec->audio_channels = audio_channels ? audio_channels : 1;
    }
    
    /* Initialize ring buffers lazily */
    dec->ring_head = 0;
    dec->ring_buffer_size = 0;
    
    /* Allocate reference frame buffer */
    size_t frame_bytes = (size_t)dec->width * dec->height * dec->channels;
    /* SEC: width/height/channels come from the file header — cap the
     * reference frame size to prevent a huge calloc (DoS guard). */
    if (frame_bytes > (1ULL << 30)) {
        qtsq_video_decoder_free(dec);
        return NULL;
    }
    /* Enforce the shared working-set budget before touching the frame: blob
     * + three output ring buffers + reference plane + the DCT decode
     * transients must all fit, otherwise playback of a large file grows the
     * process without bound. */
    {
        size_t limit = qtsq_memory_limit_bytes();
        size_t need = (size_t)ctx->singularity_size;
        need += frame_bytes * 3;   /* output ring buffers */
        need += frame_bytes;       /* reference plane */
        need += frame_bytes * 16;  /* DCT decode transients (Y/Cb/Cr) */
        if (need < (size_t)ctx->singularity_size || need > limit) {
            fprintf(stderr, "[QTSQ Decoder] Refusing %.1f MB working set "
                    "(limit %.1f MB)\n", need / 1e6, limit / 1e6);
            qtsq_video_decoder_free(dec);
            return NULL;
        }
    }
    dec->ref_frame = (uint8_t *)calloc(1, frame_bytes);
    if (!dec->ref_frame) {
        qtsq_video_decoder_free(dec);
        return NULL;
    }
    dec->has_ref_frame = 0;
    dec->last_iframe_index = UINT32_MAX;
    dec->lossy = (ctx->header.flags & QTSQ_FLAG_LOSSY) != 0;
    
    return dec;
}

/* Simple 2x bilinear upscale for 'D' (downscaled) I-frames */
static uint8_t* upscale_2x(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint8_t ch) {
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * ch);
    if (!dst) return NULL;
    
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            /* Map destination pixel to source coordinates */
            float sx = (float)x * sw / dw;
            float sy = (float)y * sh / dh;
            uint32_t sx0 = (uint32_t)sx, sy0 = (uint32_t)sy;
            uint32_t sx1 = sx0 + 1 < sw ? sx0 + 1 : sx0;
            uint32_t sy1 = sy0 + 1 < sh ? sy0 + 1 : sy0;
            float fx = sx - sx0, fy = sy - sy0;
            
            for (uint8_t c = 0; c < ch; c++) {
                float v00 = src[(sy0 * sw + sx0) * ch + c];
                float v10 = src[(sy0 * sw + sx1) * ch + c];
                float v01 = src[(sy1 * sw + sx0) * ch + c];
                float v11 = src[(sy1 * sw + sx1) * ch + c];
                float val = v00 * (1-fx)*(1-fy) + v10 * fx*(1-fy) + v01 * (1-fx)*fy + v11 * fx*fy;
                dst[(y * dw + x) * ch + c] = (uint8_t)(val + 0.5f);
            }
        }
    }
    return dst;
}

/* Decode a keyframe.
 * 'S' = Splat keyframe (pure Gaussian Splats, Phase 10)
 * 'G' = GPU gravitational (vertices + triangles + splats, legacy)
 * 'K' = zlib raw pixels
 * 'I'/'D' = legacy gravitational
 */
static int decode_iframe(qtsq_video_decoder_t *dec, uint32_t frame_index, uint8_t *output) {
    uint32_t offset = dec->frame_offsets[frame_index];
    uint32_t size = dec->frame_sizes[frame_index];
    uint8_t type = dec->frame_types[frame_index];
    
    if ((uint64_t)offset + size > dec->bitstream_size || size < 4) return QTSQ_ERR_CORRUPT;
    
    const uint8_t *data = dec->bitstream + offset;
    size_t frame_bytes = (size_t)dec->width * dec->height * dec->channels;
    
    /* Read resolution header (common to K/I/D types) */
    uint16_t comp_w, comp_h;
    memcpy(&comp_w, data, 2);
    memcpy(&comp_h, data + 2, 2);
    
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    
    if (type == 'K') {
        /* Fast keyframe.
         * Lossless:     [2B w][2B h][4B raw_size][stream of raw pixels]
         * Lossy DCT:    [2B w][2B h][1B 0][4B raw_size][qtc image-DCT]
         *   quant byte 0 → decompress yields full pixels (no level scale).
         * Lossy spatial:[2B w][2B h][1B quant][4B raw_size][stream of LEVELS]
         *   levels → recon = level * quant (clamped). */
        int lossy = dec->lossy;
        size_t need = lossy ? 9 : 8;
        if (size < need) return QTSQ_ERR_CORRUPT;
        int quant = 1;
        int is_dct = 0;
        size_t stream_off = 4;
        if (lossy) {
            quant = data[4];
            if (quant == 0) {
                is_dct = 1; /* DCT sentinel: stream is full pixels */
                quant = 1;
            } else if (quant < 1) {
                quant = 1;
            }
            stream_off = 5;
        }
        uint8_t *dec_buf = NULL;
        size_t dec_len = 0;
        /* The 3rd argument is a max_ratio, not a min size. A flat keyframe
         * (fade to black, solid colour) turns 24.8 MB of levels into ~5 KB,
         * a ~4900:1 ratio, which the old 256:1 bound rejected as CORRUPT —
         * playback then spun retrying that frame. Bound the absolute size by
         * the frame size we already know, and keep the ratio generous. */
        uint64_t ratio_cap = is_dct ? 8192 : 65536;
        uint64_t abs_cap = is_dct ? (1ULL << 30) : (uint64_t)frame_bytes + 64;
        int drc = qtsq_uncompress_bounded(data + stream_off, size - stream_off,
                                          ratio_cap, abs_cap,
                                          &dec_buf, &dec_len);
        if (drc != QTSQ_OK) { free(dec_buf); return QTSQ_ERR_CORRUPT; }
        /* Spatial levels must cover the whole frame; a short stream would
         * leave the tail of the output buffer uninitialised. */
        if (!is_dct && dec_len != frame_bytes) {
            free(dec_buf);
            return QTSQ_ERR_CORRUPT;
        }
        if (lossy && !is_dct) {
            /* levels → reconstruction in place */
            for (size_t i = 0; i < dec_len; i++) {
                int r = (int)dec_buf[i] * quant;
                if (r > 255) r = 255;
                dec_buf[i] = (uint8_t)r;
            }
        }
        decoded = dec_buf;
        decoded_size = dec_len;
    } else if (type == 'R') {
        /* Raw Uncompressed Keyframe: [2B: w] [2B: h] [raw_data] */
        size_t raw_size = size - 4;
        decoded = (uint8_t *)malloc(raw_size);
        if (!decoded) return QTSQ_ERR_ALLOC;
        memcpy(decoded, data + 4, raw_size);
        decoded_size = raw_size;
    } else {
        /* Legacy gravitational: [2B: w] [2B: h] [gravitational_data] */
        qtsq_context_t temp_ctx;
        qtsq_init(&temp_ctx);
        temp_ctx.header = dec->ctx->header;
        temp_ctx.schema = dec->ctx->schema;
        temp_ctx.schema.dimensions[0] = comp_w;
        temp_ctx.schema.dimensions[1] = comp_h;
        
        size_t payload_size = size - 4;
        temp_ctx.singularity_size = payload_size;
        temp_ctx.singularity = (uint8_t *)malloc(payload_size);
        if (!temp_ctx.singularity) return QTSQ_ERR_ALLOC;
        memcpy(temp_ctx.singularity, data + 4, payload_size);
        
        int ret = qtsq_decompress_gravitational(&temp_ctx, &decoded, &decoded_size);
        free(temp_ctx.singularity);
        temp_ctx.singularity = NULL;
        qtsq_free(&temp_ctx);
        if (ret != QTSQ_OK) {
            if (decoded) free(decoded);
            return ret;
        }
    }
    
    /* If compressed at a smaller resolution, upscale to full output size */
    if (comp_w != dec->width || comp_h != dec->height) {
        uint8_t *upscaled = upscale_2x(decoded, comp_w, comp_h, dec->width, dec->height, dec->channels);
        free(decoded);
        if (!upscaled) return QTSQ_ERR_ALLOC;
        memcpy(output, upscaled, frame_bytes);
        free(upscaled);
    } else {
        size_t copy_size = decoded_size < frame_bytes ? decoded_size : frame_bytes;
        memcpy(output, decoded, copy_size);
        free(decoded);
    }
    
    return QTSQ_OK;
}

int qtsq_video_decoder_get_frame(qtsq_video_decoder_t *dec, uint32_t frame_index, uint8_t **out_pixels, uint32_t *width, uint32_t *height) {
    if (!dec || !out_pixels) return QTSQ_ERR_NULL;
    if (frame_index >= dec->frame_count) return QTSQ_ERR_IO;

    size_t frame_bytes = (size_t)dec->width * dec->height * dec->channels;
    
    /* Lazy-init ring buffers */
    if (dec->ring_buffer_size == 0) {
        dec->ring_buffer_size = frame_bytes;
        for (int i = 0; i < 3; i++) {
            dec->ring_buffers[i] = (uint8_t *)malloc(dec->ring_buffer_size);
        }
    }
    
    uint8_t *local_buffer = dec->ring_buffers[dec->ring_head];
    dec->ring_head = (dec->ring_head + 1) % 3;
    *out_pixels = local_buffer;

    uint8_t type = dec->frame_types[frame_index];
    int ret = QTSQ_OK;
    
    /* 'I'/'D'/'K' = keyframe types */
    if (type == 'I' || type == 'D' || type == 'K') {
        /* I-frame: decode and update reference */
        ret = decode_iframe(dec, frame_index, local_buffer);
        if (ret == QTSQ_OK && type == 'K') {
            /* Pixel-path keyframe: refresh ref_frame for subsequent P-frames */
            memcpy(dec->ref_frame, local_buffer, frame_bytes);
            dec->has_ref_frame = 1;
            dec->last_iframe_index = frame_index;
        }

    } else if (type == 'P' || type == 'p' || type == 'q') {
        /* 'p' = median-predicted MV deltas (current encoder).
         * 'P' = raw 2-byte vectors (files written before the predictor).
         * Both are accepted so older .qtsq files keep decoding. */
        /* Pixel-path P-frame: needs the most recent keyframe as reference.
         * If ref_frame isn't already the immediately-previous frame, seek
         * back to the nearest 'K' and decode forward (GOP walk). */
        int ref_ok = dec->has_ref_frame && dec->last_iframe_index == frame_index - 1;
        if (!ref_ok) {
            uint32_t kf = frame_index;
            while (kf > 0 && dec->frame_types[kf] != 'K') kf--;
            if (dec->frame_types[kf] != 'K') return QTSQ_ERR_FORMAT;
            /* Decode keyframe into ref_frame */
            ret = decode_iframe(dec, kf, dec->ref_frame);
            if (ret != QTSQ_OK) return ret;
            dec->has_ref_frame = 1;
            dec->last_iframe_index = kf;
            if (kf == frame_index) {
                memcpy(local_buffer, dec->ref_frame, frame_bytes);
            } else {
                /* Walk forward P-frames kf+1 .. frame_index-1, then decode
                 * target. Intermediate frames must NOT write into ref_frame
                 * while reading it (aliasing) — use local_buffer as scratch
                 * for each step; decode_pframe refreshes ref_frame from its
                 * output at the end. */
                for (uint32_t i = kf + 1; i < frame_index; i++) {
                    ret = decode_pframe(dec, i, local_buffer,
                                       dec->frame_types[i] == 'p' ||
                                       dec->frame_types[i] == 'q',
                                       dec->frame_types[i] == 'q');
                    if (ret != QTSQ_OK) return ret;
                    /* ref_frame now holds frame i (updated inside decode_pframe) */
                }
                ret = decode_pframe(dec, frame_index, local_buffer,
                                   dec->frame_types[frame_index] == 'p' ||
                                   dec->frame_types[frame_index] == 'q',
                                   dec->frame_types[frame_index] == 'q');
                if (ret == QTSQ_OK) {
                    dec->last_iframe_index = frame_index;
                }
            }
        } else {
            /* Sequential decode: ref_frame is previous frame */
            ret = decode_pframe(dec, frame_index, local_buffer,
                               type == 'p' || type == 'q', type == 'q');
            if (ret == QTSQ_OK) dec->last_iframe_index = frame_index;
        }

    } else if (type == 'B') {
        /* Block-delta frame — fast path: copy ref + patch blocks (legacy, incomplete) */
        if (!dec->has_ref_frame) {
            /* Seek back to nearest keyframe */
            uint32_t search = frame_index;
            while (search > 0 && dec->frame_types[search] != 'I' && dec->frame_types[search] != 'D' && dec->frame_types[search] != 'K') {
                search--;
            }
            ret = decode_iframe(dec, search, dec->ref_frame);
            if (ret != QTSQ_OK) return ret;
            dec->has_ref_frame = 1;
            dec->last_iframe_index = search;
        }

    } else {
        ret = QTSQ_ERR_STRATEGY; /* Unknown frame type */
    }

    if (ret == QTSQ_OK) {
        if (width) *width = dec->width;
        if (height) *height = dec->height;
    }

    return ret;
}

uint32_t qtsq_video_decoder_get_frame_count(qtsq_video_decoder_t *dec) {
    if (!dec) return 0;
    return dec->frame_count;
}

uint8_t qtsq_video_decoder_get_frame_type(qtsq_video_decoder_t *dec, uint32_t frame_index) {
    if (!dec || frame_index >= dec->frame_count) return 0;
    return dec->frame_types[frame_index];
}

int qtsq_video_decoder_has_audio(qtsq_video_decoder_t *dec) {
    return (dec && dec->audio_blob && dec->audio_blob_size > 0) ? 1 : 0;
}

int qtsq_video_decoder_get_audio(qtsq_video_decoder_t *dec,
                                 uint8_t **out_wav, size_t *out_wav_size,
                                 uint32_t *out_sample_rate,
                                 uint8_t *out_channels) {
    if (!dec || !out_wav || !out_wav_size) return QTSQ_ERR_NULL;
    if (!dec->audio_blob || dec->audio_blob_size == 0) return QTSQ_ERR_FORMAT;

    /* The gravitational decoder reads the blob out of a context singularity,
     * so hand it a scratch context rather than duplicating the parser. */
    qtsq_context_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    qtsq_init(&tmp);
    tmp.singularity = (uint8_t *)malloc(dec->audio_blob_size);
    if (!tmp.singularity) { qtsq_free(&tmp); return QTSQ_ERR_ALLOC; }
    memcpy(tmp.singularity, dec->audio_blob, dec->audio_blob_size);
    tmp.singularity_size = dec->audio_blob_size;

    int rc = qtsq_decompress_gravitational_audio(&tmp, out_wav, out_wav_size);
    qtsq_free(&tmp);
    if (rc != QTSQ_OK) return rc;

    if (out_sample_rate) *out_sample_rate = dec->audio_sample_rate;
    if (out_channels) *out_channels = dec->audio_channels;
    return QTSQ_OK;
}

const qtsq_context_t* qtsq_video_decoder_get_context(qtsq_video_decoder_t *dec) {
    if (!dec) return NULL;
    return dec->ctx;
}

void qtsq_video_decoder_free(qtsq_video_decoder_t *dec) {
    if (!dec) return;
    if (dec->frame_offsets) free(dec->frame_offsets);
    if (dec->frame_sizes) free(dec->frame_sizes);
    if (dec->frame_types) free(dec->frame_types);
    if (dec->ref_frame) free(dec->ref_frame);
    for (int i = 0; i < 3; i++) {
        if (dec->ring_buffers[i]) free(dec->ring_buffers[i]);
    }
    free(dec);
}
