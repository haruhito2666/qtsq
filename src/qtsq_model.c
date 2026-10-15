/*
 * Quantum Tensor Sequence — Model Container implementation.
 * See include/qtsq_model.h for layout.
 */

#include <string.h>
#include <stdlib.h>
#include "qtsq.h"
#include "qtsq_model.h"
#include "qtsq_keystore.h"
#include "qtsq_codec.h"
#include "qtc.h"
#include "qtsq_image_file.h"

int qtsq_model_create(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    return qtsq_container_create(ctx);
}

int qtsq_model_add_f32(qtsq_context_t *ctx,
                        const char *name,
                        const float *data, size_t count,
                        const uint32_t *dims, uint8_t ndims,
                        int level) {
    if (!ctx || !name || !data) return QTSQ_ERR_NULL;
    if (count == 0) return QTSQ_ERR_TOO_SMALL;

    qtsq_context_t sub;
    /* RAW = true bit-exact (no sanitize, NaN/Inf preserved). */
    int r = qtsq_model_compress_one(&sub, (const uint8_t *)data,
                                    count * sizeof(float),
                                    dims, ndims, 1, 4, 0, level);
    if (r != QTSQ_OK) { qtsq_free(&sub); return r; }
    r = qtsq_container_add_stream_take(ctx, &sub, name);
    qtsq_free(&sub);
    return r;
}

int qtsq_model_compress_one(qtsq_context_t *sub,
                            const uint8_t *buf, size_t blen,
                            const uint32_t *dims, uint8_t ndims,
                            int is_f32, long elem, int nibble, int level) {
    if (!sub || !buf) return QTSQ_ERR_NULL;
    if (blen == 0) return QTSQ_ERR_TOO_SMALL;
    qtsq_init(sub);
    if (is_f32) {
        if (blen % 4 != 0) return QTSQ_ERR_FORMAT;
        return qtsq_compress_tensor_quantized(sub, (const float *)buf,
                                             blen / 4, dims, ndims,
                                             QTSQ_TENSOR_RAW, level);
    }
    uint8_t el = (elem == 1 || elem == 2 || elem == 4 || elem == 8)
                 ? (uint8_t)elem : 1;
    if (blen % el != 0) return QTSQ_ERR_FORMAT;
    return qtsq_compress_array(sub, buf, blen / el, el, dims, ndims,
                               nibble ? 1 : 0, level);
}

int qtsq_model_add_bytes(qtsq_context_t *ctx,
                          const char *name,
                          const uint8_t *raw, size_t raw_size,
                          uint8_t elem_size,
                          const uint32_t *dims, uint8_t ndims,
                          int packed_nibbles, int level) {
    if (!ctx || !name || !raw) return QTSQ_ERR_NULL;
    if (raw_size == 0) return QTSQ_ERR_TOO_SMALL;
    if (elem_size != 1 && elem_size != 2 &&
        elem_size != 4 && elem_size != 8)
        return QTSQ_ERR_FORMAT;
    if (raw_size % elem_size != 0) return QTSQ_ERR_FORMAT;
    if (ndims > 8) return QTSQ_ERR_FORMAT;

    qtsq_context_t sub;
    /* qtc array lens: transpose + XOR-delta contest, bit-exact. */
    int r = qtsq_model_compress_one(&sub, raw, raw_size, dims, ndims,
                                    0, elem_size, packed_nibbles, level);
    if (r != QTSQ_OK) { qtsq_free(&sub); return r; }
    r = qtsq_container_add_stream_take(ctx, &sub, name);
    qtsq_free(&sub);
    return r;
}

/* Store-mode raw add: TNS2 header bytes are prebuilt here (owned),
 * the payload stays borrowed. Byte-identical to the framed store
 * path — the lens framing is emitted as header parts instead of a
 * copied buffer. hash32 is the caller-computed SHA-256 over the inner
 * bytes ([hdr6][raw] for f32, [hdr6][raw] for arrays — never covering
 * the [u64] length prefix, mirroring the decoders); integrity of the
 * file itself is verified on load as usual. */
/* Inner hash for the store path: FIPS 180-4 SHA-256 over the exact
 * bytes the decoders verify ([u32 count][mode][elem][raw]) — the same
 * digest qtsq_sha256 produces (SHA3 is never used for TNS2 hashes).
 * Thread-safe (stack + caller buffers only) — workers hash in parallel. */
