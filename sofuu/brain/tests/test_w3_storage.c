/*
 * tests/test_w3_storage.c — the four W3 storage guarantees.
 *
 *   W3.5  a per-record CRC isolates damage to ONE record.  A segment this build
 *         writes sets SFB_SEG_FLAG_RECCRC_FULL (bit 3) and its per-record CRC32C
 *         covers the whole record, vector included.  On replay an interior
 *         record that fails that CRC is skipped and counted, and the rest of the
 *         segment imports; a torn/truncated segment still falls as a unit.
 *
 *   W3.2  no sealed segment ever holds more than SFB_SEGMENT_CAP (256) records,
 *         even when a single operation dirties 1000 rows.
 *
 *   W3.4  unreferenced files are reclaimed automatically — five
 *         decay_tick+flush cycles keep disk amplification bounded and collect a
 *         stale `.tmp`, without an explicit sfb_compact().
 *
 *   W3.3  open is lazy: a normal open reads the manifest and probes the newest
 *         segment's length, not every payload.  cfg.verify_full restores the
 *         paranoid path, and a torn NEWEST segment still fails the open.
 *
 * These tests damage real segment bytes, so they need the record geometry.  The
 * float32 record block is (sfb_brain.c:44-58, the authoritative layout note):
 *
 *   u32 record_count, then per record
 *   +0   24 B  id, flags, tier, text_len, role_len, strength   (CRC span)
 *   +24  16 B  f64 created_at, f64 last_hit
 *   +40   4 B  u32 crc32c(legacy span: head ++ text ++ role)
 *   +44  tl B  text
 *   +44+tl      rl B role
 *   +44+tl+rl   dim*4 B f32 vector
 *
 * and the segment file is a 48-byte header (sfb_store.c:135) followed by that
 * block.  Every test below therefore uses an UNENCRYPTED, f32 store: a sealed
 * payload would fail its GCM tag and be rejected as a unit before any per-record
 * CRC could attribute the damage, which is a different (correct) behaviour.
 *
 * Deterministic: fixed vectors, no RNG, no network, temp dirs under /tmp only.
 * Build:  cc ... sfb_*.c tests/test_w3_storage.c -o /tmp/test_w3_storage
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <time.h>

/* ── tiny harness (same shape as tests/test_w2_safety.c) ──────────────── */

static int g_pass;
static int g_fail;
static char g_root[512];

static void check(int cond, const char *what, const char *detail)
{
    if (cond) {
        printf("  [ok]   %s\n", what);
        g_pass++;
    } else {
        printf("  [FAIL] %s%s%s\n", what, detail != NULL && detail[0] ? " — " : "",
               detail != NULL ? detail : "");
        g_fail++;
    }
}

static const char *rc_name(int rc)
{
    switch (rc) {
    case SFB_OK:         return "SFB_OK";
    case SFB_E_INVALID:  return "SFB_E_INVALID";
    case SFB_E_IO:       return "SFB_E_IO";
    case SFB_E_NOMEM:    return "SFB_E_NOMEM";
    case SFB_E_NOTFOUND: return "SFB_E_NOTFOUND";
    case SFB_E_STATE:    return "SFB_E_STATE";
    case SFB_E_DIM:      return "SFB_E_DIM";
    case SFB_E_LOCKED:   return "SFB_E_LOCKED";
    case SFB_E_STALE:    return "SFB_E_STALE";
    case SFB_E_READONLY: return "SFB_E_READONLY";
    case SFB_E_CORRUPT:  return "SFB_E_CORRUPT";
    default:             return "other";
    }
}

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Best-effort recursive removal; only ever used on the test's own /tmp tree. */
static void rm_rf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        struct dirent *de;
        DIR *d = opendir(path);

        if (d != NULL) {
            while ((de = readdir(d)) != NULL) {
                char child[4096];

                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) < (int)sizeof child)
                    rm_rf(child);
            }
            (void)closedir(d);
        }
        (void)rmdir(path);
        return;
    }
    (void)unlink(path);
}

