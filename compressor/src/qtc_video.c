/*
 * qtc_video.c — Phase 5.6: lossless VIDEO_FRAMES lens.
 *
 * QTSQ's QTSQ_TYPE_VIDEO_FRAMES payloads are sequences of same-geometry
 * frames (nframes × h × w × ch, contiguous). Inter-frame redundancy is
 * invisible to a generic compressor — it cannot know where one frame ends
 * and the next begins. This lens knows, and runs a contest of four
 * bit-exact transforms (best-of-all):
 *
 *   mode 0 (flat)      — LZ the frame concatenation directly. Wins when
 *                        frames are unrelated (hard cuts, noise).
 *   mode 1 (delta)     — frame 0 verbatim, then each frame stores
 *                        (cur − prev) mod 256 per byte. Slow motion turns
 *                        most residuals into runs of zero.
 *   mode 2 (delta+MED) — the mode-1 residual frames additionally pass
 *                        through the lossless image lens's MED 2D
 *                        predictor, so residual edges and gradients
 *                        compress like the IMAGE lens compresses photos.
 *   mode 3 (dirty-rect, v0.8.0) — frame 0 verbatim, then each frame
 *                        stores only its changed-pixel bounding box: a
 *                        skip marker for identical frames, else the rect
 *                        plus (cur − prev) inside it. Sparse motion with
 *                        a static background costs bytes proportional to
 *                        the moving object, not the frame.
 *   mode 4 (motion comp, v0.9.0) — frame 0 verbatim, then each frame is
 *                        split into 8×8 blocks; each block either skips
 *                        (identical to the same-position block of the
 *                        previous frame) or stores a motion vector
 *                        (±8 px full-pel search in the previous frame,
 *                        SAD criterion) plus the residual against the
 *                        displaced block. This is the mode that wins when
 *                        objects TRANSLATE: dirty rects pay for the whole
 *                        swept area, motion comp pays for the object.
 *
 * All modes round-trip bit-exactly; the smallest payload is emitted.
 *
 * Payload layout:
 *   [u32 w][u32 h][u8 ch][u8 rsv][u32 nframes][u8 mode][u32 stream_len]
 *   [LZ of stream_len transformed bytes]
 *
 * Mode 4 per-block records (after the verbatim frame 0):
 *   0x00                          — skip: copy same-position block
 *   0x01 [u8 dx+8][u8 dy+8]       — motion vector, then bw*bh*ch
 *                                   residual bytes (cur − displaced prev)
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

#define VIDEO_HDR 19u
#define VIDEO_MC_BLOCK 8u       /* motion-compensation block size */
#define VIDEO_MC_RANGE 8        /* ± search range in pixels */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Encode one mode; returns QTC_OK with a malloc'd payload (header + LZ).
 * stream_len receives the transformed byte count (== total for modes 0-2,
 * the variable-size record stream length for mode 3). */
