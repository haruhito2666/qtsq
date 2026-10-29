/*
 * sfb_ops.c — Sofuu Brain v2 / W7 operations layer.
 *
 * Implements: sfb_verify (scrub + repair), sfb_backup, sfb_restore,
 *             sfb_stats_json, sfb_migrate.
 *
 * Owned by the ops agent. This file touches NOTHING but the frozen ABI in
 * sfb.h / sfb_internal.h. It never writes .o files, never edits any other
 * module.
 *
 * ── Design constraint: the writer modules are independent ───────────────
 * sfb_store.c owns the on-disk manifest encoding and sfb_brain.c owns the
 * record serializer. Neither layout is part of this file's contract, so the
 * scrub below is deliberately *tolerant*: it uses the store's own manifest
 * reader (sfb_store_manifest_load) for structure, and identifies segment files
 * by content (digest/size) instead of by a hard-coded filename pattern.
 *
 * ASSUMPTIONS (each one is a place where this file could disagree with
 * sfb_store.c / sfb_brain.c; all are handled conservatively):
 *
 *  A1. Layout. A brain at <path> is a directory holding
 *        <path>/brain.qtsq          manifest          (SFB_MANIFEST_NAME)
 *        <path>/brain.qtsq.d/       sealed segments + graph-<gen>.bin
 *      All paths are derived through sfb_path_join() using the SFB_*_NAME
 *      macros from sfb_internal.h — no literal names are invented here.
 *
 *  A2. Manifest. sfb_manifest_t is read with sfb_store_manifest_load(), which
 *      validates magic + CRC32C (W1.4). We only ever *write* the manifest back
 *      through sfb_store_manifest_save() so its encoding stays owned by
 *      sfb_store.c. A manifest that cannot be parsed is NEVER treated as an
 *      empty brain: verify reports it and repair refuses (W1.4).
 *
 *  A3. Segment digest. sfb_seg_entry_t::digest is assumed to be CRC32C
 *      (sfb_crc32c) over the *sealed segment file bytes*. Because the exact
 *      byte range (whole file vs. payload-after-header) belongs to
 *      sfb_store.c, the scrub accepts any of a small set of plausible bases
 *      (whole file, file minus 4/8/12/16/20/24/32-byte header, first
 *      entry->length bytes, last entry->length bytes). A digest match on any
 *      basis counts as intact. This is intentionally lenient: a false "ok" is
 *      impossible for a file whose bytes changed in a way that keeps no basis
 *      matching, and a false "damaged" (the dangerous direction for repair,
 *      which quarantines) requires the store to use a digest basis outside
 *      this set.
 *
 *  A4. Segment identity. Filenames are sfb_store.c's business, so segments are
 *      matched to manifest entries by content: first by digest (authoritative),
 *      then by byte length (used only to name the damaged file that repair
 *      quarantines). Files named graph-*, *.wal, *.tmp, *.lock, *.torn and
 *      dotfiles are never considered segment candidates.
 *
 *  A5. Per-record CRC. sfb_record_t (frozen in sfb_internal.h) serialises to
 *      either 64 bytes (native alignment: crc at offset 56) or 52+4 bytes
 *      (packed: crc at offset 52), optionally preceded by a u32 record count.
 *      The scrub probes {prefix 0/4} × {stride 56/64} × {crc offset 52/56}
 *      and only trusts a framing whose records are ≥90% structurally sane
 *      (flags ⊆ {LIVE,PINNED,TOMBSTONE,DIRTY}, tier ≤ ENTITY, text/role spans
 *      inside the payload). Per-record mismatches are only counted when that
 *      framing is trusted AND fewer than half the CRCs disagree; otherwise the
 *      payload is reported as "framing not recognised" and the segment verdict
 *      falls back to the digest alone. Encrypted payloads (key != NULL) are
 *      never parsed — digests only.
 *
 *  A6. Count invariant. man.count is the *live* count, sum(e->records) is the
 *      number of records ever sealed. Live can never exceed sealed, so
 *      man.count > sum(e->records) is reported as an inconsistency. The
 *      reverse is normal (tombstones / forgets).
 *
 *  A7. Repair is bounded by the frozen ABI: a damaged segment can be
 *      quarantined and dropped from the manifest, but its records cannot be
 *      re-imported (no record importer is exposed). Every drop is reported.
 *
 *  A8. sfb_migrate has no report parameter in the frozen ABI, so the honest
 *      machine-readable refusal is written to stderr as one JSON object. Callers
 *      that want it programmatically should redirect fd 2 (tests/test_ops.c
 *      does exactly that).
 */
#define _POSIX_C_SOURCE 200809L

#include "sfb_internal.h"
#include "../../include/qtsq.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* ── small case-insensitive suffix test ─────────────────────────────────── */
static int ops_ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int ops_has_prefix(const char *s, const char *prefix)
{
    size_t lf = strlen(prefix);
    return strncmp(s, prefix, lf) == 0;
}

/* ── growable string builder for the JSON reports ──────────────────────── */
typedef struct {
    char  *p;
    size_t n, cap;
    int    oom;
} ops_sb_t;

static void ops_sb_init(ops_sb_t *s)
{
    s->cap = 1024;
    s->n = 0;
    s->oom = 0;
    s->p = (char *)malloc(s->cap);
    if (!s->p) { s->oom = 1; s->cap = 0; return; }
    s->p[0] = '\0';
}

static void ops_sb_free(ops_sb_t *s)
{
    free(s->p);
    s->p = NULL;
    s->n = s->cap = 0;
}

static int ops_sb_reserve(ops_sb_t *s, size_t extra)
{
    if (s->oom) return 0;
    size_t need = s->n + extra + 1;
    if (need <= s->cap) return 1;
    size_t cap = s->cap ? s->cap : 1024;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) { s->oom = 1; return 0; }
        cap *= 2;
    }
    char *np = (char *)realloc(s->p, cap);
    if (!np) { s->oom = 1; return 0; }
    s->p = np;
    s->cap = cap;
    return 1;
}

static void ops_sb_puts(ops_sb_t *s, const char *str)
{
    size_t l = strlen(str);
    if (!ops_sb_reserve(s, l)) return;
    memcpy(s->p + s->n, str, l);
    s->n += l;
    s->p[s->n] = '\0';
}

static void ops_sb_printf(ops_sb_t *s, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) { s->oom = 1; return; }
    if ((size_t)k < sizeof tmp) { ops_sb_puts(s, tmp); return; }
    /* long formatted chunk: allocate exactly */
    size_t need = (size_t)k + 1;
    char *big = (char *)malloc(need);
    if (!big) { s->oom = 1; return; }
    va_start(ap, fmt);
    vsnprintf(big, need, fmt, ap);
    va_end(ap);
    ops_sb_puts(s, big);
    free(big);
}

static void ops_sb_u64(ops_sb_t *s, uint64_t v)
{
    ops_sb_printf(s, "%llu", (unsigned long long)v);
}

/* JSON string literal. Escapes the characters that can appear in the values we
 * emit (paths, error identifiers). Implemented locally so the report is valid
 * JSON regardless of sfb_util.c's sfb_json_escape() conventions. */
static void ops_json_str(ops_sb_t *s, const char *in)
{
    ops_sb_puts(s, "\"");
    if (in) {
        for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
            switch (*p) {
            case '"':  ops_sb_puts(s, "\\\""); break;
            case '\\': ops_sb_puts(s, "\\\\"); break;
            case '\n': ops_sb_puts(s, "\\n");  break;
            case '\r': ops_sb_puts(s, "\\r");  break;
            case '\t': ops_sb_puts(s, "\\t");  break;
            default:
                if (*p < 0x20) {
                    ops_sb_printf(s, "\\u%04x", (unsigned)*p);
                } else {
                    char one[2] = { (char)*p, '\0' };
                    ops_sb_puts(s, one);
                }
            }
        }
    }
    ops_sb_puts(s, "\"");
}

/* Emit the report; always NUL-terminates. Truncation (cap too small) is
 * documented: the caller gets the longest JSON prefix that fits. */
static void ops_json_flush(ops_sb_t *s, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!s->p || s->oom) { out[0] = '\0'; return; }
    size_t n = s->n;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, s->p, n);
    out[n] = '\0';
}

/* ── path helpers ──────────────────────────────────────────────────────── */
static int ops_fill_paths(sfb_store_t *s, const char *path)
{
    memset(s, 0, sizeof *s);
    if (strlen(path) >= sizeof s->dir) return SFB_E_INVALID;
    snprintf(s->dir, sizeof s->dir, "%s", path);
    if (sfb_path_join(s->manifest_path, sizeof s->manifest_path, path, SFB_MANIFEST_NAME))
        return SFB_E_INVALID;
    if (sfb_path_join(s->seg_dir, sizeof s->seg_dir, path, SFB_SEG_DIR))
        return SFB_E_INVALID;
    if (sfb_path_join(s->wal_path, sizeof s->wal_path, path, SFB_WAL_NAME))
        return SFB_E_INVALID;
    if (sfb_path_join(s->lock_path, sizeof s->lock_path, path, SFB_LOCK_NAME))
        return SFB_E_INVALID;
    return SFB_OK;
}

static int ops_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int ops_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ── whole-file read ───────────────────────────────────────────────────── */
static int ops_read_file(const char *path, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT) ? SFB_E_NOTFOUND : SFB_E_IO;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return SFB_E_IO; }
    if (!S_ISREG(st.st_mode)) { close(fd); return SFB_E_INVALID; }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX - 1) {
        close(fd);
        return SFB_E_IO;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) { close(fd); return SFB_E_NOMEM; }
    if (len && sfb_pread_all(fd, buf, len, 0) != SFB_OK) {
        free(buf);
        close(fd);
        return SFB_E_IO;
    }
    close(fd);
    *out = buf;
    *out_len = len;
    return SFB_OK;
}

