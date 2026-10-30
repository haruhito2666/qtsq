/*
 * sfb_text.c — Sofuu Brain v2: tokeniser, Jaccard similarity, BM25 scoring.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/). Implements exactly the
 * text block of sfb_internal.h.
 *
 * Thread safety: no globals, no mutable state, no allocation. Every helper
 * works on caller-supplied stack tables, so any number of threads may call
 * these simultaneously.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <math.h>

/* ── hashing + tokenisation ─────────────────────────────────────────────
 * FNV-1a 64: xor-then-multiply per byte, offset basis / prime per the spec.
 * ---------------------------------------------------------------------- */
#define SFB_FNV_OFFSET 0xcbf29ce484222325ULL
#define SFB_FNV_PRIME  0x100000001b3ULL

uint64_t sfb_text_hash(const char *s)
{
    uint64_t h = SFB_FNV_OFFSET;
    if (!s) return h;                     /* NULL == empty string */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint64_t)*p;
        h *= SFB_FNV_PRIME;
    }
    return h;
}

/* ASCII-only character classes: locale-independent, and never UB on a byte
 * outside the source charset (unlike tolower() on a plain char). */
static int sfb_is_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

/* Advance to the next token, hashing it lowercased as we scan (single pass,
 * no scratch buffer). Returns NULL at end of input, else a pointer just past
 * the token. */
static const char *sfb_tok_next(const char *p, uint64_t *hash_out)
{
    if (!p) return NULL;
    while (*p && !sfb_is_alnum((unsigned char)*p)) p++;
    if (!*p) return NULL;

    uint64_t h = SFB_FNV_OFFSET;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (!sfb_is_alnum(c)) break;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        h ^= (uint64_t)c;
        h *= SFB_FNV_PRIME;
        p++;
    }
    *hash_out = h;
    return p;
}

/* ── token set / term-frequency table ───────────────────────────────────
 * Fixed-size open-addressing table. SFB_TOK_MAX bounds distinct tokens kept
 * per string; longer inputs are truncated gracefully (extra tokens dropped,
 * never an error, never a heap allocation). Slots is a power of two and at
 * least 2*MAX so linear probing always finds an empty slot.
 * ---------------------------------------------------------------------- */
#define SFB_TOK_MAX   256u
#define SFB_TOK_SLOTS 512u

typedef struct sfb_tokmap {
    uint64_t key[SFB_TOK_SLOTS];
    uint32_t cnt[SFB_TOK_SLOTS];   /* 0 == empty slot */
    uint32_t n;                    /* distinct keys   */
} sfb_tokmap_t;

static void sfb_tokmap_reset(sfb_tokmap_t *m)
{
    memset(m->cnt, 0, sizeof m->cnt);
    m->n = 0;
}

/* 1-based slot index, 0 when absent. */
static uint32_t sfb_tokmap_find(const sfb_tokmap_t *m, uint64_t h)
{
    uint32_t i = (uint32_t)h & (SFB_TOK_SLOTS - 1u);
    while (m->cnt[i]) {
        if (m->key[i] == h) return i + 1u;
        i = (i + 1u) & (SFB_TOK_SLOTS - 1u);
    }
    return 0;
}

static void sfb_tokmap_bump(sfb_tokmap_t *m, uint64_t h)
{
    uint32_t i = (uint32_t)h & (SFB_TOK_SLOTS - 1u);
    while (m->cnt[i]) {
        if (m->key[i] == h) { m->cnt[i]++; return; }
        i = (i + 1u) & (SFB_TOK_SLOTS - 1u);
    }
    if (m->n >= SFB_TOK_MAX) return;          /* truncate gracefully */
    m->key[i] = h;
    m->cnt[i] = 1u;
    m->n++;
}

/* Fill m with token hashes/counts; *total counts every token in s (duplicates
 * included), which is the BM25 document length. */
