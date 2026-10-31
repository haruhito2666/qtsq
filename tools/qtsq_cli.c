/*
 * qtsq — Quantum Tensor Sequence CLI
 * Compress, decompress, inspect, encrypt, and manage .qtsq files.
 *
 * Usage:
 *   qtsq compress   <file>                     Compress any file → .qtsq
 *   qtsq decompress <file.qtsq> [-o output]    Decompress .qtsq → original
 *   qtsq info       <file.qtsq>                Inspect header and metadata
 *   qtsq encrypt    <file.qtsq> -p <password>  Encrypt a .qtsq file
 *   qtsq decrypt    <file.qtsq> -p <password>  Decrypt a .qtsq file
 *   qtsq protect    <file.qtsq>                Add ECC recovery data
 *   qtsq repair     <file.qtsq>                Repair corrupted .qtsq file
 *   qtsq diff       <old.qtsq> <new.qtsq> -o <patch.qtsw>
 *   qtsq patch      <file.qtsq> <patch.qtsw>
 *
 * Author: Haruhito
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include "qtsq.h"
#include "qtsq_pixel.h"
#include "qtsq_vault.h"
#include "qtsq_keystore.h"

/* ── Global CLI encryption options ── */
static const char *g_cli_password = NULL;
static int g_cli_use_keystore = 0;

/* Encrypt a context before writing based on CLI flags.
 * Priority: explicit password > keystore auto-encrypt > fail-closed.
 * Guarantee #1 (zero metadata) is enforced inside the vault: when
 * is_sanitized is not already set, qtsq_vault_encrypt_password /
 * qtsq_keystore_encrypt run qtsq_sanitize_context() to move names
 * inside the encryption envelope. */
static int cli_encrypt_for_write(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (g_cli_password) {
        return qtsq_vault_encrypt_password(ctx, g_cli_password);
    }
    if (g_cli_use_keystore && qtsq_keystore_is_initialized()) {
        return qtsq_keystore_encrypt(ctx);
    }
    return QTSQ_ERR_NOT_ENCRYPTED;
}

/* Decrypt a context after reading if it is encrypted and a credential was supplied */
static int cli_decrypt_after_read(qtsq_context_t *ctx) {
    if (!ctx || !ctx->is_encrypted) return QTSQ_OK;
    if (g_cli_password) {
        return qtsq_vault_decrypt_password(ctx, g_cli_password);
    }
    if (g_cli_use_keystore && qtsq_keystore_is_initialized()) {
        return qtsq_keystore_decrypt(ctx);
    }
    return QTSQ_OK; /* leave encrypted if no credential supplied */
}

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../include/stb_image_write.h"

/* ── Helpers ── */

static void print_usage(void) {
    printf(
        "╔══════════════════════════════════════════════════════╗\n"
        "║         qtsq — Quantum Tensor Sequence CLI          ║\n"
        "╚══════════════════════════════════════════════════════╝\n"
        "\n"
        "Usage:\n"
        "  qtsq compress   <file>                     Compress → .qtsq\n"
        "  qtsq decompress <file.qtsq> [-o output]    Decompress → original\n"
        "  qtsq view       <file.qtsq>                View contents\n"
        "  qtsq info       <file.qtsq>                Inspect file\n"
        "  qtsq encrypt    <file.qtsq> -p <password>  Encrypt\n"
        "  qtsq decrypt    <file.qtsq> -p <password>  Decrypt\n"
        "  qtsq protect    <file.qtsq>                Add ECC parity\n"
        "  qtsq repair     <file.qtsq>                Recover corrupted data\n"
        "  qtsq diff       <old> <new> -o <patch>     Generate .qtsw diff\n"
        "  qtsq patch      <file> <patch.qtsw>        Apply .qtsw patch\n"
        "\n"
        "Options:\n"
        "  -o <output>       Specify output filename\n"
        "  -p <password>     Password for encrypt/decrypt\n"
        "  --password <pw>   Global password for all encrypted I/O\n"
        "  --keystore        Use initialized keystore for encryption/decryption\n"
        "  -m <0-5>          Force specific engine (0:Singularity, 1:Accretion, 2:Gravitational, 5:Pixel)\n"
        "  -q <0.0-1.0>      Audio tier (total bitrate): 1.0=320k, 0.85=192k,\n"
        "                    0.5=128k, 0.25=96k; 0=lossless. Decompress: redshift.\n"
        "  --qtc             qtc codec (already the default; kept for compatibility)\n"
        "  --zlib            Opt out of qtc and use the legacy zlib codec instead\n"
        "  --creative        Creative intent (audio: Opus, or qtc MDCT2 when built without Opus)\n"
        "  --provenance      Provenance intent (bit-exact lossless audio)\n"
        "  -W <width>        Specify image width (for raw images)\n"
        "  -H <height>       Specify image height (for raw images)\n"
        "  --help, -h        Show this help\n"
        "\n"
        "Examples:\n"
        "  qtsq compress photo.png\n"
        "  qtsq decompress photo.png.qtsq\n"
        "  qtsq info document.txt.qtsq\n"
        "  qtsq encrypt secret.qtsq -p hunter2\n"
        "  qtsq compress data.csv -o compressed.qtsq\n"
        "  qtsq decompress image.qtsq -q 0.5\n"
        "\n"
    );
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { *out_size = 0; return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); *out_size = 0; return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(fp); *out_size = 0; return NULL; }
    fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    *out_size = (size_t)sz;
    return data;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(data, 1, size, fp);
    fclose(fp);
    return 0;
}

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* 16-bit PCM WAV (RIFF/WAVE, 44-byte canonical header). */
static int write_wav16(const char *path, const int16_t *pcm, size_t nsamples,
                       uint32_t rate, uint16_t channels) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    uint32_t data_bytes = (uint32_t)(nsamples * sizeof(int16_t));
    uint16_t block_align = (uint16_t)(channels * sizeof(int16_t));
    uint32_t byte_rate = rate * block_align;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);          put_u32le(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);     put_u32le(h + 16, 16);
    put_u16le(h + 20, 1);          put_u16le(h + 22, channels);
    put_u32le(h + 24, rate);       put_u32le(h + 28, byte_rate);
    put_u16le(h + 32, block_align);put_u16le(h + 34, 16);
    memcpy(h + 36, "data", 4);     put_u32le(h + 40, data_bytes);
    int ok = fwrite(h, 1, sizeof h, fp) == sizeof h &&
             fwrite(pcm, 1, data_bytes, fp) == data_bytes;
    fclose(fp);
    return ok ? 0 : -1;
}

