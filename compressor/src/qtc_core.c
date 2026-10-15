/*
 * qtc_core.c — container format + one-shot public API.
 *
 * Phase 1 supports two lenses:
 *   RAW    — order-0 rANS entropy coding (the Singularity core)
 *   STORED — verbatim bytes (below the Chandrasekhar limit, or when the
 *            entropy coder cannot beat the original size)
 *
 * The router (Spaghettification) and domain lenses arrive in Phase 4; until
 * then QTC_LENS_AUTO resolves to RAW.
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Stringify the version macros for qtc_version_string (audit L1: the old
 * hardcoded string drifted from the macros). */
#define QTC_STR2(x) #x
#define QTC_STR(x) QTC_STR2(x)

/* Default pixel gate; the effective value is opts->max_contest_pixels
 * when non-zero. See qtc.h for the measurements behind it. */
#define QTC_CONTEST_MAX_PIXELS QTC_CONTEST_MAX_PIXELS_DEFAULT

void qtc_options_default(qtc_options_t *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    opts->level = 3;
    opts->mode = QTC_MODE_LOSSLESS;
    opts->quality = 1.0f;
    opts->force_lens = QTC_LENS_AUTO;
    opts->flags = 0;
    opts->min_compress_size = QTC_CHANDRASEKHAR_LIMIT;
    opts->max_contest_pixels = QTC_CONTEST_MAX_PIXELS_DEFAULT;
}

const char *qtc_version_string(void)
{
    return "qtc " QTC_STR(QTC_VERSION_MAJOR) "."
           QTC_STR(QTC_VERSION_MINOR) "." QTC_STR(QTC_VERSION_PATCH)
           " (Singularity core + order-1 + LZ77 + domain lenses + router"
           " + lossy scene codecs)";
}

const char *qtc_error_string(int code)
{
    switch (code) {
    case QTC_OK:           return "ok";
    case QTC_ERR_FORMAT:   return "not a qtc stream or unsupported version";
    case QTC_ERR_ALLOC:    return "allocation failure";
    case QTC_ERR_CORRUPT:  return "stream corrupted";
    case QTC_ERR_SIZE:     return "declared output exceeds bound";
    default:               return "unknown error";
    }
}

/* Write the 16-byte container header. */
static void write_header(uint8_t *h, uint8_t lens, uint8_t flags,
                         uint64_t original_size)
{
    h[0] = QTC_MAGIC0;
    h[1] = QTC_MAGIC1;
    h[2] = QTC_MAGIC2;
    h[3] = QTC_MAGIC3;
    h[4] = lens;
    h[5] = flags;
    h[6] = 0;
    h[7] = 0;
    for (int i = 0; i < 8; i++)
        h[8 + i] = (uint8_t)((original_size >> (8 * i)) & 0xFF);
}

/* Assemble a RAW (order-0) payload: [u16 model_len][model][bitstream].
 * Returns a malloc'd payload or NULL on failure. */
static uint8_t *try_raw0(const uint8_t *in, size_t in_len, size_t *out_len)
{
    qtc_model_t model;
    qtc_model_build(&model, in, in_len);

    uint8_t *bits = NULL;
    size_t bits_len = 0;
    if (qtc_ans_encode(in, in_len, &model, &bits, &bits_len) != QTC_OK)
        return NULL;

    size_t total = 2 + 512 + bits_len;
    uint8_t *p = (uint8_t *)malloc(total);
    if (p) {
        p[0] = (uint8_t)(512 & 0xFF);
        p[1] = (uint8_t)(512 >> 8);
        qtc_model_pack(&model, p + 2);
        memcpy(p + 2 + 512, bits, bits_len);
        *out_len = total;
    }
    free(bits);
    return p;
}

/* Assemble a RAW1 (order-1) payload: [u16 model_len][model][bitstream]. */
static uint8_t *try_raw1(const uint8_t *in, size_t in_len, size_t *out_len)
{
    qtc_model1_t *model = (qtc_model1_t *)malloc(sizeof(*model));
    if (!model)
        return NULL;

    uint8_t *p = NULL;
    uint8_t *bits = NULL;
    uint8_t *packed = (uint8_t *)malloc(2 + (size_t)QTC_ALPHABET * 513);
    size_t bits_len = 0, packed_len = 0;

    if (packed &&
        qtc_model1_build(model, in, in_len) == QTC_OK) {
        /* The payload stores model_len as a u16, but the packed model is
         * 2 + present_count*513 bytes. With more than 127 present contexts
         * it exceeds 65535 and the u16 would truncate, silently corrupting
         * the stream — so decline the lens and let the contest fall back to
         * RAW/LZ2/STORED for high-diversity inputs. */
        size_t present_count = 0;
        for (int c = 0; c < QTC_ALPHABET; c++)
            present_count += model->present[c] ? 1 : 0;
        if (2 + present_count * 513 <= 65535 &&
            qtc_ans_encode_o1(in, in_len, model, &bits, &bits_len) == QTC_OK) {
            packed_len = qtc_model1_pack(model, packed);
            size_t total = 2 + packed_len + bits_len;
            p = (uint8_t *)malloc(total);
            if (p) {
                p[0] = (uint8_t)(packed_len & 0xFF);
                p[1] = (uint8_t)((packed_len >> 8) & 0xFF);
                memcpy(p + 2, packed, packed_len);
                memcpy(p + 2 + packed_len, bits, bits_len);
                *out_len = total;
            }
        }
    }
    free(packed);
    free(bits);
    free(model);
    return p;
}

