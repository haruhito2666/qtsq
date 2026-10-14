/*
 * qtc_detect.c — Phase 4: the Spaghettification router.
 *
 * Looks at the first QTC_DETECT_SAMPLE bytes of an input and classifies it
 * so qtc_compress(AUTO) can route to a domain lens. The router is
 * deliberately conservative:
 *
 *   - It never forces a lens. It only decides whether the AUDIO lens is
 *     added to the best-of-all contest (PCM detected) and whether the
 *     entropy lenses are skipped entirely (already-compressed formats).
 *   - A wrong guess can never break a round-trip: the contest emits only
 *     the smallest payload that beats the original SIZE, and each coder's
 *     round-trip correctness is established by construction and by the
 *     unit + fuzz suite (there is no per-call decode-verify pass — that
 *     was the wording bug fixed in the 2026-08-22 audit, L2).
 *
 * Signals:
 *   COMPRESSED — magic bytes of formats whose payload is already entropy
 *                coded (jpeg/png/gif/zip/gzip/mp3/...). Compressing those
 *                again cannot win, so we store them.
 *   AUDIO      — 16-bit PCM heuristic: consecutive little-endian samples
 *                change slowly (mean |delta| far below the random-walk
 *                expectation) AND the high bytes are concentrated around a
 *                few values (audio spends most of its time near silence or
 *                near a waveform's local range — unlike text or noise).
 *   TEXT       — mostly printable bytes (informational; LZ already handles
 *                text well, so no separate lens is routed on this).
 *   GENERIC    — everything else.
 */
#include "qtc_internal.h"

#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX 65536u

qtc_detect_t qtc_detect(const uint8_t *data, size_t len)
{
    qtc_detect_t r;
    r.type = QTC_TYPE_GENERIC;
    r.confidence = 0;

    if (!data || len == 0)
        return r;

    /* ── Magic bytes: already-compressed containers ─────────────────── */
    static const struct {
        uint8_t bytes[4];
        int     need;
    } magic[] = {
        {{0xFF, 0xD8, 0xFF, 0x00}, 3}, /* JPEG */
        {{0x89, 'P',  'N',  'G'},   4}, /* PNG  */
        {{'G',  'I',  'F',  '8'},   4}, /* GIF  */
        {{'P',  'K',  0x03, 0x04},  4}, /* ZIP/docx/... */
        {{0x1F, 0x8B, 0x00, 0x00},  2}, /* gzip */
        {{'B',  'Z',  'h',  0x00},  3}, /* bzip2 */
        {{0xFD, '7',  'z',  'X'},   4}, /* zstd */
        {{0x28, 0xB5, 0x2F, 0xFD},  4}, /* zstd (LE magic) */
        {{0x49, 0x44, 0x33, 0x00},  3}, /* ID3 (mp3 tag) */
        {{0xFF, 0xFB, 0x00, 0x00},  2}, /* mp3 frame */
        {{0xFF, 0xF3, 0x00, 0x00},  2}, /* mp3 frame */
        {{0xFF, 0xF2, 0x00, 0x00},  2}, /* mp3 frame */
        {{'O',  'g',  'g',  'S'},   4}, /* Ogg */
        {{'f',  'L',  'a',  'C'},   4}, /* FLAC */
        {{'R',  'I',  'F',  'F'},   4}, /* RIFF (wav/avi) */
        {{0x37, 0x7A, 0xBC, 0xAF},  4}, /* 7z */
        {{'x',  'z',  0x00, 0x00},  2}, /* xz */
        {{0x52, 0x61, 0x72, 0x21},  4}, /* RAR */
    };
    for (size_t i = 0; i < sizeof(magic) / sizeof(magic[0]); i++) {
        if (len >= (size_t)magic[i].need &&
            memcmp(data, magic[i].bytes, (size_t)magic[i].need) == 0) {
            r.type = QTC_TYPE_COMPRESSED;
            r.confidence = 100;
            return r;
        }
    }

    size_t n = len < SAMPLE_MAX ? len : SAMPLE_MAX;

    /* ── 16-bit PCM heuristic ─────────────────────────────────────────
     * Two independent signals, both must hold:
     *   smooth   — samples change slowly. We measure mean |delta| at
     *              stride 1 (mono interpretation) and stride 2 (stereo,
     *              same channel) and take the smaller: interleaved stereo
     *              has large cross-channel jumps at stride 1 but tiny
     *              within-channel steps at stride 2. Both are far below the
     *              ~21845 random-walk expectation and the ~5000 text level.
     *   hispread — the low bytes are broadly distributed, with a healthy
     *              fraction carrying the high bit. PCM low bytes are the
     *              low 8 bits of a waveform (~50% >= 128); text low bytes
     *              are ASCII (~0% >= 128), which rules prose out even when
     *              its 16-bit deltas happen to look smooth.
     */
    if (n >= 1024 && (n & 1) == 0) {
        size_t nsamp = n / 2;
        uint64_t sum1 = 0, sum2 = 0;
        size_t lo_high = 0;

        int32_t s0 = (int32_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
        int32_t s1 = (int32_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
        {
            int32_t d = s1 - s0;
            sum1 += (uint64_t)(d < 0 ? -d : d);
        }
        for (size_t i = 2; i < nsamp; i++) {
            int32_t cur = (int32_t)((uint16_t)data[2 * i] |
                                    ((uint16_t)data[2 * i + 1] << 8));
            int32_t d1 = cur - s1;          /* stride 1 */
            int32_t d2 = cur - s0;          /* stride 2 */
            sum1 += (uint64_t)(d1 < 0 ? -d1 : d1);
            sum2 += (uint64_t)(d2 < 0 ? -d2 : d2);
            s0 = s1;
            s1 = cur;
        }
        for (size_t i = 0; i < nsamp; i++)
            if (data[2 * i] >= 128)
                lo_high++;

        double mean1 = (double)sum1 / (double)(nsamp - 1);
        double mean2 = nsamp > 2 ? (double)sum2 / (double)(nsamp - 2) : mean1;
        double mean_delta = mean1 < mean2 ? mean1 : mean2;

        /* 8192 sits below the text/binary band (measured 8000–20000 on the
         * Canterbury + Silesia corpora) yet above real PCM (speech ~7000,
         * music lower, scans ~50). hispread is the hard gate that keeps
         * prose out regardless, so this threshold only avoids wasting the
         * audio lens on high-entropy 16-bit data it cannot beat. */
        int smooth = mean_delta < 8192.0;
        int hispread = (uint64_t)lo_high * 100 >= (uint64_t)nsamp * 15;

        if (smooth && hispread) {
            r.type = QTC_TYPE_AUDIO;
            /* Confidence scales with how far below the threshold the mean
             * delta sits. */
            int c = (int)(100.0 * (1.0 - mean_delta / 8192.0));
            r.confidence = c < 50 ? 50 : (c > 99 ? 99 : c);
            return r;
        }
    }

    /* ── Text heuristic (informational) ─────────────────────────────── */
    size_t printable = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if ((b >= 0x20 && b < 0x7F) || b == '\n' || b == '\r' || b == '\t')
            printable++;
    }
    if (printable * 10 >= n * 9) {
        r.type = QTC_TYPE_TEXT;
        r.confidence = (int)(printable * 100 / n);
        return r;
    }

    r.confidence = 0;
    return r;
}
