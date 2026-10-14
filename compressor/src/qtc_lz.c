/*
 * qtc_lz.c — Phase 3: LZ77 repetition matching.
 *
 * This is the layer that closes the gap to zlib measured in Phase 2
 * (+70% on Canterbury). Entropy coding alone cannot exploit repeated
 * phrases; LZ77 stores "this sequence appeared N bytes ago" instead of
 * re-encoding it.
 *
 * Token stream per block:
 *   - literal:  symbol 0..255
 *   - match:    symbol 256 + (len - MIN_MATCH),  len ∈ [MIN_MATCH, MIN_MATCH+255]
 *   lit/len alphabet = 512, entropy-coded with general rANS.
 *
 * Offsets (per match), DEFLATE-style distance coding:
 *   off1 = offset - 1 ∈ [0, QTC_LZ_WINDOW-1]
 *   - off1 == 0        → bucket 0, 0 extra bits
 *   - off1 >= 1        → bucket = floor(log2(off1)) + 1  (1..20)
 *                        extra  = off1 - 2^(bucket-1), in (bucket-1) bits
 *   Buckets (21-symbol alphabet) are entropy-coded; extra bits are packed
 *   raw (they are uniform within a bucket, so raw is optimal). This makes
 *   small offsets — the common case — cost ~1 bit instead of a fixed 20.
 *
 * v0.9.0 (optimization #7): the window grew from 64 KB to 1 MB — the full
 * block size — so a phrase can reference anything earlier in its block.
 * That is what lets video residuals, audio residuals and transposed
 * float planes reach their reference data; the bucket alphabet gained the
 * four buckets (17..20) for offsets above 64 KB.
 *
 * Large inputs are processed in blocks of up to QTC_LZ_BLOCK bytes so
 * memory stays bounded and the decoder can stream.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

/* v0.10.0 #16: multi-threaded block encoding on POSIX targets. Blocks are
 * fully independent (a match never crosses a block boundary) and each
 * writes its own output slot, so the concatenated stream is bit-identical
 * to the serial path regardless of scheduling. Define QTC_NO_THREADS=1 to
 * force the serial path; non-POSIX targets (WASM, Windows) always use it.
 * lz_encode_block is reentrant (audit M2 removed all shared state). */
#if !defined(QTC_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#define QTC_HAVE_PTHREADS 1
#include <pthread.h>
#include <unistd.h>
#else
#define QTC_HAVE_PTHREADS 0
#endif

#define QTC_LZ_MIN_MATCH   3
#define QTC_LZ_MAX_MATCH   258
#define QTC_LZ_WINDOW      (1u << 20)   /* v0.9.0: 1 MB (was 64 KB) */
#define QTC_LZ_BLOCK       (1u << 20)   /* 1 MB blocks */
#define QTC_LZ_HASH_BITS   20
#define QTC_LZ_HASH_SIZE   (1u << QTC_LZ_HASH_BITS)
#define QTC_LZ_MAX_CHAIN   128

#define LITLEN_ALPHA 512
#define BUCKET_ALPHA 21                 /* buckets 0..20 for a 1 MB window */

/* ── match finder ────────────────────────────────────────────────────── */

/* Hash width is a build-time knob (v0.10.0 #15). Measured on Canterbury +
 * Silesia (283 MB): 4-byte hashing is 1.37x faster AND 0.4% better ratio
 * than 3-byte, because tighter chains spend the 128-candidate budget on
 * real matches instead of false positives. Default is 4-byte; define
 * QTC_LZ_HASH4=0 to restore the legacy 3-byte hash. */
#ifndef QTC_LZ_HASH4
#define QTC_LZ_HASH4 1
#endif

#if QTC_LZ_HASH4
#define LZ_HASH_NEED 4
#else
#define LZ_HASH_NEED QTC_LZ_MIN_MATCH
#endif

static uint32_t lz_hash(const uint8_t *p)
{
#if QTC_LZ_HASH4
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
#else
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
#endif
    return (v * 2654435761u) >> (32 - QTC_LZ_HASH_BITS);
}

/* Find the best match for data[pos..] within the window.
 * Returns match length (>= MIN_MATCH) or 0; *out_off receives offset. */
static uint32_t find_match(const uint8_t *data, size_t block_len, size_t pos,
                           const int32_t *head, const int32_t *prev,
                           uint32_t *out_off)
{
    if (pos + LZ_HASH_NEED > block_len)
        return 0;

    uint32_t h = lz_hash(data + pos);
    size_t limit = pos > QTC_LZ_WINDOW ? pos - QTC_LZ_WINDOW : 0;
    size_t max_len = block_len - pos;
    if (max_len > QTC_LZ_MAX_MATCH)
        max_len = QTC_LZ_MAX_MATCH;

    uint32_t best_len = 0;
    size_t best_off = 0;
    int32_t cand = head[h];
    int chain = QTC_LZ_MAX_CHAIN;

    while (cand >= 0 && (size_t)cand >= limit && chain-- > 0) {
        size_t c = (size_t)cand;
        /* Quick reject: compare the byte at current best_len first. */
        if (best_len == 0 || data[c + best_len] == data[pos + best_len]) {
            uint32_t len = 0;
            while (len < max_len && data[c + len] == data[pos + len])
                len++;
            if (len > best_len) {
                best_len = len;
                best_off = pos - c;
                if (len >= max_len)
                    break;
            }
        }
        cand = prev[c];
    }

    if (best_len >= QTC_LZ_MIN_MATCH) {
        *out_off = (uint32_t)best_off;
        return best_len;
    }
    return 0;
}

/* Insert position pos into the hash chain. */
static void insert_pos(const uint8_t *data, size_t block_len, size_t pos,
                       int32_t *head, int32_t *prev)
{
    if (pos + LZ_HASH_NEED > block_len)
        return;
    uint32_t h = lz_hash(data + pos);
    prev[pos] = head[h];
    head[h] = (int32_t)pos;
}

/* ── offset bucket coding ────────────────────────────────────────────── */

static uint32_t off_bucket(uint32_t off1, uint32_t *extra, uint32_t *extra_bits)
{
    if (off1 == 0) {
        *extra = 0;
        *extra_bits = 0;
        return 0;
    }
    uint32_t b = 0;
    uint32_t v = off1;
    while (v >>= 1)
        b++;                       /* b = floor(log2(off1)), 0..19 */
    *extra_bits = b;               /* bucket-1 extra bits */
    *extra = off1 - (1u << b);
    return b + 1;                  /* bucket 1..16 */
}

/* ── raw bit writer/reader (LSB-first) ───────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   bit_pos;
    size_t   cap_bits;
} bitwriter_t;

static int bw_init(bitwriter_t *bw, size_t cap_bytes)
{
    bw->buf = (uint8_t *)calloc(cap_bytes ? cap_bytes : 1, 1);
    bw->bit_pos = 0;
    bw->cap_bits = cap_bytes * 8;
    return bw->buf ? QTC_OK : QTC_ERR_ALLOC;
}

static void bw_put(bitwriter_t *bw, uint32_t val, uint32_t nbits)
{
    for (uint32_t i = 0; i < nbits; i++) {
        if (bw->bit_pos < bw->cap_bits && (val >> i) & 1)
            bw->buf[bw->bit_pos >> 3] |= (uint8_t)(1u << (bw->bit_pos & 7));
        bw->bit_pos++;
    }
}

typedef struct {
    const uint8_t *buf;
    size_t         bit_pos;
    size_t         cap_bits;
} bitreader_t;

/* Returns 0 on success, QTC_ERR_CORRUPT if the reader is exhausted. */
static int br_get(bitreader_t *br, uint32_t nbits, uint32_t *out)
{
    uint32_t val = 0;
    for (uint32_t i = 0; i < nbits; i++) {
        if (br->bit_pos >= br->cap_bits)
            return QTC_ERR_CORRUPT;
        if (br->buf[br->bit_pos >> 3] & (1u << (br->bit_pos & 7)))
            val |= 1u << i;
        br->bit_pos++;
    }
    *out = val;
    return QTC_OK;
}

