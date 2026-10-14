/*
 * Quantum Tensor Sequence — Model Container (exact-weight multi-tensor)
 *
 * One .qtsq file holds a full trained model: N named tensors (any dtype,
 * any shape) + a JSON manifest. All payloads are bit-exact (no sanitize,
 * no delta-quant) so retraining / inference resumes with identical weights.
 *
 * Layout (container streams):
 *   "__manifest__" : JSON {format:"qtsq-model/2", tensors:[{name,kind,dtype,
 *                    shape,stream,elem_size,count}], metadata:{...}}
 *   "<tensor-name>" * N : float32 → TNS2 RAW_BITEXACT (shape in TNS2 dims);
 *                         other dtypes → RAW bytes (shape/dtype in manifest)
 *
 * Manifest versions: "qtsq-model/1" predates stream kinds and is read as
 * all-tensor; "qtsq-model/2" adds a "kind" per entry (tensor/blob/text/
 * image/audio/table, see qtsq_kind_t). The format string is validated on
 * read — it was written but never checked before 2.2, so a truncated or
 * foreign manifest used to surface as a confusing per-tensor error instead
 * of an up-front "unsupported format".
 */

#ifndef QTSQ_MODEL_H
#define QTSQ_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "qtsq_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QTSQ_MODEL_MANIFEST_STREAM "__manifest__"
#define QTSQ_MODEL_FORMAT "qtsq-model/2"

/* Create an empty model container (wraps qtsq_container_create). */
int qtsq_model_create(qtsq_context_t *ctx);

/* Add a float32 tensor bit-exact (uses TENSOR_RAW_BITEXACT). */
int qtsq_model_add_f32(qtsq_context_t *ctx,
                        const char *name,
                        const float *data, size_t count,
                        const uint32_t *dims, uint8_t ndims,
                        int level);

/* Add an arbitrary-dtype tensor as bit-exact raw bytes.
 * `raw` is elem_size*count bytes little-endian (numpy/torch contiguous).
 * Runs the qtc array lens (transpose + delta contest) for elem_size
 * 1/2/4/8; packed_nibbles selects the extra nibble-split candidate for
 * int4-style payloads. dims/ndims recorded when given (model manifest
 * stays authoritative for shapes). */
int qtsq_model_add_bytes(qtsq_context_t *ctx,
                          const char *name,
                          const uint8_t *raw, size_t raw_size,
                          uint8_t elem_size,
                          const uint32_t *dims, uint8_t ndims,
                          int packed_nibbles, int level);

/* Add the JSON manifest stream (horizon-coded). */
int qtsq_model_add_manifest(qtsq_context_t *ctx, const char *manifest_json);

/* Store-mode manifest: raw JSON bytes, no horizon codec pass. */
int qtsq_model_add_manifest_raw(qtsq_context_t *ctx, const char *manifest_json);

/* ── Typed blob streams (qtsq 2.2) ──────────────────────────────────
 *
 * A stream that is not a numeric tensor: text, images, audio, tables,
 * opaque bytes. The payload is framed with a qtsq_blob_header_t ("BLB1",
 * 64 bytes) so the stream is self-describing — a reader gets kind,
 * geometry, original size and a SHA-256 of the original payload from the
 * header alone, without decompressing anything. That is what lets
 * qtsq_container_get_entry / model_info inventory a mixed container.
 *
 *   kind         qtsq_kind_t (text/image/audio/table/blob)
 *   content_type modality-specific hint; see qtsq_content_type_t
 *   param0/1     kind-specific geometry (text: byte length / line count;
 *                 audio: sample rate / frame count; image: width / height)
 *   channels     image and audio channel count (1..16 for audio)
 *   policy       qtsq_blob_policy_t — "keep" stores the bytes verbatim,
 *                the other values are for transcoding callers
 *   level        0 = full contest, 1 = fast, 2 = store
 *
 * Compression is chosen per kind: text runs the qtc text lens (which
 * the router only elects for text-classified input), everything else
 * runs the generic qtc contest. The result is never larger than the
 * input plus the two headers.
 *
 * On failure nothing is added to the container. */
int qtsq_model_add_blob(qtsq_context_t *ctx,
                        const char *name,
                        const uint8_t *raw, size_t raw_size,
                        uint8_t kind, uint16_t content_type,
                        uint32_t param0, uint32_t param1,
                        uint32_t channels,
                        uint8_t bit_depth, uint8_t policy,
                        int level);

/* Typed blob streams count toward the same stream limit and name rules
 * as tensors; the manifest is the caller's job either way. */
int qtsq_model_blob_kind(const qtsq_blob_header_t *hdr);

/* Decode a BLB1 stream back to the original bytes.
 *
 * `in` is the stored payload (64-byte header + body). On success *out is
 * a malloc'd buffer of exactly hdr->original_size bytes and *out_len
 * says how much, regardless of how the body was coded. *out_hdr, when
 * non-NULL, receives a copy of the header so the caller can report kind
 * and geometry.
 *
 * The SHA-256 in the header covers the ORIGINAL bytes, so the returned
 * data is verified as part of decoding; a mismatch is QTSQ_ERR_FORMAT
 * rather than silently wrong output. */
