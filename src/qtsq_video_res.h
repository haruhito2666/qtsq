/* qtsq_video_res.h — sparse residual coding for pixel-path P-frames.
 *
 * A mode-2 macroblock stores bw*bh*ch residual bytes even when almost all of
 * them quantize to zero, so a 16x16 RGB block costs a full 768 raw bytes and a
 * keyframe-adjacent P-frame pays that for every block. At typical quantizers
 * most of those bytes are zero.
 *
 * The sparse form codes runs of non-zero samples instead:
 *   [u8 n_runs]                  number of non-zero runs (1..255)
 *   n_runs x:
 *     [varint zero_run]          zeros before this run
 *     [varint run_len]           length of this run (1..255)
 *     [run_len x i8 value]       two's-complement residual levels
 *
 * A dense block is not improved by this (it costs 3 + blen bytes), so the
 * encoder falls back to the existing dense mode per block whenever sparse
 * would not be smaller. That makes sparse coding strictly non-harmful.
 *
 * varint is LEB128, 7 payload bits per byte, high bit = continuation.
 */
#ifndef QTSQ_VIDEO_RES_H
#define QTSQ_VIDEO_RES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline size_t qtsq_res_varint_size(uint32_t v) {
    size_t n = 1;
    while (v >= 0x80u) { v >>= 7; n++; }
    return n;
}

static inline size_t qtsq_res_varint_put(uint8_t *out, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80u) { out[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    out[n++] = (uint8_t)v;
    return n;
}

static inline size_t qtsq_res_varint_get(const uint8_t *in, size_t len,
                                         uint32_t *out) {
    uint32_t v = 0;
    size_t n = 0;
    for (; n < len && n < 5; n++) {
        v |= (uint32_t)(in[n] & 0x7Fu) << (7 * n);
        if (!(in[n] & 0x80u)) { *out = v; return n + 1; }
    }
    return 0;   /* malformed or truncated */
}

/* Count runs and non-zero samples, and the exact encoded size. */
static inline void qtsq_res_sparse_scan(const uint8_t *res, size_t blen,
                                        uint32_t *out_runs, uint32_t *out_nz,
                                        size_t *out_size) {
    uint32_t runs = 0, nz = 0;
    size_t i = 0;
    while (i < blen) {
        if (!res[i]) { i++; continue; }
        size_t start = i;
        while (i < blen && res[i]) i++;
        runs++;
        nz += (uint32_t)(i - start);
    }
    /* Exact size is cheaper to recompute than to carry around. */
    if (runs == 0 || runs > 255) {
        *out_size = 0; *out_runs = runs; *out_nz = nz; return;
    }
    size_t sz = 1 + nz;
    i = 0;
    size_t prev_end = 0;
    while (i < blen) {
        if (!res[i]) { i++; continue; }
        size_t start = i;
        while (i < blen && res[i]) i++;
        sz += qtsq_res_varint_size((uint32_t)(start - prev_end));
        sz += qtsq_res_varint_size((uint32_t)(i - start));
        prev_end = i;
    }
    *out_runs = runs;
    *out_nz = nz;
    *out_size = sz;
}

/* Write the sparse form. Caller must have checked the size fits in 255 runs
 * (blocks are at most 16*16*3 = 768 samples but a single run covers them all,
 * so run count is always 1 in that case). */
static inline size_t qtsq_res_sparse_write(uint8_t *out, const uint8_t *res,
                                            size_t blen) {
    size_t o = 0;
    uint32_t runs = 0, nz = 0;
    size_t sz = 0;
    qtsq_res_sparse_scan(res, blen, &runs, &nz, &sz);
    if (runs == 0 || runs > 255) return 0;
    out[o++] = (uint8_t)runs;
    size_t i = 0;
    size_t prev_end = 0;
    while (i < blen) {
        if (!res[i]) { i++; continue; }
        size_t start = i;
        while (i < blen && res[i]) i++;
        /* Zero count since the previous run ended — the reader advances
         * incrementally, so this must not be an absolute index. */
        o += qtsq_res_varint_put(out + o, (uint32_t)(start - prev_end));
        o += qtsq_res_varint_put(out + o, (uint32_t)(i - start));
        for (size_t k = start; k < i; k++) out[o++] = res[k];
        prev_end = i;
    }
    return o;
}

/* Decode one sparse block into a dense blen-byte buffer. Returns bytes
 * consumed, or 0 on malformed/truncated input. */
static inline size_t qtsq_res_sparse_read(const uint8_t *in, size_t len,
                                          uint8_t *out, size_t blen) {
    if (len < 1) return 0;
    uint32_t runs = in[0];
    if (runs == 0) return 0;
    size_t o = 1, pos = 0;
    memset(out, 0, blen);
    for (uint32_t r = 0; r < runs; r++) {
        uint32_t zero_run = 0, run_len = 0;
        size_t n = qtsq_res_varint_get(in + o, len - o, &zero_run);
        if (n == 0) return 0;
        o += n;
        n = qtsq_res_varint_get(in + o, len - o, &run_len);
        if (n == 0) return 0;
        o += n;
        if (run_len == 0) return 0;
        if (zero_run > blen - pos) return 0;
        pos += zero_run;
        if (run_len > blen - pos) return 0;
        if (run_len > len - o) return 0;
        memcpy(out + pos, in + o, run_len);
        o += run_len;
        pos += run_len;
    }
    return o;
}

#endif /* QTSQ_VIDEO_RES_H */
