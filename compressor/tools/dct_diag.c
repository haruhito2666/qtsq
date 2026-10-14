/* One-off diagnostic: where do the bytes in a qtc-dct stream go? */
#include "../src/qtc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../include/stb_image.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: dct_diag img.png\n"); return 1; }
    int w, h, ch;
    uint8_t *px = stbi_load(argv[1], &w, &h, &ch, 3);
    if (!px) { printf("load failed\n"); return 1; }
    float qs[] = {0.95f, 0.85f, 0.5f};
    for (int i = 0; i < 3; i++) {
        uint8_t *c; size_t cl;
        if (qtc_image_dct_encode(px, (uint32_t)w, (uint32_t)h, 3, qs[i], &c, &cl) != QTC_OK) {
            printf("encode failed\n"); return 1;
        }
        /* header layout: [u32 w][u32 h][u8 ch][u8 q100][u8 sub][u8 rsv]
                          [u32 nsym_y][u32 nsym_c][u32 bits_y_len][u32 nampbytes] */
        uint32_t nsym_y = rd32(c + 12);
        uint32_t nsym_c = rd32(c + 16);
        uint32_t byl    = rd32(c + 20);
        uint32_t namp   = rd32(c + 24);
        size_t hdr = 28, models = 1024;
        size_t bits_c = cl - hdr - models - byl - namp;
        double bpp = (double)cl * 8.0 / ((double)w * h);
        printf("q=%.2f total=%zu (%.3f bpp) | hdr=%zu models=%zu bitsY=%u bitsC=%zu amp=%u | nsymY=%u nsymC=%u\n",
               qs[i], cl, bpp, hdr, models, byl, bits_c, namp, nsym_y, nsym_c);
        free(c);
    }
    stbi_image_free(px);
    return 0;
}
