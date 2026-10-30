/*
 * test_w7.c — W7.3 / W7.4 / W7.5: the reports must tell the truth.
 *
 * Defect 1 (W7.4): `sfb stats --json` used to answer ok:true, records_dropped:0
 *   on a store whose manifest references a deleted segment — exactly the case it
 *   exists for — reported `generations` as one scalar with no history, and
 *   printed flushes/flush_last_us as 0 because those are per-process counters a
 *   cold `sfb stats` has never touched. This suite asserts:
 *     - two flushes produce a real generation history (2 rows, correct counts);
 *     - a healthy store is still ok:true with the same counts verify reports;
 *     - a deleted sealed segment makes stats not-ok with a non-zero dropped
 *       count and a missing_files entry that names the segment;
 *     - a same-size bit flip (damage without a size change) also makes it
 *       not-ok, so the check is not just "does the file exist";
 *     - flushes/flush_last_us are JSON null (unknown), never a fake 0, while
 *       open_us is a real measurement.
 *
 * Defect 2 (W7.5): `sfb migrate` printed "(already v2, nothing re-encoded)" even
 *   after a real v1 import, and only ever wrote its report to stderr. This suite
 *   builds a genuine v1 container with qtsq_container_pack_write() (the same
 *   recipe tests/test_migrate.c uses), runs the library report and the CLI, and
 *   asserts the import message, the --json report and the already-v2 wording.
 *
 * Defect 3 (W7.3): backup silently fell back from link() to a byte copy. This
 *   suite asserts the report's link/copy counts add up to the files backed up,
 *   that a same-filesystem snapshot is all hardlinks (same inode, link count
 *   > 1), and — where this machine has a second filesystem — that the cross
 *   device fallback is reported as a copy with a reason.
 *
 * The two report entry points used here are NOT part of the frozen ABI (sfb.h
 * must not change); they are declared below exactly as sfb_ops.c defines them.
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb_internal.h"
#include "../../include/qtsq.h"

#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>

int sfb_ops_migrate_report(const char *src_path, const char *dst_path,
                           const char *key, char *json_out, size_t cap);
int sfb_ops_backup_report(const char *path, const char *dest,
                          char *json_out, size_t cap);

#define DIM 64u
#define FIRST_BATCH 100u
#define SECOND_BATCH 40u
#define V1_ROWS 20u

static int checks, fails;

static void t_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void t_check(int ok, int line, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    fails++;
    fprintf(stderr, "  [FAIL] %s:%d ", __FILE__, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define CHECK(cond, ...) t_check((cond) ? 1 : 0, __LINE__, __VA_ARGS__)

/* ── filesystem helpers ───────────────────────────────────────────────── */
static int t_rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        char full[4096];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (t_rm_rf(full) != 0) rc = -1; }
        else if (unlink(full) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

static int t_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int t_is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Regular files directly inside `dir` (".tmp"/dotfiles included on purpose:
 * a healthy store has none, and the backup report counts what is there). */
static int t_count_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (d == NULL) return -1;
    while ((de = readdir(d)) != NULL) {
        char full[4096];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) n++;
    }
    closedir(d);
    return n;
}

/* First sealed segment file of generation `gen` (sfb_ops.c spells files
 * seg-<gen>-<seq>.qtsq; the prefix test keeps the seq free). */
static int t_find_seg(const char *dir, unsigned gen, char *out, size_t cap)
{
    char want[32];
    DIR *d;
    struct dirent *de;
    int found = 0;

    snprintf(want, sizeof want, "seg-%06u-", gen);
    d = opendir(dir);
    if (d == NULL) return 0;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, want, strlen(want)) != 0) continue;
        if (strstr(de->d_name, ".qtsq") == NULL) continue;
        if (strstr(de->d_name, ".tmp") != NULL) continue;
        snprintf(out, cap, "%s/%s", dir, de->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

static int t_flip_byte_at(const char *path, long off)
{
    FILE *f = fopen(path, "r+b");
    int c, rc = -1;
    if (f == NULL) return -1;
    if (fseek(f, off, SEEK_SET) == 0 && (c = fgetc(f)) != EOF &&
        fseek(f, off, SEEK_SET) == 0 && fputc(c ^ 0xFF, f) != EOF)
        rc = 0;
    fclose(f);
    return rc;
}

static long t_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static void t_copytree(const char *src, const char *dst)
{
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "cp -R '%s' '%s'", src, dst);
    if (system(cmd) != 0) { /* reported by the caller's next check */ }
}

