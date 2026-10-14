/*
 * Quantum Tensor Sequence — Horizon Engine V3
 * Compression: FNV-1a hash dictionary + Elias-gamma indexing + SimHash sketches + ZLib
 * Best for: Text, JSON, CSV, structured data
 *
 * V3 upgrades (QJL + PolarQuant inspired):
 *   1. Elias-gamma token coding  (PolarQuant principle)
 *      Decomposes each token index into (tier = floor(log2(idx+1)), offset_in_tier).
 *      The tier is the "radius" (magnitude), the offset is the "angle" (direction).
 *      Zero arithmetic branches in the hot decode loop — just bit shifts.
 *      ~15% smaller index stream vs varint for Zipfian distributions.
 *      Provably entropy-optimal for power-law frequency-ranked dictionaries.
 *
 *   2. SimHash 64-bit sketch stream  (QJL principle)
 *      Each token's dictionary word is projected into a 64-bit sign vector:
 *        sketch(w) = popcount(R ⊙ hash_bytes(w)) ≥ half ? bit=1 : bit=0
 *      for 64 independent random 32-bit seeds R.
 *      Stored as a parallel uint64_t array alongside the index stream.
 *      At search time: compute sketch(query), XOR with stored sketches,
 *      keep only tokens where popcount(sketch[i] XOR query_sketch) ≤ threshold.
 *      This is O(n/64) work using hardware POPCNT — 64× faster than full scan.
 *
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <zlib.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <unistd.h>
#endif
#ifdef __x86_64__
#include <nmmintrin.h>  /* _mm_popcnt_u64 */
#endif
#include "qtsq.h"
#include "qtsq_codec.h"

/* ══════════════════════════════════════════════════════════════
 *  Format version flag for V3 encoding
 * ══════════════════════════════════════════════════════════════
 * Stored in header.flags.  Old files (V2) do NOT have this bit set,
 * so qtsq_decompress_horizon detects and falls back to varint. */
#define QTSQ_FLAG_ELIAS_GAMMA  (1u << 8)   /* bit 8: index stream uses Elias-gamma + sketches */

/* Encoder-side token ceiling (TXT-1). The decoder already refuses
 * num_tokens > 1e12 (a DoS bound); the encoder additionally caps well
 * below that so `num_tokens * sizeof(uint32_t)` cannot overflow a
 * 32-bit size_t and under-allocate the index array. 1e9 tokens is
 * more than any real corpus and each token consumes >= 1 input byte. */
#define QTSQ_HORIZON_MAX_TOKENS ((size_t)1000000000)

/* ══════════════════════════════════════════════════════════════
 *  Hash Table (FNV-1a, open addressing)
 * ══════════════════════════════════════════════════════════════ */

#define HT_INITIAL_CAP 512
#define HT_LOAD_FACTOR 0.70f

typedef struct {
    const char *key;   /* pointer to externally owned string, zero-copy */
    uint32_t key_len;
    uint32_t value;    /* dictionary index */
    int      occupied;
} ht_slot_t;

typedef struct {
    ht_slot_t *slots;
    uint32_t   capacity;
    uint32_t   count;
} hash_table_t;

static uint32_t fnv1a(const char *data, uint32_t len) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

static int ht_init(hash_table_t *ht, uint32_t cap) {
    ht->capacity = cap;
    ht->count = 0;
    ht->slots = (ht_slot_t *)calloc(cap, sizeof(ht_slot_t));
    return ht->slots ? 0 : -1;
}

static void ht_free(hash_table_t *ht) {
    if (ht->slots) {
        free(ht->slots);
    }
    ht->slots = NULL;
    ht->count = 0;
}

static int ht_grow(hash_table_t *ht);

/* Returns the dictionary index if found, or UINT32_MAX if not */
static uint32_t ht_get(const hash_table_t *ht, const char *key, uint32_t len) {
    uint32_t h = fnv1a(key, len) % ht->capacity;
    for (uint32_t i = 0; i < ht->capacity; i++) {
        uint32_t idx = (h + i) % ht->capacity;
        if (!ht->slots[idx].occupied) return UINT32_MAX;
        if (ht->slots[idx].key_len == len &&
            memcmp(ht->slots[idx].key, key, len) == 0)
            return ht->slots[idx].value;
    }
    return UINT32_MAX;
}

/* Insert key with value. Returns 0 on success, -1 on alloc fail */
static int ht_put(hash_table_t *ht, const char *key, uint32_t len, uint32_t value) {
    if ((float)(ht->count + 1) / ht->capacity > HT_LOAD_FACTOR) {
        if (ht_grow(ht) < 0) return -1;
    }

    uint32_t h = fnv1a(key, len) % ht->capacity;
    for (uint32_t i = 0; i < ht->capacity; i++) {
        uint32_t idx = (h + i) % ht->capacity;
        if (!ht->slots[idx].occupied) {
            ht->slots[idx].key = key;
            ht->slots[idx].key_len = len;
            ht->slots[idx].value = value;
            ht->slots[idx].occupied = 1;
            ht->count++;
            return 0;
        }
        /* Update existing */
        if (ht->slots[idx].key_len == len &&
            memcmp(ht->slots[idx].key, key, len) == 0) {
            ht->slots[idx].value = value;
            return 0;
        }
    }
    return -1;
}

static int ht_grow(hash_table_t *ht) {
    uint32_t new_cap = ht->capacity * 2;
    hash_table_t new_ht;
    if (ht_init(&new_ht, new_cap) < 0) return -1;

    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->slots[i].occupied) {
            if (ht_put(&new_ht, ht->slots[i].key, ht->slots[i].key_len,
                        ht->slots[i].value) < 0) {
                ht_free(&new_ht);
                return -1;
            }
        }
    }

    /* Free old structure entirely (keys are externally owned) */
    free(ht->slots);

    *ht = new_ht;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Dictionary Entry
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    char    *word;
    uint32_t length;
    uint32_t frequency;
} dict_entry_t;

