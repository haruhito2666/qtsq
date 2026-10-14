/*
 * qtc_model.c — order-0 statistical model.
 *
 * Counts byte frequencies and normalizes them to M = 4096 so the rANS
 * coder can work with 12-bit frequency tables. Normalization preserves
 * every symbol present in the input (freq >= 1), which means the decoder
 * never needs an escape symbol: if the encoder could encode it, the
 * decoder's table contains it.
 */
#include "qtc_internal.h"

#include <math.h>
#include <string.h>

int qtc_model_build(qtc_model_t *m, const uint8_t *data, size_t len)
{
    uint64_t counts[QTC_ALPHABET] = {0};
    uint64_t total = 0;
    int distinct = 0;

    memset(m, 0, sizeof(*m));

    for (size_t i = 0; i < len; i++) {
        counts[data[i]]++;
        total++;
    }

    if (total == 0)
        return 0;

    for (int s = 0; s < QTC_ALPHABET; s++)
        if (counts[s] > 0)
            distinct++;

    if (distinct == 1) {
        /* Degenerate single-symbol input: give it the whole table. */
        for (int s = 0; s < QTC_ALPHABET; s++) {
            if (counts[s] > 0)
                m->freq[s] = (uint16_t)QTC_MODEL_M;
        }
        qtc_model_derive_starts(m);
        return distinct;
    }

    qtc_model_normalize_row(m->freq, counts);
    qtc_model_derive_starts(m);
    return distinct;
}

void qtc_model_normalize_row(uint16_t freq[QTC_ALPHABET],
                             const uint64_t counts[QTC_ALPHABET])
{
    uint64_t total = 0;
    for (int s = 0; s < QTC_ALPHABET; s++)
        total += counts[s];

    /* Initial allocation: proportional, floor 1 for present symbols. */
    uint32_t assigned = 0;
    for (int s = 0; s < QTC_ALPHABET; s++) {
        if (counts[s] == 0) {
            freq[s] = 0;
            continue;
        }
        uint32_t f = (uint32_t)((counts[s] * QTC_MODEL_M) / total);
        if (f == 0)
            f = 1;
        freq[s] = (uint16_t)f;
        assigned += f;
    }

    /* Repair the sum to exactly M. */
    if (assigned > QTC_MODEL_M) {
        /* Over-assigned: steal from the largest donors, never below 1. */
        while (assigned > QTC_MODEL_M) {
            int best = -1;
            uint32_t best_f = 1;
            for (int s = 0; s < QTC_ALPHABET; s++) {
                if (freq[s] > best_f) {
                    best_f = freq[s];
                    best = s;
                }
            }
            if (best < 0)
                break; /* shouldn't happen: distinct >= 2 and M >= 2*1 */
            freq[best]--;
            assigned--;
        }
    } else if (assigned < QTC_MODEL_M) {
        /* Under-assigned: give the remainder to the most frequent symbol. */
        int best = 0;
        for (int s = 1; s < QTC_ALPHABET; s++)
            if (counts[s] > counts[best])
                best = s;
        freq[best] = (uint16_t)(freq[best] + (QTC_MODEL_M - assigned));
    }
}

void qtc_model_derive_starts(qtc_model_t *m)
{
    uint32_t acc = 0;
    for (int s = 0; s < QTC_ALPHABET; s++) {
        m->start[s] = (uint16_t)acc;
        acc += m->freq[s];
    }
}

void qtc_model_pack(const qtc_model_t *m, uint8_t out[512])
{
    for (int s = 0; s < QTC_ALPHABET; s++) {
        out[s * 2]     = (uint8_t)(m->freq[s] & 0xFF);
        out[s * 2 + 1] = (uint8_t)(m->freq[s] >> 8);
    }
}

int qtc_model_unpack(qtc_model_t *m, const uint8_t in[512])
{
    uint32_t sum = 0;
    memset(m, 0, sizeof(*m));
    for (int s = 0; s < QTC_ALPHABET; s++) {
        uint32_t f = (uint32_t)in[s * 2] | ((uint32_t)in[s * 2 + 1] << 8);
        m->freq[s] = (uint16_t)f;
        sum += f;
    }
    if (sum != QTC_MODEL_M)
        return QTC_ERR_CORRUPT;
    qtc_model_derive_starts(m);
    return QTC_OK;
}

double qtc_model_entropy(const qtc_model_t *m)
{
    double h = 0.0;
    for (int s = 0; s < QTC_ALPHABET; s++) {
        if (m->freq[s] == 0)
            continue;
        double p = (double)m->freq[s] / (double)QTC_MODEL_M;
        h -= p * log2(p);
    }
    return h;
}
