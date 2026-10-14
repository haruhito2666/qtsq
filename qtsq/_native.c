/*
 * qtsq Python C Extension — Full SDK
 *
 * Functions:
 *   qtsq._native.read_vertices(path)              → vertex data
 *   qtsq._native.compress_image(pixels, w, h, ch, path, mode)  → compress
 *   qtsq._native.decompress_image(path)            → pixel data
 *   qtsq._native.compress_text(text, path)          → compress text
 *   qtsq._native.decompress_raw(path)               → raw bytes
 *   qtsq._native.file_info(path)                    → metadata dict
 *   qtsq._native.compress_tensor(data, dims, path, precision)  → tensor
 *   qtsq._native.decompress_tensor(path)            → float array
 *   qtsq._native.brain_save(path, weights, biases, memories, vector_dim, meta)
 *   qtsq._native.brain_load(path)                   → dict of arrays
 *
 * Author: haruhito
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "qtsq.h"
#include "qtsq_vault.h"
#include "qtsq_keystore.h"
#include "qtsq_image_file.h"
#include <string.h>
#include <zlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#define NATIVE_HAVE_MMAP 1
#define NATIVE_HAVE_THREADS 1
#endif

#define MAX_VERTICES 100000

/* Encrypt (and sanitize) a context before writing.
 * Guarantee #1 (zero metadata) is enforced inside the vault: when
 * is_sanitized is not already set, qtsq_vault_encrypt_password runs
 * qtsq_sanitize_context() to move names inside the encryption envelope. */
static int native_encrypt_for_write(qtsq_context_t *ctx, const char *password) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (password && password[0]) {
        return qtsq_vault_encrypt_password(ctx, password);
    }
    return QTSQ_ERR_NOT_ENCRYPTED;
}

/* Weights UX: password-optional writes with necessary security retained.
 *
 *   password given  → full vault encryption (as before).
 *   no password + device keystore ready → encrypt to the device key
 *     (zero friction, still encrypted at rest; failures are errors,
 *     never silent plaintext).
 *   no password + no keystore → sanitized + checksummed plaintext via
 *     the explicit qtsq_write_plaintext escape hatch (like safetensors /
 *     .bin: integrity + atomic write, confidentiality opt-in).
 *
 * Returns QTSQ_OK and sets *out_plaintext=0 when the caller should use
 * qtsq_write(), or *out_plaintext=1 for qtsq_write_plaintext().
 * Tensor payloads carry no metadata, and every stream is SHA-256
 * checksummed either way (tamper-evident on load). */
static int native_weights_seal_for_write(qtsq_context_t *ctx, const char *password,
                                         int *out_plaintext) {
    if (!ctx || !out_plaintext) return QTSQ_ERR_NULL;
    *out_plaintext = 0;
    if (password && password[0]) {
        int rc = qtsq_vault_encrypt_password(ctx, password);
        if (rc != QTSQ_OK) return rc;
        return QTSQ_OK;
    }
    if (qtsq_keystore_is_initialized()) {
        /* Device key present: encryption is mandatory, never silently
         * downgraded — a failure is an error, not plaintext. */
        return qtsq_keystore_encrypt(ctx);
    }
    ctx->is_sanitized = 1;
    ctx->sanitize_report |= QTSQ_SANITIZE_ZERO_META;
    ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    *out_plaintext = 1;
    return QTSQ_OK;
}

static int native_weights_write(qtsq_context_t *ctx, const char *path, int plaintext) {
    if (plaintext)
        return qtsq_write_plaintext(ctx, path);
    return qtsq_write(ctx, path);
}

/* Load-side gate for weight files: plaintext opens with or without a
 * password; encrypted files need one (clear error, not garbage). */
static int native_weights_decrypt_if_needed(qtsq_context_t *ctx, const char *password,
                                            const char *path) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->is_encrypted) return QTSQ_OK;
    if (!password || !password[0]) return QTSQ_ERR_ENCRYPTED;
    return qtsq_vault_decrypt_password(ctx, password);
}

/* Apply codec/quality kwargs to a freshly initialised context.
 * qtc is the default codec (set by qtsq_init); passing "zlib" opts out to
 * the legacy zlib codec. quality > 0 enables lossy qtc paths where
 * available; text/json/csv/tensor paths stay lossless regardless. */
static int apply_codec(qtsq_context_t *ctx, const char *codec, double quality) {
    if (!codec || strcmp(codec, "qtc") == 0) {
        ctx->quality = (float)quality;
        return 0; /* qtc is already the default from qtsq_init */
    }
    if (strcmp(codec, "zlib") == 0) {
        ctx->codec = QTSQ_CODEC_ZLIB;
        return 0;
    }
    PyErr_Format(PyExc_ValueError, "Unknown codec '%s' (use 'qtc' or 'zlib')", codec);
    return -1;
}

/* ── Helper: get TRM3 dimensions ── */
static int get_dimensions(const qtsq_context_t *ctx, uint32_t *w, uint32_t *h) {
    if (!ctx || !ctx->singularity || ctx->singularity_size < 8) return -1;
    /* SEC: bound file-declared decompressed size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    if (qtsq_uncompress_bounded(ctx->singularity, ctx->singularity_size,
                                256, 1ULL << 30, &packed, &dl) != QTSQ_OK) return -1;
    if (dl < 12 || packed[0]!='T' || packed[1]!='R' || packed[2]!='M' || packed[3]!='3') {
        free(packed);
        return -1;
    }
    memcpy(w, packed + 4, 4);
    memcpy(h, packed + 8, 4);
    free(packed);
    return 0;
}

/* ══════════════════════════════════════════════════════
 *  1. read_vertices(path, max_vertices=100000)
 * ══════════════════════════════════════════════════════ */
static PyObject* py_read_vertices(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    int max_verts = MAX_VERTICES;
    const char *password = "";
    static char *kwlist[] = {"path", "max_vertices", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|is", kwlist, &path, &max_verts, &password))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }

    if (ctx.is_encrypted && password && password[0]) {
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "Decrypt failed: %s (err=%d)", path, rc);
            return NULL;
        }
    }

    uint32_t width = 0, height = 0;
    get_dimensions(&ctx, &width, &height);

    float *verts = (float *)malloc((size_t)max_verts * 5 * sizeof(float));
    if (!verts) { qtsq_free(&ctx); return PyErr_NoMemory(); }

    int nv = qtsq_extract_vertices(&ctx, verts, (uint32_t)max_verts);
    qtsq_free(&ctx);

    if (nv <= 0) {
        free(verts);
        PyErr_Format(PyExc_RuntimeError, "No vertices in: %s", path);
        return NULL;
    }

    if (width == 0 || height == 0) {
        for (int i = 0; i < nv; i++) {
            if ((uint32_t)verts[i*5+0]+1 > width)  width = (uint32_t)verts[i*5+0]+1;
            if ((uint32_t)verts[i*5+1]+1 > height) height = (uint32_t)verts[i*5+1]+1;
        }
    }

    PyObject *vert_list = PyList_New(nv);
    for (int i = 0; i < nv; i++) {
        PyList_SET_ITEM(vert_list, i, Py_BuildValue("(fffff)",
            verts[i*5+0], verts[i*5+1], verts[i*5+2], verts[i*5+3], verts[i*5+4]));
    }
    free(verts);

    PyObject *res = Py_BuildValue("{s:O,s:I,s:I,s:i}",
        "vertices", vert_list, "width", width, "height", height, "num_vertices", nv);
    /* "O" in Py_BuildValue takes a NEW reference, so the one held by
     * PyList_New must be dropped. Without this the list -- and every
     * vertex tuple it owns -- leaked on every call. */
    Py_DECREF(vert_list);
    return res;
}

/* ══════════════════════════════════════════════════════
 *  2. compress_image(pixels_bytes, w, h, ch, path, mode)
 * ══════════════════════════════════════════════════════ */