/* ── block encode ────────────────────────────────────────────────────── */

/* Pass 1 of every block encoder: greedy + lazy parse into a token stream
 * (literals 0..255, length symbols 256+(len-MIN_MATCH)) with one off1
 * value per match, in token order. Caller frees both output arrays. */
static int lz_parse_block(const uint8_t *data, size_t len,
                          uint16_t **out_tokens, size_t *out_ntok,
                          uint32_t **out_off1, size_t *out_nmatch)
{
    int32_t *head = (int32_t *)malloc(QTC_LZ_HASH_SIZE * sizeof(int32_t));
    int32_t *prev = (int32_t *)malloc(len ? len * sizeof(int32_t) : 1);
    uint16_t *tokens = (uint16_t *)malloc(len ? len * sizeof(uint16_t) : 1);
    uint32_t *match_off1 = (uint32_t *)malloc(len ? len * sizeof(uint32_t) : 1);

    if (!head || !prev || !tokens || !match_off1) {
        free(head); free(prev); free(tokens); free(match_off1);
        return QTC_ERR_ALLOC;
    }

    for (uint32_t i = 0; i < QTC_LZ_HASH_SIZE; i++)
        head[i] = -1;

    size_t ntok = 0, nmatch = 0;
    size_t pos = 0;

    while (pos < len) {
        uint32_t off1 = 0;
        uint32_t len1 = find_match(data, len, pos, head, prev, &off1);

        if (len1 >= QTC_LZ_MIN_MATCH) {
            /* Lazy matching: check if the next position has a better match. */
            insert_pos(data, len, pos, head, prev);
            uint32_t off2 = 0;
            uint32_t len2 = find_match(data, len, pos + 1, head, prev, &off2);

            if (len2 > len1 + 1 && pos + 1 < len) {
                /* Emit literal at pos, take the better match at pos+1. */
                tokens[ntok++] = data[pos];
                pos++;
                insert_pos(data, len, pos, head, prev);
                tokens[ntok++] = (uint16_t)(256 + (len2 - QTC_LZ_MIN_MATCH));
                match_off1[nmatch++] = off2 - 1;
                for (uint32_t k = 1; k < len2; k++)
                    insert_pos(data, len, pos + k, head, prev);
                pos += len2;
            } else {
                tokens[ntok++] = (uint16_t)(256 + (len1 - QTC_LZ_MIN_MATCH));
                match_off1[nmatch++] = off1 - 1;
                for (uint32_t k = 1; k < len1; k++)
                    insert_pos(data, len, pos + k, head, prev);
                pos += len1;
            }
        } else {
            tokens[ntok++] = data[pos];
            insert_pos(data, len, pos, head, prev);
            pos++;
        }
    }

    free(head);
    free(prev);
    *out_tokens = tokens;
    *out_ntok = ntok;
    *out_off1 = match_off1;
    *out_nmatch = nmatch;
    return QTC_OK;
}

/* v1 block assembly (the v0.8.0 format — bit-stable, decoded forever):
 *   [u32 orig_size][u32 num_tokens][u32 num_matches]
 *   [litlen presence bitmap, A/8 bytes][u16 freq per PRESENT symbol]
 *   [bucket presence bitmap][u16 freq per PRESENT symbol]
 *   [u32 litlen_bits_len][litlen bits]
 *   [u32 bucket_bits_len][bucket bits]
 *   [u32 extra_bytes][extra bits]
 * The bitmap skips absent symbols at full u16 precision: typical blocks
 * use a small fraction of the 512-symbol alphabet, so this cuts ~800 B
 * of fixed per-block overhead (the old raw tables were 1024+34 B). */
static int lz_assemble_v1(size_t len, const uint16_t *tokens, size_t ntok,
                          const uint32_t *match_off1, size_t nmatch,
                          uint8_t **out, size_t *out_len)
{
    int rc = QTC_OK;
    uint16_t *buckets = (uint16_t *)malloc(nmatch ? nmatch * 2 : 1);
    uint8_t *litlen_bits = NULL, *bucket_bits = NULL;
    size_t litlen_bits_len = 0, bucket_bits_len = 0;
    bitwriter_t extra_bw = {0};
    uint8_t *payload = NULL;

    if (!buckets) {
        rc = QTC_ERR_ALLOC;
        goto cleanup;
    }

    /* Bucket the offsets, count extra bits. */
    size_t extra_total_bits = 0;
    for (size_t i = 0; i < nmatch; i++) {
        uint32_t extra, extra_bits;
        buckets[i] = (uint16_t)off_bucket(match_off1[i], &extra, &extra_bits);
        extra_total_bits += extra_bits;
    }

    /* Build models. */
    uint64_t litlen_counts[LITLEN_ALPHA] = {0};
    uint64_t bucket_counts[BUCKET_ALPHA] = {0};
    for (size_t i = 0; i < ntok; i++)
        litlen_counts[tokens[i]]++;
    for (size_t i = 0; i < nmatch; i++)
        bucket_counts[buckets[i]]++;

    uint16_t litlen_freq[LITLEN_ALPHA], litlen_start[LITLEN_ALPHA];
    uint16_t bucket_freq[BUCKET_ALPHA], bucket_start[BUCKET_ALPHA];
    qtc_model_build_gen(litlen_freq, litlen_counts, LITLEN_ALPHA);
    qtc_model_derive_starts_gen(litlen_start, litlen_freq, LITLEN_ALPHA);
    qtc_model_build_gen(bucket_freq, bucket_counts, BUCKET_ALPHA);
    qtc_model_derive_starts_gen(bucket_start, bucket_freq, BUCKET_ALPHA);

    /* Entropy-code the two modeled streams. */
    rc = qtc_ans_encode_gen(tokens, ntok, LITLEN_ALPHA,
                            litlen_freq, litlen_start,
                            &litlen_bits, &litlen_bits_len);
    if (rc != QTC_OK)
        goto cleanup;
    if (nmatch > 0) {
        rc = qtc_ans_encode_gen(buckets, nmatch, BUCKET_ALPHA,
                                bucket_freq, bucket_start,
                                &bucket_bits, &bucket_bits_len);
        if (rc != QTC_OK)
            goto cleanup;
    }

    /* Pack extra bits raw. */
    rc = bw_init(&extra_bw, (extra_total_bits + 7) / 8);
    if (rc != QTC_OK)
        goto cleanup;
    for (size_t i = 0; i < nmatch; i++) {
        uint32_t extra, extra_bits;
        off_bucket(match_off1[i], &extra, &extra_bits);
        bw_put(&extra_bw, extra, extra_bits);
    }
    size_t extra_bytes = (extra_total_bits + 7) / 8;

    size_t lit_present = 0, buck_present = 0;
    for (int s = 0; s < LITLEN_ALPHA; s++)
        if (litlen_freq[s]) lit_present++;
    for (int s = 0; s < BUCKET_ALPHA; s++)
        if (bucket_freq[s]) buck_present++;

    size_t payload_len = 4 + 4 + 4
                       + LITLEN_ALPHA / 8 + lit_present * 2
                       + (BUCKET_ALPHA + 7) / 8 + buck_present * 2
                       + 4 + litlen_bits_len
                       + 4 + bucket_bits_len
                       + 4 + extra_bytes;
    payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        rc = QTC_ERR_ALLOC;
        goto cleanup;
    }
    uint8_t *w = payload;
    #define PUT32(v) do { \
        w[0]=(uint8_t)(v); w[1]=(uint8_t)((v)>>8); \
        w[2]=(uint8_t)((v)>>16); w[3]=(uint8_t)((v)>>24); w+=4; \
    } while (0)
    PUT32((uint32_t)len);
    PUT32((uint32_t)ntok);
    PUT32((uint32_t)nmatch);
    memset(w, 0, LITLEN_ALPHA / 8);
    for (int s = 0; s < LITLEN_ALPHA; s++)
        if (litlen_freq[s])
            w[s >> 3] |= (uint8_t)(1u << (s & 7));
    {
        uint8_t *freq_area = w + LITLEN_ALPHA / 8;
        size_t fi = 0;
        for (int s = 0; s < LITLEN_ALPHA; s++)
            if (litlen_freq[s]) {
                freq_area[fi * 2] = (uint8_t)(litlen_freq[s] & 0xFF);
                freq_area[fi * 2 + 1] = (uint8_t)(litlen_freq[s] >> 8);
                fi++;
            }
        w += LITLEN_ALPHA / 8 + lit_present * 2;
    }
    memset(w, 0, (BUCKET_ALPHA + 7) / 8);
    {
        uint8_t *freq_area = w + (BUCKET_ALPHA + 7) / 8;
        size_t fi = 0;
        for (int s = 0; s < BUCKET_ALPHA; s++)
            if (bucket_freq[s]) {
                w[s >> 3] |= (uint8_t)(1u << (s & 7));
                freq_area[fi * 2] = (uint8_t)(bucket_freq[s] & 0xFF);
                freq_area[fi * 2 + 1] = (uint8_t)(bucket_freq[s] >> 8);
                fi++;
            }
        w += (BUCKET_ALPHA + 7) / 8 + buck_present * 2;
    }
    PUT32((uint32_t)litlen_bits_len);
    memcpy(w, litlen_bits, litlen_bits_len); w += litlen_bits_len;
    PUT32((uint32_t)bucket_bits_len);
    if (bucket_bits_len) { memcpy(w, bucket_bits, bucket_bits_len); w += bucket_bits_len; }
    PUT32((uint32_t)extra_bytes);
    if (extra_bytes) { memcpy(w, extra_bw.buf, extra_bytes); w += extra_bytes; }
    #undef PUT32

    *out = payload;
    *out_len = payload_len;
    payload = NULL; /* ownership transferred */

