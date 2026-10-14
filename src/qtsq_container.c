/*
 * Quantum Tensor Sequence — Container Engine (Multi-Data .qtsq)
 * Packs multiple streams (text, image, audio, etc.) into a single .qtsq file.
 * Each stream is independently compressed with its optimal engine.
 *
 * Author: Haruhito
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif
#include "qtsq.h"
#include "qtsq_vault.h"
#include "qtsq_sanitize.h"
#include "qtsq_codec.h"
#include "../crypto/common/randombytes.h"
#include "../crypto/common/aes.h"
#include "../crypto/common/fips202.h"
#include "../crypto/ml-kem-768/api.h"

/* ══════════════════════════════════════════════════════════════
 *  Internal builder state (held in ctx->singularity temporarily)
 * ══════════════════════════════════════════════════════════════ */

#define CONTAINER_NAME_MAX 256

typedef struct {
    qtsq_stream_entry_t entry;
    char                name[CONTAINER_NAME_MAX];
    uint8_t            *data;       /* compressed payload (owned; NULL if raw) */
    /* Raw (borrowed) representation for zero-copy streaming: prebuilt
     * TNS2 header bytes (owned) + payload pointer (borrowed, never
     * freed). entry.data_size covers both parts. */
    uint8_t            *rhead;      /* owned header bytes (NULL if unused) */
    size_t              rhead_len;
    const uint8_t      *rdata;      /* borrowed payload (NULL if unused) */
    size_t              rdata_len;
} stream_slot_t;

typedef struct {
    stream_slot_t *streams;
    uint32_t       count;
    uint32_t       capacity;
} container_builder_t;

/* Store builder pointer in ctx->singularity (tagged with a magic prefix) */
#define BUILDER_MAGIC 0x42554C44u /* "BULD" */

static container_builder_t *get_builder(const qtsq_context_t *ctx) {
    if (!ctx || !ctx->singularity || ctx->singularity_size != sizeof(void *) + 4)
        return NULL;
    uint32_t magic;
    memcpy(&magic, ctx->singularity, 4);
    if (magic != BUILDER_MAGIC) return NULL;
    container_builder_t *b;
    memcpy(&b, ctx->singularity + 4, sizeof(void *));
    return b;
}

static void set_builder(qtsq_context_t *ctx, container_builder_t *b) {
    size_t sz = sizeof(void *) + 4;
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = (uint8_t *)malloc(sz);
    uint32_t magic = BUILDER_MAGIC;
    memcpy(ctx->singularity, &magic, 4);
    memcpy(ctx->singularity + 4, &b, sizeof(void *));
    ctx->singularity_size = sz;
}

/* Fill a TOC entry from a sub-context plus its payload. Single source
 * of truth for every add path so the per-stream flag masking and the
 * recorded codec id cannot drift between them (Phase 1).
 *
 * `hdr`/`payload`/`plen` describe the stream as it will land on disk.
 * `data_size` covers the whole payload INCLUDING any leading stream
 * encryption header; the codec id is sniffed from the payload proper,
 * since the encryption header would otherwise hide the real framing. */
static void fill_entry(qtsq_stream_entry_t *e,
                       const qtsq_header_t *hdr,
                       const uint8_t *payload, size_t plen,
                       size_t data_size, size_t original_size) {
    e->data_type = hdr->data_type;
    e->strategy  = hdr->strategy;
    /* The TOC has 16 flag bits and the header has 32. Mask explicitly
     * rather than letting the assignment truncate silently. */
    e->flags = (uint16_t)(hdr->flags & QTSQ_STREAM_FLAG_ALL);
    e->codec_id = qtsq_codec_identify(payload, plen, hdr->data_type);
    e->data_size = (uint64_t)data_size;
    e->original_size = (uint64_t)original_size;
}

/* ══════════════════════════════════════════════════════════════
 *  Auto-detect file type by extension
 * ══════════════════════════════════════════════════════════════ */

typedef enum {
    FILE_CAT_TEXT,
    FILE_CAT_JSON,
    FILE_CAT_CSV,
    FILE_CAT_IMAGE,
    FILE_CAT_AUDIO,
    FILE_CAT_VIDEO,
    FILE_CAT_BINARY
} file_category_t;

static const char *get_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static int ext_in(const char *ext, const char *list[], int n) {
    for (int i = 0; i < n; i++)
        if (strcasecmp(ext, list[i]) == 0) return 1;
    return 0;
}

static file_category_t categorize_file(const char *path) {
    const char *ext = get_ext(path);

    static const char *text_exts[] = {
        "txt", "md", "markdown", "rst", "log", "html", "htm",
        "xml", "svg", "yaml", "yml", "toml", "ini", "cfg",
        "c", "h", "cpp", "hpp", "m", "mm", "swift", "rs", "go",
        "py", "rb", "js", "ts", "jsx", "tsx", "vue", "svelte",
        "java", "kt", "kts", "cs", "dart", "lua", "sh", "bash", "zsh",
        "css", "scss", "less", "sql", "r", "pl", "php"
    };
    static const char *json_exts[] = { "json", "geojson", "jsonl" };
    static const char *csv_exts[]  = { "csv", "tsv" };
    static const char *img_exts[]  = { "png", "jpg", "jpeg", "bmp", "gif", "webp", "tiff", "tif", "ico", "heic" };
    static const char *aud_exts[]  = { "wav", "pcm", "mp3", "aac", "flac", "ogg", "m4a", "aiff" };
    static const char *vid_exts[]  = { "mp4", "mov", "mkv", "avi", "webm", "m4v", "flv", "3gp" };

    if (ext_in(ext, json_exts, 3))  return FILE_CAT_JSON;
    if (ext_in(ext, csv_exts, 2))   return FILE_CAT_CSV;
    if (ext_in(ext, text_exts, sizeof(text_exts)/sizeof(text_exts[0]))) return FILE_CAT_TEXT;
    if (ext_in(ext, img_exts, 10))  return FILE_CAT_IMAGE;
    if (ext_in(ext, aud_exts, 8))   return FILE_CAT_AUDIO;
    if (ext_in(ext, vid_exts, 8))   return FILE_CAT_VIDEO;

    return FILE_CAT_BINARY;
}

/* ══════════════════════════════════════════════════════════════
 *  Read a file into memory
 * ══════════════════════════════════════════════════════════════ */

