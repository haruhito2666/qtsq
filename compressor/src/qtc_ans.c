/*
 * qtc_ans.c — the Singularity: byte-renormalized rANS entropy coder.
 *
 * rANS (Asymmetric Numeral Systems) encodes a symbol stream into a single
 * 32-bit integer state. Encoding walks the symbols in REVERSE and emits
 * renormalization bytes into a buffer grown backwards; the final state is
 * written at the very front. Decoding reads the state, then walks the
 * symbol stream FORWARD, consuming the renormalization bytes in the same
 * order they were emitted.
 *
 * State invariant: x ∈ [L, L<<8) with L = 1<<23.
 * Model: order-0, M = 4096 (12-bit slots), every present symbol has
 * freq >= 1 so no escape handling is required.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

/* Encode renormalization threshold: before encoding symbol s we need
 * x < freq[s] << 19 so that x' = (x/freq)*M + ... stays < L<<8 = 2^31. */
#define RENORM_SHIFT (31u - QTC_MODEL_M_BITS) /* 19 */

/* ── Exact reciprocal division for the encode step ─────────────────────
 * The encode step needs x/fs and x%fs where renormalization guarantees
 * x < fs << (31 - m_bits) ≤ 2^31 and models guarantee fs ∈ [1, M ≤ 4096].
 * For an invariant divisor d with k = ceil(log2(d)) (k = 0 for d = 1) and
 * m = floor(2^(31+k)/d) + 1, floor(x/d) == (m*x) >> (31+k) for EVERY
 * x < 2^31 (division by invariant integers; m < 2^32 for d ≥ 2 and
 * m = 2^31+1 for d = 1, so the product always fits in u64). One 32×64
 * multiply + shift replaces two hardware divisions per symbol. */
typedef struct { uint64_t m; uint32_t shift; } qtc_divrec_t;
static qtc_divrec_t g_divrec[QTC_MODEL_M + 1];

/* MSVC has no __builtin_clz; _BitScanReverse gives the highest set bit, so
 * clz(v) = 31 - idx (v != 0 — d-1 >= 1 at the only call site). */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static uint32_t qtc_clz32(uint32_t v)
{
    unsigned long idx;
    _BitScanReverse(&idx, v);
    return 31u - (uint32_t)idx;
}
#define QTC_BUILTIN_CLZ(v) qtc_clz32(v)
#else
#define QTC_BUILTIN_CLZ(v) ((uint32_t)__builtin_clz(v))
#endif

static void qtc_divrec_fill(void)
{
    for (uint32_t d = 1; d <= QTC_MODEL_M; d++) {
        uint32_t k = (d <= 1) ? 0u : 32u - QTC_BUILTIN_CLZ(d - 1);
        g_divrec[d].m = (((uint64_t)1) << (31u + k)) / d + 1u;
        g_divrec[d].shift = 31u + k;
    }
}

#if defined(_MSC_VER) && !defined(__clang__)
/* MSVC lacks __attribute__((constructor)); register the initializer in the
 * CRT's .CRT$XCU section so it runs before main like a GCC/Clang ctor.
 * Safety net: qtc_div_exact() also refills lazily on first use. */
static void qtc_divrec_ctor(void)
{
    qtc_divrec_fill();
}
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (*const qtc_divrec_ctor_p)(void) = qtc_divrec_ctor;
#else
__attribute__((constructor))
static void qtc_divrec_ctor(void)
{
    qtc_divrec_fill();
}
#endif

/* Exact: returns x/fs and stores x%fs. fs must be in [1, QTC_MODEL_M] and
 * x < 2^31 — both hold at every call site by the invariants above. */
uint32_t qtc_div_exact(uint32_t x, uint32_t fs, uint32_t *rem)
{
    /* Belt-and-braces for loaders that skip ctors: identical values get
     * recomputed, so a benign race here is harmless. */
    if (!g_divrec[QTC_MODEL_M].m)
        qtc_divrec_fill();
    qtc_divrec_t r = g_divrec[fs];
    uint32_t q = (uint32_t)((r.m * (uint64_t)x) >> r.shift);
    *rem = x - q * fs;
    return q;
}

