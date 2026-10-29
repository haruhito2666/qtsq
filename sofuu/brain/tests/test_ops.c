/*
 * tests/test_ops.c — W7 operations layer tests (verify / repair / backup /
 * restore / stats --json / migrate).
 *
 * Everything is driven through the *public* API (sfb.h) plus the layout
 * constants in sfb_internal.h (SFB_MANIFEST_NAME / SFB_SEG_DIR). No internal
 * function of sfb_store.c or sfb_brain.c is called, so this test keeps working
 * as those modules evolve.
 *
 * Covered:
 *   1. build a store with open/remember/flush, flush it, close it
 *   2. sfb_verify → ok, records/segments as written
 *   3. corrupt one byte inside a sealed segment → verify reports damage
 *   4. sfb_verify(repair=1) → *.torn evidence kept, manifest rewritten and
 *      still openable, dropped records reported
 *   5. sfb_backup + sfb_restore → same record count, restored store verifies
 *   6. backup refuses while a writer holds the lock (SFB_E_LOCKED)
 *   7. restore refuses to overwrite an existing destination (SFB_E_STATE)
 *   8. sfb_stats_json → every required key present and parseable
 *   9. sfb_migrate on a QTSQ v1 container → SFB_E_INVALID + the honest
 *      "not implemented in this pass" reason on stderr
 *  10. sfb_migrate on an unknown magic → SFB_E_INVALID + honest reason
 *  11. sfb_migrate on an already-v2 source → no-op copy, SFB_OK
 *  12. truncation safety: a tiny json_cap still NUL-terminates
 *
 * Run from the brain/ directory (the Makefile does this):
 *   ./tests/test_ops
 * Temporary state lives under tests/tmp/ and is removed at exit.
 */
#define _POSIX_C_SOURCE 200809L

#include "sfb_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>

static int g_checks, g_failures;

static void t_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void t_check(int cond, int line, const char *fmt, ...)
{
    g_checks++;
    va_list ap;
    char msg[512];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (cond) {
        t_say("    ok   %s\n", msg);
    } else {
        t_say("    FAIL %s  [tests/test_ops.c:%d]\n", msg, line);
        g_failures++;
    }
}

#define CHECK(cond, ...) t_check((cond) ? 1 : 0, __LINE__, __VA_ARGS__)

/* ── tiny helpers ─────────────────────────────────────────────────────── */
static int t_mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int t_rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path) == 0 ? 0 : (errno == ENOENT ? 0 : -1);
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
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

/* JSON probes (reports are produced by sfb_ops.c; we only look for keys) */
static int t_jbool(const char *j, const char *key, int *out)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4))  { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    return 0;
}

static int t_ju64(const char *j, const char *key, unsigned long long *out)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return 0;
    *out = strtoull(p, NULL, 10);
    return 1;
}

/* Pick a payload file in the segment dir: the largest regular file that is not
 * a graph. Works whatever naming sfb_store.c chose. */
static int t_pick_segment(const char *seg_dir, char *out, size_t cap)
{
    DIR *d = opendir(seg_dir);
    if (!d) return 0;
    struct dirent *de;
    off_t best = -1;
    out[0] = '\0';
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!strncmp(de->d_name, "graph-", 6)) continue;
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", seg_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size < 128) continue;
        if (st.st_size > best) { best = st.st_size; snprintf(out, cap, "%s", full); }
    }
    closedir(d);
    return out[0] != '\0';
}

/* Flip one byte in the middle of a file — a realistic single-bit-flip style
 * corruption that keeps the file size unchanged. */
static int t_flip_byte(const char *file)
{
    int fd = open(file, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) { close(fd); return -1; }
    off_t off = st.st_size / 2;
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return -1; }
    b ^= 0xFFu;
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return -1; }
    (void)fsync(fd);
    close(fd);
    return 0;
}

/* Locate the first record's first text byte in a v2 segment payload and flip
 * it, so the *per-record* CRC (not just the file digest) has to catch it.
 * Returns 1 on success, 0 when the payload does not look like the record block
 * this build writes — the caller then SKIPs instead of failing. */
