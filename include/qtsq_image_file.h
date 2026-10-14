/*
 * Image file decode — see src/qtsq_image_file.c.
 *
 * Decodes PNG/JPEG/BMP/GIF/PNM/TGA in memory so an image FILE can be
 * transcoded into a qtsq pixel stream instead of being stored verbatim.
 * No new link dependency: the decoder is stb_image, already in-tree.
 */

#ifndef QTSQ_IMAGE_FILE_H
#define QTSQ_IMAGE_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTSQ_IMAGE_OK            0
#define QTSQ_IMAGE_ERR_FORMAT (-1)  /* not decodable / truncated */
#define QTSQ_IMAGE_ERR_SIZE   (-2)  /* dimensions overflow */

/* Read just the geometry. Cheap: stb parses headers without decoding
 * pixels, so this is safe to call on untrusted input to reject early. */
int qtsq_image_probe(const uint8_t *data, size_t len,
                     uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch);

/* Decode to 8-bit interleaved pixels, keeping the file's own channel
 * count (1=grey, 2=grey+alpha, 3=rgb, 4=rgba). The buffer is malloc'd
 * and owned by the caller; free it with free().
 *
 * A hostile file can make stbi allocate a lot before failing; the only
 * hard bound is SIZE_MAX, so callers facing untrusted input should
 * qtsq_image_probe() first and reject on a policy limit. */
int qtsq_image_decode(const uint8_t *data, size_t len,
                      uint8_t **out_pixels, size_t *out_size,
                      uint32_t *out_w, uint32_t *out_h, uint8_t *out_ch);

/* Human-readable form of a QTSQ_IMAGE_* code. */
const char *qtsq_image_error_string(int rc);

#ifdef __cplusplus
}
#endif

#endif /* QTSQ_IMAGE_FILE_H */