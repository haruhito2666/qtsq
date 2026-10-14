/*
 * qtc — QTSQ Custom Compression Library
 * Public API (Phase 1: rANS order-0 "Singularity" core)
 *
 * Standalone: no dependencies beyond libc. See ../COMPRESSION_LIBRARY_PLAN.md.
 */
#ifndef QTC_H
#define QTC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────── */
#define QTC_VERSION_MAJOR 0
#define QTC_VERSION_MINOR 16
#define QTC_VERSION_PATCH 0

/* ── Error codes ─────────────────────────────────────────────────────── */
#define QTC_OK           0
#define QTC_ERR_FORMAT  -1   /* not a qtc stream / unsupported version   */
#define QTC_ERR_ALLOC   -2   /* allocation failure                       */
#define QTC_ERR_CORRUPT -3   /* stream fails internal consistency checks */
#define QTC_ERR_SIZE    -4   /* declared output exceeds caller's bound   */

/* ── Lens ids (stored in the container header) ───────────────────────── */
typedef enum {
    QTC_LENS_AUTO   = 0,     /* encode-side only: router decides         */
    QTC_LENS_RAW    = 1,     /* order-0 rANS entropy coding (Phase 1)    */
    QTC_LENS_STORED = 2,     /* raw bytes (below Chandrasekhar limit)    */
    QTC_LENS_RAW1   = 3,     /* order-1 rANS entropy coding (Phase 2)    */
    QTC_LENS_LZ     = 4,     /* LZ77 + rANS entropy coding (Phase 3)     */
    QTC_LENS_IMAGE  = 5,     /* 2D predictor + LZ (Phase 4, typed)       */
    QTC_LENS_AUDIO  = 6,     /* mid/side + linear prediction (Phase 4)   */
    QTC_LENS_AUDIO_MDCT = 7, /* lossy MDCT transform coder (Phase 5)     */
    QTC_LENS_IMAGE_MESH = 8, /* lossy adaptive mesh (Phase 5)            */
    QTC_LENS_IMAGE_ACCRETION = 9, /* lossy multi-primitive scene: splats,
                                    triangles, pixel blocks (Phase 5.5)  */
    QTC_LENS_TENSOR     = 10,/* lossless float32 planes/XOR-delta (5.6)  */
    QTC_LENS_VIDEO      = 11,/* lossless temporal video frames (Phase 5.6)*/
    QTC_LENS_IMAGE_DCT  = 12,/* lossy 8x8 block-DCT photo coder (5.5b)   */
    QTC_LENS_AUDIO_MDCT2 = 13,/* lossy perceptual audio coder (Phase 5.7)*/
    QTC_LENS_AUDIO_FLOAT = 14,/* float32 audio: lossless planes/XOR-delta,
                                or lossy via int16 MDCT2 adapter (0.14.0) */
    QTC_LENS_LZ2        = 15,/* LZ77 v2: rep offsets + 8-bit freq mode
                                (v0.15.0; generic contest only — typed
                                lenses still embed v1 LZ streams)        */
    QTC_LENS_TEXT       = 16,/* text lens: token+rANS / front-coding / LZ2
                                contest, never worse than raw (v0.17.0)  */
    QTC_LENS_TABLE      = 17 /* delimited-text lens: row-vs-column-major
                                contest over the text lens (v0.18.0)      */
} qtc_lens_t;

typedef enum {
    QTC_MODE_AUTO     = 0,
    QTC_MODE_LOSSLESS = 1,
    QTC_MODE_LOSSY    = 2    /* Phase 5: quality-per-size (opts->quality)  */
} qtc_mode_t;

/* Ignore the Chandrasekhar limit and compress even tiny inputs. */
#define QTC_FLAG_FORCE_COMPRESS 0x0001u

/* Inputs smaller than this are stored raw: header + model overhead would
 * exceed any compression gain (CONCEPT.md §4.5). */
#define QTC_CHANDRASEKHAR_LIMIT 4096

