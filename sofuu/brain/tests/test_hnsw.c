/*
 * test_hnsw.c — graph unit tests (W4.1/W4.2/W3.1)
 *
 * Author: Haruhito
 */
#include "sfb_hnsw_internal.h"
#include <math.h>

static int checks, fails;
#define CK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint64_t st = 88172645463325252ull;
static double rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17;
    return (double)(st >> 11) / 9007199254740992.0; }
static double gauss(void) { double a = rnd() + 1e-12, b = rnd();
    return sqrt(-2.0 * log(a)) * cos(2.0 * M_PI * b); }
static void norm(float *v, uint32_t dim) {
    double n = 0; for (uint32_t i = 0; i < dim; i++) n += (double)v[i] * v[i];
    n = sqrt(n); if (n > 1e-12) for (uint32_t i = 0; i < dim; i++) v[i] = (float)(v[i] / n);
}
/* clustered dataset: `clusters` centres, members = centre + sigma*noise */
typedef struct { float *data; uint32_t n, dim, clusters; float *centres; } set_t;
static void set_make(set_t *s, uint32_t n, uint32_t dim, uint32_t clusters) {
    s->n = n; s->dim = dim; s->clusters = clusters;
    s->data = malloc((size_t)n * dim * sizeof(float));
    s->centres = malloc((size_t)clusters * dim * sizeof(float));
    for (uint32_t c = 0; c < clusters; c++) {
        for (uint32_t d = 0; d < dim; d++) s->centres[(size_t)c * dim + d] = (float)gauss();
        norm(s->centres + (size_t)c * dim, dim);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)(rnd() * clusters) % clusters;
        float *v = s->data + (size_t)i * dim;
        for (uint32_t d = 0; d < dim; d++) v[d] = (float)(s->centres[(size_t)c * dim + d] + 0.02 * gauss());
        norm(v, dim);
    }
}
static void set_free(set_t *s) { free(s->data); free(s->centres); }