cleanup:
    free(payload);
    free(litlen_bits);
    free(bucket_bits);
    free(extra_bw.buf);
    free(buckets);
    return rc;
}

static int lz_encode_block(const uint8_t *data, size_t len,
                           uint8_t **out, size_t *out_len)
{
    uint16_t *tokens = NULL;
    uint32_t *match_off1 = NULL;
    size_t ntok = 0, nmatch = 0;

    int rc = lz_parse_block(data, len, &tokens, &ntok, &match_off1, &nmatch);
    if (rc != QTC_OK)
        return rc;
    rc = lz_assemble_v1(len, tokens, ntok, match_off1, nmatch, out, out_len);
    free(tokens);
    free(match_off1);
    return rc;
}

/* ── v2 block format (v0.15.0, LZ2 lens) ─────────────────────────────
 * Three wins over v1 for text-like blocks:
 *   1. Repeat-offset matches: one flag bit per match; a rep reuses the
 *      previous match's offset and skips the bucket stream AND the raw
 *      extra bits entirely (the single biggest v1 overhead on text).
 *   2. Truncated presence bitmaps: only (highest present symbol / 8 + 1)
 *      bytes instead of the full alphabet width.
 *   3. Optional 8-bit frequency mode (M = 256): freq table entries are
 *      one byte each — cheaper than the ~4 bits/symbol precision loss
 *      costs on small blocks.
 *
 * Layout:
 *   [u32 orig_size][u32 num_tokens][u32 num_matches]
 *   [u8 flags]  bit0: 8-bit freqs (M=256); other bits reserved (0)
 *   [u32 rep_bytes][rep bits: 1 bit per match, 1 = repeat prev offset]
 *   [u16 litlen_bitmap_bytes][bitmap][freq per present symbol (1 or 2 B)]
 *   [u16 bucket_bitmap_bytes][bitmap][freq per present symbol]
 *   [u32 litlen_bits_len][litlen bits]
 *   [u32 bucket_bits_len][bucket bits]    (non-rep matches only)
 *   [u32 extra_bytes][extra bits]         (non-rep matches only)
 */

#define LZ2_FLAG_U8      0x01u
#define LZ2_U8_MAX_TOK   8192   /* beyond this, 12-bit precision wins back
                                   more than the table bytes it costs */
#define LZ2_REP_NONE     UINT32_MAX  /* "no previous offset" sentinel */

