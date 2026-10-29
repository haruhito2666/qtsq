/*
 * sfb_hnsw_internal.h — shared layout for the HNSW graph.
 *
 * Split so that two files can implement one index:
 *   sfb_hnsw.c          init/free/add/search/mark_deleted/undelete/count
 *   sfb_hnsw_persist.c  save/load
 *
 * Author: Haruhito
 */
#ifndef SFB_HNSW_INTERNAL_H
#define SFB_HNSW_INTERNAL_H

#include "sfb_internal.h"

/* Neighbour sentinel. */
#define SFB_HNSW_NONE 0xFFFFFFFFu

struct sfb_hnsw {
    uint32_t dim;
    uint32_t m;                 /* links per node above level 0              */
    uint32_t m0;                /* links per node at level 0                 */
    uint32_t ef_construction;

    uint32_t n_nodes;           /* nodes added so far                        */
    uint32_t cap;               /* allocated node slots                      */

    float   *vecs;              /* cap * dim                                 */
    uint8_t *levels;            /* cap: top level of each node               */
    uint8_t *flags;             /* cap: bit0 deleted                         */

    /* Link rows are pooled, not strided: a node only owns rows for the levels
     * it actually has (average ~2 of 33). The strided version cost
     * n_nodes*33*m0*4 = 422 MB at 100k nodes, which alone busted the RSS budget.
     * `link_offsets[slot]` is an index into link_pool, or SFB_HNSW_NONE when the
     * node has no row at that level. Layout is flat, so the file can be mmap'd. */
    uint32_t *link_counts;      /* cap * (SFB_GRAPH_MAX_LEVEL+1)             */
    uint32_t *link_offsets;     /* cap * (SFB_GRAPH_MAX_LEVEL+1)             */
    uint32_t *link_pool;        /* pool_cap * m0                             */
    size_t    pool_len;         /* rows in use                               */
    size_t    pool_cap;         /* rows allocated                            */
    uint32_t *dummy_row;        /* m0 NONE entries: safe read for absent rows */

    uint32_t entry_point;       /* SFB_HNSW_NONE when empty                  */
    int32_t  max_level;         /* -1 when empty                             */

    /* set when the arrays above point into an mmap'd file rather than the heap */
    void    *map_base;
    size_t   map_len;
    int      mapped;
};

/* helpers shared by both files */
static inline uint32_t sfb_hnsw_slot(const sfb_hnsw_t *h, uint32_t id, uint32_t level)
{
    (void)h;   /* layout is id-major; h is only here for symmetry */
    return (id * (SFB_GRAPH_MAX_LEVEL + 1u)) + level;
}
static inline uint32_t *sfb_hnsw_links(sfb_hnsw_t *h, uint32_t id, uint32_t level)
{
    uint32_t off = h->link_offsets[sfb_hnsw_slot(h, id, level)];
    if (off == SFB_HNSW_NONE)
        return h->dummy_row;      /* absent row: every link reads as NONE */
    return h->link_pool + (size_t)off * h->m0;
}
static inline uint32_t *sfb_hnsw_counts(sfb_hnsw_t *h, uint32_t id, uint32_t level)
{
    return h->link_counts + sfb_hnsw_slot(h, id, level);
}
/* squared L2, total-order safe: non-finite results become FLT_MAX */
static inline float sfb_hnsw_l2(const float *a, const float *b, uint32_t dim)
{
    double acc = 0.0;
    for (uint32_t i = 0; i < dim; i++) {
        double d = (double)a[i] - (double)b[i];
        acc += d * d;
    }
    if (!(acc == acc) || acc > 1e37) return 3.4e38f;   /* NaN or overflow */
    return (float)acc;
}

int sfb_hnsw_grow(sfb_hnsw_t *h, uint32_t need);

#endif /* SFB_HNSW_INTERNAL_H */
