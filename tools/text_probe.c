/*
 * text_probe — measure where the qtc text loss lives.
 *
 * For each input file, prints:
 *   raw size
 *   zlib-9 on raw text
 *   qtc (generic contest) on raw text
 *   qtsq_compress_text with codec=ZLIB (horizon pack + zlib-9)
 *   qtsq_compress_text with codec=QTC  (horizon pack + qtc)
 *
 * Bounded: refuses inputs larger than 64 MB.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include "qtsq.h"
#include "qtc.h"

#define PROBE_MAX_INPUT (64ULL * 1024 * 1024)

static uint8_t *load(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || (uint64_t)sz > PROBE_MAX_INPUT) { fclose(fp); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: text_probe <file>...\n");
        return 1;
    }
    printf("%-16s %10s %10s %10s %12s %12s\n",
           "file", "raw", "zlib9-raw", "qtc-raw", "qtsq-zlib", "qtsq-qtc");
    for (int a = 1; a < argc; a++) {
        size_t n = 0;
        uint8_t *data = load(argv[a], &n);
        if (!data) {
            printf("%-16s [unreadable or >64MB]\n", argv[a]);
            continue;
        }

        /* zlib-9 on raw */
        uLongf zbound = compressBound((uLong)n);
        uint8_t *zbuf = (uint8_t *)malloc(zbound);
        uLongf zlen = zbound;
        compress2(zbuf, &zlen, data, (uLong)n, Z_BEST_COMPRESSION);

        /* qtc on raw */
        uint8_t *qbuf = NULL;
        size_t qlen = 0;
        qtc_compress(data, n, &qbuf, &qlen, NULL);

        /* QTSQ horizon path, zlib default */
        qtsq_context_t cz;
        qtsq_init(&cz);
        int rcz = qtsq_compress_text(&cz, (const char *)data, n);

        /* QTSQ horizon path, qtc opt-in */
        qtsq_context_t cq;
        qtsq_init(&cq);
        cq.codec = QTSQ_CODEC_QTC;
        int rcq = qtsq_compress_text(&cq, (const char *)data, n);

        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        printf("%-16s %10zu %10lu %10zu %12llu %12llu\n",
               base, n, (unsigned long)zlen, qlen,
               rcz == QTSQ_OK ? (unsigned long long)cz.singularity_size : 0ULL,
               rcq == QTSQ_OK ? (unsigned long long)cq.singularity_size : 0ULL);

        free(zbuf);
        free(qbuf);
        qtsq_free(&cz);
        qtsq_free(&cq);
        free(data);
    }
    return 0;
}