/* Deterministic, finite vectors — no RNG state to reproduce. */
static void fill_vec(float *v, uint32_t dim, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 12345u;
    uint32_t i;

    for (i = 0; i < dim; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = ((float)((s >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
    }
}

/* ── on-disk geometry the tests have to reason about ───────────────────
 * SFB_SEG_HDR_LEN is private to sfb_store.c (48, :135); the 44-byte record
 * header is SFB_REC_HDR in sfb_brain.c (:108).  The names and the segment file
 * name format come from sfb_store.c:242 (`seg-%06u-%06u.qtsq`).
 * ---------------------------------------------------------------------- */
#define SEG_HDR_BYTES  48u
#define REC_HDR_BYTES  44u
#define REC_OFF_TEXT   0u
#define REC_OFF_ID     0u
#define REC_OFF_TL     12u
#define REC_OFF_RL     16u

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static int path_join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int seg_dir_of(char *out, size_t cap, const char *brain)
{
    return path_join(out, cap, brain, SFB_SEG_DIR);
}

/* Full path of a segment file: <brain>/brain.qtsq.d/<name>. */
static int seg_full(char *out, size_t cap, const char *brain, const char *name)
{
    char sd[1024];

    if (seg_dir_of(sd, sizeof sd, brain) != 0) return -1;
    return path_join(out, cap, sd, name);
}

/* Segment file name exactly as sfb_store.c:242 builds it. */
static int seg_name(char *out, size_t cap, uint32_t gen, uint32_t seq)
{
    int n = snprintf(out, cap, "seg-%06u-%06u.qtsq", (unsigned)gen, (unsigned)seq);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Files in `dir` matching seg-<gen>-<seq>.qtsq (final segments only; a `.tmp`
 * or `.quarantine` sibling is deliberately not counted).  Sorted: names are
 * zero padded, so lexical order is generation order. */
static int seg_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int seg_files(const char *dir, char names[][256], int max)
{
    struct dirent *de;
    DIR *d = opendir(dir);
    int n = 0;

    if (d == NULL) return -1;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);

        if (strncmp(de->d_name, "seg-", 4) != 0) continue;
        if (l < 5u + 4u || strcmp(de->d_name + l - 5u, ".qtsq") != 0) continue;
        if (n < max) {
            snprintf(names[n], 256, "%s", de->d_name);
            n++;
        }
    }
    (void)closedir(d);
    qsort(names, (size_t)n, 256, seg_cmp);
    return n;
}

/* Total bytes of every regular file under dir, recursively (what `du` reports
 * for the store tree).  Used only on the test's own /tmp tree. */
static uint64_t dir_bytes(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *de;
    uint64_t total = 0;

    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;

    d = opendir(path);
    if (d == NULL) return 0;
    while ((de = readdir(d)) != NULL) {
        char child[4096];

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (path_join(child, sizeof child, path, de->d_name) == 0)
            total += dir_bytes(child);
    }
    (void)closedir(d);
    return total;
}

/* ── segment inspector ────────────────────────────────────────────────── */

typedef struct {
    uint32_t records;
    uint32_t dim;
    uint16_t flags;
    uint64_t payload_len;
} seg_meta_t;

typedef struct {
    off_t    text_off;   /* absolute file offset of the record's text[0]   */
    off_t    vec_off;    /* absolute file offset of the record's vector[0] */
    uint32_t id, text_len, role_len;
} rec_loc_t;

/* Read a segment this build wrote and, when want_id is meaningful, locate that
 * record's text and vector inside the file.
 *
 * Returns 1 when the record was found, 0 when the segment parsed but holds no
 * such id, -1 when the file is not a plaintext f32 segment this test can walk.
 * Sealed/int8 segments are refused on purpose: the W3.5 record-isolation path
 * is only reachable on a payload whose bytes survive to the record walk. */
static int seg_scan(const char *path, uint32_t want_id, seg_meta_t *m, rec_loc_t *loc)
{
    struct stat st;
    uint8_t *buf = NULL;
    uint32_t count, off, i;
    int fd = -1, rc = -1;

    if (m != NULL) memset(m, 0, sizeof *m);
    if (loc != NULL) memset(loc, 0, sizeof *loc);

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(SEG_HDR_BYTES + 4u)) goto out;
    buf = (uint8_t *)malloc((size_t)st.st_size);
    if (buf == NULL) goto out;
    if (pread(fd, buf, (size_t)st.st_size, 0) != st.st_size) goto out;
    if (get_u32(buf) != SFB_MAGIC_SEGMENT) goto out;

    if (m != NULL) {
        m->records = get_u32(buf + 12);
        m->dim = get_u32(buf + 8);
        m->flags = (uint16_t)((uint16_t)buf[6] | ((uint16_t)buf[7] << 8));
        m->payload_len = get_u64(buf + 24);
        if ((m->flags & (SFB_SEG_FLAG_SEALED | SFB_SEG_FLAG_INT8)) != 0)
            goto out;                       /* not the f32 plaintext layout   */
    }
    if (loc == NULL) { rc = 0; goto out; }

    {
        uint32_t dim = get_u32(buf + 8);

        count = get_u32(buf + SEG_HDR_BYTES);
        off = SEG_HDR_BYTES + 4u;
        for (i = 0; i < count; i++) {
            uint32_t id, tl, rl;
            size_t need;

            if ((size_t)off + REC_HDR_BYTES > (size_t)st.st_size) goto out;
            id = get_u32(buf + off + REC_OFF_ID);
            tl = get_u32(buf + off + REC_OFF_TL);
            rl = get_u32(buf + off + REC_OFF_RL);
            need = (size_t)REC_HDR_BYTES + (size_t)tl + (size_t)rl + (size_t)dim * 4u;
            if ((size_t)off + need > (size_t)st.st_size) goto out;
            if (id == want_id) {
                loc->id = id;
                loc->text_len = tl;
                loc->role_len = rl;
                loc->text_off = (off_t)((size_t)off + REC_OFF_TEXT + REC_HDR_BYTES);
                loc->vec_off = (off_t)((size_t)off + REC_HDR_BYTES + tl + rl);
                rc = 1;
                goto out;
            }
            off += (uint32_t)need;
        }
        rc = 0;
    }

out:
    if (fd >= 0) (void)close(fd);
    free(buf);
    return rc;
}

/* Flip one byte in place — the "one damaged bit" every W3.5/W3.3 case is about. */
static int flip_byte(const char *path, off_t off, uint8_t mask)
{
    uint8_t b;
    int fd = open(path, O_RDWR);

    if (fd < 0) return -1;
    if (pread(fd, &b, 1, off) != 1) { (void)close(fd); return -1; }
    b = (uint8_t)(b ^ mask);
    if (pwrite(fd, &b, 1, off) != 1) { (void)close(fd); return -1; }
    (void)close(fd);
    return 0;
}

/* Drop `n` bytes off the end of a file: the "torn write" a crash leaves. */
static int truncate_tail(const char *path, off_t n)
{
    struct stat st;

    if (stat(path, &st) != 0) return -1;
    if (st.st_size <= n) return -1;
    return truncate(path, st.st_size - n);
}

/* Read one integer field out of a verify/stats JSON blob, e.g. "damaged_segments". */
static int json_contains(const char *json, const char *needle)
{
    return json != NULL && strstr(json, needle) != NULL;
}

/* ── brain helpers ────────────────────────────────────────────────────── */

/* One cfg for every store this file builds.  decay_floor is tiny so that a
 * decay_tick DIRTIES rows without ever demoting them to FORGOTTEN — the W3.2
 * and W3.4 tests are about what a large dirty set does to the files on disk,
 * not about forgetting. */
static int brain_open_at(const char *path, uint32_t dim, int verify_full,
                         sfb_brain_t **out)
{
    sfb_config_t cfg;

    sfb_config_default(&cfg, path, dim);
    cfg.decay_floor = 1e-30;
    cfg.verify_full = verify_full;
    return sfb_open(&cfg, out);
}

/* Remember `n` rows with unique text and well-separated vectors (so neither
 * dedup rule can fold them) and return the ids in `ids`.
 * NOTE: sfb_remember() auto-flushes at SFB_SEGMENT_CAP dirty rows
 * (sfb_brain.c:1979-1985), so seeding more than 256 rows already produces
 * several segments before the caller flushes anything. */
static int seed_rows(sfb_brain_t *b, uint32_t dim, uint32_t n, const char *tag,
                     uint32_t *ids)
{
    char text[192];
    float *v = (float *)malloc((size_t)dim * sizeof(float));
    uint32_t i;
    int rc = SFB_OK;

    if (v == NULL) return SFB_E_NOMEM;
    for (i = 0; i < n; i++) {
        sfb_remember_result_t rr;

        fill_vec(v, dim, i + 1u);
        snprintf(text, sizeof text, "row %s %06u token %06u", tag, (unsigned)i,
                 (unsigned)(i * 7u + 1u));
        memset(&rr, 0, sizeof rr);
        rc = sfb_remember_ex(b, v, (size_t)dim, text, "user", &rr);
        if (rc != SFB_OK) break;
        if (ids != NULL) ids[i] = rr.id;
    }
    free(v);
    return rc;
}

/* ── W3.5: per-record CRC isolates damage to one record ───────────────────
 * Pins the writer (sfb_store.c:808-812 puts SFB_SEG_FLAG_RECCRC_FULL into the
 * segment header; sfb_brain.c:3006-3010 asks for it, and
 * sfb_brain.c:1150-1195 computes the whole-record CRC), the reader
 * (sfb_brain.c:1348-1400 recomputes it over the whole record and
 * sfb_brain.c:1466-1473 counts + reports the drop) and the store's tolerance
 * for a non-structural mismatch (sfb_store_segment_read_damaged,
 * sfb_store.c:1046-1060 / sfb_internal.h:242-249).
 * ---------------------------------------------------------------------- */

#define W35_DIM   64u
#define W35_N     512u

/* Build a plaintext f32 store of W35_N rows: two sealed segments of 256. */
static int build_w35_store(const char *path, char names[][256], int max,
                           int *nfiles_out)
{
    sfb_brain_t *b = NULL;
    sfb_flush_result_t fr;
    char sd[1024];
    int rc, nf;

    rc = brain_open_at(path, W35_DIM, 0, &b);
    if (rc != SFB_OK) return rc;
    rc = seed_rows(b, W35_DIM, W35_N, "w35", NULL);
    if (rc == SFB_OK) rc = sfb_flush(b, &fr);
    if (rc != SFB_OK) { (void)sfb_close(b); return rc; }

    if (seg_dir_of(sd, sizeof sd, path) != 0) { (void)sfb_close(b); return SFB_E_INVALID; }
    nf = seg_files(sd, names, max);
    if (nfiles_out != NULL) *nfiles_out = nf;
    return sfb_close(b);
}

/* ── (a) one flipped bit in a record's VECTOR ─────────────────────────── */

static void test_w35_vector_bit(void)
{
    char path[512], full[4096], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    seg_meta_t meta;
    rec_loc_t loc;
    sfb_stats_t st;
    int nf = 0, scan, rc;

    printf("\n── W3.5(a) one damaged VECTOR byte costs exactly one record\n");
    snprintf(path, sizeof path, "%s/w35a", g_root);
    rc = build_w35_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 2,
          "W3.5(a) fixture: 512 rows sealed into 2 segments of 256", detail);
    if (rc != SFB_OK || nf < 1) return;

    /* The writer must have declared the whole-record CRC span.  Without this
     * bit replay would check head+text+role only and a vector flip would be
     * silently imported as a wrong vector. */
    if (seg_full(full, sizeof full, path, names[0]) != 0) return;
    scan = seg_scan(full, 0, &meta, NULL);
    snprintf(detail, sizeof detail,
             "flag word 0x%04x on %s (bit3 0x%04x is RECCRC_FULL)",
             (unsigned)meta.flags, names[0], (unsigned)SFB_SEG_FLAG_RECCRC_FULL);
    check(scan == 0 && (meta.flags & SFB_SEG_FLAG_RECCRC_FULL) != 0,
          "W3.5(a) the writer sets SFB_SEG_FLAG_RECCRC_FULL on what it seals",
          detail);

    scan = seg_scan(full, 100u, &meta, &loc);
    snprintf(detail, sizeof detail, "seg_scan=%d id=%u vec_off=%lld dim=%u",
             scan, loc.id, (long long)loc.vec_off, (unsigned)meta.dim);
    check(scan == 1 && loc.id == 100u, "W3.5(a) harness located record 100", detail);
    if (scan != 1) return;

    /* Byte 8 of the vector payload: inside the f32 row, 4-aligned, far from
     * the head the legacy CRC already covered. */
    check(flip_byte(full, loc.vec_off + 8, 0x01) == 0,
          "W3.5(a) flipped one bit inside record 100's vector payload", full);

    rc = brain_open_at(path, W35_DIM, 0, &b);
    snprintf(detail, sizeof detail, "sfb_open returned %s", rc_name(rc));
    check(rc == SFB_OK,
          "W3.5(a) the store still opens after one damaged vector byte", detail);
    if (rc != SFB_OK) return;

    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail,
             "live=%llu of %u, expected %u — the flipped record is dropped, "
             "the other %u rows import",
             (unsigned long long)st.live, W35_N, W35_N - 1u, W35_N - 1u);
    check(st.live == (uint64_t)W35_N - 1u,
          "W3.5(a) exactly one record is lost, not the whole segment", detail);

    snprintf(detail, sizeof detail, "records_dropped=%llu quarantined=%llu",
             (unsigned long long)st.records_dropped,
             (unsigned long long)st.segments_quarantined);
    check(st.records_dropped == 1u,
          "W3.5(a) the drop is counted in records_dropped", detail);
    check(st.segments_quarantined == 0u,
          "W3.5(a) the segment is NOT quarantined (damage stayed record-local)",
          detail);

    {
        sfb_hit_t *hits = (sfb_hit_t *)calloc(W35_N + 1u, sizeof *hits);
        uint32_t n = 0;
        int found_bad = 0, i;

        if (hits != NULL) {
            (void)sfb_list(b, 0, W35_N + 1u, hits, &n);
            for (i = 0; i < (int)n; i++)
                if (hits[i].id == 100u) found_bad = 1;
        }
        snprintf(detail, sizeof detail,
                 "sfb_list returned %u live rows, id 100 %s",
                 n, found_bad ? "STILL PRESENT" : "absent");
        check(hits != NULL && n == W35_N - 1u && !found_bad,
              "W3.5(a) the live set is the 511 undamaged rows (id 100 gone)",
              detail);
        free(hits);
    }
    (void)sfb_close(b);
}

