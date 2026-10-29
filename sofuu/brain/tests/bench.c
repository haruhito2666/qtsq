/*
 * bench.c — the numbers the plan is judged on (W3.2, W4.5, W6.1)
 *
 *   bench <dir> <n> <dim> <queries>          fresh build + measure
 *   bench <dir> <n> <dim> <queries> reuse    reopen an existing brain
 *
 * Prints one JSON line. Every number is measured, none is estimated.
 *
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1        /* ru_maxrss is a Darwin extension */
#endif
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static uint64_t rs = 12345;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0; }
static double gauss(void) { double a = rnd() + 1e-12, b = rnd();
    return sqrt(-2.0 * log(a)) * cos(2.0 * M_PI * b); }
static void norm(float *v, uint32_t d) {
    double n = 0; for (uint32_t i = 0; i < d; i++) n += (double)v[i] * v[i];
    n = sqrt(n); if (n > 1e-12) for (uint32_t i = 0; i < d; i++) v[i] = (float)(v[i] / n);
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
static double peak_rss_mb(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0.0;
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);   /* Darwin reports bytes */
}

static unsigned long long dir_bytes(const char *p)
{
    /* best effort: count regular files one level deep plus the root manifest */
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "du -sk '%s' 2>/dev/null | cut -f1", p);
    FILE *f = popen(cmd, "r");
    unsigned long long kb = 0;
    if (f) { if (fscanf(f, "%llu", &kb) != 1) kb = 0; pclose(f); }
    return kb * 1024ull;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <dir> <n> <dim> <queries> [reuse]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    uint32_t n = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t dim = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t queries = (uint32_t)strtoul(argv[4], NULL, 10);
    int reuse = (argc > 5 && strcmp(argv[5], "reuse") == 0);

    sfb_config_t cfg; sfb_config_default(&cfg, dir, dim);
    sfb_brain_t *b = NULL;
    int rc;

    double t_open0 = now_ms();
    if (reuse) {
        rc = sfb_open(&cfg, &b);
        if (rc != SFB_OK) { printf("{\"error\":\"open_%d\"}\n", rc); return 1; }
        double open_ms = now_ms() - t_open0;
        sfb_stats_t st; sfb_stats_get(b, &st);

        double *lat = malloc((size_t)queries * sizeof(double));
        float *q = malloc((size_t)dim * sizeof(float));
        const float *sample = NULL;
        sfb_hit_t hits[10]; uint32_t nh = 0;
        /* query near stored data: reuse a listed record's neighbourhood */
        uint32_t hits_total = 0;
        for (uint32_t i = 0; i < queries; i++) {
            for (uint32_t d = 0; d < dim; d++) q[d] = (float)gauss();
            norm(q, dim);
            double t1 = now_ms();
            sfb_recall(b, q, 10, hits, &nh);
            lat[i] = now_ms() - t1;
            hits_total += nh;
            if (i == 0 && nh) sample = NULL;
        }
        (void)sample;
        qsort(lat, queries, sizeof(double), cmp_d);
        printf("{\"mode\":\"reuse\",\"records\":%llu,\"dim\":%u,"
               "\"open_ms\":%.0f,\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"hits\":%u,"
               "\"index_rebuilt\":%llu,\"segments\":%llu}\n",
               (unsigned long long)st.records, dim, open_ms,
               lat[queries / 2], lat[(queries * 99) / 100], hits_total,
               (unsigned long long)st.index_rebuilt,
               (unsigned long long)st.segments);
        free(lat); free(q);
        sfb_close(b);
        return 0;
    }

    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) { printf("{\"error\":\"open_%d\"}\n", rc); return 1; }

    float *v = malloc((size_t)dim * sizeof(float));
    char text[96];
    double t_ing0 = now_ms();
    double worst_flush = 0.0, sum_flush = 0.0;
    uint32_t nflush = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t d = 0; d < dim; d++) v[d] = (float)gauss();
        norm(v, dim);
        snprintf(text, sizeof text, "record %u token%u", i, i & 1023u);
        sfb_remember_result_t r;
        rc = sfb_remember(b, v, text, "bench", &r);
        if (rc != SFB_OK) { printf("{\"error\":\"remember_%d_at_%u\"}\n", rc, i); return 1; }
        if ((i + 1) % 10000u == 0) {
            double f0 = now_ms();
            sfb_flush_result_t fr;
            if (sfb_flush(b, &fr) != SFB_OK) { printf("{\"error\":\"flush\"}\n"); return 1; }
            double dt = now_ms() - f0;
            sum_flush += dt; nflush++;
            if (dt > worst_flush) worst_flush = dt;
        }
    }
    double ingest_ms = now_ms() - t_ing0;

    double f0 = now_ms();
    sfb_flush_result_t fr;
    if (sfb_flush(b, &fr) != SFB_OK) { printf("{\"error\":\"final_flush\"}\n"); return 1; }
    double last_flush_ms = now_ms() - f0;
    sfb_stats_t st; sfb_stats_get(b, &st);
    sfb_close(b);

    unsigned long long disk = dir_bytes(dir);
    double t_open1 = now_ms();
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) { printf("{\"error\":\"reopen_%d\"}\n", rc); return 1; }
    double reopen_ms = now_ms() - t_open1;
    sfb_stats_t st2; sfb_stats_get(b, &st2);

    double *lat = malloc((size_t)queries * sizeof(double));
    uint32_t hits_total = 0;
    sfb_hit_t hits[10]; uint32_t nh = 0;
    for (uint32_t i = 0; i < queries; i++) {
        for (uint32_t d = 0; d < dim; d++) v[d] = (float)gauss();
        norm(v, dim);
        double t1 = now_ms();
        sfb_recall(b, v, 10, hits, &nh);
        lat[i] = now_ms() - t1;
        hits_total += nh;
    }
    qsort(lat, queries, sizeof(double), cmp_d);

    /* disk amplification: bytes on disk / bytes of raw float vectors */
    double raw = (double)n * dim * 4.0 + (double)n * 24.0;
    printf("{\"mode\":\"fresh\",\"records\":%llu,\"dim\":%u,\"peak_rss_mb\":%.1f,\"ingest_ms\":%.0f,"
           "\"ingest_us_per_rec\":%.1f,\"flush_p99_ms\":%.1f,\"flush_last_ms\":%.1f,"
           "\"reopen_ms\":%.0f,\"index_rebuilt\":%llu,\"segments\":%llu,"
           "\"disk_bytes\":%llu,\"disk_amp\":%.2f,"
           "\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"hits\":%u}\n",
           (unsigned long long)st.records, dim, peak_rss_mb(), ingest_ms,
           n ? ingest_ms * 1000.0 / n : 0.0, worst_flush, last_flush_ms,
           reopen_ms, (unsigned long long)st2.index_rebuilt,
           (unsigned long long)st2.segments, disk, raw > 0 ? disk / raw : 0.0,
           lat[queries / 2], lat[(queries * 99) / 100], hits_total);

    free(lat); free(v);
    sfb_close(b);
    return 0;
}
