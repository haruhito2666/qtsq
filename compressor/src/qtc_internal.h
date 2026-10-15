/*
 * qtc internal definitions — container format + model structures.
 * Not installed; shared by the src modules and tests.
 */
#ifndef QTC_INTERNAL_H
#define QTC_INTERNAL_H

#include "qtc.h"

/* ── Container format v0 ───────────────────────────────────────────────
 *
 *   [0..3]   magic "SGC\x01"
 *   [4]      lens id (qtc_lens_t)
 *   [5]      flags (bit0: has model table)
 *   [6..7]   reserved (0)
 *   [8..15]  original size, little-endian u64
 *   [16..]   payload
 *
 * RAW lens payload (Phase 1):
 *   [u16 model_len][model bytes][rANS bitstream]
 *
 * STORED lens payload: the original bytes verbatim.
 */
#define QTC_MAGIC0 'S'
#define QTC_MAGIC1 'G'
#define QTC_MAGIC2 'C'
#define QTC_MAGIC3 0x01

#define QTC_HEADER_SIZE 16

#define QTC_FLAG_HAS_MODEL 0x01u

/* ── Statistical model ─────────────────────────────────────────────────
 * Order-0 byte model normalized to M = 4096 (12-bit frequencies).
 * Every symbol present in the input keeps freq >= 1, so no escape
 * mechanism is needed at decode time.
 */
#define QTC_MODEL_M      4096u
#define QTC_MODEL_M_BITS 12u
#define QTC_ALPHABET     256

typedef struct {
    uint16_t freq[QTC_ALPHABET];   /* normalized, sum == QTC_MODEL_M    */
    uint16_t start[QTC_ALPHABET];  /* cumulative offsets (derived)      */
} qtc_model_t;

/* Count bytes and normalize to M, preserving present symbols.
 * Returns the number of distinct symbols (1..256). */
int qtc_model_build(qtc_model_t *m, const uint8_t *data, size_t len);

/* Normalize one row of counts to M, preserving present symbols.
 * Shared by the order-0 and order-1 model builders. */
void qtc_model_normalize_row(uint16_t freq[QTC_ALPHABET],
                             const uint64_t counts[QTC_ALPHABET]);

/* Fill start[] from freq[]. */
void qtc_model_derive_starts(qtc_model_t *m);

/* Serialize only the freq table (16 bits per entry, 512 bytes). */
void qtc_model_pack(const qtc_model_t *m, uint8_t out[512]);
int  qtc_model_unpack(qtc_model_t *m, const uint8_t in[512]);

/* Shannon entropy of the normalized model, bits/symbol. */
double qtc_model_entropy(const qtc_model_t *m);

/* ── rANS core ─────────────────────────────────────────────────────────
 * Byte-based rANS with 32-bit state, L = 1<<23. Invariant during encode
 * and decode: state ∈ [L, L<<8). Symbols are processed in reverse order
 * on encode; the bitstream is emitted least-significant-byte-first and
 * read back the same way, so decode walks forward through the original
 * symbol sequence.
 */
#define QTC_ANS_L (1u << 23)

/* Encode data[0..len) with model m into a malloc'd buffer.
 * Worst-case output size: len + len/255 + 8 bytes. */
int qtc_ans_encode(const uint8_t *data, size_t len, const qtc_model_t *m,
                   uint8_t **out, size_t *out_len);

/* Decode exactly `len` symbols from bitstream using model m.
 * Returns QTC_ERR_CORRUPT if the stream is exhausted early. */
int qtc_ans_decode(const uint8_t *bits, size_t bits_len, size_t len,
                   const qtc_model_t *m, uint8_t *out);

/* ── Order-1 model (Phase 2) ───────────────────────────────────────────
 * Static order-1: 256 per-context tables, one per previous byte. The
 * model is static (counted over the whole input) because rANS encodes in
 * reverse order, which is incompatible with a forward-adaptive model.
 * Context of the first symbol is 0.
 *
 * Wire format of the serialized model:
 *   [u16 present_count]
 *   per present context: [u8 ctx][512 bytes packed freq table]
 *
 * Absent contexts (never seen as a previous byte) are not stored; they
 * can never be queried at decode time because the decoder only follows
 * contexts that actually occurred in the data.
 */
typedef struct {
    qtc_model_t ctx[QTC_ALPHABET];
    uint8_t     present[QTC_ALPHABET]; /* 1 if this context was observed */
} qtc_model1_t;

/* Count (prev,cur) transitions and build all context tables. */
int qtc_model1_build(qtc_model1_t *m, const uint8_t *data, size_t len);