int qtc_ans_encode(const uint8_t *data, size_t len, const qtc_model_t *m,
                   uint8_t **out, size_t *out_len)
{
    /* Worst case: 2 renorm bytes per symbol + 4 state bytes. */
    size_t cap = len * 2 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return QTC_ERR_ALLOC;

    uint8_t *p = buf + cap;          /* grows backwards */
    uint32_t x = QTC_ANS_L;          /* initial state */

    for (size_t i = len; i-- > 0;) {
        uint32_t s = data[i];
        uint32_t fs = m->freq[s];
        uint32_t cs = m->start[s];

        /* Renormalize: emit low bytes until x fits the pre-encode range. */
        uint32_t x_max = fs << RENORM_SHIFT;
        while (x >= x_max) {
            *--p = (uint8_t)(x & 0xFF);
            x >>= 8;
        }

        /* rANS encode step (exact reciprocal division, see qtc_div_exact). */
        uint32_t rem;
        uint32_t q = qtc_div_exact(x, fs, &rem);
        x = q * QTC_MODEL_M + rem + cs;
    }

    /* Final state, little-endian, at the very front. The buffer grows
     * backwards, so write the most significant byte first: memory order
     * (low address → high) ends up [x&0xFF][x>>8][x>>16][x>>24], matching
     * the little-endian read in qtc_ans_decode. */
    *--p = (uint8_t)((x >> 24) & 0xFF);
    *--p = (uint8_t)((x >> 16) & 0xFF);
    *--p = (uint8_t)((x >> 8) & 0xFF);
    *--p = (uint8_t)(x & 0xFF);

    size_t used = (size_t)((buf + cap) - p);
    if (p != buf)
        memmove(buf, p, used);

    *out = buf;
    *out_len = used;
    return QTC_OK;
}