static PyObject* py_compress_image(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *pixels;
    Py_ssize_t pixels_len;
    unsigned int w, h;
    unsigned char ch;
    const char *out_path;
    const char *mode;
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    static char *kwlist[] = {"pixels", "width", "height", "channels",
                             "path", "mode", "password", "codec", "quality", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#IIbss|ssd", kwlist,
                          &pixels, &pixels_len, &w, &h, &ch, &out_path, &mode,
                          &password, &codec, &quality))
        return NULL;

    /* w, h are unsigned int and ch is unsigned char, so w*h*ch is
     * evaluated in unsigned 32-bit arithmetic and wraps BEFORE it is
     * widened. A caller-supplied 65536x65536x3 wrapped to 0, passed
     * the check against an empty buffer, and the library then walked a
     * 0-byte allocation with 65536x65536 dimensions (SIGSEGV).
     * Compute in size_t and reject anything that will not fit. */
    size_t expect_px = (size_t)w * (size_t)h * (size_t)ch;
    if (expect_px / (size_t)ch / (size_t)h != (size_t)w ||
        expect_px > (size_t)PY_SSIZE_T_MAX) {
        PyErr_Format(PyExc_ValueError,
            "Image dimensions too large: %u x %u x %u", w, h, ch);
        return NULL;
    }
    if ((Py_ssize_t)expect_px != pixels_len) {
        PyErr_Format(PyExc_ValueError,
            "Pixel buffer size mismatch: expected %zu, got %zd",
            expect_px, pixels_len);
        return NULL;
    }

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (apply_codec(&ctx, codec, quality) != 0) {
        qtsq_free(&ctx);
        return NULL;
    }

    int rc;
    size_t src_size = (size_t)pixels_len;

    /* ── Gravitational (triangle + splat) ── */
    if (strcmp(mode, "hd") == 0) {
        rc = qtsq_compress_gravitational_hd(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "lossless") == 0) {
        rc = qtsq_compress_gravitational_lossless(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "upscale") == 0) {
        rc = qtsq_compress_gravitational_upscale(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    }
    /* ── Splat-only ── */
    else if (strcmp(mode, "splat") == 0) {
        rc = qtsq_compress_splat_compact(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "splat_hd") == 0) {
        rc = qtsq_compress_splat_hd(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "splat_lossless") == 0) {
        rc = qtsq_compress_splat_lossless(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    }
    /* ── Triangle-only ── */
    else if (strcmp(mode, "triangle") == 0) {
        rc = qtsq_compress_triangle_compact(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "triangle_hd") == 0) {
        rc = qtsq_compress_triangle_hd(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "triangle_lossless") == 0) {
        rc = qtsq_compress_triangle_lossless(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "triangle_pixel") == 0) {
        rc = qtsq_compress_triangle_pixel(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    } else if (strcmp(mode, "triangle_adaptive") == 0) {
        rc = qtsq_compress_triangle_adaptive(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size,
            3,      /* max_depth: up to 64 sub-triangles per root */
            15.0f); /* error_threshold: MSE per channel */
    }
    /* ── Ionic Data Tunnel (ICE-inspired) ── */
    else if (strcmp(mode, "hd_ionic") == 0) {
        rc = qtsq_ionic_compress_hd(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    }
    /* ── Default: gravitational (triangle + splat) ── */
    else {
        rc = qtsq_compress_gravitational(&ctx,
            (const uint8_t *)pixels, w, h, ch, src_size);
    }

    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Compress failed (mode=%s, err=%d)", mode, rc);
        return NULL;
    }

    rc = native_encrypt_for_write(&ctx, password);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Encryption failed (password required): %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = qtsq_write(&ctx, out_path);
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════
 *  3. decompress_image(path) → {pixels, width, height, channels}
 * ══════════════════════════════════════════════════════ */
static PyObject* py_decompress_image(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }

    if (ctx.is_encrypted && password && password[0]) {
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "Decrypt failed: %s (err=%d)", path, rc);
            return NULL;
        }
    }

    /* Get dimensions */
    uint32_t width = 0, height = 0;
    get_dimensions(&ctx, &width, &height);

    /* Decompress based on strategy */
    uint8_t *out_data = NULL;
    size_t out_size = 0;
    int rc;

    uint8_t strategy = ctx.header.strategy;
    if (strategy == QTSQ_STRATEGY_SPLAT) {
        rc = qtsq_decompress_splat_only(&ctx, &out_data, &out_size);
    } else if (strategy == QTSQ_STRATEGY_TRIANGLE ||
               strategy == QTSQ_STRATEGY_LAYERED) {
        rc = qtsq_decompress_gravitational(&ctx, &out_data, &out_size);
    } else {
        rc = qtsq_decompress(&ctx, &out_data, &out_size);
    }

    if (rc != 0 || !out_data || out_size == 0) {
        qtsq_free(&ctx);
        if (out_data) free(out_data);
        PyErr_Format(PyExc_RuntimeError, "Decompress failed: %s (err=%d)", path, rc);
        return NULL;
    }

    /* Figure out channels */
    uint8_t ch = ctx.header.num_channels;
    if (ch == 0) ch = 3;

    /* If dimensions unknown, try to figure them out */
    if (width == 0 || height == 0) {
        /* Fallback: get from schema */
        width = ctx.schema.dimensions[0];
        height = ctx.schema.dimensions[1];
    }

    /* Return as bytes + metadata */
    PyObject *result = Py_BuildValue("{s:y#,s:I,s:I,s:B}",
        "pixels", out_data, (Py_ssize_t)out_size,
        "width", width,
        "height", height,
        "channels", ch);

    free(out_data);
    qtsq_free(&ctx);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  4. compress_text(text, path)
 * ══════════════════════════════════════════════════════ */
static PyObject* py_compress_text(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *text;
    Py_ssize_t text_len;
    const char *out_path;
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    static char *kwlist[] = {"text", "path", "password", "codec", "quality", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#s|ssd", kwlist,
                                     &text, &text_len, &out_path, &password,
                                     &codec, &quality))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (apply_codec(&ctx, codec, quality) != 0) {
        qtsq_free(&ctx);
        return NULL;
    }

    int rc = qtsq_compress_text(&ctx, text, (size_t)text_len);
    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Text compress failed (err=%d)", rc);
        return NULL;
    }

    rc = native_encrypt_for_write(&ctx, password);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Encryption failed (password required): %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = qtsq_write(&ctx, out_path);
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* py_compress_json(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *text;
    Py_ssize_t text_len;
    const char *out_path;
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    static char *kwlist[] = {"json_str", "path", "password", "codec", "quality", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#s|ssd", kwlist,
                                     &text, &text_len, &out_path, &password,
                                     &codec, &quality))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (apply_codec(&ctx, codec, quality) != 0) {
        qtsq_free(&ctx);
        return NULL;
    }

    int rc = qtsq_compress_json(&ctx, text, (size_t)text_len);
    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "JSON compress failed (err=%d)", rc);
        return NULL;
    }

    rc = native_encrypt_for_write(&ctx, password);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Encryption failed (password required): %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = qtsq_write(&ctx, out_path);
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* py_compress_csv(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *text;
    Py_ssize_t text_len;
    const char *out_path;
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    static char *kwlist[] = {"csv_str", "path", "password", "codec", "quality", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#s|ssd", kwlist,
                                     &text, &text_len, &out_path, &password,
                                     &codec, &quality))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (apply_codec(&ctx, codec, quality) != 0) {
        qtsq_free(&ctx);
        return NULL;
    }

    int rc = qtsq_compress_csv(&ctx, text, (size_t)text_len);
    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "CSV compress failed (err=%d)", rc);
        return NULL;
    }

    rc = native_encrypt_for_write(&ctx, password);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Encryption failed (password required): %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = qtsq_write(&ctx, out_path);
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════
 *  5. decompress_raw(path) → bytes
 * ══════════════════════════════════════════════════════ */
static PyObject* py_decompress_raw(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }

    if (ctx.is_encrypted && password && password[0]) {
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "Decrypt failed: %s (err=%d)", path, rc);
            return NULL;
        }
    }

    uint8_t *out_data = NULL;
    size_t out_size = 0;
    int rc = qtsq_decompress(&ctx, &out_data, &out_size);
    qtsq_free(&ctx);

    if (rc != 0 || !out_data) {
        if (out_data) free(out_data);
        PyErr_Format(PyExc_RuntimeError, "Decompress failed: %s", path);
        return NULL;
    }

    PyObject *result = PyBytes_FromStringAndSize((const char *)out_data, (Py_ssize_t)out_size);
    free(out_data);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  6. file_info(path) → dict with metadata
 * ══════════════════════════════════════════════════════ */
static PyObject* py_file_info(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }

    if (ctx.is_encrypted && password && password[0]) {
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "Decrypt failed: %s (err=%d)", path, rc);
            return NULL;
        }
    }

    uint32_t width = 0, height = 0;
    get_dimensions(&ctx, &width, &height);

    /* Strategy name */
    const char *strategy_name = "unknown";
    switch (ctx.header.strategy) {
        case QTSQ_STRATEGY_TRIANGLE: strategy_name = "triangle"; break;
        case QTSQ_STRATEGY_SPLAT:    strategy_name = "splat"; break;
        case QTSQ_STRATEGY_LAYERED:  strategy_name = "layered"; break;
        case QTSQ_STRATEGY_PROCEDURAL: strategy_name = "procedural"; break;
        case QTSQ_STRATEGY_FRACTAL:  strategy_name = "fractal"; break;
        case QTSQ_STRATEGY_FOURIER:  strategy_name = "fourier"; break;
        case QTSQ_STRATEGY_DICTIONARY: strategy_name = "dictionary"; break;
        case QTSQ_STRATEGY_PHOTON:   strategy_name = "photon"; break;
        case QTSQ_STRATEGY_TENSOR:   strategy_name = "tensor"; break;
        case QTSQ_STRATEGY_NONE:     strategy_name = "none"; break;
        default: break;
    }

    /* Data type name */
    const char *type_name = "unknown";
    switch (ctx.header.data_type) {
        case QTSQ_TYPE_IMAGE_RGB:    type_name = "image_rgb"; break;
        case QTSQ_TYPE_IMAGE_RGBA:   type_name = "image_rgba"; break;
        case QTSQ_TYPE_IMAGE_GRAY:   type_name = "image_gray"; break;
        case QTSQ_TYPE_TEXT_ASCII:   type_name = "text_ascii"; break;
        case QTSQ_TYPE_TEXT_UTF8:    type_name = "text_utf8"; break;
        case QTSQ_TYPE_AUDIO_MONO:   type_name = "audio_mono"; break;
        case QTSQ_TYPE_AUDIO_STEREO: type_name = "audio_stereo"; break;
        case QTSQ_TYPE_VIDEO_FRAMES: type_name = "video"; break;
        case QTSQ_TYPE_RAW:          type_name = "raw"; break;
        case QTSQ_TYPE_JSON:         type_name = "json"; break;
        case QTSQ_TYPE_CSV:          type_name = "csv"; break;
        case QTSQ_TYPE_TENSOR:       type_name = "tensor"; break;
        case QTSQ_TYPE_CONTAINER:    type_name = "container"; break;
        default: break;
    }

    PyObject *result = Py_BuildValue(
        "{s:s, s:s, s:I, s:I, s:B, s:K, s:I, s:i}",
        "strategy", strategy_name,
        "data_type", type_name,
        "width", width,
        "height", height,
        "channels", ctx.header.num_channels,
        "original_size", (unsigned long long)ctx.header.original_size,
        "version", (unsigned int)ctx.header.version,
        "encrypted", ctx.is_encrypted
    );

    qtsq_free(&ctx);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  7. compress_tensor(data, dims, path, precision='f32')
 * ══════════════════════════════════════════════════════ */