static int video_try(const uint8_t *frames, uint32_t nframes,
                     uint32_t w, uint32_t h, uint8_t ch, uint8_t mode,
                     uint8_t **out, size_t *out_len)
{
    size_t fsz = (size_t)w * h * ch;
    size_t total = fsz * nframes;
    /* Per-mode worst case for the transformed stream. */
    size_t cap;
    if (mode == 3) {
        cap = fsz + (size_t)(nframes - 1) * (11 + fsz) + 16;
    } else if (mode == 4) {
        uint32_t nbx = (w + VIDEO_MC_BLOCK - 1) / VIDEO_MC_BLOCK;
        uint32_t nby = (h + VIDEO_MC_BLOCK - 1) / VIDEO_MC_BLOCK;
        cap = fsz + (size_t)(nframes - 1) * (size_t)nbx * nby *
              (3 + (size_t)VIDEO_MC_BLOCK * VIDEO_MC_BLOCK * ch) + 16;
    } else {
        cap = total;
    }
    uint8_t *work = (uint8_t *)malloc(cap ? cap : 1);
    if (!work)
        return QTC_ERR_ALLOC;
    size_t wlen = 0;

    if (mode == 0) {
        memcpy(work, frames, total);
        wlen = total;
    } else if (mode == 4) {
        /* Motion compensation: 8x8 blocks, ±8 full-pel search in the
         * previous frame, SAD criterion, residual against the displaced
         * block. Identical blocks are skipped with a 1-byte marker. */
        memcpy(work, frames, fsz);            /* frame 0 verbatim */
        wlen = fsz;
        for (uint32_t f = 1; f < nframes; f++) {
            const uint8_t *prev = frames + (size_t)(f - 1) * fsz;
            const uint8_t *cur = frames + (size_t)f * fsz;
            for (uint32_t by = 0; by < h; by += VIDEO_MC_BLOCK)
                for (uint32_t bx = 0; bx < w; bx += VIDEO_MC_BLOCK) {
                    uint32_t bw = VIDEO_MC_BLOCK;
                    uint32_t bh = VIDEO_MC_BLOCK;
                    if (bx + bw > w) bw = w - bx;
                    if (by + bh > h) bh = h - by;

                    /* Skip: block identical at the same position. */
                    int same = 1;
                    for (uint32_t y = 0; y < bh && same; y++)
                        for (uint32_t x = 0; x < bw && same; x++) {
                            size_t pi = ((size_t)(by + y) * w + bx + x) * ch;
                            for (uint8_t q = 0; q < ch; q++)
                                if (cur[pi + q] != prev[pi + q]) {
                                    same = 0;
                                    break;
                                }
                        }
                    if (same) {
                        work[wlen++] = 0;
                        continue;
                    }

                    /* Full search; deterministic tie-break (first minimum
                     * in scan order) so encode and decode agree. */
                    int best_dx = 0, best_dy = 0;
                    uint64_t best_sad = UINT64_MAX;
                    for (int dy = -VIDEO_MC_RANGE; dy <= VIDEO_MC_RANGE; dy++)
                        for (int dx = -VIDEO_MC_RANGE; dx <= VIDEO_MC_RANGE; dx++) {
                            int32_t sx = (int32_t)bx + dx;
                            int32_t sy = (int32_t)by + dy;
                            if (sx < 0 || sy < 0 ||
                                (uint32_t)(sx + bw) > w ||
                                (uint32_t)(sy + bh) > h)
                                continue;
                            uint64_t sad = 0;
                            for (uint32_t y = 0; y < bh; y++)
                                for (uint32_t x = 0; x < bw; x++) {
                                    size_t ci = ((size_t)(by + y) * w +
                                                 bx + x) * ch;
                                    size_t pi = ((size_t)(sy + y) * w +
                                                 (uint32_t)sx + x) * ch;
                                    for (uint8_t q = 0; q < ch; q++) {
                                        int d = (int)cur[ci + q] -
                                                (int)prev[pi + q];
                                        sad += (uint64_t)(d < 0 ? -d : d);
                                    }
                                }
                            if (sad < best_sad) {
                                best_sad = sad;
                                best_dx = dx;
                                best_dy = dy;
                            }
                        }

                    work[wlen++] = 1;
                    work[wlen++] = (uint8_t)(best_dx + VIDEO_MC_RANGE);
                    work[wlen++] = (uint8_t)(best_dy + VIDEO_MC_RANGE);
                    int32_t sx = (int32_t)bx + best_dx;
                    int32_t sy = (int32_t)by + best_dy;
                    for (uint32_t y = 0; y < bh; y++)
                        for (uint32_t x = 0; x < bw; x++) {
                            size_t ci = ((size_t)(by + y) * w + bx + x) * ch;
                            size_t pi = ((size_t)(sy + y) * w +
                                         (uint32_t)sx + x) * ch;
                            for (uint8_t q = 0; q < ch; q++)
                                work[wlen++] =
                                    (uint8_t)(cur[ci + q] - prev[pi + q]);
                        }
                }
        }
    } else if (mode == 3) {
        memcpy(work, frames, fsz);            /* frame 0 verbatim */
        wlen = fsz;
        for (uint32_t f = 1; f < nframes; f++) {
            const uint8_t *prev = frames + (size_t)(f - 1) * fsz;
            const uint8_t *cur = frames + (size_t)f * fsz;
            /* Changed-pixel bounding box (any channel differs). */
            uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
            int changed = 0;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    size_t pi = ((size_t)y * w + x) * ch;
                    size_t n = 0;
                    for (uint8_t q = 0; q < ch; q++)
                        if (cur[pi + q] != prev[pi + q]) { n = 1; break; }
                    if (n) {
                        changed = 1;
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
            if (!changed) {
                work[wlen++] = 0;             /* skip marker */
                continue;
            }
            work[wlen++] = 1;
            work[wlen++] = (uint8_t)(x0 & 0xFF);
            work[wlen++] = (uint8_t)(x0 >> 8);
            work[wlen++] = (uint8_t)(y0 & 0xFF);
            work[wlen++] = (uint8_t)(y0 >> 8);
            work[wlen++] = (uint8_t)(x1 & 0xFF);
            work[wlen++] = (uint8_t)(x1 >> 8);
            work[wlen++] = (uint8_t)(y1 & 0xFF);
            work[wlen++] = (uint8_t)(y1 >> 8);
            for (uint32_t y = y0; y <= y1; y++)
                for (uint32_t x = x0; x <= x1; x++) {
                    size_t pi = ((size_t)y * w + x) * ch;
                    for (uint8_t q = 0; q < ch; q++)
                        work[wlen++] = (uint8_t)(cur[pi + q] - prev[pi + q]);
                }
        }
    } else {
        memcpy(work, frames, fsz);            /* frame 0 verbatim */
        for (uint32_t f = 1; f < nframes; f++) {
            const uint8_t *prev = frames + (size_t)(f - 1) * fsz;
            const uint8_t *cur = frames + (size_t)f * fsz;
            uint8_t *dst = work + (size_t)f * fsz;
            for (size_t i = 0; i < fsz; i++)
                dst[i] = (uint8_t)(cur[i] - prev[i]);
        }
        if (mode == 2) {
            /* MED-predict each residual frame in place (needs a scratch
             * copy: the predictor reads already-decoded neighbors). */
            uint8_t *scratch = (uint8_t *)malloc(fsz);
            if (!scratch) {
                free(work);
                return QTC_ERR_ALLOC;
            }
            for (uint32_t f = 1; f < nframes; f++) {
                memcpy(scratch, work + (size_t)f * fsz, fsz);
                qtc_image_predict_forward(scratch, w, h, ch,
                                          work + (size_t)f * fsz);
            }
            free(scratch);
        }
        wlen = total;
    }

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz_encode(work, wlen, &lz, &lz_len);
    free(work);
    if (rc != QTC_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(VIDEO_HDR + lz_len);
    if (!buf) {
        free(lz);
        return QTC_ERR_ALLOC;
    }
    put32(buf, w);
    put32(buf + 4, h);
    buf[8] = ch;
    buf[9] = 0;
    put32(buf + 10, nframes);
    buf[14] = mode;
    put32(buf + 15, (uint32_t)wlen);
    memcpy(buf + VIDEO_HDR, lz, lz_len);
    free(lz);

    *out = buf;
    *out_len = VIDEO_HDR + lz_len;
    return QTC_OK;
}

int qtc_video_encode(const uint8_t *frames, uint32_t nframes,
                     uint32_t w, uint32_t h, uint8_t ch,
                     uint8_t **out, size_t *out_len)
{
    if (!frames || nframes == 0 || w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    uint64_t total = (uint64_t)nframes * w * h * ch;
    if (total > ((uint64_t)1 << 31))
        return QTC_ERR_SIZE;

    uint8_t *best = NULL;
    size_t best_len = 0;
    /* Single-frame sequences: modes 1-4 degenerate to the exact same
     * pre-LZ stream as mode 0 (frame 0 verbatim, no inter-frame data),
     * so they can only tie, never beat it. Run mode 0 alone — a
     * winner-preserving short-circuit (unlike a size-bound gate, which
     * cannot be sound because LZ output size is not predictable from the
     * pre-LZ stream size). */
    uint8_t max_mode = (nframes == 1) ? 0 : 4;
    for (uint8_t mode = 0; mode <= max_mode; mode++) {
        uint8_t *p = NULL;
        size_t pl = 0;
        int rc = video_try(frames, nframes, w, h, ch, mode, &p, &pl);
        if (rc != QTC_OK) {
            free(best);
            return rc;
        }
        if (!best || pl < best_len) {
            free(best);
            best = p;
            best_len = pl;
        } else {
            free(p);
        }
    }

    *out = best;
    *out_len = best_len;
    return QTC_OK;
}

/* Encode with a single specified mode (no contest). Used by tests to
 * exercise a specific codec path directly. */
int qtc_video_encode_mode(const uint8_t *frames, uint32_t nframes,
                          uint32_t w, uint32_t h, uint8_t ch, uint8_t mode,
                          uint8_t **out, size_t *out_len)
{
    if (!frames || nframes == 0 || w == 0 || h == 0 || ch == 0 || ch > 4)
        return QTC_ERR_FORMAT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_FORMAT;
    if (mode > 4)
        return QTC_ERR_FORMAT;
    if (!out || !out_len)
        return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    uint64_t total = (uint64_t)nframes * w * h * ch;
    if (total > ((uint64_t)1 << 31))
        return QTC_ERR_SIZE;

    return video_try(frames, nframes, w, h, ch, mode, out, out_len);
}

int qtc_video_decode(const uint8_t *payload, size_t payload_len,
                     uint8_t **out_frames, uint32_t *out_nframes,
                     uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch)
{
    if (!payload || !out_frames || !out_nframes || !out_w || !out_h || !out_ch)
        return QTC_ERR_ALLOC;
    *out_frames = NULL;

    if (payload_len < VIDEO_HDR)
        return QTC_ERR_CORRUPT;

    uint32_t w = get32(payload);
    uint32_t h = get32(payload + 4);
    uint8_t ch = payload[8];
    uint32_t nframes = get32(payload + 10);
    uint8_t mode = payload[14];
    uint32_t stream_len = get32(payload + 15);

    if (w == 0 || h == 0 || ch == 0 || ch > 4 || nframes == 0)
        return QTC_ERR_CORRUPT;
    if (w > 65535 || h > 65535)
        return QTC_ERR_CORRUPT;
    if (mode > 4)
        return QTC_ERR_CORRUPT;

    uint64_t total = (uint64_t)nframes * w * h * ch;
    if (total > ((uint64_t)1 << 31))
        return QTC_ERR_SIZE;
    size_t fsz = (size_t)w * h * ch;

    /* The transformed stream length must match the mode's contract:
     * modes 0-2 always produce exactly `total` bytes, so anything else is
     * a corrupted mode byte (their frame walk reads `total` bytes from the
     * stream buffer — a short stream would read past it). Modes 3-4 are
     * variable-size record streams, bounded by their worst cases. */
    if (mode <= 2) {
        if (stream_len != (uint64_t)total)
            return QTC_ERR_CORRUPT;
    } else if (mode == 3) {
        uint64_t min_len = fsz + (uint64_t)(nframes - 1);
        uint64_t max_len = fsz + (uint64_t)(nframes - 1) * (11 + (uint64_t)fsz);
        if (stream_len < min_len || stream_len > max_len)
            return QTC_ERR_CORRUPT;
    } else {
        uint64_t nbx = ((uint64_t)w + VIDEO_MC_BLOCK - 1) / VIDEO_MC_BLOCK;
        uint64_t nby = ((uint64_t)h + VIDEO_MC_BLOCK - 1) / VIDEO_MC_BLOCK;
        uint64_t min_len = fsz + (uint64_t)(nframes - 1);
        uint64_t max_len = fsz + (uint64_t)(nframes - 1) * nbx * nby *
                           (3 + (uint64_t)VIDEO_MC_BLOCK * VIDEO_MC_BLOCK * ch);
        if (stream_len < min_len || stream_len > max_len)
            return QTC_ERR_CORRUPT;
    }

    uint8_t *work = (uint8_t *)malloc((size_t)stream_len);
    uint8_t *out = (uint8_t *)malloc((size_t)total);
    if (!work || !out) {
        free(work); free(out);
        return QTC_ERR_ALLOC;
    }

    size_t consumed = 0;
    int rc = qtc_lz_decode(payload + VIDEO_HDR, payload_len - VIDEO_HDR,
                           stream_len, work, &consumed);
    if (rc != QTC_OK) {
        free(work); free(out);
        return rc;
    }

    if (mode == 0) {
        memcpy(out, work, (size_t)total);
    } else if (mode == 4) {
        /* Motion compensation: replay per-block skip/motion records. */
        memcpy(out, work, fsz);               /* frame 0 verbatim */
        const uint8_t *p = work + fsz;
        const uint8_t *pend = work + stream_len;
        for (uint32_t f = 1; f < nframes && rc == QTC_OK; f++) {
            uint8_t *dst = out + (size_t)f * fsz;
            const uint8_t *prev = out + (size_t)(f - 1) * fsz;
            for (uint32_t by = 0; by < h && rc == QTC_OK;
                 by += VIDEO_MC_BLOCK)
                for (uint32_t bx = 0; bx < w && rc == QTC_OK;
                     bx += VIDEO_MC_BLOCK) {
                    uint32_t bw = VIDEO_MC_BLOCK, bh = VIDEO_MC_BLOCK;
                    if (bx + bw > w) bw = w - bx;
                    if (by + bh > h) bh = h - by;
                    if (p >= pend) { rc = QTC_ERR_CORRUPT; break; }
                    uint8_t kind = *p++;
                    if (kind == 0) {
                        /* Skip: copy the same-position block. */
                        for (uint32_t y = 0; y < bh; y++)
                            memcpy(dst + ((size_t)(by + y) * w + bx) * ch,
                                   prev + ((size_t)(by + y) * w + bx) * ch,
                                   (size_t)bw * ch);
                    } else if (kind == 1) {
                        if ((size_t)(pend - p) < 2) {
                            rc = QTC_ERR_CORRUPT;
                            break;
                        }
                        int dx = (int)p[0] - VIDEO_MC_RANGE;
                        int dy = (int)p[1] - VIDEO_MC_RANGE;
                        p += 2;
                        int32_t sx = (int32_t)bx + dx;
                        int32_t sy = (int32_t)by + dy;
                        if (sx < 0 || sy < 0 ||
                            (uint32_t)(sx + bw) > w ||
                            (uint32_t)(sy + bh) > h) {
                            rc = QTC_ERR_CORRUPT;
                            break;
                        }
                        size_t nres = (size_t)bw * bh * ch;
                        if ((size_t)(pend - p) < nres) {
                            rc = QTC_ERR_CORRUPT;
                            break;
                        }
                        for (uint32_t y = 0; y < bh; y++)
                            for (uint32_t x = 0; x < bw; x++) {
                                size_t ci = ((size_t)(by + y) * w +
                                             bx + x) * ch;
                                size_t pi = ((size_t)(sy + y) * w +
                                             (uint32_t)sx + x) * ch;
                                for (uint8_t q = 0; q < ch; q++)
                                    dst[ci + q] =
                                        (uint8_t)(*p++ + prev[pi + q]);
                            }
                    } else {
                        rc = QTC_ERR_CORRUPT;
                        break;
                    }
                }
        }
        if (rc == QTC_OK && p != pend)
            rc = QTC_ERR_CORRUPT;             /* exact-fit contract */
    } else if (mode == 3) {
        memcpy(out, work, fsz);               /* frame 0 verbatim */
        const uint8_t *p = work + fsz;
        const uint8_t *pend = work + stream_len;
        for (uint32_t f = 1; f < nframes; f++) {
            uint8_t *dst = out + (size_t)f * fsz;
            const uint8_t *prev = out + (size_t)(f - 1) * fsz;
            memcpy(dst, prev, fsz);           /* start from previous */
            if (p >= pend) { rc = QTC_ERR_CORRUPT; break; }
            uint8_t kind = *p++;
            if (kind == 0)
                continue;                     /* identical frame */
            if (kind != 1 || (size_t)(pend - p) < 8) {
                rc = QTC_ERR_CORRUPT;
                break;
            }
            uint32_t x0 = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            uint32_t y0 = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
            uint32_t x1 = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
            uint32_t y1 = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
            p += 8;
            if (x0 > x1 || y0 > y1 || x1 >= w || y1 >= h) {
                rc = QTC_ERR_CORRUPT;
                break;
            }
            size_t nrect = (size_t)(y1 - y0 + 1) * (x1 - x0 + 1) * ch;
            if ((size_t)(pend - p) < nrect) {
                rc = QTC_ERR_CORRUPT;
                break;
            }
            for (uint32_t y = y0; y <= y1; y++)
                for (uint32_t x = x0; x <= x1; x++) {
                    size_t pi = ((size_t)y * w + x) * ch;
                    for (uint8_t q = 0; q < ch; q++)
                        dst[pi + q] = (uint8_t)(*p++ + prev[pi + q]);
                }
        }
        if (rc == QTC_OK && p != pend)
            rc = QTC_ERR_CORRUPT;             /* exact-fit contract */
    } else {
        memcpy(out, work, fsz);               /* frame 0 verbatim */
        uint8_t *scratch = (uint8_t *)malloc(fsz);
        if (!scratch) {
            free(work); free(out);
            return QTC_ERR_ALLOC;
        }
        for (uint32_t f = 1; f < nframes; f++) {
            const uint8_t *resid = work + (size_t)f * fsz;
            uint8_t *dst = out + (size_t)f * fsz;
            const uint8_t *prev = out + (size_t)(f - 1) * fsz;
            if (mode == 2) {
                qtc_image_predict_inverse(resid, w, h, ch, scratch);
                resid = scratch;
            }
            for (size_t i = 0; i < fsz; i++)
                dst[i] = (uint8_t)(resid[i] + prev[i]);
        }
        free(scratch);
    }
    free(work);

    *out_frames = out;
    *out_nframes = nframes;
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return QTC_OK;
}