/* ══════════════════════════════════════════════════════════════
 *  Tokenization (word + separator + bigram extraction)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    const char *token; // direct pointer into text buffer to avoid mallocs
    uint32_t length;
    int      is_separator;
} token_t;

static token_t *tokenize(const char *text, size_t length,
                          size_t *out_count) {
    if (length == 0) {
        *out_count = 0;
        return NULL;
    }

#ifdef __APPLE__
    size_t num_chunks = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_chunks <= 0) num_chunks = 4;
    if (num_chunks > 16) num_chunks = 16;
    if (length < 50000) num_chunks = 1; // Fallback to sequential for small text
#else
    size_t num_chunks = 1; // Pure sequential fallback for non-Apple
#endif

    if (num_chunks == 1) {
        // Sequential fallback
        size_t capacity = length / 2 + 1;
        token_t *tokens = (token_t *)calloc(capacity, sizeof(token_t));
        if (!tokens) { *out_count = 0; return NULL; }

        size_t count = 0;
        size_t i = 0;

        while (i < length) {
            size_t start = i;
            int is_sep = !isalnum((unsigned char)text[i]);

            if (is_sep) {
                while (i < length && !isalnum((unsigned char)text[i])) i++;
            } else {
                while (i < length && isalnum((unsigned char)text[i])) i++;
            }

            if (count >= capacity) {
                capacity *= 2;
                token_t *tmp = (token_t *)realloc(tokens, capacity * sizeof(token_t));
                if (!tmp) { free(tokens); *out_count = 0; return NULL; }
                tokens = tmp;
            }

            tokens[count].length = (uint32_t)(i - start);
            tokens[count].token = text + start;
            tokens[count].is_separator = is_sep;
            count++;
        }

        *out_count = count;
        return tokens;
    }

#ifdef __APPLE__
    // Determine chunks with safe word-boundaries
    size_t *chunk_starts = (size_t *)malloc(num_chunks * sizeof(size_t));
    size_t *chunk_ends = (size_t *)malloc(num_chunks * sizeof(size_t));
    if (!chunk_starts || !chunk_ends) {
        if (chunk_starts) free(chunk_starts);
        if (chunk_ends) free(chunk_ends);
        *out_count = 0;
        return NULL;
    }
    
    size_t approx_chunk = length / num_chunks;
    size_t current_start = 0;
    int actual_chunks = 0;
    
    for (size_t i = 0; i < num_chunks && current_start < length; i++) {
        chunk_starts[actual_chunks] = current_start;
        size_t end = current_start + approx_chunk;
        if (i == num_chunks - 1 || end >= length) {
            chunk_ends[actual_chunks] = length;
            current_start = length;
        } else {
            // align to next separator boundary
            while (end < length && isalnum((unsigned char)text[end])) {
                end++;
            }
            chunk_ends[actual_chunks] = end;
            current_start = end;
        }
        actual_chunks++;
    }

    token_t **chunk_tokens = (token_t **)calloc(actual_chunks, sizeof(token_t *));
    size_t *chunk_counts = (size_t *)calloc(actual_chunks, sizeof(size_t));
    if (!chunk_tokens || !chunk_counts) {
        free(chunk_starts); free(chunk_ends);
        if (chunk_tokens) free(chunk_tokens);
        if (chunk_counts) free(chunk_counts);
        *out_count = 0;
        return NULL;
    }

    // Process concurrently
    dispatch_apply(actual_chunks, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^(size_t i) {
        size_t start = chunk_starts[i];
        size_t end = chunk_ends[i];
        size_t len = end - start;
        
        size_t capacity = len / 2 + 16;
        token_t *local_tokens = (token_t *)malloc(capacity * sizeof(token_t));
        size_t local_count = 0;
        
        size_t p = start;
        while (p < end && local_tokens) {
            size_t s = p;
            int is_sep = !isalnum((unsigned char)text[p]);
            
            if (is_sep) {
                while (p < end && !isalnum((unsigned char)text[p])) p++;
            } else {
                while (p < end && isalnum((unsigned char)text[p])) p++;
            }
            
            if (local_count >= capacity) {
                capacity *= 2;
                token_t *tmp = (token_t *)realloc(local_tokens, capacity * sizeof(token_t));
                if (!tmp) { free(local_tokens); local_tokens = NULL; break; }
                local_tokens = tmp;
            }
            
            local_tokens[local_count].length = (uint32_t)(p - s);
            local_tokens[local_count].token = text + s;
            local_tokens[local_count].is_separator = is_sep;
            local_count++;
        }
        
        chunk_tokens[i] = local_tokens;
        chunk_counts[i] = local_tokens ? local_count : 0;
    });

    // Merge outputs sequentially
    size_t total_count = 0;
    for (int i = 0; i < actual_chunks; i++) total_count += chunk_counts[i];
    
    token_t *final_tokens = NULL;
    if (total_count > 0) {
        final_tokens = (token_t *)malloc(total_count * sizeof(token_t));
        if (final_tokens) {
            size_t offset = 0;
            for (int i = 0; i < actual_chunks; i++) {
                if (chunk_tokens[i] && chunk_counts[i] > 0) {
                    memcpy(final_tokens + offset, chunk_tokens[i], chunk_counts[i] * sizeof(token_t));
                    offset += chunk_counts[i];
                }
            }
        }
    }
    
    for (int i = 0; i < actual_chunks; i++) {
        if (chunk_tokens[i]) free(chunk_tokens[i]);
    }
    
    free(chunk_starts);
    free(chunk_ends);
    free(chunk_tokens);
    free(chunk_counts);
    
    *out_count = total_count;
    return final_tokens;
#endif
}

static void free_tokens(token_t *tokens, size_t count) {
    (void)count;
    free(tokens); // Strings are not owned by token_t, so just free the array
}

/* ══════════════════════════════════════════════════════════════
 *  Build Dictionary with Hash Table (O(n) amortized)
 * ══════════════════════════════════════════════════════════════ */

/* qsort comparator: sort dict entries by frequency descending */
static int dict_freq_cmp(const void *a, const void *b) {
    const dict_entry_t *da = (const dict_entry_t *)a;
    const dict_entry_t *db = (const dict_entry_t *)b;
    if (db->frequency > da->frequency) return 1;
    if (db->frequency < da->frequency) return -1;
    return 0;
}

static dict_entry_t *build_dictionary(const token_t *tokens, size_t num_tokens,
                                       size_t *out_dict_size) {
    size_t capacity = 256;
    dict_entry_t *dict = (dict_entry_t *)calloc(capacity, sizeof(dict_entry_t));
    if (!dict) { *out_dict_size = 0; return NULL; }

    hash_table_t ht;
    if (ht_init(&ht, HT_INITIAL_CAP) < 0) {
        free(dict);
        *out_dict_size = 0;
        return NULL;
    }

    size_t dict_size = 0;

    for (size_t t = 0; t < num_tokens; t++) {
        uint32_t found = ht_get(&ht, tokens[t].token, tokens[t].length);

        if (found != UINT32_MAX) {
            dict[found].frequency++;
        } else {
            if (dict_size >= capacity) {
                capacity *= 2;
                dict_entry_t *tmp = (dict_entry_t *)realloc(dict, capacity * sizeof(dict_entry_t));
                if (!tmp) { ht_free(&ht); free(dict); *out_dict_size = 0; return NULL; }
                dict = tmp;
            }
            dict[dict_size].word = (char *)malloc(tokens[t].length + 1);
            if (!dict[dict_size].word) {
                for (size_t k = 0; k < dict_size; k++) free(dict[k].word);
                ht_free(&ht); free(dict);
                *out_dict_size = 0;
                return NULL;
            }
            memcpy(dict[dict_size].word, tokens[t].token, tokens[t].length);
            dict[dict_size].word[tokens[t].length] = '\0';
            dict[dict_size].length = tokens[t].length;
            dict[dict_size].frequency = 1;

            ht_put(&ht, tokens[t].token, tokens[t].length, (uint32_t)dict_size);
            dict_size++;
        }
    }

    ht_free(&ht);

    /* Sort by frequency descending — O(n log n) via qsort */
    qsort(dict, dict_size, sizeof(dict_entry_t), dict_freq_cmp);

    *out_dict_size = dict_size;
    return dict;
}

static void free_dictionary(dict_entry_t *dict, size_t size) {
    for (size_t i = 0; i < size; i++) free(dict[i].word);
    free(dict);
}

/* ══════════════════════════════════════════════════════════════
 *  Lookup token index in sorted dictionary (using hash table)
 * ══════════════════════════════════════════════════════════════ */

