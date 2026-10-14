/*
 * qtc.c — manual-testing CLI for the qtc compression library (v0.12.0).
 *
 * Subcommands:
 *   qtc c   IN OUT [-q Q] [-l LENS]   compress a file (generic byte path)
 *   qtc d   IN OUT                    decompress a qtc stream
 *   qtc ci  IN.png|jpg OUT.qtc [-q Q] lossy-compress a photo (DCT lens)
 *   qtc di  IN.qtc OUT.png            decompress a photo stream to PNG
 *   qtc ca  IN.audio OUT.qtc [-q Q]   compress audio (WAV native; MP3/M4A/
 *                                     AIFF/FLAC/... decoded via CoreAudio;
 *                                     1.0=lossless)
 *   qtc play IN.qtc [-r HZ] [-v G]    decode + PLAY audio (CoreAudio) with a
 *                                     live ASCII level meter in terminal
 *   qtc view IN.qtc                   decode an image stream and show it in
 *                                     a NATIVE window (Cocoa custom renderer,
 *                                     software-drawn -- no HTML/webview)
 *   qtc info FILE                     show container header info
 *   qtc version
 *
 * Byte-exact round-trip checks: use the system `cmp` tool.
 *
 * Safety: input files capped at 1 GiB, generic decode output capped at 1 GiB,
 * all allocations overflow-checked, every parser bounds-checked.
 * NOTE: every printf/fprintf format string below is kept on ONE line on
 * purpose (no adjacent-literal concatenation).
 */
#include "qtc.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../include/stb_image_write.h"

#define QTC_CLI_MAX_INPUT (1u << 30)   /* 1 GiB input file cap  */
#define QTC_CLI_MAX_OUT   (1u << 30)   /* 1 GiB decode out cap  */

/* ── file helpers ─────────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "qtc: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || (unsigned long)n > QTC_CLI_MAX_INPUT) {
        fprintf(stderr, "qtc: %s is unreadable or larger than 1 GiB\n", path);
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    /* n <= 1 GiB, so (size_t)n + 1 cannot overflow size_t. */
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "qtc: short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "qtc: cannot write %s\n", path);
        return -1;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "qtc: short write on %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static const char *lens_name(int id)
{
    switch (id) {
    case 0:  return "AUTO";
    case 1:  return "RAW (order-0 rANS)";
    case 2:  return "STORED";
    case 3:  return "RAW1 (order-1 rANS)";
    case 4:  return "LZ (LZ77 + rANS)";
    case 5:  return "IMAGE (2D predictor + LZ)";
    case 6:  return "AUDIO (mid/side + LPC)";
    case 7:  return "AUDIO_MDCT (lossy)";
    case 8:  return "IMAGE_MESH (lossy)";
    case 9:  return "IMAGE_ACCRETION (lossy)";
    case 10: return "TENSOR (float32 lossless)";
    case 11: return "VIDEO (temporal lossless)";
    case 12: return "IMAGE_DCT (lossy photo)";
    case 13: return "AUDIO_MDCT2 (perceptual lossy)";
    case 14: return "AUDIO_FLOAT (float32 audio)";
    case 15: return "LZ2 (LZ77 v2: rep offsets + 8-bit freqs)";
    default: return "UNKNOWN";
    }
}

static float parse_quality(const char *s)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) v = 0.85;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return (float)v;
}

static int parse_lens(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v < 0 || v > 255) v = 0;
    return (int)v;
}

static double parse_double(const char *s, double dflt)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) v = dflt;
    return v;
}

/* ── generic compress/decompress ─────────────────────────────────────── */

static int cmd_compress(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qtc c IN OUT [-q Q] [-l LENS]\n");
        return 2;
    }
    float quality = 0.85f;
    int lens = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-q") && i + 1 < argc)
            quality = parse_quality(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            lens = parse_lens(argv[++i]);
    }
    size_t in_len = 0;
    uint8_t *in = read_file(argv[0], &in_len);
    if (!in) return 1;

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.quality = quality;
    opts.force_lens = (qtc_lens_t)lens;
    if (quality < 1.0f)
        opts.mode = QTC_MODE_LOSSY;

    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = qtc_compress(in, in_len, &out, &out_len, &opts);
    free(in);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: compress failed: %s\n", qtc_error_string(rc));
        return 1;
    }
    if (write_file(argv[1], out, out_len) != 0) {
        free(out);
        return 1;
    }
    int lens_id = (out_len >= 5) ? out[4] : -1;
    double ratio = (out_len > 0) ? (double)in_len / (double)out_len : 0.0;
    printf("%zu -> %zu bytes (%.2fx), lens=%s\n", in_len, out_len, ratio, lens_name(lens_id));
    free(out);
    return 0;
}

static int cmd_decompress(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qtc d IN OUT\n");
        return 2;
    }
    size_t in_len = 0;
    uint8_t *in = read_file(argv[0], &in_len);
    if (!in) return 1;
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = qtc_decompress_bounded(in, in_len, &out, &out_len, QTC_CLI_MAX_OUT);
    free(in);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: decompress failed: %s\n", qtc_error_string(rc));
        return 1;
    }
    if (write_file(argv[1], out, out_len) != 0) {
        free(out);
        return 1;
    }
    printf("%zu -> %zu bytes\n", in_len, out_len);
    free(out);
    return 0;
}

/* ── photos ───────────────────────────────────────────────────────────── */

/* Minimal 16-bit PNG writer (stb_image_write only does 8-bit). Filter-0
 * scanlines wrapped in stored (uncompressed) deflate blocks inside a zlib
 * stream — valid PNG, decodable by any viewer, no dependencies. Samples are
 * big-endian per the PNG spec. */
