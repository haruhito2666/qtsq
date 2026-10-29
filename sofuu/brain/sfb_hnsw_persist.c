/*
 * sfb_hnsw_persist.c — graph persistence and mmap load (W3.1)
 *
 * Layout of graph-<gen>.bin, all sections 8-byte aligned:
 *
 *   header (sizeof(graph_header_t), 104 B today)
 *     u32 magic 'SFG4' | u32 version | u32 dim | u32 m | u32 m0 | u32 efc
 *     u32 entry_point  | i32 max_level
 *     u64 generation   | u64 n_nodes | u64 pool_len
 *     u64 off_levels   | u64 off_flags | u64 off_counts | u64 off_offsets
 *     u64 off_pool     | u32 crc32c(sections) | u32 reserved
 *   sections
 *     levels   n_nodes                 u8   top level per node
 *     flags    n_nodes                 u8   bit0 = deleted
 *     counts   n_nodes * (MAXLVL+1)    u32  links per (node, level)
 *     offsets  n_nodes * (MAXLVL+1)    u32  row index into pool, or 0xFFFFFFFF
 *     pool     pool_len * m0           u32  neighbour ids
 *
 * Topology only: vectors stay in the segments, so the brain never pays a second
 * copy of every vector on disk (disk amplification stays near 1.0x). They are
 * handed back after load with sfb_hnsw_attach_vectors().
 *
 * sfb_hnsw_map() does NOT copy: every array points straight into the mapping,
 * so opening a 100k-node brain costs one mmap and a CRC pass instead of a copy.
 *
 * Author: Haruhito
 */
#include "sfb_hnsw_internal.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SFB_GRAPH_FILE_MAGIC   0x34474653u   /* 'SFG4' */
#define SFB_GRAPH_FILE_VERSION 4u
/* header offsets are derived from sizeof(graph_header_t): a hard-coded
 * size silently overlapped the sections with the header. */

typedef struct {
    uint32_t magic, version, dim, m, m0, efc;
    uint32_t entry_point;
    int32_t  max_level;
    uint64_t generation, n_nodes, pool_len;
    uint64_t off_levels, off_flags, off_counts, off_offsets, off_pool;
    uint32_t crc;
    uint32_t reserved;
} graph_header_t;

static size_t align8(size_t v) { return (v + 7u) & ~(size_t)7u; }

static uint32_t crc_of_sections(const unsigned char *base, const graph_header_t *h)
{
    /* CRC covers every section byte in file order, so a flipped link or a
     * shifted offset cannot pass as a valid graph. */
    size_t nlev = SFB_GRAPH_MAX_LEVEL + 1u;
    uint32_t c = sfb_crc32c(base + h->off_levels, (size_t)h->n_nodes);
    c = sfb_crc32c_update(c, base + h->off_flags, (size_t)h->n_nodes);
    c = sfb_crc32c_update(c, base + h->off_counts, (size_t)h->n_nodes * nlev * 4u);
    c = sfb_crc32c_update(c, base + h->off_offsets, (size_t)h->n_nodes * nlev * 4u);
    c = sfb_crc32c_update(c, base + h->off_pool, (size_t)h->pool_len * h->m0 * 4u);
    return c;
}