int qtsq_model_hash_inner(uint8_t out32[32],
                          const uint8_t *data, size_t raw_len,
                          int is_f32, uint8_t elem, uint32_t count) {
    if (!out32 || (!data && raw_len)) return QTSQ_ERR_NULL;
    uint8_t hdr[6];
    hdr[0] = (uint8_t)count;
    hdr[1] = (uint8_t)(count >> 8);
    hdr[2] = (uint8_t)(count >> 16);
    hdr[3] = (uint8_t)(count >> 24);
    hdr[4] = is_f32 ? 7 : 3;
    hdr[5] = is_f32 ? 0 : elem;
    qtsq_sha256_state_t hs;
    if (qtsq_sha256_init(&hs) != QTSQ_OK) return QTSQ_ERR_ALLOC;
    if (qtsq_sha256_update(&hs, hdr, 6) != QTSQ_OK) return QTSQ_ERR_ALLOC;
    if (raw_len && qtsq_sha256_update(&hs, data, raw_len) != QTSQ_OK)
        return QTSQ_ERR_ALLOC;
    if (qtsq_sha256_final(&hs, out32) != QTSQ_OK) return QTSQ_ERR_ALLOC;
    return QTSQ_OK;
}

int qtsq_model_add_raw_hashed(qtsq_context_t *ctx,
                              const char *name,
                              const uint8_t *data, size_t raw_len,
                              int is_f32, uint8_t elem_size,
                              const uint32_t *dims, uint8_t ndims,
                              const uint8_t hash32[32]) {
    if (!ctx || !name || !hash32) return QTSQ_ERR_NULL;
    if ((!data && raw_len) || raw_len == 0) return QTSQ_ERR_TOO_SMALL;
    if (ndims > 8) return QTSQ_ERR_FORMAT;
    uint8_t el = is_f32 ? 4 : elem_size;
    if (el != 1 && el != 2 && el != 4 && el != 8) return QTSQ_ERR_FORMAT;
    if (raw_len % el != 0) return QTSQ_ERR_FORMAT;
    size_t count = raw_len / el;

    /* Inner lens framing for store level (verbatim): f32 → mode 7,
     * arrays → mode 3 STORED. */
    uint8_t inner_hdr[6];
    uint64_t icount = (uint64_t)count;
    if (count > 0xFFFFFFFFu) return QTSQ_ERR_TOO_LARGE;
    inner_hdr[0] = (uint8_t)icount;
    inner_hdr[1] = (uint8_t)(icount >> 8);
    inner_hdr[2] = (uint8_t)(icount >> 16);
    inner_hdr[3] = (uint8_t)(icount >> 24);
    inner_hdr[4] = is_f32 ? 7 : 3;
    inner_hdr[5] = is_f32 ? 0 : el;

    /* TNS2 header: magic(4) mode(1) ndims(1) res(2) count(8) scale(8)
     * first(8) dims hash(32) uncomp(8). Array modes carry the extra
     * [u64 inner_len] prefix inside the header part. */
    size_t hlen = 4 + 1 + 1 + 2 + 8 + 8 + 8 + (size_t)ndims * 4 + 32 + 8;
    size_t extra = is_f32 ? 0 : 8;
    uint8_t *rhead = (uint8_t *)malloc(hlen + extra + 6);
    if (!rhead) return QTSQ_ERR_ALLOC;
    size_t p = 0;
    rhead[p++] = 'T';
    rhead[p++] = 'N';
    rhead[p++] = 'S';
    rhead[p++] = '2';
    rhead[p++] = is_f32 ? 1 : (el == 1 ? 5 : el == 2 ? 6 : el == 4 ? 7 : 8);
    rhead[p++] = ndims;
    rhead[p++] = 1; /* framed-lens marker (reserved[0]) */
    rhead[p++] = 0;
    uint64_t c64 = (uint64_t)count;
    memcpy(rhead + p, &c64, 8); p += 8;
    double zero = 0.0;
    memcpy(rhead + p, &zero, 8); p += 8;
    memcpy(rhead + p, &zero, 8); p += 8;
    if (dims && ndims > 0)
        memcpy(rhead + p, dims, ndims * 4);
    p += ndims * 4;
    /* Trust-but-note: hash32 must be SHA-256 over the inner bytes
     * ([hdr6][raw]); file integrity is verified on load regardless. */
    memcpy(rhead + p, hash32, 32); p += 32;
    uint64_t uncomp = (uint64_t)raw_len;
    memcpy(rhead + p, &uncomp, 8); p += 8;
    if (!is_f32) {
        uint64_t il = (uint64_t)(6 + raw_len);
        memcpy(rhead + p, &il, 8); p += 8;
    }
    memcpy(rhead + p, inner_hdr, 6); p += 6;

    /* Verify hash input layout matches the framed store path:
     * f32 inner = [hdr6][raw]; array inner = [u64][hdr6][raw]. */
    int r = qtsq_container_add_raw(ctx, name, rhead, p, data, raw_len,
                                   QTSQ_TYPE_TENSOR,
                                   QTSQ_STRATEGY_TENSOR,
                                   QTSQ_FLAG_CHECKSUMMED,
                                   (uint64_t)raw_len);
    if (r != QTSQ_OK) free(rhead);
    return r;
}

