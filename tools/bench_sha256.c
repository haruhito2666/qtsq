/*
 * bench_sha256.c — Phase 8 SHA-256 micro-benchmark.
 *
 * Measures the throughput of the QTSQ SHA-256 used for header.checksum on
 * both the compress and decompress paths of every checksummed lossless
 * stream. Compares three costs on the same buffer:
 *
 *   memcpy      : a bare memcpy of the buffer — the overhead the pre-Phase-8
 *                 one-shot qtsq_sha256 paid (it calloc'd a padded copy of the
 *                 whole input) on top of the compression.
 *   portable    : qtsq_sha256_portable — the new streaming implementation
 *                 with NO input copy (what Android / WASM use).
 *   fast        : qtsq_sha256 — the dispatch path (CommonCrypto hardware
 *                 SHA-256 on Apple, portable elsewhere).
 *
 * "before" (pre-Phase-8) ≈ memcpy + portable, since the old code did a full
 * calloc+memcpy and then the same SHA-256 compression as portable.
 *
 * Usage: ./bin/bench_sha256 [size_mb]   (default 256 MB)
 */
#include "qtsq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void qtsq_sha256_portable(const uint8_t *data, size_t len, uint8_t out[32]);

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    size_t mb = (argc > 1) ? (size_t)atol(argv[1]) : 256;
    if (mb == 0) mb = 256;
    size_t n = mb * 1024 * 1024;

    uint8_t *buf = (uint8_t *)malloc(n);
    uint8_t *dst = (uint8_t *)malloc(n);
    if (!buf || !dst) { printf("alloc failed\n"); return 1; }
    /* compressible-ish content, deterministic */
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        buf[i] = (uint8_t)((s >> 24) % 200);
    }

    int reps = 3;
    double t0, t1;
    /* Observable sink so -O3 can't delete the memcpy / hash work. */
    volatile uint64_t sink = 0;

    /* memcpy (the removed overhead) */
    double best_memcpy = 1e30;
    for (int r = 0; r < reps; r++) {
        t0 = now_sec();
        memcpy(dst, buf, n);
        sink ^= (uint64_t)dst[r] ^ (uint64_t)dst[n - 1 - r];
        t1 = now_sec();
        if (t1 - t0 < best_memcpy) best_memcpy = t1 - t0;
    }

    /* portable streaming (new non-Apple path) */
    uint8_t h_port[32];
    double best_port = 1e30;
    for (int r = 0; r < reps; r++) {
        t0 = now_sec();
        qtsq_sha256_portable(buf, n, h_port);
        sink ^= h_port[r];
        t1 = now_sec();
        if (t1 - t0 < best_port) best_port = t1 - t0;
    }

    /* fast dispatch (CommonCrypto on Apple) */
    uint8_t h_fast[32];
    double best_fast = 1e30;
    for (int r = 0; r < reps; r++) {
        t0 = now_sec();
        qtsq_sha256(buf, n, h_fast);
        sink ^= h_fast[r];
        t1 = now_sec();
        if (t1 - t0 < best_fast) best_fast = t1 - t0;
    }

    int agree = (memcmp(h_port, h_fast, 32) == 0);
    double mbs_memcpy = (double)n / (1024.0 * 1024.0) / best_memcpy;
    double mbs_port   = (double)n / (1024.0 * 1024.0) / best_port;
    double mbs_fast   = (double)n / (1024.0 * 1024.0) / best_fast;
    /* before ≈ memcpy + compression(portable) */
    double before_secs = best_memcpy + best_port;
    double mbs_before  = (double)n / (1024.0 * 1024.0) / before_secs;

    printf("SHA-256 throughput over %zu MB (best of %d)\n", mb, reps);
    printf("  memcpy alone            : %8.1f MB/s\n", mbs_memcpy);
    printf("  before (memcpy+compress): %8.1f MB/s\n", mbs_before);
    printf("  after  portable (no cp) : %8.1f MB/s  (%.2fx vs before)\n",
           mbs_port, mbs_before > 0 ? mbs_port / mbs_before : 0.0);
    printf("  after  fast (dispatch)  : %8.1f MB/s  (%.2fx vs before)\n",
           mbs_fast, mbs_before > 0 ? mbs_fast / mbs_before : 0.0);
    printf("  digests agree (fast==portable): %s\n", agree ? "YES" : "NO");

    free(buf);
    free(dst);
    return agree ? 0 : 1;
}