/* ── (b) one flipped bit in a record's TEXT ───────────────────────────── */

static void test_w35_text_bit(void)
{
    char path[512], full[4096], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    rec_loc_t loc;
    seg_meta_t meta;
    sfb_stats_t st;
    int nf = 0, scan, rc;

    printf("\n── W3.5(b) one damaged TEXT byte costs exactly one record\n");
    snprintf(path, sizeof path, "%s/w35b", g_root);
    rc = build_w35_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 2, "W3.5(b) fixture: 2 segments of 256 rows",
          detail);
    if (rc != SFB_OK || nf < 2) return;

    if (seg_full(full, sizeof full, path, names[1]) != 0) return;
    scan = seg_scan(full, 300u, &meta, &loc);
    snprintf(detail, sizeof detail, "seg_scan=%d id=%u text_off=%lld",
             scan, loc.id, (long long)loc.text_off);
    check(scan == 1 && loc.id == 300u && loc.text_len > 0u,
          "W3.5(b) harness located record 300 in the second segment", detail);
    if (scan != 1) return;

    check(flip_byte(full, loc.text_off + 2, 0x01) == 0,
          "W3.5(b) flipped one bit inside record 300's text", full);

    rc = brain_open_at(path, W35_DIM, 0, &b);
    check(rc == SFB_OK, "W3.5(b) the store still opens", rc_name(rc));
    if (rc != SFB_OK) return;

    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail, "live=%llu of %u, records_dropped=%llu, "
             "quarantined=%llu",
             (unsigned long long)st.live, W35_N,
             (unsigned long long)st.records_dropped,
             (unsigned long long)st.segments_quarantined);
    check(st.live == (uint64_t)W35_N - 1u && st.records_dropped == 1u,
          "W3.5(b) a damaged text byte costs exactly one record", detail);
    check(st.segments_quarantined == 0u,
          "W3.5(b) the segment survives; only the record is dropped", detail);
    (void)sfb_close(b);
}