int qtsq_model_blob_kind(const qtsq_blob_header_t *hdr) {
    if (!hdr) return -1;
    if (hdr->magic[0] != QTSQ_BLOB_MAGIC_0 || hdr->magic[1] != QTSQ_BLOB_MAGIC_1 ||
        hdr->magic[2] != QTSQ_BLOB_MAGIC_2 || hdr->magic[3] != QTSQ_BLOB_MAGIC_3)
        return -1;
    if (hdr->kind > QTSQ_KIND_TABLE) return -1;
    return (int)hdr->kind;
}

/* Build a BLB1 payload: [64B header][compressed bytes].
 *
 * policy KEEP bypasses compression entirely and stores the bytes
 * verbatim after the header — the default for encoded files, where
 * re-encoding can easily make the payload BIGGER (lossless pixels from a
 * lossy JPEG). The other policies are for callers that have already
 * chosen to transcode.
 *
 * The SHA-256 covers the ORIGINAL bytes, so integrity is checkable from
 * the header for a kept payload and after decode for a coded one. */
/* Image geometry: a raster payload is width × height × channels ×
 * bytes_per_sample, and the channel count is implied rather than passed
 * separately so a caller cannot hand us a triple that does not match the
 * bytes. Returns 0 (and leaves the outputs untouched) for any payload
 * that is not an exact multiple of a 1/2/3/4-channel raster, which sends
 * the caller down the generic-codec path instead. */
static int image_geometry(size_t raw_size, uint32_t w, uint32_t h,
                          uint8_t bit_depth, uint32_t *ow, uint32_t *oh,
                          uint8_t *och) {
    if (w == 0 || h == 0) return 0;
    unsigned bps = (bit_depth == 0) ? 8u : (unsigned)bit_depth;
    if (bps != 8 && bps != 16) return 0;          /* image lens does 8/16 */
    uint64_t px = (uint64_t)w * (uint64_t)h;
    if (px == 0 || px > (uint64_t)1 << 32) return 0;
    uint64_t per_px = px * (bps / 8u);
    if (raw_size < per_px || (raw_size % per_px) != 0) return 0;
    uint64_t ch = raw_size / per_px;
    if (ch < 1 || ch > 4) return 0;
    *ow = w; *oh = h; *och = (uint8_t)ch;
    return 1;
}

/* Audio geometry: raw_size must be exactly frames x channels x
 * bytes_per_sample for the given layout, so a caller cannot hand us a
 * frame count that disagrees with the bytes. Returns 0 when the lens does
 * not support this shape, which sends the caller to the generic coder
 * rather than mis-framing the audio. */
static int audio_geometry(size_t raw_size, uint32_t channels,
                          uint8_t bit_depth, uint32_t declared_frames,
                          uint32_t *out_frames)
{
    /* qtsq is stereo-only. Mono is never stored: it would occupy the
     * same space as the stereo pair (the mid/side coder needs both
     * channels to decorrelate) while costing twice the bits per
     * second of real content. Callers duplicate a mono source instead. */
    if (channels != 2) return 0;
    if (bit_depth != 8 && bit_depth != 16 && bit_depth != 24) return 0;
    size_t frame_bytes = (size_t)channels * (bit_depth / 8u);
    if (raw_size == 0 || (raw_size % frame_bytes) != 0) return 0;
    uint32_t frames = (uint32_t)(raw_size / frame_bytes);
    /* A stated frame count that does not divide the payload is a caller
     * bug; rejecting it beats writing a stream that decodes to the wrong
     * length. */
    if (declared_frames && declared_frames != frames) return 0;
    *out_frames = frames;
    return 1;
}

/* Encode a raster with the qtc image lens (lossless 2D prediction), or
 * verbatim when the caller asked for store level. */