static uint32_t png_crc_table[256];
static int png_crc_ready = 0;

static uint32_t png_crc(const uint8_t *data, size_t len)
{
    if (!png_crc_ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            png_crc_table[n] = c;
        }
        png_crc_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = png_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t png_adler(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void png_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int png16_write_chunk(FILE *f, const char type[4],
                             const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    png_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len)
        return -1;
    /* CRC covers type + data; type is 4 bytes, data up to ~2 GiB here. */
    uint8_t *cat = (uint8_t *)malloc(4 + len);
    if (!cat)
        return -1;
    memcpy(cat, type, 4);
    if (len > 0)
        memcpy(cat + 4, data, len);
    uint32_t crc = png_crc(cat, 4 + len);
    free(cat);
    uint8_t crcb[4];
    png_be32(crcb, crc);
    return (fwrite(crcb, 1, 4, f) == 4) ? 0 : -1;
}

static int png16_write(const char *path, const uint16_t *px,
                       uint32_t w, uint32_t h, uint8_t ch)
{
    if (ch == 0 || ch > 4)
        return -1;
    size_t rowbytes = (size_t)w * ch * 2;
    size_t rawlen = (size_t)h * (rowbytes + 1); /* filter byte per row */
    if (rawlen > ((size_t)1 << 30))
        return -1; /* keep the CLI's 1 GiB working-set discipline */

    uint8_t *raw = (uint8_t *)malloc(rawlen ? rawlen : 1);
    if (!raw)
        return -1;
    size_t o = 0;
    for (uint32_t y = 0; y < h; y++) {
        raw[o++] = 0; /* filter: none */
        const uint16_t *row = px + (size_t)y * w * ch;
        for (size_t i = 0; i < (size_t)w * ch; i++) {
            raw[o++] = (uint8_t)(row[i] >> 8);   /* PNG is big-endian */
            raw[o++] = (uint8_t)row[i];
        }
    }

    /* zlib wrapper + stored deflate blocks over raw[]. */
    size_t nblocks = (rawlen + 65534) / 65535;
    if (nblocks == 0)
        nblocks = 1;
    size_t idat_len = 2 + rawlen + nblocks * 5 + 4;
    uint8_t *idat = (uint8_t *)malloc(idat_len);
    if (!idat) {
        free(raw);
        return -1;
    }
    size_t p = 0;
    idat[p++] = 0x78; idat[p++] = 0x01; /* zlib: deflate, no dict */
    size_t off = 0;
    while (off < rawlen || (off == 0 && rawlen == 0)) {
        size_t take = rawlen - off;
        if (take > 65535)
            take = 65535;
        int final = (off + take >= rawlen);
        idat[p++] = (uint8_t)(final ? 1 : 0); /* BFINAL, BTYPE=00 (stored) */
        idat[p++] = (uint8_t)take;
        idat[p++] = (uint8_t)(take >> 8);
        idat[p++] = (uint8_t)(~take & 0xFF);
        idat[p++] = (uint8_t)((~take >> 8) & 0xFF);
        if (take > 0)
            memcpy(idat + p, raw + off, take);
        p += take;
        off += take;
        if (rawlen == 0)
            break;
    }
    uint32_t adler = png_adler(raw, rawlen);
    png_be32(idat + p, adler);
    p += 4;
    free(raw);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t colortype = (ch == 1) ? 0 : (ch == 2) ? 4 : (ch == 3) ? 2 : 6;
    uint8_t ihdr[13];
    png_be32(ihdr, w);
    png_be32(ihdr + 4, h);
    ihdr[8] = 16;          /* bit depth */
    ihdr[9] = colortype;
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(idat);
        return -1;
    }
    int ok = (fwrite(sig, 1, 8, f) == 8) &&
             png16_write_chunk(f, "IHDR", ihdr, 13) == 0 &&
             png16_write_chunk(f, "IDAT", idat, p) == 0 &&
             png16_write_chunk(f, "IEND", NULL, 0) == 0;
    free(idat);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

static int cmd_compress_image(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qtc ci IN.(png|jpg) OUT.qtc [-q Q]\n");
        return 2;
    }
    float quality = 0.85f;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-q") && i + 1 < argc)
            quality = parse_quality(argv[++i]);

    int w = 0, h = 0, ch = 0;

    /* 16-bit source (camera-raw PNG): lossless/near-lossless IMAGE16 path.
     * -q 1.0 is bit-exact; lower q allows up to 8 LSBs of error. */
    if (stbi_is_16_bit(argv[0])) {
        stbi_us *px16 = stbi_load_16(argv[0], &w, &h, &ch, 0);
        if (!px16) {
            fprintf(stderr, "qtc: cannot load 16-bit image %s\n", argv[0]);
            return 1;
        }
        qtc_options_t opts;
        qtc_options_default(&opts);
        opts.quality = quality;
        opts.mode = (quality >= 1.0f) ? QTC_MODE_LOSSLESS : QTC_MODE_LOSSY;
        fprintf(stderr, "qtc: encoding 16-bit %dx%d ch=%d q=%.2f...\n", w, h, ch, (double)quality);
        uint8_t *out = NULL;
        size_t out_len = 0;
        int rc = qtc_compress_image16(px16, (uint32_t)w, (uint32_t)h, (uint8_t)ch, &out, &out_len, &opts);
        stbi_image_free(px16);
        if (rc != QTC_OK) {
            fprintf(stderr, "qtc: image16 compress failed: %s\n", qtc_error_string(rc));
            return 1;
        }
        if (write_file(argv[1], out, out_len) != 0) {
            free(out);
            return 1;
        }
        size_t raw = (size_t)w * (size_t)h * (size_t)ch * 2;
        int lens_id = (out_len >= 5) ? out[4] : -1;
        printf("%dx%d ch=%d 16-bit q=%.2f: %zu raw -> %zu bytes (%.2fx), lens=%s\n", w, h, ch, (double)quality, raw, out_len, (double)raw / (double)(out_len ? out_len : 1), lens_name(lens_id));
        free(out);
        return 0;
    }

    uint8_t *px = stbi_load(argv[0], &w, &h, &ch, 0);
    if (!px) {
        fprintf(stderr, "qtc: cannot load image %s (PNG/JPG supported)\n", argv[0]);
        return 1;
    }
    if (ch == 2 || ch == 4) {           /* DCT lens takes 1 or 3 channels */
        stbi_image_free(px);
        px = stbi_load(argv[0], &w, &h, &ch, 3);
        if (!px) {
            fprintf(stderr, "qtc: cannot reload %s as RGB\n", argv[0]);
            return 1;
        }
        ch = 3; /* stbi reports the file's channel count, not the requested one */
    }

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.quality = quality;
    opts.mode = QTC_MODE_LOSSY;

    fprintf(stderr, "qtc: encoding %dx%d q=%.2f...\n", w, h, (double)quality);

    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = qtc_compress_image(px, (uint32_t)w, (uint32_t)h, (uint8_t)ch, &out, &out_len, &opts);
    stbi_image_free(px);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: image compress failed: %s\n", qtc_error_string(rc));
        return 1;
    }
    if (write_file(argv[1], out, out_len) != 0) {
        free(out);
        return 1;
    }
    size_t raw = (size_t)w * (size_t)h * (size_t)ch;
    int lens_id = (out_len >= 5) ? out[4] : -1;
    double bpp = ((size_t)w * (size_t)h > 0) ? (double)out_len * 8.0 / ((double)w * (double)h) : 0.0;
    printf("%dx%d ch=%d q=%.2f: %zu raw -> %zu bytes (%.3f bpp), lens=%s\n", w, h, ch, (double)quality, raw, out_len, bpp, lens_name(lens_id));
    free(out);
    return 0;
}