/* ── copy / link helpers (backup, restore, migrate) ────────────────────── */
static int ops_copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return (errno == ENOENT) ? SFB_E_NOTFOUND : SFB_E_IO;
    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return SFB_E_IO; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        if (errno == EEXIST) return SFB_E_STATE;
        return SFB_E_IO;
    }
    uint8_t *buf = (uint8_t *)malloc(1u << 16);
    if (!buf) { close(in); close(out); return SFB_E_NOMEM; }
    int rc = SFB_OK;
    for (;;) {
        ssize_t got = read(in, buf, 1u << 16);
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = SFB_E_IO;
            break;
        }
        if (sfb_write_all(out, buf, (size_t)got) != SFB_OK) { rc = SFB_E_ENOSPC; break; }
    }
    free(buf);
    if (rc == SFB_OK && fsync(out) != 0) rc = SFB_E_IO;
    if (close(in) != 0 && rc == SFB_OK) rc = SFB_E_IO;
    if (close(out) != 0 && rc == SFB_OK) rc = SFB_E_IO;
    if (rc != SFB_OK) unlink(dst);   /* never leave a half copy behind */
    return rc;
}

/* Hardlink, then fall back to a byte copy (W7.3). */
static int ops_link_or_copy(const char *src, const char *dst, int *used_link)
{
    if (used_link) *used_link = 0;
    if (link(src, dst) == 0) {
        if (used_link) *used_link = 1;
        return SFB_OK;
    }
    return ops_copy_file(src, dst);
}

/* ── digest bases accepted for a segment file (assumption A3) ──────────── */
#define OPS_NBASES 9
static void ops_digest_bases(const uint8_t *buf, size_t len, uint64_t entry_len,
                             uint32_t crcs[OPS_NBASES])
{
    static const size_t skip[7] = { 0, 4, 8, 12, 16, 20, 32 };
    for (int i = 0; i < 7; i++) {
        size_t off = skip[i];
        crcs[i] = (off <= len) ? sfb_crc32c(buf + off, len - off) : 0u;
    }
    /* "length counts the payload, at the front or at the back" */
    if (entry_len > 0 && entry_len <= len) {
        crcs[7] = sfb_crc32c(buf, (size_t)entry_len);
        crcs[8] = sfb_crc32c(buf + (len - (size_t)entry_len), (size_t)entry_len);
    } else {
        crcs[7] = crcs[8] = 0u;
    }
}

/* Real segment header (sfb_store.c, 48 bytes little-endian):
 *   0 magic | 4 version | 6 flags | 8 dim | 12 records | 16 min_id | 20 max_id
 *  24 payload_len u64 | 32 payload_crc u32 | 36 iv[12] | 48 payload
 * Checking it costs one CRC32C over the payload and catches header damage and
 * a byte flip *inside* the payload without needing the key (payload_crc covers
 * the bytes as stored, i.e. ciphertext when the store is encrypted). */
#define OPS_SEG_HDR_LEN 48u

/* little-endian readers (x86_64 / arm64 hosts, as the store format assumes) */
static uint16_t ops_rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t ops_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t ops_rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Returns 1 when the header/payload checks prove damage, 0 when they pass or
 * when the file does not carry a recognisable v2 segment header. */
static int ops_seg_header_broken(const uint8_t *buf, size_t len,
                                 const sfb_seg_entry_t *e, uint32_t man_dim,
                                 char *why, size_t why_cap)
{
    why[0] = '\0';
    if (len < OPS_SEG_HDR_LEN) return 0;
    if (ops_rd32(buf) != SFB_MAGIC_SEGMENT) return 0;
    if (ops_rd16(buf + 4) != (uint16_t)SFB_FORMAT_VERSION) return 0;

    uint64_t payload_len = ops_rd64(buf + 24);
    uint32_t stored_crc  = ops_rd32(buf + 32);
    uint32_t records     = ops_rd32(buf + 12);
    uint32_t dim         = ops_rd32(buf + 8);

    if (payload_len > (uint64_t)SIZE_MAX - OPS_SEG_HDR_LEN ||
        (size_t)OPS_SEG_HDR_LEN + (size_t)payload_len != len) {
        snprintf(why, why_cap, "header_length_mismatch: payload_len %llu + %u != %llu",
                 (unsigned long long)payload_len, OPS_SEG_HDR_LEN,
                 (unsigned long long)len);
        return 1;
    }
    uint32_t calc = sfb_crc32c(buf + OPS_SEG_HDR_LEN, (size_t)payload_len);
    if (calc != stored_crc) {
        snprintf(why, why_cap, "payload_crc_mismatch: stored %u, computed %u",
                 stored_crc, calc);
        return 1;
    }
    if (records != e->records) {
        snprintf(why, why_cap, "header_record_count_mismatch: header %u, manifest %u",
                 records, e->records);
        return 1;
    }
    if (man_dim != 0 && dim != man_dim) {
        snprintf(why, why_cap, "header_dim_mismatch: header %u, manifest %u",
                 dim, man_dim);
        return 1;
    }
    return 0;
}

/* ── per-record CRC scan (assumption A5) ───────────────────────────────── */
typedef struct {
    int      recognised;      /* framing trusted                              */
    uint64_t records;         /* records in the walked framing                */
    uint64_t crc_bad;         /* per-record CRC32C mismatches                 */
    int      prefix, stride, crc_off;   /* the framing that was trusted       */
} ops_recscan_t;

/* structural plausibility of one record at p (stride bytes available) */
static int ops_rec_sane(const uint8_t *p, int crc_off, size_t blob_lo, size_t blob_hi)
{
    uint32_t flags = ops_rd32(p + 4);
    uint32_t tier  = ops_rd32(p + 8);
    uint32_t to    = ops_rd32(p + 12), tl = ops_rd32(p + 16);
    uint32_t ro    = ops_rd32(p + 20), rl = ops_rd32(p + 24);
    (void)crc_off;
    if (flags & ~(uint32_t)(SFB_F_LIVE | SFB_F_PINNED | SFB_F_TOMBSTONE | SFB_F_DIRTY))
        return 0;
    if (tier > SFB_TIER_ENTITY) return 0;
    if (tl > blob_hi) return 0;
    if (rl > blob_hi) return 0;
    if (to > blob_hi || tl > blob_hi - to) return 0;
    if (ro > blob_hi || rl > blob_hi - ro) return 0;
    if ((uint64_t)to + tl < blob_lo) return 0;
    if ((uint64_t)ro + rl < blob_lo) return 0;
    return 1;
}

/* Variable-length record block, the layout sfb_brain.c actually writes:
 *   u32 count,
 *   per record: u32 id, flags, tier, text_len, role_len, f32 strength (24 B),
 *               f64 created_at, f64 last_hit                     (16 B),
 *               u32 crc32c(head24 ++ text ++ role)              (4 B),
 *               text[text_len] role[role_len] f32 vec[dim]
 * Returns 1 and fills *out when the framing is accepted. A wrong guess is
 * rejected by the "records must tile the payload exactly" rule. */
static int ops_scan_records_v2(const uint8_t *p, size_t len, uint32_t expect,
                               uint32_t dim, ops_recscan_t *out)
{
    memset(out, 0, sizeof *out);
    if (p == NULL || len < 4 || dim == 0) return 0;
    uint32_t n = ops_rd32(p);
    if (n == 0 || n > 65536u) return 0;
    if (expect && n != expect) return 0;

    const size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t off = 4;
    uint64_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (off + 44 > len) return 0;
        const uint8_t *r = p + off;
        uint32_t tl = ops_rd32(r + 12);
        uint32_t rl = ops_rd32(r + 16);
        if (tl > (1u << 20) || rl > (1u << 20)) return 0;
        size_t stride = 44 + (size_t)tl + (size_t)rl + vec_bytes;
        if (off + stride > len) return 0;
        uint32_t stored = ops_rd32(r + 40);
        uint32_t crc = sfb_crc32c(r, 24);
        crc = sfb_crc32c_update(crc, r + 44, tl);
        crc = sfb_crc32c_update(crc, r + 44 + tl, rl);
        if (crc != stored) bad++;
        off += stride;
    }
    if (off != len) return 0;             /* must tile the payload exactly */
    if (bad * 2u > n) return 0;           /* the guess itself is wrong     */

    out->recognised = 1;
    out->records = n;
    out->crc_bad = bad;
    out->prefix = 4;
    out->stride = -1;                     /* variable length               */
    out->crc_off = 40;
    return 1;
}