int qtc_ans_decode(const uint8_t *bits, size_t bits_len, size_t len,
                   const qtc_model_t *m, uint8_t *out)
{
    if (bits_len < 4)
        return QTC_ERR_CORRUPT;

    /* O(1) slot → symbol lookup table (tANS-style speedup). Stack-local so
     * concurrent decodes with different models stay thread-safe. */
    uint8_t sym_of_slot[QTC_MODEL_M];
    for (uint32_t s = 0; s < QTC_ALPHABET; s++) {
        uint32_t start = m->start[s];
        uint32_t freq = m->freq[s];
        for (uint32_t j = 0; j < freq; j++)
            sym_of_slot[start + j] = (uint8_t)s;
    }

    uint32_t x = (uint32_t)bits[0]
               | ((uint32_t)bits[1] << 8)
               | ((uint32_t)bits[2] << 16)
               | ((uint32_t)bits[3] << 24);

    const uint8_t *bp = bits + 4;
    const uint8_t *bend = bits + bits_len;

    for (size_t i = 0; i < len; i++) {
        uint32_t slot = x & (QTC_MODEL_M - 1);
        uint32_t s = sym_of_slot[slot];
        uint32_t fs = m->freq[s];
        uint32_t cs = m->start[s];

        out[i] = (uint8_t)s;

        /* rANS decode step. */
        x = fs * (x >> QTC_MODEL_M_BITS) + slot - cs;

        /* Renormalize: pull bytes until x is back in [L, L<<8). */
        while (x < QTC_ANS_L) {
            if (bp >= bend)
                return QTC_ERR_CORRUPT;
            x = (x << 8) | *bp++;
        }
    }

    return QTC_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * General-alphabet rANS (Phase 3)
 * ══════════════════════════════════════════════════════════════════════ */

void qtc_model_build_gen_m(uint16_t *freq, const uint64_t *counts, uint32_t A,
                           uint32_t M)
{
    uint64_t total = 0;
    for (uint32_t s = 0; s < A; s++)
        total += counts[s];

    uint32_t assigned = 0;
    for (uint32_t s = 0; s < A; s++) {
        if (counts[s] == 0) {
            freq[s] = 0;
            continue;
        }
        uint32_t f = (uint32_t)((counts[s] * M) / total);
        if (f == 0)
            f = 1;
        freq[s] = (uint16_t)f;
        assigned += f;
    }

    if (assigned > M) {
        while (assigned > M) {
            int best = -1;
            uint32_t best_f = 1;
            for (uint32_t s = 0; s < A; s++) {
                if (freq[s] > best_f) {
                    best_f = freq[s];
                    best = (int)s;
                }
            }
            if (best < 0)
                break;
            freq[best]--;
            assigned--;
        }
    } else if (assigned < M) {
        int best = 0;
        for (uint32_t s = 1; s < A; s++)
            if (counts[s] > counts[best])
                best = (int)s;
        freq[best] = (uint16_t)(freq[best] + (M - assigned));
    }
}

void qtc_model_build_gen(uint16_t *freq, const uint64_t *counts, uint32_t A)
{
    qtc_model_build_gen_m(freq, counts, A, QTC_MODEL_M);
}

void qtc_model_derive_starts_gen(uint16_t *start, const uint16_t *freq,
                                 uint32_t A)
{
    uint32_t acc = 0;
    for (uint32_t s = 0; s < A; s++) {
        start[s] = (uint16_t)acc;
        acc += freq[s];
    }
}

void qtc_model_pack_gen(const uint16_t *freq, uint32_t A, uint8_t *out)
{
    for (uint32_t s = 0; s < A; s++) {
        out[s * 2]     = (uint8_t)(freq[s] & 0xFF);
        out[s * 2 + 1] = (uint8_t)(freq[s] >> 8);
    }
}

int qtc_model_unpack_gen(uint16_t *freq, uint32_t A, const uint8_t *in)
{
    uint32_t sum = 0;
    for (uint32_t s = 0; s < A; s++) {
        uint32_t f = (uint32_t)in[s * 2] | ((uint32_t)in[s * 2 + 1] << 8);
        freq[s] = (uint16_t)f;
        sum += f;
    }
    if (sum != QTC_MODEL_M)
        return QTC_ERR_CORRUPT;
    return QTC_OK;
}

/* Validate a model that was unpacked by other means (e.g. the LZ lens's
 * bitmap-packed tables): frequencies must sum to exactly M. */
int qtc_model_unpack_freqs_m(uint16_t *freq, uint32_t A, uint32_t M)
{
    uint32_t sum = 0;
    for (uint32_t s = 0; s < A; s++)
        sum += freq[s];
    if (sum != M)
        return QTC_ERR_CORRUPT;
    return QTC_OK;
}

int qtc_model_unpack_freqs(uint16_t *freq, uint32_t A)
{
    return qtc_model_unpack_freqs_m(freq, A, QTC_MODEL_M);
}

int qtc_ans_encode_gen_m(const uint16_t *syms, size_t n, uint32_t A,
                         const uint16_t *freq, const uint16_t *start,
                         uint32_t m_bits,
                         uint8_t **out, size_t *out_len)
{
    (void)A;
    uint32_t M = 1u << m_bits;
    uint32_t renorm_shift = 31u - m_bits; /* see RENORM_SHIFT above */
    size_t cap = n * 2 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return QTC_ERR_ALLOC;

    uint8_t *p = buf + cap;
    uint32_t x = QTC_ANS_L;

    for (size_t i = n; i-- > 0;) {
        uint32_t s = syms[i];
        uint32_t fs = freq[s];
        uint32_t cs = start[s];

        uint32_t x_max = fs << renorm_shift;
        while (x >= x_max) {
            *--p = (uint8_t)(x & 0xFF);
            x >>= 8;
        }

        uint32_t rem;
        uint32_t q = qtc_div_exact(x, fs, &rem);
        x = q * M + rem + cs;
    }

    *--p = (uint8_t)((x >> 24) & 0xFF);
    *--p = (uint8_t)((x >> 16) & 0xFF);
    *--p = (uint8_t)((x >> 8) & 0xFF);
    *--p = (uint8_t)(x & 0xFF);

    size_t used = (size_t)((buf + cap) - p);
    if (p != buf)
        memmove(buf, p, used);

    *out = buf;
    *out_len = used;
    return QTC_OK;
}

int qtc_ans_encode_gen(const uint16_t *syms, size_t n, uint32_t A,
                       const uint16_t *freq, const uint16_t *start,
                       uint8_t **out, size_t *out_len)
{
    return qtc_ans_encode_gen_m(syms, n, A, freq, start, QTC_MODEL_M_BITS,
                                out, out_len);
}

int qtc_ans_decode_gen_m(const uint8_t *bits, size_t bits_len, size_t n,
                         uint32_t A, const uint16_t *freq,
                         const uint16_t *start, uint32_t m_bits,
                         uint16_t *out)
{
    if (bits_len < 4)
        return QTC_ERR_CORRUPT;

    uint32_t M = 1u << m_bits;

    /* Phase 8: the slot→symbol table is at most M = 4096 entries — every
     * caller uses m_bits ≤ QTC_MODEL_M_BITS (the LZ2 lens validates its flag
     * byte to {0, U8} before deriving m_bits; the plain gen path passes
     * QTC_MODEL_M_BITS). Stack it like the order-0 decoder instead of a
     * malloc/free per LZ block. Still stack-local, so concurrent decodes with
     * different models stay reentrant. */
    if (M > QTC_MODEL_M)
        return QTC_ERR_CORRUPT;
    uint16_t sym_of_slot[QTC_MODEL_M];

    /* Slot → symbol lookup table, built per decode. NOTE: entries are only
     * written for slots covered by [start[s], start[s]+freq[s]) — this is
     * complete EXACTLY because every caller validates the model first
     * (the freq-sum check rejects any table whose frequencies do not sum
     * to M). Reading an uninitialized slot would require bypassing that
     * validation; keep it upstream of this function. */
    for (uint32_t s = 0; s < A; s++) {
        uint32_t st = start[s];
        uint32_t fq = freq[s];
        if (st + fq > M)
            return QTC_ERR_CORRUPT;
        for (uint32_t j = 0; j < fq; j++)
            sym_of_slot[st + j] = (uint16_t)s;
    }

    uint32_t x = (uint32_t)bits[0]
               | ((uint32_t)bits[1] << 8)
               | ((uint32_t)bits[2] << 16)
               | ((uint32_t)bits[3] << 24);

    const uint8_t *bp = bits + 4;
    const uint8_t *bend = bits + bits_len;
    int rc = QTC_OK;

    for (size_t i = 0; i < n; i++) {
        uint32_t slot = x & (M - 1);
        uint32_t s = sym_of_slot[slot];
        uint32_t fs = freq[s];
        uint32_t cs = start[s];

        out[i] = (uint16_t)s;
        x = fs * (x >> m_bits) + slot - cs;

        while (x < QTC_ANS_L) {
            if (bp >= bend) {
                rc = QTC_ERR_CORRUPT;
                goto done;
            }
            x = (x << 8) | *bp++;
        }
    }

done:
    return rc;
}

int qtc_ans_decode_gen(const uint8_t *bits, size_t bits_len, size_t n,
                       uint32_t A, const uint16_t *freq, const uint16_t *start,
                       uint16_t *out)
{
    return qtc_ans_decode_gen_m(bits, bits_len, n, A, freq, start,
                                QTC_MODEL_M_BITS, out);
}