/* True for a qtc stream whose lens is not one of the image lenses: give a
 * clearer error than the library's generic QTC_ERR_FORMAT message. */
static int non_image_qtc(const uint8_t *in, size_t len)
{
    if (len < 5 || in[0] != 'S' || in[1] != 'G' || in[2] != 'C' || in[3] != 0x01)
        return 0;
    return in[4] != QTC_LENS_IMAGE && in[4] != QTC_LENS_IMAGE_MESH &&
           in[4] != QTC_LENS_IMAGE_ACCRETION && in[4] != QTC_LENS_IMAGE_DCT;
}

static int cmd_decompress_image(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qtc di IN.qtc OUT.png\n");
        return 2;
    }
    size_t in_len = 0;
    uint8_t *in = read_file(argv[0], &in_len);
    if (!in) return 1;
    if (non_image_qtc(in, in_len)) {
        fprintf(stderr, "qtc: %s is a qtc stream but not an image (lens=%s)\n", argv[0], lens_name(in[4]));
        fprintf(stderr, "     decompress it with 'qtc d', or compress photos with 'qtc ci'\n");
        free(in);
        return 1;
    }

    /* 16-bit IMAGE stream (payload depth byte at container offset 16+9). */
    if (in_len > 25 && in[4] == QTC_LENS_IMAGE && in[25] == 2) {
        uint16_t *px16 = NULL;
        uint32_t w = 0, h = 0;
        uint8_t ch = 0;
        int rc = qtc_decompress_image16(in, in_len, &px16, &w, &h, &ch);
        free(in);
        if (rc != QTC_OK) {
            fprintf(stderr, "qtc: image16 decompress failed: %s\n", qtc_error_string(rc));
            return 1;
        }
        if (png16_write(argv[1], px16, w, h, ch) != 0) {
            fprintf(stderr, "qtc: cannot write 16-bit PNG %s\n", argv[1]);
            free(px16);
            return 1;
        }
        printf("wrote %ux%u ch=%u 16-bit -> %s\n", w, h, (unsigned)ch, argv[1]);
        free(px16);
        return 0;
    }

    uint8_t *px = NULL;
    uint32_t w = 0, h = 0;
    uint8_t ch = 0;
    int rc = qtc_decompress_image(in, in_len, &px, &w, &h, &ch);
    free(in);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: image decompress failed: %s\n", qtc_error_string(rc));
        return 1;
    }
    int stride = (ch > 0 && w <= (uint32_t)(INT_MAX / (uint32_t)ch)) ? (int)(w * (uint32_t)ch) : 0;
    if (stride <= 0 || !stbi_write_png(argv[1], (int)w, (int)h, ch, px, stride)) {
        fprintf(stderr, "qtc: cannot write PNG %s\n", argv[1]);
        free(px);
        return 1;
    }
    printf("wrote %ux%u ch=%u -> %s\n", w, h, (unsigned)ch, argv[1]);
    free(px);
    return 0;
}

/* ── WAV parsing ──────────────────────────────────────────────────────── */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint8_t *pcm;
    size_t   pcm_len;
    uint32_t rate;
    uint8_t  channels;
    uint8_t  bps;
    uint8_t  is_float;   /* 1 = 32-bit IEEE float samples (WAV tag 3) */
} wav_t;

/* Parse a PCM WAV (8-bit unsigned, 16-bit or 24-bit signed LE) or a
 * 32-bit IEEE-float WAV (format tag 3 — common microphone capture).
 * Returns -2 (no error printed) when the file is not a supported WAV,
 * so the caller can fall back to the CoreAudio decoder. */