int sfb_hnsw_save(const sfb_hnsw_t *h, const char *path, uint64_t generation)
{
    if (h == NULL || path == NULL)
        return SFB_E_INVALID;

    size_t nlev = SFB_GRAPH_MAX_LEVEL + 1u;
    graph_header_t hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = SFB_GRAPH_FILE_MAGIC;
    hdr.version = SFB_GRAPH_FILE_VERSION;
    hdr.dim = h->dim; hdr.m = h->m; hdr.m0 = h->m0; hdr.efc = h->ef_construction;
    hdr.entry_point = h->entry_point;
    hdr.max_level = h->max_level;
    hdr.generation = generation;
    hdr.n_nodes = h->n_nodes;
    hdr.pool_len = h->pool_len;

    size_t o = align8(sizeof hdr);
    hdr.off_levels = o;  o = align8(o + (size_t)h->n_nodes);
    hdr.off_flags  = o;  o = align8(o + (size_t)h->n_nodes);
    hdr.off_counts = o;  o = align8(o + (size_t)h->n_nodes * nlev * 4u);
    hdr.off_offsets = o; o = align8(o + (size_t)h->n_nodes * nlev * 4u);
    hdr.off_pool   = o;  o = align8(o + h->pool_len * (size_t)h->m0 * 4u);

    unsigned char *buf = (unsigned char *)calloc(1, o);
    if (buf == NULL)
        return SFB_E_NOMEM;

    if (h->n_nodes) {
        memcpy(buf + hdr.off_levels, h->levels, (size_t)h->n_nodes);
        memcpy(buf + hdr.off_flags, h->flags, (size_t)h->n_nodes);
        memcpy(buf + hdr.off_counts, h->link_counts, (size_t)h->n_nodes * nlev * 4u);
        memcpy(buf + hdr.off_offsets, h->link_offsets, (size_t)h->n_nodes * nlev * 4u);
    }
    if (h->pool_len)
        memcpy(buf + hdr.off_pool, h->link_pool, h->pool_len * (size_t)h->m0 * 4u);

    hdr.crc = crc_of_sections(buf, &hdr);
    memcpy(buf, &hdr, sizeof hdr);

    char tmp[4200];
    if (sfb_snprintf(tmp, sizeof tmp, "%s.tmp", path) < 0) { free(buf); return SFB_E_IO; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return SFB_E_IO; }
    int rc = sfb_write_all(fd, buf, o);
    if (rc == SFB_OK && fsync(fd) != 0) rc = SFB_E_IO;
    close(fd);
    free(buf);
    if (rc != SFB_OK) { unlink(tmp); return rc; }
    if (rename(tmp, path) != 0) { unlink(tmp); return SFB_E_IO; }
    return SFB_OK;
}

/* Shared validation + adoption, used by both the mmap and the copy loader. */
static int adopt(const unsigned char *base, size_t len, uint32_t dim,
                 uint32_t m, uint32_t m0, sfb_hnsw_t *h)
{
    graph_header_t hdr;
    size_t need;

    if (len < sizeof hdr)
        return SFB_E_CORRUPT;
    memcpy(&hdr, base, sizeof hdr);

    if (hdr.magic != SFB_GRAPH_FILE_MAGIC || hdr.version != SFB_GRAPH_FILE_VERSION)
        return SFB_E_CORRUPT;
    if (hdr.dim != dim || hdr.m != m || hdr.m0 != m0)
        return SFB_E_CORRUPT;
    if (hdr.n_nodes > 100000000ull || hdr.pool_len > 1000000000ull)
        return SFB_E_CORRUPT;

    need = (size_t)hdr.off_pool + (size_t)hdr.pool_len * hdr.m0 * 4u;
    if (need > len)
        return SFB_E_CORRUPT;
    if (hdr.off_levels < sizeof hdr || hdr.off_flags < hdr.off_levels ||
        hdr.off_counts < hdr.off_flags || hdr.off_offsets < hdr.off_counts ||
        hdr.off_pool < hdr.off_offsets)
        return SFB_E_CORRUPT;
    if (crc_of_sections(base, &hdr) != hdr.crc)
        return SFB_E_CORRUPT;

    h->dim = dim; h->m = m; h->m0 = m0;
    h->n_nodes = (uint32_t)hdr.n_nodes;
    h->cap = (uint32_t)hdr.n_nodes;
    h->levels = (uint8_t *)(uintptr_t)(base + hdr.off_levels);
    h->flags = (uint8_t *)(uintptr_t)(base + hdr.off_flags);
    h->link_counts = (uint32_t *)(uintptr_t)(base + hdr.off_counts);
    h->link_offsets = (uint32_t *)(uintptr_t)(base + hdr.off_offsets);
    h->link_pool = (uint32_t *)(uintptr_t)(base + hdr.off_pool);
    h->pool_len = (size_t)hdr.pool_len;
    h->pool_cap = (size_t)hdr.pool_len;
    h->entry_point = hdr.entry_point;
    h->max_level = hdr.max_level;

    h->dummy_row = (uint32_t *)malloc((size_t)m0 * sizeof(uint32_t));
    if (h->dummy_row == NULL)
        return SFB_E_NOMEM;
    for (uint32_t i = 0; i < m0; i++)
        h->dummy_row[i] = SFB_HNSW_NONE;
    return SFB_OK;
}

