/*
 * chaos_worker.c — driver used by tests/chaos.sh
 *
 * Modes:
 *   ingest <brain> <dim> <n> <flush_every>   distinct records, flush periodically
 *   count  <brain> <dim>                     open, print live count, close
 *   nan    <brain> <dim>                     attempt a NaN insert, expect rejection
 *   stampede <brain> <dim> <n>               one writer attempt (for parallel runs)
 *   recall <brain> <dim> <queries>           recall loop, used for RSS/latency soak
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

static void fill(float *v, uint32_t dim, uint32_t seed) {
    /* deterministic pseudo-random unit-ish vector with a seed-derived cluster */
    uint32_t s = seed * 2654435761u + 12345u;
    for (uint32_t i = 0; i < dim; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = ((float)((s >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
    }
    double n = 0; for (uint32_t i = 0; i < dim; i++) n += v[i]*v[i];
    n = sqrt(n); if (n > 0) for (uint32_t i = 0; i < dim; i++) v[i] = (float)(v[i] / n);
}

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <mode> <brain> [dim] [n] [extra]\n", argv[0]); return 2; }
    const char *mode = argv[1], *path = argv[2];
    uint32_t dim = argc > 3 ? (uint32_t)atoi(argv[3]) : 768;
    uint32_t n = argc > 4 ? (uint32_t)atoi(argv[4]) : 100;
    uint32_t extra = argc > 5 ? (uint32_t)atoi(argv[5]) : 50;

    sfb_config_t cfg; sfb_config_default(&cfg, path, dim);
    cfg.mode = SFB_MODE_VECTOR_DB;      /* exact retention for durability checks */
    cfg.decay = SFB_DECAY_NONE;

    sfb_brain_t *b = NULL;
    int rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) {
        printf("{\"open_error\":%d,\"name\":\"%s\"}\n", rc, sfb_strerror(rc));
        return 3;
    }

    if (strcmp(mode, "count") == 0) {
        uint64_t c = 0; sfb_count(b, &c);
        printf("{\"count\":%llu}\n", (unsigned long long)c);
        sfb_close(b);
        return 0;
    }

    if (strcmp(mode, "nan") == 0) {
        float *v = calloc(dim, sizeof(float));
        for (uint32_t i = 0; i < dim; i++) v[i] = 0.1f;
        v[dim / 2] = NAN;
        sfb_remember_result_t r;
        int e = sfb_remember(b, v, "nan payload", "user", &r);
        printf("{\"nan_insert_rc\":%d,\"name\":\"%s\",\"still_alive\":1}\n", e, sfb_strerror(e));
        free(v);
        sfb_close(b);
        return e == SFB_E_NONFINITE ? 0 : 4;
    }

    if (strcmp(mode, "stampede") == 0) {
        float *v = calloc(dim, sizeof(float));
        char text[128];
        int ok = 0, locked = 0;
        for (uint32_t i = 0; i < n; i++) {
            fill(v, dim, (uint32_t)(getpid() * 100003 + i));
            snprintf(text, sizeof text, "writer %d record %u", (int)getpid(), i);
            sfb_remember_result_t r;
            int e = sfb_remember(b, v, text, "user", &r);
            if (e == SFB_OK) ok++;
            else if (e == SFB_E_LOCKED) locked++;
        }
        sfb_flush_result_t fr;
        int fe = sfb_flush(b, &fr);
        printf("{\"pid\":%d,\"inserted\":%d,\"locked\":%d,\"flush_rc\":%d}\n",
               (int)getpid(), ok, locked, fe);
        free(v);
        sfb_close(b);
        return 0;
    }

    if (strcmp(mode, "recall") == 0) {
        float *q = calloc(dim, sizeof(float));
        sfb_hit_t *hits = calloc(10, sizeof(sfb_hit_t));
        double t0 = now_ms();
        uint32_t got = 0, nq = n;
        for (uint32_t i = 0; i < nq; i++) {
            fill(q, dim, i + 7777u);
            uint32_t k = 0;
            if (sfb_recall(b, q, 10, hits, &k) == SFB_OK) got += k;
        }
        double dt = now_ms() - t0;
        printf("{\"queries\":%u,\"hits\":%u,\"total_ms\":%.1f,\"per_query_us\":%.1f}\n",
               nq, got, dt, nq ? dt * 1000.0 / nq : 0.0);
        free(q); free(hits);
        sfb_close(b);
        return 0;
    }

    /* ingest */
    float *v = calloc(dim, sizeof(float));
    char text[256];
    uint32_t flush_every = extra ? extra : 50;
    uint64_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        fill(v, dim, (uint32_t)(time(NULL) * 1000 + i));
        snprintf(text, sizeof text, "record %u %08x-%08x payload", i,
                 (unsigned)(time(NULL) & 0xffffffff), i * 2654435761u);
        sfb_remember_result_t r;
        int e = sfb_remember(b, v, text, "user", &r);
        if (e != SFB_OK) { printf("{\"insert_error\":%d,\"at\":%u}\n", e, i); break; }
        if ((i + 1) % flush_every == 0) {
            sfb_flush_result_t fr;
            int fe = sfb_flush(b, &fr);
            uint64_t live = 0;
            sfb_count(b, &live);
            last = live;
            printf("{\"flushed_at\":%u,\"rc\":%d,\"written\":%llu,\"count_now\":%llu}\n",
                   i + 1, fe, (unsigned long long)fr.records_written,
                   (unsigned long long)live);
            fflush(stdout);   /* unbuffered progress so a SIGKILL loses nothing */
        }
    }
    sfb_flush_result_t fr;
    int fe = sfb_flush(b, &fr);
    printf("{\"done\":true,\"flush_rc\":%d,\"written\":%llu,\"last\":%llu}\n",
           fe, (unsigned long long)fr.records_written, (unsigned long long)last);
    free(v);
    sfb_close(b);
    return 0;
}