static int wav_load(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return -1;
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return -2;
    }
    size_t off = 12;
    int have_fmt = 0;
    uint16_t fmt = 1;
    while (off + 8 <= len) {
        uint32_t csz = rd32le(buf + off + 4);
        size_t body = off + 8;
        if (csz > len - body)            /* clamp hostile chunk sizes */
            csz = (uint32_t)(len - body);
        if (!memcmp(buf + off, "fmt ", 4) && csz >= 16) {
            fmt = rd16(buf + body);
            w->channels  = buf[body + 2];
            w->rate      = rd32le(buf + body + 4);
            w->bps       = buf[body + 14];
            have_fmt = 1;
            /* tag 1 = integer PCM, tag 3 = IEEE float; anything else is a
             * compressed format — let CoreAudio decode it. */
            if (fmt != 1 && fmt != 3) {
                free(buf);
                return -2;
            }
        } else if (!memcmp(buf + off, "data", 4)) {
            w->pcm = (uint8_t *)malloc(csz ? csz : 1);
            if (!w->pcm) { free(buf); return -1; }
            memcpy(w->pcm, buf + body, csz);
            w->pcm_len = csz;
        }
        off = body + csz + (csz & 1);    /* chunks are word-aligned */
    }
    free(buf);
    if (!have_fmt || !w->pcm) {
        fprintf(stderr, "qtc: %s is missing its fmt/data chunk\n", path);
        free(w->pcm);
        w->pcm = NULL;
        return -1;
    }
    w->is_float = (fmt == 3);
    int layout_ok = (w->channels > 0 && w->channels <= 16 &&
                     w->rate > 0 && w->rate <= 384000);
    if (w->is_float)
        layout_ok = layout_ok && (w->bps == 32);
    else
        layout_ok = layout_ok && (w->bps == 8 || w->bps == 16 || w->bps == 24);
    if (!layout_ok) {
        fprintf(stderr, "qtc: unsupported WAV layout (ch=%u bps=%u rate=%u fmt=%u)\n", (unsigned)w->channels, (unsigned)w->bps, w->rate, (unsigned)fmt);
        free(w->pcm);
        w->pcm = NULL;
        return -1;
    }
    return 0;
}

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

/* Decode any CoreAudio-supported format (MP3, AAC/M4A, ALAC, AIFF,
 * FLAC, ...) into 16-bit LE PCM via ExtAudioFile. Fills wav_t with
 * bps=16. Decoded output capped at 1 GiB. */
static int anyaudio_load(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, (const UInt8 *)path, strlen(path), false);
    if (!url) {
        fprintf(stderr, "qtc: cannot open %s\n", path);
        return -1;
    }
    ExtAudioFileRef af = NULL;
    OSStatus st = ExtAudioFileOpenURL(url, &af);
    CFRelease(url);
    if (st != noErr || !af) {
        fprintf(stderr, "qtc: cannot decode %s (OSStatus %d) -- unsupported or corrupt?\n", path, (int)st);
        return -1;
    }

    AudioStreamBasicDescription src;
    UInt32 sz = sizeof(src);
    st = ExtAudioFileGetProperty(af, kExtAudioFileProperty_FileDataFormat, &sz, &src);
    if (st != noErr || src.mSampleRate < 1.0 || src.mSampleRate > 384000.0 ||
        src.mChannelsPerFrame == 0 || src.mChannelsPerFrame > 16) {
        fprintf(stderr, "qtc: %s has an unsupported layout (OSStatus %d)\n", path, (int)st);
        ExtAudioFileDispose(af);
        return -1;
    }

    AudioStreamBasicDescription dst;
    memset(&dst, 0, sizeof(dst));
    dst.mSampleRate       = src.mSampleRate;
    dst.mFormatID         = kAudioFormatLinearPCM;
    dst.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    dst.mBitsPerChannel   = 16;
    dst.mChannelsPerFrame = src.mChannelsPerFrame;
    dst.mBytesPerFrame    = 2 * src.mChannelsPerFrame;
    dst.mFramesPerPacket  = 1;
    dst.mBytesPerPacket   = 2 * src.mChannelsPerFrame;
    st = ExtAudioFileSetProperty(af, kExtAudioFileProperty_ClientDataFormat, sizeof(dst), &dst);
    if (st != noErr) {
        fprintf(stderr, "qtc: cannot set decode format for %s (OSStatus %d)\n", path, (int)st);
        ExtAudioFileDispose(af);
        return -1;
    }

    /* Read in chunks into a growing buffer; some streams report no
     * length, so kExtAudioFileProperty_FileLengthFrames is not trusted. */
    const UInt32 chunk_frames = 16384;
    size_t chunk_bytes = (size_t)chunk_frames * dst.mBytesPerFrame;
    uint8_t *tmp = (uint8_t *)malloc(chunk_bytes);
    uint8_t *pcm = NULL;
    size_t pcm_len = 0, cap = 0;
    int ok = (tmp != NULL);
    while (ok) {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = dst.mChannelsPerFrame;
        abl.mBuffers[0].mDataByteSize = (UInt32)chunk_bytes;
        abl.mBuffers[0].mData = tmp;
        UInt32 n = chunk_frames;
        st = ExtAudioFileRead(af, &n, &abl);
        if (st != noErr) {
            fprintf(stderr, "qtc: decode error on %s (OSStatus %d)\n", path, (int)st);
            ok = 0;
            break;
        }
        if (n == 0) break;
        size_t got = (size_t)n * dst.mBytesPerFrame;
        if (pcm_len + got > QTC_CLI_MAX_INPUT) {
            fprintf(stderr, "qtc: %s decodes to more than 1 GiB of PCM\n", path);
            ok = 0;
            break;
        }
        if (pcm_len + got > cap) {
            size_t ncap = cap ? cap * 2 : (4u * 1024u * 1024u);
            if (ncap < pcm_len + got) ncap = pcm_len + got;
            if (ncap > QTC_CLI_MAX_INPUT) ncap = QTC_CLI_MAX_INPUT;
            uint8_t *np = (uint8_t *)realloc(pcm, ncap);
            if (!np) { ok = 0; break; }
            pcm = np;
            cap = ncap;
        }
        memcpy(pcm + pcm_len, tmp, got);
        pcm_len += got;
    }
    free(tmp);
    ExtAudioFileDispose(af);
    if (!ok || pcm_len == 0) {
        if (ok) fprintf(stderr, "qtc: %s decoded to zero frames\n", path);
        free(pcm);
        return -1;
    }
    w->pcm = pcm;
    w->pcm_len = pcm_len;
    w->rate = (uint32_t)src.mSampleRate;
    w->channels = (uint8_t)src.mChannelsPerFrame;
    w->bps = 16;
    return 0;
}
#endif /* __APPLE__ */