/* Serialize; returns bytes written (≤ 2 + 256*513). */
size_t qtc_model1_pack(const qtc_model1_t *m, uint8_t *out);

/* Deserialize; validates every table sums to M. */
int qtc_model1_unpack(qtc_model1_t *m, const uint8_t *in, size_t in_len,
                      size_t *consumed);

/* Encode/decode with the order-1 model. Same contract as qtc_ans_*. */
int qtc_ans_encode_o1(const uint8_t *data, size_t len, const qtc_model1_t *m,
                      uint8_t **out, size_t *out_len);
int qtc_ans_decode_o1(const uint8_t *bits, size_t bits_len, size_t len,
                      const qtc_model1_t *m, uint8_t *out);

/* ── General-alphabet rANS (Phase 3) ───────────────────────────────────
 * Same byte-renormalized rANS as above, but over an alphabet of A symbols
 * (A ≤ M). Used by the LZ77 lens, whose literal/length stream has a
 * 512-symbol alphabet and whose offset-high stream is also 512-wide.
 * Symbols are uint16_t values in [0, A).
 */
void qtc_model_build_gen(uint16_t *freq, const uint64_t *counts, uint32_t A);
void qtc_model_build_gen_m(uint16_t *freq, const uint64_t *counts, uint32_t A,
                           uint32_t M);
void qtc_model_derive_starts_gen(uint16_t *start, const uint16_t *freq,
                                 uint32_t A);
void qtc_model_pack_gen(const uint16_t *freq, uint32_t A, uint8_t *out);
int  qtc_model_unpack_gen(uint16_t *freq, uint32_t A, const uint8_t *in);
int  qtc_model_unpack_freqs(uint16_t *freq, uint32_t A);
int  qtc_model_unpack_freqs_m(uint16_t *freq, uint32_t A, uint32_t M);

int qtc_ans_encode_gen(const uint16_t *syms, size_t n, uint32_t A,
                       const uint16_t *freq, const uint16_t *start,
                       uint8_t **out, size_t *out_len);
int qtc_ans_decode_gen(const uint8_t *bits, size_t bits_len, size_t n,
                       uint32_t A, const uint16_t *freq, const uint16_t *start,
                       uint16_t *out);

/* Precision-parameterized variants (M = 1 << m_bits) used by the LZ2 lens's
 * 8-bit frequency mode. The rANS state math is identical; only the model
 * total and the renormalization bound change. */
int qtc_ans_encode_gen_m(const uint16_t *syms, size_t n, uint32_t A,
                         const uint16_t *freq, const uint16_t *start,
                         uint32_t m_bits,
                         uint8_t **out, size_t *out_len);
int qtc_ans_decode_gen_m(const uint8_t *bits, size_t bits_len, size_t n,
                         uint32_t A, const uint16_t *freq,
                         const uint16_t *start, uint32_t m_bits,
                         uint16_t *out);

/* Exact reciprocal division for the rANS encode step (qtc_ans.c): returns
 * x/fs and stores x%fs, bit-identical to hardware division for every
 * reachable pair (x < 2^31, fs ∈ [1, QTC_MODEL_M]). */
uint32_t qtc_div_exact(uint32_t x, uint32_t fs, uint32_t *rem);

/* ── LZ77 lens (Phase 3) ───────────────────────────────────────────────
 * Repetition matching: literals + (length, offset) matches, entropy-coded.
 * This is the layer that closes the gap to zlib measured in Phase 2.
 * qtc_lz_encode emits a self-describing payload (blocked for large inputs);
 * qtc_lz_decode reconstructs exactly orig_size bytes into out.
 *
 * Block-count framing (shared with LZ2), 1 MB blocks:
 *   byte 0 in 1..255  → legacy: that value is the block count.
 *   byte 0 == 0       → extended (v0.16.0): bytes 1..4 are a u32 LE count.
 *   The encoder never emits a 0 count (empty input still gets 1 block), so
 *   the sentinel is collision-free with every legacy stream; old decoders
 *   reject extended streams as corrupt (loud failure). This lifts the old
 *   255 MB per-stream limit.
 */
int qtc_lz_encode(const uint8_t *data, size_t len,
                  uint8_t **out, size_t *out_len);
int qtc_lz_decode(const uint8_t *payload, size_t payload_len,
                  size_t orig_size, uint8_t *out, size_t *consumed);