static void test_empty_and_bounds(void)
{
    sfb_hnsw_t *h = NULL;
    sfb_hnsw_hit_t hits[4]; uint32_t n = 99;
    CK(sfb_hnsw_init(&h, 64, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    CK(sfb_hnsw_count(h) == 0);
    float q[64] = {0};
    CK(sfb_hnsw_search(h, q, 4, 32, hits, &n) == SFB_OK);
    CK(n == 0);                                    /* empty graph: no hits */
    sfb_hnsw_free(h);

    h = NULL;
    CK(sfb_hnsw_init(&h, 7, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_DIM);
    CK(sfb_hnsw_init(&h, 4097, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_DIM);
    CK(sfb_hnsw_init(NULL, 64, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_INVALID);
    sfb_hnsw_free(NULL);                            /* must not crash */
}

static void test_self_query(void)
{
    set_t s; set_make(&s, 1500, 64, 15);
    sfb_hnsw_t *h = NULL;
    CK(sfb_hnsw_init(&h, 64, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    for (uint32_t i = 0; i < s.n; i++) {
        int rc = sfb_hnsw_add(h, i, s.data + (size_t)i * s.dim);
        if (rc != SFB_OK) { CK(!"add failed"); break; }
    }
    CK(sfb_hnsw_count(h) == s.n);

    uint32_t ok1 = 0, probe = 100;
    for (uint32_t q = 0; q < probe; q++) {
        uint32_t id = (uint32_t)(rnd() * s.n) % s.n, got = 0;
        sfb_hnsw_hit_t hits[10];
        CK(sfb_hnsw_search(h, s.data + (size_t)id * s.dim, 10, 64, hits, &got) == SFB_OK);
        if (got > 0 && hits[0].id == id) ok1++;
    }
    CK(ok1 == probe);                               /* exact self-query: 100% */

    /* a graph node id must be the next index, never a repeat or a gap */
    float v[64]; memcpy(v, s.data, sizeof v);
    CK(sfb_hnsw_add(h, 9999, v) == SFB_E_INVALID);
    CK(sfb_hnsw_add(h, 0, v) == SFB_E_INVALID);
    sfb_hnsw_free(h);
    set_free(&s);
}

static void test_deleted_traversal(void)
{
    set_t s; set_make(&s, 800, 64, 8);
    sfb_hnsw_t *h = NULL;
    CK(sfb_hnsw_init(&h, 64, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    for (uint32_t i = 0; i < s.n; i++) (void)sfb_hnsw_add(h, i, s.data + (size_t)i * s.dim);
    CK(sfb_hnsw_count(h) == s.n);

    /* delete every even node: search must never return one, but must still
     * find the odd ones (traversal goes through tombstones, not around them) */
    uint32_t ndel = 0;
    for (uint32_t i = 0; i < s.n; i += 2) { CK(sfb_hnsw_mark_deleted(h, i) == SFB_OK); ndel++; }
    CK(ndel == s.n / 2);

    uint32_t found = 0, bad = 0, got;
    for (uint32_t q = 0; q < 100; q++) {
        sfb_hnsw_hit_t hits[10];
        uint32_t id = (uint32_t)(rnd() * s.n) % s.n;
        got = 0;
        CK(sfb_hnsw_search(h, s.data + (size_t)id * s.dim, 10, 64, hits, &got) == SFB_OK);
        for (uint32_t k = 0; k < got; k++) { found++; if (hits[k].id % 2 == 0) bad++; }
    }
    CK(got > 0);
    CK(bad == 0);                                   /* deleted never emitted  */
    CK(found >= 900);                               /* still dense recall     */

    /* undelete one and it becomes visible again */
    uint32_t probe_id = 40;
    CK(sfb_hnsw_undelete(h, probe_id) == SFB_OK);
    sfb_hnsw_hit_t hits[10]; uint32_t n = 0;
    CK(sfb_hnsw_search(h, s.data + (size_t)probe_id * s.dim, 10, 64, hits, &n) == SFB_OK);
    CK(n > 0 && hits[0].id == probe_id);
    CK(sfb_hnsw_mark_deleted(h, 100000) == SFB_E_NOTFOUND);
    sfb_hnsw_free(h);
    set_free(&s);
}

static void test_save_load(void)
{
    set_t s; set_make(&s, 600, 48, 6);
    sfb_hnsw_t *h = NULL;
    CK(sfb_hnsw_init(&h, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    for (uint32_t i = 0; i < s.n; i++) (void)sfb_hnsw_add(h, i, s.data + (size_t)i * s.dim);
    for (uint32_t i = 0; i < s.n; i += 7) (void)sfb_hnsw_mark_deleted(h, i);

    const char *path = "/tmp/sfb_hnsw_test_graph.bin";
    (void)unlink(path);
    CK(sfb_hnsw_save(h, path, 3) == SFB_OK);

    sfb_hnsw_t *l = NULL;
    CK(sfb_hnsw_load(&l, path, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    CK(l != NULL);
    /* the graph file is topology only: vectors come from the caller */
    CK(l && sfb_hnsw_attach_vectors(l, s.data, s.n) == SFB_OK);
    CK(l && sfb_hnsw_attach_vectors(l, s.data, s.n - 1) == SFB_E_INVALID);   /* wrong brain */
    CK(l && sfb_hnsw_attach_vectors(l, NULL, s.n) == SFB_E_INVALID);
    if (l) {
        CK(sfb_hnsw_count(l) == sfb_hnsw_count(h));
        /* identical answers, not merely similar ones */
        uint32_t same = 0, tot = 0;
        for (uint32_t q = 0; q < 50; q++) {
            uint32_t id = (uint32_t)(rnd() * s.n) % s.n;
            sfb_hnsw_hit_t a[10], b[10]; uint32_t na = 0, nb = 0;
            CK(sfb_hnsw_search(h, s.data + (size_t)id * s.dim, 10, 64, a, &na) == SFB_OK);
            CK(sfb_hnsw_search(l, s.data + (size_t)id * s.dim, 10, 64, b, &nb) == SFB_OK);
            tot += (na < nb) ? na : nb;
            for (uint32_t k = 0; k < na && k < nb; k++) if (a[k].id == b[k].id) same++;
        }
        CK(tot > 0 && same == tot);
        sfb_hnsw_free(l);
    }

    /* a geometry mismatch must be refused, never silently accepted */
    sfb_hnsw_t *bad = NULL;
    CK(sfb_hnsw_load(&bad, path, 64, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_CORRUPT);
    CK(bad == NULL);
    CK(sfb_hnsw_load(&bad, "/tmp/definitely-not-here.bin", 48,
                     SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_NOTFOUND);

    /* flip one byte in the body: CRC must reject it */
    FILE *f = fopen(path, "r+b");
    CK(f != NULL);
    if (f) { (void)fseek(f, 200, SEEK_SET); (void)fputc(0x7F, f); fclose(f); }
    bad = NULL;
    CK(sfb_hnsw_load(&bad, path, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_CORRUPT);
    CK(bad == NULL);
    (void)unlink(path);

    sfb_hnsw_free(h);
    set_free(&s);
}


/* mmap load: same answers, corruption caught, and a mapped graph can still be
 * mutated in place (flags) or grown (detach) without touching the file. */
static void test_mmap_and_detach(void)
{
    set_t s; set_make(&s, 400, 48, 5);
    sfb_hnsw_t *h = NULL;
    CK(sfb_hnsw_init(&h, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    for (uint32_t i = 0; i < s.n; i++) (void)sfb_hnsw_add(h, i, s.data + (size_t)i * s.dim);

    const char *path = "/tmp/sfb_hnsw_test_mmap.bin";
    (void)unlink(path);
    CK(sfb_hnsw_save(h, path, 7) == SFB_OK);

    sfb_hnsw_t *m = NULL;
    CK(sfb_hnsw_map(&m, path, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_OK);
    CK(m != NULL && m->mapped == 1);
    if (m) {
        CK(sfb_hnsw_attach_vectors(m, s.data, s.n) == SFB_OK);
        /* identical results between the heap graph and the mapped one */
        uint32_t same = 0, tot = 0;
        for (uint32_t q = 0; q < 40; q++) {
            uint32_t id = (uint32_t)(rnd() * s.n) % s.n;
            sfb_hnsw_hit_t a[10], b[10]; uint32_t na = 0, nb = 0;
            CK(sfb_hnsw_search(h, s.data + (size_t)id * s.dim, 10, 64, a, &na) == SFB_OK);
            CK(sfb_hnsw_search(m, s.data + (size_t)id * s.dim, 10, 64, b, &nb) == SFB_OK);
            tot += na < nb ? na : nb;
            for (uint32_t k = 0; k < na && k < nb; k++) if (a[k].id == b[k].id) same++;
        }
        CK(tot > 0 && same == tot);

        /* a flag write on a mapped graph must not fault (copy-on-write) */
        CK(sfb_hnsw_mark_deleted(m, 5) == SFB_OK);
        CK((m->flags[5] & 1u) == 1u);
        CK(sfb_hnsw_undelete(m, 5) == SFB_OK);
        CK((m->flags[5] & 1u) == 0u);

        /* growing a mapped graph detaches it into the heap */
        float v[48];
        memcpy(v, s.data, sizeof v);
        CK(sfb_hnsw_add(m, s.n, v) == SFB_OK);
        CK(m->mapped == 0 && m->map_base == NULL);
        CK(sfb_hnsw_count(m) == s.n + 1);
        sfb_hnsw_free(m);
    }

    /* a flipped byte anywhere in the sections must be refused, not mapped */
    for (int trial = 0; trial < 3; trial++) {
        FILE *f = fopen(path, "r+b");
        CK(f != NULL);
        if (!f) break;
        long at = 300 + trial * 97;
        (void)fseek(f, at, SEEK_SET);
        int ch = fgetc(f);
        (void)fseek(f, at, SEEK_SET);
        (void)fputc(ch ^ 0xFF, f);
        fclose(f);

        sfb_hnsw_t *bad = NULL;
        CK(sfb_hnsw_map(&bad, path, 48, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC) == SFB_E_CORRUPT);
        CK(bad == NULL);

        f = fopen(path, "r+b");            /* restore */
        if (f) {
            (void)fseek(f, at, SEEK_SET);
            (void)fputc(ch, f);
            fclose(f);
        }
    }
    (void)unlink(path);
    sfb_hnsw_free(h);
    set_free(&s);
}

int main(void)
{
    printf("── sfb_hnsw tests\n");
    test_empty_and_bounds();  printf("  empty/bounds   done\n");
    test_self_query();        printf("  self-query     done\n");
    test_deleted_traversal(); printf("  deleted        done\n");
    test_save_load();         printf("  save/load      done\n");
    test_mmap_and_detach();   printf("  mmap/detach    done\n");
    printf("test_hnsw: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_HNSW FAILED\n"); return 1; }
    printf("=== TEST_HNSW PASSED ===\n");
    return 0;
}