static int image_encode(const uint8_t *raw, uint32_t w, uint32_t h,
                        uint8_t ch, uint8_t bit_depth, int store,
                        uint8_t **out, size_t *out_size) {
    *out = NULL; *out_size = 0;
    if (store) return QTSQ_ERR_STRATEGY;   /* caller falls back to keep */
    qtc_options_t o;
    qtc_options_default(&o);
    o.mode = QTC_MODE_LOSSLESS;            /* no quality > 0 ⇒ lossless */
    int rc;
    if (bit_depth == 16) {
        rc = qtc_compress_image16((const uint16_t *)raw, w, h, ch, out, out_size, &o);
    } else {
        /* Lossless-only entry point: qtc_compress_image runs a lossy
         * rate-distortion contest, and these pixels must round-trip
         * exactly (they were just transcoded out of a PNG/BMP). */
        rc = qtc_compress_image_lossless(raw, w, h, ch, out, out_size, &o);
    }
    if (rc != QTC_OK) { *out = NULL; *out_size = 0; return QTSQ_ERR_STRATEGY; }
    return QTSQ_OK;
}

static int blob_build(const uint8_t *raw, size_t raw_size,
                      uint8_t kind, uint16_t content_type,
                      uint32_t param0, uint32_t param1, uint32_t channels,
                      uint8_t bit_depth, uint8_t policy, int level,
                      uint8_t **out, size_t *out_size,
                      uint8_t *out_codec, uint16_t *out_flags) {
    uint32_t iw = 0, ih = 0, iframes = 0; uint8_t ich = 0;
    if (!out || !out_size || !raw || raw_size == 0) return QTSQ_ERR_NULL;
    *out = NULL; *out_size = 0;

    qtsq_blob_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = QTSQ_BLOB_MAGIC_0; hdr.magic[1] = QTSQ_BLOB_MAGIC_1;
    hdr.magic[2] = QTSQ_BLOB_MAGIC_2; hdr.magic[3] = QTSQ_BLOB_MAGIC_3;
    hdr.kind = kind;
    hdr.content_type = content_type;
    hdr.bit_depth = bit_depth;
    hdr.policy = policy;
    hdr.param0 = param0;
    hdr.param1 = param1;
    hdr.channels = channels;
    hdr.original_size = (uint64_t)raw_size;
    qtsq_sha256(raw, raw_size, hdr.sha256);

    uint8_t *body = NULL;
    size_t body_len = 0;
    uint8_t codec = QTSQ_BLOB_CODEC_NONE;
    uint16_t flags = 0;

    if (policy == QTSQ_BLOB_POLICY_KEEP) {
        body = (uint8_t *)malloc(raw_size);
        if (!body) return QTSQ_ERR_ALLOC;
        memcpy(body, raw, raw_size);
        body_len = raw_size;
        codec = QTSQ_BLOB_CODEC_NONE;
        flags |= QTSQ_BLOB_FLAG_STORED;
    } else {
        /* Text runs the qtc text lens directly so the payload is
         * self-identifying even when it is handed to a reader that does
         * not run the detector; everything else uses the generic contest,
         * which elects the text lens itself when it sees text. */
        qtsq_context_t sub;
        qtsq_init(&sub);
        if (level == 2) sub.codec = QTSQ_CODEC_QTC;
        int r = QTSQ_ERR_ALLOC;
        int codec_is_image = 0, codec_is_audio = 0, codec_is_table = 0;
        if (kind == QTSQ_KIND_TEXT) {
            qtc_options_t o;
            qtc_options_default(&o);
            if (level == 1) o.level = 1;
            else if (level == 2) o.level = 0;
            r = qtc_compress_text(raw, raw_size, &body, &body_len, &o);
        } else if (kind == QTSQ_KIND_TABLE && level != 2) {
            /* Table lens: contests row-major against column-major and
             * keeps the smaller, so it can never lose to the text lens
             * alone. Delegates to qtc_compress_table, which composes the
             * text lens for each candidate. */
            qtc_options_t o;
            qtc_options_default(&o);
            if (level == 1) o.level = 1;
            r = qtc_compress_table(raw, raw_size, &body, &body_len, &o);
            codec_is_table = (r == QTC_OK);
        } else if (kind == QTSQ_KIND_AUDIO && audio_geometry(raw_size, channels,
                                                             bit_depth, param1,
                                                             &iframes)) {
            /* Audio lens: mid/side + linear prediction for integer PCM.
             * Bit-exact at level 0 and far stronger on speech-grade
             * material than a byte-level coder. */
            qtc_options_t o;
            qtc_options_default(&o);
            o.mode = QTC_MODE_LOSSLESS;
            if (level == 1) o.level = 1;
            /* The audio lenses do not consult opts->level for a candidate
             * set (only the tensor lenses do), so "store" is honoured here
             * by keeping the samples instead of asking the lens at all. */
            if (level == 2) {
                body = (uint8_t *)malloc(raw_size);
                if (!body) return QTSQ_ERR_ALLOC;
                memcpy(body, raw, raw_size);
                body_len = raw_size;
                codec = QTSQ_BLOB_CODEC_NONE;
                flags |= QTSQ_BLOB_FLAG_STORED;
            } else {
                r = qtc_compress_audio(raw, iframes, (uint8_t)channels,
                                       bit_depth, &body, &body_len, &o);
                codec_is_audio = (r == QTC_OK);
            }
        } else if (kind == QTSQ_KIND_IMAGE && image_geometry(raw_size, param0, param1,
                                                             bit_depth, &iw, &ih, &ich)) {
            /* Native raster: the image lens (2D MED prediction) beats
             * the generic byte coders by a wide margin on photographs and
             * screenshots. param0 = width, param1 = height; the channel
             * count follows from the payload length so the caller cannot
             * pass an inconsistent triple. */
            r = image_encode(raw, iw, ih, ich, bit_depth, level == 2,
                             &body, &body_len);
            codec_is_image = (r == QTSQ_OK);
        } else {
            r = qtsq_codec_compress(raw, raw_size, &body, &body_len);
        }
        if (r != QTSQ_OK || !body || body_len == 0) {
            free(body); qtsq_free(&sub);
            return r != QTSQ_OK ? r : QTSQ_ERR_ALLOC;
        }
        codec = (kind == QTSQ_KIND_TEXT) ? QTSQ_BLOB_CODEC_TEXT
              : codec_is_image ? QTSQ_BLOB_CODEC_IMAGE
              : codec_is_audio ? QTSQ_BLOB_CODEC_AUDIO
              : codec_is_table ? QTSQ_BLOB_CODEC_TABLE
              : QTSQ_BLOB_CODEC_QTC;
        if (body_len >= raw_size) {
            /* Nothing won: keep the bytes rather than ship a payload
             * that is no smaller than the input. */
            free(body);
            body = (uint8_t *)malloc(raw_size);
            if (!body) return QTSQ_ERR_ALLOC;
            memcpy(body, raw, raw_size);
            body_len = raw_size;
            codec = QTSQ_BLOB_CODEC_NONE;
            flags |= QTSQ_BLOB_FLAG_STORED;
        }
        qtsq_free(&sub);
    }

    uint8_t *buf = (uint8_t *)malloc(sizeof(hdr) + body_len);
    if (!buf) { free(body); return QTSQ_ERR_ALLOC; }
    hdr.codec = codec;
    hdr.flags = flags;
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), body, body_len);
    free(body);

    *out = buf;
    *out_size = sizeof(hdr) + body_len;
    if (out_codec) *out_codec = codec;
    if (out_flags) *out_flags = flags;
    return QTSQ_OK;
}