static PyObject* py_compress_tensor(PyObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *data_obj;
    PyObject *dims_obj;
    const char *out_path;
    const char *precision_str = "f32";
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    int fast = 0, store = 0;
    static char *kwlist[] = {"data", "dims", "path", "precision", "password",
                             "codec", "quality", "fast", "store", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOs|sssdii", kwlist,
                                      &data_obj, &dims_obj, &out_path,
                                      &precision_str, &password,
                                      &codec, &quality, &fast, &store))
        return NULL;

    /* Parse precision string → enum */
    qtsq_tensor_precision_t prec = QTSQ_TENSOR_F32;
    if (strcmp(precision_str, "f16") == 0) prec = QTSQ_TENSOR_F16;
    else if (strcmp(precision_str, "f8") == 0) prec = QTSQ_TENSOR_F8;
    else if (strcmp(precision_str, "f4") == 0) prec = QTSQ_TENSOR_F4;
    else if (strcmp(precision_str, "raw") == 0) prec = QTSQ_TENSOR_RAW;
    else if (strcmp(precision_str, "f32") != 0) {
        PyErr_Format(PyExc_ValueError,
            "Unknown precision '%s'. Use: 'f32', 'f16', 'f8', 'f4', 'raw'", precision_str);
        return NULL;
    }

    /* Parse data → float array */
    PyObject *data_list = PySequence_Fast(data_obj, "data must be a sequence");
    if (!data_list) return NULL;

    Py_ssize_t count = PySequence_Fast_GET_SIZE(data_list);
    if (count <= 0) {
        Py_DECREF(data_list);
        PyErr_SetString(PyExc_ValueError, "data must not be empty");
        return NULL;
    }

    float *data = (float *)malloc((size_t)count * sizeof(float));
    if (!data) { Py_DECREF(data_list); return PyErr_NoMemory(); }

    for (Py_ssize_t i = 0; i < count; i++) {
        data[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(data_list, i));
    }
    Py_DECREF(data_list);

    if (PyErr_Occurred()) { free(data); return NULL; }

    /* Parse dims → uint32_t array */
    PyObject *dims_seq = PySequence_Fast(dims_obj, "dims must be a sequence");
    if (!dims_seq) { free(data); return NULL; }

    Py_ssize_t ndims = PySequence_Fast_GET_SIZE(dims_seq);
    if (ndims > 8) ndims = 8;
    uint32_t dims[8] = {0};
    for (Py_ssize_t i = 0; i < ndims; i++) {
        dims[i] = (uint32_t)PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(dims_seq, i));
    }
    Py_DECREF(dims_seq);

    /* Compress */
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    if (apply_codec(&ctx, codec, quality) != 0) {
        free(data);
        qtsq_free(&ctx);
        return NULL;
    }
    if (ctx.codec == QTSQ_CODEC_ZLIB) {
        free(data);
        qtsq_free(&ctx);
        PyErr_SetString(PyExc_ValueError,
            "weights are qtc-only (custom compressor); codec='zlib' is retired");
        return NULL;
    }

    int level = store ? 2 : (fast ? 1 : 0);
    int rc = qtsq_compress_tensor_quantized(&ctx, data, (size_t)count,
                                              dims, (uint8_t)ndims, prec, level);
    free(data);

    if (rc != 0) {
        qtsq_free(&ctx);
        if (rc == QTSQ_ERR_TOO_LARGE)
            PyErr_Format(PyExc_RuntimeError,
                "Tensor too large for one stream (prec=%s): use precision='raw' "
                "(auto-chunks) or save_model()/save_model_sharded()", precision_str);
        else
            PyErr_Format(PyExc_RuntimeError, "Tensor compress failed (prec=%s, err=%d)",
                         precision_str, rc);
        return NULL;
    }

    int w_plaintext = 0;
    rc = native_weights_seal_for_write(&ctx, password, &w_plaintext);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Seal failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = native_weights_write(&ctx, out_path, w_plaintext);
    /* Capture size before qtsq_free zeroes the context — reading
     * ctx.singularity_size after qtsq_free is a use-after-free. */
    size_t compressed_sz = (size_t)ctx.singularity_size;
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    return Py_BuildValue("{s:n,s:s,s:K}",
        "count", count,
        "precision", precision_str,
        "compressed_size", (unsigned long long)compressed_sz);
}

/* ══════════════════════════════════════════════════════
 *  7b. compress_tensor_bytes(raw_float32_bytes, dims, path, ...)
 *  Fast path for numpy arrays: zero-copy buffer, no Python-float list.
 * ══════════════════════════════════════════════════════ */
static PyObject* py_compress_tensor_bytes(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *raw;
    Py_ssize_t raw_len;
    PyObject *dims_obj;
    const char *out_path;
    const char *precision_str = "raw";
    const char *password = "";
    const char *codec = "qtc";
    double quality = 0.0;
    int fast = 0, store = 0;
    static char *kwlist[] = {"raw", "dims", "path", "precision", "password",
                             "codec", "quality", "fast", "store", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#Os|sssdii", kwlist,
                                      &raw, &raw_len, &dims_obj, &out_path,
                                      &precision_str, &password,
                                      &codec, &quality, &fast, &store))
        return NULL;

    qtsq_tensor_precision_t prec = QTSQ_TENSOR_F32;
    if (strcmp(precision_str, "f16") == 0) prec = QTSQ_TENSOR_F16;
    else if (strcmp(precision_str, "f8") == 0) prec = QTSQ_TENSOR_F8;
    else if (strcmp(precision_str, "f4") == 0) prec = QTSQ_TENSOR_F4;
    else if (strcmp(precision_str, "raw") == 0) prec = QTSQ_TENSOR_RAW;
    else if (strcmp(precision_str, "f32") != 0) {
        PyErr_Format(PyExc_ValueError,
            "Unknown precision '%s'. Use: 'f32', 'f16', 'f8', 'f4', 'raw'", precision_str);
        return NULL;
    }

    if (raw_len <= 0 || raw_len % 4 != 0) {
        PyErr_SetString(PyExc_ValueError, "raw must be non-empty float32 bytes");
        return NULL;
    }
    size_t count = (size_t)raw_len / 4;

    PyObject *dims_seq = PySequence_Fast(dims_obj, "dims must be a sequence");
    if (!dims_seq) return NULL;
    Py_ssize_t ndims = PySequence_Fast_GET_SIZE(dims_seq);
    if (ndims > 8) ndims = 8;
    uint32_t dims[8] = {0};
    for (Py_ssize_t i = 0; i < ndims; i++) {
        dims[i] = (uint32_t)PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(dims_seq, i));
    }
    Py_DECREF(dims_seq);
    if (PyErr_Occurred()) return NULL;

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    if (apply_codec(&ctx, codec, quality) != 0) {
        qtsq_free(&ctx);
        return NULL;
    }
    if (ctx.codec == QTSQ_CODEC_ZLIB) {
        qtsq_free(&ctx);
        PyErr_SetString(PyExc_ValueError,
            "weights are qtc-only (custom compressor); codec='zlib' is retired");
        return NULL;
    }

    int level = store ? 2 : (fast ? 1 : 0);
    int rc = qtsq_compress_tensor_quantized(&ctx, (const float *)raw, count,
                                              dims, (uint8_t)ndims, prec, level);
    if (rc != 0) {
        qtsq_free(&ctx);
        if (rc == QTSQ_ERR_TOO_LARGE)
            PyErr_Format(PyExc_RuntimeError,
                "Tensor too large for one stream (prec=%s): use precision='raw' "
                "(auto-chunks) or save_model()/save_model_sharded()", precision_str);
        else
            PyErr_Format(PyExc_RuntimeError, "Tensor compress failed (prec=%s, err=%d)",
                         precision_str, rc);
        return NULL;
    }

    int w_plaintext = 0;
    rc = native_weights_seal_for_write(&ctx, password, &w_plaintext);
    if (rc != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Seal failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    rc = native_weights_write(&ctx, out_path, w_plaintext);
    size_t compressed_sz = (size_t)ctx.singularity_size;
    qtsq_free(&ctx);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Write failed: %s (err=%d)", out_path, rc);
        return NULL;
    }

    return Py_BuildValue("{s:n,s:s,s:K}",
        "count", (Py_ssize_t)count,
        "precision", precision_str,
        "compressed_size", (unsigned long long)compressed_sz);
}

/* ══════════════════════════════════════════════════════
 *  8. decompress_tensor(path) → {data, count, dims}
 * ══════════════════════════════════════════════════════ */
static PyObject* py_decompress_tensor(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;

    qtsq_context_t ctx;
    qtsq_init(&ctx);

    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }

    if (ctx.is_encrypted) {
        if (!password || !password[0]) {
            qtsq_free(&ctx);
            PyErr_SetString(PyExc_RuntimeError,
                "file is encrypted: password required");
            return NULL;
        }
        int rc0 = qtsq_vault_decrypt_password(&ctx, password);
        if (rc0 != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "Decrypt failed — wrong password or corrupted file: %s (err=%d)", path, rc0);
            return NULL;
        }
    }

    float *out = NULL;
    size_t count = 0;
    int rc = qtsq_decompress_tensor(&ctx, &out, &count);

    if (rc != 0 || !out) {
        if (out) free(out);
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Tensor decompress failed: %s (err=%d)", path, rc);
        return NULL;
    }

    /* Build data list */
    PyObject *data_list = PyList_New((Py_ssize_t)count);
    for (size_t i = 0; i < count; i++) {
        PyList_SET_ITEM(data_list, (Py_ssize_t)i, PyFloat_FromDouble((double)out[i]));
    }

    /* Build dims tuple */
    uint8_t ndims = ctx.schema.num_dims;
    PyObject *dims_tuple = PyTuple_New(ndims);
    for (int i = 0; i < ndims; i++) {
        PyTuple_SET_ITEM(dims_tuple, i, PyLong_FromUnsignedLong(ctx.schema.dimensions[i]));
    }

    free(out);

    PyObject *result = Py_BuildValue("{s:O,s:n,s:O,s:s}",
        "data", data_list,
        "count", (Py_ssize_t)count,
        "dims", dims_tuple,
        "encoding", ctx.schema.encoding);

    Py_DECREF(data_list);
    Py_DECREF(dims_tuple);
    qtsq_free(&ctx);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  9. brain_save(path, weights, biases, memories, vector_dim, metadata_json)
 * ══════════════════════════════════════════════════════ */