static void ops_scan_records(const uint8_t *buf, size_t len, uint32_t expect_records,
                             ops_recscan_t *out)
{
    memset(out, 0, sizeof *out);
    if (len < 16) return;

    static const int prefixes[2] = { 4, 0 };
    static const int strides[2]  = { 64, 56 };
    static const int crcoffs[2]  = { 56, 52 };

    uint64_t best_sane = 0;
    const uint8_t *best_base = NULL;
    uint32_t best_count = 0;
    int best_prefix = 0, best_stride = 0, best_crc = 0;

    for (int pi = 0; pi < 2; pi++) {
        int prefix = prefixes[pi];
        if (len < (size_t)prefix + 8) continue;
        uint32_t count = prefix ? ops_rd32(buf) : 0;
        if (prefix == 0) count = expect_records;
        if (count == 0 || count > SFB_SEGMENT_CAP * 8u) continue;
        for (int si = 0; si < 2; si++) {
            int stride = strides[si];
            size_t need = (size_t)prefix + (size_t)count * (size_t)stride;
            if (need > len) continue;
            for (int ci = 0; ci < 2; ci++) {
                int crc_off = crcoffs[ci];
                if (crc_off >= stride) continue;
                size_t blob_lo = need;
                size_t blob_hi = len - prefix;
                if (blob_hi < blob_lo) continue;
                uint64_t sane = 0;
                for (uint32_t i = 0; i < count; i++) {
                    const uint8_t *p = buf + prefix + (size_t)i * (size_t)stride;
                    if (ops_rec_sane(p, crc_off, 0, blob_hi)) sane++;
                }
                if (sane > best_sane) {
                    best_sane = sane;
                    best_base = buf + prefix;
                    best_count = count;
                    best_prefix = prefix;
                    best_stride = stride;
                    best_crc = crc_off;
                }
            }
        }
    }

    if (!best_base || best_count == 0) return;
    /* ≥90 % of the records must be structurally plausible to trust a framing */
    if (best_sane * 10u < (uint64_t)best_count * 9u) return;

    uint64_t bad = 0;
    uint8_t tmp[64];
    for (uint32_t i = 0; i < best_count; i++) {
        const uint8_t *p = best_base + (size_t)i * (size_t)best_stride;
        uint32_t stored = ops_rd32(p + best_crc);
        if (stored == sfb_crc32c(p, (size_t)best_crc)) continue;
        /* second plausible convention: CRC over the whole record with the crc
         * slot zero-filled (a serializer that memcpy()s the struct as-is) */
        if (best_stride <= (int)sizeof tmp) {
            memcpy(tmp, p, (size_t)best_stride);
            memset(tmp + best_crc, 0, 4);
            if (stored == sfb_crc32c(tmp, (size_t)best_stride)) continue;
        }
        bad++;
    }
    /* more than half disagreeing means the framing guess itself is wrong */
    if (bad * 2u > best_count) return;

    out->recognised = 1;
    out->records = best_count;
    out->crc_bad = bad;
    out->prefix = best_prefix;
    out->stride = best_stride;
    out->crc_off = best_crc;
}

/* ── the verify report ─────────────────────────────────────────────────── */
#define OPS_MSG_CAP   320
#define OPS_LIST_CAP   32

typedef struct {
    char   msg[OPS_LIST_CAP][OPS_MSG_CAP];
    int    n;
    int    dropped_names;      /* how many entries did not fit */
} ops_list_t;

/* Fixed-size so a hostile store cannot make verify allocate without bound.
 * Overflow is never silent: the last slot becomes a "truncated" marker. */
static void ops_list_trunc_marker(ops_list_t *l)
{
    snprintf(l->msg[OPS_LIST_CAP - 1], OPS_MSG_CAP,
             "truncated: %d more entr%s not shown", l->dropped_names,
             l->dropped_names == 1 ? "y" : "ies");
}

static void ops_list_add(ops_list_t *l, const char *fmt, ...)
{
    if (l->dropped_names) {
        l->dropped_names++;
        ops_list_trunc_marker(l);
        return;
    }
    if (l->n >= OPS_LIST_CAP - 1) {
        l->dropped_names = 1;
        ops_list_trunc_marker(l);
        l->n = OPS_LIST_CAP;      /* marks the table as full */
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->msg[l->n], OPS_MSG_CAP, fmt, ap);
    va_end(ap);
    l->n++;
}

typedef struct {
    int        ok;
    int        manifest_ok;
    uint64_t   records;               /* sum of sealed records                */
    uint64_t   segments;              /* manifest nseg                        */
    uint64_t   files_seen;            /* candidate segment files found        */
    uint64_t   segments_damaged;      /* digest mismatch                      */
    uint64_t   segments_missing;      /* no file at all                       */
    uint64_t   records_dropped_est;
    uint64_t   rec_crc_fail;          /* per-record CRC mismatches counted    */
    uint64_t   rec_scanned;           /* records whose CRC was actually walked */
    uint64_t   rec_unframed;          /* segments whose framing was not trusted */
    uint64_t   quarantined;
    uint64_t   manifest_rewritten;
    int        hard_errors;           /* real problems (not advisory notes)   */
    int        graph_bad;             /* graph file missing (repair cannot fix) */
    ops_list_t missing;
    ops_list_t errors;                /* hard errors + notes, in that order   */
} ops_verdict_t;

/* A finding that makes the brain "not ok" in a plain scrub, but that repair is
 * allowed to resolve (it quarantines and rewrites). */
static void ops_damage(ops_verdict_t *v, int repair, const char *fmt, ...)
{
    char tmp[OPS_MSG_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, OPS_MSG_CAP, fmt, ap);
    va_end(ap);
    ops_list_add(&v->errors, "%s", tmp);
    if (!repair) v->hard_errors++;
}

/* Purely informational: never changes the verdict, always shown. */
static void ops_note(ops_verdict_t *v, const char *fmt, ...)
{
    char tmp[OPS_MSG_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, OPS_MSG_CAP, fmt, ap);
    va_end(ap);
    ops_list_add(&v->errors, "%s", tmp);
}

static void ops_hard(ops_verdict_t *v, const char *fmt, ...)
{
    char tmp[OPS_MSG_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, OPS_MSG_CAP, fmt, ap);
    va_end(ap);
    ops_list_add(&v->errors, "%s", tmp);
    v->hard_errors++;
}

/* a candidate file in the segment directory */
typedef struct {
    char     name[256];
    uint64_t size;
    uint32_t crcs[OPS_NBASES];
    int      claimed;
} ops_segfile_t;

typedef struct {
    ops_segfile_t *v;
    size_t         n, cap;
} ops_files_t;

static int ops_files_push(ops_files_t *f, const char *name, uint64_t size)
{
    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 16;
        ops_segfile_t *nv = (ops_segfile_t *)realloc(f->v, cap * sizeof *nv);
        if (!nv) return SFB_E_NOMEM;
        f->v = nv;
        f->cap = cap;
    }
    memset(&f->v[f->n], 0, sizeof f->v[f->n]);
    snprintf(f->v[f->n].name, sizeof f->v[f->n].name, "%s", name);
    f->v[f->n].size = size;
    f->n++;
    return SFB_OK;
}

static void ops_files_free(ops_files_t *f)
{
    free(f->v);
    f->v = NULL;
    f->n = f->cap = 0;
}

/* Count quarantined evidence in the segment dir: *.torn (this file's repair)
 * and *.quarantine (sfb_store_quarantine's name). These are bytes the store
 * deliberately did not delete, so verify reports them — but they never make a
 * store unhealthy on their own. */
static void ops_count_evidence(const char *dir, uint64_t *torn, uint64_t *quar)
{
    if (torn) *torn = 0;
    if (quar) *quar = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (torn && ops_ends_with_ci(de->d_name, ".torn")) (*torn)++;
        else if (quar && ops_ends_with_ci(de->d_name, ".quarantine")) (*quar)++;
    }
    closedir(d);
}

/* Is this directory entry a segment candidate? (assumption A4) */
static int ops_is_segment_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.') return 0;
    if (ops_has_prefix(name, "graph-")) return 0;
    if (ops_ends_with_ci(name, ".torn")) return 0;
    if (ops_ends_with_ci(name, ".wal")) return 0;
    if (ops_ends_with_ci(name, ".tmp")) return 0;
    if (ops_ends_with_ci(name, ".lock")) return 0;
    if (ops_ends_with_ci(name, ".partial")) return 0;
    if (ops_ends_with_ci(name, ".quarantine")) return 0;  /* sfb_store's own */
    return 1;
}

/* kind 0 = sealed segments, kind 1 = graph snapshots. Both live in seg_dir;
 * the graph must be part of a backup or the copy is not a usable store. */
static int ops_scan_dir_kind(ops_files_t *out, const char *dir, int kind,
                             uint64_t *graph_files)
{
    if (graph_files) *graph_files = 0;
    DIR *d = opendir(dir);
    if (!d) return SFB_E_NOTFOUND;
    struct dirent *de;
    int rc = SFB_OK;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full[4096];
        if (sfb_path_join(full, sizeof full, dir, de->d_name) != 0) continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int is_graph = ops_has_prefix(de->d_name, "graph-") &&
                       !ops_ends_with_ci(de->d_name, ".tmp");
        if (kind == 1) {
            if (!is_graph) continue;
            if ((rc = ops_files_push(out, de->d_name, (uint64_t)st.st_size)) != SFB_OK) break;
            continue;
        }
        if (is_graph) {
            if (graph_files) (*graph_files)++;
            continue;
        }
        if (!ops_is_segment_name(de->d_name)) continue;
        if ((rc = ops_files_push(out, de->d_name, (uint64_t)st.st_size)) != SFB_OK) break;
    }
    closedir(d);
    return rc;
}

static int ops_scan_dir(ops_files_t *out, const char *dir, uint64_t *graph_files)
{
    return ops_scan_dir_kind(out, dir, 0, graph_files);
}

static int ops_scan_graphs(ops_files_t *out, const char *dir)
{
    return ops_scan_dir_kind(out, dir, 1, NULL);
}

/* Graph file existence (assumption A4: name is graph-<gen>.bin, but we accept
 * any "graph-" file as evidence when the exact name cannot be predicted). */
static int ops_graph_present(const char *seg_dir, uint64_t graph_gen,
                             char *why, size_t why_cap)
{
    static const char *fmts[] = {
        "graph-%llu.bin", "graph-%08llu.bin", "graph-%016llx.bin",
        "graph-%llu.qtsq", "graph-%llu"
    };
    char path[4096];
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        char name[128];
        snprintf(name, sizeof name, fmts[i], (unsigned long long)graph_gen);
        if (sfb_path_join(path, sizeof path, seg_dir, name) != 0) continue;
        if (ops_is_file(path)) return 1;
    }
    /* fall back: any graph-* file at all */
    uint64_t n = 0;
    ops_files_t scratch;
    memset(&scratch, 0, sizeof scratch);
    (void)ops_scan_dir(&scratch, seg_dir, &n);
    ops_files_free(&scratch);
    if (n > 0) return 1;
    snprintf(why, why_cap, "graph-<gen %llu>.bin", (unsigned long long)graph_gen);
    return 0;
}

/* Quarantine one segment file: rename to <name>.torn, never delete.
 * If that name is taken, suffix .torn.1, .torn.2 ... so evidence is never
 * clobbered. */