/* ── LZ2 lens (v0.15.0) ────────────────────────────────────────────────
 * Same parse as the LZ lens, but each block is independently encoded in
 * the best of three formats and tagged: v1 (legacy LZ block), or v2 with
 * repeat-offset matches (a rep match reuses the previous offset and skips
 * the bucket + extra-bits streams), truncated presence bitmaps, and an
 * optional 8-bit frequency mode (M = 256) for small blocks.
 *
 * Stream framing: block count per the shared LZ framing above, then
 *   per block: [u32 block_len][u8 version][block payload]
 *   version 0 = v1 block, version 1 = v2 block.
 */
int qtc_lz2_encode(const uint8_t *data, size_t len,
                   uint8_t **out, size_t *out_len);

/* Text lens (v0.17.0) — see qtc.h for the public contract. */
int qtc_compress_text(const uint8_t *data, size_t len,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts);
int qtc_decompress_text(const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_len);
int qtc_lz2_decode(const uint8_t *payload, size_t payload_len,
                   size_t orig_size, uint8_t *out, size_t *consumed);

/* ── Image lens (Phase 4) ──────────────────────────────────────────────
 * 2D prediction (JPEG-LS MED predictor) + mod-256 residual, then LZ on the
 * residual bytes. A generic compressor cannot do 2D spatial prediction, so
 * this is where the library beats zstd on images.
 *
 * Payload: [u32 width][u32 height][u8 channels][u8 reserved][LZ payload]
 */
int qtc_image_encode(const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch,
                     uint8_t **out, size_t *out_len);
int qtc_image_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                     uint8_t *out_ch);

/* 16-bit/channel variant (v0.14.0, raw camera data). MED predictor mod
 * 65536 + contested interleaved/high-low-plane residual layout + LZ.
 * quant 1 = lossless; 2/4/8/16 = near-lossless with max error quant/2 per
 * sample (prediction fed back from reconstructed values, JPEG-LS style).
 *
 * Payload: [u32 w][u32 h][u8 ch][u8 depth=2][u8 layout][u8 quant][LZ]
 */
int qtc_image16_encode(const uint16_t *pixels, uint32_t w, uint32_t h,
                       uint8_t ch, uint8_t quant,
                       uint8_t **out, size_t *out_len);
int qtc_image16_decode(const uint8_t *payload, size_t payload_len,
                       uint16_t **out_pixels, uint32_t *out_w,
                       uint32_t *out_h, uint8_t *out_ch);

/* ── Audio lens (Phase 4) ──────────────────────────────────────────────
 * FLAC-style lossless PCM: stereo mid/side decorrelation + fixed linear
 * prediction (orders 0..4, best per channel per block) + LZ on the
 * residual. bps must be 8 or 16; channels 1..8.
 *
 * Payload: [u32 nframes][u8 channels][u8 bps][u8 decor][u8 reserved]
 *          [LZ payload]
 */
int qtc_audio_encode(const uint8_t *pcm, uint32_t nframes, uint8_t channels,
                     uint8_t bps, uint8_t **out, size_t *out_len);
int qtc_audio_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_pcm, uint32_t *out_nframes,
                     uint8_t *out_channels, uint8_t *out_bps);

/* ── Type-detection router (Phase 4) ───────────────────────────────────
 * The Spaghettification router: classify an input from its first bytes so
 * qtc_compress(AUTO) can route to a domain lens. Conservative by design —
 * a wrong guess can never break a round-trip, because every lens is still
 * verified against the original before it is emitted.
 */
typedef enum {
    QTC_TYPE_GENERIC = 0,    /* no strong signal; use the generic contest */
    QTC_TYPE_COMPRESSED,     /* already entropy-coded (jpeg/zip/...): store */
    QTC_TYPE_AUDIO,          /* raw 16-bit PCM-like: add the audio lens     */
    QTC_TYPE_TEXT            /* mostly printable (informational)            */
} qtc_type_t;

typedef struct {
    qtc_type_t type;
    int        confidence;   /* 0..100 */
} qtc_detect_t;

qtc_detect_t qtc_detect(const uint8_t *data, size_t len);

/* ── MDCT transform (Phase 5) ──────────────────────────────────────────
 * The "Gravitational Lensing" frequency transform. 2N time samples <-> N
 * coefficients, sine window, 50% overlap-add gives perfect reconstruction.
 * FFT-based O(N log N) since v0.9.0 (one 2N-point complex FFT per
 * direction), tables held in a CALLER-OWNED context (thread-safe: no
 * shared state — audit M2).
 */
typedef struct {
    int N;
    double *win;       /* 2N: sine window */
    double *rot_re;    /* 2N: cos(pi n / 2N) pre/post-rotation */
    double *rot_im;    /* 2N: sin(pi n / 2N) */
} qtc_mdct_kernel_t;