static int lz_assemble_v2(size_t len, const uint16_t *tokens, size_t ntok,
                          const uint32_t *match_off1, size_t nmatch,
                          int use_u8, uint8_t **out, size_t *out_len)
{
    int rc = QTC_OK;
    uint8_t *rep_bits = NULL;
    uint16_t *nb_buckets = NULL;
    uint32_t *nb_off1 = NULL;
    uint8_t *litlen_bits = NULL, *bucket_bits = NULL;
    size_t litlen_bits_len = 0, bucket_bits_len = 0;
    bitwriter_t extra_bw = {0};
    uint8_t *payload = NULL;

    size_t rep_bytes_cnt = (nmatch + 7) / 8;
    rep_bits = (uint8_t *)calloc(rep_bytes_cnt ? rep_bytes_cnt : 1, 1);
    nb_buckets = (uint16_t *)malloc(nmatch ? nmatch * 2 : 1);
    nb_off1 = (uint32_t *)malloc(nmatch ? nmatch * 4 : 1);
    if (!rep_bits || !nb_buckets || !nb_off1) {
        rc = QTC_ERR_ALLOC;
        goto cleanup;
    }

    /* Mark rep matches; bucket only the non-rep ones. */
    size_t nnonrep = 0;
    uint32_t last_off1 = LZ2_REP_NONE;
    for (size_t i = 0; i < nmatch; i++) {
        if (match_off1[i] == last_off1) {
            rep_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        } else {
            uint32_t extra, extra_bits;
            nb_buckets[nnonrep] =
                (uint16_t)off_bucket(match_off1[i], &extra, &extra_bits);
            nb_off1[nnonrep] = match_off1[i];
            nnonrep++;
            last_off1 = match_off1[i];
        }
    }

    /* Build models (8-bit precision when requested). */
    uint32_t M = use_u8 ? 256u : QTC_MODEL_M;
    uint32_t m_bits = use_u8 ? 8u : QTC_MODEL_M_BITS;

    uint64_t litlen_counts[LITLEN_ALPHA] = {0};
    uint64_t bucket_counts[BUCKET_ALPHA] = {0};
    for (size_t i = 0; i < ntok; i++)
        litlen_counts[tokens[i]]++;
    for (size_t i = 0; i < nnonrep; i++)
        bucket_counts[nb_buckets[i]]++;

    uint16_t litlen_freq[LITLEN_ALPHA], litlen_start[LITLEN_ALPHA];
    uint16_t bucket_freq[BUCKET_ALPHA], bucket_start[BUCKET_ALPHA];
    qtc_model_build_gen_m(litlen_freq, litlen_counts, LITLEN_ALPHA, M);
    qtc_model_derive_starts_gen(litlen_start, litlen_freq, LITLEN_ALPHA);
    qtc_model_build_gen_m(bucket_freq, bucket_counts, BUCKET_ALPHA, M);
    qtc_model_derive_starts_gen(bucket_start, bucket_freq, BUCKET_ALPHA);

    if (use_u8) {
        /* 8-bit mode cannot represent more than 256 distinct symbols; the
         * normalizer would emit a table that does not sum to M. Callers
         * treat QTC_ERR_FORMAT from this variant as "variant unavailable". */
        size_t lp = 0;
        for (int s = 0; s < LITLEN_ALPHA; s++)
            if (litlen_freq[s]) lp++;
        if (lp > 256) {
            rc = QTC_ERR_FORMAT;
            goto cleanup;
        }
    }

    /* Entropy-code the modeled streams. */
    rc = qtc_ans_encode_gen_m(tokens, ntok, LITLEN_ALPHA,
                              litlen_freq, litlen_start, m_bits,
                              &litlen_bits, &litlen_bits_len);
    if (rc != QTC_OK)
        goto cleanup;
    if (nnonrep > 0) {
        rc = qtc_ans_encode_gen_m(nb_buckets, nnonrep, BUCKET_ALPHA,
                                  bucket_freq, bucket_start, m_bits,
                                  &bucket_bits, &bucket_bits_len);
        if (rc != QTC_OK)
            goto cleanup;
    }

    /* Pack extra bits raw (non-rep matches only). */
    size_t extra_total_bits = 0;
    for (size_t i = 0; i < nnonrep; i++) {
        uint32_t extra, extra_bits;
        off_bucket(nb_off1[i], &extra, &extra_bits);
        extra_total_bits += extra_bits;
    }
    rc = bw_init(&extra_bw, (extra_total_bits + 7) / 8);
    if (rc != QTC_OK)
        goto cleanup;
    for (size_t i = 0; i < nnonrep; i++) {
        uint32_t extra, extra_bits;
        off_bucket(nb_off1[i], &extra, &extra_bits);
        bw_put(&extra_bw, extra, extra_bits);
    }
    size_t extra_bytes = (extra_total_bits + 7) / 8;

    /* Truncated presence bitmaps. */
    int lit_hi = -1, buck_hi = -1;
    size_t lit_present = 0, buck_present = 0;
    for (int s = 0; s < LITLEN_ALPHA; s++)
        if (litlen_freq[s]) { lit_present++; lit_hi = s; }
    for (int s = 0; s < BUCKET_ALPHA; s++)
        if (bucket_freq[s]) { buck_present++; buck_hi = s; }
    size_t lit_bm_bytes = (lit_hi >= 0) ? (size_t)(lit_hi / 8 + 1) : 0;
    size_t buck_bm_bytes = (buck_hi >= 0) ? (size_t)(buck_hi / 8 + 1) : 0;
    size_t freq_w = use_u8 ? 1 : 2;

    size_t payload_len = 4 + 4 + 4 + 1
                       + 4 + rep_bytes_cnt
                       + 2 + lit_bm_bytes + lit_present * freq_w
                       + 2 + buck_bm_bytes + buck_present * freq_w
                       + 4 + litlen_bits_len
                       + 4 + bucket_bits_len
                       + 4 + extra_bytes;
    payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        rc = QTC_ERR_ALLOC;
        goto cleanup;
    }
    uint8_t *w = payload;
    #define PUT32(v) do { \
        w[0]=(uint8_t)(v); w[1]=(uint8_t)((v)>>8); \
        w[2]=(uint8_t)((v)>>16); w[3]=(uint8_t)((v)>>24); w+=4; \
    } while (0)
    PUT32((uint32_t)len);
    PUT32((uint32_t)ntok);
    PUT32((uint32_t)nmatch);
    *w++ = use_u8 ? LZ2_FLAG_U8 : 0;
    PUT32((uint32_t)rep_bytes_cnt);
    if (rep_bytes_cnt) { memcpy(w, rep_bits, rep_bytes_cnt); w += rep_bytes_cnt; }
    w[0] = (uint8_t)(lit_bm_bytes & 0xFF);
    w[1] = (uint8_t)(lit_bm_bytes >> 8);
    w += 2;
    memset(w, 0, lit_bm_bytes);
    for (int s = 0; s < LITLEN_ALPHA; s++)
        if (litlen_freq[s])
            w[s >> 3] |= (uint8_t)(1u << (s & 7));
    {
        uint8_t *freq_area = w + lit_bm_bytes;
        size_t fi = 0;
        for (int s = 0; s < LITLEN_ALPHA; s++)
            if (litlen_freq[s]) {
                if (use_u8) {
                    /* freq ∈ [1,256] — store freq-1 so 256 (a single-symbol
                     * distribution) fits in one byte. */
                    freq_area[fi] = (uint8_t)(litlen_freq[s] - 1);
                    fi++;
                } else {
                    freq_area[fi * 2] = (uint8_t)(litlen_freq[s] & 0xFF);
                    freq_area[fi * 2 + 1] = (uint8_t)(litlen_freq[s] >> 8);
                    fi++;
                }
            }
        w += lit_bm_bytes + lit_present * freq_w;
    }
    w[0] = (uint8_t)(buck_bm_bytes & 0xFF);
    w[1] = (uint8_t)(buck_bm_bytes >> 8);
    w += 2;
    memset(w, 0, buck_bm_bytes);
    for (int s = 0; s < BUCKET_ALPHA; s++)
        if (bucket_freq[s])
            w[s >> 3] |= (uint8_t)(1u << (s & 7));
    {
        uint8_t *freq_area = w + buck_bm_bytes;
        size_t fi = 0;
        for (int s = 0; s < BUCKET_ALPHA; s++)
            if (bucket_freq[s]) {
                if (use_u8) {
                    freq_area[fi] = (uint8_t)(bucket_freq[s] - 1);
                    fi++;
                } else {
                    freq_area[fi * 2] = (uint8_t)(bucket_freq[s] & 0xFF);
                    freq_area[fi * 2 + 1] = (uint8_t)(bucket_freq[s] >> 8);
                    fi++;
                }
            }
        w += buck_bm_bytes + buck_present * freq_w;
    }
    PUT32((uint32_t)litlen_bits_len);
    memcpy(w, litlen_bits, litlen_bits_len); w += litlen_bits_len;
    PUT32((uint32_t)bucket_bits_len);
    if (bucket_bits_len) { memcpy(w, bucket_bits, bucket_bits_len); w += bucket_bits_len; }
    PUT32((uint32_t)extra_bytes);
    if (extra_bytes) { memcpy(w, extra_bw.buf, extra_bytes); w += extra_bytes; }
    #undef PUT32

    *out = payload;
    *out_len = payload_len;
    payload = NULL; /* ownership transferred */

cleanup:
    free(payload);
    free(litlen_bits);
    free(bucket_bits);
    free(extra_bw.buf);
    free(rep_bits);
    free(nb_buckets);
    free(nb_off1);
    return rc;
}

/* Encode one block in the smallest of v1 / v2-u16 / v2-u8. *out_ver
 * receives the block version tag (0 = v1, 1 = v2). */
static int lz_encode_block_best(const uint8_t *data, size_t len,
                                uint8_t **out, size_t *out_len,
                                uint8_t *out_ver)
{
    uint16_t *tokens = NULL;
    uint32_t *match_off1 = NULL;
    size_t ntok = 0, nmatch = 0;
    uint8_t *p0 = NULL, *p1 = NULL, *p2 = NULL;
    size_t l0 = 0, l1 = 0, l2 = 0;

    int rc = lz_parse_block(data, len, &tokens, &ntok, &match_off1, &nmatch);
    if (rc != QTC_OK)
        return rc;

    rc = lz_assemble_v1(len, tokens, ntok, match_off1, nmatch, &p0, &l0);
    if (rc != QTC_OK)
        goto done;
    rc = lz_assemble_v2(len, tokens, ntok, match_off1, nmatch, 0, &p1, &l1);
    if (rc != QTC_OK)
        goto done;
    if (ntok <= LZ2_U8_MAX_TOK) {
        uint8_t seen[LITLEN_ALPHA] = {0};
        size_t distinct = 0;
        for (size_t i = 0; i < ntok; i++)
            if (!seen[tokens[i]]) { seen[tokens[i]] = 1; distinct++; }
        if (distinct <= 256) {
            int rc2 = lz_assemble_v2(len, tokens, ntok, match_off1, nmatch,
                                     1, &p2, &l2);
            if (rc2 == QTC_ERR_FORMAT) {
                p2 = NULL;   /* variant unavailable */
            } else if (rc2 != QTC_OK) {
                rc = rc2;
                goto done;
            }
        }
    }

    {
        uint8_t *best = p0; size_t best_len = l0; uint8_t best_ver = 0;
        if (p1 && l1 < best_len) { best = p1; best_len = l1; best_ver = 1; }
        if (p2 && l2 < best_len) { best = p2; best_len = l2; best_ver = 1; }
        *out = best;
        *out_len = best_len;
        *out_ver = best_ver;
        if (best != p0) free(p0);
        if (best != p1) free(p1);
        if (best != p2) free(p2);
    }

done:
    free(tokens);
    free(match_off1);
    return rc;
}