/* The float32 capture path (qtsq_compress_audio_float) writes PROCEDURAL
 * streams with sample_depth 32 and an audio dtype; the generic decoder
 * hands back interleaved float32 bytes. */
static int is_float32_audio(const qtsq_context_t *ctx) {
    return ctx->header.strategy == QTSQ_STRATEGY_PROCEDURAL &&
           ctx->header.sample_depth == 32 &&
           (ctx->header.data_type == QTSQ_TYPE_AUDIO_MONO ||
            ctx->header.data_type == QTSQ_TYPE_AUDIO_STEREO ||
            ctx->header.data_type == QTSQ_TYPE_AUDIO_MULTI ||
            ctx->header.data_type == QTSQ_TYPE_SIGNAL_1D);
}

static const char *find_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return NULL;
}

static int has_flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return 1;
    }
    return 0;
}

/* Get file extension */
static const char *get_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    return dot ? dot + 1 : "";
}

/* Build default output name: strips original extension, adds new suffix.
 * e.g. "photo.jpg" + ".qtsq" → "photo.qtsq" (NOT "photo.jpg.qtsq") */
static void make_output_name(const char *input, const char *suffix,
                              char *out, size_t out_capacity) {
    /* Find last dot in the basename (not in directory path) */
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;
    const char *dot = strrchr(base, '.');

    if (dot && dot > base) {
        /* Strip original extension, add new suffix */
        size_t stem_len = (size_t)(dot - input);
        if (stem_len + strlen(suffix) >= out_capacity) {
            snprintf(out, out_capacity, "%s%s", input, suffix);
        } else {
            memcpy(out, input, stem_len);
            strcpy(out + stem_len, suffix);
        }
    } else {
        /* No extension, just append */
        snprintf(out, out_capacity, "%s%s", input, suffix);
    }
}