int  qtc_mdct_kernel_init(qtc_mdct_kernel_t *k, int N);
void qtc_mdct_kernel_free(qtc_mdct_kernel_t *k);
void qtc_mdct_forward(const qtc_mdct_kernel_t *k, const double *in,
                      double *out);
void qtc_mdct_inverse(const qtc_mdct_kernel_t *k, const double *in,
                      double *out);

/* ── Lossy audio lens: MDCT transform coder (Phase 5) ──────────────────
 * Windowed MDCT + perceptually-weighted quantization + LZ on the quantized
 * indices. LOSSY: output ≈ input, quality controlled by quality ∈ [0,1].
 *
 * Payload: [u32 nframes][u8 channels][u8 bps][u16 N][u8 quality*255]
 *          [u8 reserved][u32 stream_len][LZ of the varint coefficient
 *          stream: nonzero → varint(zigzag(v)), zero runs → 0x00+varint]
 */
int qtc_audio_mdct_encode(const uint8_t *pcm, uint32_t nframes,
                          uint8_t channels, uint8_t bps, float quality,
                          uint8_t **out, size_t *out_len);
int qtc_audio_mdct_decode(const uint8_t *payload, size_t payload_len,
                          uint8_t **out_pcm, uint32_t *out_nframes,
                          uint8_t *out_channels, uint8_t *out_bps);

/* ── Perceptual lossy audio coder (Phase 5.7, lens AUDIO_MDCT2) ────────
 * Mid/side joint stereo + N=1024 MDCT + masking-based per-band bit
 * allocation + two-pass rate control. target_kbps > 0 overrides the
 * quality knob's bitrate mapping (44.1 kHz reference). */
int qtc_audio_mdct2_encode(const uint8_t *pcm, uint32_t nframes,
                           uint8_t channels, uint8_t bps, float quality,
                           int target_kbps, int sample_rate,
                           uint8_t **out, size_t *out_len);
int qtc_audio_mdct2_decode(const uint8_t *payload, size_t payload_len,
                           uint8_t **out_pcm, uint32_t *out_nframes,
                           uint8_t *out_channels, uint8_t *out_bps);

/* ── Float32 audio lens (v0.14.0, lens AUDIO_FLOAT) ────────────────────
 * Lossless (mode 0): deinterleave per channel as u32 bit patterns,
 * contest transform 0 = 4-plane byte transpose vs 1 = per-channel
 * XOR-delta along time + transpose, LZ the winner. Bit-exact for every
 * IEEE-754 pattern (pure byte ops, no float arithmetic).
 * Lossy (mode 1): scaled int16 intermediate fed to the MDCT2 perceptual
 * coder; the peak scale is stored so decode returns floats.
 *
 * Payload: [u32 nframes][u8 channels][u8 mode][u8 param][u8 reserved]
 *   mode 0: param = transform (0/1), then LZ of nframes*channels*4 bytes
 *   mode 1: param = 0, then [float32 peak][MDCT2 payload]
 */
#define QTC_AUDIO_FLOAT_MAX_CH 16

int qtc_audio_float_encode(const float *pcm, uint32_t nframes,
                           uint8_t channels, float quality, int target_kbps,
                           int lossy, int sample_rate,
                           uint8_t **out, size_t *out_len);
int qtc_audio_float_decode(const uint8_t *payload, size_t payload_len,
                           float **out_pcm, uint32_t *out_nframes,
                           uint8_t *out_channels);

/* ── Delaunay triangulation (Phase 5) ──────────────────────────────────
 * Bowyer–Watson incremental construction. Returns a malloc'd flat array of
 * triangle index triples (*out_tris, 3 * *out_ntris ints) over the caller's
 * point indices [0, npts). Caller frees *out_tris.
 */
int qtc_delaunay(const double *xs, const double *ys, int npts,
                 int **out_tris, int *out_ntris);

/* ── Lossy image lens: adaptive mesh (Phase 5) ─────────────────────────
 * Delaunay mesh with error-driven vertex placement + barycentric
 * reconstruction. LOSSY: output ≈ input, quality controlled by quality ∈
 * [0,1]. The triangulation is not stored; decode re-runs Delaunay.
 *
 * Payload: [u32 w][u32 h][u8 ch][u8 reserved][u32 nverts]
 *          [LZ payload of nverts * (u16 x, u16 y, ch color bytes)]
 */
int qtc_image_mesh_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                          uint8_t ch, float quality,
                          uint8_t **out, size_t *out_len);