static int cmd_ca(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: qtc ca IN.(wav|mp3|m4a|aiff|flac|...) OUT.qtc [-q Q] [-b KBPS]\n");
        fprintf(stderr, "  -q 1.0 = lossless (default), below 1.0 = lossy\n");
        fprintf(stderr, "  -b KBPS = explicit lossy bitrate target (overrides -q size mapping)\n");
        return 2;
    }
    float quality = 1.0f;
    int target_kbps = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-q") && i + 1 < argc)
            quality = parse_quality(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            double b = parse_double(argv[++i], 0.0);
            if (b < 16.0) b = 16.0;
            if (b > 1024.0) b = 1024.0;
            target_kbps = (int)b;
        }
    }

    wav_t w;
    int wrc = wav_load(argv[0], &w);
#if defined(__APPLE__)
    if (wrc == -2) {   /* not plain PCM WAV: decode via CoreAudio */
        fprintf(stderr, "qtc: decoding %s via CoreAudio...\n", argv[0]);
        wrc = anyaudio_load(argv[0], &w);
    }
#else
    if (wrc == -2)
        fprintf(stderr, "qtc: %s is not a PCM WAV (other formats need macOS)\n", argv[0]);
#endif
    if (wrc != 0) return 1;
    size_t bytes_per_frame = (size_t)w.channels * (w.bps / 8u);
    if (bytes_per_frame == 0 || w.pcm_len < bytes_per_frame) {
        fprintf(stderr, "qtc: WAV too small for its own header\n");
        free(w.pcm);
        return 1;
    }
    uint32_t nframes = (uint32_t)(w.pcm_len / bytes_per_frame);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.quality = quality;
    opts.target_kbps = target_kbps;
    /* An explicit bitrate target means lossy, even at the default -q 1.0. */
    opts.mode = (quality >= 1.0f && target_kbps == 0) ? QTC_MODE_LOSSLESS
                                                      : QTC_MODE_LOSSY;

    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc;
    if (w.is_float)
        rc = qtc_compress_audio_float((const float *)w.pcm, nframes,
                                      w.channels, &out, &out_len, &opts);
    else
        rc = qtc_compress_audio(w.pcm, nframes, w.channels, w.bps,
                                &out, &out_len, &opts);
    free(w.pcm);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: audio compress failed: %s\n", qtc_error_string(rc));
        return 1;
    }
    if (write_file(argv[1], out, out_len) != 0) {
        free(out);
        return 1;
    }
    int lens_id = (out_len >= 5) ? out[4] : -1;
    double ratio = (out_len > 0) ? (double)w.pcm_len / (double)out_len : 0.0;
    char depth[16];
    if (w.is_float)
        snprintf(depth, sizeof(depth), "float32");
    else
        snprintf(depth, sizeof(depth), "bps=%u", (unsigned)w.bps);
    printf("%u frames ch=%u %s @ %u Hz: %zu -> %zu bytes (%.2fx), lens=%s\n", nframes, (unsigned)w.channels, depth, w.rate, w.pcm_len, out_len, ratio, lens_name(lens_id));
    printf("note: sample rate is NOT stored in the stream; play with: qtc play %s -r %u\n", argv[1], w.rate);
    free(out);
    return 0;
}

/* ── audio playback (CoreAudio AudioQueue) ────────────────────────────── */

#if defined(__APPLE__)
#include <AudioToolbox/AudioQueue.h>

typedef struct {
    float          *samples;      /* interleaved stereo f32 */
    size_t          frames;
    size_t          cursor;
    float           gain;
    float           last_peak;
    int             drained;
    pthread_mutex_t lock;
} play_ctx_t;

static void play_fill(play_ctx_t *pc, AudioQueueBufferRef buf)
{
    pthread_mutex_lock(&pc->lock);
    size_t cap_frames = buf->mAudioDataBytesCapacity / (2u * sizeof(float));
    size_t remain = pc->frames - pc->cursor;
    size_t n = (remain < cap_frames) ? remain : cap_frames;
    float *dst = (float *)buf->mAudioData;
    const float *src = pc->samples + pc->cursor * 2;
    float peak = 0.0f;
    for (size_t i = 0; i < n * 2; i++) {
        float v = src[i] * pc->gain;
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        dst[i] = v;
        float a = (v < 0.0f) ? -v : v;
        if (a > peak) peak = a;
    }
    buf->mAudioDataByteSize = (UInt32)(n * 2 * sizeof(float));
    pc->cursor += n;
    pc->last_peak = peak;
    if (pc->cursor >= pc->frames)
        pc->drained = 1;
    pthread_mutex_unlock(&pc->lock);
}