/* ── (c) a torn segment still falls as a unit ─────────────────────────── */

static void test_w35_torn_segment(void)
{
    char path[512], full[4096], sd[1024], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    sfb_stats_t st;
    int nf = 0, rc;

    printf("\n── W3.5(c) a truncated segment is rejected as a unit, the rest imports\n");
    snprintf(path, sizeof path, "%s/w35c", g_root);
    rc = build_w35_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 2, "W3.5(c) fixture: 2 segments of 256 rows",
          detail);
    if (rc != SFB_OK || nf < 2) return;

    /* Truncate the OLDER segment: the newest one is the one a normal open
     * length-probes (W3.3(c) covers that case separately). */
    if (seg_full(full, sizeof full, path, names[0]) != 0) return;
    check(truncate_tail(full, 40) == 0,
          "W3.5(c) truncated 40 bytes off the older segment (torn write)", full);

    rc = brain_open_at(path, W35_DIM, 0, &b);
    snprintf(detail, sizeof detail, "sfb_open returned %s", rc_name(rc));
    check(rc == SFB_OK,
          "W3.5(c) the store still opens with the surviving segment", detail);
    if (rc != SFB_OK) return;

    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail,
             "live=%llu (expected 256 = the intact segment), "
             "quarantined=%llu dropped=%llu",
             (unsigned long long)st.live,
             (unsigned long long)st.segments_quarantined,
             (unsigned long long)st.records_dropped);
    check(st.live == 256u,
          "W3.5(c) the torn segment's 256 rows are gone, the other 256 survive",
          detail);
    check(st.segments_quarantined == 1u,
          "W3.5(c) the torn segment was quarantined as a unit", detail);
    (void)sfb_close(b);

    {
        char final_name[256], quarantine_name[300];

        if (seg_name(final_name, sizeof final_name, 1u, 1u) != 0) return;
        snprintf(quarantine_name, sizeof quarantine_name, "%s.quarantine",
                 final_name);
        if (path_join(sd, sizeof sd, path, SFB_SEG_DIR) != 0) return;
        {
            char a[4096], q[4096];

            (void)path_join(a, sizeof a, sd, final_name);
            (void)path_join(q, sizeof q, sd, quarantine_name);
            snprintf(detail, sizeof detail, "%s exists=%d, %s exists=%d",
                     final_name, file_exists(a), quarantine_name, file_exists(q));
            check(!file_exists(a) && file_exists(q),
                  "W3.5(c) the damaged bytes were preserved under .quarantine",
                  detail);
        }
    }
}

/* ── W3.2: 256-record segment cap ─────────────────────────────────────────
 * Pins sfb_brain.c:2986-3009 (flush loops sfb_brain_serialize_dirty_n with
 * SFB_SEGMENT_CAP) and sfb_brain.c:1110-1122 (the cap is enforced in
 * serialisation order), plus SFB_SEGMENT_CAP itself (sfb_internal.h:41).
 * A decay_tick is the operation used here because remember() self-flushes at
 * 256 (sfb_brain.c:1979), so only an in-place dirtying can build a >256 dirty
 * set for one flush to split.
 * ---------------------------------------------------------------------- */

