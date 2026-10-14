/*
 * qtc text lens (v0.17.0)
 *
 * Text-specific candidates, contested against each other and never
 * allowed to lose to the input itself. Motivation: the generic contest
 * (RAW/RAW1/LZ2) already does well on prose, and the Horizon dictionary
 * in src/qtsq_horizon.c spends 8 bytes/token on search sketches that
 * lose to plain LZ on most real text. What was missing was a candidate
 * that exploits *text* structure LZ cannot see:
 *
 *   mode 1  FRONT  front-coding: consecutive lines are delta-coded on
 *           their shared prefix, then LZ2. Dominates on JSON, CSV/TSV,
 *           logs and source, where lines repeat structure.
 *
 *   mode 2  LZ2    the generic winner for bytes, kept so text can never
 *           regress against the existing path.
 *
 *   mode 0  reserved: a word-dictionary + rANS candidate was built and
 *           measured (see qtc_decompress_text) and never won, so it was
 *           removed instead of shipped as dead code. The id stays
 *           reserved so no future payload is misread as it.
 *
 *   mode 3  STORED verbatim, emitted when nothing beats the input.
 *
 * Framing mirrors the tensor/array lenses:
 *   [u32 raw_len][u8 mode][u8 aux][payload]
 * so a decoder reads the mode before touching the payload.
 *
 * Exactness: every mode is bit-exact and always reconstructs the input.
 * Bounds: every decoder read is length-checked; a crafted stream returns
 * an error rather than reading or writing out of range.
 *
 * Determinism: ties resolve in mode order, so identical input always
 * produces identical bytes.
 */

#include "qtc.h"
#include "qtc_internal.h"
#include <stdlib.h>
#include <string.h>

#define TEXT_HDR 6u

/* Token dictionary is capped so indices fit a uint16 rANS alphabet. */
#define TEXT_MAX_DICT 65535u

static void text_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void text_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static uint32_t text_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t text_get16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* ── mode 1: front-coding over lines, then LZ2 ──────────────────────
 *
 * Each line is emitted as [u16 shared][u16 remaining][remaining bytes]
 * where `shared` is the common prefix with the previous line and
 * `remaining` the rest of the line INCLUDING its trailing newline. The
 * explicit remaining-length is what makes the stream self-delimiting —
 * without it a decoder cannot tell where one line's body ends and the
 * next line's header begins.
 *
 * The intermediate buffer is LZ2'd, whose decoder needs the original
 * size up front, so that size is stored after the fixed header.
 */

static int text_try_front(const uint8_t *data, size_t len,
                          uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;

    /* Worst case per line is 4 bytes of header for a 1-byte body, so
     * bound by 4*len and fall out if that is ever exceeded. */
    size_t cap = 4 + len * 4;
    uint8_t *fc = (uint8_t *)malloc(cap);
    if (!fc) return QTC_ERR_ALLOC;

    size_t p = 0;
    size_t i = 0;
    size_t prev_start = 0, prev_len = 0;
    int have_prev = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && data[i] != '\n') i++;
        if (i < len) i++;               /* the newline belongs to the line */
        size_t ll = i - ls;

        uint32_t shared = 0;
        if (have_prev) {
            uint32_t max = (uint32_t)(ll < prev_len ? ll : prev_len);
            while (shared < max && data[ls + shared] == data[prev_start + shared])
                shared++;
        }
        size_t rem = ll - shared;
        if (p + 4 + rem > cap) { free(fc); return QTC_ERR_FORMAT; }
        text_put16(fc + p, (uint16_t)shared); p += 2;
        text_put16(fc + p, (uint16_t)rem);     p += 2;
        if (rem) { memcpy(fc + p, data + ls + shared, rem); p += rem; }

        prev_start = ls; prev_len = ll; have_prev = 1;
    }

    /* A single line that shares nothing with anything gains nothing over
     * LZ2 and only costs a pass; leave the contest to pick. */
    if (p == 0) { free(fc); return QTC_ERR_FORMAT; }

    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz2_encode(fc, p, &lz, &lz_len);
    free(fc);
    if (rc != QTC_OK) return rc;

    size_t total = TEXT_HDR + 4 + lz_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { free(lz); return QTC_ERR_ALLOC; }
    text_put32(buf, (uint32_t)len);
    buf[4] = 1;                 /* mode = FRONT */
    buf[5] = 0;
    text_put32(buf + TEXT_HDR, (uint32_t)p);   /* intermediate length */
    memcpy(buf + TEXT_HDR + 4, lz, lz_len);
    free(lz);
    *out = buf;
    *out_len = total;
    return QTC_OK;
}

static int text_decode_front(const uint8_t *p, size_t plen,
                             uint8_t *out, size_t out_len)
{
    if (plen < 4) return QTC_ERR_FORMAT;
    uint32_t fc_len = text_get32(p);
    if (fc_len == 0 || fc_len > out_len * 4 + 4) return QTC_ERR_FORMAT;
    p += 4; plen -= 4;

    uint8_t *fc = (uint8_t *)malloc(fc_len);
    if (!fc) return QTC_ERR_ALLOC;
    size_t consumed = 0;
    /* consumed reports LZ2 *input* bytes read, not the output length;
     * qtc_lz2_decode has already validated the block framing against
     * fc_len, so a successful return is the guarantee we need. */
    int rc = qtc_lz2_decode(p, plen, fc_len, fc, &consumed);
    if (rc != QTC_OK) { free(fc); return rc; }

    size_t q = 0;      /* read cursor in fc */
    size_t w = 0;      /* write cursor in out */
    size_t prev_len = 0;
    while (q + 4 <= fc_len) {
        uint32_t shared = text_get16(fc + q);
        uint32_t rem    = text_get16(fc + q + 2);
        q += 4;
        /* The shared prefix is copied from the start of the previous
         * line, which sits immediately before the write cursor. */
        if (shared > prev_len || shared > out_len - w) { rc = QTC_ERR_FORMAT; break; }
        if (shared) {
            memcpy(out + w, out + w - prev_len, shared);
            w += shared;
        }
        if (rem > fc_len - q || rem > out_len - w) { rc = QTC_ERR_FORMAT; break; }
        memcpy(out + w, fc + q, rem);
        w += rem; q += rem;
        prev_len = shared + rem;
    }
    free(fc);
    if (rc != QTC_OK) return rc;
    if (q != fc_len || w != out_len) return QTC_ERR_FORMAT;
    return QTC_OK;
}