/* Strip .qtsq extension for decompress output */
static void strip_qtsq_extension(const char *input, char *out, size_t cap) {
    size_t len = strlen(input);
    if (len > 5 && strcmp(input + len - 5, ".qtsq") == 0) {
        size_t base_len = len - 5;
        if (base_len >= cap) base_len = cap - 1;
        memcpy(out, input, base_len);
        out[base_len] = '\0';
    } else {
        snprintf(out, cap, "%s.out", input);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  COMPRESS — auto-detect file type and compress
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_compress(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq compress <file> [-o output.qtsq]\n");
        return 1;
    }

    /* Find the positional input file (skip flags like --hd, --lossless, -o, -m, --password) */
    const char *input = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--hd") == 0 || strcmp(argv[i], "--lossless") == 0 ||
            strcmp(argv[i], "--keystore") == 0 || strcmp(argv[i], "--qtc") == 0 ||
            strcmp(argv[i], "--zlib") == 0 ||
            strcmp(argv[i], "--creative") == 0 || strcmp(argv[i], "--provenance") == 0) continue;
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "-m") == 0 ||
            strcmp(argv[i], "-W") == 0 || strcmp(argv[i], "-H") == 0 ||
            strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--password") == 0 ||
            strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quality") == 0) { i++; continue; } /* skip flag + value */
        input = argv[i];
        break;
    }
    if (!input) {
        printf("Usage: qtsq compress <file> [--hd|--lossless] [-o output.qtsq]\n");
        return 1;
    }
    const char *output = find_arg(argc, argv, "-o");
    char out_path[512];
    if (!output) {
        make_output_name(input, ".qtsq", out_path, sizeof(out_path));
        output = out_path;
    }

    int forced_mode = -1;
    const char *m_str = find_arg(argc, argv, "-m");
    if (m_str) forced_mode = atoi(m_str);

    /* --hd flag is shorthand for -m 3 (Gravitational HD) */
    /* --lossless flag is shorthand for -m 4 (Max fidelity 6M vertices) */
    int hd_mode = 0, lossless_mode = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--hd") == 0) { hd_mode = 1; }
        if (strcmp(argv[i], "--lossless") == 0) { lossless_mode = 1; }
    }
    if (lossless_mode && forced_mode < 0) forced_mode = 4;
    else if (hd_mode && forced_mode < 0) forced_mode = 3;

    int custom_w = 0, custom_h = 0;
    const char *w_str = find_arg(argc, argv, "-W");
    const char *h_str = find_arg(argc, argv, "-H");
    if (w_str) custom_w = atoi(w_str);
    if (h_str) custom_h = atoi(h_str);

    /* Read input file */
    size_t file_size;
    uint8_t *data = read_file(input, &file_size);
    if (!data) {
        printf("Error: Cannot read '%s'\n", input);
        return 1;
    }

    printf("Input:  %s (%zu bytes)\n", input, file_size);

    /* Auto-detect type */
    uint8_t detected = qtsq_detect_type(data, file_size);
    printf("Type:   %s\n", qtsq_type_name(detected));

    /* Also check file extension for better detection */
    const char *ext = get_extension(input);

    qtsq_context_t ctx;
    if (has_flag(argc, argv, "--creative")) {
        qtsq_create_creative(&ctx);
        printf("Intent: Creative\n");
    } else if (has_flag(argc, argv, "--provenance")) {
        qtsq_create_provenance(&ctx);
        printf("Intent: Provenance\n");
    } else {
        qtsq_init(&ctx);
    }

    /* Phase 7: qtc is the default codec (set by qtsq_init). --zlib opts out
     * to the legacy zlib codec; --qtc is accepted for compatibility but is
     * already the default. use_qtc mirrors the active codec for the routing
     * decisions below (16-bit pixel, lossy quality, flavor strings). */
    int use_zlib = has_flag(argc, argv, "--zlib");
    if (use_zlib) {
        ctx.codec = QTSQ_CODEC_ZLIB;
        printf("Codec:  zlib (opt-out)\n");
    }
    int use_qtc = !use_zlib;

    /* Phase 6b: -q/--quality on compress sets the qtc lossy quality
     * (0 = lossless). Meaningful whenever the qtc codec is active. */
    const char *q_str = find_arg(argc, argv, "-q");
    if (!q_str) q_str = find_arg(argc, argv, "--quality");
    if (q_str) {
        ctx.quality = (float)atof(q_str);
        if (ctx.quality < 0.0f) ctx.quality = 0.0f;
        if (ctx.quality > 1.0f) ctx.quality = 1.0f;
        if (use_qtc) printf("Quality: %.2f (qtc lossy)\n", ctx.quality);
        else printf("Quality: %.2f (ignored: zlib codec is lossless-only)\n", ctx.quality);
    }

    int result;

    /* Route to best engine based on detected type + extension */

    /* Pre-compressed formats: MP3, MP4, ZIP, etc.
     * These are already compressed — Singularity (raw binary) is best.
     * Images (JPEG/PNG) are handled separately below for gravitational compression. */
    int is_precompressed = (
        strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "aac") == 0 ||
        strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "ogg") == 0 ||
        strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "gz") == 0 ||
        strcasecmp(ext, "zst") == 0 || strcasecmp(ext, "xz") == 0 ||
        strcasecmp(ext, "bz2") == 0 || strcasecmp(ext, "7z") == 0 ||
        strcasecmp(ext, "rar") == 0 || strcasecmp(ext, "tar") == 0 ||
        strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "docx") == 0 ||
        strcasecmp(ext, "xlsx") == 0 || strcasecmp(ext, "pptx") == 0 ||
        strcasecmp(ext, "exe") == 0 || strcasecmp(ext, "dll") == 0 ||
        strcasecmp(ext, "so") == 0 || strcasecmp(ext, "dylib") == 0
    );

    int is_image_ext = (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
                        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "bmp") == 0 ||
                        strcasecmp(ext, "webp") == 0);

    /* Forced Pixel Mode (-m 5): exact pixels via the pixel engine.
     * Default codec → qtc lossless; -q <0-1> → qtc lossy DCT; --zlib → zlib.
     * 16-bit PNG sources route to qtsq_compress_pixel16 (qtc codec only —
     * the only 16-bit image coder in tree). */
    if (forced_mode == 5 && is_image_ext) {
        int width, height, channels;
        if (use_qtc && strcasecmp(ext, "png") == 0 &&
            stbi_is_16_bit_from_memory(data, (int)file_size)) {
            uint16_t *img16 = stbi_load_16_from_memory(data, (int)file_size, &width, &height, &channels, 0);
            if (!img16) {
                printf("Error: Could not decode 16-bit PNG for pixel compression.\n");
                qtsq_free(&ctx);
                free(data);
                return 1;
            }
            printf("Engine: Pixel (16-bit camera raw, qtc) [%dx%d, %d ch]\n", width, height, channels);
            result = qtsq_compress_pixel16(&ctx, img16, (uint32_t)width, (uint32_t)height, (uint8_t)channels, QTSQ_PIXEL_COMPRESS_ZLIB);
            stbi_image_free(img16);
        } else {
            uint8_t *img_data = stbi_load_from_memory(data, (int)file_size, &width, &height, &channels, 0);
            if (!img_data) {
                printf("Error: Could not decode image data for pixel compression.\n");
                qtsq_free(&ctx);
                free(data);
                return 1;
            }
            const char *flavor = use_qtc ? (ctx.quality > 0.0f ? "qtc lossy DCT" : "qtc lossless") : "zlib";
            printf("Engine: Pixel (exact pixels, %s) [%dx%d, %d ch]\n", flavor, width, height, channels);
            result = qtsq_compress_pixel(&ctx, img_data, (uint32_t)width, (uint32_t)height, (uint8_t)channels, QTSQ_PIXEL_COMPRESS_ZLIB);
            stbi_image_free(img_data);
        }
    }
    /* Forced Gravitational Mode Override (-m 2) */
    else if (forced_mode == 2 && is_image_ext) {
        printf("Engine: Gravitational (Triangle RGB Mesh) [Forced]\n");
        int width, height, channels;
        uint8_t *img_data = stbi_load_from_memory(data, (int)file_size, &width, &height, &channels, 3);
        if (!img_data) {
            printf("Error: Could not decode image data for Gravitational compression.\n");
            qtsq_free(&ctx);
            free(data);
            return 1;
        }
        result = qtsq_compress_gravitational(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
        stbi_image_free(img_data);
    }
    /* Forced Gravitational HD Mode (-m 3 / --hd) */
    else if (forced_mode == 3 && is_image_ext) {
        int width, height, channels;
        uint8_t *img_data = stbi_load_from_memory(data, (int)file_size, &width, &height, &channels, 3);
        if (!img_data) {
            printf("Error: Could not decode image data for Gravitational HD compression.\n");
            qtsq_free(&ctx);
            free(data);
            return 1;
        }
        /* Size-based + format routing: small OR lossy → upscale, large lossless → HD */
        int is_lossy_ext = (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "webp") == 0);
        if (file_size < 500000 || is_lossy_ext) {
            printf("Engine: Gravitational Upscale (quality reconstruction) [HD + %s]\n", is_lossy_ext ? "lossy" : "small");
            result = qtsq_compress_gravitational_upscale(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
        } else {
            printf("Engine: Gravitational HD (Triangle RGB Mesh) [HD + lossless]\n");
            result = qtsq_compress_gravitational_hd(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
        }
        stbi_image_free(img_data);
    }
    /* Forced Gravitational Lossless Mode (-m 4 / --lossless) */
    else if (forced_mode == 4 && is_image_ext) {
        printf("Engine: Gravitational Lossless (6M vertices) [max fidelity]\n");
        int width, height, channels;
        uint8_t *img_data = stbi_load_from_memory(data, (int)file_size, &width, &height, &channels, 3);
        if (!img_data) {
            printf("Error: Could not decode image data for Lossless compression.\n");
            qtsq_free(&ctx);
            free(data);
            return 1;
        }
        result = qtsq_compress_gravitational_lossless(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
        stbi_image_free(img_data);
    }
    /* Auto-detect image files (no forced mode) — use gravitational HD by default */
    else if (forced_mode < 0 && is_image_ext) {
        int width, height, channels;
        uint8_t *img_data = stbi_load_from_memory(data, (int)file_size, &width, &height, &channels, 3);
        if (!img_data) {
            printf("Engine: Singularity (Delta-Seed) [image decode failed]\n");
            result = qtsq_compress_raw(&ctx, data, file_size, 0);
        } else {
            int is_lossy_auto = (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "webp") == 0);
            if (file_size < 500000 || is_lossy_auto) {
                printf("Engine: Gravitational Upscale (quality reconstruction) [auto + %s]\n", is_lossy_auto ? "lossy" : "small");
                result = qtsq_compress_gravitational_upscale(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
            } else {
                printf("Engine: Gravitational HD (Triangle RGB Mesh) [auto + lossless]\n");
                result = qtsq_compress_gravitational_hd(&ctx, img_data, (uint32_t)width, (uint32_t)height, 3, file_size);
            }
            stbi_image_free(img_data);
        }
    }
    else if (is_precompressed) {
        printf("Engine: Singularity (Delta-Seed) [pre-compressed format]\n");
        result = qtsq_compress_raw(&ctx, data, file_size, 0);
    } else if (strcasecmp(ext, "csv") == 0 || detected == QTSQ_TYPE_CSV) {
        printf("Engine: Horizon (Dictionary)\n");
        result = qtsq_compress_csv(&ctx, (const char *)data, file_size);
    } else if (strcasecmp(ext, "json") == 0 || detected == QTSQ_TYPE_JSON) {
        printf("Engine: Horizon (Dictionary)\n");
        result = qtsq_compress_text(&ctx, (const char *)data, file_size);
        if (result == QTSQ_OK) ctx.header.data_type = QTSQ_TYPE_JSON;
    } else if (qtsq_type_is_text(detected) ||
               strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "md") == 0 ||
               strcasecmp(ext, "html") == 0 || strcasecmp(ext, "xml") == 0 ||
               strcasecmp(ext, "js") == 0 || strcasecmp(ext, "py") == 0 ||
               strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0 ||
               strcasecmp(ext, "rs") == 0 || strcasecmp(ext, "go") == 0 ||
               strcasecmp(ext, "java") == 0 || strcasecmp(ext, "ts") == 0 ||
               strcasecmp(ext, "css") == 0 || strcasecmp(ext, "scss") == 0 ||
               strcasecmp(ext, "swift") == 0 || strcasecmp(ext, "kt") == 0) {
        printf("Engine: Horizon (Dictionary)\n");
        result = qtsq_compress_text(&ctx, (const char *)data, file_size);
    } else if (strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "pcm") == 0 ||
               detected == QTSQ_TYPE_AUDIO_MONO || detected == QTSQ_TYPE_AUDIO_STEREO) {
        /* Parse the RIFF/WAVE fmt + data chunks for the real channel
         * count and sample rate (previously hardcoded mono 44.1 kHz,
         * which corrupted every stereo or 48 kHz WAV). */
        uint32_t wav_rate = 44100, wav_ch = 1, wav_bits = 16;
        uint16_t wav_fmt_tag = 0;
        size_t pcm_off = 0, pcm_bytes = 0;
        int is_wav = (file_size > 12 &&
                      data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
                      data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E');
        if (is_wav) {
            size_t off = 12;
            while (off + 8 <= file_size) {
                uint32_t csz;
                memcpy(&csz, data + off + 4, 4);
                if (memcmp(data + off, "fmt ", 4) == 0 && off + 8 + 16 <= file_size) {
                    uint16_t fmt_tag = 0, nch = 0, bits = 0;
                    uint32_t rate = 0;
                    memcpy(&fmt_tag, data + off + 8, 2);
                    memcpy(&nch, data + off + 10, 2);
                    memcpy(&rate, data + off + 12, 4);
                    memcpy(&bits, data + off + 22, 2);
                    if (fmt_tag == 1 || fmt_tag == 3) { /* PCM / IEEE float */
                        wav_ch = nch;
                        wav_rate = rate;
                        wav_bits = bits;
                        wav_fmt_tag = fmt_tag;
                    }
                } else if (memcmp(data + off, "data", 4) == 0) {
                    pcm_off = off + 8;
                    pcm_bytes = csz;
                    if (pcm_off + pcm_bytes > file_size) pcm_bytes = file_size - pcm_off;
                    break;
                }
                off += 8 + (size_t)csz + (csz & 1); /* chunks are word-aligned */
            }
        }
        int wav_std_bits = (wav_bits == 8 || wav_bits == 16 ||
                            wav_bits == 24 || wav_bits == 32);
        int wav_std_fmt = (wav_fmt_tag == 1) ||
                          (wav_fmt_tag == 3 && wav_bits == 32);
        if (is_wav && pcm_bytes > 0 && wav_ch >= 1 && wav_ch <= 2 &&
            wav_std_bits && wav_std_fmt &&
            ctx.quality > 0.0f && use_qtc) {
            /* MDCT2 perceptual at the requested tier -- the same path
             * QTSQStudio captures with. The standard type is stereo, but a
             * genuinely mono source stays mono instead of being duplicated
             * into a fake stereo pair; the tier is a total bitrate, so the
             * file size is the same either way.
             *
             * 8/16/24-bit integer PCM and 32-bit int/float WAVs are all
             * converted to the float input the codec expects. 24-bit is
             * the common music-production case; rejecting it to the raw
             * fallback made the CLI's "standard" path much larger than
             * the source. */
            size_t bytes_per_sample = wav_bits / 8u;
            size_t frames = pcm_bytes / bytes_per_sample / wav_ch;
            size_t total = frames * (size_t)wav_ch;
            const uint8_t *src = data + pcm_off;
            float *pcmf = (float *)malloc(total * sizeof(float));
            if (!pcmf) {
                printf("Error: Cannot allocate PCM\n");
                free(data);
                return 1;
            }
            for (size_t i = 0; i < total; i++) {
                const uint8_t *p = src + i * bytes_per_sample;
                if (wav_bits == 8) {
                    pcmf[i] = ((int)p[0] - 128) / 128.0f;
                } else if (wav_bits == 16) {
                    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8);
                    if (v & 0x8000) v -= 0x10000;
                    pcmf[i] = v / 32768.0f;
                } else if (wav_bits == 24) {
                    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) |
                                ((int32_t)p[2] << 16);
                    if (v & 0x800000) v -= 0x1000000;
                    pcmf[i] = v / 8388608.0f;
                } else if (wav_fmt_tag == 3) {
                    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    float f;
                    memcpy(&f, &u, sizeof(f));
                    pcmf[i] = f;
                } else {
                    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    pcmf[i] = (int32_t)u / 2147483648.0f;
                }
            }
            printf("Engine: MDCT2 (Perceptual, lossy) [%u Hz, %s, %u-bit %s]\n",
                   wav_rate, wav_ch == 2 ? "stereo" : "mono", wav_bits,
                   wav_fmt_tag == 3 ? "float" : "PCM");
            result = qtsq_compress_audio_float(&ctx, pcmf, total, wav_rate,
                                               (uint8_t)wav_ch);
            free(pcmf);
        } else if (is_wav && pcm_bytes > 0 && wav_bits == 16 && wav_ch >= 1 && wav_ch <= 2) {
            printf("Engine: Lensing (Fourier) [%u Hz, %u ch PCM]\n", wav_rate, wav_ch);
            size_t num_samples = pcm_bytes / sizeof(int16_t); /* total interleaved samples */
            result = qtsq_compress_audio(&ctx, (const int16_t *)(data + pcm_off),
                                          num_samples, wav_rate, (uint8_t)wav_ch);
        } else if (is_wav) {
            printf("Engine: Singularity (Delta-Seed) [WAV: %u-bit/%u-ch unsupported by Lensing]\n", wav_bits, wav_ch);
            result = qtsq_compress_raw(&ctx, data, file_size, 0);
        } else {
            printf("Engine: Lensing (Fourier) [raw PCM: assuming mono 44.1 kHz]\n");
            size_t num_samples = file_size / sizeof(int16_t);
            result = qtsq_compress_audio(&ctx, (const int16_t *)data,
                                          num_samples, 44100, 1);
        }
    } else if ((strcasecmp(ext, "bmp") == 0 || strcasecmp(ext, "pgm") == 0 ||
                strcasecmp(ext, "ppm") == 0 || strcasecmp(ext, "raw") == 0 ||
                detected == QTSQ_TYPE_IMAGE_GRAY || detected == QTSQ_TYPE_IMAGE_RGB)) {
        /* Accretion (fractal) — adaptive block sizes handle any image size */
        printf("Engine: Accretion (Fractal)\n");
        uint32_t w, h;
        if (custom_w > 0 && custom_h > 0) {
            w = custom_w;
            h = custom_h;
        } else {
            uint32_t side = (uint32_t)sqrt((double)file_size);
            if (side == 0) side = 1;
            w = side;
            h = (uint32_t)(file_size / side);
            if (h == 0) h = 1;
        }
        result = qtsq_compress_image(&ctx, data, w, h, 1);
    } else {
        /* Default: raw binary (Singularity) — fast, lossless, works on anything */
        printf("Engine: Singularity (Delta-Seed)\n");
        result = qtsq_compress_raw(&ctx, data, file_size, 0);
    }

    free(data);

    if (result != QTSQ_OK) {
        printf("Error:  %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    /* Encrypt before write (Phase 1 fail-closed) */
    result = cli_encrypt_for_write(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: encryption required before write: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    /* Write output */
    result = qtsq_write(&ctx, output);
    if (result != QTSQ_OK) {
        printf("Error:  %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    printf("Output: %s (%llu bytes)\n", output, (unsigned long long)ctx.singularity_size);
    printf("Ratio:  %.1f:1\n", qtsq_compression_ratio(&ctx));
    printf("✅ Compressed successfully.\n");

    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  DECOMPRESS — decompress .qtsq → original
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_decompress(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq decompress <file.qtsq> [-o output] [-q quality]\n");
        return 1;
    }

    const char *input = argv[2];
    const char *output = find_arg(argc, argv, "-o");
    const char *quality_str = find_arg(argc, argv, "-q");
    double quality = 1.0;
    if (quality_str) quality = atof(quality_str);

    char out_path[512];
    if (!output) {
        strip_qtsq_extension(input, out_path, sizeof(out_path));
        output = out_path;
    }

    /* Read .qtsq file */
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int result = qtsq_read(&ctx, input);
    if (result != QTSQ_OK) {
        printf("Error: Cannot read '%s': %s\n", input, qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    result = cli_decrypt_after_read(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    printf("Input:    %s\n", input);
    printf("Type:     %s\n", qtsq_type_name(ctx.header.data_type));
    printf("Strategy: %s\n", qtsq_strategy_name(ctx.header.strategy));
    printf("Original: %llu bytes\n", (unsigned long long)ctx.header.original_size);

    /* Check if encrypted */
    if (qtsq_is_encrypted(&ctx)) {
        printf("Error: File is encrypted. Use 'qtsq decrypt' first.\n");
        qtsq_free(&ctx);
        return 1;
    }

    /* Decompress */
    uint8_t *out_data = NULL;
    size_t out_size = 0;

    if (quality < 1.0) {
        printf("Quality:  %.0f%%\n", quality * 100);
        result = qtsq_decompress_redshift(&ctx, quality, &out_data, &out_size);
    } else {
        result = qtsq_decompress(&ctx, &out_data, &out_size);
    }

    if (result != QTSQ_OK) {
        printf("Error: Decompression failed: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    /* Write output */
    if (ctx.header.strategy == QTSQ_STRATEGY_TRIANGLE) {
        /* Re-encode raw RGB to PNG natively so the user gets an actual image back! */
        char out_png[512];
        snprintf(out_png, sizeof(out_png), "%s.png", output);
        int written = stbi_write_png(out_png, ctx.schema.dimensions[0], ctx.schema.dimensions[1], 3, out_data, ctx.schema.dimensions[0] * 3);
        if (!written) {
            printf("Error: Cannot write PNG output '%s'\n", out_png);
            free(out_data);
            qtsq_free(&ctx);
            return 1;
        }
        printf("Output:   %s (%zu raw bytes decompressed to PNG)\n", out_png, out_size);
    } else if (ctx.header.strategy == QTSQ_STRATEGY_PIXEL &&
               ctx.schema.dimensions[0] > 0 && ctx.schema.dimensions[1] > 0 &&
               ctx.header.sample_depth != 16) {
        /* Pixel-engine output is raw 8-bit pixels — re-encode to PNG.
         * Passthrough payloads (JPEG/PNG bytes) have zero dimensions and
         * 16-bit payloads keep their raw sample bytes below. */
        char out_png[512];
        snprintf(out_png, sizeof(out_png), "%s.png", output);
        int ch = ctx.header.num_channels ? ctx.header.num_channels : 3;
        int written = stbi_write_png(out_png, ctx.schema.dimensions[0], ctx.schema.dimensions[1], ch, out_data, ctx.schema.dimensions[0] * ch);
        if (!written) {
            printf("Error: Cannot write PNG output '%s'\n", out_png);
            free(out_data);
            qtsq_free(&ctx);
            return 1;
        }
        printf("Output:   %s (%zu raw bytes decompressed to PNG)\n", out_png, out_size);
    } else if (is_float32_audio(&ctx)) {
        /* Interleaved float32 -> 16-bit PCM WAV so the user gets a file
         * they can actually play. qtsq_compress_audio_float stores floats;
         * nothing else in the toolchain consumes raw float bytes. */
        size_t nsamples = out_size / sizeof(float);
        uint16_t ch = ctx.header.num_channels ? (uint16_t)ctx.header.num_channels : 2;
        uint32_t rate = ctx.schema.sample_rate;
        int16_t *pcm16 = (int16_t *)malloc(nsamples * sizeof(int16_t));
        if (!pcm16) {
            printf("Error: Cannot allocate PCM\n");
            free(out_data);
            qtsq_free(&ctx);
            return 1;
        }
        const float *f32 = (const float *)out_data;
        for (size_t i = 0; i < nsamples; i++) {
            float v = f32[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            pcm16[i] = (int16_t)(v * 32767.0f);
        }
        char out_wav[512];
        snprintf(out_wav, sizeof(out_wav), "%s.wav", output);
        if (write_wav16(out_wav, pcm16, nsamples, rate ? rate : 44100, ch) != 0) {
            printf("Error: Cannot write '%s'\n", out_wav);
            free(pcm16);
            free(out_data);
            qtsq_free(&ctx);
            return 1;
        }
        free(pcm16);
        printf("Output:   %s (%zu samples, %u Hz, %u ch, 16-bit)\n",
               out_wav, nsamples, rate ? rate : 44100, ch);
    } else if (write_file(output, out_data, out_size) != 0) {
        printf("Error: Cannot write '%s'\n", output);
        free(out_data);
        qtsq_free(&ctx);
        return 1;
    } else {
        printf("Output:   %s (%zu bytes)\n", output, out_size);
    }
    printf("✅ Decompressed successfully.\n");

    free(out_data);
    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  INFO — inspect .qtsq file
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_info(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq info <file.qtsq>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int result = qtsq_read(&ctx, argv[2]);
    if (result != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(result));
        return 1;
    }

    result = cli_decrypt_after_read(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    printf("File: %s\n\n", argv[2]);
    qtsq_print_info(&ctx);

    /* Additional details */
    printf("\nChecksum (SHA-256): ");
    for (int i = 0; i < 32; i++) printf("%02x", ctx.header.checksum[i]);
    printf("\n");

    if (qtsq_is_encrypted(&ctx))
        printf("Status: 🔒 ENCRYPTED\n");
    else
        printf("Status: 🔓 Not encrypted\n");

    if (ctx.has_recovery)
        printf("ECC: ✅ Protected (%u chunks)\n", ctx.recovery.num_chunks);
    else
        printf("ECC: ⚠️ No protection\n");

    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  ENCRYPT / DECRYPT
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_encrypt(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq encrypt <file.qtsq> -p <password>\n");
        return 1;
    }

    const char *password = find_arg(argc, argv, "-p");
    if (!password) {
        printf("Error: Password required. Use -p <password>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, argv[2]);
    if (r != QTSQ_OK) { printf("Error: %s\n", qtsq_error_string(r)); return 1; }

    if (qtsq_is_encrypted(&ctx)) {
        printf("File is already encrypted.\n");
        qtsq_free(&ctx);
        return 0;
    }

    r = qtsq_encrypt(&ctx, password);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_write(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    printf("✅ Encrypted: %s\n", argv[2]);
    qtsq_free(&ctx);
    return 0;
}

static int cmd_decrypt(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq decrypt <file.qtsq> -p <password>\n");
        return 1;
    }

    const char *password = find_arg(argc, argv, "-p");
    if (!password) {
        printf("Error: Password required. Use -p <password>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, argv[2]);
    if (r != QTSQ_OK) { printf("Error: %s\n", qtsq_error_string(r)); return 1; }

    if (!qtsq_is_encrypted(&ctx)) {
        printf("File is not encrypted.\n");
        qtsq_free(&ctx);
        return 0;
    }

    r = qtsq_decrypt(&ctx, password);
    if (r == QTSQ_ERR_DECRYPT) {
        printf("❌ Wrong password.\n");
        qtsq_free(&ctx);
        return 1;
    }
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_write(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    printf("✅ Decrypted: %s\n", argv[2]);
    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  PROTECT / REPAIR — ECC
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_protect(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq protect <file.qtsq>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, argv[2]);
    if (r != QTSQ_OK) { printf("Error: %s\n", qtsq_error_string(r)); return 1; }

    r = cli_decrypt_after_read(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_add_recovery(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = cli_encrypt_for_write(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: encryption required before write: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_write(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    printf("✅ Protected: %s (%u chunks, %u bytes parity)\n",
           argv[2], ctx.recovery.num_chunks, ctx.recovery.parity_size);
    qtsq_free(&ctx);
    return 0;
}

static int cmd_repair(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq repair <file.qtsq>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, argv[2]);
    if (r != QTSQ_OK) { printf("Error: %s\n", qtsq_error_string(r)); return 1; }

    r = cli_decrypt_after_read(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    if (!ctx.has_recovery) {
        printf("⚠️ No ECC data. Use 'qtsq protect' first.\n");
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_health_check(&ctx);
    if (r == QTSQ_OK) {
        printf("✅ File is healthy. No repair needed.\n");
        qtsq_free(&ctx);
        return 0;
    }

    printf("⚠️ Corruption detected. Attempting recovery...\n");

    r = qtsq_recover(&ctx);
    if (r != QTSQ_OK) {
        printf("❌ Recovery failed: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = cli_encrypt_for_write(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: encryption required before write: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_write(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    printf("✅ Repaired: %s\n", argv[2]);
    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  DIFF / PATCH — Gravitational waves
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_diff(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: qtsq diff <old.qtsq> <new.qtsq> [-o patch.qtsw]\n");
        return 1;
    }

    const char *output = find_arg(argc, argv, "-o");
    char out_path[512];
    if (!output) {
        snprintf(out_path, sizeof(out_path), "patch.qtsw");
        output = out_path;
    }

    qtsq_context_t old_ctx, new_ctx;
    qtsq_init(&old_ctx);
    qtsq_init(&new_ctx);

    int r = qtsq_read(&old_ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error reading old: %s\n", qtsq_error_string(r));
        return 1;
    }

    r = cli_decrypt_after_read(&old_ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed (old): %s\n", qtsq_error_string(r));
        qtsq_free(&old_ctx);
        return 1;
    }

    r = qtsq_read(&new_ctx, argv[3]);
    if (r != QTSQ_OK) {
        printf("Error reading new: %s\n", qtsq_error_string(r));
        qtsq_free(&old_ctx);
        return 1;
    }

    r = cli_decrypt_after_read(&new_ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed (new): %s\n", qtsq_error_string(r));
        qtsq_free(&old_ctx);
        qtsq_free(&new_ctx);
        return 1;
    }

    r = qtsq_emit_wave(&old_ctx, &new_ctx, output);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&old_ctx);
        qtsq_free(&new_ctx);
        return 1;
    }

    printf("✅ Diff: %s → %s → %s\n", argv[2], argv[3], output);

    qtsq_free(&old_ctx);
    qtsq_free(&new_ctx);
    return 0;
}

static int cmd_patch(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: qtsq patch <file.qtsq> <patch.qtsw>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        return 1;
    }

    r = cli_decrypt_after_read(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_apply_wave(&ctx, argv[3]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = cli_encrypt_for_write(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: encryption required before write: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = qtsq_write(&ctx, argv[2]);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    printf("✅ Patched: %s\n", argv[2]);
    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  VIEW — decompress and open with system viewer
 * ═══════════════════════════════════════════════════════════════════ */





static int cmd_view(int argc, char **argv) {
    const char *input = NULL;

    if (argc >= 3) {
        input = argv[2];
    } else if (argc == 2) {
        input = argv[1];
    } else {
        printf("Usage: qtsq view <file.qtsq>\n");
        return 1;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int r = qtsq_read(&ctx, input);
    if (r != QTSQ_OK) {
        printf("Error: Cannot read '%s': %s\n", input, qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    r = cli_decrypt_after_read(&ctx);
    if (r != QTSQ_OK) {
        printf("Error: decryption failed: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }

    if (qtsq_is_encrypted(&ctx)) {
        printf("🔒 File is encrypted.\n");
        printf("Decrypt first: qtsq decrypt '%s' -p <password>\n", input);
        qtsq_free(&ctx);
        return 1;
    }

    uint8_t *data = NULL;
    size_t data_size = 0;
    r = qtsq_decompress(&ctx, &data, &data_size);
    if (r != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(r));
        qtsq_free(&ctx);
        return 1;
    }


    /* Get basename for display */
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;



    /* Launch native viewer (H14 fix: use posix_spawn instead of system() to
     * prevent shell injection from filenames containing quotes/backticks). */
    printf("Opening Native QTSQ Viewer...\n");

    pid_t pid;
    char *spawn_argv[] = {"./bin/QTSQViewer", (char *)input, NULL};
    extern char **environ;
    int ret = posix_spawn(&pid, "./bin/QTSQViewer", NULL, NULL, spawn_argv, environ);
    if (ret == 0) {
        int status;
        waitpid(pid, &status, 0);
        ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (ret != 0) {
        printf("Error: Could not launch native viewer. Make sure to run 'make cli' first.\n");
    }

    free(data);
    qtsq_free(&ctx);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 *  PACK — create multi-data container from files/folders
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_pack(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq pack <file_or_folder> [file2...] [-o output.qtsq]\n");
        return 1;
    }

    const char *output = find_arg(argc, argv, "-o");
    char out_path[512];
    if (!output) {
        snprintf(out_path, sizeof(out_path), "bundle.qtsq");
        output = out_path;
    }

    qtsq_context_t ctx;
    int result = qtsq_container_create(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: Cannot create container: %s\n", qtsq_error_string(result));
        return 1;
    }

    int files_added = 0;

    for (int i = 2; i < argc; i++) {
        /* Skip -o and its argument, plus global encryption flags */
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--password") == 0) { i++; continue; }
        if (strcmp(argv[i], "--keystore") == 0) continue;

        struct stat st;
        if (stat(argv[i], &st) != 0) {
            printf("Warning: Cannot access '%s', skipping.\n", argv[i]);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            printf("📁 Adding folder: %s\n", argv[i]);
            result = qtsq_container_add_folder(&ctx, argv[i]);
        } else {
            printf("📄 Adding file:   %s\n", argv[i]);
            result = qtsq_container_add_file(&ctx, argv[i], NULL);
        }

        if (result != QTSQ_OK) {
            printf("Error adding '%s': %s\n", argv[i], qtsq_error_string(result));
            qtsq_free(&ctx);
            return 1;
        }
        files_added++;
    }

    if (files_added == 0) {
        printf("Error: No files to pack.\n");
        qtsq_free(&ctx);
        return 1;
    }

    /* Pack all streams into singularity */
    result = qtsq_container_pack(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: Pack failed: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    /* Get count for display */
    uint32_t count = 0;
    qtsq_container_get_count(&ctx, &count);

    /* Encrypt before write (Phase 1 fail-closed) */
    result = cli_encrypt_for_write(&ctx);
    if (result != QTSQ_OK) {
        printf("Error: encryption required before write: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    /* Write */
    result = qtsq_write(&ctx, output);
    if (result != QTSQ_OK) {
        printf("Error: Write failed: %s\n", qtsq_error_string(result));
        qtsq_free(&ctx);
        return 1;
    }

    printf("\n✅ Packed %u streams → %s (%llu bytes)\n",
           count, output, (unsigned long long)ctx.singularity_size);

    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  UNPACK — extract streams from container
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_unpack(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: qtsq unpack <bundle.qtsq> [-o output_dir]\n");
        return 1;
    }

    const char *input = argv[2];
    const char *outdir = find_arg(argc, argv, "-o");
    if (!outdir) outdir = ".";

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int result = qtsq_read(&ctx, input);
    if (result != QTSQ_OK) {
        printf("Error: %s\n", qtsq_error_string(result));
        return 1;
    }

    if (ctx.header.data_type != QTSQ_TYPE_CONTAINER) {
        printf("Error: '%s' is not a container file.\n", input);
        qtsq_free(&ctx);
        return 1;
    }

    /* Check if this is a Tsubu engine asset */
    int is_tsubu = (ctx.header.flags & QTSQ_FLAG_TSUBU_ATOMS) != 0;
    if (is_tsubu) {
        printf("Engine: Tsubu (4D World-Space Atoms)\n");
    }

    uint32_t count = 0;
    qtsq_container_get_count(&ctx, &count);
    printf("Container: %s (%u streams)\n\n", input, count);

    for (uint32_t i = 0; i < count; i++) {
        qtsq_context_t stream;
        char name[256];
        result = qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name));
        if (result != QTSQ_OK) {
            printf("  ⚠️  Stream %u: extraction failed (%s)\n", i, qtsq_error_string(result));
            continue;
        }

        /* Decompress the stream */
        uint8_t *out_data = NULL;
        size_t out_size = 0;
        result = qtsq_decompress(&stream, &out_data, &out_size);

        if (result != QTSQ_OK) {
            printf("  ⚠️  Stream %u (%s): decompress failed (%s)\n", i, name, qtsq_error_string(result));
            qtsq_free(&stream);
            continue;
        }

        /* Create subdirectories if name contains path separators */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", outdir, name);

        /* Ensure parent directory exists (H14 fix: use C mkdir instead of
         * system("mkdir -p '...") to prevent shell injection from stream
         * names containing single quotes). */
        char dir_buf[1024];
        strncpy(dir_buf, full_path, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            /* Recursive mkdir: walk the path creating each component. */
            char *p = dir_buf;
            while (*p == '/') p++; /* skip leading slashes */
            while ((p = strchr(p, '/')) != NULL) {
                *p = '\0';
                mkdir(dir_buf, 0755);
                *p = '/';
                p++;
            }
            mkdir(dir_buf, 0755);
        }

        /* Write file */
        if (write_file(full_path, out_data, out_size) != 0) {
            printf("  ⚠️  Stream %u (%s): write failed\n", i, name);
        } else {
            printf("  📄 %s (%zu bytes)\n", name, out_size);
        }

        free(out_data);
        qtsq_free(&stream);
    }

    printf("\n✅ Unpacked %u streams → %s/\n", count, outdir);

    qtsq_free(&ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN — command router
 * ═══════════════════════════════════════════════════════════════════ */

/* Recognised subcommand names / aliases */
static int is_subcommand(const char *s) {
    if (!s) return 0;
    static const char *cmds[] = {
        "compress", "c",
        "decompress", "d", "x",
        "info", "i",
        "view", "v", "open",
        "encrypt", "e",
        "decrypt",
        "protect",
        "repair",
        "diff",
        "patch",
        "pack",
        "unpack",
        NULL
    };
    for (const char **c = cmds; *c; c++) {
        if (strcmp(s, *c) == 0) return 1;
    }
    size_t len = strlen(s);
    if (len > 5 && strcmp(s + len - 5, ".qtsq") == 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_usage();
        return 0;
    }

    /* Locate the subcommand; global flags may appear before it. */
    int subcommand_idx = 1;
    while (subcommand_idx < argc && !is_subcommand(argv[subcommand_idx])) {
        subcommand_idx++;
    }

    if (subcommand_idx >= argc) {
        printf("Unknown command: '%s'. Run 'qtsq --help' for usage.\n", argv[1]);
        return 1;
    }

    /* Parse global encryption flags.  --password is always global.  -p is
     * accepted anywhere too: compress/decompress have no -p of their own, so
     * restricting it to before the subcommand meant `qtsq compress f -p pw`
     * silently ignored the password and failed with "context not encrypted".
     * encrypt/decrypt parse -p themselves and are unaffected. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            g_cli_password = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_cli_password = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--keystore") == 0) {
            g_cli_use_keystore = 1;
        }
    }

    /* Build a filtered argv whose first element is the program name and
     * whose second element is the subcommand.  Consume global flags that
     * appear before the subcommand, but leave -p after the subcommand for
     * the individual commands (encrypt/decrypt). */
    char *filtered_argv[128];
    int filtered_argc = 0;
    filtered_argv[filtered_argc++] = argv[0];

    for (int i = subcommand_idx; i < argc; i++) {
        if (filtered_argc < (int)(sizeof(filtered_argv) / sizeof(filtered_argv[0]))) {
            filtered_argv[filtered_argc++] = argv[i];
        }
    }
    /* Also copy global --password/--keystore so subcommands that walk argv
     * (compress, pack) can skip them if they appear after the subcommand. */
    for (int i = 1; i < subcommand_idx; i++) {
        if (strcmp(argv[i], "--password") == 0 || strcmp(argv[i], "--keystore") == 0) {
            if (filtered_argc < (int)(sizeof(filtered_argv) / sizeof(filtered_argv[0]))) {
                filtered_argv[filtered_argc++] = argv[i];
                if (strcmp(argv[i], "--password") == 0 && i + 1 < subcommand_idx &&
                    filtered_argc < (int)(sizeof(filtered_argv) / sizeof(filtered_argv[0]))) {
                    filtered_argv[filtered_argc++] = argv[i + 1];
                }
            }
        }
    }

    argc = filtered_argc;
    argv = filtered_argv;

    const char *cmd = argv[1];

    /* If the first arg is a .qtsq file (not a command), auto-view it.
     * This enables macOS app association: double-click → qtsq view file */
    size_t cmd_len = strlen(cmd);
    if (cmd_len > 5 && strcmp(cmd + cmd_len - 5, ".qtsq") == 0) {
        return cmd_view(argc, argv);
    }

    if (strcmp(cmd, "compress") == 0 || strcmp(cmd, "c") == 0)
        return cmd_compress(argc, argv);
    if (strcmp(cmd, "decompress") == 0 || strcmp(cmd, "d") == 0 || strcmp(cmd, "x") == 0)
        return cmd_decompress(argc, argv);
    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "i") == 0)
        return cmd_info(argc, argv);
    if (strcmp(cmd, "view") == 0 || strcmp(cmd, "v") == 0 || strcmp(cmd, "open") == 0)
        return cmd_view(argc, argv);
    if (strcmp(cmd, "encrypt") == 0 || strcmp(cmd, "e") == 0)
        return cmd_encrypt(argc, argv);
    if (strcmp(cmd, "decrypt") == 0)
        return cmd_decrypt(argc, argv);
    if (strcmp(cmd, "protect") == 0)
        return cmd_protect(argc, argv);
    if (strcmp(cmd, "repair") == 0)
        return cmd_repair(argc, argv);
    if (strcmp(cmd, "diff") == 0)
        return cmd_diff(argc, argv);
    if (strcmp(cmd, "patch") == 0)
        return cmd_patch(argc, argv);
    if (strcmp(cmd, "pack") == 0)
        return cmd_pack(argc, argv);
    if (strcmp(cmd, "unpack") == 0)
        return cmd_unpack(argc, argv);

    printf("Unknown command: '%s'. Run 'qtsq --help' for usage.\n", cmd);
    return 1;
}

