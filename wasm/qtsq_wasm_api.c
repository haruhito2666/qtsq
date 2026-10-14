#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "qtsq.h"

/* WASM-1 NOTE: All state below is global. This module assumes single-threaded
 * JavaScript usage. Do not call qtsq_wasm_decode() concurrently from Web Workers
 * sharing the same WASM instance — use separate Worker instances instead. */
static qtsq_context_t g_ctx;
static uint8_t *g_pixels = NULL;
static size_t g_pixels_size = 0;
static char *g_text = NULL;
static int g_width = 0;
static int g_height = 0;
static int g_type = 0;

// Read from memory helper since we don't have the internal qtsq_read_from_memory exposed in qtsq.h 
// Actually, qtsq.h doesn't expose it, so we'll just write it to a virtual file and read it back,
// or we can use the private struct parsing if we must, but virtual file is safer.
EMSCRIPTEN_KEEPALIVE int qtsq_wasm_decode(const uint8_t *data, size_t len, const char *password) {
    if (!data || len == 0) return QTSQ_ERR_NULL;
    
    // Cleanup previous state
    qtsq_free(&g_ctx);
    if (g_pixels) { free(g_pixels); g_pixels = NULL; }
    if (g_text) { free(g_text); g_text = NULL; }
    g_pixels_size = 0;
    g_width = 0;
    g_height = 0;
    g_type = 0;

    qtsq_init(&g_ctx);

    // Emscripten virtual file system
    FILE *f = fopen("/virtual.qtsq", "wb");
    if (!f) return QTSQ_ERR_IO;
    fwrite(data, 1, len, f);
    fclose(f);

    int res = qtsq_read(&g_ctx, "/virtual.qtsq");
    remove("/virtual.qtsq");

    if (res != QTSQ_OK) {
        return res;
    }

    g_type = g_ctx.header.data_type;

    if (password && strlen(password) > 0) {
        if (g_ctx.is_encrypted) {
            int dec_res = qtsq_decrypt(&g_ctx, password);
            /* GEN-2 FIX: wipe password from WASM heap after use.
             * JS passes passwords as C strings into WASM memory \u2014 they must
             * not linger in heap where a dump could expose them. */
            volatile char *pw = (volatile char *)password;
            while (*pw) *pw++ = '\0';
            if (dec_res != QTSQ_OK) {
                return dec_res;
            }
        }
    } else if (g_ctx.is_encrypted) {
        return QTSQ_ERR_ENCRYPTED; // Needs password
    }

    uint8_t *out_data = NULL;
    size_t out_size = 0;

    int dec_res = qtsq_decompress(&g_ctx, &out_data, &out_size);
    if (dec_res != QTSQ_OK) {
        return dec_res;
    }

    if (g_type == QTSQ_TYPE_IMAGE_RGB || g_type == QTSQ_TYPE_IMAGE_RGBA || g_type == QTSQ_TYPE_IMAGE_GRAY) {
        g_width = g_ctx.schema.dimensions[0];
        g_height = g_ctx.schema.dimensions[1];
        
        // JPEG/PNG passthrough check
        if (g_width == 0 || g_height == 0) {
            g_pixels = out_data;
            g_pixels_size = out_size;
            // Let the JS side handle it by passing the raw bytes
        } else {
            /* Convert everything to RGBA for canvas ImageData.
             * WASM-2 FIX: g_width/g_height are int; use int64 intermediate to detect
             * overflow before casting to size_t (wasm32 has 32-bit size_t). */
            int64_t rgba_check = (int64_t)g_width * g_height * 4;
            if (rgba_check <= 0 || rgba_check > 512 * 1024 * 1024LL) {
                free(out_data);
                return QTSQ_ERR_FORMAT; /* image too large for WASM heap */
            }
            size_t rgba_size = (size_t)rgba_check;
            g_pixels = (uint8_t *)malloc(rgba_size);
            if (!g_pixels) {
                free(out_data);
                return QTSQ_ERR_ALLOC;
            }
            
            if (g_type == QTSQ_TYPE_IMAGE_RGB) {
                for (size_t i = 0; i < (size_t)g_width * g_height; i++) {
                    g_pixels[i*4 + 0] = out_data[i*3 + 0];
                    g_pixels[i*4 + 1] = out_data[i*3 + 1];
                    g_pixels[i*4 + 2] = out_data[i*3 + 2];
                    g_pixels[i*4 + 3] = 255;
                }
            } else if (g_type == QTSQ_TYPE_IMAGE_RGBA) {
                memcpy(g_pixels, out_data, rgba_size);
            } else if (g_type == QTSQ_TYPE_IMAGE_GRAY) {
                for (size_t i = 0; i < (size_t)g_width * g_height; i++) {
                    g_pixels[i*4 + 0] = out_data[i];
                    g_pixels[i*4 + 1] = out_data[i];
                    g_pixels[i*4 + 2] = out_data[i];
                    g_pixels[i*4 + 3] = 255;
                }
            }
            g_pixels_size = rgba_size;
            free(out_data);
        }
    } else if (g_type == QTSQ_TYPE_TEXT_ASCII || g_type == QTSQ_TYPE_TEXT_UTF8 || g_type == QTSQ_TYPE_JSON || g_type == QTSQ_TYPE_CSV) {
        g_text = (char *)out_data; // Text usually comes back null-terminated or we just pass the size
        g_pixels_size = out_size;
    } else {
        // Unhandled type for WASM right now
        free(out_data);
        return QTSQ_ERR_TYPE;
    }

    return QTSQ_OK;
}