static void play_cb(void *ud, AudioQueueRef aq, AudioQueueBufferRef buf)
{
    play_ctx_t *pc = (play_ctx_t *)ud;
    play_fill(pc, buf);
    if (buf->mAudioDataByteSize == 0)
        return;                          /* exhausted; timer stops queue */
    AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
}

static volatile sig_atomic_t g_sigint = 0;

static void on_sigint(int sig) { (void)sig; g_sigint = 1; }

typedef struct {
    play_ctx_t      *pc;
    AudioQueueRef    aq;
    CFRunLoopRef     rl;
    double           rate_hint;
} play_ctl_t;

static void meter_print(const play_ctx_t *pc, double rate)
{
    char bar[41];
    int fill = (int)(pc->last_peak * 40.0f + 0.5f);
    if (fill < 0) fill = 0;
    if (fill > 40) fill = 40;
    for (int i = 0; i < 40; i++)
        bar[i] = (i < fill) ? '#' : '.';
    bar[40] = '\0';
    double pos = (double)pc->cursor / rate;
    double dur = (double)pc->frames / rate;
    printf("\r  [%s] %6.2fs / %6.2fs ", bar, pos, dur);
    fflush(stdout);
}

static void play_timer_cb(CFRunLoopTimerRef t, void *ud)
{
    (void)t;
    play_ctl_t *ctl = (play_ctl_t *)ud;
    int stop = g_sigint || ctl->pc->drained;
    if (!stop) {
        meter_print(ctl->pc, (double)ctl->rate_hint);
        return;
    }
    printf("\n");
    AudioQueueStop(ctl->aq, false);
    CFRunLoopStop(ctl->rl);
}

/* Decode interleaved int PCM (8/16/24-bit, up to 16ch) into stereo f32. */
static float *pcm_to_stereo(const uint8_t *pcm, size_t nframes, uint8_t ch, uint8_t bps)
{
    float *out = (float *)malloc(nframes * 2 * sizeof(float));
    if (!out) return NULL;
    for (size_t f = 0; f < nframes; f++) {
        double L = 0, R = 0;
        int nl = 0, nr = 0;
        for (uint8_t c = 0; c < ch; c++) {
            double v = 0;
            size_t idx = f * (size_t)ch + c;
            if (bps == 8) {
                v = ((int)pcm[idx] - 128) / 128.0;
            } else if (bps == 16) {
                int16_t s = (int16_t)(pcm[idx * 2] | (pcm[idx * 2 + 1] << 8));
                v = s / 32768.0;
            } else {                      /* 24-bit LE, sign-extended */
                uint32_t u = (uint32_t)pcm[idx * 3] | ((uint32_t)pcm[idx * 3 + 1] << 8) | ((uint32_t)pcm[idx * 3 + 2] << 16);
                int32_t s = (u & 0x800000u) ? (int32_t)(u - 0x1000000u) : (int32_t)u;
                v = s / 8388608.0;
            }
            if ((c & 1) == 0) { L += v; nl++; }
            else              { R += v; nr++; }
        }
        if (nl == 0) nl = 1;
        if (nr == 0) { R = L; nr = 1; }
        out[f * 2]     = (float)(L / nl);
        out[f * 2 + 1] = (float)(R / nr);
    }
    return out;
}

/* Mix down interleaved float32 audio to stereo f32 (for AUDIO_FLOAT). */
static float *float_to_stereo(const float *pcm, size_t nframes, uint8_t ch)
{
    float *out = (float *)malloc(nframes * 2 * sizeof(float));
    if (!out) return NULL;
    for (size_t f = 0; f < nframes; f++) {
        double L = 0, R = 0;
        int nl = 0, nr = 0;
        for (uint8_t c = 0; c < ch; c++) {
            double v = pcm[f * (size_t)ch + c];
            if ((c & 1) == 0) { L += v; nl++; }
            else              { R += v; nr++; }
        }
        if (nl == 0) nl = 1;
        if (nr == 0) { R = L; nr = 1; }
        out[f * 2]     = (float)(L / nl);
        out[f * 2 + 1] = (float)(R / nr);
    }
    return out;
}
#endif /* __APPLE__ */

/* True for a qtc stream whose lens is not one of the audio lenses. */
static int non_audio_qtc(const uint8_t *in, size_t len)
{
    if (len < 5 || in[0] != 'S' || in[1] != 'G' || in[2] != 'C' || in[3] != 0x01)
        return 0;
    return in[4] != QTC_LENS_AUDIO && in[4] != QTC_LENS_AUDIO_MDCT &&
           in[4] != QTC_LENS_AUDIO_MDCT2 && in[4] != QTC_LENS_AUDIO_FLOAT;
}