static void sfb_tokmap_build(sfb_tokmap_t *m, const char *s, uint32_t *total)
{
    uint32_t t = 0;
    uint64_t h;
    sfb_tokmap_reset(m);
    for (const char *p = s; (p = sfb_tok_next(p, &h)) != NULL; ) {
        sfb_tokmap_bump(m, h);
        t++;
    }
    if (total) *total = t;
}

/* ── Jaccard over token SETS ─────────────────────────────────────────────
 * Distinct tokens only: repeats do not inflate either side. A 64-bit hash
 * collision would fold two distinct tokens into one; for real text that is
 * not reachable in practice and it can only ever shift the ratio slightly.
 * ---------------------------------------------------------------------- */
double sfb_text_jaccard(const char *a, const char *b)
{
    sfb_tokmap_t ma, mb;
    sfb_tokmap_build(&ma, a, NULL);
    sfb_tokmap_build(&mb, b, NULL);

    if (ma.n == 0 && mb.n == 0) return 1.0;   /* two empties are identical */
    if (ma.n == 0 || mb.n == 0) return 0.0;

    uint32_t inter = 0;
    for (uint32_t i = 0; i < SFB_TOK_SLOTS; i++)
        if (ma.cnt[i] && sfb_tokmap_find(&mb, ma.key[i])) inter++;

    uint32_t uni = ma.n + mb.n - inter;
    return uni ? (double)inter / (double)uni : 1.0;
}

/* ── BM25 ────────────────────────────────────────────────────────────────
 * Classic Okapi BM25 over term frequencies in `doc`, scored against the
 * (de-duplicated) query terms:
 *
 *   score = Σ_q  idf(q) · tf·(k1+1) / (tf + k1·(1 - b + b·dl/avg_len))
 *
 * This entry point is the SINGLE-DOCUMENT form, deliberately: it is handed one
 * document and nothing else, so it treats the collection as N = 1 and for any
 * query term that occurs in `doc` df == N == 1, giving the constant
 *
 *   idf = ln(1 + (N - df + 0.5)/(df + 0.5)) = ln(1 + 0.5/1.5) = ln(4/3)
 *
 * A ranking over a corpus must NOT use this: a constant idf scores a term that
 * appears in every document exactly like one that appears in one, so the
 * lexical list could not tell a rare term from a stopword (W4.6).  The
 * corpus-aware form is sfb_text_stats_begin/add + sfb_text_bm25_stats below,
 * which computes real document frequencies.  This helper stays because it has
 * no corpus by contract and existing callers/tests use it as a monotone
 * per-document weight.  Terms absent from `doc` contribute nothing (tf == 0),
 * so the score can never be negative.
 * ---------------------------------------------------------------------- */
double sfb_text_bm25(const char *query, const char *doc, double avg_len,
                     double k1, double b)
{
    sfb_tokmap_t mq, md;
    uint32_t dl_tok = 0;

    sfb_tokmap_build(&mq, query, NULL);
    sfb_tokmap_build(&md, doc, &dl_tok);
    if (mq.n == 0 || md.n == 0) return 0.0;

    double dl = (double)dl_tok;
    if (!isfinite(avg_len) || avg_len <= 0.0) avg_len = dl;
    if (!isfinite(k1) || k1 < 0.0) k1 = 1.2;
    if (!isfinite(b)) b = 0.75;
    if (b < 0.0) b = 0.0;
    if (b > 1.0) b = 1.0;

    const double N  = 1.0;                    /* single-document collection */
    const double df = 1.0;                    /* term present ⇒ df == N     */
    const double idf = log(1.0 + (N - df + 0.5) / (df + 0.5));

    const double norm = k1 * (1.0 - b + b * (dl / avg_len));
    double score = 0.0;

    for (uint32_t i = 0; i < SFB_TOK_SLOTS; i++) {
        if (!mq.cnt[i]) continue;            /* query term, visited once */
        uint32_t j = sfb_tokmap_find(&md, mq.key[i]);
        if (!j) continue;                    /* tf == 0 ⇒ no contribution */
        double tf = (double)md.cnt[j - 1u];
        score += idf * (tf * (k1 + 1.0)) / (tf + norm);
    }

    if (!isfinite(score) || score < 0.0) return 0.0;
    return score;
}

