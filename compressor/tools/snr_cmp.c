/* Compare two 16-bit stereo/mono WAVs: align by cross-correlation, then
 * report global SNR of B relative to A (and per-channel). Used to measure
 * the reference MP3's SNR against the original WAV, and qtc decodes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t *rd_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1L << 30)) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int wav16_load(const char *path, int16_t **pcm, size_t *frames,
                      uint32_t *rate, int *ch)
{
    size_t len = 0;
    uint8_t *buf = rd_file(path, &len);
    if (!buf || len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return -1;
    }
    size_t off = 12;
    uint8_t *data = NULL;
    size_t dlen = 0;
    int bits = 0;
    while (off + 8 <= len) {
        uint32_t csz = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                       ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
        size_t body = off + 8;
        if (csz > len - body) csz = (uint32_t)(len - body);
        if (!memcmp(buf + off, "fmt ", 4) && csz >= 16) {
            *ch = buf[body + 2];
            *rate = (uint32_t)buf[body + 4] | ((uint32_t)buf[body + 5] << 8) |
                    ((uint32_t)buf[body + 6] << 16) | ((uint32_t)buf[body + 7] << 24);
            bits = buf[body + 14] | (buf[body + 15] << 8);
        } else if (!memcmp(buf + off, "data", 4)) {
            data = buf + body;
            dlen = csz;
        }
        off = body + csz + (csz & 1);
    }
    if (!data || bits != 16 || dlen == 0) { free(buf); return -1; }
    size_t tot = dlen / 2;
    int16_t *p = (int16_t *)malloc(tot * sizeof(int16_t));
    if (!p) { free(buf); return -1; }
    for (size_t i = 0; i < tot; i++)
        p[i] = (int16_t)((uint16_t)data[i * 2] | ((uint16_t)data[i * 2 + 1] << 8));
    free(buf);
    *pcm = p;
    *frames = tot / (size_t)*ch;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s ref.wav test.wav\n", argv[0]);
        return 2;
    }
    int16_t *a = NULL, *b = NULL;
    size_t na = 0, nb = 0;
    uint32_t ra = 0, rb = 0;
    int ca = 0, cb = 0;
    if (wav16_load(argv[1], &a, &na, &ra, &ca) != 0 ||
        wav16_load(argv[2], &b, &nb, &rb, &cb) != 0) {
        fprintf(stderr, "load failed\n");
        return 1;
    }
    if (ca != cb || ra != rb) {
        fprintf(stderr, "format mismatch: %dch@%u vs %dch@%u\n", ca, ra, cb, rb);
        return 1;
    }
    printf("ref: %zu frames, test: %zu frames, %d ch @ %u Hz\n", na, nb, ca, ra);

    /* Align: search offset d in [-8192, 8192] maximizing correlation of
     * channel 0. MP3 codec delay is typically ~1100-2200 samples. */
    size_t n = (na < nb ? na : nb);
    if (n > 400000) n = 400000;      /* enough to find the offset */
    double best = -1e300;
    int best_d = 0;
    for (int d = -8192; d <= 8192; d += 1) {
        /* test[i] vs ref[i + d] */
        double c = 0.0;
        size_t start = (d > 0) ? 0 : (size_t)(-d);
        size_t end = start + n - ((d > 0) ? (size_t)d : 0);
        if (end > (na < nb ? na : nb)) end = (na < nb ? na : nb);
        for (size_t i = start; i < end; i += 4) {   /* decimate for speed */
            size_t ja = (size_t)((int64_t)i + d);
            if (ja >= na || i >= nb) continue;
            c += (double)a[ja * ca] * (double)b[i * cb];
        }
        if (c > best) { best = c; best_d = d; }
    }
    printf("best alignment offset: %+d samples (ref index = test index + d)\n",
           best_d);

    /* SNR over the aligned overlap, both channels. */
    size_t usable = (na < nb ? na : nb);
    double sig = 0.0, noise = 0.0, sigc[8], noisec[8];
    for (int c = 0; c < ca; c++) sigc[c] = noisec[c] = 0.0;
    size_t counted = 0;
    for (size_t i = 0; i < usable; i++) {
        size_t ja = (size_t)((int64_t)i + best_d);
        if (ja >= na || i >= nb) continue;
        for (int c = 0; c < ca; c++) {
            double x = (double)a[ja * ca + c];
            double y = (double)b[i * cb + c];
            sig += x * x;
            noise += (x - y) * (x - y);
            sigc[c] += x * x;
            noisec[c] += (x - y) * (x - y);
        }
        counted++;
    }
    double snr = (noise > 0.0) ? 10.0 * log10(sig / noise) : 200.0;
    printf("compared %zu frames\n", counted);
    printf("global SNR: %.2f dB\n", snr);
    for (int c = 0; c < ca; c++)
        printf("  ch%d SNR: %.2f dB\n", c,
               (noisec[c] > 0.0) ? 10.0 * log10(sigc[c] / noisec[c]) : 200.0);
    free(a);
    free(b);
    return 0;
}