static PyObject* py_brain_save(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    PyObject *weights_obj, *biases_obj, *memories_obj;
    unsigned int vector_dim = 0;
    const char *metadata_json = NULL;
    const char *password = "";
    static char *kwlist[] = {"path", "weights", "biases", "memories",
                              "vector_dim", "metadata_json", "password", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sOOO|Iss", kwlist,
                                      &path, &weights_obj, &biases_obj,
                                      &memories_obj, &vector_dim, &metadata_json,
                                      &password))
        return NULL;

    if (!password || !password[0]) {
        PyErr_SetString(PyExc_RuntimeError, "password required");
        return NULL;
    }

    /* Helper: convert Python list → float array */
    #define PY_TO_FLOAT_ARRAY(obj, arr, cnt) do { \
        PyObject *seq = PySequence_Fast(obj, #obj " must be a sequence"); \
        if (!seq) return NULL; \
        cnt = (size_t)PySequence_Fast_GET_SIZE(seq); \
        arr = (float *)malloc(cnt * sizeof(float)); \
        if (!arr) { Py_DECREF(seq); return PyErr_NoMemory(); } \
        for (size_t _i = 0; _i < cnt; _i++) \
            arr[_i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, (Py_ssize_t)_i)); \
        Py_DECREF(seq); \
        if (PyErr_Occurred()) { free(arr); return NULL; } \
    } while(0)

    float *weights = NULL, *biases = NULL, *memories = NULL;
    size_t wc = 0, bc = 0, mc = 0;

    PY_TO_FLOAT_ARRAY(weights_obj, weights, wc);
    PY_TO_FLOAT_ARRAY(biases_obj, biases, bc);
    PY_TO_FLOAT_ARRAY(memories_obj, memories, mc);

    #undef PY_TO_FLOAT_ARRAY

    /* Compute num_memories */
    size_t num_memories = (vector_dim > 0) ? mc / vector_dim : 0;

    /* Build the brain-state container directly (bypassing qtsq_brain_save,
     * which is keystore-gated and fails when no keystore is initialized).
     * This mirrors the C layout: 4 streams (weights, biases, memories,
     * metadata), then pack + password-encrypt + write. */
    qtsq_context_t container;
    if (qtsq_init(&container) != 0 || qtsq_container_create(&container) != 0) {
        free(weights); free(biases); free(memories);
        PyErr_SetString(PyExc_RuntimeError, "qtsq_container_create failed");
        return NULL;
    }

    #define ADD_TENSOR_STREAM(arr, cnt, name, dims_arr, ndims_arr) do { \
        if (arr && cnt > 0) { \
            qtsq_context_t sub; qtsq_init(&sub); \
            int _r = qtsq_compress_tensor(&sub, arr, cnt, dims_arr, ndims_arr, 0); \
            if (_r == QTSQ_OK) _r = qtsq_container_add_stream(&container, &sub, name); \
            qtsq_free(&sub); \
            if (_r != QTSQ_OK) { \
                free(weights); free(biases); free(memories); \
                qtsq_free(&container); \
                PyErr_Format(PyExc_RuntimeError, "brain stream %s failed (err=%d)", name, _r); \
                return NULL; \
            } \
        } \
    } while(0)

    if (weights && wc > 0) {
        uint32_t dims[1] = { (uint32_t)wc };
        ADD_TENSOR_STREAM(weights, wc, "network_weights", dims, 1);
    }
    if (biases && bc > 0) {
        uint32_t dims[1] = { (uint32_t)bc };
        ADD_TENSOR_STREAM(biases, bc, "neuron_biases", dims, 1);
    }
    if (memories && num_memories > 0 && vector_dim > 0) {
        size_t total_floats = num_memories * vector_dim;
        uint32_t dims[2] = { (uint32_t)num_memories, (uint32_t)vector_dim };
        ADD_TENSOR_STREAM(memories, total_floats, "memories", dims, 2);
    }
    #undef ADD_TENSOR_STREAM

    free(weights); free(biases); free(memories);

    /* Stream 4: metadata (JSON text) */
    if (metadata_json && strlen(metadata_json) > 0) {
        qtsq_context_t sub;
        qtsq_init(&sub);
        int r = qtsq_compress_text(&sub, metadata_json, strlen(metadata_json));
        if (r == QTSQ_OK) r = qtsq_container_add_stream(&container, &sub, "metadata");
        qtsq_free(&sub);
        if (r != QTSQ_OK) {
            qtsq_free(&container);
            PyErr_Format(PyExc_RuntimeError, "metadata stream failed (err=%d)", r);
            return NULL;
        }
    }

    /* Pack + encrypt + write */
    int rc = qtsq_container_pack(&container);
    if (rc != QTSQ_OK) {
        qtsq_free(&container);
        PyErr_Format(PyExc_RuntimeError, "container_pack failed (err=%d)", rc);
        return NULL;
    }

    rc = native_encrypt_for_write(&container, password);
    if (rc != QTSQ_OK) {
        qtsq_free(&container);
        PyErr_Format(PyExc_RuntimeError, "Brain encryption failed: %s (err=%d)", path, rc);
        return NULL;
    }

    rc = qtsq_write(&container, path);
    qtsq_free(&container);

    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "Encrypted brain write failed: %s (err=%d)", path, rc);
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════
 *  10. brain_load(path) → dict
 * ══════════════════════════════════════════════════════ */
static PyObject* py_brain_load(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;

    if (!password || !password[0]) {
        PyErr_SetString(PyExc_RuntimeError, "password required");
        return NULL;
    }

    /* Read + decrypt the container, then extract brain streams.
     * Previously this called qtsq_brain_load (which can't decrypt) and
     * discarded the password — so no encrypted brain file could be loaded. */
    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    int rc = qtsq_read(&ctx, path);
    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Failed to read brain file: %s (err=%d)", path, rc);
        return NULL;
    }

    rc = qtsq_vault_decrypt_password(&ctx, password);
    if (rc != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_RuntimeError, "Brain decrypt failed: %s (err=%d)", path, rc);
        return NULL;
    }

    /* Extract streams by name */
    float *weights = NULL, *biases = NULL, *memories = NULL;
    size_t wc = 0, bc = 0, mc = 0, vd = 0;
    char *metadata = NULL;

    uint32_t num_streams = 0;
    qtsq_container_get_count(&ctx, &num_streams);

    for (uint32_t i = 0; i < num_streams; i++) {
        char name[256];
        qtsq_context_t stream;
        rc = qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name));
        if (rc != QTSQ_OK) continue;

        if (strcmp(name, "network_weights") == 0) {
            qtsq_decompress_tensor(&stream, &weights, &wc);
        } else if (strcmp(name, "neuron_biases") == 0) {
            qtsq_decompress_tensor(&stream, &biases, &bc);
        } else if (strcmp(name, "memories") == 0) {
            qtsq_decompress_tensor(&stream, &memories, &mc);
            /* mc comes back as total floats; derive num_memories + vector_dim */
            if (stream.schema.num_dims >= 2) {
                vd = stream.schema.dimensions[1];
            }
        } else if (strcmp(name, "metadata") == 0) {
            uint8_t *raw = NULL;
            size_t raw_size = 0;
            if (qtsq_decompress(&stream, &raw, &raw_size) == QTSQ_OK && raw) {
                metadata = (char *)malloc(raw_size + 1);
                if (metadata) {
                    memcpy(metadata, raw, raw_size);
                    metadata[raw_size] = '\0';
                }
                free(raw);
            }
        }
        qtsq_free(&stream);
    }
    qtsq_free(&ctx);

    /* Compute num_memories from total floats / vector_dim */
    size_t num_memories = (vd > 0) ? mc / vd : 0;

    /* Build Python lists */
    PyObject *w_list = PyList_New((Py_ssize_t)wc);
    for (size_t i = 0; i < wc; i++)
        PyList_SET_ITEM(w_list, (Py_ssize_t)i, PyFloat_FromDouble((double)weights[i]));

    PyObject *b_list = PyList_New((Py_ssize_t)bc);
    for (size_t i = 0; i < bc; i++)
        PyList_SET_ITEM(b_list, (Py_ssize_t)i, PyFloat_FromDouble((double)biases[i]));

    size_t mem_total = num_memories * vd;
    PyObject *m_list = PyList_New((Py_ssize_t)mem_total);
    for (size_t i = 0; i < mem_total; i++)
        PyList_SET_ITEM(m_list, (Py_ssize_t)i, PyFloat_FromDouble((double)memories[i]));

    PyObject *result = Py_BuildValue("{s:O,s:n,s:O,s:n,s:O,s:n,s:n,s:s}",
        "weights", w_list, "weight_count", (Py_ssize_t)wc,
        "biases", b_list, "bias_count", (Py_ssize_t)bc,
        "memories", m_list, "num_memories", (Py_ssize_t)num_memories,
        "vector_dim", (Py_ssize_t)vd,
        "metadata", metadata ? metadata : "{}");

    Py_DECREF(w_list);
    Py_DECREF(b_list);
    Py_DECREF(m_list);
    free(weights); free(biases); free(memories); free(metadata);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  11. model_save(path, manifest_json, names, payloads, dims_list,
 *                 is_f32_list, elem_list, nibble_list, password)
 *  Exact-weight multi-tensor container: one .qtsq holds a full model.
 *  - names: list[str], payloads: list[bytes] (contiguous little-endian),
 *    dims_list: list[tuple] (possibly () — manifest is authoritative),
 *    is_f32_list: list[int 0/1], elem_list: list[int 1/2/4/8],
 *    nibble_list: list[int 0/1] (int4-packed candidate)
 *  - float32 → TNS2 RAW_BITEXACT (bit-exact, NaN/Inf preserved)
 *  - other dtypes → TNS2 array lens (transpose + delta contest)
 *  - password optional: vault → device keystore → sanitized plaintext
 * ══════════════════════════════════════════════════════ */
/* Cross-tensor parallel build for model_save. Each job compresses one
 * tensor into its own context (never shared); the main thread adds the
 * finished streams in order, so file bytes are deterministic. File-scope
 * for the pthread workers. */
typedef struct {
    char tname[256];
    const uint8_t *buf;
    size_t blen;
    Py_buffer view;
    int has_view;
    uint32_t dims[8];
    uint8_t ndims;
    int isf;
    long elem;
    int nib;
    int level;
    int store_raw; /* level 2 with directly storable bytes: hash only */
    uint8_t hash[32];
    qtsq_context_t sub;
    int rc;
} model_job_t;

typedef struct {
    model_job_t *jobs;
    Py_ssize_t lo, hi;
} model_span_t;

/* Hash one job's source bytes (store path pre-pass). Shared by the
 * pool worker and the serial fallback. */
static void model_job_hash(model_job_t *j) {
    uint8_t el = j->isf ? 4 : (uint8_t)j->elem;
    size_t units = (el && j->blen % el == 0) ? j->blen / el : 0;
    if (units == 0 || units > 0xFFFFFFFFu) {
        j->rc = QTSQ_ERR_FORMAT;
        return;
    }
    j->rc = qtsq_model_hash_inner(j->hash, j->buf, j->blen,
                                  j->isf, el, (uint32_t)units);
}

static void *model_job_fn(void *arg) {
    model_span_t *sp = (model_span_t *)arg;
    for (Py_ssize_t i = sp->lo; i < sp->hi; i++) {
        model_job_t *j = &sp->jobs[i];
        if (j->store_raw) {
            model_job_hash(j);
            continue;
        }
        j->rc = qtsq_model_compress_one(&j->sub, j->buf, j->blen,
                                        j->dims, j->ndims,
                                        j->isf, j->elem, j->nib, j->level);
    }
    return NULL;
}

static int model_ncpu(void) {
#if defined(__unix__) || defined(__APPLE__)
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    if (v >= 1)
        return v > 8 ? 8 : (int)v;
#endif
    return 4;
}

/* Release buffer views held by jobs (called on every exit). */
static void model_release_views(model_job_t *jobs, Py_ssize_t n) {
    if (!jobs) return;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (jobs[i].has_view) {
            PyBuffer_Release(&jobs[i].view);
            jobs[i].has_view = 0;
        }
    }
}