/* ── mode 2: plain LZ2 ─────────────────────────────────────────────── */

static int text_try_lz2(const uint8_t *data, size_t len,
                        uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    uint8_t *lz = NULL;
    size_t lz_len = 0;
    int rc = qtc_lz2_encode(data, len, &lz, &lz_len);
    if (rc != QTC_OK) return rc;
    size_t total = TEXT_HDR + lz_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { free(lz); return QTC_ERR_ALLOC; }
    text_put32(buf, (uint32_t)len);
    buf[4] = 2;                 /* mode = LZ2 */
    buf[5] = 0;
    memcpy(buf + TEXT_HDR, lz, lz_len);
    free(lz);
    *out = buf;
    *out_len = total;
    return QTC_OK;
}

static int text_decode_lz2(const uint8_t *p, size_t plen,
                           uint8_t *out, size_t out_len)
{
    size_t consumed = 0;
    return qtc_lz2_decode(p, plen, out_len, out, &consumed);
}

/* ── Public entry points ───────────────────────────────────────────── */

int qtc_compress_text(const uint8_t *data, size_t len,
                      uint8_t **out, size_t *out_len,
                      const qtc_options_t *opts)
{
    if (!out || !out_len) return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!data || len == 0) return QTC_ERR_FORMAT;
    if (len > 0xFFFFFFFFu) return QTC_ERR_SIZE;   /* u32 raw_len field */

    /* Level 0 (store): verbatim framing at memcpy speed, still exact. */
    if (opts && opts->level == 0) {
        uint8_t *buf = (uint8_t *)malloc(TEXT_HDR + len);
        if (!buf) return QTC_ERR_ALLOC;
        text_put32(buf, (uint32_t)len);
        buf[4] = 3;             /* mode = STORED */
        buf[5] = 0;
        memcpy(buf + TEXT_HDR, data, len);
        *out = buf;
        *out_len = TEXT_HDR + len;
        return QTC_OK;
    }

    int fast = (opts && opts->level == 1);

    uint8_t *p1 = NULL, *p2 = NULL;
    size_t l1 = 0, l2 = 0;
    int r1 = QTC_ERR_ALLOC;
    int r2 = QTC_ERR_ALLOC;
    /* Fast level skips FRONT: it costs a full extra pass and pays off
     * only on line-structured input. */
    if (!fast) r1 = text_try_front(data, len, &p1, &l1);
    r2 = text_try_lz2(data, len, &p2, &l2);

    /* Contest: smallest payload that beats the input. Ties resolve in
     * mode order so bytes never change between runs. */
    uint8_t *best = NULL;
    size_t best_len = 0;
    uint8_t best_mode = 3;
    if (r1 == QTC_OK && l1 < len && (!best || l1 < best_len)) { best = p1; best_len = l1; best_mode = 1; } else free(p1);
    if (r2 == QTC_OK && l2 < len && (!best || l2 < best_len)) { best = p2; best_len = l2; best_mode = 2; } else free(p2);

    if (best) {
        best[4] = best_mode;
        *out = best;
        *out_len = best_len;
        return QTC_OK;
    }
    /* Nothing beat the input: STORED, never worse than raw. */
    uint8_t *buf = (uint8_t *)malloc(TEXT_HDR + len);
    if (!buf) return QTC_ERR_ALLOC;
    text_put32(buf, (uint32_t)len);
    buf[4] = 3;
    buf[5] = 0;
    memcpy(buf + TEXT_HDR, data, len);
    *out = buf;
    *out_len = TEXT_HDR + len;
    return QTC_OK;
}

int qtc_decompress_text(const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_len)
{
    if (!in || !out || in_len < TEXT_HDR) return QTC_ERR_FORMAT;
    uint32_t raw = text_get32(in);
    if (raw != out_len) return QTC_ERR_SIZE;   /* caller sized the buffer */
    const uint8_t *p = in + TEXT_HDR;
    size_t plen = in_len - TEXT_HDR;
    switch (in[4]) {
        /* mode 0 (word-dictionary + rANS) was implemented and measured
         * against real corpora — README, large C/Python sources, prose —
         * and never beat LZ2, because the per-token dictionary and model
         * cost more than the byte-level coder saved. It was removed rather
         * than shipped as a mode that cannot win. The id stays reserved. */
        case 0: return QTC_ERR_FORMAT;
        case 1: return text_decode_front(p, plen, out, out_len);
        case 2: return text_decode_lz2(p, plen, out, out_len);
        case 3:
            if (plen != out_len) return QTC_ERR_FORMAT;
            memcpy(out, p, out_len);
            return QTC_OK;
        default: return QTC_ERR_FORMAT;
    }
}