/* ── tiny JSON probes (no JSON library on this module's dependency list) ─ */
static const char *jf(const char *j, const char *key)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? p + strlen(pat) : NULL;
}

static int jbool(const char *j, const char *key, int *out)
{
    const char *p = jf(j, key);
    if (!p) return 0;
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    return 0;
}

static int ju64(const char *j, const char *key, unsigned long long *out)
{
    const char *p = jf(j, key);
    char *end = NULL;
    unsigned long long v;
    if (!p) return 0;
    while (*p == ' ') p++;
    v = strtoull(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static int jstr(const char *j, const char *key, char *out, size_t cap)
{
    const char *p = jf(j, key);
    size_t n = 0;
    if (out && cap) out[0] = '\0';
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;    /* paths/codes: copy the raw byte */
        if (out && n + 1 < cap) out[n++] = *p;
        p++;
    }
    if (out && cap) out[n] = '\0';
    return 1;
}

static int jnull(const char *j, const char *key)
{
    const char *p = jf(j, key);
    if (!p) return 0;
    while (*p == ' ') p++;
    return !strncmp(p, "null", 4);
}

/* Structural JSON check: balanced braces/brackets outside strings, and the
 * report starts as an object. This is what "parseable" can honestly mean
 * without a parser; every field is then read with the probes above. */
static int jvalid(const char *j)
{
    int depth = 0, in_str = 0, esc = 0;
    if (j == NULL || j[0] != '{') return 0;
    for (const char *p = j; *p; p++) {
        if (in_str) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') in_str = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth < 0) return 0; }
    }
    return depth == 0 && !in_str;
}

/* "missing_files" contains `needle` (inside the array, not anywhere else) */
static int jarray_has(const char *j, const char *key, const char *needle)
{
    const char *p = jf(j, key);
    const char *end;
    const char *hit;
    if (!p) return 0;
    end = strchr(p, ']');
    if (!end) return 0;
    hit = strstr(p, needle);
    return hit != NULL && hit < end;
}

typedef struct {
    unsigned long long gen;
    unsigned long long segments;
    unsigned long long records;
} gen_row_t;

/* Reads "generation_history":[{...},...] into rows. Returns the row count or -1. */
static int parse_history(const char *j, gen_row_t *rows, int max)
{
    const char *p = jf(j, "generation_history");
    int n = 0;

    if (!p) return -1;
    while (*p == ' ') p++;
    if (*p != '[') return -1;
    p++;
    while (*p && n < max) {
        const char *end;
        char obj[256];
        size_t len;
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '{') return -1;
        end = strchr(p, '}');
        if (!end) return -1;
        len = (size_t)(end - p) + 1;
        if (len >= sizeof obj) return -1;
        memcpy(obj, p, len);
        obj[len] = '\0';
        if (!ju64(obj, "generation", &rows[n].gen) ||
            !ju64(obj, "segments", &rows[n].segments) ||
            !ju64(obj, "records", &rows[n].records))
            return -1;
        n++;
        p = end + 1;
    }
    return n;
}

/* ── store builders ───────────────────────────────────────────────────── */
static void fill_vec(float *v, uint32_t i)
{
    for (uint32_t j = 0; j < DIM; j++)
        v[j] = (float)(((i * 131u + j * 17u) % 61u)) / 61.0f;
}

/* One open, two flushes, one close: exactly two sealed generations, provided
 * nothing auto-seals in between (the batches stay under SFB_SEGMENT_CAP). */