static PyObject* py_model_save(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *manifest_json;
    PyObject *names_obj, *payloads_obj, *dims_list_obj, *is_f32_obj;
    PyObject *elem_obj = NULL, *nibble_obj = NULL;
    PyObject *blob_names_obj = NULL, *blob_kinds_obj = NULL;
    PyObject *blob_channels_obj = NULL;
    PyObject *payloads_blob = NULL;
    PyObject *blob_ctypes_obj = NULL, *blob_params_obj = NULL;
    PyObject *blob_depths_obj = NULL, *blob_policies_obj = NULL;
    const char *password = "";
    int fast = 0, store = 0, durable = 1;
    static char *kwlist[] = {"path", "manifest_json", "names", "payloads",
                             "dims_list", "is_f32_list", "elem_list",
                             "nibble_list", "password", "fast", "store",
                             "durable", "blob_names", "blob_kinds",
                             "blob_ctypes", "blob_params", "blob_depths",
                             "blob_policies", "blob_payloads",
                             "blob_channels", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ssOOOO|OOsiiiOOOOOOOO", kwlist,
            &path, &manifest_json, &names_obj, &payloads_obj,
            &dims_list_obj, &is_f32_obj, &elem_obj, &nibble_obj, &password, &fast, &store,
            &durable,
            &blob_names_obj, &blob_kinds_obj, &blob_ctypes_obj,
            &blob_params_obj, &blob_depths_obj, &blob_policies_obj,
            &payloads_blob, &blob_channels_obj))
        return NULL;
    int level = store ? 2 : (fast ? 1 : 0);
    PyObject *names = PySequence_Fast(names_obj, "names must be a sequence");
    PyObject *payloads = PySequence_Fast(payloads_obj, "payloads must be a sequence");
    PyObject *dims_list = PySequence_Fast(dims_list_obj, "dims_list must be a sequence");
    PyObject *is_f32 = PySequence_Fast(is_f32_obj, "is_f32_list must be a sequence");
    PyObject *elems = elem_obj ? PySequence_Fast(elem_obj, "elem_list must be a sequence") : NULL;
    PyObject *nibbles = nibble_obj ? PySequence_Fast(nibble_obj, "nibble_list must be a sequence") : NULL;
    if (!names || !payloads || !dims_list || !is_f32) {
        Py_XDECREF(names); Py_XDECREF(payloads);
        Py_XDECREF(dims_list); Py_XDECREF(is_f32);
        Py_XDECREF(elems); Py_XDECREF(nibbles);
        return NULL;
    }
#define MODEL_DECREF_ALL do { \
        Py_DECREF(names); Py_DECREF(payloads); \
        Py_DECREF(dims_list); Py_DECREF(is_f32); \
        Py_XDECREF(elems); Py_XDECREF(nibbles); \
    } while (0)
    Py_ssize_t n = PySequence_Fast_GET_SIZE(names);
    if (PySequence_Fast_GET_SIZE(payloads) != n ||
        PySequence_Fast_GET_SIZE(dims_list) != n ||
        PySequence_Fast_GET_SIZE(is_f32) != n ||
        (elems && PySequence_Fast_GET_SIZE(elems) != n) ||
        (nibbles && PySequence_Fast_GET_SIZE(nibbles) != n)) {
        MODEL_DECREF_ALL;
        PyErr_SetString(PyExc_ValueError, "model arg length mismatch");
        return NULL;
    }
    qtsq_context_t container;
    if (qtsq_init(&container) != 0 || qtsq_model_create(&container) != 0) {
        MODEL_DECREF_ALL;
        PyErr_SetString(PyExc_RuntimeError, "qtsq_model_create failed");
        return NULL;
    }
    /* Phase 1 (serial): parse every tensor into jobs. Buffers borrow
     * the Python bytes (kept alive by `payloads` until the end). */
    model_job_t *jobs = (model_job_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(model_job_t));
    if (!jobs) {
        MODEL_DECREF_ALL;
        qtsq_free(&container);
        return PyErr_NoMemory();
    }
    size_t total_bytes = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        const char *tname = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(names, i));
        if (!tname) goto fail_jobs;
        strncpy(jobs[i].tname, tname, sizeof(jobs[i].tname) - 1);
        /* Any contiguous buffer (bytes, memoryview, ndarray) — no copy. */
        memset(&jobs[i].view, 0, sizeof(Py_buffer));
        if (PyObject_GetBuffer(PySequence_Fast_GET_ITEM(payloads, i),
                               &jobs[i].view, PyBUF_SIMPLE) != 0)
            goto fail_jobs;
        jobs[i].has_view = 1;
        jobs[i].buf = (const uint8_t *)jobs[i].view.buf;
        jobs[i].blen = (size_t)jobs[i].view.len;
        total_bytes += (size_t)jobs[i].view.len;
        long isf = PyLong_AsLong(PySequence_Fast_GET_ITEM(is_f32, i));
        long elem = elems ? PyLong_AsLong(PySequence_Fast_GET_ITEM(elems, i)) : 0;
        long nib = nibbles ? PyLong_AsLong(PySequence_Fast_GET_ITEM(nibbles, i)) : 0;
        if (PyErr_Occurred()) goto fail_jobs;
        jobs[i].isf = isf ? 1 : 0;
        jobs[i].elem = elem;
        jobs[i].nib = nib ? 1 : 0;
        jobs[i].level = store ? 2 : (fast ? 1 : 0);
        /* Store mode skips framing except packed nibbles (which need
         * repacking anyway): hash the source bytes directly. */
        jobs[i].store_raw = (jobs[i].level == 2 && !jobs[i].nib) ? 1 : 0;
        jobs[i].rc = -1;
        {
            PyObject *d = PySequence_Fast_GET_ITEM(dims_list, i);
            uint8_t ndims = 0;
            if (d && d != Py_None) {
                PyObject *ds = PySequence_Fast(d, "dims must be a sequence");
                if (!ds) goto fail_jobs;
                ndims = (uint8_t)PySequence_Fast_GET_SIZE(ds);
                if (ndims > 8) ndims = 8;
                for (int k = 0; k < ndims; k++)
                    jobs[i].dims[k] = (uint32_t)PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(ds, k));
                Py_DECREF(ds);
                if (PyErr_Occurred()) goto fail_jobs;
            }
            jobs[i].ndims = ndims;
        }
        if (jobs[i].isf) {
            if (jobs[i].blen == 0 || jobs[i].blen % 4 != 0) {
                MODEL_DECREF_ALL;
                model_release_views(jobs, n);
                qtsq_free(&container);
                free(jobs);
                PyErr_Format(PyExc_ValueError, "tensor '%s' is not float32 bytes", jobs[i].tname);
                return NULL;
            }
        } else {
            uint8_t el = (jobs[i].elem == 1 || jobs[i].elem == 2 ||
                          jobs[i].elem == 4 || jobs[i].elem == 8)
                         ? (uint8_t)jobs[i].elem : 1;
            if (jobs[i].blen == 0 || jobs[i].blen % el != 0) {
                MODEL_DECREF_ALL;
                model_release_views(jobs, n);
                qtsq_free(&container);
                free(jobs);
                PyErr_Format(PyExc_ValueError, "tensor '%s' bytes mismatch elem size", jobs[i].tname);
                return NULL;
            }
        }
        continue;