int qtsq_model_decompress_blob(const uint8_t *in, size_t in_len,
                               uint8_t **out, size_t *out_len,
                               qtsq_blob_header_t *out_hdr) {
    if (!in || !out || !out_len) return QTSQ_ERR_NULL;
    *out = NULL; *out_len = 0;
    if (in_len < sizeof(qtsq_blob_header_t)) return QTSQ_ERR_FORMAT;

    qtsq_blob_header_t hdr;
    memcpy(&hdr, in, sizeof(hdr));
    if (hdr.magic[0] != QTSQ_BLOB_MAGIC_0 || hdr.magic[1] != QTSQ_BLOB_MAGIC_1 ||
        hdr.magic[2] != QTSQ_BLOB_MAGIC_2 || hdr.magic[3] != QTSQ_BLOB_MAGIC_3)
        return QTSQ_ERR_FORMAT;
    if (hdr.kind > QTSQ_KIND_TABLE) return QTSQ_ERR_FORMAT;
    /* original_size must be representable and must not exceed what the
     * stored payload could plausibly reconstruct. */
    if (hdr.original_size == 0 || hdr.original_size > (uint64_t)1 << 40)
        return QTSQ_ERR_FORMAT;

    const uint8_t *body = in + sizeof(hdr);
    size_t body_len = in_len - sizeof(hdr);
    size_t n = (size_t)hdr.original_size;

    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return QTSQ_ERR_ALLOC;

    int rc = QTSQ_OK;
    if (hdr.codec == QTSQ_BLOB_CODEC_NONE) {
        if (body_len != n) { free(buf); return QTSQ_ERR_FORMAT; }
        memcpy(buf, body, n);
    } else if (hdr.codec == QTSQ_BLOB_CODEC_TEXT) {
        rc = qtc_decompress_text(body, body_len, buf, n);
        if (rc != QTC_OK) { free(buf); return QTSQ_ERR_FORMAT; }
    } else if (hdr.codec == QTSQ_BLOB_CODEC_IMAGE) {
        /* The image lens records w/h/ch inside its own payload, so the
         * caller can hand us just the header's geometry back and have it
         * checked rather than trusted. */
        uint8_t *px = NULL;
        uint32_t ow = 0, oh = 0;
        uint8_t och = 0;
        rc = qtc_decompress_image(body, body_len, &px, &ow, &oh, &och);
        if (rc != QTC_OK || !px) { free(px); free(buf); return QTSQ_ERR_FORMAT; }
        /* Geometry must agree with the header; a mismatch means the
         * header and the payload were written by different code. */
        if ((hdr.param0 && ow != hdr.param0) ||
            (hdr.param1 && oh != hdr.param1)) {
            free(px); free(buf); return QTSQ_ERR_FORMAT;
        }
        uint64_t expect = (uint64_t)ow * oh * (uint64_t)och *
                          (hdr.bit_depth == 16 ? 2u : 1u);
        if (expect != hdr.original_size || (size_t)expect != n) {
            free(px); free(buf); return QTSQ_ERR_FORMAT;
        }
        memcpy(buf, px, n);
        free(px);
    } else if (hdr.codec == QTSQ_BLOB_CODEC_TABLE) {
        rc = qtc_decompress_table(body, body_len, buf, n);
        if (rc != QTC_OK) { free(buf); return QTSQ_ERR_FORMAT; }
    } else if (hdr.codec == QTSQ_BLOB_CODEC_AUDIO) {
        /* The audio lens records frames/channels/bits-per-sample inside
         * its own payload, so the decoder's values are the authority and
         * the header's are checked against them. */
        uint8_t *pcm = NULL;
        uint32_t nfr = 0;
        uint8_t och = 0, obps = 0;
        rc = qtc_decompress_audio(body, body_len, &pcm, &nfr, &och, &obps);
        if (rc != QTC_OK || !pcm) { free(pcm); free(buf); return QTSQ_ERR_FORMAT; }
        uint64_t got = (uint64_t)nfr * och * (uint64_t)(obps / 8u);
        if (got != hdr.original_size || (size_t)got != n) {
            free(pcm); free(buf); return QTSQ_ERR_FORMAT;
        }
        if (hdr.channels && och != hdr.channels) {
            free(pcm); free(buf); return QTSQ_ERR_FORMAT;
        }
        if (hdr.bit_depth && obps != hdr.bit_depth) {
            free(pcm); free(buf); return QTSQ_ERR_FORMAT;
        }
        memcpy(buf, pcm, n);
        free(pcm);
    } else if (hdr.codec == QTSQ_BLOB_CODEC_QTC) {
        /* qtc stream: the header declares the original size, which must
         * agree with ours before we trust it to size the decode. */
        uint64_t declared = qtsq_codec_declared_size(body, body_len);
        if (declared != hdr.original_size) { free(buf); return QTSQ_ERR_FORMAT; }
        uint8_t *tmp = NULL;
        size_t tmp_len = 0;
        rc = qtsq_codec_decompress(body, body_len, hdr.original_size, &tmp, &tmp_len);
        if (rc != QTSQ_OK || !tmp || tmp_len != n) {
            free(tmp); free(buf); return QTSQ_ERR_FORMAT;
        }
        memcpy(buf, tmp, n);
        free(tmp);
    } else {
        /* Unknown inner codec: the id is a hint, never a licence to
         * guess. Refuse rather than return bytes we cannot verify. */
        free(buf);
        return QTSQ_ERR_STRATEGY;
    }

    /* The header's digest covers the ORIGINAL bytes, so this checks the
     * decode result and not merely the framing. */
    uint8_t digest[32];
    qtsq_sha256(buf, n, digest);
    if (memcmp(digest, hdr.sha256, 32) != 0) {
        free(buf);
        return QTSQ_ERR_FORMAT;   /* corrupt payload */
    }

    if (out_hdr) memcpy(out_hdr, &hdr, sizeof(hdr));
    *out = buf;
    *out_len = n;
    return QTSQ_OK;
}