static int build_two_flush_store(const char *path)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    sfb_flush_result_t fr;
    sfb_remember_result_t res;
    float vec[DIM];
    uint32_t i;
    int rc;

    sfb_config_default(&cfg, path, DIM);
    cfg.path = path;
    cfg.dim = DIM;
    cfg.mode = SFB_MODE_COGNITIVE;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) return rc;

    for (i = 0; i < FIRST_BATCH; i++) {
        char text[64];
        fill_vec(vec, i);
        snprintf(text, sizeof text, "w7 alpha record %u", i);
        memset(&res, 0, sizeof res);
        rc = sfb_remember(b, vec, text, "user", &res);
        if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
    }
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
    if (fr.records_written != FIRST_BATCH) {
        (void)sfb_close(b);
        t_say("    NOTE: first flush wrote %llu of %u records (dedup?)\n",
              (unsigned long long)fr.records_written, FIRST_BATCH);
        return SFB_E_STATE;
    }

    for (i = FIRST_BATCH; i < FIRST_BATCH + SECOND_BATCH; i++) {
        char text[64];
        fill_vec(vec, i + 977u);        /* keep vectors far from batch one */
        snprintf(text, sizeof text, "w7 beta record %u", i);
        memset(&res, 0, sizeof res);
        rc = sfb_remember(b, vec, text, "user", &res);
        if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
    }
    memset(&fr, 0, sizeof fr);
    rc = sfb_flush(b, &fr);
    if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
    if (fr.records_written != SECOND_BATCH) {
        (void)sfb_close(b);
        t_say("    NOTE: second flush wrote %llu of %u records (dedup?)\n",
              (unsigned long long)fr.records_written, SECOND_BATCH);
        return SFB_E_STATE;
    }
    return sfb_close(b);
}

/* A genuine v1 container this process can read: packed plaintext through
 * qtsq_container_pack_write() — the same recipe tests/test_migrate.c uses,
 * because qtsq_brain_save() encrypts to the OS keystore. */
static int build_v1_fixture(const char *path, size_t rows, size_t dim, uint32_t salt)
{
    qtsq_context_t c, sub;
    float *mems;
    uint32_t dims[2];
    int rc;

    mems = (float *)malloc(rows * dim * sizeof *mems);
    if (mems == NULL) return -1;
    for (size_t r = 0; r < rows; r++)
        for (size_t j = 0; j < dim; j++)
            mems[r * dim + j] = (float)(((r + salt) * (j + 5)) % 29) / 29.0f;

    dims[0] = (uint32_t)rows;
    dims[1] = (uint32_t)dim;
    if (qtsq_container_create(&c) != QTSQ_OK) { free(mems); return -1; }
    qtsq_init(&sub);
    rc = qtsq_compress_tensor(&sub, mems, rows * dim, dims, 2, 0);
    if (rc == QTSQ_OK) rc = qtsq_container_add_stream_take(&c, &sub, "memories");
    qtsq_free(&sub);
    if (rc == QTSQ_OK) rc = qtsq_container_pack_write(&c, path, 1);
    qtsq_free(&c);
    free(mems);
    return rc == QTSQ_OK ? 0 : -1;
}