fail_jobs:
        MODEL_DECREF_ALL;
        model_release_views(jobs, n);
        qtsq_free(&container);
        free(jobs);
        return NULL;
    }
    /* Phase 2: compress. Thread pool when worthwhile (≥2 tensors and
     * ≥1 MiB); each worker owns its contexts. GIL released: pure C. */
    {
        int use_pool = 0;
#ifdef NATIVE_HAVE_THREADS
        use_pool = (n >= 2 && total_bytes >= ((size_t)1 << 20));
#endif
        if (use_pool) {
#ifdef NATIVE_HAVE_THREADS
            int nw = model_ncpu();
            if (nw > n) nw = (int)n;
            model_span_t spans[8];
            pthread_t tids[8];
            for (int w = 0; w < nw; w++) {
                spans[w].jobs = jobs;
                spans[w].lo = (Py_ssize_t)((int64_t)n * w / nw);
                spans[w].hi = (Py_ssize_t)((int64_t)n * (w + 1) / nw);
            }
            int nstarted = 0;
            Py_BEGIN_ALLOW_THREADS
            for (int w = 0; w < nw; w++) {
                if (pthread_create(&tids[w], NULL, model_job_fn, &spans[w]) == 0)
                    nstarted++;
                else
                    break;
            }
            for (int w = nstarted; w < nw; w++) {
                /* Serial fallback for unstarted spans. */
                for (Py_ssize_t i = spans[w].lo; i < spans[w].hi; i++) {
                    model_job_t *j = &jobs[i];
                    if (j->store_raw)
                        model_job_hash(j);
                    else
                        j->rc = qtsq_model_compress_one(&j->sub, j->buf, j->blen,
                                                        j->dims, j->ndims,
                                                        j->isf, j->elem, j->nib, j->level);
                }
            }
            for (int w = 0; w < nstarted; w++)
                pthread_join(tids[w], NULL);
            Py_END_ALLOW_THREADS
#else
            (void)0;
#endif
        } else {
            for (Py_ssize_t i = 0; i < n; i++) {
                model_job_t *j = &jobs[i];
                if (j->store_raw)
                    model_job_hash(j);
                else
                    j->rc = qtsq_model_compress_one(&j->sub, j->buf, j->blen,
                                                    j->dims, j->ndims,
                                                    j->isf, j->elem, j->nib, j->level);
            }
        }
    }
    /* Phase 3 (serial, in order): hand finished streams to the
     * container — file bytes are deterministic. Store-mode streams
     * carry prebuilt headers + borrowed bytes (no framing copy). */
    for (Py_ssize_t i = 0; i < n; i++) {
        int rc;
        if (jobs[i].rc != QTSQ_OK) {
            rc = jobs[i].rc;
            qtsq_free(&jobs[i].sub);
            for (Py_ssize_t q = 0; q < n; q++) {
                if (q != i) qtsq_free(&jobs[q].sub);
            }
            MODEL_DECREF_ALL;
            model_release_views(jobs, n);
            qtsq_free(&container);
            free(jobs);
            PyErr_Format(PyExc_RuntimeError, "tensor '%s' compress failed (err=%d)",
                         jobs[i].tname, rc);
            return NULL;
        }
        if (jobs[i].store_raw) {
            uint8_t el = jobs[i].isf ? 4 : (uint8_t)jobs[i].elem;
            rc = qtsq_model_add_raw_hashed(&container, jobs[i].tname,
                                           jobs[i].buf, jobs[i].blen,
                                           jobs[i].isf, el,
                                           jobs[i].dims, jobs[i].ndims,
                                           jobs[i].hash);
        } else {
            rc = qtsq_container_add_stream_take(&container, &jobs[i].sub, jobs[i].tname);
            qtsq_free(&jobs[i].sub);
        }
        if (rc != QTSQ_OK) {
            for (Py_ssize_t q = i + 1; q < n; q++) qtsq_free(&jobs[q].sub);
            MODEL_DECREF_ALL;
            model_release_views(jobs, n);
            qtsq_free(&container);
            free(jobs);
            PyErr_Format(PyExc_RuntimeError, "tensor '%s' add failed (err=%d)",
                         jobs[i].tname, rc);
            return NULL;
        }
    }
    model_release_views(jobs, n);
    free(jobs);

    /* Typed blob streams (text/image/audio/table/blob). Added after the
     * tensors and before the manifest so the manifest stays last in the
     * TOC, matching the tensor path. Each parallel list must be the same
     * length; payloads live in blob_payloads_obj. */
    if (blob_names_obj && blob_names_obj != Py_None && payloads_blob) {
        PyObject *bn = PySequence_Fast(blob_names_obj, "blob_names must be a sequence");
        PyObject *bk = PySequence_Fast(blob_kinds_obj, "blob_kinds must be a sequence");
        PyObject *bc = PySequence_Fast(blob_ctypes_obj, "blob_ctypes must be a sequence");
        PyObject *bp = PySequence_Fast(blob_params_obj, "blob_params must be a sequence");
        PyObject *bd = PySequence_Fast(blob_depths_obj, "blob_depths must be a sequence");
        PyObject *bo = PySequence_Fast(blob_policies_obj, "blob_policies must be a sequence");
        PyObject *bpay = PySequence_Fast(payloads_blob, "blob_payloads must be a sequence");
        if (!bn || !bk || !bc || !bp || !bd || !bo || !bpay) {
            Py_XDECREF(bn); Py_XDECREF(bk); Py_XDECREF(bc);
            Py_XDECREF(bp); Py_XDECREF(bd); Py_XDECREF(bo); Py_XDECREF(bpay);
            qtsq_free(&container);
            return NULL;
        }
        Py_ssize_t m = PySequence_Fast_GET_SIZE(bn);
        /* blob_channels is optional: absent means "not a channeled kind",
         * which is what plain blob callers pass. Synthesize zeros so the
         * rest of the loop stays uniform. */
        PyObject *bch = NULL;
        if (blob_channels_obj && blob_channels_obj != Py_None) {
            bch = PySequence_Fast(blob_channels_obj, "blob_channels must be a sequence");
            if (!bch) {
                Py_DECREF(bn); Py_XDECREF(bk); Py_XDECREF(bc); Py_XDECREF(bp);
                Py_XDECREF(bd); Py_XDECREF(bo); Py_XDECREF(bpay);
                qtsq_free(&container);
                return NULL;
            }
        } else {
            bch = PyList_New(m);
            if (!bch) { /* handled by the null check below */ }
            else for (Py_ssize_t z = 0; z < m; z++)
                PyList_SET_ITEM(bch, z, PyLong_FromLong(0));
        }
        if (m != PySequence_Fast_GET_SIZE(bk) || m != PySequence_Fast_GET_SIZE(bc) ||
            m != PySequence_Fast_GET_SIZE(bp) || m != PySequence_Fast_GET_SIZE(bd) ||
            m != PySequence_Fast_GET_SIZE(bo) || m != PySequence_Fast_GET_SIZE(bpay) ||
            (bch && PySequence_Fast_GET_SIZE(bch) != m)) {
            Py_DECREF(bn); Py_DECREF(bk); Py_DECREF(bc); Py_DECREF(bp);
            Py_DECREF(bd); Py_DECREF(bo); Py_DECREF(bpay); Py_DECREF(bch);
            qtsq_free(&container);
            PyErr_SetString(PyExc_ValueError,
                            "blob_names/kinds/ctypes/params/depths/policies/payloads/"
                            "channels must all be the same length");
            return NULL;
        }
        for (Py_ssize_t i = 0; i < m; i++) {
            const char *bname = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(bn, i));
            long kind = PyLong_AsLong(PySequence_Fast_GET_ITEM(bk, i));
            long ctype = PyLong_AsLong(PySequence_Fast_GET_ITEM(bc, i));
            long depth = PyLong_AsLong(PySequence_Fast_GET_ITEM(bd, i));
            long policy = PyLong_AsLong(PySequence_Fast_GET_ITEM(bo, i));
            long bchan = bch ? PyLong_AsLong(PySequence_Fast_GET_ITEM(bch, i)) : 0;
            PyObject *pair = PySequence_Fast(PySequence_Fast_GET_ITEM(bp, i),
                                             "each blob param must be (p0, p1)");
            if (!bname || PyErr_Occurred()) { Py_XDECREF(pair); break; }
            /* The parallel-list length check above only proves the
             * lists agree with each other, NOT that each entry is a
             * 2-sequence. GET_ITEM is a raw ob_item[] with no bounds
             * or NULL check, so an empty or 1-element pair crashed. */
            if (PySequence_Fast_GET_SIZE(pair) < 2) {
                PyErr_SetString(PyExc_ValueError,
                    "each blob param must be a (p0, p1) pair");
                Py_DECREF(pair);
                break;
            }
            long p0 = PyLong_AsLong(PySequence_Fast_GET_ITEM(pair, 0));
            long p1 = PyLong_AsLong(PySequence_Fast_GET_ITEM(pair, 1));
            Py_XDECREF(pair);
            if (PyErr_Occurred()) break;
            Py_buffer bv;
            memset(&bv, 0, sizeof(bv));
            if (PyObject_GetBuffer(PySequence_Fast_GET_ITEM(bpay, i), &bv, PyBUF_SIMPLE) != 0)
                break;
            int rc = qtsq_model_add_blob(&container, bname,
                                         (const uint8_t *)bv.buf, (size_t)bv.len,
                                         (uint8_t)kind, (uint16_t)ctype,
                                         (uint32_t)p0, (uint32_t)p1,
                                         (uint32_t)bchan,
                                         (uint8_t)depth, (uint8_t)policy, level);
            PyBuffer_Release(&bv);
            if (rc != QTSQ_OK) {
                Py_DECREF(bn); Py_XDECREF(bk); Py_XDECREF(bc); Py_XDECREF(bp);
                Py_XDECREF(bd); Py_XDECREF(bo); Py_XDECREF(bpay); Py_XDECREF(bch);
                qtsq_free(&container);
                PyErr_Format(PyExc_RuntimeError,
                             "blob add failed for %s (err=%d)", bname, rc);
                return NULL;
            }
        }
        Py_DECREF(bn); Py_DECREF(bk); Py_DECREF(bc); Py_DECREF(bp);
        Py_DECREF(bd); Py_DECREF(bo); Py_DECREF(bpay); Py_DECREF(bch);
        if (PyErr_Occurred()) { qtsq_free(&container); return NULL; }
    }
    {
        /* Store files keep the manifest raw too (no horizon pass). */
        int rc = store ? qtsq_model_add_manifest_raw(&container, manifest_json)
                       : qtsq_model_add_manifest(&container, manifest_json);
        if (rc != QTSQ_OK) {
            MODEL_DECREF_ALL;
            qtsq_free(&container);
            PyErr_Format(PyExc_RuntimeError, "manifest add failed (err=%d)", rc);
            return NULL;
        }
        /* Pack + seal + write in one step: the plaintext vs encrypted
         * layout is decided before packing (stream-name handling). */
        rc = qtsq_model_pack_write(&container, path, password, durable);
        qtsq_free(&container);
        MODEL_DECREF_ALL;
        if (rc != 0) {
            PyErr_Format(PyExc_RuntimeError, "write failed: %s (err=%d)", path, rc);
            return NULL;
        }
    }
    Py_RETURN_NONE;
#undef MODEL_DECREF_ALL
}

/* ══════════════════════════════════════════════════════
 *  12. model_load(path, password) → {manifest: bytes, tensors: {name: bytes}}
 *  Decompresses each stream to raw bytes (f32 via tensor decode,
 *  others via generic decompress). Python rebuilds dtype/shape.
 * ══════════════════════════════════════════════════════ */
static PyObject* py_model_load(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;
    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }
    if (ctx.is_encrypted) {
        if (!password || !password[0]) {
            qtsq_free(&ctx);
            PyErr_SetString(PyExc_RuntimeError, "password required");
            return NULL;
        }
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "decrypt failed — wrong password or corrupted file (err=%d)", rc);
            return NULL;
        }
    }
    uint32_t num_streams = 0;
    if (qtsq_container_get_count(&ctx, &num_streams) != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_SetString(PyExc_RuntimeError, "not a model container");
        return NULL;
    }
    PyObject *tensors = PyDict_New();
    PyObject *manifest = NULL;
    if (!tensors) { qtsq_free(&ctx); return PyErr_NoMemory(); }
    for (uint32_t i = 0; i < num_streams; i++) {
        qtsq_context_t stream;
        char name[256];
        if (qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name)) != QTSQ_OK)
            continue;
        uint8_t *raw = NULL;
        size_t raw_size = 0;
        /* f32 streams → tensor decode; array modes (5..8) → array
         * decode (bytes); anything else → generic decompress. */
        float *fdata = NULL;
        size_t fcount = 0;
        uint8_t *adata = NULL;
        size_t asize = 0;
        /* BLB1 typed streams (text/image/audio/table/blob) are framed
         * their own way, so they must be tried FIRST — otherwise the
         * tensor/array ladders would read the 64-byte blob header as a
         * TNS2 envelope and misinterpret the geometry. */
        int is_blob = (stream.singularity_size >= sizeof(qtsq_blob_header_t) &&
                       qtsq_model_blob_kind((const qtsq_blob_header_t *)stream.singularity) >= 0);
        if (is_blob &&
            qtsq_model_decompress_blob(stream.singularity, stream.singularity_size,
                                       &raw, &raw_size, NULL) == QTSQ_OK && raw) {
            PyObject *b = PyBytes_FromStringAndSize((const char *)raw, (Py_ssize_t)raw_size);
            free(raw);
            qtsq_free(&stream);
            if (!b) { Py_DECREF(tensors); Py_XDECREF(manifest); qtsq_free(&ctx); return PyErr_NoMemory(); }
            if (strcmp(name, "__manifest__") == 0) {
                Py_XDECREF(manifest);
                manifest = b;
            } else { PyDict_SetItemString(tensors, name, b); Py_DECREF(b); }
            continue;
        }
        if (qtsq_decompress_tensor(&stream, &fdata, &fcount) == QTSQ_OK && fdata) {
            raw_size = fcount * sizeof(float);
            PyObject *b = PyBytes_FromStringAndSize((const char *)fdata, (Py_ssize_t)raw_size);
            free(fdata);
            qtsq_free(&stream);
            if (!b) { Py_DECREF(tensors); Py_XDECREF(manifest); qtsq_free(&ctx); return PyErr_NoMemory(); }
            if (strcmp(name, "__manifest__") == 0) {
                Py_XDECREF(manifest);
                manifest = b; /* own ref */
            } else { PyDict_SetItemString(tensors, name, b); Py_DECREF(b); }
        } else if (qtsq_decompress_array(&stream, &adata, &asize, NULL) == QTSQ_OK && adata) {
            PyObject *b = PyBytes_FromStringAndSize((const char *)adata, (Py_ssize_t)asize);
            free(adata);
            qtsq_free(&stream);
            if (!b) { Py_DECREF(tensors); Py_XDECREF(manifest); qtsq_free(&ctx); return PyErr_NoMemory(); }
            if (strcmp(name, "__manifest__") == 0) {
                Py_XDECREF(manifest);
                manifest = b; /* own ref */
            } else { PyDict_SetItemString(tensors, name, b); Py_DECREF(b); }
        } else if (qtsq_decompress(&stream, &raw, &raw_size) == QTSQ_OK && raw) {
            PyObject *b = PyBytes_FromStringAndSize((const char *)raw, (Py_ssize_t)raw_size);
            free(raw);
            qtsq_free(&stream);
            if (!b) { Py_DECREF(tensors); Py_XDECREF(manifest); qtsq_free(&ctx); return PyErr_NoMemory(); }
            if (strcmp(name, "__manifest__") == 0) {
                Py_XDECREF(manifest);
                manifest = b; /* own ref */
            } else { PyDict_SetItemString(tensors, name, b); Py_DECREF(b); }
        } else {
            qtsq_free(&stream);
        }
    }
    qtsq_free(&ctx);
    if (!manifest) {
        manifest = PyBytes_FromStringAndSize("", 0);
        if (!manifest) { Py_DECREF(tensors); return PyErr_NoMemory(); }
    }
    PyObject *result = Py_BuildValue("{s:O,s:O}", "manifest", manifest, "tensors", tensors);
    Py_DECREF(manifest);
    Py_DECREF(tensors);
    return result;
}