EMSCRIPTEN_KEEPALIVE uint8_t* qtsq_wasm_get_pixels(void) {
    return g_pixels;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_get_pixels_size(void) {
    return (int)g_pixels_size;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_get_width(void) {
    return g_width;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_get_height(void) {
    return g_height;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_get_type(void) {
    return g_type;
}

EMSCRIPTEN_KEEPALIVE char* qtsq_wasm_get_text(void) {
    return g_text;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_get_text_length(void) {
    return (int)g_pixels_size;
}

EMSCRIPTEN_KEEPALIVE void qtsq_wasm_free(void) {
    qtsq_free(&g_ctx);
    if (g_pixels) { free(g_pixels); g_pixels = NULL; }
    if (g_text) { free(g_text); g_text = NULL; }
    g_pixels_size = 0;
    g_width = 0;
    g_height = 0;
    g_type = 0;
}

EMSCRIPTEN_KEEPALIVE int qtsq_wasm_is_encrypted(const uint8_t *data, size_t len) {
    if (!data || len < sizeof(qtsq_header_t)) return 0;
    qtsq_header_t header;
    memcpy(&header, data, sizeof(qtsq_header_t));
    if (header.magic[0] != QTSQ_MAGIC_0 || header.magic[1] != QTSQ_MAGIC_1 || header.magic[2] != QTSQ_MAGIC_2 || header.magic[3] != QTSQ_MAGIC_3) return 0;
    
    // Check flags for encryption
    return (header.flags & QTSQ_FLAG_ENCRYPTED) ? 1 : 0;
}

/* WASM-3 FIX: Separate export for post-quantum encrypted files.
 * qtsq_wasm_decode() cannot currently decrypt PQ-encrypted (ML-KEM-768) files.
 * JS callers must check this and display a meaningful error rather than
 * silently failing with a generic decryption error. */
EMSCRIPTEN_KEEPALIVE int qtsq_wasm_is_pq_encrypted(const uint8_t *data, size_t len) {
    if (!data || len < sizeof(qtsq_header_t)) return 0;
    qtsq_header_t header;
    memcpy(&header, data, sizeof(qtsq_header_t));
    if (header.magic[0] != QTSQ_MAGIC_0 || header.magic[1] != QTSQ_MAGIC_1 ||
        header.magic[2] != QTSQ_MAGIC_2 || header.magic[3] != QTSQ_MAGIC_3) return 0;
    return (header.flags & QTSQ_FLAG_PQ_ENCRYPTED) ? 1 : 0;
}