/* ── W7.4: stats honesty ──────────────────────────────────────────────── */
static void test_stats_healthy(const char *store)
{
    char segdir[512], json[65536], vjson[8192];
    gen_row_t rows[8];
    unsigned long long stats_records = 0, verify_records = 0, live = 0;
    unsigned long long segments = 0, generations = 0, open_us = 0;
    int ok = 0, nrows;
    int rc;

    t_say("── W7.4: healthy store (two flushes)\n");
    snprintf(segdir, sizeof segdir, "%s/%s", store, SFB_SEG_DIR);

    memset(json, 0, sizeof json);
    rc = sfb_stats_json(store, NULL, json, sizeof json);
    memset(vjson, 0, sizeof vjson);
    (void)sfb_verify(store, NULL, 0, vjson, sizeof vjson);

    CHECK(rc == SFB_OK, "stats rc == SFB_OK (%s)", sfb_strerror(rc));
    CHECK(jvalid(json), "stats report is structurally valid JSON");
    CHECK(jbool(json, "ok", &ok) && ok, "healthy store reports ok:true");
    CHECK(ju64(json, "records", &stats_records) && stats_records == FIRST_BATCH + SECOND_BATCH,
          "stats records == %u (got %llu)", FIRST_BATCH + SECOND_BATCH, stats_records);
    CHECK(ju64(json, "live", &live) && live == FIRST_BATCH + SECOND_BATCH,
          "stats live == %u (got %llu)", FIRST_BATCH + SECOND_BATCH, live);
    CHECK(ju64(json, "segments", &segments) && segments == 2,
          "two flushes -> 2 segments (got %llu)", segments);
    CHECK(ju64(json, "generations", &generations) && generations == 2,
          "two flushes -> generation 2 (got %llu)", generations);

    /* (b) real generation history, derived from the manifest segment table */
    nrows = parse_history(json, rows, 8);
    CHECK(nrows == 2, "generation_history lists 2 generations (got %d)", nrows);
    if (nrows == 2) {
        CHECK(rows[0].gen == 1 && rows[0].segments == 1 && rows[0].records == FIRST_BATCH,
              "history[0] = gen 1: 1 segment, %u records (got gen %llu, %llu seg, %llu rec)",
              FIRST_BATCH, rows[0].gen, rows[0].segments, rows[0].records);
        CHECK(rows[1].gen == 2 && rows[1].segments == 1 && rows[1].records == SECOND_BATCH,
              "history[1] = gen 2: 1 segment, %u records (got gen %llu, %llu seg, %llu rec)",
              SECOND_BATCH, rows[1].gen, rows[1].segments, rows[1].records);
    }

    /* (c) latency honesty: unknown is null, measured is a real number */
    CHECK(jnull(json, "flushes"), "\"flushes\":null (per-process counter, never 0)");
    CHECK(jnull(json, "flush_last_us"), "\"flush_last_us\":null (never 0)");
    CHECK(ju64(json, "open_us", &open_us) && open_us > 0,
          "open_us is a real measurement (> 0)");
    CHECK(strstr(json, "\"open_us_source\":\"measured_this_process\"") != NULL,
          "open_us is labelled as measured by this process");
    CHECK(strstr(json, "\"flushes\":0") == NULL, "no fake \"flushes\":0 in the report");

    /* healthy counts must match verify's, field for field */
    CHECK(jvalid(vjson), "verify report is structurally valid JSON");
    CHECK(ju64(vjson, "records", &verify_records) && verify_records == stats_records,
          "stats and verify agree on records (%llu vs %llu)", stats_records, verify_records);
    CHECK(strstr(json, "\"missing_files\":[]") != NULL,
          "healthy store has an empty missing_files list");

    /* the history must describe the files that are actually on disk */
    CHECK(t_count_files(segdir) >= 2, "segment directory holds the payload files");
}

static void test_stats_deleted_segment(const char *store)
{
    char segdir[512], victim[600], json[65536], vjson[8192];
    unsigned long long dropped = 0, vdropped = 0, missing = 0, damaged = 1;
    unsigned long long expect = SECOND_BATCH;
    int ok = 1;
    int rc;

    t_say("── W7.4: one sealed segment deleted (the W7.4 repro)\n");
    snprintf(segdir, sizeof segdir, "%s/%s", store, SFB_SEG_DIR);
    CHECK(t_find_seg(segdir, 2, victim, sizeof victim),
          "found the generation-2 segment file");
    if (!t_is_file(victim)) return;
    CHECK(unlink(victim) == 0, "deleted %s", victim);

    memset(json, 0, sizeof json);
    rc = sfb_stats_json(store, NULL, json, sizeof json);
    memset(vjson, 0, sizeof vjson);
    (void)sfb_verify(store, NULL, 0, vjson, sizeof vjson);
    t_say("    stats: %s\n", json);

    CHECK(rc == SFB_E_CORRUPT, "stats rc == SFB_E_CORRUPT (got %s)", sfb_strerror(rc));
    CHECK(jvalid(json), "damaged-store report is still valid JSON");
    CHECK(jbool(json, "ok", &ok) && !ok, "stats says ok:false (never true here)");
    CHECK(ju64(json, "records_dropped", &dropped) && dropped == expect,
          "records_dropped == %llu (got %llu)", expect, dropped);
    CHECK(dropped > 0, "records_dropped is non-zero");
    CHECK(ju64(json, "segments_missing", &missing) && missing == 1,
          "segments_missing == 1 (got %llu)", missing);
    CHECK(ju64(json, "damaged_segments", &damaged) && damaged == 0,
          "damaged_segments == 0 for a deletion (got %llu)", damaged);
    CHECK(jarray_has(json, "missing_files", "segment gen=2 seq=1 (40 records)"),
          "missing_files names the deleted segment and its record count");
    CHECK(jarray_has(json, "errors", "segment_file_missing"),
          "errors carries the typed finding");

    /* stats and verify must tell the same story about the same store */
    CHECK(ju64(vjson, "records_dropped_estimate", &vdropped) &&
          vdropped == expect && vdropped == dropped,
          "verify agrees on the loss (%llu vs %llu)", vdropped, dropped);
    CHECK(jarray_has(vjson, "missing_files", "segment gen=2 seq=1 (40 records)"),
          "verify names the same missing segment");
}