/* ══════════════════════════════════════════════════════
 *  12b. model_info(path, password) → manifest bytes (no tensors)
 *  Lists a checkpoint without decompressing weights.
 * ══════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
 *  image_probe / image_decode — in-memory image file decode
 * ═══════════════════════════════════════════════════════════════ */

static PyObject* py_image_probe(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_buffer vb;
    static char *kwlist[] = {"data", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*", kwlist, &vb))
        return NULL;
    uint32_t w = 0, h = 0; uint8_t ch = 0;
    int rc = qtsq_image_probe((const uint8_t *)vb.buf, (size_t)vb.len, &w, &h, &ch);
    PyBuffer_Release(&vb);
    if (rc != QTSQ_IMAGE_OK) {
        PyErr_Format(PyExc_ValueError, "not a decodable image: %s",
                     qtsq_image_error_string(rc));
        return NULL;
    }
    return Py_BuildValue("IIB", w, h, ch);
}

static PyObject* py_image_decode(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_buffer vb;
    static char *kwlist[] = {"data", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*", kwlist, &vb))
        return NULL;
    uint8_t *px = NULL; size_t n = 0;
    uint32_t w = 0, h = 0; uint8_t ch = 0;
    int rc = qtsq_image_decode((const uint8_t *)vb.buf, (size_t)vb.len,
                               &px, &n, &w, &h, &ch);
    PyBuffer_Release(&vb);
    if (rc != QTSQ_IMAGE_OK || !px) {
        free(px);
        PyErr_Format(PyExc_ValueError, "image decode failed: %s",
                     qtsq_image_error_string(rc));
        return NULL;
    }
    PyObject *b = PyBytes_FromStringAndSize((const char *)px, (Py_ssize_t)n);
    free(px);
    if (!b) return NULL;
    return Py_BuildValue("NIIB", b, w, h, ch);
}

static PyObject* py_model_info(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    const char *password = "";
    static char *kwlist[] = {"path", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s", kwlist, &path, &password))
        return NULL;
    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "qtsq_init failed");
        return NULL;
    }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }
    if (ctx.is_encrypted) {
        if (!password || !password[0]) {
            qtsq_free(&ctx);
            PyErr_SetString(PyExc_RuntimeError, "file is encrypted: password required");
            return NULL;
        }
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx);
            PyErr_Format(PyExc_RuntimeError, "decrypt failed — wrong password or corrupted file (err=%d)", rc);
            return NULL;
        }
    }
    uint32_t num_streams = 0;
    if (qtsq_container_get_count(&ctx, &num_streams) != QTSQ_OK) {
        qtsq_free(&ctx);
        PyErr_SetString(PyExc_RuntimeError, "not a model container");
        return NULL;
    }
    PyObject *result = NULL;
    for (uint32_t i = 0; i < num_streams; i++) {
        qtsq_context_t stream;
        char name[256];
        if (qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name)) != QTSQ_OK)
            continue;
        if (strcmp(name, "__manifest__") == 0) {
            uint8_t *raw = NULL;
            size_t raw_size = 0;
            if (qtsq_decompress(&stream, &raw, &raw_size) == QTSQ_OK && raw) {
                result = PyBytes_FromStringAndSize((const char *)raw, (Py_ssize_t)raw_size);
                free(raw);
            }
            qtsq_free(&stream);
            break;
        }
        qtsq_free(&stream);
    }
    qtsq_free(&ctx);
    if (!result) {
        PyErr_SetString(PyExc_RuntimeError, "manifest stream not found");
        return NULL;
    }
    return result;
}

/* ══════════════════════════════════════════════════════
 *  12c. model_load_some(path, names, password) → {manifest, tensors}
 *  Lazy per-tensor read for inference engines: only the requested
 *  streams are decompressed; the rest are skipped untouched.
 *
 *  Plaintext V3 files take an mmap fast path: the container TOC is
 *  walked in the mapping and only wanted payloads are faulted in —
 *  a 14 GB file serving 2 tensors touches megabytes, not gigabytes.
 *  Encrypted (or old-version) files use the full read+decrypt path.
 * ══════════════════════════════════════════════════════ */
/* Shared subset-gather: decompress manifest + wanted streams only. */
static int model_gather_some(qtsq_context_t *ctx, PyObject *want,
                             PyObject *tensors, PyObject **manifest_out) {
    Py_ssize_t nw = PySequence_Fast_GET_SIZE(want);
    uint32_t num_streams = 0;
    if (qtsq_container_get_count(ctx, &num_streams) != QTSQ_OK)
        return -1;
    PyObject *manifest = NULL;
    for (uint32_t i = 0; i < num_streams; i++) {
        /* Name first: skipped streams cost no payload copy. */
        char name[256];
        if (qtsq_container_get_stream_name(ctx, i, name, sizeof(name)) != QTSQ_OK)
            continue;
        int is_manifest = (strcmp(name, "__manifest__") == 0);
        int wanted = is_manifest;
        if (!wanted) {
            for (Py_ssize_t k = 0; k < nw; k++) {
                PyObject *item = PySequence_Fast_GET_ITEM(want, k);
                const char *wname = PyUnicode_AsUTF8(item);
                if (wname && strcmp(name, wname) == 0) { wanted = 1; break; }
                if (PyErr_Occurred()) break;
            }
            if (PyErr_Occurred()) { PyErr_Clear(); continue; }
        }
        if (!wanted)
            continue;
        qtsq_context_t stream;
        if (qtsq_container_get_stream(ctx, i, &stream, NULL, 0) != QTSQ_OK)
            continue;
        uint8_t *raw = NULL;
        size_t raw_size = 0;
        float *fdata = NULL;
        size_t fcount = 0;
        uint8_t *adata = NULL;
        size_t asize = 0;
        PyObject *b = NULL;
        /* BLB1 typed streams first: their 64-byte header must not be
         * mistaken for a TNS2 envelope by the tensor ladder below. */
        int is_blob = (stream.singularity_size >= sizeof(qtsq_blob_header_t) &&
                       qtsq_model_blob_kind((const qtsq_blob_header_t *)stream.singularity) >= 0);
        if (is_blob &&
            qtsq_model_decompress_blob(stream.singularity, stream.singularity_size,
                                       &raw, &raw_size, NULL) == QTSQ_OK && raw) {
            b = PyBytes_FromStringAndSize((const char *)raw, (Py_ssize_t)raw_size);
            free(raw);
        }
        if (!b && qtsq_decompress_tensor(&stream, &fdata, &fcount) == QTSQ_OK && fdata) {
            b = PyBytes_FromStringAndSize((const char *)fdata, (Py_ssize_t)(fcount * sizeof(float)));
            free(fdata);
        } else if (qtsq_decompress_array(&stream, &adata, &asize, NULL) == QTSQ_OK && adata) {
            b = PyBytes_FromStringAndSize((const char *)adata, (Py_ssize_t)asize);
            free(adata);
        } else if (qtsq_decompress(&stream, &raw, &raw_size) == QTSQ_OK && raw) {
            b = PyBytes_FromStringAndSize((const char *)raw, (Py_ssize_t)raw_size);
            free(raw);
        }
        qtsq_free(&stream);
        if (!b) {
            Py_XDECREF(manifest);
            return -1;
        }
        if (is_manifest) {
            Py_XDECREF(manifest);
            manifest = b; /* own ref */
        } else {
            if (PyDict_SetItemString(tensors, name, b) != 0) {
                Py_DECREF(b);
                Py_XDECREF(manifest);
                return -1;
            }
            Py_DECREF(b);
        }
    }
    if (!manifest)
        return -1;
    *manifest_out = manifest;
    return 0;
}

#ifdef NATIVE_HAVE_MMAP
/* mmap fast path for plaintext V3 containers: parse the header + TOC in
 * the mapping and gather without reading the whole file. Returns 1 when
 * handled (result or error set), 0 to fall back to the full read path
 * (encrypted, old version, or anything unexpected). */
