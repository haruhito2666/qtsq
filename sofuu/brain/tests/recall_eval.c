/*
 * recall_eval.c — ground-truth recall harness for sfb_hnsw (W4.5)
 *
 * Generates clustered vectors (the shape that exposed the 44% cliff), builds
 * the graph, and measures recall@1/10/50 against brute-force search on the
 * SAME vectors. Honest numbers only: no ef inflation, no query reuse of
 * stored points unless intended.
 *
 * usage: recall_eval <n> <dim> <queries> <seed> [ef] [clusters]
 * output: one JSON line on stdout
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb_internal.h"
#include <math.h>
#include <time.h>

static uint64_t rng_state;
static double rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;   /* [0,1) */
}
static double gauss(void) {
    double u1 = rnd() + 1e-12, u2 = rnd();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
static void normalise(float *v, uint32_t dim) {
    double n = 0; for (uint32_t i = 0; i < dim; i++) n += (double)v[i] * v[i];
    n = sqrt(n);
    if (n > 1e-12) for (uint32_t i = 0; i < dim; i++) v[i] = (float)(v[i] / n);
}
static void make_cluster(float *centre, uint32_t dim) {
    for (uint32_t i = 0; i < dim; i++) centre[i] = (float)gauss();
    normalise(centre, dim);
}
static void make_member(const float *centre, float *out, uint32_t dim, double sigma) {
    for (uint32_t i = 0; i < dim; i++) out[i] = (float)(centre[i] + sigma * gauss());
    normalise(out, dim);
}
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* brute force: returns k smallest (dist,id) pairs */
typedef struct { float d; uint32_t id; } pair_t;
static int cmp_pair(const void *a, const void *b) {
    const pair_t *x = a, *y = b;
    if (x->d < y->d) return -1;
    if (x->d > y->d) return 1;
    return (x->id > y->id) - (x->id < y->id);
}

int main(int argc, char **argv) {
    uint32_t n        = argc > 1 ? (uint32_t)atoi(argv[1]) : 25000;
    uint32_t dim      = argc > 2 ? (uint32_t)atoi(argv[2]) : 768;
    uint32_t queries  = argc > 3 ? (uint32_t)atoi(argv[3]) : 100;
    uint64_t seed     = argc > 4 ? strtoull(argv[4], NULL, 10) : 42;
    uint32_t ef       = argc > 5 ? (uint32_t)atoi(argv[5]) : 64;
    uint32_t clusters = argc > 6 ? (uint32_t)atoi(argv[6]) : (n > 1000 ? n / 100 : 10);
    const double sigma = 0.02;

    if (n == 0 || dim < 8 || dim > 4096) { fprintf(stderr, "bad args\n"); return 2; }
    rng_state = seed ? seed : 1;

    float *centres = malloc((size_t)clusters * dim * sizeof(float));
    float *vecs    = malloc((size_t)n * dim * sizeof(float));
    uint32_t *owner = malloc((size_t)n * sizeof(uint32_t));
    float *vec     = malloc((size_t)dim * sizeof(float));
    if (!centres || !vecs || !owner || !vec) { fprintf(stderr, "oom\n"); return 2; }

    for (uint32_t c = 0; c < clusters; c++) make_cluster(centres + (size_t)c * dim, dim);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)(rnd() * clusters) % clusters;
        owner[i] = c;
        make_member(centres + (size_t)c * dim, vecs + (size_t)i * dim, dim, sigma);
    }

    sfb_hnsw_t *h = NULL;
    double t0 = now_ms();
    int rc = sfb_hnsw_init(&h, dim, SFB_DEFAULT_M, SFB_DEFAULT_M0, SFB_DEFAULT_EFC);
    if (rc != SFB_OK) { printf("{\"error\":\"hnsw_init_%d\"}\n", rc); return 3; }
    for (uint32_t i = 0; i < n; i++) {
        rc = sfb_hnsw_add(h, i, vecs + (size_t)i * dim);
        if (rc != SFB_OK) { printf("{\"error\":\"hnsw_add_%d_at_%u\"}\n", rc, i); return 3; }
    }
    double build_ms = now_ms() - t0;

    pair_t   *bf   = malloc((size_t)n * sizeof(pair_t));
    double   *lat  = malloc((size_t)queries * sizeof(double));
    sfb_hnsw_hit_t hits[64];
    double sum1 = 0, sum10 = 0, sum50 = 0;
    uint32_t k10 = 0;

    for (uint32_t q = 0; q < queries; q++) {
        uint32_t c = (uint32_t)(rnd() * clusters) % clusters;
        make_member(centres + (size_t)c * dim, vec, dim, sigma);

        for (uint32_t i = 0; i < n; i++) {
            const float *v = vecs + (size_t)i * dim;
            float d = 0;
            for (uint32_t j = 0; j < dim; j++) { float t = v[j] - vec[j]; d += t * t; }
            bf[i].d = d; bf[i].id = i;
        }
        qsort(bf, n, sizeof(pair_t), cmp_pair);

        double t1 = now_ms();
        uint32_t got = 0;
        sfb_hnsw_search(h, vec, 10, ef, hits, &got);
        lat[q] = now_ms() - t1;
        if (got == 0) continue;

        uint32_t hit1 = 0, hit10 = 0, hit50 = 0;
        sfb_hnsw_hit_t wide[64];
        uint32_t nwide = 0;
        sfb_hnsw_search(h, vec, 50, ef, wide, &nwide);
        for (uint32_t k = 0; k < got && k < 10; k++) {
            for (uint32_t j = 0; j < 10; j++) if (hits[k].id == bf[j].id) { hit10++; break; }
            if (hits[k].id == bf[0].id) hit1 = 1;
        }
        for (uint32_t k = 0; k < nwide && k < 50; k++)
            for (uint32_t j = 0; j < 50; j++) if (wide[k].id == bf[j].id) { hit50++; break; }
        sum1 += hit1;
        sum10 += (double)hit10 / 10.0;
        sum50 += (double)hit50 / 50.0;
        k10++;
    }

    qsort(lat, queries, sizeof(double), cmp_double);
    printf("{\"n\":%u,\"dim\":%u,\"queries\":%u,\"ef\":%u,\"clusters\":%u,"
           "\"recall@1\":%.4f,\"recall@10\":%.4f,\"recall@50\":%.4f,"
           "\"build_ms\":%.0f,\"p50_us\":%.0f,\"p99_us\":%.0f}\n",
           n, dim, k10 ? k10 : 1, ef, clusters,
           k10 ? sum1 / k10 : 0.0, k10 ? sum10 / k10 : 0.0, k10 ? sum50 / k10 : 0.0,
           build_ms,
           queries ? lat[queries / 2] * 1000.0 : 0.0,
           queries ? lat[(queries * 99) / 100] * 1000.0 : 0.0);

    sfb_hnsw_free(h);
    free(centres); free(vecs); free(owner); free(vec); free(bf); free(lat);
    return 0;
}
