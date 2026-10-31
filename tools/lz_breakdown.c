/*
 * lz_breakdown — parse a qtc LZ/LZ2 payload (single-block inputs) and
 * print where the bytes go: header, model bitmaps, freq tables,
 * bitstreams. Defaults to the LZ2 lens; pass --v1 for the legacy LZ.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "qtc.h"

#define LITLEN_ALPHA 512
#define BUCKET_ALPHA 21

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t popcount_bytes(const uint8_t *bm, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        for (int j = 0; j < 8; j++)
            if (bm[i] & (1u << j)) c++;
    return c;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int v1 = (argc > 2 && strcmp(argv[2], "--v1") == 0);
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > (64L * 1024 * 1024)) { fclose(fp); return 1; }
    uint8_t *in = (uint8_t *)malloc((size_t)sz);
    if (!in || fread(in, 1, (size_t)sz, fp) != (size_t)sz) {
        free(in); fclose(fp); return 1;
    }
    fclose(fp);

    uint8_t *out = NULL;
    size_t out_len = 0;
    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.force_lens = v1 ? QTC_LENS_LZ : QTC_LENS_LZ2;
    if (qtc_compress(in, (size_t)sz, &out, &out_len, &opts) != QTC_OK) {
        printf("lz encode failed\n");
        free(in);
        return 1;
    }
    printf("input %ld bytes -> %s payload %zu bytes (container overhead incl.)\n",
           sz, v1 ? "LZ" : "LZ2", out_len);

    /* Container: 16B header, then stream: [u8 num_blocks]
     * per block: [u32 block_len][u8 version (LZ2 only)][block payload]. */
    if (out_len < 16 + 1) { printf("too short\n"); return 1; }
    const uint8_t *p = out + 16;
    size_t rem = out_len - 16;
    uint32_t nblocks = *p;
    p += 1; rem -= 1;
    printf("blocks: %u\n", nblocks);
    for (uint32_t b = 0; b < nblocks && rem >= 4; b++) {
        uint32_t block_len = rd32(p);
        p += 4; rem -= 4;
        (void)block_len;
        uint8_t ver = 0;
        if (!v1) { ver = *p; p += 1; rem -= 1; }
        uint32_t orig = rd32(p);
        uint32_t ntok = rd32(p + 4);
        uint32_t nmatch = rd32(p + 8);
        const uint8_t *q = p + 12;

        if (!v1 && ver == 1) {
            /* v2 block */
            uint8_t flags = *q; q += 1;
            int use_u8 = flags & 1;
            size_t freq_w = use_u8 ? 1 : 2;
            uint32_t rep_bytes = rd32(q); q += 4;
            size_t nrep = popcount_bytes(q, rep_bytes);
            q += rep_bytes;
            uint32_t lit_bm = (uint32_t)q[0] | ((uint32_t)q[1] << 8); q += 2;
            size_t lit_present = popcount_bytes(q, lit_bm);
            q += lit_bm + lit_present * freq_w;
            uint32_t buck_bm = (uint32_t)q[0] | ((uint32_t)q[1] << 8); q += 2;
            size_t buck_present = popcount_bytes(q, buck_bm);
            q += buck_bm + buck_present * freq_w;
            uint32_t lit_bits = rd32(q); q += 4;
            q += lit_bits;
            uint32_t buck_bits = rd32(q); q += 4;
            q += buck_bits;
            uint32_t extra = rd32(q); q += 4;
            q += extra;
            size_t block_total = (size_t)(q - p);
            printf("block %u (v2%s): orig=%u ntok=%u nmatch=%u (rep=%zu) total=%zu\n",
                   b, use_u8 ? ", u8 freqs" : "", orig, ntok, nmatch, nrep,
                   block_total);
            printf("  header+counts+flags : 13\n");
            printf("  rep flags           : %u (+4 len)\n", rep_bytes);
            printf("  litlen bitmap       : %u  (present=%zu)\n", lit_bm, lit_present);
            printf("  litlen freqs        : %zu\n", lit_present * freq_w);
            printf("  bucket bitmap       : %u  (present=%zu)\n", buck_bm, buck_present);
            printf("  bucket freqs        : %zu\n", buck_present * freq_w);
            printf("  litlen bits         : %u\n", lit_bits);
            printf("  bucket bits         : %u\n", buck_bits);
            printf("  extra bits          : %u\n", extra);
        } else {
            /* v1 block */
            size_t lit_bitmap = LITLEN_ALPHA / 8;
            size_t lit_present = popcount_bytes(q, lit_bitmap);
            q += lit_bitmap + lit_present * 2;
            size_t buck_bitmap = (BUCKET_ALPHA + 7) / 8;
            size_t buck_present = popcount_bytes(q, buck_bitmap);
            q += buck_bitmap + buck_present * 2;
            uint32_t lit_bits = rd32(q); q += 4;
            q += lit_bits;
            uint32_t buck_bits = rd32(q); q += 4;
            q += buck_bits;
            uint32_t extra = rd32(q); q += 4;
            q += extra;
            size_t block_total = (size_t)(q - p);
            printf("block %u (v1): orig=%u ntok=%u nmatch=%u total=%zu\n",
                   b, orig, ntok, nmatch, block_total);
            printf("  header+counts : 12\n");
            printf("  litlen bitmap : %zu  (present=%zu)\n", lit_bitmap, lit_present);
            printf("  litlen freqs  : %zu\n", lit_present * 2);
            printf("  bucket bitmap : %zu  (present=%zu)\n", buck_bitmap, buck_present);
            printf("  bucket freqs  : %zu\n", buck_present * 2);
            printf("  litlen bits   : %u\n", lit_bits);
            printf("  bucket bits   : %u\n", buck_bits);
            printf("  extra bits    : %u\n", extra);
        }
        p = q;
        rem -= block_len;
        if (b >= 2) break;
    }
    free(in);
    free(out);
    return 0;
}