#define W32_DIM  64u
#define W32_N    1000u
/* The W3.2 contract number, deliberately spelled out instead of read from
 * SFB_SEGMENT_CAP: deriving the expectation from the constant under test would
 * make the test agree with any value the constant is changed to. */
#define W32_CAP  256u
#define W32_SEGS 4u

static void test_w32_segment_cap(void)
{
    char path[512], sd[1024], names[64][256], detail[256];
    sfb_brain_t *b = NULL;
    sfb_flush_result_t fr;
    sfb_stats_t st;
    uint64_t total = 0, live = 0;
    uint32_t i;
    int nf = 0, rc, over = 0, idx_232 = -1;

    printf("\n── W3.2 a 1000-row dirty set splits at %u records per segment\n",
           (unsigned)W32_CAP);
    snprintf(path, sizeof path, "%s/w32", g_root);
    rc = brain_open_at(path, W32_DIM, 0, &b);
    check(rc == SFB_OK, "W3.2 fixture open", rc_name(rc));
    if (rc != SFB_OK) return;

    rc = seed_rows(b, W32_DIM, W32_N, "w32", NULL);
    check(rc == SFB_OK, "W3.2 seeded 1000 rows", rc_name(rc));
    if (rc != SFB_OK) { (void)sfb_close(b); return; }

    /* One tick over the whole corpus: every live row's strength changes, so
     * all 1000 rows are dirty at once and a single flush has to place them. */
    (void)sfb_decay_tick(b, 60);
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    snprintf(detail, sizeof detail,
             "sfb_flush wrote %llu records in %llu segments (want %u in %u)",
             (unsigned long long)fr.records_written,
             (unsigned long long)fr.segments_sealed, W32_N, W32_SEGS);
    check(rc == SFB_OK && fr.records_written == (uint64_t)W32_N &&
              fr.segments_sealed == (uint64_t)W32_SEGS,
          "W3.2 one flush of 1000 dirty rows seals 4 segments of <=256", detail);

    /* The manifest is the durable truth: every entry it references must obey
     * the cap, and their record counts must add up to the corpus exactly. */
    for (i = 0; i < b->store.man.nseg; i++) {
        const sfb_seg_entry_t *e = &b->store.man.segs[i];

        total += e->records;
        if (e->records > W32_CAP) over++;
        if (e->records == 232u) idx_232 = (int)i;
    }
    snprintf(detail, sizeof detail,
             "manifest: %u segments, %llu records total, %d over the cap",
             b->store.man.nseg, (unsigned long long)total, over);
    check(over == 0,
          "W3.2 no manifest-referenced segment holds more than 256 records",
          detail);
    check(total == (uint64_t)W32_N && b->store.man.nseg == W32_SEGS,
          "W3.2 the manifest references exactly the 1000 rows in 4 segments",
          detail);
    check(idx_232 >= 0,
          "W3.2 the split is 256/256/256/232 (the remainder segment exists)",
          detail);

    /* And the segment FILES agree with the manifest, read from their own
     * 48-byte headers rather than from the manifest struct. */
    if (seg_dir_of(sd, sizeof sd, path) != 0) { (void)sfb_close(b); return; }
    nf = seg_files(sd, names, 64);
    {
        int bad = 0;
        char worst[128] = "";

        for (i = 0; i < (uint32_t)nf; i++) {
            char full[4096];
            seg_meta_t meta;

            if (path_join(full, sizeof full, sd, names[i]) != 0) continue;
            if (seg_scan(full, 0, &meta, NULL) != 0) { bad++; continue; }
            if (meta.records > W32_CAP) {
                bad++;
                snprintf(worst, sizeof worst, "%s holds %u records",
                         names[i], (unsigned)meta.records);
            }
        }
        snprintf(detail, sizeof detail, "%d segment files, %d offending%s%s",
                 nf, bad, worst[0] ? ": " : "", worst);
        check(nf == (int)W32_SEGS && bad == 0,
              "W3.2 every sealed segment file's own header respects the cap",
              detail);
    }
    (void)sfb_close(b);

    /* The split must be invisible to a reader. */
    b = NULL;
    rc = brain_open_at(path, W32_DIM, 0, &b);
    if (rc == SFB_OK) {
        (void)sfb_stats_get(b, &st);
        (void)sfb_count(b, &live);
        snprintf(detail, sizeof detail, "reopen live=%llu count=%llu (want %u)",
                 (unsigned long long)st.live, (unsigned long long)live, W32_N);
        check(st.live == (uint64_t)W32_N && live == (uint64_t)W32_N,
              "W3.2 a reopen returns all 1000 rows from the split segments",
              detail);

        /* The other writer that places the whole corpus: sfb_compact() walks
         * the live set SFB_SEGMENT_CAP rows at a time (sfb_brain.c:3126-3155),
         * so a compact must not be able to reintroduce an oversized segment. */
        rc = sfb_compact(b, NULL, NULL);
        total = 0;
        over = 0;
        for (i = 0; i < b->store.man.nseg; i++) {
            total += b->store.man.segs[i].records;
            if (b->store.man.segs[i].records > W32_CAP) over++;
        }
        snprintf(detail, sizeof detail,
                 "sfb_compact rc=%s: %u segments, %llu records, %d over %u",
                 rc_name(rc), b->store.man.nseg, (unsigned long long)total,
                 over, (unsigned)W32_CAP);
        check(rc == SFB_OK && over == 0 && total == (uint64_t)W32_N &&
                  b->store.man.nseg == W32_SEGS,
              "W3.2 sfb_compact also splits the corpus at the cap", detail);
        (void)sfb_close(b);
    } else {
        check(0, "W3.2 a reopen returns all 1000 rows from the split segments",
              rc_name(rc));
    }
}