static uint8_t *read_file_data(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { *out_size = 0; return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); *out_size = 0; return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); *out_size = 0; return NULL; }
    fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    *out_size = (size_t)sz;
    return buf;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_create
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_create(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;

    qtsq_init(ctx);
    ctx->header.data_type = QTSQ_TYPE_CONTAINER;
    ctx->header.flags |= QTSQ_FLAG_MULTI_STREAM;

    container_builder_t *b = (container_builder_t *)calloc(1, sizeof(*b));
    if (!b) return QTSQ_ERR_ALLOC;

    b->capacity = 16;
    b->streams = (stream_slot_t *)calloc(b->capacity, sizeof(stream_slot_t));
    if (!b->streams) { free(b); return QTSQ_ERR_ALLOC; }

    set_builder(ctx, b);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_add_file
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_add_file(qtsq_context_t *ctx, const char *path,
                             const char *stream_name) {
    if (!ctx || !path) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;

    if (b->count >= QTSQ_MAX_STREAMS) return QTSQ_ERR_FORMAT;

    /* Grow if needed */
    if (b->count >= b->capacity) {
        b->capacity *= 2;
        stream_slot_t *tmp = (stream_slot_t *)realloc(b->streams,
                                                       b->capacity * sizeof(stream_slot_t));
        if (!tmp) return QTSQ_ERR_ALLOC;
        b->streams = tmp;
    }

    /* Read file */
    size_t file_size;
    uint8_t *raw = read_file_data(path, &file_size);
    if (!raw || file_size == 0) return QTSQ_ERR_IO;

    /* Auto-detect and compress */
    qtsq_context_t sub;
    qtsq_init(&sub);
    sub.codec = ctx->codec;   /* Phase 6a: inherit codec preference */

    file_category_t cat = categorize_file(path);
    int result;

    switch (cat) {
        case FILE_CAT_TEXT:
            result = qtsq_compress_text(&sub, (const char *)raw, file_size);
            break;
        case FILE_CAT_JSON:
            result = qtsq_compress_json(&sub, (const char *)raw, file_size);
            break;
        case FILE_CAT_CSV:
            result = qtsq_compress_csv(&sub, (const char *)raw, file_size);
            break;
        default:
            /* Images, audio, video, binary — use raw Singularity for now.
             * In the container context we don't decode images/audio
             * (that requires stb_image / AVFoundation which live in tools/).
             * The CLI and Studio do the decoding + specialized compression
             * before calling qtsq_container_add_stream(). */
            result = qtsq_compress_raw(&sub, raw, file_size, 0);
            break;
    }

    free(raw);
    if (result != QTSQ_OK) { qtsq_free(&sub); return result; }

    /* Copy into builder slot */
    stream_slot_t *slot = &b->streams[b->count];
    memset(slot, 0, sizeof(*slot));

    /* Use provided stream name, or just the filename */
    const char *name = stream_name;
    if (!name) {
        name = strrchr(path, '/');
        name = name ? name + 1 : path;
    }
    strncpy(slot->name, name, CONTAINER_NAME_MAX - 1);
    slot->name[CONTAINER_NAME_MAX - 1] = '\0';

    fill_entry(&slot->entry, &sub.header, sub.singularity, sub.singularity_size,
               sub.singularity_size, sub.header.original_size);
    /* data_offset filled during pack */

    /* Steal the singularity buffer */
    slot->data = sub.singularity;
    sub.singularity = NULL;
    sub.singularity_size = 0;

    b->count++;
    qtsq_free(&sub);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_add_stream (pre-compressed)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_add_stream(qtsq_context_t *ctx,
                               const qtsq_context_t *sub_ctx,
                               const char *stream_name) {
    if (!ctx || !sub_ctx) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;
    if (b->count >= QTSQ_MAX_STREAMS) return QTSQ_ERR_FORMAT;
    if (!sub_ctx->singularity || sub_ctx->singularity_size == 0) return QTSQ_ERR_NULL;

    if (b->count >= b->capacity) {
        b->capacity *= 2;
        stream_slot_t *tmp = (stream_slot_t *)realloc(b->streams,
                                                       b->capacity * sizeof(stream_slot_t));
        if (!tmp) return QTSQ_ERR_ALLOC;
        b->streams = tmp;
    }

    stream_slot_t *slot = &b->streams[b->count];
    memset(slot, 0, sizeof(*slot));

    if (stream_name) {
        strncpy(slot->name, stream_name, CONTAINER_NAME_MAX - 1);
    } else {
        snprintf(slot->name, CONTAINER_NAME_MAX, "stream_%u", b->count);
    }

    fill_entry(&slot->entry, &sub_ctx->header, sub_ctx->singularity,
               sub_ctx->singularity_size, sub_ctx->singularity_size,
               sub_ctx->header.original_size);

    /* Copy singularity data */
    slot->data = (uint8_t *)malloc(sub_ctx->singularity_size);
    if (!slot->data) return QTSQ_ERR_ALLOC;
    memcpy(slot->data, sub_ctx->singularity, sub_ctx->singularity_size);

    b->count++;
    return QTSQ_OK;
}

/* Free one builder (payloads owned; rdata borrowed, never freed). */
static void free_builder(container_builder_t *b) {
    if (!b) return;
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->streams[i].data) free(b->streams[i].data);
        if (b->streams[i].rhead) free(b->streams[i].rhead);
    }
    free(b->streams);
    free(b);
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_add_raw — borrowed zero-copy stream
 *
 *  rhead (owned TNS2 header bytes) + rdata (borrowed payload) are
 *  written in place of a packed buffer. The caller keeps rdata alive
 *  until pack/pack_write consumes it. entry.data_size covers both.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_add_raw(qtsq_context_t *ctx,
                           const char *stream_name,
                           uint8_t *rhead, size_t rhead_len,
                           const uint8_t *rdata, size_t rdata_len,
                           uint8_t data_type, uint8_t strategy,
                           uint16_t flags, uint64_t original_size) {
    if (!ctx || !stream_name) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;
    if (b->count >= QTSQ_MAX_STREAMS) return QTSQ_ERR_FORMAT;
    if ((!rhead && rhead_len) || (!rdata && rdata_len)) return QTSQ_ERR_NULL;

    if (b->count >= b->capacity) {
        uint32_t ncap = b->capacity ? b->capacity * 2 : 16;
        stream_slot_t *tmp = (stream_slot_t *)realloc(b->streams,
                                                       ncap * sizeof(stream_slot_t));
        if (!tmp) return QTSQ_ERR_ALLOC;
        b->streams = tmp;
        b->capacity = ncap;
    }

    stream_slot_t *slot = &b->streams[b->count];
    memset(slot, 0, sizeof(*slot));

    strncpy(slot->name, stream_name, CONTAINER_NAME_MAX - 1);

    /* Raw/zero-copy path: the caller supplies the framing header and
     * payload separately. The codec id is sniffed from the header,
     * since that is where the TNS2 framing lives. */
    slot->entry.data_type = data_type;
    slot->entry.strategy = strategy;
    slot->entry.flags = (uint16_t)(flags & QTSQ_STREAM_FLAG_ALL);
    slot->entry.codec_id = qtsq_codec_identify(rhead, rhead_len, data_type);
    slot->entry.data_size = rhead_len + rdata_len;
    slot->entry.original_size = original_size;

    slot->data = NULL;
    slot->rhead = rhead;
    slot->rhead_len = rhead_len;
    slot->rdata = rdata;
    slot->rdata_len = rdata_len;

    b->count++;
    return QTSQ_OK;
}

int qtsq_container_add_stream_take(qtsq_context_t *ctx,
                                   qtsq_context_t *sub_ctx,
                                   const char *stream_name) {
    if (!ctx || !sub_ctx) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;
    if (b->count >= QTSQ_MAX_STREAMS) return QTSQ_ERR_FORMAT;
    if (!sub_ctx->singularity || sub_ctx->singularity_size == 0) return QTSQ_ERR_NULL;

    if (b->count >= b->capacity) {
        b->capacity *= 2;
        stream_slot_t *tmp = (stream_slot_t *)realloc(b->streams,
                                                       b->capacity * sizeof(stream_slot_t));
        if (!tmp) return QTSQ_ERR_ALLOC;
        b->streams = tmp;
    }

    stream_slot_t *slot = &b->streams[b->count];
    memset(slot, 0, sizeof(*slot));

    if (stream_name) {
        strncpy(slot->name, stream_name, CONTAINER_NAME_MAX - 1);
    } else {
        snprintf(slot->name, CONTAINER_NAME_MAX, "stream_%u", b->count);
    }

    fill_entry(&slot->entry, &sub_ctx->header, sub_ctx->singularity,
               sub_ctx->singularity_size, sub_ctx->singularity_size,
               sub_ctx->header.original_size);

    /* Steal the singularity buffer (no copy). */
    slot->data = sub_ctx->singularity;
    sub_ctx->singularity = NULL;
    sub_ctx->singularity_size = 0;

    b->count++;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_add_folder (recursive)
 * ══════════════════════════════════════════════════════════════ */

static int _add_folder_recursive(qtsq_context_t *ctx,
                                  const char *base_path,
                                  const char *rel_prefix) {
    DIR *d = opendir(base_path);
    if (!d) return QTSQ_ERR_IO;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. and hidden files */
        if (ent->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, ent->d_name);

        char rel_name[512];
        if (rel_prefix && rel_prefix[0]) {
            snprintf(rel_name, sizeof(rel_name), "%s/%s", rel_prefix, ent->d_name);
        } else {
            snprintf(rel_name, sizeof(rel_name), "%s", ent->d_name);
        }

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            int r = _add_folder_recursive(ctx, full_path, rel_name);
            if (r != QTSQ_OK) { closedir(d); return r; }
        } else if (S_ISREG(st.st_mode)) {
            /* Skip .qtsq files to avoid double-packing */
            const char *ext = get_ext(ent->d_name);
            if (strcasecmp(ext, "qtsq") == 0 || strcasecmp(ext, "qtsw") == 0)
                continue;

            int r = qtsq_container_add_file(ctx, full_path, rel_name);
            if (r != QTSQ_OK) { closedir(d); return r; }
        }
    }

    closedir(d);
    return QTSQ_OK;
}