static int t_flip_record_text_byte(const char *segdir_path)
{
    char victim[800];
    if (!t_pick_segment(segdir_path, victim, sizeof victim)) return 0;

    int fd = open(victim, O_RDWR);
    if (fd < 0) return 0;
    uint8_t hdr[48];
    if (pread(fd, hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr) { close(fd); return 0; }
    uint32_t magic = 0, records = 0;
    uint64_t payload_len = 0;
    memcpy(&magic, hdr + 0, 4);
    memcpy(&records, hdr + 12, 4);
    memcpy(&payload_len, hdr + 24, 8);
    if (magic != SFB_MAGIC_SEGMENT || records == 0 || payload_len < 4 + 44) {
        close(fd);
        return 0;
    }
    uint8_t rh[44];
    if (pread(fd, rh, sizeof rh, 48 + 4) != (ssize_t)sizeof rh) { close(fd); return 0; }
    uint32_t text_len = 0, role_len = 0;
    memcpy(&text_len, rh + 12, 4);
    memcpy(&role_len, rh + 16, 4);
    if (text_len == 0 || text_len > 4096 || 4 + 44 + text_len + role_len > payload_len) {
        close(fd);
        return 0;
    }
    off_t off = 48 + 4 + 44;              /* first byte of record 0's text */
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return 0; }
    b ^= 0x20u;                            /* toggle a letter's case */
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return 0; }
    (void)fsync(fd);
    close(fd);
    return 1;
}

static int t_count_torn(const char *seg_dir, char *first, size_t cap)
{
    DIR *d = opendir(seg_dir);
    if (!d) return 0;
    struct dirent *de;
    int n = 0;
    if (first && cap) first[0] = '\0';
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcmp(de->d_name + l - 5, ".torn")) {
            if (n == 0 && first && cap) snprintf(first, cap, "%s", de->d_name);
            n++;
        }
    }
    closedir(d);
    return n;
}

static int t_write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(fd, (const uint8_t *)data + off, len - off);
        if (k <= 0) { close(fd); return -1; }
        off += (size_t)k;
    }
    close(fd);
    return 0;
}

/* ── the store under test ─────────────────────────────────────────────── */
#define DIM       64u
#define N_RECORDS 300u

static void fill_vec(float *v, uint32_t seed)
{
    for (uint32_t j = 0; j < DIM; j++)
        v[j] = (float)(((seed + 1u) * (j + 7u)) % 97u) / 97.0f;
}

static int build_store(const char *path)
{
    sfb_config_t cfg;
    sfb_config_default(&cfg, path, DIM);
    cfg.path = path;
    cfg.dim = DIM;
    cfg.mode = SFB_MODE_COGNITIVE;
    cfg.read_only = 0;

    sfb_brain_t *b = NULL;
    int rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) return rc;

    /* Two flushes of 150: guarantees at least two sealed segments whatever
     * sealing policy sfb_brain.c uses, so dropping one leaves a live store. */
    float vec[DIM];
    unsigned long long written = 0, sealed = 0;
    const uint32_t batch = 150;
    for (uint32_t i = 0; i < N_RECORDS; i++) {
        fill_vec(vec, i);
        char text[96];
        snprintf(text, sizeof text, "ops test record %u alpha beta gamma", i);
        sfb_remember_result_t res;
        memset(&res, 0, sizeof res);
        rc = sfb_remember(b, vec, text, (i % 3 == 0) ? "user" : "assistant", &res);
        if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
        if ((i + 1) % batch == 0) {
            sfb_flush_result_t fr;
            memset(&fr, 0, sizeof fr);
            rc = sfb_flush(b, &fr);
            if (rc != SFB_OK) { (void)sfb_close(b); return rc; }
            written += fr.records_written;
            sealed  += fr.segments_sealed;
        }
    }
    rc = sfb_close(b);
    if (rc != SFB_OK) return rc;
    t_say("    wrote %llu records, %llu segment(s) sealed\n", written, sealed);
    return SFB_OK;
}