static int ops_quarantine_file(const char *seg_dir, const char *name,
                               char *new_name, size_t cap)
{
    char src[4096], dst[4096];
    if (sfb_path_join(src, sizeof src, seg_dir, name) != 0) return SFB_E_INVALID;
    for (int i = 0; i < 1000; i++) {
        char cand[300];
        if (i == 0) snprintf(cand, sizeof cand, "%s.torn", name);
        else        snprintf(cand, sizeof cand, "%s.torn.%d", name, i);
        if (sfb_path_join(dst, sizeof dst, seg_dir, cand) != 0) return SFB_E_INVALID;
        if (ops_is_file(dst)) continue;          /* keep existing evidence */
        if (rename(src, dst) == 0) {
            snprintf(new_name, cap, "%s", cand);
            return SFB_OK;
        }
        if (errno == ENOENT) return SFB_E_NOTFOUND;
        return SFB_E_IO;
    }
    return SFB_E_IO;
}

/* Remove entry `idx` from the manifest table. */
static void ops_manifest_drop(sfb_manifest_t *m, uint32_t idx)
{
    if (idx + 1 < m->nseg && m->segs) {
        memmove(&m->segs[idx], &m->segs[idx + 1],
                (size_t)(m->nseg - idx - 1) * sizeof m->segs[0]);
    }
    if (m->nseg > 0) m->nseg--;
}