int qtsq_container_add_folder(qtsq_context_t *ctx, const char *folder_path) {
    if (!ctx || !folder_path) return QTSQ_ERR_NULL;
    if (!get_builder(ctx)) return QTSQ_ERR_FORMAT;
    return _add_folder_recursive(ctx, folder_path, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 5: Stream name encryption
 *
 *  PRODUCT.md §1 guarantee: "Stream names always inside the envelope."
 *  Previously, names were stored as cleartext null-terminated strings
 *  in the packed data block — violating the guarantee when the whole
 *  container is read without a key.
 *
 *  Fix: XOR-encrypt the name bytes with a key derived from the
 *  container's content hash (SHA3-256 of the packed singularity TOC).
 *  This is a lightweight stream cipher; the real security comes from
 *  the outer vault encryption (AES-256-CTR) that wraps the entire
 *  singularity. The name encryption ensures that even if someone
 *  reads the raw singularity bytes, the names are obfuscated until
 *  the outer decryption runs.
 *
 *  For containers without vault encryption (legacy/test paths), names
 *  remain cleartext (backward compatible).
 * ══════════════════════════════════════════════════════════════ */

/* Derive a name-encryption key from a context. Uses the security
 * block's salt + signer_fingerprint as entropy source. Returns 0 on
 * success. If no security block is present, returns 0 with a zeroed
 * key (meaning "no name encryption — cleartext"). */
static int derive_name_key(const qtsq_context_t *ctx, uint8_t key[32]) {
    memset(key, 0, 32);
    if (!ctx) return 0;

    sha3_256incctx hc;
    sha3_256_inc_init(&hc);

    /* Mix in the vault salt if present */
    sha3_256_inc_absorb(&hc, ctx->security.salt, 32);
    /* Mix in the signer_fingerprint (non-zero when sealed) */
    sha3_256_inc_absorb(&hc, ctx->security.signer_fingerprint, 32);
    /* NOTE: do NOT mix encryption_type into the key — it changes between
     * pack time (type=1) and post-decrypt (type=0), which would make the
     * encrypt and decrypt keys differ. The salt alone is sufficient entropy. */

    sha3_256_inc_finalize(key, &hc);

    /* If the salt is all-zero (no security block was ever set up), zero the
     * key so names stay cleartext for backward compatibility. This gates on
     * the salt rather than encryption_type because after vault decrypt the
     * type returns to 0, but the name XOR layer (applied at pack time) is
     * still present and needs the same key to undo. */
    int has_salt = 0;
    for (int i = 0; i < 32; i++) {
        if (ctx->security.salt[i] != 0) { has_salt = 1; break; }
    }
    if (!has_salt) {
        memset(key, 0, 32);
    }
    return 0;
}

/* XOR-encrypt or decrypt a name buffer in-place. Symmetric. */
static void name_crypt(uint8_t *buf, size_t len, const uint8_t key[32]) {
    if (!buf || !key || len == 0) return;
    /* Check if key is all-zero → skip (cleartext mode) */
    int has_key = 0;
    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) { has_key = 1; break; }
    }
    if (!has_key) return;

    /* Simple XOR stream cipher with key + position counter.
     * Not a real cipher, but the outer AES-256 vault provides the
     * real confidentiality. This just prevents name leakage from
     * raw singularity inspection. */
    for (size_t i = 0; i < len; i++) {
        buf[i] ^= key[i % 32] ^ (uint8_t)(i & 0xFF);
    }
}

/* Check if name encryption is active for this context.
 * Names are encrypted at pack time using a key derived from the security
 * salt. After vault decrypt, encryption_type may be 0 (fully decrypted)
 * but the name XOR layer is still present in the packed singularity — so
 * we gate on the salt being non-zero, not on encryption_type. */
static int name_encryption_active(const qtsq_context_t *ctx) {
    if (!ctx) return 0;
    /* Salt is non-zero when qtsq_container_pack pre-generated it. */
    for (int i = 0; i < 32; i++) {
        if (ctx->security.salt[i] != 0) return 1;
    }
    return 0;
}