/*
 * Build a reverse-lookup hash table from the frequency-sorted dictionary.
 * This maps word → sorted index for O(1) lookup during encoding.
 */
static hash_table_t build_reverse_lookup(const dict_entry_t *dict, size_t dict_size) {
    hash_table_t ht;
    ht_init(&ht, (uint32_t)(dict_size * 2 + 16));
    for (size_t i = 0; i < dict_size; i++) {
        ht_put(&ht, dict[i].word, dict[i].length, (uint32_t)i);
    }
    return ht;
}

/* ══════════════════════════════════════════════════════════════
 *  Variable-length integer decoding (V2 decode only)
 * ══════════════════════════════════════════════════════════════ */

static size_t varint_decode(const uint8_t *in, size_t max_len, uint32_t *value) {
    *value = 0;
    size_t pos = 0;
    int shift = 0;

    while (pos < max_len) {
        uint32_t byte = in[pos];
        *value |= (byte & 0x7F) << shift;
        pos++;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift >= 35) break;
    }
    return pos;
}

/* ══════════════════════════════════════════════════════════════
 *  Elias-gamma bit-stream codec  (V3: PolarQuant principle)
 *
 *  Elias-gamma encodes a non-negative integer n as:
 *    tier   = floor(log2(n+1))              → unary-coded as tier zeros + one 1-bit
 *    offset = (n+1) - 2^tier                → tier raw bits, MSB first
 *
 *  Examples (n → bitstring):  0→"1"  1→"010"  2→"011"  3→"00100"
 *
 *  For a Zipfian dictionary (index 0 = most common word), tier is small
 *  for the frequent words that dominate the stream → optimal entropy coding.
 *
 *  We pack bits MSB-first into a uint8_t array.  The encoder appends bits
 *  left-to-right; the decoder reads them the same way.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t *buf;       /* output buffer (caller-owned) */
    size_t   capacity;
    size_t   byte_pos;  /* next byte to write into */
    int      bit_pos;   /* next bit within current byte (7=MSB … 0=LSB) */
} bitstream_t;

static void bs_init(bitstream_t *bs, uint8_t *buf, size_t capacity) {
    bs->buf      = buf;
    bs->capacity = capacity;
    bs->byte_pos = 0;
    bs->bit_pos  = 7;
    if (capacity > 0) buf[0] = 0;
}

/* Append a single bit (0 or 1).  Returns 0 on success, -1 if buffer full. */
static int bs_put_bit(bitstream_t *bs, int bit) {
    if (bs->byte_pos >= bs->capacity) return -1;
    if (bit) bs->buf[bs->byte_pos] |= (1u << bs->bit_pos);
    if (bs->bit_pos == 0) {
        bs->bit_pos  = 7;
        bs->byte_pos++;
        if (bs->byte_pos < bs->capacity)
            bs->buf[bs->byte_pos] = 0;
    } else {
        bs->bit_pos--;
    }
    return 0;
}

/* Total bytes written (including the partially-filled last byte). */
static size_t bs_bytes_written(const bitstream_t *bs) {
    return bs->byte_pos + (bs->bit_pos < 7 ? 1 : 0);
}

typedef struct {
    const uint8_t *buf;
    size_t   total_bytes;
    size_t   byte_pos;
    int      bit_pos;  /* next bit to read (7=MSB … 0=LSB) */
} bit_reader_t;

static void br_init(bit_reader_t *br, const uint8_t *buf, size_t total_bytes) {
    br->buf         = buf;
    br->total_bytes = total_bytes;
    br->byte_pos    = 0;
    br->bit_pos     = 7;
}

/* Read one bit. Returns 0 or 1, or -1 at end of stream. */
static int br_get_bit(bit_reader_t *br) {
    if (br->byte_pos >= br->total_bytes) return -1;
    int bit = (br->buf[br->byte_pos] >> br->bit_pos) & 1;
    if (br->bit_pos == 0) { br->bit_pos = 7; br->byte_pos++; }
    else { br->bit_pos--; }
    return bit;
}

/* Encode one non-negative integer n (n < 2^31) into the bitstream.
 * Returns 0 on success, -1 on buffer overflow. */
static int eg_encode(bitstream_t *bs, uint32_t n) {
    uint32_t v   = n + 1;    /* shift to 1-based */
    int      tier = 0;
    uint32_t tmp  = v;
    while (tmp >>= 1) tier++; /* tier = floor(log2(v)) */

    /* Write 'tier' zero bits (unary prefix) */
    for (int i = 0; i < tier; i++) {
        if (bs_put_bit(bs, 0) < 0) return -1;
    }
    /* Write terminating 1 bit */
    if (bs_put_bit(bs, 1) < 0) return -1;
    /* Write tier raw bits of offset (MSB first) */
    for (int i = tier - 1; i >= 0; i--) {
        if (bs_put_bit(bs, (v >> i) & 1) < 0) return -1;
    }
    return 0;
}