int sfb_hnsw_map(sfb_hnsw_t **out, const char *path, uint32_t dim,
                 uint32_t m, uint32_t m0, uint32_t ef_construction)
{
    struct stat st;
    void *base;
    sfb_hnsw_t *h;
    int fd, rc;

    if (out == NULL || path == NULL)
        return SFB_E_INVALID;
    *out = NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return SFB_E_NOTFOUND;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return SFB_E_CORRUPT; }

    /* MAP_PRIVATE + PROT_WRITE is copy-on-write: tombstone/undelete writes land
     * in private pages and never touch the file, so a mapped graph can take flag
     * updates without a full copy. Structural growth still detaches. */
    base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return SFB_E_IO;

    h = (sfb_hnsw_t *)calloc(1, sizeof *h);
    if (h == NULL) { munmap(base, (size_t)st.st_size); return SFB_E_NOMEM; }

    rc = adopt((const unsigned char *)base, (size_t)st.st_size, dim, m, m0, h);
    if (rc != SFB_OK) {
        munmap(base, (size_t)st.st_size);
        free(h->dummy_row);
        free(h);
        return rc;
    }
    h->ef_construction = ef_construction;
    h->vecs = NULL;
    h->mapped = 1;                 /* free() munmaps, never free()s the arrays */
    h->map_base = base;
    h->map_len = (size_t)st.st_size;
    *out = h;
    return SFB_OK;
}

int sfb_hnsw_load(sfb_hnsw_t **out, const char *path, uint32_t dim,
                  uint32_t m, uint32_t m0, uint32_t ef_construction)
{
    FILE *f;
    long sz;
    unsigned char *buf;
    sfb_hnsw_t *h;
    int rc;

    if (out == NULL || path == NULL)
        return SFB_E_INVALID;
    *out = NULL;

    f = fopen(path, "rb");
    if (f == NULL)
        return SFB_E_NOTFOUND;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return SFB_E_CORRUPT;
    }
    buf = (unsigned char *)malloc((size_t)sz);
    if (buf == NULL) { fclose(f); return SFB_E_NOMEM; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f);
        return SFB_E_CORRUPT;
    }
    fclose(f);

    h = (sfb_hnsw_t *)calloc(1, sizeof *h);
    if (h == NULL) { free(buf); return SFB_E_NOMEM; }

    rc = adopt(buf, (size_t)sz, dim, m, m0, h);
    if (rc != SFB_OK) { free(buf); free(h->dummy_row); free(h); return rc; }

    /* The arrays point into `buf`, which the graph now owns. map_base != NULL
     * with mapped == 0 tells free() to free() the buffer instead of munmap. */
    h->ef_construction = ef_construction;
    h->vecs = NULL;
    h->mapped = 0;
    h->map_base = buf;
    h->map_len = (size_t)sz;
    *out = h;
    return SFB_OK;
}

int sfb_hnsw_attach_vectors(sfb_hnsw_t *h, const float *vecs, uint32_t n)
{
    if (h == NULL || vecs == NULL)
        return SFB_E_INVALID;
    if (n != h->n_nodes)
        return SFB_E_INVALID;          /* wrong brain for this topology      */
    if (h->vecs != NULL)
        free(h->vecs);
    size_t bytes = (size_t)n * h->dim * sizeof(float);
    h->vecs = (float *)malloc(bytes ? bytes : 1);
    if (h->vecs == NULL)
        return SFB_E_NOMEM;
    memcpy(h->vecs, vecs, bytes);
    return SFB_OK;
}