int qtsq_model_add_audio(qtsq_context_t *ctx, const char *name,
                         const uint8_t *pcm, size_t pcm_size,
                         uint8_t channels, uint8_t bit_depth,
                         uint32_t sample_rate, uint8_t policy,
                         int level) {
    if (!ctx || !name || !pcm) return QTSQ_ERR_NULL;
    if (pcm_size == 0) return QTSQ_ERR_TOO_SMALL;
    /* Only KEEP vs "code it" matters for audio — there is no audio analogue
     * of the image PIXELS/PHOTO split, and rejecting anything above KEEP
     * would refuse the only policy a caller can meaningfully ask for. */
    if (policy != QTSQ_BLOB_POLICY_KEEP && policy != QTSQ_BLOB_POLICY_PIXELS)
        return QTSQ_ERR_FORMAT;

    uint32_t frames = 0;
    if (!audio_geometry(pcm_size, channels, bit_depth, 0, &frames))
        return QTSQ_ERR_FORMAT;   /* unsupported layout, or a bad frame count */

    return qtsq_model_add_blob(ctx, name, pcm, pcm_size,
                               QTSQ_KIND_AUDIO, QTSQ_CT_AUDIO_PCM,
                               sample_rate, frames, channels,
                               bit_depth, policy, level);
}