static int cmd_play(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: qtc play IN.qtc [-r HZ] [-v GAIN]\n");
        return 2;
    }
    double rate = 48000.0, gain = 1.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
            rate = parse_double(argv[++i], 48000.0);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc)
            gain = parse_double(argv[++i], 1.0);
    }
    if (rate < 8000.0) rate = 8000.0;
    if (rate > 192000.0) rate = 192000.0;
    if (gain < 0.0) gain = 0.0;
    if (gain > 2.0) gain = 2.0;

    size_t in_len = 0;
    uint8_t *in = read_file(argv[0], &in_len);
    if (!in) return 1;
    if (non_audio_qtc(in, in_len)) {
        fprintf(stderr, "qtc: %s is a qtc stream but not audio (lens=%s)\n", argv[0], lens_name(in[4]));
        fprintf(stderr, "     recover the original file with 'qtc d', or make qtc audio from a WAV with 'qtc ca'\n");
        free(in);
        return 1;
    }
    int is_float_lens = (in_len >= 5 && in[4] == QTC_LENS_AUDIO_FLOAT);
    uint8_t *pcm = NULL;
    float *fpcm = NULL;
    uint32_t nframes = 0;
    uint8_t ch = 0, bps = 0;
    int rc;
    if (is_float_lens) {
        rc = qtc_decompress_audio_float(in, in_len, &fpcm, &nframes, &ch);
        bps = 32;
    } else {
        rc = qtc_decompress_audio(in, in_len, &pcm, &nframes, &ch, &bps);
    }
    free(in);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: audio decode failed: %s\n", qtc_error_string(rc));
        free(pcm);
        free(fpcm);
        return 1;
    }
    if (is_float_lens)
        printf("decoded %u frames ch=%u float32 (%.2fs at %.0f Hz)\n", nframes, (unsigned)ch, (double)nframes / rate, rate);
    else
        printf("decoded %u frames ch=%u bps=%u (%.2fs at %.0f Hz)\n", nframes, (unsigned)ch, (unsigned)bps, (double)nframes / rate, rate);

#if defined(__APPLE__)
    float *stereo = is_float_lens ? float_to_stereo(fpcm, nframes, ch)
                                  : pcm_to_stereo(pcm, nframes, ch, bps);
    free(pcm);
    free(fpcm);
    if (!stereo) return 1;

    play_ctx_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.samples = stereo;
    pc.frames  = nframes;
    pc.gain    = (float)gain;
    pthread_mutex_init(&pc.lock, NULL);

    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket   = 2 * sizeof(float);
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = 2 * sizeof(float);
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel   = 32;

    AudioQueueRef aq = NULL;
    OSStatus st = AudioQueueNewOutput(&asbd, play_cb, &pc, NULL, NULL, 0, &aq);
    if (st != noErr) {
        fprintf(stderr, "qtc: AudioQueueNewOutput failed (%d)\n", (int)st);
        free(stereo);
        return 1;
    }

    enum { NBUF = 6 };
    const UInt32 buf_bytes = 4096u * 2u * (UInt32)sizeof(float);
    AudioQueueBufferRef bufs[NBUF];
    for (int i = 0; i < NBUF; i++) {
        AudioQueueAllocateBuffer(aq, buf_bytes, &bufs[i]);
        play_fill(&pc, bufs[i]);
        if (bufs[i]->mAudioDataByteSize > 0)
            AudioQueueEnqueueBuffer(aq, bufs[i], 0, NULL);
    }

    signal(SIGINT, on_sigint);
    AudioQueueStart(aq, NULL);

    play_ctl_t ctl;
    ctl.pc = &pc;
    ctl.aq = aq;
    ctl.rl = CFRunLoopGetMain();
    ctl.rate_hint = rate;
    CFAbsoluteTime fire = CFAbsoluteTimeGetCurrent() + 0.05;
    CFRunLoopTimerContext tctx = {0, &ctl, NULL, NULL, NULL};
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, fire, 0.06, 0, 0, play_timer_cb, &tctx);
    CFRunLoopAddTimer(ctl.rl, timer, kCFRunLoopCommonModes);

    printf("playing (Ctrl-C stops)...\n");
    CFRunLoopRun();

    AudioQueueDispose(aq, true);
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    pthread_mutex_destroy(&pc.lock);
    free(stereo);
    signal(SIGINT, SIG_DFL);
    return 0;
#else
    fprintf(stderr, "qtc: audio playback requires macOS\n");
    free(pcm);
    return 1;
#endif
}

/* ── native image viewer (Cocoa, software-rendered) ───────────────────── */

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>

@interface QtcImageView : NSView
{
@public
    uint8_t   *rgba;
    NSUInteger iw, ih;
}
@end

@implementation QtcImageView
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect b = [self bounds];
    [[NSColor colorWithCalibratedWhite:0.08 alpha:1.0] set];
    NSRectFill(b);
    if (!rgba || iw == 0 || ih == 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bmp = CGBitmapContextCreate(rgba, (size_t)iw, (size_t)ih, 8, (size_t)iw * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    if (!bmp) { CGColorSpaceRelease(cs); return; }
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    if (img) {
        CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
        CGFloat scale = fmin(b.size.width / (CGFloat)iw, b.size.height / (CGFloat)ih);
        CGFloat dw = (CGFloat)iw * scale;
        CGFloat dh = (CGFloat)ih * scale;
        CGRect dr = CGRectMake((b.size.width - dw) / 2.0, (b.size.height - dh) / 2.0, dw, dh);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationMedium);
        CGContextDrawImage(ctx, dr, img);
        CGImageRelease(img);
    }
    CGContextRelease(bmp);
    CGColorSpaceRelease(cs);
}
@end

@interface QtcAppDel : NSObject <NSApplicationDelegate>
@end
@implementation QtcAppDel
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
    (void)a;
    return YES;
}
@end

static QtcAppDel *g_del = nil;
static NSWindow *g_win = nil;
static QtcImageView *g_view = nil;