/* Assemble an LZ payload (self-describing, blocked). */
static uint8_t *try_lz(const uint8_t *in, size_t in_len, size_t *out_len)
{
    uint8_t *p = NULL;
    size_t len = 0;
    if (qtc_lz_encode(in, in_len, &p, &len) != QTC_OK)
        return NULL;
    *out_len = len;
    return p;
}

/* Assemble an LZ2 payload (v0.15.0). Each block is independently encoded
 * in the smallest of the v1 and v2 formats, so LZ2 is never meaningfully
 * worse than LZ — the AUTO contest runs this instead of try_lz. */
static uint8_t *try_lz2(const uint8_t *in, size_t in_len, size_t *out_len)
{
    uint8_t *p = NULL;
    size_t len = 0;
    if (qtc_lz2_encode(in, in_len, &p, &len) != QTC_OK)
        return NULL;
    *out_len = len;
    return p;
}

/* Assemble an AUDIO payload from raw bytes. The channel layout is not
 * known from bytes alone, so we try mono and stereo interpretations of the
 * same sample stream and keep the smaller one — both decode bit-exactly. */
static uint8_t *try_audio(const uint8_t *in, size_t in_len, size_t *out_len)
{
    if (in_len < 2 || (in_len & 1) || in_len > ((size_t)UINT32_MAX) * 2)
        return NULL;

    uint8_t *best = NULL;
    size_t best_len = 0;
    for (uint8_t ch = 1; ch <= 2; ch++) {
        size_t frame_bytes = (size_t)ch * 2;
        if (in_len % frame_bytes != 0)
            continue;
        uint32_t nf = (uint32_t)(in_len / frame_bytes);
        uint8_t *p = NULL;
        size_t pl = 0;
        if (qtc_audio_encode(in, nf, ch, 16, &p, &pl) == QTC_OK) {
            if (!best || pl < best_len) {
                free(best);
                best = p;
                best_len = pl;
            } else {
                free(p);
            }
        }
    }
    if (best)
        *out_len = best_len;
    return best;
}

