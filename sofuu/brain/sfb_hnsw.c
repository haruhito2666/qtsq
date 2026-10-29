/*
 * sfb_hnsw.c — HNSW graph core: insert, search, tombstones.
 *
 * Owns the non-persistent half of the index; sfb_hnsw_persist.c owns save/load.
 * No globals, no I/O, no SIMD, no OpenMP: every scratch buffer a query needs is
 * allocated per call, so concurrent readers share the graph safely.
 *
 * Determinism. Node levels are drawn from an xorshift32 seeded with
 * id*2654435761u+1, and every heap/result comparison breaks distance ties on
 * id, so the same vector set always produces the same graph and the same
 * ordering. Distances always go through sfb_hnsw_l2() (squared L2, total-order
 * safe), so a NaN or overflowing query degrades to a stable sort order instead
 * of corrupting the heaps.
 *
 * Level assignment follows the standard HNSW draw
 *   l = (uint32_t)(-log(u) * (1.0 / log((double)m)))
 * which gives a geometric decay of the level population.
 *
 * Author: Haruhito
 */
/* sfb_hnsw_internal.h is frozen ABI and not ours to edit, but its inline
 * sfb_hnsw_slot() does not read its `h` argument, so every translation unit
 * that includes it trips -Wunused-parameter. Scope the suppression to the
 * include only: this file's own code is still held to -Wall -Wextra. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "sfb_hnsw_internal.h"
#include <sys/mman.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <math.h>

/* flags bit0: node is deleted (still traversable, never emitted). */
#define SFB_HNSW_F_DELETED 0x01u

/* ── level assignment ──────────────────────────────────────────────────── */

