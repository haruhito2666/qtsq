/*
 * Image file decode for QTSQ (qtsq 2.2)
 *
 * The library already had from-scratch image coders — lossless 2D MED
 * prediction and a JPEG-like 8x8 DCT photo coder, both benchmarked
 * against libjpeg and libwebp — but PNG/JPEG/WebP files could only be
 * stored verbatim: there was no decoder, so qtsq_container_add_file sent
 * every .png through qtsq_compress_raw and it came out uncompressed.
 *
 * This translation unit closes that gap by hosting stb_image's
 * implementation. It is the ONLY file that defines
 * STB_IMAGE_IMPLEMENTATION, so the ~60 KB of JPEG/PNG/HDR decoder lands
 * in one object rather than being pulled into every consumer, and it
 * costs no new link dependency: stb_image.h has been in-tree since
 * before this and was previously used only by command-line tools.
 *
 * Decoding happens in memory; nothing here touches the filesystem, so a
 * caller can transcode bytes it already read.
 */

#include <stdlib.h>
#include <string.h>

#include "qtsq_image_file.h"

/* Only the decoders worth shipping: the formats QTSQ claims to carry.
 * Dropping PSD/PIC/TGA/GIF/HDR keeps this object small; adding them back
 * is a matter of deleting the STBI_ONLY_ lines. */
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_PNM
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int qtsq_image_probe(const uint8_t *data, size_t len,
                     uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch) {
    if (!data || len == 0 || len > (size_t)0x7fffffff) return QTSQ_IMAGE_ERR_FORMAT;
    int x = 0, y = 0, comp = 0;
    if (!stbi_info_from_memory((const stbi_uc *)data, (int)len, &x, &y, &comp))
        return QTSQ_IMAGE_ERR_FORMAT;
    if (x <= 0 || y <= 0 || comp < 1 || comp > 4) return QTSQ_IMAGE_ERR_FORMAT;
    if (out_w) *out_w = (uint32_t)x;
    if (out_h) *out_h = (uint32_t)y;
    if (out_ch) *out_ch = (uint8_t)comp;
    return QTSQ_IMAGE_OK;
}

int qtsq_image_decode(const uint8_t *data, size_t len,
                      uint8_t **out_pixels, size_t *out_size,
                      uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch) {
    if (!data || !out_pixels || !out_size || len == 0 ||
        len > (size_t)0x7fffffff)
        return QTSQ_IMAGE_ERR_FORMAT;
    *out_pixels = NULL;
    *out_size = 0;

    int x = 0, y = 0, comp = 0;
    /* req_comp = 0 keeps the file's own channel count, so a greyscale PNG
     * stays 1 channel instead of being widened to RGB and tripling. */
    stbi_uc *px = stbi_load_from_memory((const stbi_uc *)data, (int)len,
                                        &x, &y, &comp, 0);
    if (!px) return QTSQ_IMAGE_ERR_FORMAT;
    if (x <= 0 || y <= 0 || comp < 1 || comp > 4) { stbi_image_free(px); return QTSQ_IMAGE_ERR_FORMAT; }

    size_t n = (size_t)x * (size_t)y * (size_t)comp;
    /* Guard the multiply before it can wrap: 4 channels × 2^31 px would. */
    if ((size_t)x > (SIZE_MAX / ((size_t)y * (size_t)comp))) {
        stbi_image_free(px);
        return QTSQ_IMAGE_ERR_SIZE;
    }
    *out_pixels = (uint8_t *)px;   /* already malloc'd by stb */
    *out_size = n;
    if (out_w) *out_w = (uint32_t)x;
    if (out_h) *out_h = (uint32_t)y;
    if (out_ch) *out_ch = (uint8_t)comp;
    return QTSQ_IMAGE_OK;
}

const char *qtsq_image_error_string(int rc) {
    switch (rc) {
        case QTSQ_IMAGE_OK:      return "ok";
        case QTSQ_IMAGE_ERR_FORMAT: return "not a decodable image (or truncated)";
        case QTSQ_IMAGE_ERR_SIZE:   return "image dimensions overflow";
        default:                 return "unknown error";
    }
}