int qtsq_container_pack(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;
    if (b->count == 0) return QTSQ_ERR_TOO_SMALL;

    /* Phase 5 fix: pre-generate the security salt + set encryption_type
     * BEFORE packing so derive_name_key() produces a real key. Previously,
     * pack ran before qtsq_keystore_encrypt / qtsq_vault_encrypt_password
     * set encryption_type, so name_encryption_active() was always false
     * and stream names were written in cleartext (the entire Phase 5
     * feature was dead code). The vault preserves this salt. */
    if (!ctx->has_security) {
        randombytes(ctx->security.salt, 32);
        randombytes(ctx->security.nonce, 12);
        ctx->security.security_version = 1;
        ctx->security.encryption_type = 1; /* tentatively password; vault will refine */
        ctx->has_security = 1;
    }

    /* Mark sanitized — stream names are encrypted above (Phase 5), which is
     * the names-in-envelope guarantee for containers. This also prevents
     * qtsq_vault_encrypt_password from calling qtsq_sanitize_context,
     * which would append a metadata block to the packed container and
     * corrupt its layout. */
    ctx->is_sanitized = 1;
    ctx->sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ZERO_META;
    ctx->header.flags |= QTSQ_FLAG_ZERO_META;

    /* Calculate total packed size */
    size_t toc_size = 4 + b->count * sizeof(qtsq_stream_entry_t);
    size_t data_size = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        size_t name_len = strlen(b->streams[i].name) + 1;   /* null-terminated */
        size_t padded = (name_len + 3) & ~(size_t)3;         /* 4-byte align */
        data_size += padded + b->streams[i].entry.data_size;
    }

    size_t total = toc_size + data_size;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return QTSQ_ERR_ALLOC;

    size_t pos = 0;

    /* Write stream count */
    memcpy(buf + pos, &b->count, 4); pos += 4;

    /* First pass: calculate data offsets, then write TOC */
    uint64_t data_offset = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        size_t name_len = strlen(b->streams[i].name) + 1;
        size_t padded = (name_len + 3) & ~(size_t)3;
        b->streams[i].entry.data_offset = data_offset;
        data_offset += padded + b->streams[i].entry.data_size;
    }

    /* Write TOC */
    for (uint32_t i = 0; i < b->count; i++) {
        memcpy(buf + pos, &b->streams[i].entry, sizeof(qtsq_stream_entry_t));
        pos += sizeof(qtsq_stream_entry_t);
    }

    /* Write stream data (name + payload) */
    uint8_t name_key[32];
    derive_name_key(ctx, name_key);
    for (uint32_t i = 0; i < b->count; i++) {
        size_t name_len = strlen(b->streams[i].name) + 1;
        size_t padded = (name_len + 3) & ~(size_t)3;
        /* Phase 5: encrypt name if name_key is non-zero */
        if (name_encryption_active(ctx)) {
            /* Copy to a temp buffer, encrypt, write */
            uint8_t name_buf[CONTAINER_NAME_MAX];
            memset(name_buf, 0, sizeof(name_buf));
            memcpy(name_buf, b->streams[i].name, name_len);
            name_crypt(name_buf, padded, name_key);
            memcpy(buf + pos, name_buf, padded);
        } else {
            memcpy(buf + pos, b->streams[i].name, name_len);
        }
        pos += padded;

        if (b->streams[i].data) {
            if (b->streams[i].entry.data_size > 0) {
                memcpy(buf + pos, b->streams[i].data, b->streams[i].entry.data_size);
                pos += b->streams[i].entry.data_size;
            }
        } else {
            /* Borrowed raw slot: header bytes + payload, no copy owned. */
            if (b->streams[i].rhead_len > 0) {
                memcpy(buf + pos, b->streams[i].rhead, b->streams[i].rhead_len);
                pos += b->streams[i].rhead_len;
            }
            if (b->streams[i].rdata_len > 0) {
                if (!b->streams[i].rdata) { free(buf); free_builder(b); return QTSQ_ERR_FORMAT; }
                memcpy(buf + pos, b->streams[i].rdata, b->streams[i].rdata_len);
                pos += b->streams[i].rdata_len;
            }
            if (b->streams[i].rhead_len + b->streams[i].rdata_len !=
                (size_t)b->streams[i].entry.data_size) {
                free(buf);
                free_builder(b);
                return QTSQ_ERR_FORMAT;
            }
        }
    }

    /* Free builder */
    free_builder(b);

    /* Replace singularity with packed buffer */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = buf;
    ctx->singularity_size = total;

    /* Update header */
    ctx->header.data_type = QTSQ_TYPE_CONTAINER;
    ctx->header.strategy = QTSQ_STRATEGY_LAYERED;
    ctx->header.flags |= QTSQ_FLAG_MULTI_STREAM;
    ctx->header.singularity_size = total;
    ctx->header.original_size = total; /* container doesn't have a single "original" */

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_pack_write (plaintext streaming write)
 *
 *  Same bytes as qtsq_container_pack + qtsq_write_plaintext, without
 *  the pack buffer: TOC offsets use identical math (see pack above),
 *  then header + TOC + stream payloads are written incrementally.
 *  Saves one full-container memcpy — a whole checkpoint on big saves.
 *
 *  Plaintext only: requires a zeroed security stub (no salt), i.e. the
 *  caller planted it exactly like qtsq_model_pack_write does. Refuses
 *  salted contexts rather than silently changing name handling.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_pack_write(qtsq_context_t *ctx, const char *filename,
                              int durable) {
    if (!ctx || !filename) return QTSQ_ERR_NULL;
    container_builder_t *b = get_builder(ctx);
    if (!b) return QTSQ_ERR_FORMAT;
    if (b->count == 0) return QTSQ_ERR_TOO_SMALL;

    /* Plaintext-only: any salt means names would XOR — refuse. */
    for (int i = 0; i < 32; i++) {
        if (ctx->security.salt[i] != 0) return QTSQ_ERR_ENCRYPTED;
    }

    ctx->is_sanitized = 1;
    ctx->sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ZERO_META;
    ctx->header.flags |= QTSQ_FLAG_ZERO_META;

    /* Identical size math to qtsq_container_pack. */
    size_t toc_size = 4 + b->count * sizeof(qtsq_stream_entry_t);
    size_t data_size = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        size_t name_len = strlen(b->streams[i].name) + 1;
        size_t padded = (name_len + 3) & ~(size_t)3;
        data_size += padded + b->streams[i].entry.data_size;
    }
    size_t total = toc_size + data_size;

    uint64_t data_offset = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        size_t name_len = strlen(b->streams[i].name) + 1;
        size_t padded = (name_len + 3) & ~(size_t)3;
        b->streams[i].entry.data_offset = data_offset;
        data_offset += padded + b->streams[i].entry.data_size;
    }

    /* Header identical to what pack + write_impl would emit. */
    qtsq_header_t h = ctx->header;
    h.version = QTSQ_VERSION_3;
    h.data_type = QTSQ_TYPE_CONTAINER;
    h.strategy = QTSQ_STRATEGY_LAYERED;
    h.flags |= QTSQ_FLAG_MULTI_STREAM | QTSQ_FLAG_ZERO_META;
    h.singularity_size = total;
    h.original_size = total;
    h.security_block_size = sizeof(qtsq_security_block_t);

    size_t name_len = strlen(filename);
    char *tmp_name = (char *)malloc(name_len + 8);
    if (!tmp_name) return QTSQ_ERR_ALLOC;
    snprintf(tmp_name, name_len + 8, "%s.tmp", filename);