static int model_load_some_mmap(const char *path, PyObject *want,
                                PyObject *tensors, PyObject **manifest_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 112) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *map = (uint8_t *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return 0;

    int hard_error = 0;
    do {
        if (map[0] != 'Q' || map[1] != 'T' || map[2] != 'S' ||
            map[3] != '\x01' || map[4] != 3)
            break; /* not V3 → full path */
        uint32_t flags;
        uint8_t data_type;
        uint64_t horizon, accretion, sing_size, sec_size, rad_size;
        memcpy(&flags, map + 6, 4);
        data_type = map[10];
        /* V3 layout (112 bytes): magic(4) ver(1) strat(1) flags(4)
         * type(1) ch(1) depth(2) orig(8) ts(8) checksum(32)
         * horizon(8)@62 accretion(8)@70 sing(8)@78 rad(8)@86
         * secsize(8)@94 intent(1) ... */
        memcpy(&horizon, map + 62, 8);
        memcpy(&accretion, map + 70, 8);
        memcpy(&sing_size, map + 78, 8);
        memcpy(&rad_size, map + 86, 8);
        memcpy(&sec_size, map + 94, 8);
        if (data_type != 0xF0) { /* not a container */
            PyErr_SetString(PyExc_RuntimeError, "not a model container");
            hard_error = 1;
            break;
        }
        if (flags & (0x10u | 0x10000u | 0x20000000u))
            break; /* encrypted → full decrypt path */
        /* Plaintext files still carry a zero-filled security stub
         * (mirroring write_impl); only a NONZERO block means keys. */
        size_t sec_off = 112 + (size_t)horizon + (size_t)accretion +
                         (size_t)sing_size + (size_t)rad_size;
        if (sec_size != 0 && sec_size != sizeof(qtsq_security_block_t))
            break;
        if (sec_size != 0) {
            if (sec_off + sec_size > size || sec_off < 112)
                break;
            /* Salt sits at +32 (version/type/nonce/tag precede it); the
             * name key derives from the salt alone: zero salt = cleartext. */
            if (sec_off + 64 > size)
                break;
            int i;
            for (i = 0; i < 32; i++) {
                if (map[sec_off + 32 + i] != 0)
                    break;
            }
            if (i < 32)
                break; /* salted → full path */
        }
        if (horizon > size || accretion > size || sing_size > size ||
            sing_size > 4000000000ULL)
            break;
        size_t off = 112;
        if (off + (size_t)horizon > size)
            break;
        off += (size_t)horizon;
        /* Walk accretion transforms to find the singularity. */
        if (off + (size_t)accretion > size)
            break;
        size_t acc_end = off + (size_t)accretion;
        while (off < acc_end) {
            uint8_t type;
            uint32_t psize;
            if (off + 5 > size)
                break;
            type = map[off];
            memcpy(&psize, map + off + 1, 4);
            if (psize == 0 && type == 0)
                break;
            if (psize > 65536)
                break;
            off += 5 + (size_t)psize;
        }
        if (off != acc_end)
            off = 112 + (size_t)horizon + (size_t)accretion;
        if (off + (size_t)sing_size > size)
            break;
        qtsq_context_t fctx;
        if (qtsq_init(&fctx) != 0)
            break;
        memcpy(&fctx.header, map, 112);
        fctx.header.version = 3;
        fctx.singularity = map + off;
        fctx.singularity_size = (uint64_t)sing_size;
        /* Plaintext: zero security → cleartext stream names. */
        if (model_gather_some(&fctx, want, tensors, manifest_out) == 0) {
            fctx.singularity = NULL; /* mapped, not owned */
            fctx.singularity_size = 0;
            qtsq_free(&fctx);
            munmap(map, size);
            return 1;
        }
        fctx.singularity = NULL;
        fctx.singularity_size = 0;
        qtsq_free(&fctx);
        break; /* gather failed → full path decides the error */
    } while (0);

    munmap(map, size);
    if (hard_error)
        return 1; /* error already set */
    return 0;
}
#endif

static PyObject* py_model_load_some(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *path;
    PyObject *names_obj;
    const char *password = "";
    static char *kwlist[] = {"path", "names", "password", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|s", kwlist, &path, &names_obj, &password))
        return NULL;
    PyObject *want = PySequence_Fast(names_obj, "names must be a sequence");
    if (!want) return NULL;

    PyObject *tensors = PyDict_New();
    PyObject *manifest = NULL;
    if (!tensors) { Py_DECREF(want); return PyErr_NoMemory(); }

#ifdef NATIVE_HAVE_MMAP
    /* Plaintext fast path first (no password involved). Encrypted files
     * always take the full path below. */
    if (!password || !password[0]) {
        int m = model_load_some_mmap(path, want, tensors, &manifest);
        if (m == 1) {
            PyObject *result;
            Py_DECREF(want);
            if (!manifest) {
                Py_DECREF(tensors);
                if (!PyErr_Occurred())
                    PyErr_SetString(PyExc_RuntimeError, "manifest stream not found");
                return NULL;
            }
            result = Py_BuildValue("{s:O,s:O}", "manifest", manifest, "tensors", tensors);
            Py_DECREF(manifest);
            Py_DECREF(tensors);
            return result;
        }
        /* m == 0 → fall through; manifest stays NULL, tensors empty. */
    }
#endif

    qtsq_context_t ctx;
    if (qtsq_init(&ctx) != 0) { Py_DECREF(want); Py_DECREF(tensors); return NULL; }
    if (qtsq_read(&ctx, path) != 0) {
        qtsq_free(&ctx); Py_DECREF(want); Py_DECREF(tensors);
        PyErr_Format(PyExc_FileNotFoundError, "Failed to read: %s", path);
        return NULL;
    }
    if (ctx.is_encrypted) {
        if (!password || !password[0]) {
            qtsq_free(&ctx); Py_DECREF(want); Py_DECREF(tensors);
            PyErr_SetString(PyExc_RuntimeError, "file is encrypted: password required");
            return NULL;
        }
        int rc = qtsq_vault_decrypt_password(&ctx, password);
        if (rc != QTSQ_OK) {
            qtsq_free(&ctx); Py_DECREF(want); Py_DECREF(tensors);
            PyErr_Format(PyExc_RuntimeError, "decrypt failed — wrong password or corrupted file (err=%d)", rc);
            return NULL;
        }
    }
    if (model_gather_some(&ctx, want, tensors, &manifest) != 0) {
        qtsq_free(&ctx); Py_DECREF(want); Py_DECREF(tensors); Py_XDECREF(manifest);
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "manifest stream not found");
        return NULL;
    }
    qtsq_free(&ctx);
    Py_DECREF(want);
    PyObject *result = Py_BuildValue("{s:O,s:O}", "manifest", manifest, "tensors", tensors);
    Py_DECREF(manifest);
    Py_DECREF(tensors);
    return result;
}

/* ── Module definition ── */

static PyMethodDef qtsq_methods[] = {
    {"read_vertices", (PyCFunction)py_read_vertices, METH_VARARGS|METH_KEYWORDS,
     "Read vertices from a .qtsq file."},
    {"compress_image", (PyCFunction)py_compress_image, METH_VARARGS|METH_KEYWORDS,
     "Compress pixel data to .qtsq file.\n"
     "Args: pixels_bytes, width, height, channels, output_path, mode, password"},
    {"decompress_image", (PyCFunction)py_decompress_image, METH_VARARGS|METH_KEYWORDS,
     "Decompress a .qtsq image file to pixel data.\n"
     "Returns dict: {pixels, width, height, channels}"},
    {"compress_text", (PyCFunction)py_compress_text, METH_VARARGS|METH_KEYWORDS,
     "Compress text string to .qtsq file.\n"
     "Args: text, output_path, password"},
    {"compress_json", (PyCFunction)py_compress_json, METH_VARARGS|METH_KEYWORDS,
     "Minify and compress JSON string to .qtsq file.\n"
     "Args: json_str, output_path, password"},
    {"compress_csv", (PyCFunction)py_compress_csv, METH_VARARGS|METH_KEYWORDS,
     "Compress CSV string to .qtsq file.\n"
     "Args: csv_str, output_path, password"},
    {"decompress_raw", (PyCFunction)py_decompress_raw, METH_VARARGS|METH_KEYWORDS,
     "Decompress a .qtsq file to raw bytes."},
    {"file_info", (PyCFunction)py_file_info, METH_VARARGS|METH_KEYWORDS,
     "Get metadata from a .qtsq file without decompressing."},
    {"compress_tensor", (PyCFunction)py_compress_tensor, METH_VARARGS|METH_KEYWORDS,
     "Compress a float32 array to .qtsq tensor format.\n"
     "Args: data (list/bytes), count, dims (tuple), output_path, precision='f32'"},
    {"compress_tensor_bytes", (PyCFunction)py_compress_tensor_bytes, METH_VARARGS|METH_KEYWORDS,
     "Fast path: compress float32 little-endian bytes to .qtsq tensor format.\n"
     "Args: raw (bytes), dims (tuple), output_path, precision='raw'"},
    {"decompress_tensor", (PyCFunction)py_decompress_tensor, METH_VARARGS|METH_KEYWORDS,
     "Decompress a .qtsq tensor file to float32 list.\n"
     "Returns dict: {data, count, dims}"},
    {"brain_save", (PyCFunction)py_brain_save, METH_VARARGS|METH_KEYWORDS,
     "Save AI brain state to .qtsq container.\n"
     "Args: path, weights, biases, memories, vector_dim, metadata_json"},
    {"brain_load", (PyCFunction)py_brain_load, METH_VARARGS|METH_KEYWORDS,
     "Load AI brain state from .qtsq container.\n"
     "Returns dict: {weights, biases, memories, num_memories, vector_dim, metadata}"},
    {"model_save", (PyCFunction)py_model_save, METH_VARARGS|METH_KEYWORDS,
     "Save exact-weight model container (named tensors, one .qtsq).\n"
     "Password optional: vault → device keystore → sanitized plaintext."},
    {"model_load", (PyCFunction)py_model_load, METH_VARARGS|METH_KEYWORDS,
     "Load exact-weight model container → {manifest, tensors}."},
    {"image_probe", (PyCFunction)py_image_probe, METH_VARARGS|METH_KEYWORDS,
     "Probe an encoded image's geometry from bytes → (w, h, channels)."},
    {"image_decode", (PyCFunction)py_image_decode, METH_VARARGS|METH_KEYWORDS,
     "Decode an encoded image to pixels → (bytes, w, h, channels)."},
    {"model_info", (PyCFunction)py_model_info, METH_VARARGS|METH_KEYWORDS,
     "Read a model manifest without decompressing weights → bytes."},
    {"model_load_some", (PyCFunction)py_model_load_some, METH_VARARGS|METH_KEYWORDS,
     "Load only the named tensor streams → {manifest, tensors}."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef qtsq_module = {
    PyModuleDef_HEAD_INIT,
    "qtsq._native",
    "C extension for .qtsq file operations",
    -1,
    qtsq_methods
};

PyMODINIT_FUNC PyInit__native(void) {
    return PyModule_Create(&qtsq_module);
}
