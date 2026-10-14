/*
 * QTSQ Multi-Core Parallelism Utilities
 * Shared threading infrastructure for compression & decompression
 * Author: Haruhito
 */

#ifndef QTSQ_PARALLEL_H
#define QTSQ_PARALLEL_H

#include <pthread.h>
#include <stdint.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

#define QTSQ_MAX_THREADS 16

static inline int qtsq_num_cores(void) {
    int n;
#ifdef __APPLE__
    int mib[2] = { CTL_HW, HW_NCPU };
    size_t len = sizeof(n);
    if (sysctl(mib, 2, &n, &len, NULL, 0) != 0) n = 4;
#else
    n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
#endif
    if (n > QTSQ_MAX_THREADS) n = QTSQ_MAX_THREADS;
    return n;
}

/* Helper: dispatch work across threads. Thread 0 runs on caller. */
static inline void qtsq_dispatch(void *(*fn)(void *), void *args, size_t arg_stride, int num_threads) {
    /* Clamp to the fixed tids[] / per-thread arg array size — a caller
     * passing > QTSQ_MAX_THREADS would index tids[] out of bounds. */
    if (num_threads > QTSQ_MAX_THREADS) num_threads = QTSQ_MAX_THREADS;
    if (num_threads < 1) num_threads = 1;
    pthread_t tids[QTSQ_MAX_THREADS];
    for (int t = 1; t < num_threads; t++)
        pthread_create(&tids[t], NULL, fn, (char *)args + t * arg_stride);
    fn(args); /* thread 0 on caller */
    for (int t = 1; t < num_threads; t++)
        pthread_join(tids[t], NULL);
}

#endif /* QTSQ_PARALLEL_H */