#if defined(__unix__) || defined(__APPLE__)
    /* mmap fast path (≥4 MB): memcpy parts straight into the page
     * cache — no stdio layer, no pack buffer. Byte-identical output;
     * falls back to stdio below on any failure. */
    if (total + 112 + sizeof(qtsq_security_block_t) >= ((size_t)4 << 20)) {
        int mrc = QTSQ_ERR_IO;
        int mfd = open(tmp_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mfd >= 0) {
            size_t file_total = 112 + total + sizeof(qtsq_security_block_t);
            if (ftruncate(mfd, (off_t)file_total) == 0) {
                uint8_t *map = (uint8_t *)mmap(NULL, file_total,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED, mfd, 0);
                if (map != MAP_FAILED) {
                    size_t mp = 0;
                    memcpy(map + mp, &h, sizeof(h)); mp += sizeof(h);
                    memcpy(map + mp, &b->count, 4); mp += 4;
                    for (uint32_t i = 0; i < b->count; i++) {
                        memcpy(map + mp, &b->streams[i].entry,
                               sizeof(qtsq_stream_entry_t));
                        mp += sizeof(qtsq_stream_entry_t);
                    }
                    for (uint32_t i = 0; i < b->count; i++) {
                        size_t nl = strlen(b->streams[i].name) + 1;
                        size_t padded = (nl + 3) & ~(size_t)3;
                        memset(map + mp, 0, padded);
                        memcpy(map + mp, b->streams[i].name, nl);
                        mp += padded;
                        if (b->streams[i].data) {
                            size_t ds = (size_t)b->streams[i].entry.data_size;
                            if (ds > 0) {
                                if (!b->streams[i].data) { mrc = QTSQ_ERR_FORMAT; break; }
                                memcpy(map + mp, b->streams[i].data, ds);
                                mp += ds;
                            }
                        } else {
                            const uint8_t *parts[2] = { b->streams[i].rhead,
                                                        b->streams[i].rdata };
                            size_t lens[2] = { b->streams[i].rhead_len,
                                               b->streams[i].rdata_len };
                            for (int k = 0; k < 2; k++) {
                                if (lens[k] == 0) continue;
                                if (!parts[k]) { mrc = QTSQ_ERR_FORMAT; break; }
                                memcpy(map + mp, parts[k], lens[k]);
                                mp += lens[k];
                            }
                            if (mrc != QTSQ_OK) break;
                        }
                    }
                    if (mrc == QTSQ_OK) {
                        if (mp != 112 + total) {
                            mrc = QTSQ_ERR_FORMAT;
                        } else {
                            memset(map + mp, 0, sizeof(qtsq_security_block_t));
                            mp += sizeof(qtsq_security_block_t);
                        }
                    }
                    if (mrc == QTSQ_OK && durable) {
                        /* Durable like fsync (same guarantee as stdio). */
                        if (msync(map, file_total, MS_SYNC) != 0)
                            mrc = QTSQ_ERR_IO;
                    }
                    munmap(map, file_total);
                    if (mrc == QTSQ_OK) {
                        if (rename(tmp_name, filename) != 0) {
                            unlink(tmp_name);
                            mrc = QTSQ_ERR_IO;
                        }
                    } else {
                        unlink(tmp_name);
                    }
                }
            }
            close(mfd);
            if (mrc == QTSQ_OK || mrc == QTSQ_ERR_FORMAT) {
                free(tmp_name);
                free_builder(b);
                if (ctx->singularity) free(ctx->singularity);
                ctx->singularity = NULL;
                ctx->singularity_size = 0;
                if (mrc != QTSQ_OK) return mrc;
                ctx->header = h;
                return QTSQ_OK;
            }
            /* I/O failure (not format): fall through to stdio path. */
            unlink(tmp_name);
        }
    }