int qtc_compress(const uint8_t *in, size_t in_len,
                 uint8_t **out, size_t *out_len,
                 const qtc_options_t *opts)
{
    qtc_options_t defaults;
    if (!opts) {
        qtc_options_default(&defaults);
        opts = &defaults;
    }
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (in_len > 0 && !in)
        return QTC_ERR_FORMAT;

    uint32_t limit = opts->min_compress_size
                         ? opts->min_compress_size
                         : QTC_CHANDRASEKHAR_LIMIT;
    int force = (opts->flags & QTC_FLAG_FORCE_COMPRESS) != 0;

    /* Spaghettification router: classify the input. A COMPRESSED verdict
     * (magic bytes of an already-entropy-coded format) skips the contest —
     * recompressing jpeg/zip/mp3 cannot win, so store it. An AUDIO verdict
     * adds the audio lens to the contest. The router never forces a lens:
     * the smallest verified payload always wins. */
    qtc_detect_t det = qtc_detect(in, in_len);

    /* Chandrasekhar: tiny inputs are stored raw unless forced. */
    int use_stored = (!force && in_len < limit) ||
                     opts->force_lens == QTC_LENS_STORED ||
                     (opts->force_lens == QTC_LENS_AUTO &&
                      det.type == QTC_TYPE_COMPRESSED);

    if (!use_stored) {
        /* Try every entropy lens and emit the smallest payload that beats
         * the original size — later phases can never regress earlier ones.
         * If force_lens pins a specific lens, only that lens is tried. */
        uint8_t *p0 = NULL, *p1 = NULL, *plz = NULL, *paud = NULL;
        uint8_t *ptext = NULL;
        size_t l0 = 0, l1 = 0, llz = 0, laud = 0, ltext = 0;
        qtc_lens_t forced = opts->force_lens;

        if (forced == QTC_LENS_AUTO || forced == QTC_LENS_RAW)
            p0 = try_raw0(in, in_len, &l0);
        if (forced == QTC_LENS_AUTO || forced == QTC_LENS_RAW1)
            p1 = try_raw1(in, in_len, &l1);
        /* AUTO runs LZ2, which contests the v1 format per block and so
         * dominates the legacy LZ lens (at the cost of a 1-byte-per-block
         * version tag). force_lens=LZ keeps the v1-only stream. */
        if (forced == QTC_LENS_AUTO || forced == QTC_LENS_LZ2)
            plz = try_lz2(in, in_len, &llz);
        else if (forced == QTC_LENS_LZ)
            plz = try_lz(in, in_len, &llz);
        /* The audio lens only enters the contest when the router sees PCM
         * (or when explicitly forced) — running it on arbitrary bytes is
         * wasted work and its output would rarely win there. */
        if (forced == QTC_LENS_AUDIO ||
            (forced == QTC_LENS_AUTO && det.type == QTC_TYPE_AUDIO))
            paud = try_audio(in, in_len, &laud);
        /* The text lens only enters the contest when the router actually
         * sees text (or when forced). It costs an extra pass, and on
         * images/binaries its front-coding pass would never win — but on
         * line-structured text (JSON records, CSV, logs, repetitive
         * source) it measures 30-44% smaller than LZ2 alone. */
        if (forced == QTC_LENS_TEXT ||
            (forced == QTC_LENS_AUTO && det.type == QTC_TYPE_TEXT)) {
            qtc_options_t topts;
            qtc_options_default(&topts);
            topts.level = opts->level;
            topts.min_compress_size = opts->min_compress_size;
            if (qtc_compress_text(in, in_len, &ptext, &ltext, &topts) != QTC_OK) {
                ptext = NULL; ltext = 0;
            }
        }

        uint8_t *best = NULL;
        size_t best_len = 0;
        uint8_t best_lens = 0;

        if (p0 && l0 < in_len && (!best || l0 < best_len)) {
            best = p0; best_len = l0; best_lens = QTC_LENS_RAW;
        }
        if (p1 && l1 < in_len && (!best || l1 < best_len)) {
            best = p1; best_len = l1; best_lens = QTC_LENS_RAW1;
        }
        if (plz && llz < in_len && (!best || llz < best_len)) {
            best = plz; best_len = llz;
            best_lens = (uint8_t)((forced == QTC_LENS_LZ)
                                      ? QTC_LENS_LZ : QTC_LENS_LZ2);
        }
        if (paud && laud < in_len && (!best || laud < best_len)) {
            best = paud; best_len = laud; best_lens = QTC_LENS_AUDIO;
        }
        /* Text is elected last so ties resolve toward the older lenses,
         * keeping byte-for-byte output stable for inputs where it does
         * not clearly win. Do NOT free ptext here on the losing branch:
         * the shared cleanup below frees every candidate exactly once,
         * and freeing it in both places is a double free. */
        if (ptext && ltext < in_len && (!best || ltext < best_len)) {
            best = ptext; best_len = ltext; best_lens = QTC_LENS_TEXT;
        }

        if (best) {
            size_t total = QTC_HEADER_SIZE + best_len;
            uint8_t *buf = (uint8_t *)malloc(total);
            if (!buf) {
                free(p0); free(p1); free(plz); free(paud); free(ptext);
                return QTC_ERR_ALLOC;
            }
            write_header(buf, best_lens, QTC_FLAG_HAS_MODEL, in_len);
            memcpy(buf + QTC_HEADER_SIZE, best, best_len);
            free(p0); free(p1); free(plz); free(paud); free(ptext);
            *out = buf;
            *out_len = total;
            return QTC_OK;
        }

        free(p0); free(p1); free(plz); free(paud); free(ptext);
        /* No lens beat the original → fall through to STORED. */
    }

    /* STORED container. */
    size_t total = QTC_HEADER_SIZE + in_len;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf)
        return QTC_ERR_ALLOC;
    write_header(buf, QTC_LENS_STORED, 0, in_len);
    if (in_len > 0)
        memcpy(buf + QTC_HEADER_SIZE, in, in_len);
    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_bounded(const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len,
                           uint64_t max_out)
{
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;

    uint8_t lens = in[4];
    uint8_t flags = in[5];

    uint64_t original_size = 0;
    for (int i = 0; i < 8; i++)
        original_size |= (uint64_t)in[8 + i] << (8 * i);

    if (original_size > max_out)
        return QTC_ERR_SIZE;

    const uint8_t *payload = in + QTC_HEADER_SIZE;
    size_t payload_len = in_len - QTC_HEADER_SIZE;

    if (original_size == 0) {
        /* Empty original: nothing to produce. */
        return QTC_OK;
    }

    uint8_t *outbuf = (uint8_t *)malloc(original_size);
    if (!outbuf)
        return QTC_ERR_ALLOC;

    if (lens == QTC_LENS_STORED) {
        if (payload_len < original_size) {
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        memcpy(outbuf, payload, original_size);
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_RAW) {
        if (!(flags & QTC_FLAG_HAS_MODEL)) {
            free(outbuf);
            return QTC_ERR_FORMAT;
        }
        if (payload_len < 2) {
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        uint32_t model_len = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8);
        /* The order-0 model is a FIXED 512-byte table (256 symbols x u16).
         * qtc_model_unpack always reads exactly that many bytes, so the
         * bounds check must pin model_len to 512 rather than trust the
         * (attacker-controlled) stored value — a corrupted model_len < 512
         * used to let unpack read past the payload (found by the #18
         * structured container fuzz). */
        if (model_len != QTC_ALPHABET * 2 ||
            payload_len < 2 + (size_t)model_len) {
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        qtc_model_t model;
        int rc = qtc_model_unpack(&model, payload + 2);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        const uint8_t *bits = payload + 2 + model_len;
        size_t bits_len = payload_len - 2 - model_len;
        rc = qtc_ans_decode(bits, bits_len, (size_t)original_size,
                            &model, outbuf);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_RAW1) {
        if (!(flags & QTC_FLAG_HAS_MODEL)) {
            free(outbuf);
            return QTC_ERR_FORMAT;
        }
        if (payload_len < 2) {
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        uint32_t model_len = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8);
        if (payload_len < 2 + model_len) {
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        qtc_model1_t *model1 = (qtc_model1_t *)malloc(sizeof(*model1));
        if (!model1) {
            free(outbuf);
            return QTC_ERR_ALLOC;
        }
        size_t consumed = 0;
        int rc = qtc_model1_unpack(model1, payload + 2, model_len, &consumed);
        if (rc != QTC_OK || consumed != model_len) {
            free(model1);
            free(outbuf);
            return rc == QTC_OK ? QTC_ERR_CORRUPT : rc;
        }
        const uint8_t *bits = payload + 2 + model_len;
        size_t bits_len = payload_len - 2 - model_len;
        rc = qtc_ans_decode_o1(bits, bits_len, (size_t)original_size,
                               model1, outbuf);
        free(model1);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_LZ) {
        size_t consumed = 0;
        int rc = qtc_lz_decode(payload, payload_len, (size_t)original_size,
                               outbuf, &consumed);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_LZ2) {
        size_t consumed = 0;
        int rc = qtc_lz2_decode(payload, payload_len, (size_t)original_size,
                                outbuf, &consumed);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_TEXT) {
        /* The text payload is self-describing: its own 6-byte header
         * carries raw_len, which must agree with the stream header.
         * Assign the out params the way the sibling lenses do — a bare
         * return here would report success while handing the caller an
         * empty result. */
        int rc = qtc_decompress_text(payload, payload_len, outbuf,
                                     (size_t)original_size);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        *out = outbuf;
        *out_len = (size_t)original_size;
        return QTC_OK;
    }

    if (lens == QTC_LENS_AUDIO) {
        /* The audio payload is self-describing (nframes/channels/bps are
         * stored inside it). Decode into its own buffer, then verify the
         * reconstructed byte count matches the header's original size. */
        uint8_t *pcm = NULL;
        uint32_t nf = 0;
        uint8_t ch = 0, bps = 0;
        int rc = qtc_audio_decode(payload, payload_len, &pcm, &nf, &ch, &bps);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)nf * ch * (bps / 8);
        if (got != (size_t)original_size) {
            free(pcm);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = pcm;
        *out_len = got;
        return QTC_OK;
    }

    if (lens == QTC_LENS_AUDIO_MDCT) {
        /* Same self-describing contract as the lossless audio lens. */
        uint8_t *pcm = NULL;
        uint32_t nf = 0;
        uint8_t ch = 0, bps = 0;
        int rc = qtc_audio_mdct_decode(payload, payload_len, &pcm, &nf,
                                       &ch, &bps);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)nf * ch * (bps / 8);
        if (got != (size_t)original_size) {
            free(pcm);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = pcm;
        *out_len = got;
        return QTC_OK;
    }

    if (lens == QTC_LENS_AUDIO_MDCT2) {
        /* Same self-describing contract as the other audio lenses. */
        uint8_t *pcm = NULL;
        uint32_t nf = 0;
        uint8_t ch = 0, bps = 0;
        int rc = qtc_audio_mdct2_decode(payload, payload_len, &pcm, &nf,
                                        &ch, &bps);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)nf * ch * (bps / 8);
        if (got != (size_t)original_size) {
            free(pcm);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = pcm;
        *out_len = got;
        return QTC_OK;
    }

    if (lens == QTC_LENS_AUDIO_FLOAT) {
        /* Self-describing float32 audio; byte count must match header. */
        float *fpcm = NULL;
        uint32_t nf = 0;
        uint8_t ch = 0;
        int rc = qtc_audio_float_decode(payload, payload_len, &fpcm, &nf, &ch);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)nf * ch * 4;
        if (got != (size_t)original_size) {
            free(fpcm);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = (uint8_t *)fpcm;
        *out_len = got;
        return QTC_OK;
    }

    if (lens == QTC_LENS_IMAGE || lens == QTC_LENS_IMAGE_MESH ||
        lens == QTC_LENS_IMAGE_ACCRETION || lens == QTC_LENS_IMAGE_DCT) {
        /* Image payloads are self-describing (w/h/ch stored inside). */
        /* The IMAGE lens carries a depth byte: 2 = 16-bit samples (v0.14.0),
         * anything else = 8-bit. Route before decoding so the original-size
         * check uses the right bytes-per-sample. */
        if (lens == QTC_LENS_IMAGE && payload_len >= 10 && payload[9] == 2) {
            uint16_t *px16 = NULL;
            uint32_t iw = 0, ih = 0;
            uint8_t ich = 0;
            int rc = qtc_image16_decode(payload, payload_len, &px16, &iw,
                                        &ih, &ich);
            if (rc != QTC_OK) {
                free(outbuf);
                return rc;
            }
            size_t got = (size_t)iw * ih * ich * 2;
            if (got != (size_t)original_size) {
                free(px16);
                free(outbuf);
                return QTC_ERR_CORRUPT;
            }
            free(outbuf);
            *out = (uint8_t *)px16;
            *out_len = got;
            return QTC_OK;
        }
        uint8_t *px = NULL;
        uint32_t iw = 0, ih = 0;
        uint8_t ich = 0;
        int rc;
        if (lens == QTC_LENS_IMAGE)
            rc = qtc_image_decode(payload, payload_len, &px, &iw, &ih, &ich);
        else if (lens == QTC_LENS_IMAGE_MESH)
            rc = qtc_image_mesh_decode(payload, payload_len, &px, &iw, &ih,
                                       &ich);
        else if (lens == QTC_LENS_IMAGE_DCT)
            rc = qtc_image_dct_decode(payload, payload_len, &px, &iw, &ih,
                                      &ich);
        else
            rc = qtc_accretion_decode(payload, payload_len, &px, &iw, &ih,
                                      &ich);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)iw * ih * ich;
        if (got != (size_t)original_size) {
            free(px);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = px;
        *out_len = got;
        return QTC_OK;
    }

    if (lens == QTC_LENS_TENSOR) {
        /* Self-describing float32 payload; count must match the header. */
        float *f = NULL;
        size_t count = 0;
        int rc = qtc_tensor_decode(payload, payload_len, &f, &count);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        if ((size_t)count * 4 != (size_t)original_size) {
            free(f);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = (uint8_t *)f;
        *out_len = (size_t)count * 4;
        return QTC_OK;
    }

    if (lens == QTC_LENS_VIDEO) {
        /* Self-describing frame sequence; geometry must match the header. */
        uint8_t *frames = NULL;
        uint32_t nf = 0, vw = 0, vh = 0;
        uint8_t vch = 0;
        int rc = qtc_video_decode(payload, payload_len, &frames, &nf, &vw,
                                  &vh, &vch);
        if (rc != QTC_OK) {
            free(outbuf);
            return rc;
        }
        size_t got = (size_t)nf * vw * vh * vch;
        if (got != (size_t)original_size) {
            free(frames);
            free(outbuf);
            return QTC_ERR_CORRUPT;
        }
        free(outbuf);
        *out = frames;
        *out_len = got;
        return QTC_OK;
    }

    free(outbuf);
    return QTC_ERR_FORMAT; /* unknown lens */
}

int qtc_decompress(const uint8_t *in, size_t in_len,
                   uint8_t **out, size_t *out_len)
{
    /* Internal cap of 4 GB (audit M3: the previous 2^40 default let a tiny
     * crafted header force a near-1 TB allocation). Since v0.16.0 the LZ
     * lens handles larger streams via its extended block count, but this
     * unbounded convenience wrapper keeps the 4 GB safeguard; callers with
     * bigger (trusted) streams should use qtc_decompress_bounded. */
    return qtc_decompress_bounded(in, in_len, out, out_len,
                                  (uint64_t)1 << 32);
}

/* ── Typed domain APIs (Phase 4) ─────────────────────────────────────── */

/* PSNR between the original and a lossy reconstruction (used by the LOSSY
 * best-of-all contest between the mesh and accretion lenses). */
static double lossy_psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double mse = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse < 1e-9)
        return 200.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* Lossless-only image entry point: runs the 2D MED lens and nothing
 * else. qtc_compress_image runs a lossy rate-distortion contest when
 * opts->mode is LOSSY, which is the wrong call for pixels that were just
 * transcoded out of a PNG/BMP and must round-trip exactly. */
int qtc_compress_image_lossless(const uint8_t *pixels, uint32_t w, uint32_t h,
                                uint8_t ch, uint8_t **out, size_t *out_len,
                                const qtc_options_t *opts)
{
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    (void)opts;   /* lossless-only: quality/lens knobs do not apply */
    if (!pixels && (size_t)w * h * ch > 0)
        return QTC_ERR_FORMAT;
    if (ch < 1 || ch > 4)
        return QTC_ERR_FORMAT;

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = qtc_image_encode(pixels, w, h, ch, &payload, &payload_len);
    if (rc != QTC_OK)
        return rc;

    /* Never worse than raw: emit STORED verbatim framing when the
     * prediction cannot help (already-compressed or noisy pixels). */
    if (payload_len >= (size_t)w * h * ch) {
        free(payload);
        size_t raw = (size_t)w * h * ch;
        size_t total = QTC_HEADER_SIZE + raw;
        uint8_t *buf = (uint8_t *)malloc(total);
        if (!buf)
            return QTC_ERR_ALLOC;
        write_header(buf, QTC_LENS_STORED, 0, (uint64_t)raw);
        memcpy(buf + QTC_HEADER_SIZE, pixels, raw);
        *out = buf;
        *out_len = total;
        return QTC_OK;
    }

    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, QTC_LENS_IMAGE, 0, (uint64_t)w * h * ch);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);
    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_compress_image(const uint8_t *pixels, uint32_t w, uint32_t h,
                       uint8_t ch, uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts)
{
    qtc_options_t defaults;
    if (!opts) {
        qtc_options_default(&defaults);
        opts = &defaults;
    }
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!pixels && (size_t)w * h * ch > 0)
        return QTC_ERR_FORMAT;

    /* Phase 5.5: QTC_MODE_LOSSY runs the lossy image lenses — the Delaunay
     * mesh (triangles), the accretion scene (Gaussian splats + pixel
     * blocks), and since v0.12.0 the 8x8 block-DCT photo coder (1/3-channel
     * input only) — decodes each, and keeps the better reconstruction, with
     * the smaller payload breaking near-ties. The encoder holds the
     * original, so the contest is honest; this is the same best-of-all
     * philosophy the generic path uses for lossless lenses, extended to
     * rate-distortion. No single coder dominates all content: the mesh wins
     * smooth/edge structure, splats+blocks win texture, DCT wins photos.
     * Above QTC_CONTEST_MAX_PIXELS only DCT runs (see below). */
    int lossy = (opts->mode == QTC_MODE_LOSSY);

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc;
    uint8_t lens = QTC_LENS_IMAGE;
    if (lossy) {
        uint8_t *cand_payload[3] = {NULL, NULL, NULL};
        size_t cand_len[3] = {0, 0, 0};
        const uint8_t cand_lens[3] = {QTC_LENS_IMAGE_MESH,
                                      QTC_LENS_IMAGE_ACCRETION,
                                      QTC_LENS_IMAGE_DCT};

        /* force_lens=IMAGE_DCT (video I-frames): skip mesh/accretion —
         * they are O(n²)/multi-pass and dominate encode time at 720p+
         * when the caller already knows DCT is the right tool. */
        int force_dct = (opts->force_lens == QTC_LENS_IMAGE_DCT);

        /* Above QTC_CONTEST_MAX_PIXELS the contest runs only the DCT
         * photo coder. Measured (Apple Silicon, q=0.85): the scene
         * codecs take 70-124 s each at 2.2-10 MP while DCT encodes in
         * <=0.11 s, and DCT won the contest on every real
         * photo/screenshot measured (the scene codecs' size wins did
         * not survive the PSNR scoring). All gated benchmark images
         * are <=0.4 MP, so the full 3-way contest is preserved exactly
         * where the gate numbers were measured. ch==2/4 keeps the full
         * contest at any size because DCT rejects those inputs. */
        uint32_t pixel_gate = opts->max_contest_pixels
                                  ? opts->max_contest_pixels
                                  : QTC_CONTEST_MAX_PIXELS;
        int scene = !force_dct &&
                    ((uint64_t)w * (uint64_t)h <= pixel_gate ||
                     (ch != 1 && ch != 3));
        if (scene) {
            rc = qtc_image_mesh_encode(pixels, w, h, ch, opts->quality,
                                       &cand_payload[0], &cand_len[0]);
            if (rc != QTC_OK && rc != QTC_ERR_FORMAT && rc != QTC_ERR_SIZE) {
                free(cand_payload[0]);
                return rc;
            }
            rc = qtc_accretion_encode(pixels, w, h, ch, opts->quality,
                                      &cand_payload[1], &cand_len[1]);
            if (rc != QTC_OK) {
                free(cand_payload[0]);
                free(cand_payload[1]);
                return rc;
            }
        }
        if (ch == 1 || ch == 3) {
            rc = qtc_image_dct_encode(pixels, w, h, ch, opts->quality,
                                      &cand_payload[2], &cand_len[2]);
            if (rc != QTC_OK && rc != QTC_ERR_FORMAT && rc != QTC_ERR_SIZE) {
                for (int i = 0; i < 3; i++)
                    free(cand_payload[i]);
                return rc;
            }
        }

        /* Score every candidate by PSNR against the original. */
        int best = -1;
        double best_psnr = -1.0;
        for (int i = 0; i < 3; i++) {
            if (!cand_payload[i])
                continue;
            uint8_t *dec = NULL;
            uint32_t dw = 0, dh = 0;
            uint8_t dch = 0;
            double psnr = -1.0;
            int drc;
            if (cand_lens[i] == QTC_LENS_IMAGE_MESH)
                drc = qtc_image_mesh_decode(cand_payload[i], cand_len[i],
                                            &dec, &dw, &dh, &dch);
            else if (cand_lens[i] == QTC_LENS_IMAGE_ACCRETION)
                drc = qtc_accretion_decode(cand_payload[i], cand_len[i],
                                           &dec, &dw, &dh, &dch);
            else
                drc = qtc_image_dct_decode(cand_payload[i], cand_len[i],
                                           &dec, &dw, &dh, &dch);
            if (drc == QTC_OK) {
                psnr = lossy_psnr(pixels, dec, (size_t)w * h * ch);
                free(dec);
            }
            /* Prefer accuracy; within 0.25 dB prefer the smaller file. */
            int better = 0;
            if (psnr >= 0.0) {
                if (best < 0 || best_psnr < 0.0)
                    better = 1;
                else if (psnr > best_psnr + 0.25)
                    better = 1;
                else if (psnr >= best_psnr - 0.25 &&
                         cand_len[i] <= cand_len[best])
                    better = 1;
            }
            if (better) {
                best = i;
                best_psnr = psnr;
            }
        }
        if (best < 0) {
            /* No candidate decoded (should not happen — accretion always
             * succeeds on valid input). Fall back to the first payload. */
            for (int i = 0; i < 3; i++)
                if (cand_payload[i]) {
                    best = i;
                    break;
                }
        }
        if (best < 0) {
            /* Every encoder failed; report the last error. */
            int r = rc != QTC_OK ? rc : QTC_ERR_FORMAT;
            for (int i = 0; i < 3; i++) free(cand_payload[i]);
            return r;
        }
        payload = cand_payload[best];
        payload_len = cand_len[best];
        lens = cand_lens[best];
        for (int i = 0; i < 3; i++)
            if (i != best)
                free(cand_payload[i]);
        /* A later candidate may have failed with a non-fatal error (mesh
         * rejects some inputs, DCT rejects ch!=1/3) even though `best` is a
         * perfectly good payload. That stale rc must not fail the call — and
         * must not leak the winning payload either. */
        rc = QTC_OK;
    } else {
        rc = qtc_image_encode(pixels, w, h, ch, &payload, &payload_len);
    }
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)w * h * ch;
    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, lossy ? lens : QTC_LENS_IMAGE,
                 QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_image(const uint8_t *in, size_t in_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch)
{
    if (!out_pixels || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_pixels = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;
    if (in[4] == QTC_LENS_IMAGE)
        return qtc_image_decode(in + QTC_HEADER_SIZE, in_len - QTC_HEADER_SIZE,
                                out_pixels, out_w, out_h, out_ch);
    if (in[4] == QTC_LENS_IMAGE_MESH)
        return qtc_image_mesh_decode(in + QTC_HEADER_SIZE,
                                     in_len - QTC_HEADER_SIZE,
                                     out_pixels, out_w, out_h, out_ch);
    if (in[4] == QTC_LENS_IMAGE_ACCRETION)
        return qtc_accretion_decode(in + QTC_HEADER_SIZE,
                                    in_len - QTC_HEADER_SIZE,
                                    out_pixels, out_w, out_h, out_ch);
    if (in[4] == QTC_LENS_IMAGE_DCT)
        return qtc_image_dct_decode(in + QTC_HEADER_SIZE,
                                    in_len - QTC_HEADER_SIZE,
                                    out_pixels, out_w, out_h, out_ch);
    return QTC_ERR_FORMAT;
}

/* Near-lossless quantization step from the quality knob (16-bit images).
 * quality 1.0 stays exactly lossless; each step down doubles the allowed
 * per-sample error (max 8 LSBs). */
static uint8_t image16_quant_for_quality(float quality)
{
    if (quality >= 1.0f)
        return 1;
    if (quality >= 0.95f)
        return 2;
    if (quality >= 0.80f)
        return 4;
    if (quality >= 0.60f)
        return 8;
    return 16;
}

int qtc_compress_image16(const uint16_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, uint8_t **out, size_t *out_len,
                         const qtc_options_t *opts)
{
    qtc_options_t defaults;
    if (!opts) {
        qtc_options_default(&defaults);
        opts = &defaults;
    }
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!pixels && (size_t)w * h * ch > 0)
        return QTC_ERR_FORMAT;

    uint8_t quant = (opts->mode == QTC_MODE_LOSSY)
                        ? image16_quant_for_quality(opts->quality)
                        : 1;

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = qtc_image16_encode(pixels, w, h, ch, quant, &payload,
                                &payload_len);
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)w * h * ch * 2;
    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, QTC_LENS_IMAGE, QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_image16(const uint8_t *in, size_t in_len,
                           uint16_t **out_pixels, uint32_t *out_w,
                           uint32_t *out_h, uint8_t *out_ch)
{
    if (!out_pixels || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_pixels = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;
    if (in[4] != QTC_LENS_IMAGE)
        return QTC_ERR_FORMAT;
    return qtc_image16_decode(in + QTC_HEADER_SIZE, in_len - QTC_HEADER_SIZE,
                              out_pixels, out_w, out_h, out_ch);
}

int qtc_compress_audio(const uint8_t *pcm, uint32_t nframes,
                       uint8_t channels, uint8_t bps,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts)
{
    qtc_options_t defaults;
    if (!opts) {
        qtc_options_default(&defaults);
        opts = &defaults;
    }
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!pcm && (size_t)nframes * channels * (bps / 8) > 0)
        return QTC_ERR_FORMAT;

    /* Phase 5.7: QTC_MODE_LOSSY routes to the perceptual MDCT coder
     * (mid/side + masking-based allocation + rate control). The quality
     * knob (0..1) maps to a bitrate target; target_kbps overrides it.
     * The v1 MDCT lens (QTC_LENS_AUDIO_MDCT) remains decodable for
     * streams made before v0.13.0. */
    int lossy = (opts->mode == QTC_MODE_LOSSY);

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc;
    if (lossy)
        rc = qtc_audio_mdct2_encode(pcm, nframes, channels, bps,
                                    opts->quality, opts->target_kbps,
                                    opts->sample_rate,
                                    &payload, &payload_len);
    else
        rc = qtc_audio_encode(pcm, nframes, channels, bps,
                              &payload, &payload_len);
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)nframes * channels * (bps / 8);
    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, lossy ? QTC_LENS_AUDIO_MDCT2 : QTC_LENS_AUDIO,
                 QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_audio(const uint8_t *in, size_t in_len,
                         uint8_t **out_pcm, uint32_t *out_nframes,
                         uint8_t *out_channels, uint8_t *out_bps)
{
    if (!out_pcm || !out_nframes || !out_channels || !out_bps)
        return QTC_ERR_ALLOC;
    *out_pcm = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;
    if (in[4] == QTC_LENS_AUDIO)
        return qtc_audio_decode(in + QTC_HEADER_SIZE, in_len - QTC_HEADER_SIZE,
                                out_pcm, out_nframes, out_channels, out_bps);
    if (in[4] == QTC_LENS_AUDIO_MDCT)
        return qtc_audio_mdct_decode(in + QTC_HEADER_SIZE,
                                     in_len - QTC_HEADER_SIZE,
                                     out_pcm, out_nframes, out_channels,
                                     out_bps);
    if (in[4] == QTC_LENS_AUDIO_MDCT2)
        return qtc_audio_mdct2_decode(in + QTC_HEADER_SIZE,
                                      in_len - QTC_HEADER_SIZE,
                                      out_pcm, out_nframes, out_channels,
                                      out_bps);
    return QTC_ERR_FORMAT;
}

int qtc_compress_audio_float(const float *pcm, uint32_t nframes,
                             uint8_t channels,
                             uint8_t **out, size_t *out_len,
                             const qtc_options_t *opts)
{
    qtc_options_t defaults;
    if (!opts) {
        qtc_options_default(&defaults);
        opts = &defaults;
    }
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!pcm && (size_t)nframes * channels > 0)
        return QTC_ERR_FORMAT;

    int lossy = (opts->mode == QTC_MODE_LOSSY);

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = qtc_audio_float_encode(pcm, nframes, channels, opts->quality,
                                    opts->target_kbps, lossy,
                                    opts->sample_rate,
                                    &payload, &payload_len);
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)nframes * channels * 4;
    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, QTC_LENS_AUDIO_FLOAT, QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_audio_float(const uint8_t *in, size_t in_len,
                               float **out_pcm, uint32_t *out_nframes,
                               uint8_t *out_channels)
{
    if (!out_pcm || !out_nframes || !out_channels)
        return QTC_ERR_ALLOC;
    *out_pcm = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;
    if (in[4] != QTC_LENS_AUDIO_FLOAT)
        return QTC_ERR_FORMAT;
    return qtc_audio_float_decode(in + QTC_HEADER_SIZE,
                                  in_len - QTC_HEADER_SIZE,
                                  out_pcm, out_nframes, out_channels);
}