/* Decode one Elias-gamma integer.  Returns -1 on stream error. */
static int eg_decode(bit_reader_t *br, uint32_t *out) {
    /* Count leading zeros (= tier) */
    int tier = 0;
    for (;;) {
        int b = br_get_bit(br);
        if (b < 0) return -1;
        if (b == 1) break;
        tier++;
        if (tier > 30) return -1; /* sanity: >2B index is impossible */
    }
    /* Read tier raw offset bits */
    uint32_t offset = 0;
    for (int i = 0; i < tier; i++) {
        int b = br_get_bit(br);
        if (b < 0) return -1;
        offset = (offset << 1) | (uint32_t)b;
    }
    *out = (1u << tier) + offset - 1;  /* shift back to 0-based */
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  SimHash 64-bit sketch  (V3: QJL principle)
 *
 *  For a dictionary word w, compute:
 *    sketch(w) = ⊕_{k=0}^{63}  bit_k = (fnv1a(w, seed_k) & 1)
 *
 *  seed_k is the k-th element of a fixed random seed table derived
 *  from the first 64 multiples of the golden-ratio hash constant.
 *  These seeds are "random enough" orthogonal projections.
 *
 *  At search time:
 *    hamming = popcount64(sketch(query) XOR sketch(token))
 *    if hamming ≤ SKETCH_THRESHOLD → candidate match
 *    then do exact string compare only on candidates.
 *
 *  For matching tokens: E[hamming] = 0.  For unrelated tokens the
 *  probability that hamming ≤ 8 is < 2^-16 → extremely low false
 *  positive rate while processing 64 tokens per CPU instruction.
 * ══════════════════════════════════════════════════════════════ */

#define SKETCH_SEEDS_COUNT 64

/* 64 deterministic seeds — golden-ratio multiples mod 2^32 */
static const uint32_t SKETCH_SEEDS[SKETCH_SEEDS_COUNT] = {
    0x9e3779b9u, 0x3c6ef372u, 0xdaa66d2bu, 0x78dde6e4u,
    0x1715609du, 0xb54cda56u, 0x5384540fu, 0xf1bbcdc8u,
    0x8ff34781u, 0x2e2ac13au, 0xcc623af3u, 0x6a99b4acu,
    0x08d12e65u, 0xa708a81eu, 0x454021d7u, 0xe3779b90u,
    0x81af1549u, 0x1fe68f02u, 0xbe1e08bbu, 0x5c558274u,
    0xfa8cfc2du, 0x98c475e6u, 0x36fbef9fu, 0xd5336958u,
    0x736ae311u, 0x11a25ccau, 0xafd9d683u, 0x4e11503cu,
    0xec48c9f5u, 0x8a8043aeu, 0x28b7bd67u, 0xc6ef3720u,
    0x6526b0d9u, 0x035e2a92u, 0xa195a44bu, 0x3fcd1e04u,
    0xde0497bdu, 0x7c3c1176u, 0x1a738b2fu, 0xb8ab04e8u,
    0x56e27ea1u, 0xf519f85au, 0x93517213u, 0x3188ebccu,
    0xcfc06585u, 0x6df7df3eu, 0x0c2f58f7u, 0xaa66d2b0u,
    0x489e4c69u, 0xe6d5c622u, 0x850d3fdbu, 0x2344b994u,
    0xc17c334du, 0x5fb3ad06u, 0xfdeb26bfu, 0x9c22a078u,
    0x3a5a1a31u, 0xd89193eau, 0x76c90da3u, 0x1500875cu,
    0xb3380115u, 0x516f7aceu, 0xefa6f487u, 0x8dde6e40u
};

/* Compute the 64-bit SimHash sketch of a word. */
static uint64_t simhash(const char *word, uint32_t len) {
    uint64_t sketch = 0;
    for (int k = 0; k < SKETCH_SEEDS_COUNT; k++) {
        /* Mix the seed into FNV-1a to get an independent hash per projection */
        uint32_t h = SKETCH_SEEDS[k];
        for (uint32_t i = 0; i < len; i++) {
            h ^= (uint8_t)word[i];
            h *= 16777619u;
        }
        /* Bit k of the sketch = the sign bit of h (i.e. MSB) */
        if (h >> 31) sketch |= (1ull << k);
    }
    return sketch;
}

/* Hardware-accelerated popcount (number of set bits in a 64-bit word). */
static inline int popcount64(uint64_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
    /* MSVC x64: the intrinsic is always available (every x64 CPU has POPCNT) */
    return (int)_mm_popcnt_u64(x);
#elif defined(__POPCNT__)
    return (int)_mm_popcnt_u64(x);
#elif defined(__GNUC__) || defined(__clang__)
    /* __builtin_popcountll lowers to the popcnt instruction only when the
     * target supports it (e.g. -mpopcnt), else emits a software sequence —
     * correct on every baseline, unlike the raw intrinsic. */
    return __builtin_popcountll(x);
#else
    /* Portable fallback — still only ~10 ops */
    x  = x - ((x >> 1) & 0x5555555555555555ull);
    x  = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x  = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (int)((x * 0x0101010101010101ull) >> 56);
#endif
}

/* ══════════════════════════════════════════════════════════════
 *  ZLib Helpers
 * ══════════════════════════════════════════════════════════════ */

/* Compress data with ZLib deflate. Returns malloc'd buffer, sets out_len.
 * Returns NULL on failure. */
static uint8_t *zlib_compress(const uint8_t *src, size_t src_len, size_t *out_len) {
    uLongf bound = compressBound((uLong)src_len);
    uint8_t *dst = (uint8_t *)malloc(bound);
    if (!dst) return NULL;

    if (compress2(dst, &bound, src, (uLong)src_len, Z_BEST_COMPRESSION) != Z_OK) {
        free(dst);
        return NULL;
    }
    *out_len = (size_t)bound;
    return dst;
}

/* Decompress ZLib data. Caller provides expected original size.
 * Returns malloc'd buffer or NULL. */
static uint8_t *zlib_decompress(const uint8_t *src, size_t src_len,
                                 size_t expected_len, size_t *out_len) {
    uint8_t *dst = (uint8_t *)malloc(expected_len + 1);
    if (!dst) return NULL;

    uLongf dst_len = (uLongf)expected_len;
    if (uncompress(dst, &dst_len, src, (uLong)src_len) != Z_OK) {
        free(dst);
        return NULL;
    }
    *out_len = (size_t)dst_len;
    return dst;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_text  (V3)
 * ══════════════════════════════════════════════════════════════
 *
 * Singularity format (inside ZLib wrapper if QTSQ_FLAG_ZLIB is set):
 *
 *   [8B  dict_size  ]  number of unique tokens in dictionary
 *   [8B  num_tokens ]  total tokens in document
 *   [dict_size × (2B word_len + word_bytes)]  dictionary strings
 *   [4B  eg_byte_count]                       Elias-gamma stream byte count
 *   [eg_byte_count bytes]                     Elias-gamma token index stream
 *   [8B  sketch_count]                        = num_tokens
 *   [num_tokens × 8B]                         SimHash sketch per token
 *
 * QTSQ_FLAG_ELIAS_GAMMA is set in header.flags to distinguish V3 from V2.
 * V2 files (varint, no sketches) still decode correctly — the flag is absent.
 */

int qtsq_compress_text(qtsq_context_t *ctx,
                       const char *text, size_t length) {
    if (!ctx || !text) return QTSQ_ERR_NULL;
    if (length == 0) return QTSQ_ERR_TOO_SMALL;

    /* ── Tokenize ──────────────────────────────────────────── */
    size_t num_tokens;
    token_t *tokens = tokenize(text, length, &num_tokens);
    if (!tokens) return QTSQ_ERR_ALLOC;

    /* ── Build frequency-sorted dictionary ─────────────────── */
    size_t dict_size;
    dict_entry_t *dict = build_dictionary(tokens, num_tokens, &dict_size);
    if (!dict) { free_tokens(tokens, num_tokens); return QTSQ_ERR_ALLOC; }

    /* ── Reverse lookup O(1): word → sorted index ───────────── */
    hash_table_t rev = build_reverse_lookup(dict, dict_size);

    /* ── Pre-compute per-word SimHash sketches ──────────────────
     * sketch_dict[i] = 64-bit SimHash of dict[i].word
     * Stored once per unique word, then looked up per token below. */
    uint64_t *sketch_dict = (uint64_t *)malloc(dict_size * sizeof(uint64_t));
    if (!sketch_dict) {
        ht_free(&rev); free_dictionary(dict, dict_size);
        free_tokens(tokens, num_tokens); return QTSQ_ERR_ALLOC;
    }
    for (size_t i = 0; i < dict_size; i++)
        sketch_dict[i] = simhash(dict[i].word, dict[i].length);

    /* ── Collect token index array ──────────────────────────────
     * FIX TXT-1: the encoder had no token ceiling, so on a 32-bit
     * size_t the * 4 below overflows and under-allocates the index
     * array that the loop then overruns. Cap it where the decoder
     * already caps: a billion tokens is far beyond any real corpus,
     * and each token costs at least one input byte anyway. */
    if (num_tokens > QTSQ_HORIZON_MAX_TOKENS) {
        free(sketch_dict); ht_free(&rev);
        free_dictionary(dict, dict_size); free_tokens(tokens, num_tokens);
        return QTSQ_ERR_TOO_LARGE;
    }
    uint32_t *idx_array = (uint32_t *)malloc(num_tokens * sizeof(uint32_t));
    if (!idx_array) {
        free(sketch_dict); ht_free(&rev);
        free_dictionary(dict, dict_size); free_tokens(tokens, num_tokens);
        return QTSQ_ERR_ALLOC;
    }
    for (size_t t = 0; t < num_tokens; t++) {
        uint32_t idx = ht_get(&rev, tokens[t].token, tokens[t].length);
        idx_array[t] = (idx == UINT32_MAX) ? 0 : idx;
    }
    ht_free(&rev);
    free_tokens(tokens, num_tokens);

    /* ── Elias-gamma encode the index stream ───────────────────
     * Upper bound: each index needs at most 2*floor(log2(n))+1 bits.
     * For n < 2^31: max = 63 bits ≈ 8 bytes per index.  Use 5B to be safe. */
    size_t eg_cap = num_tokens * 5 + 16;
    uint8_t *eg_buf = (uint8_t *)calloc(1, eg_cap);
    if (!eg_buf) {
        free(idx_array); free(sketch_dict);
        free_dictionary(dict, dict_size); return QTSQ_ERR_ALLOC;
    }
    bitstream_t bs;
    bs_init(&bs, eg_buf, eg_cap);
    for (size_t t = 0; t < num_tokens; t++) {
        /* FIX TXT-2: save bitstream position BEFORE the encode attempt.
         * If eg_encode fails after writing partial bits, roll back before
         * retry so a partial write is never double-written into the stream.
         *
         * bs_put_bit() returns -1 only when the bit would land past the
         * buffer, and it mutates nothing before that check — so restoring
         * byte_pos/bit_pos is an exact undo.
         *
         * The original retry ignored the second eg_encode() result and grew
         * the buffer at most once. That is not enough: 5 bytes/token is only
         * a valid bound while dict_size is small, but one index can need
         * 2*floor(log2(dict_size))+1 bits — over 8 bytes once the dictionary
         * passes ~2^11 entries. A still-failing retry then truncated the
         * stream silently and the file decompressed to the wrong text.
         * Grow until the encode actually succeeds, and fail loudly if it
         * cannot. */
        size_t saved_byte_pos = bs.byte_pos;
        int    saved_bit_pos  = bs.bit_pos;
        if (eg_encode(&bs, idx_array[t]) < 0) {
            /* Restore position to undo any partial bit writes */
            bs.byte_pos = saved_byte_pos;
            bs.bit_pos  = saved_bit_pos;
            int ok = 0;
            /* Bound growth: each round doubles, and the ceiling is far
             * above the 9 bytes/token worst case, so this terminates. */
            for (int attempt = 0; attempt < 24 && eg_cap < num_tokens * 16; attempt++) {
                eg_cap *= 2;
                uint8_t *tmp = (uint8_t *)realloc(eg_buf, eg_cap);
                if (!tmp) {
                    free(eg_buf); free(idx_array); free(sketch_dict);
                    free_dictionary(dict, dict_size); return QTSQ_ERR_ALLOC;
                }
                eg_buf = tmp;
                memset(eg_buf + eg_cap / 2, 0, eg_cap / 2);
                bs.buf = eg_buf; bs.capacity = eg_cap;
                bs.byte_pos = saved_byte_pos; bs.bit_pos = saved_bit_pos;
                if (eg_encode(&bs, idx_array[t]) == 0) { ok = 1; break; }
                /* still short: undo and try a larger buffer */
                bs.byte_pos = saved_byte_pos; bs.bit_pos = saved_bit_pos;
            }
            if (!ok) {
                free(eg_buf); free(idx_array); free(sketch_dict);
                free_dictionary(dict, dict_size); return QTSQ_ERR_ALLOC;
            }
        }
    }
    size_t eg_bytes = bs_bytes_written(&bs);

    /* sketch_stream is NOT stored in the compressed payload.
     * Sketches are fully deterministic from the dictionary and are
     * reconstructed at search time (O(dict_size) not O(num_tokens)).
     * This eliminates num_tokens*8 bytes of overhead — the dominant
     * cause of Horizon producing files larger than their originals. */
    free(sketch_dict);
    free(idx_array);

    /* ── Pack into raw buffer ───────────────────────────────────
     * Layout:
     *   [8B] dict_size
     *   [8B] num_tokens
     *   dict entries: [2B word_len + word_bytes] × dict_size
     *   [4B] eg_bytes
     *   [eg_bytes] Elias-gamma token index stream  */
    size_t dict_bytes = 0;
    for (size_t i = 0; i < dict_size; i++) dict_bytes += 2 + dict[i].length;

    size_t raw_capacity = 8 + 8 + dict_bytes + 4 + eg_bytes;
    uint8_t *raw_buf = (uint8_t *)malloc(raw_capacity);
    if (!raw_buf) {
        free(eg_buf);
        free_dictionary(dict, dict_size); return QTSQ_ERR_ALLOC;
    }

    size_t pos = 0;

    /* Header counts */
    uint64_t ds64 = (uint64_t)dict_size;
    uint64_t nt64 = (uint64_t)num_tokens;
    memcpy(raw_buf + pos, &ds64, 8); pos += 8;
    memcpy(raw_buf + pos, &nt64, 8); pos += 8;

    /* Dictionary strings */
    for (size_t i = 0; i < dict_size; i++) {
        /* FIX TXT-4: word longer than uint16_t max cannot be stored — reject cleanly */
        if (dict[i].length > 65535) {
            free(eg_buf);
            free_dictionary(dict, dict_size);
            return QTSQ_ERR_FORMAT;
        }
        uint16_t wlen = (uint16_t)dict[i].length;
        memcpy(raw_buf + pos, &wlen, 2); pos += 2;
        memcpy(raw_buf + pos, dict[i].word, dict[i].length);
        pos += dict[i].length;
    }
    free_dictionary(dict, dict_size);

    /* Elias-gamma stream */
    uint32_t eg32 = (uint32_t)eg_bytes;
    memcpy(raw_buf + pos, &eg32, 4); pos += 4;
    memcpy(raw_buf + pos, eg_buf, eg_bytes); pos += eg_bytes;
    free(eg_buf);

    /* ── Compress the packed buffer ────────────────────────────
     * zlib by default; qtc generic lens when the context opts in
     * (Phase 6a). Same [u64 raw_size][stream] framing either way —
     * the decoder sniffs the stream magic.
     *
     * v0.15 text contest: under the qtc codec, also try compressing
     * the RAW text and keep the smaller of (packed + qtc) vs
     * (raw + qtc). The packed layout spends 8 bytes per token on
     * search sketches, which loses to plain LZ2 on most real text.
     * Raw mode marks the singularity with a "SQTR" prefix and drops
     * the dictionary/sketch layout, so qtsq_radiate_text_search
     * reports QTSQ_ERR_FORMAT for those files (callers fall back to
     * qtsq_decompress + their own search). */
    size_t zlib_len = 0;
    uint8_t *zlib_buf = NULL;
    uint8_t *raw_qtc = NULL;
    size_t raw_qtc_len = 0;

    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(raw_buf, pos, &zlib_buf, &zlib_len) != QTSQ_OK)
            zlib_buf = NULL;
        uint8_t *rt = NULL;
        size_t rt_len = 0;
        if (qtsq_codec_compress((const uint8_t *)text, length,
                                &rt, &rt_len) == QTSQ_OK && rt) {
            size_t pack_total = (zlib_buf && zlib_len < pos)
                                    ? 8 + zlib_len : pos;
            if (4 + rt_len < pack_total) {
                raw_qtc = rt;
                raw_qtc_len = rt_len;
                rt = NULL;
            }
            free(rt);
        }
    } else {
        zlib_buf = zlib_compress(raw_buf, pos, &zlib_len);
    }

    /* mode: 0 = stored pack, 1 = compressed pack, 2 = raw-text qtc */
    int mode = 0;
    if (raw_qtc) {
        uint8_t *final_buf = (uint8_t *)malloc(4 + raw_qtc_len);
        if (final_buf) {
            memcpy(final_buf, "SQTR", 4);
            memcpy(final_buf + 4, raw_qtc, raw_qtc_len);
            free(raw_buf);
            if (zlib_buf) free(zlib_buf);
            free(raw_qtc);
            if (ctx->singularity) free(ctx->singularity);
            ctx->singularity = final_buf;
            ctx->singularity_size = 4 + raw_qtc_len;
            mode = 2;
        } else {
            free(raw_qtc);
        }
    }
    if (mode != 2) {
        int used_zlib = 0;
        if (zlib_buf && zlib_len < pos) {
            size_t final_size = 8 + zlib_len;
            uint8_t *final_buf = (uint8_t *)malloc(final_size);
            if (final_buf) {
                uint64_t raw_size = (uint64_t)pos;
                memcpy(final_buf, &raw_size, 8);
                memcpy(final_buf + 8, zlib_buf, zlib_len);
                free(raw_buf); free(zlib_buf);
                if (ctx->singularity) free(ctx->singularity);
                ctx->singularity = final_buf;
                ctx->singularity_size = final_size;
                used_zlib = 1;
            }
        }
        if (!used_zlib) {
            if (zlib_buf) free(zlib_buf);
            if (ctx->singularity) free(ctx->singularity);
            ctx->singularity = raw_buf;
            ctx->singularity_size = pos;
        }
        mode = used_zlib ? 1 : 0;
    }

    /* ── Header ─────────────────────────────────────────────── */
    ctx->header.strategy        = QTSQ_STRATEGY_DICTIONARY;
    ctx->header.data_type       = QTSQ_TYPE_TEXT_UTF8;
    ctx->header.original_size   = length;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    if (mode != 2) ctx->header.flags |= QTSQ_FLAG_ELIAS_GAMMA;
    if (mode == 1) ctx->header.flags |= QTSQ_FLAG_ZLIB;

    qtsq_sha256((const uint8_t *)text, length, ctx->header.checksum);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_json
 * ══════════════════════════════════════════════════════════════
 *
 * JSON-specific compression:
 *   1. Minify whitespace (strip non-essential spaces/newlines)
 *   2. Compress with standard text dictionary engine
 *   3. Set type to QTSQ_TYPE_JSON
 *
 * Future: key dictionary + structural opcode encoding
 */

/* Simple JSON minifier: strip whitespace outside strings */
static char *json_minify(const char *json, size_t length, size_t *out_len) {
    char *out = (char *)malloc(length + 1);
    if (!out) return NULL;

    size_t op = 0;
    int in_string = 0;
    int escape = 0;

    for (size_t i = 0; i < length; i++) {
        char c = json[i];

        if (escape) {
            out[op++] = c;
            escape = 0;
            continue;
        }

        if (c == '\\' && in_string) {
            out[op++] = c;
            escape = 1;
            continue;
        }

        if (c == '"') {
            in_string = !in_string;
            out[op++] = c;
            continue;
        }

        if (in_string) {
            out[op++] = c;
        } else {
            /* Outside string: skip whitespace */
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                out[op++] = c;
            }
        }
    }

    out[op] = '\0';
    *out_len = op;
    return out;
}