int qtsq_model_decompress_blob(const uint8_t *in, size_t in_len,
                               uint8_t **out, size_t *out_len,
                               qtsq_blob_header_t *out_hdr);

/* ── Image files (qtsq 2.2) ───────────────────────────────────────
 *
 * Store an ENCODED image (PNG/JPEG/BMP/TGA/PPM/GIF) as one stream,
 * either keeping the file bytes or transcoding to a qtsq pixel stream.
 *
 * policy KEEP   (default) stores the encoded bytes verbatim — byte-for-byte
 *               identical to the input, and the right choice for anything
 *               already compressed. Measured: a JPEG re-coded losslessly is
 *               ~6x BIGGER, and a PNG is roughly a tie.
 * policy PIXELS decodes to 8-bit raster and applies the qtc image lens.
 *               Worth it for UNCOMPRESSED inputs: BMP/TGA/PPM of the same
 *               image shrink ~1.15x, and far more on images with flat
 *               regions (the lens is a 2D MED predictor).
 *
 * max_pixels: refuse to decode an image whose pixel count exceeds this
 * (0 = no limit). Untrusted files can declare huge dimensions, so probing
 * first is the cheap way to apply a policy limit before allocating.
 *
 * Either way the stream carries geometry and a SHA-256 of the original,
 * and the caller can read both from the TOC without decoding.
 * On failure nothing is added to the container. */
int qtsq_model_add_image_file(qtsq_context_t *ctx, const char *name,
                              const uint8_t *file_bytes, size_t file_size,
                              uint8_t policy, uint32_t max_pixels,
                              int level);

/* ── Audio (qtsq 2.2) ────────────────────────────────────────────
 *
 * Store interleaved PCM audio as one stream using the qtc audio lens.
 * The lens is self-describing (frames/channels/bits-per-sample live in
 * its payload) and is bit-exact at level 0 — a mid/side + linear
 * prediction coder, measured at ~37x on int16 speech-grade material,
 * well past the ~18x a byte-level coder reaches.
 *
 *   bit_depth    8, 16 or 24 bits per sample (integer PCM)
 *   channels     1..16
 *   param0       sample rate in Hz (recorded, not used for coding)
 *   param1       frame count (also inside the payload; checked on decode)
 *   policy       KEEP stores the raw PCM verbatim; anything else codes it
 *   level        0 = full contest, 1 = fast, 2 = store (verbatim)
 *
 * A frame count that disagrees with the payload, or channels/bits the
 * lens does not support, is rejected rather than written. */
int qtsq_model_add_audio(qtsq_context_t *ctx, const char *name,
                         const uint8_t *pcm, size_t pcm_size,
                         uint8_t channels, uint8_t bit_depth,
                         uint32_t sample_rate, uint8_t policy,
                         int level);

/* Compress one tensor into a standalone context (for parallel builds).
 * buf holds elem-padded little-endian bytes; dims/ndims recorded when
 * given. Caller hands the result to qtsq_container_add_stream_take
 * (zero-copy, preserves order). Thread-safe: contexts never shared. */
int qtsq_model_compress_one(qtsq_context_t *sub,
                            const uint8_t *buf, size_t blen,
                            const uint32_t *dims, uint8_t ndims,
                            int is_f32, long elem, int nibble, int level);

/* Inner hash for the store path: SHA-256 over the exact framed bytes
 * ([hdr6][raw]). Thread-safe; workers hash source bytes in parallel. */
int qtsq_model_hash_inner(uint8_t out32[32],
                          const uint8_t *data, size_t raw_len,
                          int is_f32, uint8_t elem, uint32_t count);

/* Store-mode raw add: prebuilt TNS2 header (owned) + borrowed payload.
 * Byte-identical to the framed store path. hash32 comes from
 * qtsq_model_hash_inner. elem is ignored for f32. */
int qtsq_model_add_raw_hashed(qtsq_context_t *ctx,
                              const char *name,
                              const uint8_t *data, size_t raw_len,
                              int is_f32, uint8_t elem_size,
                              const uint32_t *dims, uint8_t ndims,
                              const uint8_t hash32[32]);

/* Store-mode raw add: prebuilt TNS2 header (owned) + borrowed payload.
 * Byte-identical to the framed store path. hash32 comes from
 * qtsq_model_hash_inner. elem is ignored for f32. */

/* Pack (wraps qtsq_container_pack). Caller encrypts + writes. */
int qtsq_model_pack(qtsq_context_t *ctx);

/* Seal + write with weights UX: password → vault encryption; no
 * password + device keystore → device-key encryption; otherwise
 * sanitized + checksummed plaintext via the explicit escape hatch
 * (integrity + atomic write, confidentiality opt-in). Returns the
 * qtsq_write* status. */
int qtsq_model_write(qtsq_context_t *ctx, const char *filename,
                     const char *password);

/* Pack + seal + write in one step (preferred): decides the plaintext
 * vs encrypted layout BEFORE packing so stream names are stored
 * correctly in both cases (cleartext like safetensors, or
 * name-encrypted under the vault). */
int qtsq_model_pack_write(qtsq_context_t *ctx, const char *filename,
                          const char *password, int durable);

#ifdef __cplusplus
}
#endif

#endif /* QTSQ_MODEL_H */