typedef struct {
    int          level;             /* contest effort: 3 = full (default),
                                       1 = fast subset (fewer candidates,
                                       still bit-exact, different bytes),
                                       0 = store (no contest: verbatim
                                       framing, memcpy speed, ratio ~1.0,
                                       still bit-exact + checksummed) */
    qtc_mode_t   mode;              /* reserved (Phase 5)                */
    float        quality;           /* reserved (Phase 5)                */
    qtc_lens_t   force_lens;        /* QTC_LENS_AUTO = router decides    */
    uint32_t     flags;             /* QTC_FLAG_*                        */
    uint32_t     min_compress_size; /* 0 → QTC_CHANDRASEKHAR_LIMIT       */
    int          target_kbps;       /* lossy audio bitrate target; 0 =
                                       derive from quality (Phase 5.7)   */
    /* Pixel count above which the lossy image contest stops running the
     * O(n^2)/multi-pass scene coders (mesh, accretion) and runs only the
     * DCT photo coder. 0 = QTC_CONTEST_MAX_PIXELS_DEFAULT. This was a
     * compile-time constant; it is a knob now so callers with time to
     * spare on large images can raise it, and so a benchmark can lower
     * it to exercise the 3-way contest above the default gate. */
    uint32_t     max_contest_pixels;
} qtc_options_t;

/* Default for qtc_options_t::max_contest_pixels. Above this the image
 * contest runs DCT only: measured on Apple Silicon at q=0.85, the scene
 * coders take 70-124 s each at 2.2-10 MP while DCT encodes in <=0.11 s,
 * and DCT won on every real photo/screenshot measured. */
#define QTC_CONTEST_MAX_PIXELS_DEFAULT 1000000u

/* Fill opts with defaults (level 3, lossless, auto lens). */
void qtc_options_default(qtc_options_t *opts);

/* ── One-shot API ────────────────────────────────────────────────────── */

/* Compress in[0..in_len) into a malloc'd buffer (*out, *out_len).
 * Caller frees *out. opts may be NULL for defaults.
 * Bit-exact round-trip: qtc_decompress(qtc_compress(x)) == x. */
int qtc_compress(const uint8_t *in, size_t in_len,
                 uint8_t **out, size_t *out_len,
                 const qtc_options_t *opts);

/* Decompress a qtc stream into a malloc'd buffer (*out, *out_len).
 * Caller frees *out (NULL when the original was empty).
 * Convenience wrapper around qtc_decompress_bounded with a 4 GB internal
 * cap; callers handling untrusted input should use the bounded variant
 * with their own limit.
 *
 * Thread safety: all functions are reentrant — the library keeps no shared
 * mutable state, so independent streams may be coded concurrently from
 * multiple threads (do not share one buffer/context across threads). */
int qtc_decompress(const uint8_t *in, size_t in_len,
                   uint8_t **out, size_t *out_len);

/* As qtc_decompress, but refuses to allocate more than max_out bytes
 * (QTC_ERR_SIZE). The decoder never reads past in_len and never writes
 * past the declared original size. */
int qtc_decompress_bounded(const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len,
                           uint64_t max_out);

/* ── Typed domain APIs (Phase 4) ───────────────────────────────────────
 * Domain lenses need structure that raw bytes don't carry (dimensions,
 * channel count). These entry points take that structure explicitly and
 * route to the matching lens. The generic qtc_compress() path uses the
 * detection router instead.
 */

/* Compress a raw image (interleaved pixels, w*h*ch bytes).
 * ch must be 1..4. Uses the IMAGE lens (2D predictor + LZ). */
int qtc_compress_image(const uint8_t *pixels, uint32_t w, uint32_t h,
                       uint8_t ch, uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts);

/* Decompress any qtc stream that holds an image; returns the pixels and
 * writes back the dimensions/channel count recorded at encode time. */
int qtc_decompress_image(const uint8_t *in, size_t in_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch);

/* Compress a raw 16-bit/channel image (interleaved samples, w*h*ch
 * uint16s) — camera-raw capture data. ch must be 1..4. Uses the IMAGE
 * lens with a 16-bit MED predictor + contested byte layout + LZ.
 * Lossless mode is bit-exact; lossy mode is NEAR-LOSSLESS: each sample
 * is guaranteed within quant/2 of the original (quant derived from
 * opts->quality, max error 8 LSBs at quality 0). */
int qtc_compress_image16(const uint16_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, uint8_t **out, size_t *out_len,
                         const qtc_options_t *opts);

/* Decompress any qtc stream that holds a 16-bit image; returns the
 * samples and writes back the dimensions/channel count. */