/* ── Typed tensor / video APIs (Phase 5.6) ───────────────────────────── */

int qtc_compress_tensor(const float *data, uint32_t count,
                        uint8_t **out, size_t *out_len,
                        const qtc_options_t *opts)
{
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!data && count > 0)
        return QTC_ERR_FORMAT;

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc;

    if (opts && opts->mode == QTC_MODE_LOSSY) {
        /* v0.9.0: 16-bit requantization. The quality knob selects the
         * precision class — quality >= 0.5 → bf16 (no range surprises),
         * below → f16 (finer mantissa). The caller's quality choice is
         * honored, not overruled by a size contest. */
        uint8_t lmode = (opts->quality >= 0.5f) ? 2 : 3;
        rc = qtc_tensor_encode_lossy(data, count, lmode, &payload,
                                     &payload_len);
    } else {
        rc = qtc_tensor_encode(data, count, &payload, &payload_len, opts);
    }
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)count * 4;

    /* STORED fallback when neither transform beats the raw size. */
    uint8_t lens = QTC_LENS_TENSOR;
    if (payload_len >= orig && orig > 0) {
        free(payload);
        payload = (uint8_t *)malloc(orig);
        if (!payload)
            return QTC_ERR_ALLOC;
        memcpy(payload, data, orig);
        payload_len = orig;
        lens = QTC_LENS_STORED;
    }

    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, lens, QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_tensor(const uint8_t *in, size_t in_len,
                          float **out_data, uint32_t *out_count)
{
    if (!out_data || !out_count)
        return QTC_ERR_ALLOC;
    *out_data = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;

    uint64_t original_size = 0;
    for (int i = 0; i < 8; i++)
        original_size |= (uint64_t)in[8 + i] << (8 * i);
    if (original_size == 0 || (original_size & 3) != 0)
        return QTC_ERR_CORRUPT;

    if (in[4] == QTC_LENS_STORED) {
        if (in_len - QTC_HEADER_SIZE < original_size)
            return QTC_ERR_CORRUPT;
        float *f = (float *)malloc((size_t)original_size);
        if (!f)
            return QTC_ERR_ALLOC;
        memcpy(f, in + QTC_HEADER_SIZE, (size_t)original_size);
        *out_data = f;
        *out_count = (uint32_t)(original_size / 4);
        return QTC_OK;
    }

    if (in[4] != QTC_LENS_TENSOR)
        return QTC_ERR_FORMAT;

    float *f = NULL;
    size_t count = 0;
    int rc = qtc_tensor_decode(in + QTC_HEADER_SIZE,
                               in_len - QTC_HEADER_SIZE, &f, &count);
    if (rc != QTC_OK)
        return rc;
    if ((size_t)count * 4 != (size_t)original_size) {
        free(f);
        return QTC_ERR_CORRUPT;
    }
    *out_data = f;
    *out_count = (uint32_t)count;
    return QTC_OK;
}