int qtc_image_mesh_decode(const uint8_t *payload, size_t payload_len,
                          uint8_t **out_pixels, uint32_t *out_w,
                          uint32_t *out_h, uint8_t *out_ch);

/* Barycentric triangle rasterizer shared by the mesh lens and the accretion
 * scene codec (Phase 5.5). Overwrites out for every pixel inside a triangle. */
void qtc_mesh_render(const uint32_t *vx, const uint32_t *vy,
                     const uint8_t *vcol,
                     const int *tris, int ntris,
                     uint32_t w, uint32_t h, uint8_t ch, uint8_t *out);

/* ── Lossy image lens: multi-primitive accretion scene (Phase 5.5) ──────
 * A mixed scene of Gaussian splats (soft base layer, normalized-weight
 * partition of unity), Delaunay mesh vertices (sharp edge layer), and
 * quantized pixel blocks (texture layer). Encode accretes primitives onto
 * error maxima under a quality-derived byte budget, choosing the primitive
 * type per region from local structure. LOSSY.
 *
 * Payload: [u32 w][u32 h][u8 ch][u8 depth-shift][u32 n_splat][u32 n_vert]
 *          [u32 n_block][LZ of splat + vertex + block records]
 */
int qtc_accretion_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, float quality,
                         uint8_t **out, size_t *out_len);
int qtc_accretion_decode(const uint8_t *payload, size_t payload_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch);

/* ── Lossy image lens: 8x8 block-DCT photo coder (Phase 5.5b, v0.12.0) ──
 * YCbCr + 4:2:0 chroma subsampling + orthonormal 8x8 DCT + JPEG-Annex-K
 * quantization scaled by the quality knob + zigzag + DC-DPCM, serialized
 * as low/high byte planes and LZ-compressed. Built to beat JPEG on
 * photographic content. LOSSY. ch must be 1 or 3.
 *
 * Payload: [u32 w][u32 h][u8 ch][u8 q100][u8 subsample][u8 reserved]
 *          [u32 coeff_len][LZ of coeff_len bytes]
 */
int qtc_image_dct_encode(const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint8_t ch, float quality,
                         uint8_t **out, size_t *out_len);
int qtc_image_dct_decode(const uint8_t *payload, size_t payload_len,
                         uint8_t **out_pixels, uint32_t *out_w,
                         uint32_t *out_h, uint8_t *out_ch);

/* ── Float32 tensor lens (Phase 5.6; lossy modes in v0.9.0) ─────────────
 * Lossless: byte-plane transpose (+ optional XOR-delta) + LZ, bit-exact.
 * Lossy (mode 2 bf16 / mode 3 f16): requantize to 16-bit floats, XOR-delta,
 * 2-plane transpose + LZ; decode expands back to float32.
 * Payload: [u32 count][u8 mode][u8 reserved][LZ of the transposed bytes]
 * (public decl in qtc.h carries opts; kept in sync here).
 */
int qtc_tensor_encode(const float *data, size_t count,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts);
int qtc_tensor_encode_lossy(const float *data, size_t count, uint8_t mode,
                            uint8_t **out, size_t *out_len);
int qtc_tensor_decode(const uint8_t *payload, size_t payload_len,
                      float **out_data, size_t *out_count);

/* ── Lossless video lens (Phase 5.6) ────────────────────────────────────
 * nframes same-geometry frames; flat / temporal-delta / delta+MED /
 * dirty-rect / motion-comp contest (mode 4 + per-mode stream_len since
 * v0.9.0).
 * Payload: [u32 w][u32 h][u8 ch][u8 rsv][u32 nframes][u8 mode]
 *          [u32 stream_len][LZ of stream_len bytes]
 */
int qtc_video_encode(const uint8_t *frames, uint32_t nframes,
                     uint32_t w, uint32_t h, uint8_t ch,
                     uint8_t **out, size_t *out_len);
int qtc_video_encode_mode(const uint8_t *frames, uint32_t nframes,
                          uint32_t w, uint32_t h, uint8_t ch, uint8_t mode,
                          uint8_t **out, size_t *out_len);
int qtc_video_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_frames, uint32_t *out_nframes,
                     uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch);

/* MED 2D predictor forward/inverse, shared by the IMAGE and VIDEO lenses.
 * Mod-256 residual domain; exactly invertible. */
void qtc_image_predict_forward(const uint8_t *px, uint32_t w, uint32_t h,
                               uint8_t ch, uint8_t *resid);
void qtc_image_predict_inverse(const uint8_t *resid, uint32_t w, uint32_t h,
                               uint8_t ch, uint8_t *px);

#endif /* QTC_INTERNAL_H */