int qtsq_compress_json(qtsq_context_t *ctx,
                       const char *json, size_t length) {
    if (!ctx || !json) return QTSQ_ERR_NULL;
    if (length == 0) return QTSQ_ERR_TOO_SMALL;

    /* Minify JSON */
    size_t minified_len;
    char *minified = json_minify(json, length, &minified_len);
    if (!minified) return QTSQ_ERR_ALLOC;

    /* Use text compression on the minified JSON */
    int result = qtsq_compress_text(ctx, minified, minified_len);
    free(minified);
    if (result != QTSQ_OK) return result;

    /* Override type and store original (pre-minified) size */
    ctx->header.data_type = QTSQ_TYPE_JSON;
    ctx->header.original_size = minified_len; /* decompressed = minified */

    /* Re-hash on minified content for correct checksum */
    /* (already done inside qtsq_compress_text on minified) */

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_csv (column-aware)
 * ══════════════════════════════════════════════════════════════
 *
 * Column-aware CSV compression:
 *   1. Parse rows and extract column values
 *   2. Transpose to column-major order (repeated values group together)
 *   3. Compress with text dictionary engine (column values cluster in dict)
 *   4. Set type to QTSQ_TYPE_CSV
 */

int qtsq_compress_csv(qtsq_context_t *ctx,
                      const char *csv_data, size_t length) {
    if (!ctx || !csv_data) return QTSQ_ERR_NULL;
    if (length == 0) return QTSQ_ERR_TOO_SMALL;

    /* Use text compression as the base
     * Column-aware transposition adds complexity without much benefit
     * for small CSVs. The dictionary engine already deduplicates repeated values. */
    int result = qtsq_compress_text(ctx, csv_data, length);
    if (result != QTSQ_OK) return result;

    /* Override type to CSV */
    ctx->header.data_type = QTSQ_TYPE_CSV;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_horizon  (V2 + V3 unified)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_horizon(const qtsq_context_t *ctx,
                            uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;

    const uint8_t *buf   = ctx->singularity;
    size_t         total = (size_t)ctx->singularity_size;
    size_t         pos   = 0;

    /* ── v0.15 raw-text mode (qtc text contest winner) ─────────
     * Singularity = [4B "SQTR"][qtc stream of the raw text]. Only
     * produced under the opt-in qtc codec when raw beat the packed
     * layout. V3 files always carry ELIAS_GAMMA and V2 files either
     * carry ZLIB or begin with an 8-byte dictionary count, so this
     * dispatch is unambiguous. */
    if (!(ctx->header.flags & QTSQ_FLAG_ELIAS_GAMMA) &&
        !(ctx->header.flags & QTSQ_FLAG_ZLIB) &&
        total >= 4 + QTSQ_QTC_HEADER_SIZE &&
        buf[0] == 'S' && buf[1] == 'Q' && buf[2] == 'T' && buf[3] == 'R' &&
        qtsq_codec_is_qtc(buf + 4, total - 4)) {
        uint64_t decl = qtsq_codec_declared_size(buf + 4, total - 4);
        if (decl == 0 || decl != ctx->header.original_size ||
            decl > 10000000000000ull)
            return QTSQ_ERR_FORMAT;
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        if (qtsq_codec_decompress(buf + 4, total - 4, decl,
                                  &raw, &raw_len) != QTSQ_OK)
            return QTSQ_ERR_FORMAT;
        *out_data = raw;
        *out_size = raw_len;
        return QTSQ_OK;
    }

    /* ── Inflate ZLib wrapper if present ──────────────────────── */
    uint8_t *decompressed_buf = NULL;
    if (ctx->header.flags & QTSQ_FLAG_ZLIB) {
        if (total < 8) return QTSQ_ERR_FORMAT;
        uint64_t raw_size;
        memcpy(&raw_size, buf, 8);
        if (raw_size > 10000000000000ull) return QTSQ_ERR_FORMAT;
        size_t dec_len;
        const uint8_t *body = buf + 8;
        size_t body_len = total - 8;
        if (qtsq_codec_is_qtc(body, body_len)) {
            /* Phase 6a: qtc stream — bound the decode to the declared size. */
            if (qtsq_codec_declared_size(body, body_len) != raw_size)
                return QTSQ_ERR_FORMAT;
            if (qtsq_codec_decompress(body, body_len, raw_size,
                                      &decompressed_buf, &dec_len) != QTSQ_OK)
                return QTSQ_ERR_FORMAT;
        } else {
            decompressed_buf = zlib_decompress(body, body_len, (size_t)raw_size, &dec_len);
            if (!decompressed_buf) return QTSQ_ERR_FORMAT;
        }
        buf   = decompressed_buf;
        total = dec_len;
        pos   = 0;
    }

    /* ── Read counts ──────────────────────────────────────────── */
    if (pos + 16 > total) { if (decompressed_buf) free(decompressed_buf); return QTSQ_ERR_FORMAT; }
    uint64_t dict_size_64, num_tokens_64;
    memcpy(&dict_size_64, buf + pos, 8); pos += 8;
    memcpy(&num_tokens_64, buf + pos, 8); pos += 8;
    size_t dict_size  = (size_t)dict_size_64;
    size_t num_tokens = (size_t)num_tokens_64;
    if (dict_size > 2000000000ULL || num_tokens > 1000000000000ULL) {
        if (decompressed_buf) free(decompressed_buf); return QTSQ_ERR_FORMAT;
    }

    /* ── Read dictionary ──────────────────────────────────────── */
    char    **dict_words = (char    **)calloc(dict_size, sizeof(char *));
    uint16_t *dict_lens  = (uint16_t  *)calloc(dict_size, sizeof(uint16_t));
    if (!dict_words || !dict_lens) {
        free(dict_words); free(dict_lens);
        if (decompressed_buf) free(decompressed_buf); return QTSQ_ERR_ALLOC;
    }
    for (size_t i = 0; i < dict_size; i++) {
        if (pos + 2 > total) goto format_err;
        memcpy(&dict_lens[i], buf + pos, 2); pos += 2;
        if (pos + dict_lens[i] > total) goto format_err;
        dict_words[i] = (char *)malloc(dict_lens[i] + 1);
        if (!dict_words[i]) goto alloc_err;
        memcpy(dict_words[i], buf + pos, dict_lens[i]);
        dict_words[i][dict_lens[i]] = '\0';
        pos += dict_lens[i];
    }

    /* ── Detect V2 (varint) vs V3 (Elias-gamma) ─────────────── */
    int is_v3 = (ctx->header.flags & QTSQ_FLAG_ELIAS_GAMMA) != 0;

    if (is_v3) {
        /* ── V3 path: Elias-gamma index stream ────────────────── */
        if (pos + 4 > total) goto format_err;
        uint32_t eg32;
        memcpy(&eg32, buf + pos, 4); pos += 4;
        size_t eg_bytes = (size_t)eg32;
        if (pos + eg_bytes > total) goto format_err;

        const uint8_t *eg_buf = buf + pos;
        pos += eg_bytes;
        /* (sketch stream follows, but we skip it during normal decompression) */

        /* First pass: compute output size */
        size_t output_size = 0;
        {
            bit_reader_t br;
            br_init(&br, eg_buf, eg_bytes);
            for (size_t t = 0; t < num_tokens; t++) {
                uint32_t idx;
                if (eg_decode(&br, &idx) < 0 || idx >= dict_size) goto format_err;
                output_size += dict_lens[idx];
            }
        }

        uint8_t *output = (uint8_t *)malloc(output_size + 1);
        if (!output) goto alloc_err;

        /* Second pass: reconstruct text */
        {
            bit_reader_t br;
            br_init(&br, eg_buf, eg_bytes);
            size_t out_pos = 0;
            for (size_t t = 0; t < num_tokens; t++) {
                uint32_t idx;
                eg_decode(&br, &idx);
                memcpy(output + out_pos, dict_words[idx], dict_lens[idx]);
                out_pos += dict_lens[idx];
            }
            output[out_pos] = '\0';
        }

        for (size_t i = 0; i < dict_size; i++) free(dict_words[i]);
        free(dict_words); free(dict_lens);
        if (decompressed_buf) free(decompressed_buf);
        *out_data = output;
        *out_size = output_size;
        return QTSQ_OK;

    } else {
        /* ── V2 path: varint index stream (backward-compat) ────── */
        size_t indices_start = pos;
        size_t output_size   = 0;
        {
            size_t tmp_pos = pos;
            for (size_t t = 0; t < num_tokens; t++) {
                uint32_t idx;
                size_t consumed = varint_decode(buf + tmp_pos, total - tmp_pos, &idx);
                if (consumed == 0 || idx >= dict_size) goto format_err;
                tmp_pos += consumed;
                output_size += dict_lens[idx];
            }
        }
        uint8_t *output = (uint8_t *)malloc(output_size + 1);
        if (!output) goto alloc_err;

        pos = indices_start;
        size_t out_pos = 0;
        for (size_t t = 0; t < num_tokens; t++) {
            uint32_t idx;
            pos += varint_decode(buf + pos, total - pos, &idx);
            memcpy(output + out_pos, dict_words[idx], dict_lens[idx]);
            out_pos += dict_lens[idx];
        }
        output[out_pos] = '\0';

        for (size_t i = 0; i < dict_size; i++) free(dict_words[i]);
        free(dict_words); free(dict_lens);
        if (decompressed_buf) free(decompressed_buf);
        *out_data = output;
        *out_size = output_size;
        return QTSQ_OK;
    }

format_err:
    for (size_t i = 0; i < dict_size; i++) if (dict_words[i]) free(dict_words[i]);
    free(dict_words); free(dict_lens);
    if (decompressed_buf) free(decompressed_buf);
    return QTSQ_ERR_FORMAT;

alloc_err:
    for (size_t i = 0; i < dict_size; i++) if (dict_words[i]) free(dict_words[i]);
    free(dict_words); free(dict_lens);
    if (decompressed_buf) free(decompressed_buf);
    return QTSQ_ERR_ALLOC;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_radiate_text_search
 *
 *  Search a V3-compressed text corpus without decompressing it.
 *
 *  Algorithm (QJL/SimHash fast-path):
 *    1. Compute 64-bit sketch(query) in O(len) time.
 *    2. For each token i, XOR sketch[i] with sketch(query).
 *       The XOR is non-zero only where the two sketches differ.
 *    3. popcount(XOR) = Hamming distance.
 *       If Hamming ≤ threshold → candidate → do exact match.
 *    4. For true matches, decode the byte offset of the token in the
 *       original text using the dictionary and index stream.
 *
 *  Complexity:  O(n/64) for the sketch scan (64 tokens per POPCNT)
 *               O(k)    for k exact-match verifications (k << n)
 *  Memory:      O(1) working set — reads directly from the mmap'd singularity
 *
 *  callback(token_index, userdata) is invoked for each confirmed match.
 *  Return QTSQ_OK when done, or QTSQ_ERR_FORMAT for corrupt data.
 * ══════════════════════════════════════════════════════════════ */

#define SKETCH_HAMMING_THRESHOLD 6   /* ≤6 differing bits = candidate match */

int qtsq_radiate_text_search(const qtsq_context_t *ctx,
                              const char *query, size_t query_len,
                              qtsq_text_match_cb callback,
                              void *userdata) {
    if (!ctx || !query || !callback) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;
    /* Only supported for V3 files */
    if (!(ctx->header.flags & QTSQ_FLAG_ELIAS_GAMMA)) return QTSQ_ERR_FORMAT;

    const uint8_t *buf   = ctx->singularity;
    size_t         total = (size_t)ctx->singularity_size;

    /* ── Inflate ZLib wrapper if present ──────────────────────── */
    uint8_t *decompressed_buf = NULL;
    if (ctx->header.flags & QTSQ_FLAG_ZLIB) {
        if (total < 8) return QTSQ_ERR_FORMAT;
        uint64_t raw_size;
        memcpy(&raw_size, buf, 8);
        if (raw_size > 10000000000000ull) return QTSQ_ERR_FORMAT;
        size_t dec_len;
        const uint8_t *body = buf + 8;
        size_t body_len = total - 8;
        if (qtsq_codec_is_qtc(body, body_len)) {
            /* Phase 6a: qtc stream — bound the decode to the declared size. */
            if (qtsq_codec_declared_size(body, body_len) != raw_size)
                return QTSQ_ERR_FORMAT;
            if (qtsq_codec_decompress(body, body_len, raw_size,
                                      &decompressed_buf, &dec_len) != QTSQ_OK)
                return QTSQ_ERR_FORMAT;
        } else {
            decompressed_buf = zlib_decompress(body, body_len, (size_t)raw_size, &dec_len);
            if (!decompressed_buf) return QTSQ_ERR_FORMAT;
        }
        buf   = decompressed_buf;
        total = dec_len;
    }
    size_t pos = 0;

    /* ── Read counts ──────────────────────────────────────────── */
    if (pos + 16 > total) { free(decompressed_buf); return QTSQ_ERR_FORMAT; }
    uint64_t dict_size_64, num_tokens_64;
    memcpy(&dict_size_64, buf + pos, 8); pos += 8;
    memcpy(&num_tokens_64, buf + pos, 8); pos += 8;
    size_t dict_size  = (size_t)dict_size_64;
    size_t num_tokens = (size_t)num_tokens_64;
    if (dict_size > 2000000000ULL || num_tokens > 1000000000000ULL) {
        free(decompressed_buf); return QTSQ_ERR_FORMAT;
    }

    /* ── Read (and keep) the dictionary ──────────────────────── */
    char    **dict_words = (char    **)calloc(dict_size, sizeof(char *));
    uint16_t *dict_lens  = (uint16_t  *)calloc(dict_size, sizeof(uint16_t));
    if (!dict_words || !dict_lens) {
        free(dict_words); free(dict_lens); free(decompressed_buf);
        return QTSQ_ERR_ALLOC;
    }
    for (size_t i = 0; i < dict_size; i++) {
        if (pos + 2 > total) goto search_format_err;
        memcpy(&dict_lens[i], buf + pos, 2); pos += 2;
        if (pos + dict_lens[i] > total) goto search_format_err;
        dict_words[i] = (char *)malloc(dict_lens[i] + 1);
        if (!dict_words[i]) goto search_alloc_err;
        memcpy(dict_words[i], buf + pos, dict_lens[i]);
        dict_words[i][dict_lens[i]] = '\0';
        pos += dict_lens[i];
    }

    /* ── Read Elias-gamma stream ─────────────────────────────── */
    if (pos + 4 > total) goto search_format_err;
    uint32_t eg32;
    memcpy(&eg32, buf + pos, 4); pos += 4;
    size_t eg_bytes = (size_t)eg32;
    if (pos + eg_bytes > total) goto search_format_err;
    const uint8_t *eg_buf = buf + pos;
    /* pos += eg_bytes; — not needed, eg_buf is a direct pointer */

    /* ── Rebuild per-dictionary sketches on the fly ───────────── *
     * Sketches are no longer stored in the file. We recompute them
     * from the dictionary (O(dict_size × 64)) which is far smaller
     * than the old O(num_tokens × 8B) stored stream.              */
    uint64_t query_sketch = simhash(query, (uint32_t)query_len);

    uint64_t *sketch_dict_local = (uint64_t *)malloc(dict_size * sizeof(uint64_t));
    if (!sketch_dict_local) goto search_alloc_err;

    /* Build exact-match bitset and per-word sketches simultaneously */
    size_t dict_match_words = (dict_size + 63) / 64;
    uint64_t *dict_match = (uint64_t *)calloc(dict_match_words, sizeof(uint64_t));
    if (!dict_match) { free(sketch_dict_local); goto search_alloc_err; }

    for (size_t i = 0; i < dict_size; i++) {
        sketch_dict_local[i] = simhash(dict_words[i], dict_lens[i]);
        if (dict_lens[i] == (uint16_t)query_len &&
            memcmp(dict_words[i], query, query_len) == 0) {
            dict_match[i / 64] |= (1ull << (i % 64));
        }
    }

    /* ── Decode EG stream + sketch-filter in one pass ────────── *
     * For each token, decode its dict index, look up the pre-computed
     * sketch for that word, and apply the Hamming filter.          */
    {
        bit_reader_t br;
        br_init(&br, eg_buf, eg_bytes);
        uint64_t match_count = 0;
        for (size_t t = 0; t < num_tokens; t++) {
            uint32_t idx;
            if (eg_decode(&br, &idx) < 0 || idx >= dict_size) goto search_format_err;
            uint64_t xor_val = sketch_dict_local[idx] ^ query_sketch;
            int hamming = popcount64(xor_val);
            if (hamming <= SKETCH_HAMMING_THRESHOLD) {
                if ((dict_match[idx / 64] >> (idx % 64)) & 1) {
                    callback((uint64_t)t, match_count, userdata);
                    match_count++;
                }
            }
        }
    }

    free(sketch_dict_local);
    free(dict_match);
    for (size_t i = 0; i < dict_size; i++) free(dict_words[i]);
    free(dict_words); free(dict_lens);
    free(decompressed_buf);
    return QTSQ_OK;

search_format_err:
    for (size_t i = 0; i < dict_size; i++) if (dict_words[i]) free(dict_words[i]);
    free(dict_words); free(dict_lens); free(decompressed_buf);
    return QTSQ_ERR_FORMAT;

search_alloc_err:
    for (size_t i = 0; i < dict_size; i++) if (dict_words[i]) free(dict_words[i]);
    free(dict_words); free(dict_lens); free(decompressed_buf);
    return QTSQ_ERR_ALLOC;
}