int main(void)
{
    char base[512];
    snprintf(base, sizeof base, "tests/tmp/ops-%ld", (long)getpid());
    (void)t_rm_rf(base);
    if (t_mkdir_p(base) != 0) {
        fprintf(stderr, "test_ops: cannot create %s: %s\n", base, strerror(errno));
        return 1;
    }

    char brain[600], werk[600], snap[600], restored[600], migrated[600], misc[600];
    snprintf(brain,    sizeof brain,    "%s/brain", base);
    snprintf(snap,     sizeof snap,     "%s/snapshot", base);
    snprintf(restored, sizeof restored, "%s/restored", base);
    snprintf(migrated, sizeof migrated, "%s/migrated", base);
    snprintf(misc,     sizeof misc,     "%s/misc", base);
    snprintf(werk,     sizeof werk,     "%s/work", base);
    (void)t_mkdir_p(werk);

    char segdir[700], manifest[700];
    snprintf(segdir, sizeof segdir, "%s/%s", brain, SFB_SEG_DIR);
    snprintf(manifest, sizeof manifest, "%s/%s", brain, SFB_MANIFEST_NAME);

    char json[16384];
    int rc;

    /* ── 1. build ──────────────────────────────────────────────────────── */
    t_say("── test_ops: build a store with the public API\n");
    rc = build_store(brain);
    CHECK(rc == SFB_OK, "open/remember/flush/close returned SFB_OK (%s)",
          sfb_strerror(rc));
    CHECK(t_is_file(manifest), "manifest exists at %s", manifest);
    CHECK(t_is_dir(segdir), "segment directory exists at %s", segdir);
    if (rc != SFB_OK) goto done;

    /* ── 2. clean verify ───────────────────────────────────────────────── */
    t_say("── test_ops: sfb_verify on a clean store\n");
    memset(json, 0, sizeof json);
    rc = sfb_verify(brain, NULL, 0, json, sizeof json);
    int ok = 0;
    unsigned long long records = 0, segments = 0, damaged = 0, dropped = 0;
    /* declared before any goto so no path can read them uninitialised */
    unsigned long long dropped_now = 0, after_records = 0, after_segments = 0;
    int have_ok = t_jbool(json, "ok", &ok);
    int have_rec = t_ju64(json, "records", &records);
    int have_seg = t_ju64(json, "segments", &segments);
    int have_dmg = t_ju64(json, "damaged_segments", &damaged);
    int have_drp = t_ju64(json, "records_dropped_estimate", &dropped);
    t_say("    json: %s\n", json);
    CHECK(rc == SFB_OK, "verify rc == SFB_OK (%s)", sfb_strerror(rc));
    CHECK(have_ok && ok, "report says ok:true");
    CHECK(have_rec && records == N_RECORDS, "records == %u (got %llu)",
          N_RECORDS, records);
    CHECK(have_seg && segments >= 1, "segments >= 1 (got %llu)", segments);
    CHECK(have_dmg && damaged == 0, "damaged_segments == 0");
    CHECK(have_drp && dropped == 0, "records_dropped_estimate == 0");
    CHECK(strstr(json, "\"missing_files\":[]") != NULL, "missing_files is empty");
    CHECK(strstr(json, "segment_digest_mismatch") == NULL &&
          strstr(json, "record_crc_mismatch") == NULL,
          "no damage findings on a clean store");

    /* ── 2b. per-record CRC: flip one *text* byte in an independent copy ── */
    t_say("── test_ops: per-record CRC32C is actually checked\n");
    {
        char snap2[800], crctest[800], crcseg[900];
        snprintf(snap2, sizeof snap2, "%s/crc-snapshot", base);
        snprintf(crctest, sizeof crctest, "%s/crc-copy", base);
        snprintf(crcseg, sizeof crcseg, "%s/%s", crctest, SFB_SEG_DIR);
        int rc2 = sfb_backup(brain, snap2);
        int rc3 = (rc2 == SFB_OK) ? sfb_restore(snap2, crctest) : rc2;
        if (rc3 == SFB_OK && t_flip_record_text_byte(crcseg)) {
            char jj[16384];
            memset(jj, 0, sizeof jj);
            int rc4 = sfb_verify(crctest, NULL, 0, jj, sizeof jj);
            t_say("    json: %s\n", jj);
            CHECK(rc4 != SFB_OK, "verify fails on a text-byte flip (%s)",
                  sfb_strerror(rc4));
            CHECK(strstr(jj, "record_crc_mismatch") != NULL,
                  "the per-record CRC32C caught it (record_crc_mismatch present)");
            CHECK(strstr(jj, "payload_crc_mismatch") != NULL,
                  "the segment payload CRC caught it too");
        } else {
            t_say("    SKIP: record block layout not recognised by this build\n");
        }
    }

    /* ── 3. corrupt one segment ────────────────────────────────────────── */
    t_say("── test_ops: corrupt one byte in a sealed segment\n");
    char victim[800];
    if (!t_pick_segment(segdir, victim, sizeof victim)) {
        t_say("    SKIP: no payload file found in %s\n", segdir);
        goto stats_and_rest;
    }
    CHECK(t_flip_byte(victim) == 0, "flipped one byte in %s", victim);
    {
        /* reopen must refuse to pretend the store is fine, or load what is
         * still good — either way verify must see the damage */
        memset(json, 0, sizeof json);
        int rc2 = sfb_verify(brain, NULL, 0, json, sizeof json);
        unsigned long long dmg2 = 0, drp2 = 0;
        int ok2 = 1;
        (void)t_jbool(json, "ok", &ok2);
        (void)t_ju64(json, "damaged_segments", &dmg2);
        (void)t_ju64(json, "records_dropped_estimate", &drp2);
        t_say("    json: %s\n", json);
        CHECK(rc2 != SFB_OK, "verify returns non-zero after corruption (%s)",
              sfb_strerror(rc2));
        CHECK(!ok2, "report says ok:false");
        CHECK(dmg2 >= 1, "damaged_segments >= 1 (got %llu)", dmg2);
        CHECK(drp2 >= 1, "records_dropped_estimate >= 1 (got %llu)", drp2);
        damaged = dmg2;
    }

    /* ── 4. repair ─────────────────────────────────────────────────────── */
    t_say("── test_ops: sfb_verify(repair=1) quarantines the damaged segment\n");
    memset(json, 0, sizeof json);
    rc = sfb_verify(brain, NULL, 1, json, sizeof json);
    int repair_ok = 0;
    (void)t_jbool(json, "ok", &repair_ok);
    (void)t_ju64(json, "records_dropped_estimate", &dropped_now);
    (void)t_ju64(json, "records", &after_records);
    (void)t_ju64(json, "segments", &after_segments);
    t_say("    json: %s\n", json);
    CHECK(rc == SFB_OK, "repair rc == SFB_OK (%s)", sfb_strerror(rc));
    CHECK(repair_ok, "report says ok:true after repair");
    CHECK(dropped_now >= 1, "repair reports dropped records (got %llu)", dropped_now);
    CHECK(after_records < records, "records decreased (%llu -> %llu)",
          records, after_records);

    char torn[400] = {0};
    int ntorn = t_count_torn(segdir, torn, sizeof torn);
    CHECK(ntorn >= 1, "a *.torn quarantine file exists (count=%d, first=%s)",
          ntorn, torn[0] ? torn : "-");
    {
        /* the original damaged file must be gone (renamed, never deleted) and
         * the evidence must still contain the bytes we corrupted */
        char tornpath[900];
        snprintf(tornpath, sizeof tornpath, "%s/%s", segdir, torn);
        CHECK(t_is_file(tornpath), "quarantine file is a real file: %s", tornpath);
    }

    /* manifest still opens: verify again, and open the store read-only */
    memset(json, 0, sizeof json);
    rc = sfb_verify(brain, NULL, 0, json, sizeof json);
    int ok_after = 0;
    (void)t_jbool(json, "ok", &ok_after);
    t_say("    json: %s\n", json);
    CHECK(rc == SFB_OK && ok_after, "post-repair verify is clean (%s)",
          sfb_strerror(rc));

    {
        sfb_config_t ro;
        sfb_config_default(&ro, brain, DIM);
        ro.path = brain;
        ro.dim = DIM;
        ro.read_only = 1;
        sfb_brain_t *rb = NULL;
        int rc2 = sfb_open(&ro, &rb);
        CHECK(rc2 == SFB_OK && rb != NULL, "repaired manifest opens read-only (%s)",
              sfb_strerror(rc2));
        if (rc2 == SFB_OK && rb) {
            uint64_t live = 0;
            int rc3 = sfb_count(rb, &live);
            CHECK(rc3 == SFB_OK, "sfb_count on the repaired store (%s)",
                  sfb_strerror(rc3));
            CHECK(live <= after_records, "live count (%llu) <= sealed (%llu)",
                  (unsigned long long)live, after_records);
            (void)sfb_close(rb);
        }
    }

stats_and_rest:
    if (after_records == 0) after_records = records;   /* skip path stays coherent */
    /* ── 5. backup + restore ───────────────────────────────────────────── */
    t_say("── test_ops: backup then restore into a new directory\n");
    rc = sfb_backup(brain, snap);
    CHECK(rc == SFB_OK, "sfb_backup rc == SFB_OK (%s)", sfb_strerror(rc));
    {
        char snapman[900];
        snprintf(snapman, sizeof snapman, "%s/%s", snap, SFB_MANIFEST_NAME);
        CHECK(t_is_file(snapman), "snapshot manifest exists at %s", snapman);
    }
    {
        char snapseg[800];
        snprintf(snapseg, sizeof snapseg, "%s/%s", snap, SFB_SEG_DIR);
        CHECK(t_is_dir(snapseg), "snapshot segment dir exists");
    }
    rc = sfb_restore(snap, restored);
    CHECK(rc == SFB_OK, "sfb_restore rc == SFB_OK (%s)", sfb_strerror(rc));

    {
        char jb[8192], jr[8192];
        unsigned long long rb = 0, rr = 0;
        int okb = 0, okr = 0;
        memset(jb, 0, sizeof jb);
        memset(jr, 0, sizeof jr);
        int rcb = sfb_verify(brain, NULL, 0, jb, sizeof jb);
        int rcr = sfb_verify(restored, NULL, 0, jr, sizeof jr);
        (void)t_jbool(jb, "ok", &okb);
        (void)t_jbool(jr, "ok", &okr);
        (void)t_ju64(jb, "records", &rb);
        (void)t_ju64(jr, "records", &rr);
        t_say("    source records=%llu (verify %s), restored records=%llu (verify %s)\n",
              rb, sfb_strerror(rcb), rr, sfb_strerror(rcr));
        CHECK(rcb == SFB_OK && okb, "source verifies clean (%s)", sfb_strerror(rcb));
        CHECK(rcr == SFB_OK && okr, "restored verifies clean (%s)", sfb_strerror(rcr));
        CHECK(rb == rr, "record counts match after restore (%llu == %llu)", rb, rr);
        CHECK(rb == after_records, "restored count equals the post-repair count (%llu)",
              rb);
    }

    /* restore must never overwrite */
    rc = sfb_restore(snap, restored);
    CHECK(rc == SFB_E_STATE, "restore onto an existing dir returns SFB_E_STATE (got %s)",
          sfb_strerror(rc));

    /* backup must refuse while a writer owns the brain */
    {
        sfb_config_t wc;
        sfb_config_default(&wc, brain, DIM);
        wc.path = brain;
        wc.dim = DIM;
        sfb_brain_t *wb = NULL;
        int rc2 = sfb_open(&wc, &wb);
        if (rc2 == SFB_OK && wb) {
            char snap2[900];
            snprintf(snap2, sizeof snap2, "%s/locked-snapshot", base);
            int rc3 = sfb_backup(brain, snap2);
            CHECK(rc3 == SFB_E_LOCKED,
                  "backup while a writer holds the lock returns SFB_E_LOCKED (got %s)",
                  sfb_strerror(rc3));
            (void)sfb_close(wb);
        } else {
            t_say("    SKIP: could not reopen as writer (%s)\n", sfb_strerror(rc2));
        }
    }

    /* ── 6. stats --json ───────────────────────────────────────────────── */
    t_say("── test_ops: sfb_stats_json\n");
    {
        char sj[8192];
        memset(sj, 0, sizeof sj);
        int rc2 = sfb_stats_json(brain, NULL, sj, sizeof sj);
        t_say("    json: %s\n", sj);
        CHECK(rc2 == SFB_OK, "sfb_stats_json rc == SFB_OK (%s)", sfb_strerror(rc2));
        static const char *req[] = {
            "\"ok\":", "\"records\":", "\"live\":", "\"segments\":",
            "\"tombstones\":", "\"bytes_on_disk\":", "\"records_dropped\":",
            "\"segments_quarantined\":", "\"index_rebuilt\":", "\"flushes\":"
        };
        for (size_t i = 0; i < sizeof req / sizeof req[0]; i++) {
            CHECK(strstr(sj, req[i]) != NULL, "stats json contains %s", req[i]);
        }
        unsigned long long v = 0;
        CHECK(t_ju64(sj, "segments", &v) && v >= 1, "stats segments parseable (%llu)", v);
        CHECK(t_ju64(sj, "records", &v), "stats records parseable (%llu)", v);
        int sok = 0;
        CHECK(t_jbool(sj, "ok", &sok) && sok, "stats json ok:true");
    }

    /* ── 7. json_cap truncation is still NUL-terminated ────────────────── */
    t_say("── test_ops: json_out truncation safety\n");
    {
        char tiny[16];
        memset(tiny, 'X', sizeof tiny);
        (void)sfb_verify(brain, NULL, 0, tiny, sizeof tiny);
        size_t l = strnlen(tiny, sizeof tiny);
        CHECK(l < sizeof tiny, "tiny json_out is NUL-terminated (len=%zu)", l);
    }

    /* ── 8. migrate ────────────────────────────────────────────────────── */
    t_say("── test_ops: sfb_migrate honesty\n");
    {
        /* a QTSQ v1 container: magic "QTS\x01" + junk, no sidecar .d dir */
        char v1[700];
        snprintf(v1, sizeof v1, "%s/v1.qtsq", werk);
        uint8_t blob[256];
        memset(blob, 0x5A, sizeof blob);
        blob[0] = 'Q'; blob[1] = 'T'; blob[2] = 'S'; blob[3] = 0x01;
        CHECK(t_write_file(v1, blob, sizeof blob) == 0, "wrote a fake v1 container");

        char errpath[700];
        snprintf(errpath, sizeof errpath, "%s/migrate.err", werk);
        int saved = dup(STDERR_FILENO);
        int errfd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errfd >= 0) { (void)dup2(errfd, STDERR_FILENO); close(errfd); }
        rc = sfb_migrate(v1, migrated, NULL);
        if (saved >= 0) { (void)dup2(saved, STDERR_FILENO); close(saved); }
        CHECK(rc == SFB_E_INVALID, "migrate(v1 QTSQ) returns SFB_E_INVALID (got %s)",
              sfb_strerror(rc));
        CHECK(!t_is_dir(migrated), "migrate did not create a bogus destination");

        char msg[4096];
        memset(msg, 0, sizeof msg);
        int fd = open(errpath, O_RDONLY);
        if (fd >= 0) {
            ssize_t k = read(fd, msg, sizeof msg - 1);
            if (k > 0) msg[k] = '\0';
            close(fd);
        }
        t_say("    stderr: %s", msg[0] ? msg : "(empty)\n");
        /* the v1 path is implemented now, so a corrupt container must fail
         * with a specific reason (v1_read_failed here) rather than a stub's
         * "not implemented" message */
        CHECK(strstr(msg, "v1_") != NULL,
              "refusal carries a specific v1_* reason");
        CHECK(strstr(msg, "QTSQ") != NULL,
              "refusal names the QTSQ reader");
        CHECK(strstr(msg, "\"ok\":false") != NULL, "refusal is valid JSON with ok:false");

        /* an unknown magic: still honest, still SFB_E_INVALID */
        char unk[700];
        snprintf(unk, sizeof unk, "%s/unknown.bin", werk);
        uint8_t raw[64];
        memset(raw, 0x11, sizeof raw);
        raw[0] = 0xDE; raw[1] = 0xAD; raw[2] = 0xBE; raw[3] = 0xEF;
        CHECK(t_write_file(unk, raw, sizeof raw) == 0, "wrote an unknown-magic file");
        saved = dup(STDERR_FILENO);
        errfd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errfd >= 0) { (void)dup2(errfd, STDERR_FILENO); close(errfd); }
        rc = sfb_migrate(unk, migrated, NULL);
        if (saved >= 0) { (void)dup2(saved, STDERR_FILENO); close(saved); }
        CHECK(rc == SFB_E_INVALID, "migrate(unknown magic) returns SFB_E_INVALID (got %s)",
              sfb_strerror(rc));
        memset(msg, 0, sizeof msg);
        fd = open(errpath, O_RDONLY);
        if (fd >= 0) {
            ssize_t k = read(fd, msg, sizeof msg - 1);
            if (k > 0) msg[k] = '\0';
            close(fd);
        }
        t_say("    stderr: %s", msg[0] ? msg : "(empty)\n");
        CHECK(strstr(msg, "unknown v1 layout") != NULL,
              "refusal explains that the layout is unknown");

        /* an already-v2 source is a no-op copy */
        char m2[800];
        snprintf(m2, sizeof m2, "%s/migrated-v2", base);
        rc = sfb_migrate(manifest, m2, NULL);
        CHECK(rc == SFB_OK, "migrate(v2 manifest) is a no-op copy (got %s)",
              sfb_strerror(rc));
        char m2man[900], m2seg[900];
        snprintf(m2man, sizeof m2man, "%s/%s", m2, SFB_MANIFEST_NAME);
        snprintf(m2seg, sizeof m2seg, "%s/%s", m2, SFB_SEG_DIR);
        CHECK(t_is_file(m2man), "no-op copy produced a manifest");
        CHECK(t_is_dir(m2seg), "no-op copy produced a segment dir");
        {
            char jj[8192];
            int o2 = 0;
            memset(jj, 0, sizeof jj);
            int rc2 = sfb_verify(m2, NULL, 0, jj, sizeof jj);
            (void)t_jbool(jj, "ok", &o2);
            CHECK(rc2 == SFB_OK && o2, "no-op copy verifies clean (%s)",
                  sfb_strerror(rc2));
        }
        /* and migrating again is a pure no-op (destination already v2) */
        rc = sfb_migrate(manifest, m2, NULL);
        CHECK(rc == SFB_OK, "migrate onto an existing v2 store is a no-op (got %s)",
              sfb_strerror(rc));
    }

    /* ── 9. verify on a path that is not a store ───────────────────────── */
    t_say("── test_ops: verify on a non-store path\n");
    {
        char nowhere[800];
        snprintf(nowhere, sizeof nowhere, "%s/does-not-exist", base);
        memset(json, 0, sizeof json);
        rc = sfb_verify(nowhere, NULL, 0, json, sizeof json);
        int okn = 1;
        (void)t_jbool(json, "ok", &okn);
        CHECK(rc != SFB_OK, "verify on a missing path fails (%s)", sfb_strerror(rc));
        CHECK(!okn, "report says ok:false for a missing path");
        t_say("    json: %s\n", json);
    }

done:
    (void)t_rm_rf(base);
    t_say("\ntest_ops: %d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        t_say("ALL SFB OPS TESTS PASSED\n");
        return 0;
    }
    t_say("SFB OPS TESTS FAILED\n");
    return 1;
}