int qtc_decompress_image16(const uint8_t *in, size_t in_len,
                           uint16_t **out_pixels, uint32_t *out_w,
                           uint32_t *out_h, uint8_t *out_ch);

/* Compress raw interleaved PCM audio (nframes frames of `channels`
 * samples, bps 8, 16 or 24, little-endian). Uses the AUDIO lens (mid/side
 * decorrelation + fixed linear prediction + LZ). Phase 5.6: 24-bit depth
 * and up to 16 channels, matching QTSQ's audio pipeline. */
int qtc_compress_audio(const uint8_t *pcm, uint32_t nframes,
                       uint8_t channels, uint8_t bps,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts);

/* Decompress any qtc stream that holds PCM audio; returns the samples and
 * writes back the frame count / channel count / bits-per-sample recorded
 * at encode time. */
int qtc_decompress_audio(const uint8_t *in, size_t in_len,
                         uint8_t **out_pcm, uint32_t *out_nframes,
                         uint8_t *out_channels, uint8_t *out_bps);

/* Compress raw interleaved float32 audio (nframes frames of `channels`
 * samples) — microphone / DSP capture data. Uses the AUDIO_FLOAT lens.
 * LOSSLESS mode is bit-exact for every IEEE-754 bit pattern (denormals,
 * ±inf, NaN, −0): per-channel XOR-delta + byte-plane transpose contest +
 * LZ, pure byte ops with zero float arithmetic. LOSSY mode routes through
 * the perceptual MDCT2 coder via a scaled int16 intermediate (quality is
 * 16-bit-equivalent, ~96 dB ceiling; opts->quality / target_kbps apply). */
int qtc_compress_audio_float(const float *pcm, uint32_t nframes,
                             uint8_t channels,
                             uint8_t **out, size_t *out_len,
                             const qtc_options_t *opts);

/* Decompress any qtc stream that holds float32 audio; returns the samples
 * and writes back the frame count / channel count recorded at encode
 * time. Bit-exact for streams made with QTC_MODE_LOSSLESS. */
int qtc_decompress_audio_float(const uint8_t *in, size_t in_len,
                               float **out_pcm, uint32_t *out_nframes,
                               uint8_t *out_channels);

/* Low-level float32 tensor lens (transpose / delta / per-plane /
 * second-order contest, STORED-safe). Bit-exact lossless.
 * Payload: [u32 count][u8 mode][u8 0][LZ bytes], mode 0/1/4/6;
 * mode 7 = verbatim raw (store level). */
int qtc_tensor_encode(const float *data, size_t count,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts);
int qtc_tensor_decode(const uint8_t *payload, size_t payload_len,
                      float **out_data, size_t *out_count);

/* Compress an array of float32 elements (tensors, tables of floats, float
 * record sets). Bit-exact LOSSLESS: byte-plane transpose + XOR-delta
 * contest + LZ (TENSOR lens, Phase 5.6). Falls back to the STORED lens
 * when neither transform beats the raw size. */
int qtc_compress_tensor(const float *data, uint32_t count,
                        uint8_t **out, size_t *out_len,
                        const qtc_options_t *opts);

/* Decompress any qtc stream that holds a float32 tensor; returns the
 * elements and writes back the element count recorded at encode time. */
int qtc_decompress_tensor(const uint8_t *in, size_t in_len,
                          float **out_data, uint32_t *out_count);

/* Compress a same-element array (quantized weights, int sensors, fp16
 * bit patterns, …). Bit-exact LOSSLESS contest: byte-plane transpose
 * and XOR-delta+transpose (delta width = elem_size), each LZ'd, best
 * wins; with packed_nibbles an extra nibble-split candidate contests
 * (for int4-style packed payloads). Falls back to the STORED lens when
 * nothing beats the raw size. elem_size is 1, 2, 4 or 8.
 * Payload layout: [u32 count][u8 mode][u8 elem][LZ bytes]
 *   mode 0 = transposed raw, 1 = XOR-delta + transpose, 2 = nibble-split,
 *   3 = verbatim (STORED), 4 = per-plane (XOR-delta + transpose, each
 *   plane LZ or verbatim; multi-byte elements only),
 *   5 = second-order arithmetic delta + transpose.
 * count is element count (raw bytes = count*elem_size). */