int qtsq_model_add_image_file(qtsq_context_t *ctx, const char *name,
                              const uint8_t *file_bytes, size_t file_size,
                              uint8_t policy, uint32_t max_pixels,
                              int level) {
    if (!ctx || !name || !file_bytes) return QTSQ_ERR_NULL;
    if (file_size == 0) return QTSQ_ERR_TOO_SMALL;
    if (policy > QTSQ_BLOB_POLICY_PHOTO) return QTSQ_ERR_FORMAT;

    if (policy == QTSQ_BLOB_POLICY_KEEP) {
        /* Still probe: a stream that claims to be an image should be one,
         * and it lets the TOC carry real geometry instead of zeros. */
        uint32_t w = 0, h = 0; uint8_t ch = 0;
        int prc = qtsq_image_probe(file_bytes, file_size, &w, &h, &ch);
        if (prc != QTSQ_IMAGE_OK) return QTSQ_ERR_FORMAT;
        return qtsq_model_add_blob(ctx, name, file_bytes, file_size,
                                   QTSQ_KIND_IMAGE, QTSQ_CT_IMAGE_FILE,
                                   w, h, ch, 8, QTSQ_BLOB_POLICY_KEEP, level);
    }

    /* Probe before decoding: cheap, and the only place a hostile file's
     * declared dimensions can be bounded before stb allocates. */
    uint32_t w = 0, h = 0; uint8_t ch = 0;
    if (qtsq_image_probe(file_bytes, file_size, &w, &h, &ch) != QTSQ_IMAGE_OK)
        return QTSQ_ERR_FORMAT;
    if (max_pixels && (uint64_t)w * (uint64_t)h > (uint64_t)max_pixels)
        return QTSQ_ERR_TOO_LARGE;

    uint8_t *px = NULL;
    size_t px_size = 0;
    uint32_t dw = 0, dh = 0; uint8_t dc = 0;
    int rc = qtsq_image_decode(file_bytes, file_size, &px, &px_size, &dw, &dh, &dc);
    if (rc != QTSQ_IMAGE_OK || !px) { free(px); return QTSQ_ERR_FORMAT; }
    if (max_pixels && (uint64_t)dw * (uint64_t)dh > (uint64_t)max_pixels) {
        free(px);
        return QTSQ_ERR_TOO_LARGE;
    }

    rc = qtsq_model_add_blob(ctx, name, px, px_size,
                             QTSQ_KIND_IMAGE, QTSQ_CT_IMAGE_PIXELS,
                             dw, dh, dc, 8, policy, level);
    free(px);
    return rc;
}