/* ── the scrub itself ──────────────────────────────────────────────────── */
static int ops_verify_run(const char *path, const char *key, int repair,
                          ops_verdict_t *v)
{
    memset(v, 0, sizeof *v);

    if (!path || !path[0]) {
        ops_list_add(&v->errors, "invalid_argument: path is required");
        return SFB_E_INVALID;
    }
    if (!ops_is_dir(path)) {
        ops_list_add(&v->errors, "not_a_store: %s is not a directory", path);
        return SFB_E_NOTFOUND;
    }

    sfb_store_t st;
    int rc = ops_fill_paths(&st, path);
    if (rc != SFB_OK) {
        ops_list_add(&v->errors, "path_too_long");
        return rc;
    }
    st.readonly = repair ? 0 : 1;

    if (!ops_is_file(st.manifest_path)) {
        ops_list_add(&v->missing, "%s", SFB_MANIFEST_NAME);
        ops_list_add(&v->errors, "manifest_missing: %s", st.manifest_path);
        return SFB_E_CORRUPT;
    }

    /* Manifest: loaded through sfb_store.c so its encoding stays its own. */
    rc = sfb_store_manifest_load(&st);
    if (rc != SFB_OK) {
        v->manifest_ok = 0;
        ops_list_add(&v->errors, "manifest_unreadable (crc or magic): SFB error %d", rc);
        return (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc;
    }
    if (st.man.magic != SFB_MAGIC_MANIFEST) {
        v->manifest_ok = 0;
        ops_list_add(&v->errors, "manifest_magic_mismatch: 0x%08x", st.man.magic);
        return SFB_E_CORRUPT;
    }
    v->manifest_ok = 1;
    v->segments = st.man.nseg;
    if (st.man.nseg > SFB_MAX_SEGS)
        ops_list_add(&v->errors, "manifest_nseg_out_of_range: %u", st.man.nseg);

    uint64_t sealed = 0;
    for (uint32_t i = 0; i < st.man.nseg; i++) {
        const sfb_seg_entry_t *e = &st.man.segs[i];
        sealed += e->records;
        if (e->records == 0)
            ops_note(v, "segment_without_records: gen %u seq %u (advisory)",
                     e->gen, e->seq);
        if (e->records > SFB_SEGMENT_CAP)
            ops_note(v, "segment_over_cap: gen %u seq %u records %u > %u (advisory)",
                     e->gen, e->seq, e->records, SFB_SEGMENT_CAP);
    }
    v->records = sealed;

    /* count invariant (assumption A6) */
    if (st.man.count > sealed)
        ops_list_add(&v->errors,
                     "count_exceeds_segment_records: manifest count %llu > sealed %llu",
                     (unsigned long long)st.man.count, (unsigned long long)sealed);

    /* ── enumerate candidates and match them to manifest entries ────────── */
    ops_files_t files;
    memset(&files, 0, sizeof files);
    uint64_t graph_files = 0;
    rc = ops_scan_dir(&files, st.seg_dir, &graph_files);
    if (rc == SFB_E_NOTFOUND) {
        if (st.man.nseg > 0) {
            ops_list_add(&v->missing, "%s/", SFB_SEG_DIR);
            ops_list_add(&v->errors, "segment_directory_missing: %s", st.seg_dir);
        }
    } else if (rc != SFB_OK) {
        ops_list_add(&v->errors, "segment_directory_unreadable: SFB error %d", rc);
    }
    v->files_seen = files.n;

    /* digest every candidate file once */
    for (size_t i = 0; i < files.n; i++) {
        char full[4096];
        if (sfb_path_join(full, sizeof full, st.seg_dir, files.v[i].name) != 0) continue;
        uint8_t *buf = NULL;
        size_t len = 0;
        int r = ops_read_file(full, &buf, &len);
        if (r != SFB_OK) {
            memset(files.v[i].crcs, 0, sizeof files.v[i].crcs);
            continue;
        }
        files.v[i].size = len;
        ops_digest_bases(buf, len, 0, files.v[i].crcs);
        free(buf);
    }

    int *entry_file = (int *)malloc((st.man.nseg ? st.man.nseg : 1) * sizeof(int));
    int *entry_ok   = (int *)malloc((st.man.nseg ? st.man.nseg : 1) * sizeof(int));
    if (!entry_file || !entry_ok) {
        free(entry_file); free(entry_ok);
        ops_files_free(&files);
        ops_list_add(&v->errors, "out_of_memory");
        return SFB_E_NOMEM;
    }
    for (uint32_t i = 0; i < st.man.nseg; i++) { entry_file[i] = -1; entry_ok[i] = 0; }

    /* Pass 1 — digest match is authoritative (assumption A4). */
    for (size_t fi = 0; fi < files.n; fi++) {
        if (files.v[fi].claimed) continue;
        for (uint32_t ei = 0; ei < st.man.nseg; ei++) {
            if (entry_file[ei] >= 0) continue;
            const sfb_seg_entry_t *e = &st.man.segs[ei];
            int hit = 0;
            for (int k = 0; k < OPS_NBASES; k++)
                if (files.v[fi].crcs[k] == e->digest) { hit = 1; break; }
            if (hit) {
                entry_file[ei] = (int)fi;
                entry_ok[ei] = 1;
                files.v[fi].claimed = 1;
                break;
            }
        }
    }
    /* Pass 2 — same byte length, wrong digest ⇒ that file is the damaged one. */
    for (size_t fi = 0; fi < files.n; fi++) {
        if (files.v[fi].claimed) continue;
        for (uint32_t ei = 0; ei < st.man.nseg; ei++) {
            if (entry_file[ei] >= 0) continue;
            if (files.v[fi].size == st.man.segs[ei].length) {
                entry_file[ei] = (int)fi;
                entry_ok[ei] = 0;
                files.v[fi].claimed = 1;
                break;
            }
        }
    }

    /* ── walk each matched segment, verifying the file and its records ──── */
    for (uint32_t ei = 0; ei < st.man.nseg; ei++) {
        const sfb_seg_entry_t *e = &st.man.segs[ei];
        if (entry_file[ei] < 0) {
            v->segments_missing++;
            v->records_dropped_est += e->records;
            ops_list_add(&v->missing, "segment gen=%u seq=%u (%u records)",
                         e->gen, e->seq, e->records);
            ops_damage(v, repair, "segment_file_missing: gen %u seq %u (%u records)",
                       e->gen, e->seq, e->records);
            continue;
        }
        ops_segfile_t *f = &files.v[entry_file[ei]];
        char full[4096];
        if (sfb_path_join(full, sizeof full, st.seg_dir, f->name) != 0) continue;

        uint8_t *buf = NULL;
        size_t len = 0;
        int r = ops_read_file(full, &buf, &len);
        if (r != SFB_OK) {
            v->segments_damaged++;
            v->records_dropped_est += e->records;
            ops_damage(v, repair, "segment_unreadable: %s (SFB error %d)", f->name, r);
            continue;
        }
        uint32_t crcs[OPS_NBASES];
        ops_digest_bases(buf, len, e->length, crcs);
        int digest_hit = 0;
        for (int k = 0; k < OPS_NBASES; k++)
            if (crcs[k] == e->digest) { digest_hit = 1; break; }

        if (!digest_hit) {
            v->segments_damaged++;
            entry_ok[ei] = 0;
            v->records_dropped_est += e->records;
            ops_damage(v, repair,
                       "segment_digest_mismatch: %s (manifest %u, file %u, %llu bytes)",
                       f->name, e->digest, crcs[0], (unsigned long long)len);
        }

        /* Finer-grained check: the v2 segment header carries its own payload
         * CRC, so a flip inside a segment is pinned to that segment even when
         * the manifest digest basis is not the whole file. */
        {
            char why[192] = {0};
            if (ops_seg_header_broken(buf, len, e, st.man.dim, why, sizeof why)) {
                if (entry_ok[ei]) {          /* do not double count a segment */
                    v->segments_damaged++;
                    v->records_dropped_est += e->records;
                }
                entry_ok[ei] = 0;
                ops_damage(v, repair, "segment_header_corrupt: %s (%s)", f->name, why);
            }
        }

        /* Per-record CRC — advisory only (assumption A5). A file whose digest
         * matches cannot have per-record corruption; a per-record mismatch on
         * such a file means our framing guess is wrong, so we count it but
         * never fail the store on it. */
        if (key == NULL && (st.man.flags & 0x0001u) == 0) {
            /* walk the payload, i.e. the bytes *after* the 48-byte segment
             * header; feeding the raw file would misalign every record */
            const uint8_t *recbuf = buf;
            size_t reclen = len;
            if (len >= OPS_SEG_HDR_LEN && ops_rd32(buf) == SFB_MAGIC_SEGMENT &&
                ops_rd16(buf + 4) == (uint16_t)SFB_FORMAT_VERSION) {
                uint64_t pl = ops_rd64(buf + 24);
                if (pl <= (uint64_t)SIZE_MAX - OPS_SEG_HDR_LEN &&
                    (size_t)OPS_SEG_HDR_LEN + (size_t)pl <= len) {
                    recbuf = buf + OPS_SEG_HDR_LEN;
                    reclen = (size_t)pl;
                }
            }
            ops_recscan_t rs;
            if (!ops_scan_records_v2(recbuf, reclen, e->records, st.man.dim, &rs))
                ops_scan_records(recbuf, reclen, e->records, &rs);
            if (rs.recognised) {
                v->rec_scanned += rs.records;
                v->rec_crc_fail += rs.crc_bad;
                if (rs.crc_bad) {
                    ops_note(v, "record_crc_mismatch: %s %llu of %llu records "
                                "(framing %s, crc@%d)",
                             f->name, (unsigned long long)rs.crc_bad,
                             (unsigned long long)rs.records,
                             rs.stride < 0 ? "variable-length v2" : "fixed-stride",
                             rs.crc_off);
                }
            } else {
                v->rec_unframed++;
                ops_note(v, "record_framing_not_recognised: %s "
                            "(segment digest is the authoritative check)",
                         f->name);
            }
        }
        free(buf);
    }

    /* graph file referenced by the manifest must exist */
    if (st.man.graph_gen != 0) {
        char why[128] = {0};
        if (!ops_graph_present(st.seg_dir, st.man.graph_gen, why, sizeof why)) {
            v->graph_bad = 1;
            ops_list_add(&v->missing, "%s", why);
            ops_damage(v, repair, "graph_file_missing: gen %llu "
                                  "(rebuildable by reopen, not by this repair)",
                       (unsigned long long)st.man.graph_gen);
        }
    }

    /* Quarantined evidence is a fact about this directory, not damage: a
     * healthy store that was repaired in the past still carries .torn bytes.
     * Surfacing it stops "verify says ok" from hiding recoverable data that
     * the manifest no longer references. */
    if (ops_is_dir(st.seg_dir)) {
        uint64_t torn = 0, quar = 0;
        ops_count_evidence(st.seg_dir, &torn, &quar);
        if (torn || quar)
            ops_note(v, "quarantined_evidence_present: %llu .torn, %llu .quarantine "
                        "file(s) kept on disk but not referenced by the manifest "
                        "(advisory)",
                     (unsigned long long)torn, (unsigned long long)quar);
    }

    /* ── repair: quarantine damaged segments, rewrite the manifest ──────── */
    if (repair) {
        sfb_lock_t lk;
        memset(&lk, 0, sizeof lk);
        snprintf(lk.path, sizeof lk.path, "%s", st.lock_path);
        lk.fd = -1;
        /* W2.1: mutating a store requires the exclusive writer lock. */
        rc = sfb_lock_acquire(&lk, st.lock_path, 0 /*exclusive*/, 0);
        if (rc != SFB_OK) {
            ops_hard(v, "repair_needs_writer_lock: SFB error %d", rc);
            free(entry_file); free(entry_ok); ops_files_free(&files);
            return rc;
        }

        int manifest_dirty = 0;
        uint64_t dropped_now = 0;
        /* walk backwards so ops_manifest_drop() keeps the remaining indices
         * pointing at the entries they were matched with */
        for (uint32_t i = st.man.nseg; i > 0; i--) {
            uint32_t ei = i - 1;
            if (entry_ok[ei]) continue;
            const sfb_seg_entry_t *e = &st.man.segs[ei];
            if (entry_file[ei] >= 0) {
                char torn[300] = {0};
                int q = ops_quarantine_file(st.seg_dir, files.v[entry_file[ei]].name,
                                            torn, sizeof torn);
                if (q == SFB_OK) {
                    v->quarantined++;
                    /* the old name and the new one, so an operator can find
                     * the bytes we refused to delete */
                    ops_note(v, "quarantined: %s -> %s",
                             files.v[entry_file[ei]].name, torn);
                } else if (q == SFB_E_NOTFOUND) {
                    ops_note(v, "quarantine_skipped_already_missing: %s",
                             files.v[entry_file[ei]].name);
                } else {
                    /* never drop a manifest entry whose bytes we could not keep */
                    ops_hard(v, "quarantine_failed: %s (SFB error %d); "
                                "manifest entry kept",
                             files.v[entry_file[ei]].name, q);
                    continue;
                }
            } else {
                ops_note(v, "dropped_missing_segment: gen %u seq %u (%u records)",
                         e->gen, e->seq, e->records);
            }
            dropped_now += e->records;
            if (st.man.total_bytes >= (uint64_t)e->length)
                st.man.total_bytes -= (uint64_t)e->length;
            else
                st.man.total_bytes = 0;
            ops_manifest_drop(&st.man, ei);
            manifest_dirty = 1;
        }

        if (manifest_dirty) {
            uint64_t sealed_now = 0;
            for (uint32_t i = 0; i < st.man.nseg; i++) sealed_now += st.man.segs[i].records;
            if (st.man.count > sealed_now) st.man.count = sealed_now;
            rc = sfb_store_manifest_save(&st);
            if (rc == SFB_OK) {
                v->manifest_rewritten = 1;
                (void)sfb_fsync_dir(st.dir);
            } else {
                ops_hard(v, "manifest_rewrite_failed: SFB error %d "
                            "(quarantined files are kept as .torn; manifest still "
                            "lists them)", rc);
            }
        }
        if (v->quarantined > 0) (void)sfb_fsync_dir(st.seg_dir);
        sfb_lock_release(&lk);

        /* re-derive the post-repair numbers from the manifest we just wrote,
         * so the report describes the store as it now is */
        v->records = 0;
        for (uint32_t i = 0; i < st.man.nseg; i++) v->records += st.man.segs[i].records;
        v->segments = st.man.nseg;
        v->records_dropped_est = dropped_now;
        v->segments_damaged = 0;
        v->segments_missing = 0;
        v->missing.n = 0;
        v->missing.dropped_names = 0;
        if (v->graph_bad) {
            /* repair cannot conjure a graph back; keep saying so */
            char why[128] = {0};
            if (!ops_graph_present(st.seg_dir, st.man.graph_gen, why, sizeof why))
                ops_list_add(&v->missing, "%s", why);
            ops_hard(v, "graph_file_missing_unrepairable: gen %llu",
                     (unsigned long long)st.man.graph_gen);
        }
        if (dropped_now > 0 && !v->manifest_rewritten) {
            /* nothing was actually dropped from the manifest: do not pretend */
            v->records_dropped_est = 0;
        }
    }

    /* ── verdict ────────────────────────────────────────────────────────── */
    v->ok = v->manifest_ok && v->hard_errors == 0 &&
            v->segments_damaged == 0 && v->segments_missing == 0;
    /* bytes moved aside but the manifest still points at them */
    if (repair && v->quarantined > 0 && !v->manifest_rewritten) v->ok = 0;

    int out_rc = v->ok ? SFB_OK : SFB_E_CORRUPT;

    free(entry_file);
    free(entry_ok);
    ops_files_free(&files);
    return out_rc;
}

/* JSON schema is fixed by the W7.1 spec:
 * {"ok","records","segments","damaged_segments","records_dropped_estimate",
 *  "missing_files":[...],"errors":[...]}
 * Extra keys are deliberately NOT added — the schema is a contract. */
static void ops_verify_json(ops_sb_t *sb, const char *path, const ops_verdict_t *v,
                            int rc, int repair)
{
    ops_sb_puts(sb, "{");
    ops_sb_puts(sb, "\"ok\":");
    ops_sb_puts(sb, v->ok ? "true" : "false");
    ops_sb_puts(sb, ",\"records\":");
    ops_sb_u64(sb, v->records);
    ops_sb_puts(sb, ",\"segments\":");
    ops_sb_u64(sb, v->segments);
    ops_sb_puts(sb, ",\"damaged_segments\":");
    ops_sb_u64(sb, v->segments_damaged);
    ops_sb_puts(sb, ",\"records_dropped_estimate\":");
    ops_sb_u64(sb, v->records_dropped_est);
    ops_sb_puts(sb, ",\"missing_files\":[");
    for (int i = 0; i < v->missing.n; i++) {
        if (i) ops_sb_puts(sb, ",");
        ops_json_str(sb, v->missing.msg[i]);
    }
    ops_sb_puts(sb, "],\"errors\":[");
    for (int i = 0; i < v->errors.n; i++) {
        if (i) ops_sb_puts(sb, ",");
        ops_json_str(sb, v->errors.msg[i]);
    }
    ops_sb_puts(sb, "]");
    (void)path; (void)rc; (void)repair;
    ops_sb_puts(sb, "}");
}

int sfb_verify(const char *path, const char *key, int repair,
               char *json_out, size_t json_cap)
{
    ops_sb_t sb;
    ops_sb_init(&sb);
    ops_verdict_t *v = (ops_verdict_t *)calloc(1, sizeof *v);
    if (!v) {
        ops_sb_free(&sb);
        return SFB_E_NOMEM;
    }
    int rc = ops_verify_run(path, key, repair ? 1 : 0, v);
    ops_verify_json(&sb, path, v, rc, repair);
    ops_json_flush(&sb, json_out, json_cap);
    ops_sb_free(&sb);
    free(v);
    return rc;
}

/* ── writer-lock probe ────────────────────────────────────────────────── */
/*
 * sfb_lock_holder() reads the PID *record* inside the lock file, and a shared
 * reader leaves its own record behind on release (sfb_lock_release only unlinks
 * for writers). So a record saying "pid N, mode reader" can outlive the lock —
 * it is evidence, not ownership. The authoritative test is the kernel's flock,
 * which is exactly what a writer would have to take:
 *
 *   open the lock file O_RDONLY (no create, no truncate, no mutation)
 *   flock(LOCK_SH|LOCK_NB)  → EWOULDBLOCK means an exclusive writer is inside
 *
 * Holding that shared flock for the whole copy also stops a writer from
 * starting halfway through the backup. sfb_lock_holder() is still consulted to
 * name the holder in the error message.
 */
typedef struct {
    int fd;
} ops_lockprobe_t;

static int ops_lock_probe(const char *lock_path, ops_lockprobe_t *p,
                          pid_t *holder, char *started, size_t started_cap)
{
    p->fd = -1;
    if (holder) *holder = 0;
    if (started && started_cap) started[0] = '\0';

    int fd = open(lock_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT) ? SFB_OK : SFB_OK;  /* no file, no writer */
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK || e == EAGAIN) {
            (void)sfb_lock_holder(lock_path, holder, started, started_cap);
            return SFB_E_LOCKED;
        }
        return SFB_OK;      /* probe failed for an unrelated reason: be tolerant */
    }
    p->fd = fd;             /* keep it: blocks new writers while we copy */
    return SFB_OK;
}