int qtc_compress_array(const uint8_t *data, uint32_t count, uint8_t elem_size,
                       int packed_nibbles,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts);

/* Decompress a qtc array stream; returns bytes, element count and elem_size. */
int qtc_decompress_array(const uint8_t *in, size_t in_len,
                         uint8_t **out_data, uint32_t *out_count,
                         uint8_t *out_elem_size);

/* ── Text lens (v0.17.0) ────────────────────────────────────────────
 *
 * Bit-exact text compression. Three candidates are tried and the
 * smallest payload that beats the input wins; ties resolve in mode
 * order so identical input always yields identical bytes. Falls back
 * to verbatim STORED framing when nothing compresses, so the result is
 * never larger than the input plus a 6-byte header.
 *
 *   mode 0 TOKEN  word-aware dictionary + rANS over the token stream
 *   mode 1 FRONT  front-coded lines, then LZ2 (JSON/CSV/logs/source)
 *   mode 2 LZ2    the generic byte-level winner, kept so text can never
 *                 regress against the existing path
 *   mode 3 STORED verbatim
 *
 * Payload layout: [u32 raw_len][u8 mode][u8 aux][mode-specific body]
 * FRONT carries a further [u32 intermediate_len] before its LZ2 stream.
 *
 * level: 3 = full contest (default), 1 = fast subset (skips FRONT),
 * 0 = store (verbatim framing, memcpy speed, ratio ~1.0).
 * Inputs whose distinct-token count exceeds 65535 cannot use TOKEN (the
 * alphabet would not fit uint16) and fall through to the other modes. */
int qtc_compress_text(const uint8_t *data, size_t len,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts);

/* Decompress a text stream into out, which must be exactly raw_len bytes
 * (the value stored in the header). Never reads past in_len and never
 * writes past out_len; a crafted stream returns an error. */
int qtc_decompress_text(const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_len);

/* ── Table lens (v0.18.0) ───────────────────────────────────────────
 *
 * Compress delimited text (CSV/TSV) by CONTESTING row-major against
 * column-major and keeping whichever encodes smaller. Column-major wins
 * large on numeric tables (measured -34% on a 165 kB CSV) because the
 * bytes are already compressible and only their order defeats LZ; row
 * order wins on categorical tables (-26%), which repeat whole rows that
 * front-coding already collapses. Contesting both can never lose to not
 * doing this at all.
 *
 * The transpose is only attempted for a strict rectangle (one
 * delimiter, no quoting, no CR, no empty/ragged rows), so the inverse
 * is exactly lossless; anything else falls back to storing the input
 * verbatim through the text lens. Comma/semicolon/tab/pipe are detected
 * automatically and only when one clearly dominates.
 *
 * Bit-exact: qtc_decompress_table reproduces the input byte for byte. */
int qtc_compress_table(const uint8_t *data, size_t len,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts);
int qtc_decompress_table(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_len);

/* Lossless-only image compression: the 2D MED prediction lens, with no
 * lossy rate-distortion contest. Use when the pixels must round-trip
 * exactly (e.g. just transcoded out of a PNG/BMP). Falls back to STORED
 * when prediction cannot beat the raw size. */
int qtc_compress_image_lossless(const uint8_t *pixels, uint32_t w, uint32_t h,
                                uint8_t ch, uint8_t **out, size_t *out_len,
                                const qtc_options_t *opts);

/* Compress a sequence of same-geometry video frames (nframes frames of
 * h × w × ch interleaved pixels, contiguous). Bit-exact LOSSLESS:
 * flat / temporal-delta / delta+MED contest + LZ (VIDEO lens, Phase 5.6).
 * Falls back to the STORED lens when nothing beats the raw size. */
int qtc_compress_video(const uint8_t *frames, uint32_t nframes,
                       uint32_t w, uint32_t h, uint8_t ch,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts);

/* Decompress any qtc stream that holds video frames; returns the frames
 * and writes back the geometry recorded at encode time. */
int qtc_decompress_video(const uint8_t *in, size_t in_len,
                         uint8_t **out_frames, uint32_t *out_nframes,
                         uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch);

/* ── Utility ─────────────────────────────────────────────────────────── */
const char *qtc_error_string(int code);
const char *qtc_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* QTC_H */