/* ── block decode ────────────────────────────────────────────────────── */

/* Read a little-endian u32 and advance the pointer. */
static uint32_t get32(const uint8_t **r)
{
    uint32_t v = (uint32_t)(*r)[0] | ((uint32_t)(*r)[1] << 8) |
                 ((uint32_t)(*r)[2] << 16) | ((uint32_t)(*r)[3] << 24);
    *r += 4;
    return v;
}

static int lz_decode_block(const uint8_t *payload, size_t payload_len,
                           uint8_t *out, size_t *consumed)
{
    /* v0.8.0 packed models: bitmap + present-symbol u16 freqs. Every read
     * is bounds-checked against payload_len as it goes. */
    if (payload_len < 12 + LITLEN_ALPHA / 8)
        return QTC_ERR_CORRUPT;

    const uint8_t *r = payload;
    const uint8_t *rend = payload + payload_len;
    uint32_t orig_size = get32(&r);
    uint32_t ntok = get32(&r);
    uint32_t nmatch = get32(&r);

    uint16_t litlen_freq[LITLEN_ALPHA], litlen_start[LITLEN_ALPHA];
    uint16_t bucket_freq[BUCKET_ALPHA], bucket_start[BUCKET_ALPHA];

    /* Unpack the litlen model from its presence bitmap. */
    memset(litlen_freq, 0, sizeof(litlen_freq));
    {
        size_t present = 0;
        for (int s = 0; s < LITLEN_ALPHA; s++)
            if (r[s >> 3] & (1u << (s & 7)))
                present++;
        if ((size_t)(rend - r) < LITLEN_ALPHA / 8 + present * 2)
            return QTC_ERR_CORRUPT;
        const uint8_t *fr = r + LITLEN_ALPHA / 8;
        for (int s = 0; s < LITLEN_ALPHA; s++)
            if (r[s >> 3] & (1u << (s & 7))) {
                litlen_freq[s] = (uint16_t)(fr[0] | ((uint16_t)fr[1] << 8));
                fr += 2;
            }
        r += LITLEN_ALPHA / 8 + present * 2;
    }
    int rc = qtc_model_unpack_freqs(litlen_freq, LITLEN_ALPHA);
    if (rc != QTC_OK) return rc;

    /* Unpack the bucket model the same way. */
    if ((size_t)(rend - r) < (BUCKET_ALPHA + 7) / 8)
        return QTC_ERR_CORRUPT;
    memset(bucket_freq, 0, sizeof(bucket_freq));
    {
        size_t present = 0;
        for (int s = 0; s < BUCKET_ALPHA; s++)
            if (r[s >> 3] & (1u << (s & 7)))
                present++;
        if ((size_t)(rend - r) < (BUCKET_ALPHA + 7) / 8 + present * 2)
            return QTC_ERR_CORRUPT;
        const uint8_t *fr = r + (BUCKET_ALPHA + 7) / 8;
        for (int s = 0; s < BUCKET_ALPHA; s++)
            if (r[s >> 3] & (1u << (s & 7))) {
                bucket_freq[s] = (uint16_t)(fr[0] | ((uint16_t)fr[1] << 8));
                fr += 2;
            }
        r += (BUCKET_ALPHA + 7) / 8 + present * 2;
    }
    rc = qtc_model_unpack_freqs(bucket_freq, BUCKET_ALPHA);
    if (rc != QTC_OK) return rc;
    qtc_model_derive_starts_gen(litlen_start, litlen_freq, LITLEN_ALPHA);
    qtc_model_derive_starts_gen(bucket_start, bucket_freq, BUCKET_ALPHA);

    if ((size_t)(r - payload) + 4 > payload_len)
        return QTC_ERR_CORRUPT;
    uint32_t litlen_bits_len = get32(&r);
    if ((size_t)(r - payload) + litlen_bits_len > payload_len)
        return QTC_ERR_CORRUPT;

    uint16_t *tokens = (uint16_t *)malloc(ntok ? (size_t)ntok * 2 : 1);
    uint16_t *buckets = (uint16_t *)malloc(nmatch ? (size_t)nmatch * 2 : 1);
    if (!tokens || !buckets) {
        free(tokens); free(buckets);
        return QTC_ERR_ALLOC;
    }

    rc = qtc_ans_decode_gen(r, litlen_bits_len, ntok, LITLEN_ALPHA,
                            litlen_freq, litlen_start, tokens);
    if (rc != QTC_OK) goto cleanup;
    r += litlen_bits_len;

    if ((size_t)(r - payload) + 4 > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    uint32_t bucket_bits_len = get32(&r);
    if ((size_t)(r - payload) + bucket_bits_len > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    if (nmatch > 0) {
        rc = qtc_ans_decode_gen(r, bucket_bits_len, nmatch, BUCKET_ALPHA,
                                bucket_freq, bucket_start, buckets);
        if (rc != QTC_OK) goto cleanup;
    }
    r += bucket_bits_len;

    if ((size_t)(r - payload) + 4 > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    uint32_t extra_bytes = get32(&r);
    if ((size_t)(r - payload) + extra_bytes > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    bitreader_t br = { r, 0, (size_t)extra_bytes * 8 };
    r += extra_bytes;

    /* Replay tokens. */
    size_t opos = 0, mi = 0;
    for (uint32_t i = 0; i < ntok; i++) {
        uint32_t t = tokens[i];
        if (t < 256) {
            if (opos >= orig_size) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            out[opos++] = (uint8_t)t;
        } else {
            if (mi >= nmatch) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            uint32_t len = (t - 256) + QTC_LZ_MIN_MATCH;
            uint32_t bucket = buckets[mi];
            uint32_t off1;
            if (bucket >= BUCKET_ALPHA) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            if (bucket == 0) {
                off1 = 0;
            } else {
                uint32_t extra_bits = bucket - 1;
                uint32_t extra;
                if (extra_bits > 19) { rc = QTC_ERR_CORRUPT; goto cleanup; }
                rc = br_get(&br, extra_bits, &extra);
                if (rc != QTC_OK) goto cleanup;
                off1 = (1u << (bucket - 1)) + extra;
            }
            mi++;
            uint32_t off = off1 + 1;
            if (off > opos || opos + len > orig_size) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            for (uint32_t k = 0; k < len; k++)
                out[opos + k] = out[opos - off + k];
            opos += len;
        }
    }
    if (opos != orig_size) { rc = QTC_ERR_CORRUPT; goto cleanup; }

    *consumed = (size_t)(r - payload);
    rc = QTC_OK;

cleanup:
    free(tokens);
    free(buckets);
    return rc;
}

/* Read a little-endian u16 and advance the pointer. */
static uint32_t get16(const uint8_t **r)
{
    uint32_t v = (uint32_t)(*r)[0] | ((uint32_t)(*r)[1] << 8);
    *r += 2;
    return v;
}

/* Unpack one truncated-bitmap model section (v2). Returns the number of
 * present symbols, or -1 on corruption. Advances *r past the freqs. */
static int lz2_unpack_model(const uint8_t **r, const uint8_t *rend,
                            uint16_t *freq, uint32_t A, int use_u8)
{
    size_t freq_w = use_u8 ? 1 : 2;
    uint32_t M = use_u8 ? 256u : QTC_MODEL_M;
    uint32_t bm_max = (A + 7) / 8;

    if ((size_t)(rend - *r) < 2)
        return -1;
    uint32_t bm_bytes = get16(r);
    if (bm_bytes > bm_max)
        return -1;
    if ((size_t)(rend - *r) < bm_bytes)
        return -1;

    memset(freq, 0, A * sizeof(uint16_t));
    const uint8_t *bm = *r;
    size_t present = 0;
    for (uint32_t s = 0; s < bm_bytes * 8; s++)
        if (bm[s >> 3] & (1u << (s & 7)))
            present++;
    if ((size_t)(rend - *r) < bm_bytes + present * freq_w)
        return -1;
    const uint8_t *fr = *r + bm_bytes;
    for (uint32_t s = 0; s < bm_bytes * 8; s++) {
        if (!(bm[s >> 3] & (1u << (s & 7))))
            continue;
        uint32_t f;
        if (use_u8) {
            f = (uint32_t)*fr++ + 1;   /* stored as freq-1 (freq ∈ [1,256]) */
        } else {
            f = (uint32_t)fr[0] | ((uint32_t)fr[1] << 8);
            fr += 2;
        }
        if (f == 0)
            return -1;   /* present symbol with zero frequency */
        freq[s] = (uint16_t)f;
    }
    *r += bm_bytes + present * freq_w;

    /* An empty model (present == 0) is legal for the bucket section when
     * every match is a rep; the caller refuses to decode symbols against
     * it (ntok/nnonrep guards), so only non-empty tables need the sum
     * check. */
    if (present > 0 && qtc_model_unpack_freqs_m(freq, A, M) != QTC_OK)
        return -1;
    return (int)present;
}

static int lz_decode_block_v2(const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *consumed)
{
    if (payload_len < 13)
        return QTC_ERR_CORRUPT;

    const uint8_t *r = payload;
    const uint8_t *rend = payload + payload_len;
    uint32_t orig_size = get32(&r);
    uint32_t ntok = get32(&r);
    uint32_t nmatch = get32(&r);
    uint8_t flags = *r++;
    if (flags & ~LZ2_FLAG_U8)
        return QTC_ERR_CORRUPT;
    int use_u8 = (flags & LZ2_FLAG_U8) != 0;
    uint32_t m_bits = use_u8 ? 8u : QTC_MODEL_M_BITS;

    /* Rep flags: exactly one bit per match. */
    if ((size_t)(rend - r) < 4)
        return QTC_ERR_CORRUPT;
    uint32_t rep_bytes = get32(&r);
    if (rep_bytes != (nmatch + 7u) / 8u)
        return QTC_ERR_CORRUPT;
    if ((size_t)(rend - r) < rep_bytes)
        return QTC_ERR_CORRUPT;
    const uint8_t *rep_bits = r;
    r += rep_bytes;

    size_t nrep = 0;
    for (uint32_t i = 0; i < nmatch; i++)
        if (rep_bits[i >> 3] & (1u << (i & 7)))
            nrep++;
    size_t nnonrep = (size_t)nmatch - nrep;

    /* Models. */
    uint16_t litlen_freq[LITLEN_ALPHA], litlen_start[LITLEN_ALPHA];
    uint16_t bucket_freq[BUCKET_ALPHA], bucket_start[BUCKET_ALPHA];

    int lit_present = lz2_unpack_model(&r, rend, litlen_freq, LITLEN_ALPHA,
                                       use_u8);
    if (lit_present < 0)
        return QTC_ERR_CORRUPT;
    if (ntok > 0 && lit_present == 0)
        return QTC_ERR_CORRUPT;
    int buck_present = lz2_unpack_model(&r, rend, bucket_freq, BUCKET_ALPHA,
                                        use_u8);
    if (buck_present < 0)
        return QTC_ERR_CORRUPT;
    if (nnonrep > 0 && buck_present == 0)
        return QTC_ERR_CORRUPT;
    qtc_model_derive_starts_gen(litlen_start, litlen_freq, LITLEN_ALPHA);
    qtc_model_derive_starts_gen(bucket_start, bucket_freq, BUCKET_ALPHA);

    /* Litlen bitstream. */
    if ((size_t)(r - payload) + 4 > payload_len)
        return QTC_ERR_CORRUPT;
    uint32_t litlen_bits_len = get32(&r);
    if ((size_t)(r - payload) + litlen_bits_len > payload_len)
        return QTC_ERR_CORRUPT;

    uint16_t *tokens = (uint16_t *)malloc(ntok ? (size_t)ntok * 2 : 1);
    uint16_t *buckets = (uint16_t *)malloc(nnonrep ? nnonrep * 2 : 1);
    if (!tokens || !buckets) {
        free(tokens); free(buckets);
        return QTC_ERR_ALLOC;
    }

    int rc = qtc_ans_decode_gen_m(r, litlen_bits_len, ntok, LITLEN_ALPHA,
                                  litlen_freq, litlen_start, m_bits, tokens);
    if (rc != QTC_OK) goto cleanup;
    r += litlen_bits_len;

    /* Bucket bitstream: non-rep matches only. */
    if ((size_t)(r - payload) + 4 > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    uint32_t bucket_bits_len = get32(&r);
    if ((size_t)(r - payload) + bucket_bits_len > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    if (nnonrep > 0) {
        rc = qtc_ans_decode_gen_m(r, bucket_bits_len, nnonrep, BUCKET_ALPHA,
                                  bucket_freq, bucket_start, m_bits, buckets);
        if (rc != QTC_OK) goto cleanup;
    }
    r += bucket_bits_len;

    if ((size_t)(r - payload) + 4 > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    uint32_t extra_bytes = get32(&r);
    if ((size_t)(r - payload) + extra_bytes > payload_len) { rc = QTC_ERR_CORRUPT; goto cleanup; }
    bitreader_t br = { r, 0, (size_t)extra_bytes * 8 };
    r += extra_bytes;

    /* Replay tokens with repeat-offset tracking. */
    size_t opos = 0, mi = 0;
    uint32_t match_idx = 0;
    uint32_t last_off1 = LZ2_REP_NONE;
    for (uint32_t i = 0; i < ntok; i++) {
        uint32_t t = tokens[i];
        if (t < 256) {
            if (opos >= orig_size) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            out[opos++] = (uint8_t)t;
        } else {
            if (match_idx >= nmatch) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            uint32_t len = (t - 256) + QTC_LZ_MIN_MATCH;
            uint32_t off1;
            if (rep_bits[match_idx >> 3] & (1u << (match_idx & 7))) {
                if (last_off1 == LZ2_REP_NONE) { rc = QTC_ERR_CORRUPT; goto cleanup; }
                off1 = last_off1;
            } else {
                if (mi >= nnonrep) { rc = QTC_ERR_CORRUPT; goto cleanup; }
                uint32_t bucket = buckets[mi++];
                if (bucket >= BUCKET_ALPHA) { rc = QTC_ERR_CORRUPT; goto cleanup; }
                if (bucket == 0) {
                    off1 = 0;
                } else {
                    uint32_t extra_bits = bucket - 1;
                    uint32_t extra;
                    if (extra_bits > 19) { rc = QTC_ERR_CORRUPT; goto cleanup; }
                    rc = br_get(&br, extra_bits, &extra);
                    if (rc != QTC_OK) goto cleanup;
                    off1 = (1u << (bucket - 1)) + extra;
                }
                last_off1 = off1;
            }
            match_idx++;
            uint32_t off = off1 + 1;
            if (off > opos || opos + len > orig_size) { rc = QTC_ERR_CORRUPT; goto cleanup; }
            for (uint32_t k = 0; k < len; k++)
                out[opos + k] = out[opos - off + k];
            opos += len;
        }
    }
    if (opos != orig_size || match_idx != nmatch || mi != nnonrep) {
        rc = QTC_ERR_CORRUPT;
        goto cleanup;
    }

    *consumed = (size_t)(r - payload);
    rc = QTC_OK;

cleanup:
    free(tokens);
    free(buckets);
    return rc;
}

/* ── public entry points ─────────────────────────────────────────────── */

#if QTC_HAVE_PTHREADS
/* Worker state for parallel block encoding. Each worker owns a contiguous
 * range of block indices and writes only its own slots in blocks/lens/rcs
 * — no shared mutable state, so no locks are needed and the result is
 * identical to the serial loop. block_vers non-NULL = LZ2 stream. */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         b0;          /* first block index (inclusive) */
    size_t         b1;          /* last block index (exclusive) */
    uint8_t      **blocks;
    size_t        *block_lens;
    uint8_t       *block_vers;  /* NULL for v1 streams */
    int           *rcs;
} lz_worker_t;

static void *lz_encode_worker(void *arg)
{
    lz_worker_t *wk = (lz_worker_t *)arg;
    for (size_t b = wk->b0; b < wk->b1; b++) {
        size_t off = b * QTC_LZ_BLOCK;
        size_t blen = wk->len - off;
        if (blen > QTC_LZ_BLOCK)
            blen = QTC_LZ_BLOCK;
        if (wk->block_vers)
            wk->rcs[b] = lz_encode_block_best(wk->data + off, blen,
                                              &wk->blocks[b],
                                              &wk->block_lens[b],
                                              &wk->block_vers[b]);
        else
            wk->rcs[b] = lz_encode_block(wk->data + off, blen,
                                         &wk->blocks[b], &wk->block_lens[b]);
    }
    return NULL;
}
#endif

static int lz_encode_common(const uint8_t *data, size_t len,
                            uint8_t **out, size_t *out_len, int v2)
{
    size_t nblocks = (len + QTC_LZ_BLOCK - 1) / QTC_LZ_BLOCK;
    if (nblocks == 0)
        nblocks = 1;
    /* Block count framing (v0.16.0): ≤255 blocks serialize as ONE byte,
     * exactly as before. Beyond that, byte 0 is the sentinel 0 — never a
     * valid legacy count, the encoder always emits ≥1 — followed by a
     * u32 LE count. This lifts the old 255 MB (255-block) stream limit.
     * Old decoders reject extended streams as corrupt: loud failure. */
    if (nblocks > UINT32_MAX)
        return QTC_ERR_SIZE;
    int extended = (nblocks > 255);

    uint8_t **blocks = (uint8_t **)calloc(nblocks, sizeof(uint8_t *));
    size_t *block_lens = (size_t *)calloc(nblocks, sizeof(size_t));
    uint8_t *block_vers = v2 ? (uint8_t *)calloc(nblocks, 1) : NULL;
    int *rcs = (int *)calloc(nblocks, sizeof(int));
    if (!blocks || !block_lens || !rcs || (v2 && !block_vers)) {
        free(blocks); free(block_lens); free(block_vers); free(rcs);
        return QTC_ERR_ALLOC;
    }

    int rc = QTC_OK;
    int encoded = 0;

#if QTC_HAVE_PTHREADS
    /* Encode blocks in parallel when there is more than one. Thread count
     * is capped: beyond a handful of workers the gains flatten out while
     * the per-thread stack/alloc cost keeps growing. */
    if (nblocks >= 2) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        size_t nthreads = (ncpu > 0) ? (size_t)ncpu : 1;
        if (nthreads > nblocks)
            nthreads = nblocks;
        if (nthreads > 8)
            nthreads = 8;

        pthread_t *tids = (pthread_t *)calloc(nthreads, sizeof(pthread_t));
        lz_worker_t *wks =
            (lz_worker_t *)calloc(nthreads, sizeof(lz_worker_t));
        size_t launched = 0;
        if (tids && wks) {
            size_t per = (nblocks + nthreads - 1) / nthreads;
            for (size_t t = 0; t < nthreads; t++) {
                size_t b0 = t * per;
                size_t b1 = b0 + per;
                if (b0 >= nblocks)
                    break;
                if (b1 > nblocks)
                    b1 = nblocks;
                wks[t].data = data;
                wks[t].len = len;
                wks[t].b0 = b0;
                wks[t].b1 = b1;
                wks[t].blocks = blocks;
                wks[t].block_lens = block_lens;
                wks[t].block_vers = block_vers;
                wks[t].rcs = rcs;
                if (pthread_create(&tids[t], NULL, lz_encode_worker,
                                   &wks[t]) == 0)
                    launched++;
                else
                    break;
            }
        }
        if (launched > 0) {
            for (size_t t = 0; t < launched; t++)
                pthread_join(tids[t], NULL);
            encoded = 1;
            /* Any block the launched workers did not cover (a late
             * pthread_create failure) is encoded serially below. */
        }
        free(tids);
        free(wks);
    }
#endif

    if (!encoded) {
        for (size_t b = 0; b < nblocks; b++) {
            size_t off = b * QTC_LZ_BLOCK;
            size_t blen = len - off;
            if (blen > QTC_LZ_BLOCK)
                blen = QTC_LZ_BLOCK;
            if (v2)
                rcs[b] = lz_encode_block_best(data + off, blen,
                                              &blocks[b], &block_lens[b],
                                              &block_vers[b]);
            else
                rcs[b] = lz_encode_block(data + off, blen,
                                         &blocks[b], &block_lens[b]);
        }
    } else {
#if QTC_HAVE_PTHREADS
        /* Finish any blocks left over after a partial thread launch. */
        for (size_t b = 0; b < nblocks; b++) {
            if (blocks[b] == NULL && rcs[b] == 0) {
                size_t off = b * QTC_LZ_BLOCK;
                size_t blen = len - off;
                if (blen > QTC_LZ_BLOCK)
                    blen = QTC_LZ_BLOCK;
                if (v2)
                    rcs[b] = lz_encode_block_best(data + off, blen,
                                                  &blocks[b], &block_lens[b],
                                                  &block_vers[b]);
                else
                    rcs[b] = lz_encode_block(data + off, blen,
                                             &blocks[b], &block_lens[b]);
            }
        }
#endif
    }

    size_t total = extended ? 5 : 1; /* sentinel 0 + u32 count, or u8 count */
    for (size_t b = 0; b < nblocks; b++) {
        if (rcs[b] != QTC_OK) {
            rc = rcs[b];
            goto cleanup;
        }
        /* u32 len + payload (+ u8 version tag for LZ2) */
        total += 4 + block_lens[b] + (v2 ? 1 : 0);
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { rc = QTC_ERR_ALLOC; goto cleanup; }
    uint8_t *w = buf;
    if (extended) {
        uint32_t nb = (uint32_t)nblocks;
        *w++ = 0; /* sentinel: extended u32 count follows */
        w[0]=(uint8_t)nb; w[1]=(uint8_t)(nb>>8);
        w[2]=(uint8_t)(nb>>16); w[3]=(uint8_t)(nb>>24); w+=4;
    } else {
        *w++ = (uint8_t)nblocks;
    }
    for (size_t b = 0; b < nblocks; b++) {
        uint32_t bl = (uint32_t)block_lens[b];
        w[0]=(uint8_t)bl; w[1]=(uint8_t)(bl>>8);
        w[2]=(uint8_t)(bl>>16); w[3]=(uint8_t)(bl>>24); w+=4;
        if (v2)
            *w++ = block_vers[b];
        memcpy(w, blocks[b], block_lens[b]);
        w += block_lens[b];
    }
    *out = buf;
    *out_len = total;

cleanup:
    for (size_t b = 0; b < nblocks; b++)
        free(blocks[b]);
    free(blocks);
    free(block_lens);
    free(block_vers);
    free(rcs);
    return rc;
}

int qtc_lz_encode(const uint8_t *data, size_t len,
                  uint8_t **out, size_t *out_len)
{
    return lz_encode_common(data, len, out, out_len, 0);
}

int qtc_lz2_encode(const uint8_t *data, size_t len,
                   uint8_t **out, size_t *out_len)
{
    return lz_encode_common(data, len, out, out_len, 1);
}

/* ── parallel block decode ───────────────────────────────────────────── */

/* One block located by the sequential framing pre-pass. Blocks are fully
 * independent: block b reads only its own payload slice and writes only
 * its own [out, out + borig) region (the pre-pass bound-checks borig so
 * regions never overlap), so blocks decode in any order on any threads
 * and reassemble to the identical result. */
typedef struct {
    const uint8_t *in;
    uint8_t       *out;
    uint32_t       bl;
    uint8_t        ver;
    uint8_t        done;
    int            rc;
    size_t         consumed;
} lz_dblock_t;

static void lz_decode_one(lz_dblock_t *db, int v2)
{
    db->rc = (v2 && db->ver == 1)
                 ? lz_decode_block_v2(db->in, db->bl, db->out, &db->consumed)
                 : lz_decode_block(db->in, db->bl, db->out, &db->consumed);
    db->done = 1;
}

#if QTC_HAVE_PTHREADS
/* Worker state for parallel block decoding. Each worker owns a contiguous
 * range of block indices and writes only its own slots — no shared mutable
 * state, so no locks are needed and the result is identical to the serial
 * loop. */
typedef struct {
    lz_dblock_t *blocks;
    size_t       b0;          /* first block index (inclusive) */
    size_t       b1;          /* last block index (exclusive) */
    int          v2;
} lz_dworker_t;

static void *lz_decode_worker(void *arg)
{
    lz_dworker_t *wk = (lz_dworker_t *)arg;
    for (size_t b = wk->b0; b < wk->b1; b++)
        lz_decode_one(&wk->blocks[b], wk->v2);
    return NULL;
}
#endif

static int lz_decode_common(const uint8_t *payload, size_t payload_len,
                            size_t orig_size, uint8_t *out, size_t *consumed,
                            int v2)
{
    if (payload_len < 1)
        return QTC_ERR_CORRUPT;
    uint32_t nblocks = payload[0];
    const uint8_t *r = payload + 1;
    size_t remaining = payload_len - 1;
    if (nblocks == 0) {
        /* Extended count (v0.16.0): sentinel 0 + u32 LE block count. */
        if (remaining < 4)
            return QTC_ERR_CORRUPT;
        nblocks = (uint32_t)r[0] | ((uint32_t)r[1]<<8) |
                  ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
        r += 4; remaining -= 4;
    }
    /* The count is fully determined by orig_size (1 MB blocks, last one
     * partial); reject a mismatch up front so a crafted extended count
     * fails early instead of driving the block loop. */
    {
        uint64_t expect = ((uint64_t)orig_size + QTC_LZ_BLOCK - 1) /
                          QTC_LZ_BLOCK;
        if (expect == 0)
            expect = 1;
        if ((uint64_t)nblocks != expect)
            return QTC_ERR_CORRUPT;
    }

    /* Sequential pre-pass: validate all framing and locate each block's
     * payload and destination. These are the same checks the serial
     * decoder ran inline before touching a block; doing them up front
     * keeps the parallel phase to pure decoding. */
    lz_dblock_t *blocks =
        (lz_dblock_t *)calloc(nblocks, sizeof(lz_dblock_t));
    if (!blocks)
        return QTC_ERR_ALLOC;

    size_t opos = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        /* u32 len, plus a u8 version tag in v2 streams */
        size_t hdr = v2 ? 5 : 4;
        if (remaining < hdr) {
            free(blocks);
            return QTC_ERR_CORRUPT;
        }
        uint32_t bl = (uint32_t)r[0] | ((uint32_t)r[1]<<8) |
                      ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
        r += 4; remaining -= 4;
        uint8_t ver = 0;
        if (v2) {
            ver = *r++;
            remaining -= 1;
            if (ver > 1) {
                free(blocks);
                return QTC_ERR_CORRUPT;
            }
        }
        if (bl < 4 || bl > remaining) {
            free(blocks);
            return QTC_ERR_CORRUPT;
        }

        /* Bound-check BEFORE decoding (audit H1): the block replays up to
         * its own declared orig_size into out + opos, so a block claiming
         * more than the caller's remaining space must be rejected up front.
         * This also guarantees the per-block output regions are disjoint. */
        uint32_t borig = (uint32_t)r[0] | ((uint32_t)r[1]<<8) |
                         ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
        if ((uint64_t)opos + borig > (uint64_t)orig_size) {
            free(blocks);
            return QTC_ERR_CORRUPT;
        }

        blocks[b].in = r;
        blocks[b].bl = bl;
        blocks[b].out = out + opos;
        blocks[b].ver = ver;

        opos += borig;
        r += bl;
        remaining -= bl;
    }

    /* Exact-fit contract (audit L3): trailing bytes after the last block
     * mean the payload is not the one the encoder produced. */
    if (opos != orig_size || remaining != 0) {
        free(blocks);
        return QTC_ERR_CORRUPT;
    }

    int rc = QTC_OK;
    int decoded = 0;

#if QTC_HAVE_PTHREADS
    /* Decode blocks in parallel when there is more than one. Thread count
     * is capped exactly as in encode: beyond a handful of workers the
     * gains flatten out while per-thread cost keeps growing. */
    if (nblocks >= 2) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        size_t nthreads = (ncpu > 0) ? (size_t)ncpu : 1;
        if (nthreads > nblocks)
            nthreads = nblocks;
        if (nthreads > 8)
            nthreads = 8;

        pthread_t *tids = (pthread_t *)calloc(nthreads, sizeof(pthread_t));
        lz_dworker_t *wks =
            (lz_dworker_t *)calloc(nthreads, sizeof(lz_dworker_t));
        size_t launched = 0;
        if (tids && wks) {
            size_t per = (nblocks + nthreads - 1) / nthreads;
            for (size_t t = 0; t < nthreads; t++) {
                size_t b0 = t * per;
                size_t b1 = b0 + per;
                if (b0 >= nblocks)
                    break;
                if (b1 > nblocks)
                    b1 = nblocks;
                wks[t].blocks = blocks;
                wks[t].b0 = b0;
                wks[t].b1 = b1;
                wks[t].v2 = v2;
                if (pthread_create(&tids[t], NULL, lz_decode_worker,
                                   &wks[t]) == 0)
                    launched++;
                else
                    break;
            }
        }
        if (launched > 0) {
            for (size_t t = 0; t < launched; t++)
                pthread_join(tids[t], NULL);
            decoded = 1;
            /* Any block the launched workers did not cover (a late
             * pthread_create failure) is decoded serially below. */
        }
        free(tids);
        free(wks);
    }
#endif

    if (!decoded) {
        for (uint32_t b = 0; b < nblocks; b++)
            lz_decode_one(&blocks[b], v2);
    }
#if QTC_HAVE_PTHREADS
    else {
        /* Finish any blocks left over after a partial thread launch. */
        for (uint32_t b = 0; b < nblocks; b++)
            if (!blocks[b].done)
                lz_decode_one(&blocks[b], v2);
    }
#endif

    /* Report errors in block order so a corrupt stream fails with the
     * same code the serial decoder would have returned. */
    for (uint32_t b = 0; b < nblocks; b++) {
        if (blocks[b].rc != QTC_OK) {
            rc = blocks[b].rc;
            break;
        }
        if (blocks[b].consumed != blocks[b].bl) {
            rc = QTC_ERR_CORRUPT;
            break;
        }
    }

    free(blocks);
    if (rc != QTC_OK)
        return rc;

    *consumed = (size_t)(r - payload);
    return QTC_OK;
}

int qtc_lz_decode(const uint8_t *payload, size_t payload_len,
                  size_t orig_size, uint8_t *out, size_t *consumed)
{
    return lz_decode_common(payload, payload_len, orig_size, out, consumed,
                            0);
}

int qtc_lz2_decode(const uint8_t *payload, size_t payload_len,
                   size_t orig_size, uint8_t *out, size_t *consumed)
{
    return lz_decode_common(payload, payload_len, orig_size, out, consumed,
                            1);
}