static void ops_lock_probe_end(ops_lockprobe_t *p)
{
    if (p->fd >= 0) {
        (void)flock(p->fd, LOCK_UN);
        (void)close(p->fd);
        p->fd = -1;
    }
}

/* ── shared copy of a whole store (backup, migrate v2 no-op) ───────────── */
/* Copies <src_dir> → <dst_dir>: sealed segments and the graph first, the
 * manifest last, so <dst_dir> is never observable in a half state (W7.3). */
static int ops_copy_store(const char *src_path, const char *dst_path,
                          int check_writer_lock)
{
    if (!src_path || !src_path[0] || !dst_path || !dst_path[0]) return SFB_E_INVALID;

    sfb_store_t src;
    if (ops_fill_paths(&src, src_path) != SFB_OK) return SFB_E_INVALID;
    if (!ops_is_dir(src_path)) return SFB_E_NOTFOUND;
    if (!ops_is_file(src.manifest_path)) return SFB_E_NOTFOUND;

    int rc = SFB_OK;
    ops_lockprobe_t probe;
    probe.fd = -1;
    int got_lock = 0;                 /* release the probe on every exit path */
    char dst_seg[4096];
    dst_seg[0] = '\0';
    ops_files_t files;
    memset(&files, 0, sizeof files);
    int have_files = 0;

    /* A writer mid-flush can tear a segment; refuse rather than ship it. */
    if (check_writer_lock) {
        pid_t pid = 0;
        char started[128] = {0};
        rc = ops_lock_probe(src.lock_path, &probe, &pid, started, sizeof started);
        if (rc != SFB_OK) { rc = SFB_E_LOCKED; goto out; }
        got_lock = 1;
    }

    rc = sfb_store_manifest_load(&src);
    if (rc != SFB_OK) { rc = (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc; goto out; }
    if (src.man.magic != SFB_MAGIC_MANIFEST) { rc = SFB_E_CORRUPT; goto out; }

    if (ops_is_file(dst_path)) { rc = SFB_E_STATE; goto out; }
    if (ops_is_dir(dst_path)) {
        char man[4096];
        if (sfb_path_join(man, sizeof man, dst_path, SFB_MANIFEST_NAME) == 0 &&
            ops_is_file(man)) {
            rc = SFB_E_STATE;   /* never merge into an existing snapshot */
            goto out;
        }
    }
    rc = sfb_mkdir_p(dst_path, 0700);
    if (rc != SFB_OK) { rc = SFB_E_IO; goto out; }
    if (sfb_path_join(dst_seg, sizeof dst_seg, dst_path, SFB_SEG_DIR) != 0) {
        rc = SFB_E_INVALID; goto out;
    }
    rc = sfb_mkdir_p(dst_seg, 0700);
    if (rc != SFB_OK) { rc = SFB_E_IO; goto out; }

    /* segments + graph, in directory order */
    rc = ops_scan_dir(&files, src.seg_dir, NULL);
    if (rc == SFB_E_NOTFOUND) rc = SFB_OK;
    if (rc != SFB_OK) goto out;
    have_files = 1;

    uint64_t files_copied = 0;
    for (size_t i = 0; i < files.n; i++) {
        char sp[4096], dp[4096];
        if (sfb_path_join(sp, sizeof sp, src.seg_dir, files.v[i].name) != 0) continue;
        if (sfb_path_join(dp, sizeof dp, dst_seg, files.v[i].name) != 0) continue;
        int used_link = 0;
        rc = ops_link_or_copy(sp, dp, &used_link);
        if (rc != SFB_OK) goto out;
        files_copied++;
        (void)used_link;
    }

    /* HNSW snapshots are part of the store (W7.3 says "the graph file"), so
     * they are copied before the manifest exactly like the segments. */
    {
        ops_files_t graphs;
        memset(&graphs, 0, sizeof graphs);
        int grc = ops_scan_graphs(&graphs, src.seg_dir);
        uint64_t copied_graphs = 0;
        if (grc == SFB_OK) {
            for (size_t i = 0; i < graphs.n; i++) {
                char sp[4096], dp[4096];
                if (sfb_path_join(sp, sizeof sp, src.seg_dir, graphs.v[i].name) != 0)
                    continue;
                if (sfb_path_join(dp, sizeof dp, dst_seg, graphs.v[i].name) != 0)
                    continue;
                int used_link = 0;
                rc = ops_link_or_copy(sp, dp, &used_link);
                if (rc != SFB_OK) { ops_files_free(&graphs); goto out; }
                copied_graphs++;
                (void)used_link;
            }
        }
        ops_files_free(&graphs);
        if (src.man.graph_gen != 0 && copied_graphs == 0) {
            char why[128] = {0};
            (void)ops_graph_present(src.seg_dir, src.man.graph_gen, why, sizeof why);
            rc = SFB_E_CORRUPT;   /* incomplete snapshot is worse than none */
            goto out;
        }
    }

    if (files_copied < src.man.nseg) {
        rc = SFB_E_CORRUPT;       /* fewer payload files than the manifest claims */
        goto out;
    }

    /* the manifest LAST — this is the consistency point (W7.3) */
    {
        char dman[4096];
        if (sfb_path_join(dman, sizeof dman, dst_path, SFB_MANIFEST_NAME) != 0) {
            rc = SFB_E_INVALID; goto out;
        }
        rc = ops_copy_file(src.manifest_path, dman);
        if (rc != SFB_OK) goto out;
    }
    (void)sfb_fsync_dir(dst_seg);
    (void)sfb_fsync_dir(dst_path);

    /* close the race: a writer that rewrote the manifest while we copied must
     * not leave a snapshot referencing files we never linked. */
    {
        sfb_store_t chk;
        if (ops_fill_paths(&chk, dst_path) == SFB_OK &&
            sfb_store_manifest_load(&chk) == SFB_OK) {
            ops_files_t have;
            memset(&have, 0, sizeof have);
            (void)ops_scan_dir(&have, chk.seg_dir, NULL);
            int missing = 0;
            for (uint32_t i = 0; i < chk.man.nseg; i++) {
                const sfb_seg_entry_t *e = &chk.man.segs[i];
                int found = 0;
                for (size_t fi = 0; fi < have.n && !found; fi++) {
                    char fp[4096];
                    if (sfb_path_join(fp, sizeof fp, chk.seg_dir, have.v[fi].name) != 0)
                        continue;
                    uint8_t *buf = NULL;
                    size_t len = 0;
                    if (ops_read_file(fp, &buf, &len) != SFB_OK) continue;
                    uint32_t crcs[OPS_NBASES];
                    ops_digest_bases(buf, len, e->length, crcs);
                    free(buf);
                    for (int k = 0; k < OPS_NBASES; k++)
                        if (crcs[k] == e->digest) { found = 1; break; }
                }
                if (!found) missing++;
            }
            ops_files_free(&have);
            if (missing) rc = SFB_E_CORRUPT;
        }
    }

out:
    if (have_files) ops_files_free(&files);
    if (got_lock) ops_lock_probe_end(&probe);
    return rc;
}

int sfb_backup(const char *path, const char *dest)
{
    return ops_copy_store(path, dest, 1 /* refuse while a writer holds it */);
}

/* ── restore: inverse of backup, never overwriting ─────────────────────── */
int sfb_restore(const char *backup_path, const char *dest)
{
    if (!backup_path || !backup_path[0] || !dest || !dest[0]) return SFB_E_INVALID;

    sfb_store_t b;
    if (ops_fill_paths(&b, backup_path) != SFB_OK) return SFB_E_INVALID;
    if (!ops_is_dir(backup_path)) return SFB_E_NOTFOUND;
    if (!ops_is_file(b.manifest_path)) return SFB_E_INVALID;   /* not a snapshot */

    /* never overwrite an existing destination (W7.3) */
    struct stat st;
    if (stat(dest, &st) == 0) return SFB_E_STATE;

    int rc = sfb_store_manifest_load(&b);
    if (rc != SFB_OK) return (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc;

    if (sfb_mkdir_p(dest, 0700) != SFB_OK) return SFB_E_IO;
    char dseg[4096];
    if (sfb_path_join(dseg, sizeof dseg, dest, SFB_SEG_DIR) != 0) return SFB_E_INVALID;
    if (sfb_mkdir_p(dseg, 0700) != SFB_OK) return SFB_E_IO;

    ops_files_t files;
    memset(&files, 0, sizeof files);
    int scan = ops_scan_dir(&files, b.seg_dir, NULL);
    if (scan != SFB_OK && scan != SFB_E_NOTFOUND) { ops_files_free(&files); return scan; }
    for (size_t i = 0; i < files.n; i++) {
        char s[4096], d[4096];
        if (sfb_path_join(s, sizeof s, b.seg_dir, files.v[i].name) != 0) continue;
        if (sfb_path_join(d, sizeof d, dseg, files.v[i].name) != 0) continue;
        /* Always a byte copy: the snapshot must stay pristine, never inode-
         * shared with the restored store. */
        rc = ops_copy_file(s, d);
        if (rc != SFB_OK) { ops_files_free(&files); return rc; }
    }
    ops_files_free(&files);
    /* HNSW snapshots (graph-<gen>.bin) belong to the store */
    {
        ops_files_t graphs;
        memset(&graphs, 0, sizeof graphs);
        int grc = ops_scan_graphs(&graphs, b.seg_dir);
        if (grc == SFB_OK) {
            for (size_t i = 0; i < graphs.n; i++) {
                char s[4096], d[4096];
                if (sfb_path_join(s, sizeof s, b.seg_dir, graphs.v[i].name) != 0) continue;
                if (sfb_path_join(d, sizeof d, dseg, graphs.v[i].name) != 0) continue;
                rc = ops_copy_file(s, d);
                if (rc != SFB_OK) { ops_files_free(&graphs); return rc; }
            }
        }
        ops_files_free(&graphs);
    }

    char dman[4096];
    if (sfb_path_join(dman, sizeof dman, dest, SFB_MANIFEST_NAME) != 0) return SFB_E_INVALID;
    rc = ops_copy_file(b.manifest_path, dman);
    if (rc != SFB_OK) return rc;
    (void)sfb_fsync_dir(dseg);
    (void)sfb_fsync_dir(dest);

    /* the restored layout must be openable */
    sfb_store_t chk;
    if (ops_fill_paths(&chk, dest) != SFB_OK) return SFB_E_INVALID;
    rc = sfb_store_manifest_load(&chk);
    if (rc != SFB_OK) return (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc;
    return SFB_OK;
}

/* ── stats ─────────────────────────────────────────────────────────────── */
static int ops_probe_dim(const char *path, uint32_t *dim)
{
    sfb_store_t st;
    if (ops_fill_paths(&st, path) != SFB_OK) return SFB_E_INVALID;
    if (!ops_is_file(st.manifest_path)) return SFB_E_NOTFOUND;
    int rc = sfb_store_manifest_load(&st);
    if (rc != SFB_OK) return (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc;
    if (st.man.magic != SFB_MAGIC_MANIFEST) return SFB_E_CORRUPT;
    if (st.man.dim < SFB_MIN_DIM || st.man.dim > SFB_MAX_DIM) return SFB_E_CORRUPT;
    *dim = st.man.dim;
    return SFB_OK;
}

static void ops_stats_json_emit(ops_sb_t *sb, const char *path, const uint32_t dim,
                                const sfb_stats_t *st)
{
    ops_sb_puts(sb, "{\"ok\":true,\"path\":");
    ops_json_str(sb, path);
    ops_sb_printf(sb, ",\"dim\":%u", dim);
    ops_sb_printf(sb, ",\"format_version\":%d", SFB_FORMAT_VERSION);
    ops_sb_printf(sb, ",\"records\":%llu", (unsigned long long)st->records);
    ops_sb_printf(sb, ",\"live\":%llu", (unsigned long long)st->live);
    ops_sb_printf(sb, ",\"tombstones\":%llu", (unsigned long long)st->tombstones);
    ops_sb_printf(sb, ",\"segments\":%llu", (unsigned long long)st->segments);
    ops_sb_printf(sb, ",\"generations\":%llu", (unsigned long long)st->generations);
    ops_sb_printf(sb, ",\"bytes_records\":%llu", (unsigned long long)st->bytes_records);
    ops_sb_printf(sb, ",\"bytes_on_disk\":%llu", (unsigned long long)st->bytes_on_disk);
    ops_sb_printf(sb, ",\"records_dropped\":%llu", (unsigned long long)st->records_dropped);
    ops_sb_printf(sb, ",\"segments_quarantined\":%llu",
                  (unsigned long long)st->segments_quarantined);
    ops_sb_printf(sb, ",\"ids_renumbered\":%llu", (unsigned long long)st->ids_renumbered);
    ops_sb_printf(sb, ",\"index_rebuilt\":%llu", (unsigned long long)st->index_rebuilt);
    ops_sb_printf(sb, ",\"flushes\":%llu", (unsigned long long)st->flushes);
    ops_sb_printf(sb, ",\"flush_last_us\":%llu", (unsigned long long)st->flush_last_us);
    ops_sb_printf(sb, ",\"open_us\":%llu", (unsigned long long)st->open_us);
    ops_sb_printf(sb, ",\"events\":%llu", (unsigned long long)st->events);
    ops_sb_puts(sb, "}");
}

int sfb_stats_json(const char *path, const char *key, char *out, size_t cap)
{
    ops_sb_t sb;
    ops_sb_init(&sb);

    if (!path || !path[0]) {
        ops_sb_puts(&sb, "{\"ok\":false,\"errors\":[\"invalid_argument: path is required\"]}");
        ops_json_flush(&sb, out, cap);
        ops_sb_free(&sb);
        return SFB_E_INVALID;
    }

    uint32_t dim = 0;
    int rc = ops_probe_dim(path, &dim);
    if (rc != SFB_OK) {
        ops_sb_printf(&sb, "{\"ok\":false,\"errors\":[\"manifest_unreadable: SFB error %d\"]}", rc);
        ops_json_flush(&sb, out, cap);
        ops_sb_free(&sb);
        return rc;
    }

    sfb_config_t cfg;
    sfb_config_default(&cfg, path, dim);
    cfg.path = path;
    cfg.dim = dim;
    cfg.read_only = 1;      /* W2.2: observers never write */
    cfg.no_lock = 1;        /* stats must not create or steal a lock file */
    cfg.verify_full = 0;
    cfg.key = key;

    sfb_brain_t *b = NULL;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK || !b) {
        ops_sb_printf(&sb, "{\"ok\":false,\"errors\":[\"open_failed: SFB error %d\"]}", rc);
        ops_json_flush(&sb, out, cap);
        ops_sb_free(&sb);
        return rc != SFB_OK ? rc : SFB_E_STATE;
    }

    sfb_stats_t st;
    memset(&st, 0, sizeof st);
    rc = sfb_stats_get(b, &st);
    if (rc != SFB_OK) {
        (void)sfb_close(b);
        ops_sb_printf(&sb, "{\"ok\":false,\"errors\":[\"stats_failed: SFB error %d\"]}", rc);
        ops_json_flush(&sb, out, cap);
        ops_sb_free(&sb);
        return rc;
    }
    ops_stats_json_emit(&sb, path, dim, &st);
    ops_json_flush(&sb, out, cap);
    ops_sb_free(&sb);

    rc = sfb_close(b);
    if (rc != SFB_OK) return rc;
    if (sb.oom) return SFB_E_NOMEM;
    return SFB_OK;
}

/* ── migrate: v1 single-file brain → v2 segmented store (W7.5) ─────────── */
/*
 * v1 record layout is unknown here, and inventing one would corrupt data, so
 * this function is deliberately honest about what it can and cannot do:
 *
 *   • first 4 bytes == SFB_MAGIC_MANIFEST   → already v2: no-op copy of the
 *     whole store into dst (segments first, manifest last). If dst already
 *     holds a v2 store, this is a pure no-op and SFB_OK.
 *   • first 4 bytes == "QTS\x01"            → a QTSQ v1 container. Decoding it
 *     needs the QTSQ stream reader, which is out of scope for this pass:
 *     SFB_E_INVALID plus a JSON reason on stderr.
 *   • anything else                          → SFB_E_INVALID plus a JSON reason
 *     naming the magic we saw.
 *
 * The frozen ABI (int sfb_migrate) has no report parameter, so the JSON goes to
 * stderr — see assumption A8.
 */
static int ops_migrate_refuse(const char *src, const char *code, const char *reason,
                              uint32_t magic)
{
    ops_sb_t sb;
    ops_sb_init(&sb);
    ops_sb_puts(&sb, "{\"ok\":false,\"error\":");
    ops_json_str(&sb, code);
    ops_sb_puts(&sb, ",\"path\":");
    ops_json_str(&sb, src);
    ops_sb_printf(&sb, ",\"magic\":\"0x%08x\"", magic);
    ops_sb_puts(&sb, ",\"reason\":");
    ops_json_str(&sb, reason);
    ops_sb_puts(&sb, "}");
    if (sb.p && !sb.oom) fprintf(stderr, "%s\n", sb.p);
    else fprintf(stderr, "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}\n", code, reason);
    fflush(stderr);
    ops_sb_free(&sb);
    return SFB_E_INVALID;
}


/* ── v1 QTSQ brain import (W7.5) ────────────────────────────────────────────
 * A v1 brain is a single QTSQ container holding the network weights, neuron
 * biases, a (num_memories x vector_dim) float32 memory matrix and a metadata
 * JSON blob. libqtsq already exposes the reader, so migration is a real import,
 * not a guess at the format: qtsq_brain_load() hands back exactly the matrix
 * this store wants, and each row becomes one v2 record.
 *
 * The source is copied to "<src>.v1backup" before anything is written, so the
 * pre-migration state survives even if the import is later rejected.
 * ─────────────────────────────────────────────────────────────────────────── */
static int ops_migrate_v1(const char *src, const char *dst, const char *key,
                          uint32_t magic)
{
    qtsq_context_t c;
    float *mems = NULL;
    size_t nmem = 0, vdim = 0, count = 0;
    uint32_t nstreams = 0, i;
    int rc, seen_memories = 0, need_key = 0, bad_key = 0;

    qtsq_init(&c);
    rc = qtsq_read(&c, src);
    if (rc != QTSQ_OK) {
        qtsq_free(&c);
        return ops_migrate_refuse(src, "v1_read_failed",
                                  "QTSQ refused to read the container", magic);
    }

    /* v1 brains are written encrypted; the passphrase arrives via sfb_migrate's
     * `key` argument. Without it there is nothing to guess at. */
    if (qtsq_is_encrypted(&c)) {
        if (key == NULL || key[0] == '\0') {
            qtsq_free(&c);
            return ops_migrate_refuse(
                src, "v1_key_required",
                "the v1 container is encrypted; pass the passphrase to migrate",
                magic);
        }
        if (qtsq_decrypt(&c, key) != QTSQ_OK) {
            qtsq_free(&c);
            return ops_migrate_refuse(src, "v1_decrypt_failed",
                                      "the passphrase does not open this v1 container",
                                      magic);
        }
    }
    (void)need_key; (void)bad_key;

    rc = qtsq_container_get_count(&c, &nstreams);
    if (rc != QTSQ_OK) {
        qtsq_free(&c);
        return ops_migrate_refuse(src, "v1_streams_unreadable",
                                 "the container has no readable stream table", magic);
    }

    for (i = 0; i < nstreams; i++) {
        qtsq_context_t sub;
        char name[128];
        float *data = NULL;
        size_t n = 0;

        memset(name, 0, sizeof name);
        if (qtsq_container_get_stream(&c, i, &sub, name, sizeof name) != QTSQ_OK)
            continue;
        if (strcmp(name, "memories") == 0) {
            if (qtsq_decompress_tensor(&sub, &data, &n) == QTSQ_OK && data != NULL) {
                /* dims are carried in the stream schema: {rows, vector_dim} */
                if (sub.schema.num_dims >= 2 && sub.schema.dimensions[1] > 0) {
                    nmem = sub.schema.dimensions[0];
                    vdim = sub.schema.dimensions[1];
                } else if (n > 0) {
                    vdim = 0;   /* unknown shape: refused below, never guessed */
                }
                mems = data;
                count = n;
                seen_memories = 1;
            }
        }
        qtsq_free(&sub);
        if (seen_memories)
            break;
    }
    qtsq_free(&c);

    if (!seen_memories || mems == NULL || count == 0) {
        free(mems);
        return ops_migrate_refuse(src, "v1_has_no_memories",
                                  "the v1 container holds no memory stream to import",
                                  magic);
    }
    if (vdim == 0 || nmem == 0 || nmem * vdim != count) {
        free(mems);
        return ops_migrate_refuse(
            src, "v1_unknown_shape",
            "the memory stream does not declare a usable {rows, vector_dim} shape",
            magic);
    }
    if (vdim < 64u || vdim > 4096u) {
        free(mems);
        return ops_migrate_refuse(src, "v1_unsupported_dimension",
                                  "v1 vector_dim is outside the v2 range 64..4096", magic);
    }

    /* Never import into a store that already holds data: rerunning migrate
     * would silently duplicate every record (dedup is KEEP_BOTH here). */
    if (ops_is_dir(dst)) {
        char dman[4200];
        if (sfb_path_join(dman, sizeof dman, dst, SFB_MANIFEST_NAME) == SFB_OK &&
            ops_is_file(dman)) {
            free(mems);
            return ops_migrate_refuse(
                src, "dest_not_empty",
                "the destination is already a v2 store; migrate needs an empty path",
                magic);
        }
    }

    /* Never import into a store that already holds data: rerunning migrate
     * would silently duplicate every record (dedup is KEEP_BOTH here). */
    if (ops_is_dir(dst)) {
        char dman[4200];
        if (sfb_path_join(dman, sizeof dman, dst, SFB_MANIFEST_NAME) == SFB_OK &&
            ops_is_file(dman)) {
            free(mems);
            return ops_migrate_refuse(
                src, "dest_not_empty",
                "the destination is already a v2 store; migrate needs an empty path",
                magic);
        }
    }

    /* pre-migration snapshot of the source, never overwritten */
    char backup[4200];
    if (sfb_snprintf(backup, sizeof backup, "%s.v1backup", src) >= 0 &&
        !ops_is_file(backup)) {
        if (ops_copy_file(src, backup) != SFB_OK) {
            free(mems);
            return ops_migrate_refuse(src, "backup_failed",
                                      "could not write the pre-migration snapshot", magic);
        }
    }

    sfb_config_t cfg;
    sfb_config_default(&cfg, dst, (uint32_t)vdim);
    cfg.mode = SFB_MODE_VECTOR_DB;      /* imported weights are data, not memories */
    cfg.key = key;
    cfg.dedup_action = SFB_DEDUP_KEEP_BOTH;  /* never fold two imported rows */

    sfb_brain_t *b = NULL;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) {
        free(mems);
        char why[160];
        snprintf(why, sizeof why, "cannot open the destination store: %s",
                 sfb_strerror(rc));
        return ops_migrate_refuse(src, "dest_open_failed", why, magic);
    }

    uint64_t imported = 0, rejected = 0;
    char text[96];
    for (size_t r = 0; r < nmem; r++) {
        sfb_remember_result_t res;
        snprintf(text, sizeof text, "v1-memory-%zu", r);
        if (sfb_remember(b, mems + r * vdim, text, "v1_import", &res) == SFB_OK)
            imported++;
        else
            rejected++;               /* non-finite row: counted, never silent */
    }
    sfb_flush_result_t fr;
    int frc = sfb_flush(b, &fr);
    sfb_close(b);
    free(mems);

    if (frc != SFB_OK) {
        fprintf(stderr, "{\"ok\":false,\"v1\":true,\"error\":\"flush_failed\","
                        "\"code\":%d,\"imported\":%llu}\n", frc,
                (unsigned long long)imported);
        return frc;
    }
    fprintf(stderr, "{\"ok\":true,\"v1\":true,\"action\":\"imported\","
                    "\"records\":%llu,\"rejected\":%llu,\"dim\":%zu,"
                    "\"backup\":\"%s\"}\n",
            (unsigned long long)imported, (unsigned long long)rejected,
            vdim, backup);
    return SFB_OK;
}

int sfb_migrate(const char *src_path, const char *dst_path, const char *key)
{
    if (!src_path || !src_path[0] || !dst_path || !dst_path[0]) {
        return ops_migrate_refuse(src_path ? src_path : "", "invalid_argument",
                                 "both src_path and dst_path are required", 0);
    }
    /* key is forwarded to the importer, which KDFs it for the v2 store */

    if (strcmp(src_path, dst_path) == 0) {
        return ops_migrate_refuse(src_path, "same_path",
                                 "src_path and dst_path are the same path", 0);
    }
    if (ops_is_file(dst_path)) {
        return ops_migrate_refuse(src_path, "dest_is_a_file",
                                 "dst_path exists and is a regular file, not a store "
                                 "directory", 0);
    }

    if (!ops_is_file(src_path)) {
        return ops_migrate_refuse(src_path, "source_missing",
                                 "src_path is not a regular file", 0);
    }

    /* v2 files are a directory layout; a lone file can only be a v1 brain or a
     * copy of our manifest. */
    uint8_t head[4] = {0};
    int fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ops_migrate_refuse(src_path, "io_error", "cannot open source", 0);
    ssize_t got = pread(fd, head, sizeof head, 0);
    close(fd);
    if (got < 4)
        return ops_migrate_refuse(src_path, "source_too_short",
                                 "source is smaller than 4 bytes; not a brain", 0);

    uint32_t magic;
    memcpy(&magic, head, sizeof magic);

    /* ---- already v2 ---------------------------------------------------- */
    if (magic == SFB_MAGIC_MANIFEST) {
        char src_dir[4096];
        snprintf(src_dir, sizeof src_dir, "%s", src_path);
        char *slash = strrchr(src_dir, '/');
        if (slash) *slash = '\0';
        else snprintf(src_dir, sizeof src_dir, ".");
        if (!ops_is_dir(src_dir)) {
            return ops_migrate_refuse(src_path, "v2_manifest_without_directory",
                                     "source is a v2 manifest but its directory is gone", magic);
        }
        /* If dst already is a v2 store, migration is a no-op. */
        char dman[4096];
        if (ops_is_dir(dst_path) &&
            sfb_path_join(dman, sizeof dman, dst_path, SFB_MANIFEST_NAME) == 0 &&
            ops_is_file(dman)) {
            fprintf(stderr, "{\"ok\":true,\"already_v2\":true,\"action\":\"noop\"}\n");
            return SFB_OK;
        }
        int rc = ops_copy_store(src_dir, dst_path, 0);
        if (rc != SFB_OK) {
            fprintf(stderr, "{\"ok\":false,\"already_v2\":true,\"error\":\"copy_failed\","
                            "\"code\":%d}\n", rc);
            return rc;
        }
        /* a v2 source is a no-op in content terms: nothing is re-encoded */
        fprintf(stderr, "{\"ok\":true,\"already_v2\":true,\"action\":\"copied\"}\n");
        return SFB_OK;
    }

    /* ---- a v1 brain must be a single file with no sidecar .d dir -------- */
    char sidecar[4096];
    snprintf(sidecar, sizeof sidecar, "%s.d", src_path);
    if (ops_is_dir(sidecar)) {
        return ops_migrate_refuse(src_path, "ambiguous_layout",
                                 "source has a .d directory but a non-v2 magic; "
                                 "not a v1 single-file brain", magic);
    }

    /* ---- QTSQ container: recognised, but not importable in this pass ---- */
    if (head[0] == 'Q' && head[1] == 'T' && head[2] == 'S' && head[3] == 0x01) {
        return ops_migrate_v1(src_path, dst_path, key, magic);
    }

    return ops_migrate_refuse(src_path, "unrecognised_v1_layout",
                             "unknown v1 layout: no importer for this magic", magic);
}