/* ── W3.4: automatic reclamation ─────────────────────────────────────────
 * Pins sfb_brain.c:3030-3053 (a publish that supersedes the whole corpus drops
 * the old manifest entries and calls sfb_store_reclaim_request) and
 * sfb_brain.c:3073-3078 (flush runs sfb_store_reclaim_auto), backed by
 * sfb_store.c:1280-1478 (the sweep) and sfb_store.c:1466-1486 (only a
 * requested sweep runs).  sfb_compact() is never called by this test.
 * ---------------------------------------------------------------------- */

#define W34_DIM    512u   /* vectors dominate the graph file's ~412 B/node     */
#define W34_N      200u
#define W34_CYCLES 5

static void test_w34_auto_reclaim(void)
{
    char path[512], sd[1024], detail[256];
    sfb_brain_t *b = NULL;
    sfb_stats_t st;
    uint64_t disk_cycle1 = 0, disk_cycle5 = 0;
    uint32_t i;
    int rc, cycle;

    printf("\n── W3.4 superseded generations are reclaimed without sfb_compact\n");
    snprintf(path, sizeof path, "%s/w34", g_root);
    rc = brain_open_at(path, W34_DIM, 0, &b);
    check(rc == SFB_OK, "W3.4 fixture open", rc_name(rc));
    if (rc != SFB_OK) return;

    rc = seed_rows(b, W34_DIM, W34_N, "w34", NULL);
    if (rc != SFB_OK) { check(0, "W3.4 seeded 200 rows", rc_name(rc)); (void)sfb_close(b); return; }
    if (sfb_flush(b, NULL) != SFB_OK) {
        check(0, "W3.4 initial flush", NULL);
        (void)sfb_close(b);
        return;
    }

    for (cycle = 0; cycle < W34_CYCLES; cycle++) {
        (void)sfb_decay_tick(b, 60);
        rc = sfb_flush(b, NULL);
        if (rc != SFB_OK) break;
        if (cycle == 0) disk_cycle1 = dir_bytes(path);
        if (cycle == W34_CYCLES - 1) disk_cycle5 = dir_bytes(path);
    }
    snprintf(detail, sizeof detail, "5 cycles finished, sfb_flush rc=%s",
             rc_name(rc));
    check(rc == SFB_OK, "W3.4 five decay_tick+flush cycles ran clean", detail);

    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail, "live=%llu (want %u), segments=%llu",
             (unsigned long long)st.live, W34_N,
             (unsigned long long)st.segments);
    check(st.live == (uint64_t)W34_N,
          "W3.4 the corpus survived every cycle", detail);

    /* Every cycle rewrote the whole corpus, so the steady state is ONE segment
     * and nothing else unless reclamation failed. */
    snprintf(detail, sizeof detail,
             "manifest references %llu segments for %llu rows (want 1 for %u)",
             (unsigned long long)st.segments, (unsigned long long)st.live, W34_N);
    check(st.segments == 1u,
          "W3.4 each publish supersedes the previous generation", detail);

    {
        uint64_t total = dir_bytes(path);
        double amp = st.bytes_records ? (double)total / (double)st.bytes_records : 0.0;

        snprintf(detail, sizeof detail,
                 "%.2fx = %llu B on disk / %llu B of live records "
                 "(includes the HNSW graph)",
                 amp, (unsigned long long)total,
                 (unsigned long long)st.bytes_records);
        check(amp > 0.0 && amp < 1.5,
              "W3.4 disk amplification stays under 1.5x without sfb_compact",
              detail);
    }

    /* The direct proof that nothing accumulates: cycle 5 costs no more disk
     * than cycle 1 did.  With a broken sweep this ratio is ~5x. */
    snprintf(detail, sizeof detail, "cycle1=%llu B, cycle5=%llu B",
             (unsigned long long)disk_cycle1, (unsigned long long)disk_cycle5);
    check(disk_cycle1 > 0u && disk_cycle5 <= disk_cycle1 + disk_cycle1 / 20u,
          "W3.4 disk use does not grow with the number of cycles", detail);

    /* (b) an orphan .tmp must be collected by the same sweep.  Its generation
     * (1) is older than the manifest's, so sfb_store.c:1355-1360 collects it
     * regardless of age — no 300 s wait. */
    {
        char segtmp[256], segtmp_path[4096], graph_path[4096];
        int made = 0;

        if (seg_dir_of(sd, sizeof sd, path) == 0) {
            /* seg-000001-000001.qtsq.tmp — the leftover an interrupted
             * sfb_store_segment_write_ex() leaves (sfb_store.c:861). */
            snprintf(segtmp, sizeof segtmp, "seg-%06u-%06u.qtsq.tmp", 1u, 1u);
            if (path_join(segtmp_path, sizeof segtmp_path, sd, segtmp) == 0) {
                int fd = open(segtmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

                if (fd >= 0) { (void)write(fd, "orphan", 6); (void)close(fd); made++; }
            }
        }
        if (path_join(graph_path, sizeof graph_path, path, "graph-1.bin.tmp") == 0) {
            int fd = open(graph_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (fd >= 0) { (void)write(fd, "orphan", 6); (void)close(fd); made++; }
        }
        check(made == 2, "W3.4(b) planted two orphan .tmp files", NULL);
        snprintf(detail, sizeof detail, "%s=%d %s=%d", segtmp_path,
                 file_exists(segtmp_path), graph_path, file_exists(graph_path));
        check(made == 2 && file_exists(segtmp_path) && file_exists(graph_path),
              "W3.4(b) both orphans exist before the sweep", detail);

        (void)sfb_decay_tick(b, 60);
        rc = sfb_flush(b, NULL);

        snprintf(detail, sizeof detail, "flush rc=%s; seg-dir orphan exists=%d, "
                 "brain-dir orphan exists=%d",
                 rc_name(rc), file_exists(segtmp_path), file_exists(graph_path));
        check(rc == SFB_OK && !file_exists(segtmp_path) && !file_exists(graph_path),
              "W3.4(b) a flush sweeps the orphan .tmp files away", detail);
    }

    /* (c) everything the manifest references must still be there. */
    {
        int missing = 0;

        for (i = 0; i < b->store.man.nseg; i++) {
            char name[256], full[4096];

            if (seg_name(name, sizeof name, b->store.man.segs[i].gen,
                         b->store.man.segs[i].seq) != 0) continue;
            if (path_join(full, sizeof full, b->store.seg_dir, name) != 0) continue;
            if (!file_exists(full)) missing++;
        }
        snprintf(detail, sizeof detail, "%d of %u manifest segments are missing",
                 missing, b->store.man.nseg);
        check(missing == 0,
              "W3.4(c) every manifest-referenced segment still exists", detail);
    }
    (void)sfb_close(b);

    {
        char json[8192];

        memset(json, 0, sizeof json);
        rc = sfb_verify(path, NULL, 0, json, sizeof json);
        snprintf(detail, sizeof detail, "sfb_verify rc=%s %.160s",
                 rc_name(rc), json);
        check(rc == SFB_OK && json_contains(json, "\"ok\":true"),
              "W3.4(c) sfb_verify passes after five automatic sweeps", detail);
    }
}

/* ── W3.3: lazy open ─────────────────────────────────────────────────────
 * Pins sfb_internal.h:209-213 and sfb_store.c:1050-1058: `segments_read` counts
 * every segment payload read and is incremented in exactly ONE place — the
 * payload read inside seg_read_impl() — so "0 reads after open" is precisely
 * "no segment bytes were read".  A lazy open must leave it at 0; cfg.verify_full
 * (sfb_brain.c:1512-1520) must read and check every payload and quarantine
 * damage at open; and a torn NEWEST segment must still fail the open
 * (sfb_brain.c:1735-1752 via sfb_store_segment_probe, sfb_store.c:1130-1145).
 *
 * LIMITATION, clearly marked: `segments_verified` is NOT usable as the second
 * half of this proof.  The paranoid path calls sfb_store_segment_verify()
 * (sfb_store.c:1146-1225), which does the payload-CRC + whole-file-digest check
 * but never increments the counter; the only increment (sfb_store.c:1054) sits
 * in the strict reader, whose sole caller is the encrypted-store passphrase
 * probe (sfb_store.c:684).  So `segments_verified` reads 0 even after a
 * verify_full open that genuinely verified every segment.  This test therefore
 * proves the lazy half with the counter and the paranoid half behaviourally
 * (record-level drop vs segment-level quarantine on identical damage).
 * ---------------------------------------------------------------------- */

#define W33_DIM  256u
#define W33_N    640u    /* 256 + 256 + 128 -> three segments              */

static int build_w33_store(const char *path, char names[][256], int max,
                           int *nf_out)
{
    sfb_brain_t *b = NULL;
    char sd[1024];
    int rc, nf;

    rc = brain_open_at(path, W33_DIM, 0, &b);
    if (rc != SFB_OK) return rc;
    rc = seed_rows(b, W33_DIM, W33_N, "w33", NULL);
    if (rc == SFB_OK) rc = sfb_flush(b, NULL);
    if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
    if (seg_dir_of(sd, sizeof sd, path) != 0) { (void)sfb_close(b); return SFB_E_INVALID; }
    nf = seg_files(sd, names, max);
    if (nf_out != NULL) *nf_out = nf;
    return sfb_close(b);
}

/* ── (a) a normal open must not read every segment ────────────────────── */

static void test_w33_lazy_open(void)
{
    char path[512], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    uint64_t normal_read = 0, normal_verified = 0;
    int nf = 0, rc;

    printf("\n── W3.3(a) open is lazy; verify_full is the paranoid path\n");
    snprintf(path, sizeof path, "%s/w33a", g_root);
    rc = build_w33_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 3, "W3.3(a) fixture: 640 rows in 3 segments",
          detail);
    if (rc != SFB_OK || nf != 3) return;

    rc = brain_open_at(path, W33_DIM, 0, &b);
    check(rc == SFB_OK, "W3.3(a) normal open succeeds", rc_name(rc));
    if (rc != SFB_OK) return;
    normal_read = b->store.segments_read;
    normal_verified = b->store.segments_verified;
    snprintf(detail, sizeof detail,
             "segments_read=%llu of %d segment payloads (want 0), "
             "segments_verified=%llu",
             (unsigned long long)normal_read, nf,
             (unsigned long long)normal_verified);
    check(normal_read == 0u,
          "W3.3(a) a normal open reads ZERO segment payloads", detail);
    check(normal_verified == 0u,
          "W3.3(a) a normal open verifies no payload CRC or digest", detail);
    (void)sfb_close(b);
    b = NULL;

    /* The counter must be alive: under verify_full the same store is read in
     * full.  Without this, a permanently-zero counter would make the check
     * above vacuous. */
    rc = brain_open_at(path, W33_DIM, 1, &b);
    check(rc == SFB_OK, "W3.3(a) verify_full open succeeds", rc_name(rc));
    if (rc == SFB_OK) {
        snprintf(detail, sizeof detail, "segments_read=%llu of %d",
                 (unsigned long long)b->store.segments_read, nf);
        check(b->store.segments_read == (uint64_t)nf,
              "W3.3(a) verify_full reads every payload (the counter is alive)",
              detail);
        (void)sfb_close(b);
        b = NULL;
    }
}