static void test_stats_damaged_segment(const char *store)
{
    char segdir[512], victim[600], json[65536];
    unsigned long long dropped = 0, damaged = 0;
    long size;
    int ok = 1;
    int rc;

    t_say("── W7.4: same-size byte flip in a sealed segment\n");
    snprintf(segdir, sizeof segdir, "%s/%s", store, SFB_SEG_DIR);
    CHECK(t_find_seg(segdir, 2, victim, sizeof victim), "found a sealed segment");
    size = t_is_file(victim) ? t_file_size(victim) : -1;
    CHECK(size > 200, "segment is big enough to corrupt mid-payload (size=%ld)", size);
    if (size <= 200) return;
    CHECK(t_flip_byte_at(victim, size / 2) == 0,
          "flipped one payload byte in %s (size unchanged)", victim);

    memset(json, 0, sizeof json);
    rc = sfb_stats_json(store, NULL, json, sizeof json);
    t_say("    stats: %s\n", json);
    CHECK(rc == SFB_E_CORRUPT, "stats rc == SFB_E_CORRUPT (got %s)", sfb_strerror(rc));
    CHECK(jbool(json, "ok", &ok) && !ok, "damage without a size change is not ok");
    CHECK(ju64(json, "damaged_segments", &damaged) && damaged == 1,
          "damaged_segments == 1 (got %llu)", damaged);
    CHECK(ju64(json, "records_dropped", &dropped) && dropped == SECOND_BATCH,
          "records_dropped counts the damaged segment's records (got %llu)", dropped);
}