/* ── BM25 with real corpus statistics (W4.6) ─────────────────────────────
 * The contract is in sfb_internal.h.  The corpus owner (sfb_brain.c) walks its
 * live rows once to accumulate df and the average length, then once more to
 * score.  Two passes, no allocation, exact df — the honest version of the
 * constant-idf shortcut above.
 *
 * df is the standard document frequency: the number of live documents that
 * contain the term at least once, the document being scored included.  idf is
 * the BM25+ style ln(1 + (N - df + 0.5)/(df + 0.5)), which is positive for
 * every df <= N and therefore cannot invert a match into a penalty.
 * ---------------------------------------------------------------------- */
#if SFB_TEXT_STATS_SLOTS != SFB_TOK_SLOTS
#error "sfb_internal.h and sfb_text.c disagree about the token-table geometry"
#endif

void sfb_text_stats_begin(sfb_text_stats_t *st, const char *query)
{
    sfb_tokmap_t mq;
    uint32_t i;

    if (st == NULL)
        return;
    memset(st, 0, sizeof *st);
    sfb_tokmap_build(&mq, query, NULL);
    for (i = 0; i < SFB_TOK_SLOTS; i++) {
        if (!mq.cnt[i])
            continue;
        st->key[i] = mq.key[i];
        st->in_query[i] = 1u;
        st->nterms++;
    }
}

void sfb_text_stats_add(sfb_text_stats_t *st, const char *doc)
{
    sfb_tokmap_t md;
    uint32_t dl = 0, i;

    if (st == NULL)
        return;
    sfb_tokmap_build(&md, doc, &dl);
    st->n_docs++;
    st->total_len += (double)dl;
    for (i = 0; i < SFB_TEXT_STATS_SLOTS; i++)
        if (st->in_query[i] && sfb_tokmap_find(&md, st->key[i]) != 0u)
            st->df[i]++;
}

double sfb_text_bm25_stats(const sfb_text_stats_t *st, const char *doc,
                           double k1, double b)
{
    sfb_tokmap_t md;
    uint32_t dl_tok = 0, i;
    double n, avg, norm, score = 0.0;

    if (st == NULL || st->n_docs == 0 || st->nterms == 0)
        return 0.0;
    sfb_tokmap_build(&md, doc, &dl_tok);
    if (md.n == 0)
        return 0.0;

    n = (double)st->n_docs;
    avg = st->total_len > 0.0 ? st->total_len / n : (double)dl_tok;
    if (!isfinite(avg) || avg < 1.0)
        avg = dl_tok > 0 ? (double)dl_tok : 1.0;
    if (!isfinite(k1) || k1 < 0.0) k1 = 1.2;
    if (!isfinite(b)) b = 0.75;
    if (b < 0.0) b = 0.0;
    if (b > 1.0) b = 1.0;

    norm = k1 * (1.0 - b + b * ((double)dl_tok / avg));

    for (i = 0; i < SFB_TEXT_STATS_SLOTS; i++) {
        uint32_t j, df;
        double idf, tf;

        if (!st->in_query[i])
            continue;
        j = sfb_tokmap_find(&md, st->key[i]);
        if (!j)
            continue;                       /* tf == 0 ⇒ no contribution    */
        df = st->df[i];
        if (df == 0u)
            continue;                       /* cannot happen with tf > 0    */
        idf = log(1.0 + (n - (double)df + 0.5) / ((double)df + 0.5));
        tf = (double)md.cnt[j - 1u];
        score += idf * (tf * (k1 + 1.0)) / (tf + norm);
    }

    if (!isfinite(score) || score < 0.0)
        return 0.0;
    return score;
}
