/*
 * qtc_order1.c — Phase 2: static order-1 context model over the rANS core.
 *
 * Why static, not adaptive: rANS encodes symbols in REVERSE order, so a
 * forward-adaptive model (one that updates as it encodes/decodes) cannot
 * stay synchronized between encoder and decoder. A model counted over the
 * whole input, however, is identical on both sides by construction — the
 * encoder walking backwards and the decoder walking forwards index the
 * exact same per-context tables. (This is the same reason zstd/FSE use
 * static per-block tables.)
 *
 * Context rule: the context of symbol i is the previous byte, with 0 as
 * the context of the first symbol.
 *
 * Robustness: a context table exists only for contexts that actually
 * occurred with a successor in the training data. A valid decode can only
 * ever query such contexts (it reproduces the input exactly); querying an
 * absent context therefore proves corruption and returns QTC_ERR_CORRUPT.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

/* Renormalization threshold, same semantics as qtc_ans.c. */
#define RENORM_SHIFT_O1 (31u - QTC_MODEL_M_BITS)

int qtc_model1_build(qtc_model1_t *m, const uint8_t *data, size_t len)
{
    /* counts[ctx][sym] — 256*256 u64 = 512 KB; heap-allocated. */
    uint64_t(*counts)[QTC_ALPHABET] =
        (uint64_t(*)[QTC_ALPHABET])calloc(QTC_ALPHABET, sizeof(*counts));
    if (!counts)
        return QTC_ERR_ALLOC;

    memset(m, 0, sizeof(*m));

    if (len > 0) {
        /* First symbol: context 0. */
        counts[0][data[0]]++;
        m->present[0] = 1;

        for (size_t i = 1; i < len; i++) {
            uint8_t ctx = data[i - 1];
            counts[ctx][data[i]]++;
            m->present[ctx] = 1;
        }
    }

    for (int c = 0; c < QTC_ALPHABET; c++) {
        if (!m->present[c])
            continue;
        qtc_model_normalize_row(m->ctx[c].freq, counts[c]);
        qtc_model_derive_starts(&m->ctx[c]);
    }

    free(counts);
    return QTC_OK;
}

size_t qtc_model1_pack(const qtc_model1_t *m, uint8_t *out)
{
    uint16_t present_count = 0;
    for (int c = 0; c < QTC_ALPHABET; c++)
        present_count += m->present[c] ? 1 : 0;

    out[0] = (uint8_t)(present_count & 0xFF);
    out[1] = (uint8_t)(present_count >> 8);
    size_t off = 2;

    for (int c = 0; c < QTC_ALPHABET; c++) {
        if (!m->present[c])
            continue;
        out[off++] = (uint8_t)c;
        qtc_model_pack(&m->ctx[c], out + off);
        off += 512;
    }
    return off;
}

int qtc_model1_unpack(qtc_model1_t *m, const uint8_t *in, size_t in_len,
                      size_t *consumed)
{
    if (in_len < 2)
        return QTC_ERR_CORRUPT;

    uint32_t present_count = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
    if (present_count > QTC_ALPHABET)
        return QTC_ERR_CORRUPT;

    size_t need = 2 + (size_t)present_count * (1 + 512);
    if (in_len < need)
        return QTC_ERR_CORRUPT;

    memset(m, 0, sizeof(*m));

    size_t off = 2;
    for (uint32_t i = 0; i < present_count; i++) {
        uint32_t c = in[off++];
        if (m->present[c])
            return QTC_ERR_CORRUPT; /* duplicate context in a crafted stream */
        int rc = qtc_model_unpack(&m->ctx[c], in + off);
        if (rc != QTC_OK)
            return rc;
        off += 512;
        m->present[c] = 1;
    }

    *consumed = off;
    return QTC_OK;
}

int qtc_ans_encode_o1(const uint8_t *data, size_t len, const qtc_model1_t *m,
                      uint8_t **out, size_t *out_len)
{
    size_t cap = len * 2 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return QTC_ERR_ALLOC;

    uint8_t *p = buf + cap;
    uint32_t x = QTC_ANS_L;

    for (size_t i = len; i-- > 0;) {
        uint32_t s = data[i];
        uint32_t ctx = (i == 0) ? 0 : data[i - 1];
        const qtc_model_t *t = &m->ctx[ctx];
        uint32_t fs = t->freq[s];
        uint32_t cs = t->start[s];

        uint32_t x_max = fs << RENORM_SHIFT_O1;
        while (x >= x_max) {
            *--p = (uint8_t)(x & 0xFF);
            x >>= 8;
        }

        uint32_t rem;
        uint32_t q = qtc_div_exact(x, fs, &rem);
        x = q * QTC_MODEL_M + rem + cs;
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

int qtc_ans_decode_o1(const uint8_t *bits, size_t bits_len, size_t len,
                      const qtc_model1_t *m, uint8_t *out)
{
    if (bits_len < 4)
        return QTC_ERR_CORRUPT;

    /* Slot → symbol lookup tables for every present context (1 MB total).
     * Built once per decode; direct lookup keeps the hot loop branchless. */
    uint8_t *sym_of_slot = (uint8_t *)malloc((size_t)QTC_ALPHABET * QTC_MODEL_M);
    if (!sym_of_slot)
        return QTC_ERR_ALLOC;

    for (int c = 0; c < QTC_ALPHABET; c++) {
        if (!m->present[c])
            continue;
        uint8_t *tab = sym_of_slot + (size_t)c * QTC_MODEL_M;
        const qtc_model_t *t = &m->ctx[c];
        for (uint32_t s = 0; s < QTC_ALPHABET; s++) {
            uint32_t start = t->start[s];
            uint32_t freq = t->freq[s];
            for (uint32_t j = 0; j < freq; j++)
                tab[start + j] = (uint8_t)s;
        }
    }

    uint32_t x = (uint32_t)bits[0]
               | ((uint32_t)bits[1] << 8)
               | ((uint32_t)bits[2] << 16)
               | ((uint32_t)bits[3] << 24);

    const uint8_t *bp = bits + 4;
    const uint8_t *bend = bits + bits_len;
    uint32_t ctx = 0;
    int rc = QTC_OK;

    for (size_t i = 0; i < len; i++) {
        if (!m->present[ctx]) {
            /* A valid decode reproduces the input exactly and can only
             * reach contexts that occurred in training. Reaching an absent
             * context means the stream (or a prior decode step) is corrupt. */
            rc = QTC_ERR_CORRUPT;
            break;
        }
        const uint8_t *tab = sym_of_slot + (size_t)ctx * QTC_MODEL_M;
        const qtc_model_t *t = &m->ctx[ctx];

        uint32_t slot = x & (QTC_MODEL_M - 1);
        uint32_t s = tab[slot];
        uint32_t fs = t->freq[s];
        uint32_t cs = t->start[s];

        out[i] = (uint8_t)s;
        x = fs * (x >> QTC_MODEL_M_BITS) + slot - cs;

        while (x < QTC_ANS_L) {
            if (bp >= bend) {
                rc = QTC_ERR_CORRUPT;
                goto done;
            }
            x = (x << 8) | *bp++;
        }

        ctx = s;
    }

done:
    free(sym_of_slot);
    return rc;
}