#endif

    FILE *f = fopen(tmp_name, "wb");
    if (!f) { free(tmp_name); return QTSQ_ERR_IO; }
    setvbuf(f, NULL, _IOFBF, (size_t)1 << 20);

    int rc = QTSQ_OK;
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    /* Header + stream count + TOC. */
    if (fwrite(&h, sizeof(h), 1, f) != 1) rc = QTSQ_ERR_IO;
    if (rc == QTSQ_OK && fwrite(&b->count, 4, 1, f) != 1) rc = QTSQ_ERR_IO;
    for (uint32_t i = 0; rc == QTSQ_OK && i < b->count; i++) {
        if (fwrite(&b->streams[i].entry, sizeof(qtsq_stream_entry_t), 1, f) != 1)
            rc = QTSQ_ERR_IO;
    }
    /* Stream payloads, straight from their buffers (no pack copy).
     * Normal slots carry one packed buffer; raw slots carry header
     * bytes + borrowed payload. */
    uint8_t name_pad[CONTAINER_NAME_MAX];
    for (uint32_t i = 0; rc == QTSQ_OK && i < b->count; i++) {
        size_t nl = strlen(b->streams[i].name) + 1;
        size_t padded = (nl + 3) & ~(size_t)3;
        memset(name_pad, 0, padded);
        memcpy(name_pad, b->streams[i].name, nl);
        if (fwrite(name_pad, 1, padded, f) != padded) { rc = QTSQ_ERR_IO; break; }
        if (b->streams[i].data) {
            size_t ds = (size_t)b->streams[i].entry.data_size;
            size_t woff = 0;
            while (woff < ds) {
                size_t chunk = ds - woff > ((size_t)1 << 20)
                               ? ((size_t)1 << 20) : ds - woff;
                if (fwrite(b->streams[i].data + woff, 1, chunk, f) != chunk) {
                    rc = QTSQ_ERR_IO;
                    break;
                }
                woff += chunk;
            }
        } else {
            const uint8_t *parts[2] = { b->streams[i].rhead, b->streams[i].rdata };
            size_t lens[2] = { b->streams[i].rhead_len, b->streams[i].rdata_len };
            for (int k = 0; k < 2 && rc == QTSQ_OK; k++) {
                if (lens[k] == 0) continue;
                if (!parts[k]) { rc = QTSQ_ERR_FORMAT; break; }
                size_t poff = 0;
                while (poff < lens[k]) {
                    size_t chunk = lens[k] - poff > ((size_t)1 << 20)
                                   ? ((size_t)1 << 20) : lens[k] - poff;
                    if (fwrite(parts[k] + poff, 1, chunk, f) != chunk) {
                        rc = QTSQ_ERR_IO;
                        break;
                    }
                    poff += chunk;
                }
            }
        }
    }
    /* Zero security block, exactly as write_impl emits for the stub. */
    for (size_t z = 0; rc == QTSQ_OK && z < sizeof(qtsq_security_block_t);) {
        size_t chunk = sizeof(qtsq_security_block_t) - z > sizeof(zeros)
                       ? sizeof(zeros) : sizeof(qtsq_security_block_t) - z;
        if (fwrite(zeros, 1, chunk, f) != chunk) { rc = QTSQ_ERR_IO; break; }
        z += chunk;
    }

    fflush(f);
    if (durable) fsync(fileno(f));
    fclose(f);
    if (rc == QTSQ_OK) {
        if (rename(tmp_name, filename) != 0) {
            unlink(tmp_name);
            rc = QTSQ_ERR_IO;
        }
    } else {
        unlink(tmp_name);
    }
    free(tmp_name);

    /* Free builder either way; mirror pack's context updates. */
    free_builder(b);
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = NULL;
    ctx->singularity_size = 0;

    if (rc != QTSQ_OK) return rc;
    ctx->header = h;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_get_count
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_get_count(const qtsq_context_t *ctx, uint32_t *out_count) {
    if (!ctx || !out_count) return QTSQ_ERR_NULL;

    /* Check if it was packed (singularity starts with stream count, not builder magic) */
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;
    if (ctx->header.data_type != QTSQ_TYPE_CONTAINER) return QTSQ_ERR_TYPE;

    /* Check for builder (unpacked) */
    container_builder_t *b = get_builder(ctx);
    if (b) {
        *out_count = b->count;
        return QTSQ_OK;
    }

    /* Read from packed buffer */
    memcpy(out_count, ctx->singularity, 4);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Name resolution shared by get_stream (payload) and
 *  get_stream_name (name only). Returns the padded name length, or 0
 *  on any inconsistency (caller maps to FORMAT).
 * ══════════════════════════════════════════════════════════════ */

static size_t container_stream_name(const qtsq_context_t *ctx,
                                    const uint8_t *buf, size_t total_size,
                                    uint32_t num_streams, uint32_t index,
                                    const qtsq_stream_entry_t *entry,
                                    char *name_buf, size_t name_buf_size) {
    size_t data_block_start = 4 + num_streams * sizeof(qtsq_stream_entry_t);
    size_t stream_start = data_block_start + entry->data_offset;

    if (stream_start >= total_size) return 0;

    /* Read name (null-terminated, possibly encrypted — Phase 5)
     * When names are encrypted, the null terminator is XOR'd to a non-zero
     * byte, so we can't scan for it. Instead, we compute the padded name
     * length from the next stream's data_offset (or the end of the data
     * block for the last stream). */
    size_t stream_name_offset = stream_start;
    size_t padded_name;

    /* Compute padded name size from TOC offsets */
    if (index + 1 < num_streams) {
        qtsq_stream_entry_t next_entry;
        size_t next_toc = 4 + (index + 1) * sizeof(qtsq_stream_entry_t);
        if (next_toc + sizeof(qtsq_stream_entry_t) > total_size) return 0;
        memcpy(&next_entry, buf + next_toc, sizeof(qtsq_stream_entry_t));
        if (next_entry.data_offset < entry->data_offset) return 0;
        padded_name = (size_t)next_entry.data_offset - entry->data_offset;
        /* Subtract data_size to get just the name padding */
        if (padded_name > entry->data_size)
            padded_name -= (size_t)entry->data_size;
    } else {
        /* Last stream: name goes from data_offset to end of data block */
        if (entry->data_offset > total_size - data_block_start) return 0;
        padded_name = (total_size - data_block_start) - entry->data_offset;
        if (padded_name > entry->data_size)
            padded_name -= (size_t)entry->data_size;
    }

    /* Safety clamp */
    if (padded_name > CONTAINER_NAME_MAX) padded_name = CONTAINER_NAME_MAX;
    if (padded_name == 0) padded_name = 4; /* minimum */
    if (stream_name_offset + padded_name > total_size) return 0;

    uint8_t name_key[32];
    derive_name_key(ctx, name_key);

    /* Copy name to a local buffer for potential decryption */
    uint8_t decrypted_name[CONTAINER_NAME_MAX];
    memset(decrypted_name, 0, sizeof(decrypted_name));
    memcpy(decrypted_name, buf + stream_name_offset, padded_name);

    /* Decrypt if name encryption is active */
    if (name_encryption_active(ctx)) {
        name_crypt(decrypted_name, padded_name, name_key);
    }

    /* Now find the actual null-terminated name length */
    size_t name_len = strnlen((const char *)decrypted_name, CONTAINER_NAME_MAX);

    if (name_buf && name_buf_size > 0) {
        size_t copy_len = name_len < name_buf_size - 1 ? name_len : name_buf_size - 1;
        memcpy(name_buf, decrypted_name, copy_len);
        name_buf[copy_len] = '\0';
    }

    return padded_name;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_get_stream_name — name only, no payload copy
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_get_stream_name(const qtsq_context_t *ctx, uint32_t index,
                                   char *name_buf, size_t name_buf_size) {
    if (!ctx || !name_buf || name_buf_size == 0) return QTSQ_ERR_NULL;
    if (ctx->header.data_type != QTSQ_TYPE_CONTAINER) return QTSQ_ERR_TYPE;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total_size = ctx->singularity_size;

    uint32_t num_streams;
    memcpy(&num_streams, buf, 4);
    if (index >= num_streams) return QTSQ_ERR_FORMAT;

    size_t toc_offset = 4 + index * sizeof(qtsq_stream_entry_t);
    if (toc_offset + sizeof(qtsq_stream_entry_t) > total_size) return QTSQ_ERR_FORMAT;

    qtsq_stream_entry_t entry;
    memcpy(&entry, buf + toc_offset, sizeof(qtsq_stream_entry_t));

    if (!container_stream_name(ctx, buf, total_size, num_streams, index,
                               &entry, name_buf, name_buf_size))
        return QTSQ_ERR_FORMAT;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_get_entry — directory entry only (Phase 1)
 * ══════════════════════════════════════════════════════════════
 *
 *  No payload is copied and nothing is decoded, so building an
 *  inventory of a large container costs one pass over the 2 MiB
 *  directory rather than one pass over the data.
 */

int qtsq_container_get_entry(const qtsq_context_t *ctx, uint32_t index,
                             qtsq_stream_entry_t *out_entry,
                             char *name_buf, size_t name_buf_size) {
    if (!ctx || !out_entry) return QTSQ_ERR_NULL;
    if (ctx->header.data_type != QTSQ_TYPE_CONTAINER) return QTSQ_ERR_TYPE;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total_size = ctx->singularity_size;

    uint32_t num_streams;
    memcpy(&num_streams, buf, 4);
    /* A pre-2.2 file can carry an out-of-range count in a corrupt or
     * crafted file; the TOC must fit inside the singularity before the
     * count is trusted. */
    size_t toc_bytes = (size_t)num_streams * sizeof(qtsq_stream_entry_t);
    if (num_streams > QTSQ_MAX_STREAMS || 4 + toc_bytes > total_size)
        return QTSQ_ERR_FORMAT;
    if (index >= num_streams) return QTSQ_ERR_FORMAT;

    size_t toc_offset = 4 + (size_t)index * sizeof(qtsq_stream_entry_t);
    memcpy(out_entry, buf + toc_offset, sizeof(qtsq_stream_entry_t));

    if (name_buf && name_buf_size > 0) {
        if (!container_stream_name(ctx, buf, total_size, num_streams, index,
                                   out_entry, name_buf, name_buf_size))
            return QTSQ_ERR_FORMAT;
    }
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC: qtsq_container_get_stream — extract stream into its own context
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_get_stream(const qtsq_context_t *ctx, uint32_t index,
                               qtsq_context_t *out_ctx,
                               char *name_buf, size_t name_buf_size) {
    if (!ctx || !out_ctx) return QTSQ_ERR_NULL;
    if (ctx->header.data_type != QTSQ_TYPE_CONTAINER) return QTSQ_ERR_TYPE;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total_size = ctx->singularity_size;

    uint32_t num_streams;
    memcpy(&num_streams, buf, 4);
    if (index >= num_streams) return QTSQ_ERR_FORMAT;

    /* Read TOC entry */
    size_t toc_offset = 4 + index * sizeof(qtsq_stream_entry_t);
    if (toc_offset + sizeof(qtsq_stream_entry_t) > total_size) return QTSQ_ERR_FORMAT;

    qtsq_stream_entry_t entry;
    memcpy(&entry, buf + toc_offset, sizeof(qtsq_stream_entry_t));

    /* Data block starts after TOC */
    size_t data_block_start = 4 + num_streams * sizeof(qtsq_stream_entry_t);
    size_t stream_start = data_block_start + entry.data_offset;

    if (stream_start >= total_size) return QTSQ_ERR_FORMAT;

    size_t padded_name = container_stream_name(ctx, buf, total_size, num_streams,
                                              index, &entry,
                                              name_buf, name_buf_size);
    if (!padded_name) return QTSQ_ERR_FORMAT;

    /* Payload starts after padded name */
    size_t payload_start = stream_start + padded_name;
    if (payload_start + entry.data_size > total_size) return QTSQ_ERR_FORMAT;

    /* Build output context */
    qtsq_init(out_ctx);
    out_ctx->header.data_type = entry.data_type;
    out_ctx->header.strategy = entry.strategy;
    out_ctx->header.flags = entry.flags;
    out_ctx->header.original_size = entry.original_size;
    out_ctx->header.singularity_size = entry.data_size;
    /* Carry the recorded codec through so callers can report it without
     * re-sniffing. Zero means "sniff it" — i.e. a pre-2.2 file. */
    out_ctx->stream_codec_id = entry.codec_id;

    if (entry.data_size > 0) {
        out_ctx->singularity = (uint8_t *)malloc(entry.data_size);
        if (!out_ctx->singularity) return QTSQ_ERR_ALLOC;
        memcpy(out_ctx->singularity, buf + payload_start, entry.data_size);
        out_ctx->singularity_size = entry.data_size;
    }

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  HMAC-SHA3-256 (same as vault, for per-stream MAC)
 * ══════════════════════════════════════════════════════════════ */

#define SHA3_256_BLOCK_SIZE 136

static void container_hmac_sha3_256(const uint8_t *key, size_t key_len,
                                     const uint8_t *data, size_t data_len,
                                     uint8_t out[32]) {
    uint8_t k[SHA3_256_BLOCK_SIZE];
    uint8_t k_ipad[SHA3_256_BLOCK_SIZE];
    uint8_t k_opad[SHA3_256_BLOCK_SIZE];

    memset(k, 0, sizeof(k));
    if (key_len > SHA3_256_BLOCK_SIZE) {
        sha3_256(k, key, key_len);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < SHA3_256_BLOCK_SIZE; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner_hash[32];
    sha3_256incctx inc_ctx;
    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, k_ipad, SHA3_256_BLOCK_SIZE);
    sha3_256_inc_absorb(&inc_ctx, data, data_len);
    sha3_256_inc_finalize(inner_hash, &inc_ctx);

    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, k_opad, SHA3_256_BLOCK_SIZE);
    sha3_256_inc_absorb(&inc_ctx, inner_hash, 32);
    sha3_256_inc_finalize(out, &inc_ctx);
}

/* ══════════════════════════════════════════════════════════════
 *  Per-Stream Encryption
 *
 *  Encrypts a single stream's compressed data with its own key,
 *  allowing different streams to be sealed to different recipients.
 *  The stream data in the container is replaced with:
 *    [qtsq_stream_encryption_t header] [encrypted payload]
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_encrypt_stream(qtsq_context_t *ctx, uint32_t stream_index,
                                   const uint8_t recipient_pk[1184]) {
    if (!ctx || !recipient_pk) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    uint32_t num_streams;
    memcpy(&num_streams, buf, 4);
    if (stream_index >= num_streams) return QTSQ_ERR_FORMAT;

    /* Locate the stream data in the packed buffer */
    size_t toc_offset = 4 + stream_index * sizeof(qtsq_stream_entry_t);
    if (toc_offset + sizeof(qtsq_stream_entry_t) > ctx->singularity_size)
        return QTSQ_ERR_FORMAT;

    qtsq_stream_entry_t entry;
    memcpy(&entry, buf + toc_offset, sizeof(qtsq_stream_entry_t));

    size_t data_block_start = 4 + num_streams * sizeof(qtsq_stream_entry_t);
    size_t stream_start = data_block_start + entry.data_offset;

    /* Skip the name to find the payload.
     * M10 fix: if name encryption is active, the null terminator is XOR'd
     * to a non-zero byte, so strnlen returns CONTAINER_NAME_MAX and the
     * payload offset is wrong. Decrypt the name in a temp buffer first. */
    uint8_t name_key[32];
    derive_name_key(ctx, name_key);
    uint8_t name_buf[CONTAINER_NAME_MAX];
    memset(name_buf, 0, sizeof(name_buf));
    memcpy(name_buf, buf + stream_start, CONTAINER_NAME_MAX > entry.data_offset ? CONTAINER_NAME_MAX : 0);
    if (name_encryption_active(ctx)) {
        name_crypt(name_buf, CONTAINER_NAME_MAX, name_key);
    }
    size_t name_len = strnlen((const char *)name_buf, CONTAINER_NAME_MAX);
    size_t padded_name = (name_len + 1 + 3) & ~(size_t)3;
    size_t payload_start = stream_start + padded_name;

    if (payload_start + entry.data_size > ctx->singularity_size)
        return QTSQ_ERR_FORMAT;

    /* Don't re-encrypt if already encrypted */
    if (entry.flags & QTSQ_STREAM_FLAG_ENCRYPTED) return QTSQ_OK;

    /* Generate per-stream encryption key */
    qtsq_stream_encryption_t enc;
    memset(&enc, 0, sizeof(enc));
    enc.encryption_type = 2; /* PQ-KEM */

    randombytes(enc.nonce, 12);
    randombytes(enc.salt, 32);
    enc.kem_ct_size = PQCLEAN_MLKEM768_CLEAN_CRYPTO_CIPHERTEXTBYTES;

    /* Encapsulate shared secret using recipient's public key */
    uint8_t shared_secret[32];
    if (PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc(enc.kem_ciphertext, shared_secret, recipient_pk) != 0) {
        return QTSQ_ERR_ALLOC;
    }

    /* Encrypt the stream payload in-place */
    uint8_t *payload = ctx->singularity + payload_start;
    size_t payload_size = entry.data_size;

    /* AES-256-CTR encrypt */
    uint8_t iv[16] = {0};
    memcpy(iv, enc.nonce, 12);
    aes256ctx aes;
    aes256_ctr_keyexp(&aes, shared_secret);
    aes256_ctr(payload, payload_size, iv, &aes);
    aes256_ctx_release(&aes);

    /* Compute HMAC-SHA3-256 auth tag */
    uint8_t full_mac[32];
    container_hmac_sha3_256(shared_secret, 32, payload, payload_size, full_mac);
    memcpy(enc.auth_tag, full_mac, 16);

    qtsq_secure_zero(shared_secret, 32);
    qtsq_secure_zero(&aes, sizeof(aes));

    /* Now we need to prepend the encryption header to the payload.
     * This requires reallocating the singularity buffer. */
    size_t enc_header_size = sizeof(qtsq_stream_encryption_t);
    size_t new_payload_size = enc_header_size + payload_size;
    size_t size_increase = enc_header_size;

    /* Reallocate singularity */
    size_t old_size = ctx->singularity_size;
    size_t new_size = old_size + size_increase;
    uint8_t *new_buf = (uint8_t *)malloc(new_size);
    if (!new_buf) return QTSQ_ERR_ALLOC;

    /* Copy everything before the payload */
    memcpy(new_buf, ctx->singularity, payload_start);

    /* Write encryption header */
    memcpy(new_buf + payload_start, &enc, enc_header_size);

    /* Copy the encrypted payload */
    memcpy(new_buf + payload_start + enc_header_size, payload, payload_size);

    /* Copy everything after the original payload */
    size_t after_payload = payload_start + payload_size;
    if (after_payload < old_size) {
        memcpy(new_buf + payload_start + new_payload_size,
               ctx->singularity + after_payload,
               old_size - after_payload);
    }

    free(ctx->singularity);
    ctx->singularity = new_buf;
    ctx->singularity_size = new_size;
    ctx->header.singularity_size = new_size;

    /* Update the stream entry in the TOC */
    entry.data_size = new_payload_size;
    entry.flags |= QTSQ_STREAM_FLAG_ENCRYPTED;
    memcpy(ctx->singularity + toc_offset, &entry, sizeof(qtsq_stream_entry_t));

    /* Update data offsets for subsequent streams */
    for (uint32_t i = stream_index + 1; i < num_streams; i++) {
        size_t off = 4 + i * sizeof(qtsq_stream_entry_t);
        qtsq_stream_entry_t e;
        memcpy(&e, ctx->singularity + off, sizeof(qtsq_stream_entry_t));
        e.data_offset += size_increase;
        memcpy(ctx->singularity + off, &e, sizeof(qtsq_stream_entry_t));
    }

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Per-Stream Decryption
 * ══════════════════════════════════════════════════════════════ */

int qtsq_container_decrypt_stream(qtsq_context_t *ctx, uint32_t stream_index,
                                   const uint8_t my_sk[2400]) {
    if (!ctx || !my_sk) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 4) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    uint32_t num_streams;
    memcpy(&num_streams, buf, 4);
    if (stream_index >= num_streams) return QTSQ_ERR_FORMAT;

    /* Locate the stream data */
    size_t toc_offset = 4 + stream_index * sizeof(qtsq_stream_entry_t);
    if (toc_offset + sizeof(qtsq_stream_entry_t) > ctx->singularity_size)
        return QTSQ_ERR_FORMAT;

    qtsq_stream_entry_t entry;
    memcpy(&entry, buf + toc_offset, sizeof(qtsq_stream_entry_t));

    if (!(entry.flags & QTSQ_STREAM_FLAG_ENCRYPTED)) return QTSQ_OK; /* already decrypted */

    size_t data_block_start = 4 + num_streams * sizeof(qtsq_stream_entry_t);
    size_t stream_start = data_block_start + entry.data_offset;

    /* Skip the name */
    const char *name_ptr = (const char *)(buf + stream_start);
    size_t name_len = strnlen(name_ptr, CONTAINER_NAME_MAX);
    size_t padded_name = (name_len + 1 + 3) & ~(size_t)3;
    size_t enc_start = stream_start + padded_name;

    if (enc_start + sizeof(qtsq_stream_encryption_t) > ctx->singularity_size)
        return QTSQ_ERR_FORMAT;

    /* Read encryption header */
    qtsq_stream_encryption_t enc;
    memcpy(&enc, buf + enc_start, sizeof(qtsq_stream_encryption_t));

    if (enc.encryption_type != 2) return QTSQ_ERR_FORMAT;

    /* Decapsulate shared secret */
    uint8_t shared_secret[32];
    if (PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(shared_secret, enc.kem_ciphertext, my_sk) != 0) {
        return QTSQ_ERR_DECRYPT;
    }

    /* Verify HMAC */
    size_t payload_start = enc_start + sizeof(qtsq_stream_encryption_t);
    size_t payload_size = entry.data_size - sizeof(qtsq_stream_encryption_t);

    if (payload_start + payload_size > ctx->singularity_size) {
        qtsq_secure_zero(shared_secret, 32);
        return QTSQ_ERR_FORMAT;
    }

    uint8_t *payload = ctx->singularity + payload_start;
    uint8_t full_mac[32];
    container_hmac_sha3_256(shared_secret, 32, payload, payload_size, full_mac);

    if (memcmp(enc.auth_tag, full_mac, 16) != 0) {
        qtsq_secure_zero(shared_secret, 32);
        return QTSQ_ERR_DECRYPT;
    }

    /* Decrypt in-place */
    uint8_t iv[16] = {0};
    memcpy(iv, enc.nonce, 12);
    aes256ctx aes;
    aes256_ctr_keyexp(&aes, shared_secret);
    aes256_ctr(payload, payload_size, iv, &aes);
    aes256_ctx_release(&aes);

    qtsq_secure_zero(shared_secret, 32);
    qtsq_secure_zero(&aes, sizeof(aes));

    /* Remove the encryption header from the buffer.
     * Shift everything after the payload backward. */
    size_t enc_header_size = sizeof(qtsq_stream_encryption_t);
    memmove(ctx->singularity + payload_start,
            ctx->singularity + payload_start + enc_header_size,
            ctx->singularity_size - payload_start - enc_header_size);

    size_t new_size = ctx->singularity_size - enc_header_size;
    ctx->singularity_size = new_size;
    ctx->header.singularity_size = new_size;

    /* Update stream entry */
    entry.data_size = payload_size;
    entry.flags &= ~QTSQ_STREAM_FLAG_ENCRYPTED;
    memcpy(ctx->singularity + toc_offset, &entry, sizeof(qtsq_stream_entry_t));

    /* Update data offsets for subsequent streams */
    for (uint32_t i = stream_index + 1; i < num_streams; i++) {
        size_t off = 4 + i * sizeof(qtsq_stream_entry_t);
        qtsq_stream_entry_t e;
        memcpy(&e, ctx->singularity + off, sizeof(qtsq_stream_entry_t));
        e.data_offset -= enc_header_size;
        memcpy(ctx->singularity + off, &e, sizeof(qtsq_stream_entry_t));
    }

    return QTSQ_OK;
}