static int cocoa_show(const uint8_t *px, uint32_t w, uint32_t h, uint8_t ch)
{
    if (w == 0 || h == 0 || (uint64_t)w * (uint64_t)h > ((uint64_t)1 << 28))
        return -1;
    size_t npix = (size_t)w * (size_t)h;
    uint8_t *rgba = (uint8_t *)malloc(npix * 4);
    if (!rgba) return -1;
    for (size_t i = 0; i < npix; i++) {
        if (ch == 1) {
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = px[i];
        } else if (ch >= 3) {
            rgba[i * 4]     = px[i * (size_t)ch];
            rgba[i * 4 + 1] = px[i * (size_t)ch + 1];
            rgba[i * 4 + 2] = px[i * (size_t)ch + 2];
        } else {
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        }
        rgba[i * 4 + 3] = 255;
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    g_del = [[QtcAppDel alloc] init];
    [NSApp setDelegate:g_del];

    NSRect avail = [[NSScreen mainScreen] visibleFrame];
    CGFloat scale = fmin(1.0, fmin(avail.size.width / (CGFloat)w, avail.size.height / (CGFloat)h));
    NSRect frame = NSMakeRect(0, 0, floor((CGFloat)w * scale), floor((CGFloat)h * scale));

    g_win = [[NSWindow alloc] initWithContentRect:frame styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable) backing:NSBackingStoreBuffered defer:NO];
    g_view = [[QtcImageView alloc] initWithFrame:frame];
    g_view->rgba = rgba;
    g_view->iw = w;
    g_view->ih = h;
    [g_win setContentView:g_view];
    g_win.title = [NSString stringWithFormat:@"%ux%u -- qtc renderer", (unsigned)w, (unsigned)h];
    g_win.releasedWhenClosed = NO;
    [g_win center];
    [g_win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];

    free(rgba);
    return 0;
}
#endif /* __APPLE__ */

static int cmd_view(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: qtc view IN.qtc\n");
        return 2;
    }
    size_t in_len = 0;
    uint8_t *in = read_file(argv[0], &in_len);
    if (!in) return 1;
    if (non_image_qtc(in, in_len)) {
        fprintf(stderr, "qtc: %s is a qtc stream but not an image (lens=%s)\n", argv[0], lens_name(in[4]));
        fprintf(stderr, "     decompress it with 'qtc d', or compress photos with 'qtc ci'\n");
        free(in);
        return 1;
    }
    uint8_t *px = NULL;
    uint32_t w = 0, h = 0;
    uint8_t ch = 0;
    int rc = qtc_decompress_image(in, in_len, &px, &w, &h, &ch);
    free(in);
    if (rc != QTC_OK) {
        fprintf(stderr, "qtc: image decode failed: %s\n", qtc_error_string(rc));
        return 1;
    }
#if defined(__APPLE__)
    int grc = cocoa_show(px, w, h, ch);
#else
    int grc = -1;
#endif
    free(px);
    if (grc != 0) {
        fprintf(stderr, "qtc: native viewer unavailable here\n");
        return 1;
    }
    return 0;
}

/* ── info ─────────────────────────────────────────────────────────────── */

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: qtc info FILE\n");
        return 2;
    }
    size_t len = 0;
    uint8_t *in = read_file(argv[0], &len);
    if (!in) return 1;
    if (len < 16 || in[0] != 'S' || in[1] != 'G' || in[2] != 'C' || in[3] != 0x01) {
        fprintf(stderr, "qtc: %s is not a qtc stream (bad magic)\n", argv[0]);
        free(in);
        return 1;
    }
    uint64_t orig = 0;
    for (int i = 0; i < 8; i++)
        orig |= (uint64_t)in[8 + i] << (8 * i);
    printf("file:          %s\n", argv[0]);
    printf("stream size:   %zu bytes\n", len);
    printf("lens:          %u (%s)\n", (unsigned)in[4], lens_name(in[4]));
    printf("flags:         0x%02x\n", (unsigned)in[5]);
    printf("original size: %llu bytes\n", (unsigned long long)orig);
    if (orig > 0 && len > 0)
        printf("ratio:         %.2fx\n", (double)orig / (double)len);
    free(in);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "qtc manual testing CLI -- commands:\n");
        fprintf(stderr, "  c IN OUT [-q Q] [-l LENS]    compress (generic)\n");
        fprintf(stderr, "  d IN OUT                     decompress\n");
        fprintf(stderr, "  ci IN.(png|jpg) OUT.qtc [-q Q] lossy photo compress\n");
        fprintf(stderr, "  di IN.qtc OUT.png            decompress to PNG\n");
        fprintf(stderr, "  ca IN.audio OUT.qtc [-q Q]   audio to qtc (wav/mp3/m4a/...)\n");
        fprintf(stderr, "  play IN.qtc [-r HZ] [-v G]   play audio + meter\n");
        fprintf(stderr, "  view IN.qtc                  open image window\n");
        fprintf(stderr, "  info FILE                    show stream info\n");
        fprintf(stderr, "  version\n");
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version")) {
        printf("%s\n", qtc_version_string());
        return 0;
    }
    if (!strcmp(cmd, "c"))    return cmd_compress(argc - 2, argv + 2);
    if (!strcmp(cmd, "d"))    return cmd_decompress(argc - 2, argv + 2);
    if (!strcmp(cmd, "ci"))   return cmd_compress_image(argc - 2, argv + 2);
    if (!strcmp(cmd, "di"))   return cmd_decompress_image(argc - 2, argv + 2);
    if (!strcmp(cmd, "ca"))   return cmd_ca(argc - 2, argv + 2);
    if (!strcmp(cmd, "play")) return cmd_play(argc - 2, argv + 2);
    if (!strcmp(cmd, "view")) return cmd_view(argc - 2, argv + 2);
    if (!strcmp(cmd, "info")) return cmd_info(argc - 2, argv + 2);
    fprintf(stderr, "qtc: unknown command '%s'\n", cmd);
    return 2;
}