/* ── (b) a flipped byte is reported by verify and by a read ───────────── */

static void test_w33_damage_reporting(void)
{
    char path[512], full[4096], detail[512];
    char names[64][256];
    sfb_brain_t *b = NULL;
    sfb_stats_t st;
    rec_loc_t loc;
    seg_meta_t meta;
    char json[8192];
    int nf = 0, scan, rc;

    printf("\n── W3.3(b) a flipped byte is reported by sfb_verify and by a read\n");
    snprintf(path, sizeof path, "%s/w33b", g_root);
    rc = build_w33_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 3, "W3.3(b) fixture: 640 rows in 3 segments",
          detail);
    if (rc != SFB_OK || nf < 3) return;

    /* Damage the NEWEST segment: a length probe cannot see this, so it is
     * exactly the damage the verify path exists for. */
    if (seg_full(full, sizeof full, path, names[nf - 1]) != 0) return;
    scan = seg_scan(full, (uint32_t)(W33_N - 10u), &meta, &loc);
    snprintf(detail, sizeof detail, "seg_scan=%d id=%u text_off=%lld",
             scan, loc.id, (long long)loc.text_off);
    check(scan == 1, "W3.3(b) harness located a record in the newest segment",
          detail);
    if (scan != 1) return;
    check(flip_byte(full, loc.text_off + 1, 0x04) == 0,
          "W3.3(b) flipped one bit in the newest segment's payload", full);

    memset(json, 0, sizeof json);
    rc = sfb_verify(path, NULL, 0, json, sizeof json);
    snprintf(detail, sizeof detail, "sfb_verify rc=%s %.240s", rc_name(rc), json);
    check(rc != SFB_OK && json_contains(json, "\"ok\":false") &&
              !json_contains(json, "\"damaged_segments\":0"),
          "W3.3(b) sfb_verify names the damaged segment", detail);

    rc = brain_open_at(path, W33_DIM, 0, &b);
    if (rc == SFB_OK) {
        (void)sfb_stats_get(b, &st);
        snprintf(detail, sizeof detail,
                 "live=%llu of %u, records_dropped=%llu, quarantined=%llu",
                 (unsigned long long)st.live, W33_N,
                 (unsigned long long)st.records_dropped,
                 (unsigned long long)st.segments_quarantined);
        check(st.live == (uint64_t)W33_N - 1u,
              "W3.3(b) the read that touches the damage drops the bad record",
              detail);
        (void)sfb_close(b);
    } else {
        check(0, "W3.3(b) the read that touches the damage drops the bad record",
              rc_name(rc));
    }
}