int qtsq_model_add_blob(qtsq_context_t *ctx,
                        const char *name,
                        const uint8_t *raw, size_t raw_size,
                        uint8_t kind, uint16_t content_type,
                        uint32_t param0, uint32_t param1, uint32_t channels,
                        uint8_t bit_depth, uint8_t policy,
                        int level) {
    if (!ctx || !name || !raw) return QTSQ_ERR_NULL;
    if (raw_size == 0) return QTSQ_ERR_TOO_SMALL;
    if (kind > QTSQ_KIND_TABLE) return QTSQ_ERR_FORMAT;

    uint8_t *payload = NULL;
    size_t payload_size = 0;
    uint8_t codec = 0;
    uint16_t bflags = 0;
    int r = blob_build(raw, raw_size, kind, content_type, param0, param1,
                       channels, bit_depth, policy, level, &payload,
                       &payload_size, &codec, &bflags);
    if (r != QTSQ_OK) return r;

    /* Wrap in a sub-context so the container records the right
     * data_type/strategy and computes the TOC entry itself. */
    qtsq_context_t sub;
    qtsq_init(&sub);
    if (sub.singularity) { free(sub.singularity); sub.singularity = NULL; }
    sub.singularity = payload;
    sub.singularity_size = payload_size;
    sub.header.data_type = QTSQ_TYPE_RAW;
    sub.header.strategy = (kind == QTSQ_KIND_TEXT) ? QTSQ_STRATEGY_DICTIONARY
                                                   : QTSQ_STRATEGY_SINGULARITY;
    sub.header.original_size = (uint64_t)raw_size;
    sub.header.singularity_size = payload_size;
    sub.header.flags = QTSQ_FLAG_CHECKSUMMED | QTSQ_STREAM_FLAG_TYPED;
    memcpy(sub.header.checksum, ((const qtsq_blob_header_t *)payload)->sha256, 32);

    r = qtsq_container_add_stream_take(ctx, &sub, name);
    qtsq_free(&sub);   /* takes ownership of payload on success */
    return r;
}

int qtsq_model_add_manifest(qtsq_context_t *ctx, const char *manifest_json) {    if (!ctx || !manifest_json) return QTSQ_ERR_NULL;
    size_t n = strlen(manifest_json);
    if (n == 0) return QTSQ_ERR_TOO_SMALL;

    qtsq_context_t sub;
    qtsq_init(&sub);
    int r = qtsq_compress_json(&sub, manifest_json, n);
    if (r != QTSQ_OK) { qtsq_free(&sub); return r; }
    r = qtsq_container_add_stream_take(ctx, &sub, QTSQ_MODEL_MANIFEST_STREAM);
    qtsq_free(&sub);
    return r;
}

/* Store-mode manifest: raw JSON bytes (no horizon codec pass).
 * Decodes through the generic raw path, so no reader changes are
 * needed — and multi-thousand-tensor manifests skip the dictionary
 * build entirely. */
int qtsq_model_add_manifest_raw(qtsq_context_t *ctx, const char *manifest_json) {
    if (!ctx || !manifest_json) return QTSQ_ERR_NULL;
    size_t n = strlen(manifest_json);
    if (n == 0) return QTSQ_ERR_TOO_SMALL;

    qtsq_context_t sub;
    qtsq_init(&sub);
    int r = qtsq_compress_raw(&sub, (const uint8_t *)manifest_json, n, 0);
    if (r != QTSQ_OK) { qtsq_free(&sub); return r; }
    r = qtsq_container_add_stream_take(ctx, &sub, QTSQ_MODEL_MANIFEST_STREAM);
    qtsq_free(&sub);
    return r;
}

int qtsq_model_pack(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    return qtsq_container_pack(ctx);
}

int qtsq_model_write(qtsq_context_t *ctx, const char *filename,
                     const char *password) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;
    if (password && password[0]) {
        int r = qtsq_vault_encrypt_password(ctx, password);
        if (r != QTSQ_OK) return r;
        return qtsq_write(ctx, filename);
    }
    if (qtsq_keystore_is_initialized()) {
        /* Device key present: encryption is mandatory, never silently
         * downgraded — a failure is an error, not plaintext. */
        int r = qtsq_keystore_encrypt(ctx);
        if (r != QTSQ_OK) return r;
        return qtsq_write(ctx, filename);
    }
    /* Plaintext: the pack-time name-XOR stub must already be absent
     * (see qtsq_model_pack_write); clear defensively and write. */
    memset(&ctx->security, 0, sizeof(ctx->security));
    ctx->has_security = 0;
    ctx->is_encrypted = 0;
    ctx->header.security_block_size = 0;
    ctx->is_sanitized = 1;
    ctx->sanitize_report |= QTSQ_SANITIZE_ZERO_META;
    ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    return qtsq_write_plaintext(ctx, filename);
}

int qtsq_model_pack_write(qtsq_context_t *ctx, const char *filename,
                          const char *password, int durable) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;
    int want_plain = !(password && password[0]) &&
                     !qtsq_keystore_is_initialized();
    if (want_plain) {
        /* Plant a zeroed security stub so stream names stay cleartext
         * (like safetensors) and no key is ever needed to list them,
         * then stream straight to disk (no pack buffer). */
        memset(&ctx->security, 0, sizeof(ctx->security));
        ctx->has_security = 1;
        return qtsq_container_pack_write(ctx, filename, durable);
    }
    int r = qtsq_container_pack(ctx);
    if (r != QTSQ_OK) return r;
    return qtsq_model_write(ctx, filename, password);
}