static uint32_t sfb_hnsw_rng_next(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/*
 * Top level for node `id`: -log(u) * 1/log(m), with u in (0,1) drawn from a
 * per-node xorshift32 seeded from id alone (reproducible builds).
 */
static uint32_t sfb_hnsw_pick_level(uint32_t m, uint32_t id)
{
    uint32_t state = id * 2654435761u + 1u;
    uint32_t x;
    double u, mult, l;

    if (state == 0u)
        state = 0x9E3779B9u;
    x = sfb_hnsw_rng_next(&state);

    /* (x + 1) / (2^32 + 1) keeps u strictly inside (0,1): no log(0). */
    u = ((double)x + 1.0) / 4294967297.0;
    mult = (m > 1u) ? 1.0 / log((double)m) : 0.0;
    l = -log(u) * mult;

    if (!(l > 0.0))
        return 0u;
    if (l >= (double)SFB_GRAPH_MAX_LEVEL)
        return SFB_GRAPH_MAX_LEVEL;
    return (uint32_t)l;
}

/* ── total-order comparison ────────────────────────────────────────────── */

static int sfb_hnsw_hit_less(const sfb_hnsw_hit_t *a, const sfb_hnsw_hit_t *b)
{
    if (a->dist < b->dist)
        return 1;
    if (a->dist > b->dist)
        return 0;
    return a->id < b->id;
}

static int sfb_hnsw_cmp_hit(const void *pa, const void *pb)
{
    const sfb_hnsw_hit_t *a = (const sfb_hnsw_hit_t *)pa;
    const sfb_hnsw_hit_t *b = (const sfb_hnsw_hit_t *)pb;

    if (a->dist < b->dist)
        return -1;
    if (a->dist > b->dist)
        return 1;
    if (a->id < b->id)
        return -1;
    if (a->id > b->id)
        return 1;
    return 0;
}

static int sfb_hnsw_contains(const uint32_t *ids, uint32_t n, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        if (ids[i] == id)
            return 1;
    return 0;
}

static int sfb_hnsw_deleted(const sfb_hnsw_t *h, uint32_t id)
{
    return h->flags != NULL && (h->flags[id] & SFB_HNSW_F_DELETED) != 0u;
}

/* ── binary heaps over (dist, id) ──────────────────────────────────────── */

typedef struct sfb_hnsw_heap {
    sfb_hnsw_hit_t *a;
    size_t          n, cap;
    int             max_first;   /* 1: furthest on top, 0: closest on top */
} sfb_hnsw_heap_t;

/* Does x come out before y in this heap's priority direction? */
static int sfb_hnsw_before(const sfb_hnsw_heap_t *hp, const sfb_hnsw_hit_t *x,
                           const sfb_hnsw_hit_t *y)
{
    return hp->max_first ? sfb_hnsw_hit_less(y, x) : sfb_hnsw_hit_less(x, y);
}

static int sfb_hnsw_heap_init(sfb_hnsw_heap_t *hp, size_t cap, int max_first)
{
    if (cap < 4u)
        cap = 4u;
    hp->a = (sfb_hnsw_hit_t *)malloc(cap * sizeof *hp->a);
    if (hp->a == NULL)
        return SFB_E_NOMEM;
    hp->n = 0;
    hp->cap = cap;
    hp->max_first = max_first;
    return SFB_OK;
}

static void sfb_hnsw_heap_free(sfb_hnsw_heap_t *hp)
{
    free(hp->a);
    hp->a = NULL;
    hp->n = 0;
    hp->cap = 0;
}

static int sfb_hnsw_heap_push(sfb_hnsw_heap_t *hp, sfb_hnsw_hit_t v)
{
    size_t i;

    if (hp->n == hp->cap) {
        size_t ncap = hp->cap * 2u;
        sfb_hnsw_hit_t *na;

        if (ncap > SIZE_MAX / sizeof *na)
            return SFB_E_NOMEM;
        na = (sfb_hnsw_hit_t *)realloc(hp->a, ncap * sizeof *na);
        if (na == NULL)
            return SFB_E_NOMEM;
        hp->a = na;
        hp->cap = ncap;
    }

    i = hp->n++;
    while (i > 0u) {
        size_t p = (i - 1u) / 2u;

        if (!sfb_hnsw_before(hp, &v, &hp->a[p]))
            break;
        hp->a[i] = hp->a[p];
        i = p;
    }
    hp->a[i] = v;
    return SFB_OK;
}

static sfb_hnsw_hit_t sfb_hnsw_heap_pop(sfb_hnsw_heap_t *hp)
{
    sfb_hnsw_hit_t top = hp->a[0];
    size_t i;

    hp->n--;
    if (hp->n == 0u)
        return top;

    hp->a[0] = hp->a[hp->n];
    i = 0;
    for (;;) {
        size_t l = 2u * i + 1u;
        size_t r = l + 1u;
        size_t best = i;

        if (l < hp->n && sfb_hnsw_before(hp, &hp->a[l], &hp->a[best]))
            best = l;
        if (r < hp->n && sfb_hnsw_before(hp, &hp->a[r], &hp->a[best]))
            best = r;
        if (best == i)
            break;
        {
            sfb_hnsw_hit_t t = hp->a[i];

            hp->a[i] = hp->a[best];
            hp->a[best] = t;
        }
        i = best;
    }
    return top;
}

/* ── visited set (per call) ────────────────────────────────────────────── */

typedef struct sfb_hnsw_bits {
    uint64_t *w;
    size_t    nwords;
} sfb_hnsw_bits_t;

static int sfb_hnsw_bits_init(sfb_hnsw_bits_t *b, uint32_t n)
{
    b->nwords = ((size_t)n + 63u) / 64u;
    if (b->nwords == 0u)
        b->nwords = 1u;
    b->w = (uint64_t *)calloc(b->nwords, sizeof *b->w);
    if (b->w == NULL)
        return SFB_E_NOMEM;
    return SFB_OK;
}

static void sfb_hnsw_bits_free(sfb_hnsw_bits_t *b)
{
    free(b->w);
    b->w = NULL;
    b->nwords = 0;
}

/* 1 when `i` was already visited, 0 when this call marks it. */
static int sfb_hnsw_visited_take(sfb_hnsw_bits_t *b, uint32_t i)
{
    uint64_t mask = 1ull << (i & 63u);
    uint64_t *w = &b->w[i >> 6];

    if ((*w & mask) != 0u)
        return 1;
    *w |= mask;
    return 0;
}

/* ── one layer of the graph ────────────────────────────────────────────── */

/*
 * Best-first search confined to `level`, seeded with `seeds` (usually one).
 * A min-heap drives traversal, a max-heap keeps the `ef` closest results.
 * `skip_deleted` only suppresses emission: deleted nodes stay traversable so
 * tombstones never sever the graph. `out` must hold at least `ef` hits.
 */
static int sfb_hnsw_search_layer(const sfb_hnsw_t *h, const float *q, uint32_t level,
                                 const uint32_t *seeds, uint32_t n_seeds,
                                 uint32_t ef, int skip_deleted,
                                 sfb_hnsw_hit_t *out, uint32_t *n_out)
{
    sfb_hnsw_heap_t cand, res;
    sfb_hnsw_bits_t seen;
    uint32_t i, j;
    int rc;

    *n_out = 0u;
    if (ef < 1u)
        ef = 1u;
    if (h->n_nodes == 0u || h->vecs == NULL || h->link_offsets == NULL)
        return SFB_OK;

    rc = sfb_hnsw_heap_init(&cand, (size_t)ef + 8u, 0);
    if (rc != SFB_OK)
        return rc;
    rc = sfb_hnsw_heap_init(&res, (size_t)ef + 8u, 1);
    if (rc != SFB_OK) {
        sfb_hnsw_heap_free(&cand);
        return rc;
    }
    rc = sfb_hnsw_bits_init(&seen, h->n_nodes);
    if (rc != SFB_OK) {
        sfb_hnsw_heap_free(&cand);
        sfb_hnsw_heap_free(&res);
        return rc;
    }

    rc = SFB_OK;
    for (i = 0; i < n_seeds; i++) {
        sfb_hnsw_hit_t v;
        uint32_t s = seeds[i];

        if (s >= h->n_nodes || sfb_hnsw_visited_take(&seen, s))
            continue;
        v.id = s;
        v.dist = sfb_hnsw_l2(q, h->vecs + (size_t)s * h->dim, h->dim);
        rc = sfb_hnsw_heap_push(&cand, v);
        if (rc != SFB_OK)
            goto done;
        if (skip_deleted && sfb_hnsw_deleted(h, s))
            continue;
        rc = sfb_hnsw_heap_push(&res, v);
        if (rc != SFB_OK)
            goto done;
        if (res.n > ef)
            (void)sfb_hnsw_heap_pop(&res);
    }

    while (cand.n > 0u) {
        sfb_hnsw_hit_t c = sfb_hnsw_heap_pop(&cand);
        const uint32_t *links = sfb_hnsw_links((sfb_hnsw_t *)h, c.id, level);
        uint32_t cnt = *sfb_hnsw_counts((sfb_hnsw_t *)h, c.id, level);

        /* The node list is only worth expanding while it can beat the worst
         * result already kept (ef == 1 degenerates to hill climbing). */
        if (res.n >= ef && sfb_hnsw_hit_less(&res.a[0], &c))
            break;
        if (cnt > h->m0)
            cnt = h->m0;

        for (j = 0; j < cnt; j++) {
            sfb_hnsw_hit_t v;
            uint32_t nb = links[j];

            if (nb >= h->n_nodes || sfb_hnsw_visited_take(&seen, nb))
                continue;
            v.id = nb;
            v.dist = sfb_hnsw_l2(q, h->vecs + (size_t)nb * h->dim, h->dim);
            if (res.n >= ef && !sfb_hnsw_hit_less(&v, &res.a[0]))
                continue;
            rc = sfb_hnsw_heap_push(&cand, v);
            if (rc != SFB_OK)
                goto done;
            if (skip_deleted && sfb_hnsw_deleted(h, nb))
                continue;   /* traverse through it, never emit it */
            rc = sfb_hnsw_heap_push(&res, v);
            if (rc != SFB_OK)
                goto done;
            if (res.n > ef)
                (void)sfb_hnsw_heap_pop(&res);
        }
    }

    *n_out = (uint32_t)res.n;
    for (i = 0; i < res.n; i++)
        out[i] = res.a[i];

done:
    sfb_hnsw_heap_free(&cand);
    sfb_hnsw_heap_free(&res);
    sfb_hnsw_bits_free(&seen);
    return rc;
}

/* ── the diversity heuristic ───────────────────────────────────────────── */

/*
 * HNSW neighbour selection over `cand`, sorted by distance to the query point.
 * A candidate c is kept only when, for every already-kept neighbour r,
 * dist(c,q) < dist(c,r) — i.e. c covers a direction no closer pick already
 * covers. Selection stops at `mmax`; if the diversity pass kept fewer than
 * `mmax`, the remaining candidates are appended in distance order so a node
 * never ends up under-connected. `out` must hold `mmax` ids.
 */
static uint32_t sfb_hnsw_select_neighbours(const sfb_hnsw_t *h,
                                           sfb_hnsw_hit_t *cand, uint32_t ncand,
                                           uint32_t mmax, uint32_t *out)
{
    uint32_t i, j, nsel = 0u;

    if (mmax == 0u || ncand == 0u)
        return 0u;
    qsort(cand, (size_t)ncand, sizeof *cand, sfb_hnsw_cmp_hit);

    /* 1. diversity pass — keep c only if it beats every selected r. */
    for (i = 0; i < ncand && nsel < mmax; i++) {
        int keep = 1;

        if (cand[i].id >= h->n_nodes)
            continue;
        for (j = 0; j < nsel; j++) {
            float dr = sfb_hnsw_l2(h->vecs + (size_t)cand[i].id * h->dim,
                                   h->vecs + (size_t)out[j] * h->dim, h->dim);

            if (!(cand[i].dist < dr)) {
                keep = 0;
                break;
            }
        }
        if (keep)
            out[nsel++] = cand[i].id;
    }

    /* 2. fill up from whatever the diversity pass rejected. */
    for (i = 0; i < ncand && nsel < mmax; i++) {
        if (cand[i].id >= h->n_nodes)
            continue;
        if (sfb_hnsw_contains(out, nsel, cand[i].id))
            continue;
        out[nsel++] = cand[i].id;
    }
    return nsel;
}

/* ── link insertion ────────────────────────────────────────────────────── */

/*
 * Append dst to src's link list at `level`. When the list is full the same
 * heuristic picks the best `lim` out of (existing links + dst), measured
 * against src's own vector — diversity repair instead of blind eviction.
 * `scratch` holds lim+1 hits, `scratch_out` holds lim ids.
 */
static void sfb_hnsw_link_append(sfb_hnsw_t *h, uint32_t src, uint32_t dst,
                                 uint32_t level, uint32_t mmax,
                                 sfb_hnsw_hit_t *scratch, uint32_t *scratch_out)
{
    uint32_t *counts, *links, lim, i;

    if (src >= h->n_nodes || dst >= h->n_nodes || src == dst)
        return;
    counts = sfb_hnsw_counts(h, src, level);
    links = sfb_hnsw_links(h, src, level);
    lim = mmax < h->m0 ? mmax : h->m0;   /* one row holds at most m0 ids */
    if (lim == 0u)
        return;

    for (i = 0; i < *counts && i < lim; i++)
        if (links[i] == dst)
            return;                      /* already linked */

    if (*counts < lim) {
        links[*counts] = dst;
        *counts += 1u;
        return;
    }

    {
        const float *base = h->vecs + (size_t)src * h->dim;
        uint32_t nc = 0u, nsel;

        for (i = 0; i < *counts && i < lim; i++) {
            uint32_t nb = links[i];

            if (nb >= h->n_nodes || nb == dst)
                continue;
            scratch[nc].id = nb;
            scratch[nc].dist = sfb_hnsw_l2(base, h->vecs + (size_t)nb * h->dim, h->dim);
            nc++;
        }
        scratch[nc].id = dst;
        scratch[nc].dist = sfb_hnsw_l2(base, h->vecs + (size_t)dst * h->dim, h->dim);
        nc++;

        nsel = sfb_hnsw_select_neighbours(h, scratch, nc, lim, scratch_out);
        for (i = 0; i < nsel; i++)
            links[i] = scratch_out[i];
        *counts = nsel;
    }
}

/* ── growth ────────────────────────────────────────────────────────────── */

/* A loaded graph keeps its arrays inside one foreign block (an mmap or a single
 * read buffer). Those pointers cannot be realloc'd, so the first mutation copies
 * them onto the heap. Read-only opens therefore never pay for the copy. */
static int sfb_hnsw_detach(sfb_hnsw_t *h)
{
    size_t nlev = (size_t)SFB_GRAPH_MAX_LEVEL + 1u;
    size_t n = (size_t)h->n_nodes;
    uint8_t *levels = NULL, *flags = NULL;
    uint32_t *counts = NULL, *offsets = NULL, *pool = NULL;
    int rc = SFB_E_NOMEM;

    if (h->map_base == NULL)
        return SFB_OK;

    levels = (uint8_t *)malloc(n ? n : 1u);
    flags = (uint8_t *)malloc(n ? n : 1u);
    counts = (uint32_t *)malloc((n * nlev + 1u) * sizeof(uint32_t));
    offsets = (uint32_t *)malloc((n * nlev + 1u) * sizeof(uint32_t));
    pool = (uint32_t *)malloc((h->pool_len * (size_t)h->m0 + 1u) * sizeof(uint32_t));
    if (levels == NULL || flags == NULL || counts == NULL ||
        offsets == NULL || pool == NULL)
        goto out;

    if (n) {
        memcpy(levels, h->levels, n);
        memcpy(flags, h->flags, n);
        memcpy(counts, h->link_counts, n * nlev * sizeof(uint32_t));
        memcpy(offsets, h->link_offsets, n * nlev * sizeof(uint32_t));
    }
    if (h->pool_len)
        memcpy(pool, h->link_pool, h->pool_len * (size_t)h->m0 * sizeof(uint32_t));

    if (h->mapped)
        munmap(h->map_base, h->map_len);
    else
        free(h->map_base);

    h->levels = levels;
    h->flags = flags;
    h->link_counts = counts;
    h->link_offsets = offsets;
    h->link_pool = pool;
    h->pool_cap = h->pool_len;
    h->cap = h->n_nodes;
    h->map_base = NULL;
    h->map_len = 0u;
    h->mapped = 0;
    return SFB_OK;

out:
    free(levels); free(flags); free(counts); free(offsets); free(pool);
    return rc;
}

static int sfb_hnsw_ensure_capacity(sfb_hnsw_t *h, uint32_t need)
{
    uint32_t want;
    int drc;

    if (need <= h->cap)
        return SFB_OK;
    drc = sfb_hnsw_detach(h);          /* a mapped graph becomes mutable here */
    if (drc != SFB_OK)
        return drc;
    if (need <= h->cap)
        return SFB_OK;
    want = h->cap != 0u ? h->cap : 64u;      /* start at 64, then double */
    while (want < need) {
        if (want > UINT32_MAX / 2u) {
            want = need;
            break;
        }
        want *= 2u;
    }
    return sfb_hnsw_grow(h, want);
}

int sfb_hnsw_grow(sfb_hnsw_t *h, uint32_t need)
{
    uint32_t old, i;
    uint32_t lv, nlev = SFB_GRAPH_MAX_LEVEL + 1u;
    size_t   links_per_node;
    void    *p;

    if (h == NULL)
        return SFB_E_INVALID;
    if (need <= h->cap)
        return SFB_OK;
    if (h->dim == 0u || h->m0 == 0u)
        return SFB_E_INVALID;
    if ((size_t)need > SIZE_MAX / sizeof(float) / (size_t)h->dim)
        return SFB_E_NOMEM;
    links_per_node = (size_t)nlev * (size_t)h->m0;
    if ((size_t)need > SIZE_MAX / links_per_node / sizeof(uint32_t))
        return SFB_E_NOMEM;

    old = h->cap;

    p = realloc(h->vecs, (size_t)need * h->dim * sizeof(float));
    if (p == NULL)
        return SFB_E_NOMEM;
    h->vecs = (float *)p;

    p = realloc(h->levels, (size_t)need * sizeof(uint8_t));
    if (p == NULL)
        return SFB_E_NOMEM;
    h->levels = (uint8_t *)p;

    p = realloc(h->flags, (size_t)need * sizeof(uint8_t));
    if (p == NULL)
        return SFB_E_NOMEM;
    h->flags = (uint8_t *)p;

    p = realloc(h->link_counts, (size_t)need * (size_t)nlev * sizeof(uint32_t));
    if (p == NULL)
        return SFB_E_NOMEM;
    h->link_counts = (uint32_t *)p;

    p = realloc(h->link_offsets, (size_t)need * (size_t)nlev * sizeof(uint32_t));
    if (p == NULL)
        return SFB_E_NOMEM;
    h->link_offsets = (uint32_t *)p;

    memset(h->vecs + (size_t)old * h->dim, 0,
           (size_t)(need - old) * h->dim * sizeof(float));
    memset(h->levels + old, 0, (size_t)(need - old) * sizeof(uint8_t));
    memset(h->flags + old, 0, (size_t)(need - old) * sizeof(uint8_t));

    /* No pool rows yet: a node claims its rows in add(), only for the levels
     * it really has. Until then every row reads as "absent". */
    for (i = old; i < need; i++) {
        for (lv = 0; lv < nlev; lv++)
            h->link_offsets[sfb_hnsw_slot(h, i, lv)] = SFB_HNSW_NONE;
    }

    h->cap = need;
    return SFB_OK;
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

int sfb_hnsw_init(sfb_hnsw_t **out, uint32_t dim, uint32_t m, uint32_t m0,
                  uint32_t ef_construction)
{
    sfb_hnsw_t *h;

    if (out == NULL)
        return SFB_E_INVALID;
    *out = NULL;
    if (dim < 8u || dim > (uint32_t)SFB_MAX_DIM)
        return SFB_E_DIM;
    if (m == 0u || m0 == 0u)
        return SFB_E_INVALID;   /* both are link-row strides, never zero */

    h = (sfb_hnsw_t *)calloc(1u, sizeof *h);
    if (h == NULL)
        return SFB_E_NOMEM;

    h->dim = dim;
    h->m = m;
    h->m0 = m0;
    h->ef_construction = ef_construction;

    h->n_nodes = 0u;
    h->cap = 0u;

    /* nothing allocated until the first add */
    h->vecs = NULL;
    h->levels = NULL;
    h->flags = NULL;
    h->link_counts = NULL;
    h->link_offsets = NULL;
    h->link_pool = NULL;
    h->pool_len = 0u;
    h->pool_cap = 0u;
    h->mapped = 0;
    h->map_base = NULL;
    h->map_len = 0u;
    h->dummy_row = NULL;

    h->entry_point = SFB_HNSW_NONE;
    h->max_level = -1;

    *out = h;
    return SFB_OK;
}

void sfb_hnsw_free(sfb_hnsw_t *h)
{
    if (h == NULL)
        return;
    if (h->map_base != NULL) {
        /* mapped file -> munmap; copied file -> free the single backing block.
         * Either way the arrays point into it and must not be freed singly. */
        if (h->mapped)
            munmap(h->map_base, h->map_len);
        else
            free(h->map_base);
        free(h->dummy_row);
        free(h);
        return;
    }
    free(h->vecs);
    free(h->levels);
    free(h->flags);
    free(h->link_counts);
    free(h->link_offsets);
    free(h->link_pool);
    free(h->dummy_row);
    free(h);
}

/* ── insert ────────────────────────────────────────────────────────────── */

int sfb_hnsw_add(sfb_hnsw_t *h, uint32_t id, const float *vec)
{
    sfb_hnsw_hit_t *cand = NULL, *scratch = NULL;
    uint32_t *sel = NULL, *scratch_out = NULL;
    const float *q;
    uint32_t l, mmax = 0u, mmax_max, efc, i, nsel, ncand = 0u, ep;
    int32_t lc;
    int rc;

    if (h == NULL || vec == NULL)
        return SFB_E_INVALID;
    if (id != h->n_nodes)
        return SFB_E_INVALID;       /* append-only: add() assigns ids in order */
    if (id == UINT32_MAX)
        return SFB_E_CAPACITY;      /* id + 1 would wrap */
    if (h->dim == 0u)
        return SFB_E_INVALID;

    rc = sfb_hnsw_ensure_capacity(h, id + 1u);
    if (rc != SFB_OK)
        return rc;

    q = h->vecs + (size_t)id * h->dim;
    memcpy(h->vecs + (size_t)id * h->dim, vec, (size_t)h->dim * sizeof(float));

    l = sfb_hnsw_pick_level(h->m, id);
    h->levels[id] = (uint8_t)l;
    h->flags[id] = 0u;
    /* Claim one pooled row per level this node actually has (levels are fixed
     * at insertion, so an offset never moves once assigned). */
    if (h->dummy_row == NULL) {
        h->dummy_row = (uint32_t *)malloc((size_t)h->m0 * sizeof(uint32_t));
        if (h->dummy_row == NULL)
            return SFB_E_NOMEM;
        for (i = 0; i < h->m0; i++)
            h->dummy_row[i] = SFB_HNSW_NONE;
    }
    if (h->pool_len + (size_t)l + 1u > h->pool_cap) {
        size_t want = h->pool_cap ? h->pool_cap * 2u : 64u;
        uint32_t *np;
        while (want < h->pool_len + (size_t)l + 1u)
            want *= 2u;
        np = (uint32_t *)realloc(h->link_pool, want * (size_t)h->m0 * sizeof(uint32_t));
        if (np == NULL)
            return SFB_E_NOMEM;
        h->link_pool = np;
        h->pool_cap = want;
    }
    for (lc = 0; lc <= (int32_t)l; lc++) {
        uint32_t slot = sfb_hnsw_slot(h, id, (uint32_t)lc);
        uint32_t *lnk;

        h->link_offsets[slot] = (uint32_t)h->pool_len;
        h->link_counts[slot] = 0u;
        lnk = h->link_pool + (h->pool_len * (size_t)h->m0);
        for (i = 0; i < h->m0; i++)
            lnk[i] = SFB_HNSW_NONE;
        h->pool_len++;
    }

    if (h->n_nodes == 0u) {           /* first node: the whole graph is it */
        h->entry_point = id;
        h->max_level = (int32_t)l;
        h->n_nodes = id + 1u;
        return SFB_OK;
    }

    mmax_max = h->m > h->m0 ? h->m : h->m0;
    efc = h->ef_construction;
    if (efc < mmax_max)
        efc = mmax_max;

    /* Allocated before the slot becomes addressable, so an OOM here leaves the
     * graph exactly as it was and the same id can be retried. */
    cand = (sfb_hnsw_hit_t *)malloc((size_t)efc * sizeof *cand);
    sel = (uint32_t *)malloc(((size_t)mmax_max + 1u) * sizeof *sel);
    scratch = (sfb_hnsw_hit_t *)malloc(((size_t)mmax_max + 1u) * sizeof *scratch);
    scratch_out = (uint32_t *)malloc(((size_t)mmax_max + 1u) * sizeof *scratch_out);
    if (cand == NULL || sel == NULL || scratch == NULL || scratch_out == NULL) {
        rc = SFB_E_NOMEM;
        goto done;
    }

    h->n_nodes = id + 1u;             /* reverse links may name the new node */

    /* 1. greedy descent, ef = 1, from the top of the graph down to l+1. */
    ep = h->entry_point;
    for (lc = h->max_level; lc > (int32_t)l; lc--) {
        sfb_hnsw_hit_t g;
        uint32_t rn = 0u, seed = ep;

        g.id = SFB_HNSW_NONE;
        g.dist = 0.0f;
        rc = sfb_hnsw_search_layer(h, q, (uint32_t)lc, &seed, 1u, 1u, 0, &g, &rn);
        if (rc != SFB_OK)
            goto done;
        if (rn > 0u)
            ep = g.id;
    }

    /* 2. one beam search per layer from min(l, max_level) down to 0. */
    for (lc = (int32_t)l < h->max_level ? (int32_t)l : h->max_level; lc >= 0; lc--) {
        uint32_t lv = (uint32_t)lc;

        mmax = lv == 0u ? h->m0 : h->m;
        rc = sfb_hnsw_search_layer(h, q, lv, &ep, 1u, efc, 0, cand, &ncand);
        if (rc != SFB_OK)
            goto done;

        nsel = sfb_hnsw_select_neighbours(h, cand, ncand, mmax, sel);
        for (i = 0; i < nsel; i++) {
            sfb_hnsw_link_append(h, id, sel[i], lv, mmax, scratch, scratch_out);
            sfb_hnsw_link_append(h, sel[i], id, lv, mmax, scratch, scratch_out);
        }
        if (ncand > 0u)
            ep = cand[0].id;          /* select_neighbours sorted cand */
    }

    if ((int32_t)l > h->max_level) {
        h->entry_point = id;
        h->max_level = (int32_t)l;
    }
    rc = SFB_OK;

done:
    free(cand);
    free(sel);
    free(scratch);
    free(scratch_out);
    return rc;
}

/* ── query ─────────────────────────────────────────────────────────────── */

int sfb_hnsw_search(const sfb_hnsw_t *h, const float *query, uint32_t k,
                    uint32_t ef, sfb_hnsw_hit_t *out, uint32_t *n_out)
{
    sfb_hnsw_hit_t *tmp;
    uint32_t ep, ef0, rn = 0u, i;
    int32_t lc;
    int rc;

    if (n_out == NULL)
        return SFB_E_INVALID;
    *n_out = 0u;
    if (h == NULL || query == NULL)
        return SFB_E_INVALID;
    if (k == 0u || h->n_nodes == 0u || h->entry_point == SFB_HNSW_NONE ||
        h->vecs == NULL || h->link_offsets == NULL)
        return SFB_OK;
    if (out == NULL)
        return SFB_E_INVALID;
    ep = h->entry_point;
    if (ep >= h->n_nodes)
        return SFB_OK;

    /* greedy descent above level 0, ef = 1 */
    for (lc = h->max_level; lc >= 1; lc--) {
        sfb_hnsw_hit_t g;
        uint32_t seed = ep;

        g.id = SFB_HNSW_NONE;
        g.dist = 0.0f;
        rn = 0u;
        rc = sfb_hnsw_search_layer(h, query, (uint32_t)lc, &seed, 1u, 1u, 1, &g, &rn);
        if (rc != SFB_OK)
            return rc;
        if (rn > 0u)
            ep = g.id;
    }

    /* level 0 with a beam wide enough for both k and ef */
    ef0 = ef > k ? ef : k;
    tmp = (sfb_hnsw_hit_t *)malloc((size_t)ef0 * sizeof *tmp);
    if (tmp == NULL)
        return SFB_E_NOMEM;

    rc = sfb_hnsw_search_layer(h, query, 0u, &ep, 1u, ef0, 1, tmp, &rn);
    if (rc != SFB_OK) {
        free(tmp);
        return rc;
    }

    qsort(tmp, (size_t)rn, sizeof *tmp, sfb_hnsw_cmp_hit);
    if (rn > k)
        rn = k;
    for (i = 0; i < rn; i++)
        out[i] = tmp[i];
    *n_out = rn;
    free(tmp);
    return SFB_OK;
}

/* ── tombstones ────────────────────────────────────────────────────────── */

int sfb_hnsw_mark_deleted(sfb_hnsw_t *h, uint32_t id)
{
    if (h == NULL)
        return SFB_E_INVALID;
    if (id >= h->n_nodes || h->flags == NULL)
        return SFB_E_NOTFOUND;
    h->flags[id] |= SFB_HNSW_F_DELETED;
    return SFB_OK;
}

int sfb_hnsw_undelete(sfb_hnsw_t *h, uint32_t id)
{
    if (h == NULL)
        return SFB_E_INVALID;
    if (id >= h->n_nodes || h->flags == NULL)
        return SFB_E_NOTFOUND;
    h->flags[id] &= (uint8_t)~SFB_HNSW_F_DELETED;
    return SFB_OK;
}

uint64_t sfb_hnsw_count(const sfb_hnsw_t *h)
{
    return h != NULL ? (uint64_t)h->n_nodes : 0u;
}
