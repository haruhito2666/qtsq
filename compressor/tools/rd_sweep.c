/* RD sweep for the v2 perceptual audio lens: encode + decode + SNR. */
#include "qtc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Minimal PCM-WAV loader (16-bit only, this harness). */
static int wav16_load(const char *path, uint8_t **pcm, size_t *pcm_len,
                      uint32_t *rate, uint8_t *ch)
{
    size_t len = 0;
    uint8_t *buf = rd_file(path, &len);
    if (!buf || len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return -1;
    }
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t csz = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                       ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
        size_t body = off + 8;
        if (csz > len - body) csz = (uint32_t)(len - body);
        if (!memcmp(buf + off, "fmt ", 4) && csz >= 16) {
            *ch = buf[body + 2];
            *rate = (uint32_t)buf[body + 4] | ((uint32_t)buf[body + 5] << 8) |
                    ((uint32_t)buf[body + 6] << 16) | ((uint32_t)buf[body + 7] << 24);
        } else if (!memcmp(buf + off, "data", 4)) {
            *pcm = (uint8_t *)malloc(csz);
            if (!*pcm) { free(buf); return -1; }
            memcpy(*pcm, buf + body, csz);
            *pcm_len = csz;
        }
        off = body + csz + (csz & 1);
    }
    free(buf);
    return (*pcm && *pcm_len) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s in.wav [kbps...]\n", argv[0]);
        return 2;
    }
    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    uint32_t rate = 0;
    uint8_t ch = 0;
    if (wav16_load(argv[1], &pcm, &pcm_len, &rate, &ch) != 0) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    uint32_t nframes = (uint32_t)(pcm_len / (2 * ch));
    double secs = (double)nframes / (double)rate;
    printf("signal: %u frames ch=%u @ %u Hz (%.2fs), %zu B raw\n",
           nframes, (unsigned)ch, rate, secs, pcm_len);
    printf("\npoint            bytes     kbps    ratio    SNR(dB)\n");

    double kbps_list[16];
    int nk = 0;
    for (int i = 2; i < argc && nk < 16; i++)
        kbps_list[nk++] = atof(argv[i]);

    for (int i = 0; i < nk; i++) {
        qtc_options_t opts;
        qtc_options_default(&opts);
        opts.mode = QTC_MODE_LOSSY;
        opts.quality = 0.5f;
        opts.target_kbps = (int)kbps_list[i];
        uint8_t *comp = NULL;
        size_t cl = 0;
        int rc = qtc_compress_audio(pcm, nframes, ch, 16, &comp, &cl, &opts);
        if (rc != QTC_OK) {
            printf("%6.0f kbps: encode failed (%d)\n", kbps_list[i], rc);
            continue;
        }
        uint8_t *dec = NULL;
        uint32_t dn = 0;
        uint8_t dch = 0, dbps = 0;
        rc = qtc_decompress_audio(comp, cl, &dec, &dn, &dch, &dbps);
        if (rc != QTC_OK || dn != nframes) {
            printf("%6.0f kbps: decode failed (%d)\n", kbps_list[i], rc);
            free(comp);
            continue;
        }
        double sig = 0.0, noise = 0.0;
        size_t tot = (size_t)dn * ch;
        for (size_t s = 0; s < tot; s++) {
            int16_t a = (int16_t)((uint16_t)pcm[s * 2] | ((uint16_t)pcm[s * 2 + 1] << 8));
            int16_t b = (int16_t)((uint16_t)dec[s * 2] | ((uint16_t)dec[s * 2 + 1] << 8));
            sig += (double)a * a;
            noise += (double)(a - b) * (a - b);
        }
        double snr = (noise > 0.0) ? 10.0 * log10(sig / noise) : 200.0;
        double kbps_act = (double)cl * 8.0 / secs / 1000.0;
        printf("%6.0f kbps  %8zu  %7.1f  %6.2fx  %7.2f\n",
               kbps_list[i], cl, kbps_act, (double)pcm_len / (double)cl, snr);
        free(comp);
        free(dec);
    }
    free(pcm);
    return 0;
}