/* ── W7.3: backup link/copy report ────────────────────────────────────── */
static void test_backup(const char *store)
{
    char snap[600], srcseg[600], dstseg[600], json[16384];
    unsigned long long files = 0, linked = 0, copied = 0;
    char snapshot[32] = {0};
    int fallback = 1, mcopy = 0;
    int rc, nsrc;

    t_say("── W7.3: backup reports hardlink vs copy\n");
    snprintf(snap, sizeof snap, "%s-snap", store);
    (void)t_rm_rf(snap);
    snprintf(srcseg, sizeof srcseg, "%s/%s", store, SFB_SEG_DIR);
    snprintf(dstseg, sizeof dstseg, "%s/%s", snap, SFB_SEG_DIR);

    memset(json, 0, sizeof json);
    rc = sfb_ops_backup_report(store, snap, json, sizeof json);
    t_say("    report: %s\n", json);
    CHECK(rc == SFB_OK, "backup rc == SFB_OK (%s)", sfb_strerror(rc));
    CHECK(jvalid(json), "backup report is structurally valid JSON");
    CHECK(ju64(json, "files", &files) && files >= 2,
          "report counts the payload files (got %llu)", files);
    CHECK(ju64(json, "files_linked", &linked) && linked == files,
          "all %llu payload file(s) were hardlinked (got %llu)", files, linked);
    CHECK(ju64(json, "files_copied", &copied) && copied == 0,
          "nothing fell back to a byte copy (got %llu)", copied);
    CHECK(linked + copied == files,
          "link + copy counts add up to the files backed up (%llu + %llu = %llu)",
          linked, copied, files);
    CHECK(jstr(json, "snapshot", snapshot, sizeof snapshot) &&
          strcmp(snapshot, "hardlink") == 0,
          "snapshot kind is \"hardlink\" (got \"%s\")", snapshot);
    CHECK(jbool(json, "link_fallback", &fallback) && !fallback,
          "link_fallback == false on the same filesystem");
    CHECK(jnull(json, "copy_reason"), "copy_reason is null when nothing was copied");
    CHECK(jbool(json, "manifest_copied", &mcopy) && mcopy == 1,
          "the manifest is reported as a byte copy (the consistency point)");

    /* independent count: the number of regular files in the source seg dir */
    nsrc = t_count_files(srcseg);
    CHECK(nsrc == (int)files, "report files == %d file(s) in the source store (got %llu)",
          nsrc, files);

    /* and the acceptance must be real, not just a number in the report: same
     * inode, link count > 1 for every payload file in the snapshot */
    {
        DIR *d = opendir(dstseg);
        struct dirent *de;
        int checked = 0;
        if (d != NULL) {
            while ((de = readdir(d)) != NULL) {
                char a[1200], b[1200];
                struct stat sa, sb;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                snprintf(a, sizeof a, "%s/%s", srcseg, de->d_name);
                snprintf(b, sizeof b, "%s/%s", dstseg, de->d_name);
                if (stat(a, &sa) != 0 || stat(b, &sb) != 0) continue;
                CHECK(sa.st_ino == sb.st_ino,
                      "%s is the same inode in the snapshot", de->d_name);
                CHECK(sb.st_nlink > 1, "%s has link count %d > 1",
                      de->d_name, (int)sb.st_nlink);
                checked++;
            }
            closedir(d);
        }
        CHECK(checked > 0, "walked the snapshot payload files (%d)", checked);
    }

    {
        char vj[8192];
        int vok = 0;
        memset(vj, 0, sizeof vj);
        rc = sfb_verify(snap, NULL, 0, vj, sizeof vj);
        (void)jbool(vj, "ok", &vok);
        CHECK(rc == SFB_OK && vok, "the hardlinked snapshot verifies clean (%s)",
              sfb_strerror(rc));
    }
    (void)t_rm_rf(snap);

    /* The fallback path needs a second filesystem; run it when this machine has
     * one (SFB_W7_XDEV=<dir>) instead of pretending to cover it. */
    {
        const char *xd = getenv("SFB_W7_XDEV");
        struct stat s1, s2;
        if (xd && xd[0] && stat(store, &s1) == 0 && stat(xd, &s2) == 0 &&
            s1.st_dev != s2.st_dev && t_is_dir(xd)) {
            char xsnap[800];
            snprintf(xsnap, sizeof xsnap, "%s/sfb-w7-xdev-%ld", xd, (long)getpid());
            (void)t_rm_rf(xsnap);
            memset(json, 0, sizeof json);
            rc = sfb_ops_backup_report(store, xsnap, json, sizeof json);
            t_say("    cross-device report: %s\n", json);
            CHECK(rc == SFB_OK, "cross-device backup succeeds via a copy (%s)",
                  sfb_strerror(rc));
            CHECK(ju64(json, "files", &files) && ju64(json, "files_copied", &copied) &&
                  copied == files,
                  "every file was copied across devices (%llu of %llu)", copied, files);
            CHECK(ju64(json, "files_linked", &linked) && linked == 0,
                  "no file was linked across devices (got %llu)", linked);
            CHECK(jbool(json, "link_fallback", &fallback) && fallback,
                  "link_fallback == true");
            CHECK(jarray_has(json, "copy_reasons", "cross_device_link"),
                  "the reason is named (cross_device_link)");
            (void)t_rm_rf(xsnap);
        } else {
            t_say("    SKIP cross-device fallback: no second filesystem "
                  "(set SFB_W7_XDEV=<dir on another device> to run it)\n");
        }
    }
}

/* ── W7.5: migrate reports ────────────────────────────────────────────── */
static int cli_available(const char *bin)
{
    return access(bin, X_OK) == 0;
}