/* ── (d) verify_full on the same damage: paranoid, segment-level ──────── */

static void test_w33_verify_full_paranoid(void)
{
    char path[512], full[4096], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    sfb_stats_t st;
    rec_loc_t loc;
    seg_meta_t meta;
    int nf = 0, scan, rc;

    printf("\n── W3.3(a) cfg.verify_full restores the paranoid (segment-level) path\n");
    snprintf(path, sizeof path, "%s/w33d", g_root);
    rc = build_w33_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 3, "W3.3(d) fixture: 640 rows in 3 segments",
          detail);
    if (rc != SFB_OK || nf < 3) return;

    if (seg_full(full, sizeof full, path, names[nf - 1]) != 0) return;
    scan = seg_scan(full, (uint32_t)(W33_N - 10u), &meta, &loc);
    check(scan == 1, "W3.3(d) harness located a record in the newest segment",
          NULL);
    if (scan != 1) return;
    check(flip_byte(full, loc.text_off + 1, 0x04) == 0,
          "W3.3(d) flipped the same single bit as (b)", full);

    /* Same damage, paranoid open: sfb_brain.c:1512-1520 verifies every segment
     * before decoding and quarantines the damaged one as a UNIT, so the whole
     * 128-record tail segment is lost instead of one record. */
    rc = brain_open_at(path, W33_DIM, 1, &b);
    check(rc == SFB_OK, "W3.3(d) verify_full open still succeeds",
          rc_name(rc));
    if (rc != SFB_OK) return;
    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail,
             "quarantined=%llu live=%llu (expected the 128-row newest segment "
             "quarantined, leaving %u)",
             (unsigned long long)st.segments_quarantined,
             (unsigned long long)st.live, W33_N - 128u);
    check(st.segments_quarantined == 1u && st.live == (uint64_t)W33_N - 128u,
          "W3.3(d) verify_full quarantines the damaged segment at open", detail);
    (void)sfb_close(b);
}

/* ── (c) a torn NEWEST segment fails the open ─────────────────────────── */

static void test_w33_torn_newest(void)
{
    char path[512], full[4096], detail[256];
    char names[64][256];
    sfb_brain_t *b = NULL;
    int nf = 0, rc;

    printf("\n── W3.3(c) a torn NEWEST segment fails the open loudly\n");
    snprintf(path, sizeof path, "%s/w33c", g_root);
    rc = build_w33_store(path, names, 64, &nf);
    snprintf(detail, sizeof detail, "%s, %d segment files", rc_name(rc), nf);
    check(rc == SFB_OK && nf == 3, "W3.3(c) fixture: 640 rows in 3 segments",
          detail);
    if (rc != SFB_OK || nf < 3) return;

    if (seg_full(full, sizeof full, path, names[nf - 1]) != 0) return;
    check(truncate_tail(full, 64) == 0,
          "W3.3(c) truncated the newest segment by 64 bytes", full);

    rc = brain_open_at(path, W33_DIM, 0, &b);
    snprintf(detail, sizeof detail, "sfb_open returned %s, *out %s",
             rc_name(rc), b == NULL ? "left NULL" : "non-NULL");
    check(rc == SFB_E_CORRUPT && b == NULL,
          "W3.3(c) a torn newest segment fails the open (SFB_E_CORRUPT)", detail);
    if (b != NULL) (void)sfb_close(b);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char tmpl[] = "/tmp/sfb_w3_storage.XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    printf("── W3.2/W3.3/W3.4/W3.5 storage tests (tmp dir %s)\n", g_root);

    test_w35_vector_bit();
    test_w35_text_bit();
    test_w35_torn_segment();
    test_w32_segment_cap();
    test_w34_auto_reclaim();
    test_w33_lazy_open();
    test_w33_damage_reporting();
    test_w33_verify_full_paranoid();
    test_w33_torn_newest();

    rm_rf(g_root);
    printf("── %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("W3 STORAGE TESTS FAILED\n");
        return 1;
    }
    printf("ALL W3 STORAGE TESTS PASSED\n");
    return 0;
}