int qtc_compress_video(const uint8_t *frames, uint32_t nframes,
                       uint32_t w, uint32_t h, uint8_t ch,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts)
{
    (void)opts;                     /* lossless lens: no knobs yet */
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = qtc_video_encode(frames, nframes, w, h, ch, &payload,
                              &payload_len);
    if (rc != QTC_OK)
        return rc;

    size_t orig = (size_t)nframes * w * h * ch;

    /* STORED fallback when no mode beats the raw size. */
    uint8_t lens = QTC_LENS_VIDEO;
    if (payload_len >= orig && orig > 0) {
        free(payload);
        payload = (uint8_t *)malloc(orig);
        if (!payload)
            return QTC_ERR_ALLOC;
        memcpy(payload, frames, orig);
        payload_len = orig;
        lens = QTC_LENS_STORED;
    }

    size_t total = QTC_HEADER_SIZE + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(payload);
        return QTC_ERR_ALLOC;
    }
    write_header(buf, lens, QTC_FLAG_HAS_MODEL, orig);
    memcpy(buf + QTC_HEADER_SIZE, payload, payload_len);
    free(payload);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_video(const uint8_t *in, size_t in_len,
                         uint8_t **out_frames, uint32_t *out_nframes,
                         uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch)
{
    if (!out_frames || !out_nframes || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_frames = NULL;

    if (in_len < QTC_HEADER_SIZE || !in)
        return QTC_ERR_FORMAT;
    if (in[0] != QTC_MAGIC0 || in[1] != QTC_MAGIC1 ||
        in[2] != QTC_MAGIC2 || in[3] != QTC_MAGIC3)
        return QTC_ERR_FORMAT;
    if (in[4] != QTC_LENS_VIDEO)
        return QTC_ERR_FORMAT;

    return qtc_video_decode(in + QTC_HEADER_SIZE, in_len - QTC_HEADER_SIZE,
                            out_frames, out_nframes, out_w, out_h, out_ch);
}