/* Runs `cmdline` with stdout captured to `outpath`; returns the exit status. */
static int run_cli(const char *cmdline, const char *outpath, char *out, size_t cap)
{
    char cmd[8192];
    int st;

    snprintf(cmd, sizeof cmd, "%s >'%s' 2>'%s.err'", cmdline, outpath, outpath);
    st = system(cmd);
    out[0] = '\0';
    {
        FILE *f = fopen(outpath, "rb");
        if (f != NULL) {
            size_t n = fread(out, 1, cap - 1, f);
            out[n] = '\0';
            fclose(f);
        }
    }
    if (st == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_migrate(const char *base)
{
    char src[600], dst[600], json[8192], vj[8192], outpath[700], out[16384];
    unsigned long long records = 0, rejected = 1;
    char action[32] = {0}, backup[600] = {0}, want_backup[700];
    int ok = 0, alread = 0, vok = 0;
    int rc;

    t_say("── W7.5: migrate a real v1 container\n");
    snprintf(src, sizeof src, "%s/v1-brain.qtsq", base);
    snprintf(dst, sizeof dst, "%s/v1-imported", base);
    (void)t_rm_rf(src);
    (void)t_rm_rf(dst);
    {
        char bk[700];
        snprintf(bk, sizeof bk, "%s.v1backup", src);
        (void)t_rm_rf(bk);
    }
    CHECK(build_v1_fixture(src, V1_ROWS, DIM, 3) == 0,
          "built a genuine v1 QTSQ container at %s", src);
    snprintf(want_backup, sizeof want_backup, "%s.v1backup", src);

    memset(json, 0, sizeof json);
    rc = sfb_ops_migrate_report(src, dst, NULL, json, sizeof json);
    t_say("    report: %s\n", json);
    CHECK(rc == SFB_OK, "migrate rc == SFB_OK (%s)", sfb_strerror(rc));
    CHECK(jvalid(json), "migrate report is structurally valid JSON");
    CHECK(jstr(json, "action", action, sizeof action) && strcmp(action, "imported") == 0,
          "action is \"imported\" (got \"%s\")", action);
    CHECK(jbool(json, "v1", &ok) && ok, "report says v1:true");
    CHECK(ju64(json, "records", &records) && records == V1_ROWS,
          "imported %u records (got %llu)", V1_ROWS, records);
    CHECK(ju64(json, "rejected", &rejected) && rejected == 0,
          "rejected == 0 (got %llu)", rejected);
    CHECK(jstr(json, "backup", backup, sizeof backup) && strcmp(backup, want_backup) == 0,
          "report names the pre-migration backup (got \"%s\")", backup);
    CHECK(t_is_file(want_backup), "the .v1backup snapshot exists on disk");

    memset(vj, 0, sizeof vj);
    rc = sfb_verify(dst, NULL, 0, vj, sizeof vj);
    (void)jbool(vj, "ok", &vok);
    CHECK(rc == SFB_OK && vok, "the imported store verifies clean (%s)", sfb_strerror(rc));

    /* ── the CLI: the message must match what happened ───────────────────── */
    {
        const char *bin = getenv("SFB_BIN");
        char cmd[2048], src2[600], dst2[600], dst3[600], dst4[600], dst5[600];
        char src2b[600];
        int st;

        if (bin == NULL || bin[0] == '\0') bin = "./sfb";
        if (!cli_available(bin)) {
            t_say("    SKIP CLI checks: %s is not executable here "
                  "(run from the module directory or set SFB_BIN)\n", bin);
            return;
        }

        snprintf(outpath, sizeof outpath, "%s/migrate-cli.out", base);

        /* 1. a real import must say it imported, not "already v2" */
        snprintf(src2, sizeof src2, "%s/v1-cli.qtsq", base);
        snprintf(dst2, sizeof dst2, "%s/v1-cli-imported", base);
        (void)t_rm_rf(src2);
        (void)t_rm_rf(dst2);
        {
            char bk[700];
            snprintf(bk, sizeof bk, "%s.v1backup", src2);
            (void)t_rm_rf(bk);
        }
        CHECK(build_v1_fixture(src2, 12, DIM, 7) == 0, "built a second v1 container");
        snprintf(cmd, sizeof cmd, "%s migrate '%s' '%s'", bin, src2, dst2);
        st = run_cli(cmd, outpath, out, sizeof out);
        t_say("    cli: %s", out);
        CHECK(st == 0, "sfb migrate exit 0 (got %d)", st);
        CHECK(strstr(out, "imported") != NULL,
              "the success line says it imported records");
        CHECK(strstr(out, "12") != NULL, "the success line reports the record count");
        CHECK(strstr(out, "already v2") == NULL,
              "the success line does not claim \"already v2\" after a real import");
        CHECK(strstr(out, "v1.qtsq.v1backup") != NULL || strstr(out, "v1 backup") != NULL,
              "the success line names the pre-migration backup");

        /* 2. --json prints the report on stdout and no human line */
        snprintf(dst3, sizeof dst3, "%s/v1-cli-json", base);
        (void)t_rm_rf(dst3);
        snprintf(cmd, sizeof cmd, "%s migrate '%s' '%s' --json", bin, src2, dst3);
        st = run_cli(cmd, outpath, out, sizeof out);
        t_say("    cli --json: %s", out);
        CHECK(st == 0, "sfb migrate --json exit 0 (got %d)", st);
        CHECK(jvalid(out), "--json stdout is structurally valid JSON");
        CHECK(jstr(out, "action", action, sizeof action) &&
              strcmp(action, "imported") == 0,
              "--json action is \"imported\" (got \"%s\")", action);
        CHECK(ju64(out, "records", &records) && records == 12,
              "--json reports 12 imported records (got %llu)", records);
        {
            char bk2[700];
            snprintf(bk2, sizeof bk2, "%s.v1backup", src2);
            CHECK(strstr(out, bk2) != NULL,
                  "--json carries the backup path (%s)", bk2);
        }
        CHECK(strstr(out, "migrate:") == NULL,
              "--json suppresses the human line");

        /* 3. an already-v2 source: the old message, and it must be the right one */
        snprintf(src2b, sizeof src2b, "%s/%s", dst, SFB_MANIFEST_NAME);
        snprintf(dst4, sizeof dst4, "%s/v2-copied", base);
        (void)t_rm_rf(dst4);
        snprintf(cmd, sizeof cmd, "%s migrate '%s' '%s'", bin, src2b, dst4);
        st = run_cli(cmd, outpath, out, sizeof out);
        t_say("    cli already-v2: %s", out);
        CHECK(st == 0, "sfb migrate on a v2 store exit 0 (got %d)", st);
        CHECK(strstr(out, "already v2") != NULL,
              "an already-v2 source still says \"already v2\"");
        CHECK(strstr(out, "(imported") == NULL,
              "an already-v2 source does not claim an import");

        snprintf(dst5, sizeof dst5, "%s/v2-copied-json", base);
        (void)t_rm_rf(dst5);
        snprintf(cmd, sizeof cmd, "%s migrate '%s' '%s' --json", bin, src2b, dst5);
        st = run_cli(cmd, outpath, out, sizeof out);
        CHECK(st == 0, "sfb migrate --json on a v2 store exit 0 (got %d)", st);
        CHECK(jvalid(out), "already-v2 --json is valid JSON");
        CHECK(jbool(out, "already_v2", &alread) && alread,
              "already-v2 --json says already_v2:true");
        CHECK(jstr(out, "action", action, sizeof action) &&
              strcmp(action, "copied") == 0,
              "already-v2 --json action is \"copied\" (got \"%s\")", action);

        /* 4. usage stays a usage error */
        snprintf(cmd, sizeof cmd, "%s migrate", bin);
        st = run_cli(cmd, outpath, out, sizeof out);
        CHECK(st == 2, "sfb migrate without arguments exits 2 (got %d)", st);
    }
}

int main(void)
{
    char base[512], healthy[600], deleted[600], damaged[600];
    int rc;

    snprintf(base, sizeof base, "tests/tmp/w7-%ld", (long)getpid());
    (void)t_rm_rf(base);
    if (mkdir("tests/tmp", 0700) != 0 && errno != EEXIST) { /* best effort */ }
    if (mkdir(base, 0700) != 0) {
        t_say("cannot create %s\n", base);
        return 1;
    }

    snprintf(healthy, sizeof healthy, "%s/healthy", base);
    rc = build_two_flush_store(healthy);
    CHECK(rc == SFB_OK, "built a two-flush store (%s)", sfb_strerror(rc));
    if (rc != SFB_OK) goto done;

    test_stats_healthy(healthy);

    /* the same store, once with a deleted segment and once with a flipped byte */
    snprintf(deleted, sizeof deleted, "%s/deleted", base);
    snprintf(damaged, sizeof damaged, "%s/damaged", base);
    t_copytree(healthy, deleted);
    t_copytree(healthy, damaged);
    CHECK(t_is_dir(deleted) && t_is_dir(damaged), "made working copies of the store");

    test_stats_deleted_segment(deleted);
    test_stats_damaged_segment(damaged);
    test_backup(healthy);
    test_migrate(base);

done:
    (void)t_rm_rf(base);
    t_say("\ntest_w7: %d checks, %d failure(s)\n", checks, fails);
    if (fails == 0) {
        t_say("ALL SFB W7 REPORT TESTS PASSED\n");
        return 0;
    }
    t_say("SFB W7 REPORT TESTS FAILED\n");
    return 1;
}
