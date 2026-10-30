/*
 * tests/test_w1_names.c — W1.5, second half: a segment file NAME is never reused.
 *
 * The plan asks W1.5 for two things: a monotonic generation counter, and that
 * a segment file name is never reused after a crash.  The first half is pinned
 * elsewhere (every publish bumps the manifest generation).  This suite pins
 * the second half, which had no test at all: next_seq() in sfb_store.c:826-852
 * refuses to hand out a `seg-<gen>-<seq>` name that already exists on disk,
 * because a crash between "segment written" and "manifest saved" leaves exactly
 * that: an orphan at the generation the *next* publish will compute again (the
 * manifest still holds the old generation, so the bump lands on the orphan's
 * number).
 *
 * The test plants junk at the name the next publish would naturally choose —
 * derived from the name observed on disk, not from a constant: same directory,
 * the generation the publish will bump to, seq 1 — and at two names above it.
 * Then it publishes and asserts:
 *
 *   (a) the publish SUCCEEDED,
 *   (b) the new segment's name is none of the planted names (it is seq 4),
 *   (c) every planted file is still there, byte for byte, and the manifest
 *       never claims a planted name as its own,
 *   (d) every record is readable after a reopen: the skip lost nothing.
 *
 * A second publish plants the natural name plus the name with the HIGHEST seq
 * in its generation (9).  That proves the skip never steps onto the far end of
 * the range either, and that the hole below the plant is filled (seq 2) rather
 * than leapfrogged to seq 10.
 *
 * The plants are unreferenced orphans by design, so an automatic reclaim sweep
 * is entitled to collect them at the NEXT open (W3.4).  Every persistence check
 * happens before the close — which is the moment the guarantee is about: the
 * flush that must not touch them.
 *
 * Deterministic: fixed vectors, no RNG, no network, temp dirs under /tmp only.
 * Build:  make test   (or cc -DSFB_TEST_HAVE_CRYPTO ... tests/test_w1_names.c)
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <dirent.h>
#include <fcntl.h>

/* ── tiny harness (same shape as tests/test_w3_storage.c) ─────────────── */

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

/* ── the on-disk naming scheme this test reasons about ───────────────────
 * sfb_store.c:283-293 builds every segment name as `seg-%06u-%06u.qtsq` inside
 * <brain>/<SFB_SEG_DIR>.  The test derives the next name from a name it READ
 * OFF THE DISK rather than from a constant, so a change to either half of the
 * scheme breaks this suite loudly instead of making it agree with itself.
 * ---------------------------------------------------------------------- */

static int path_join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int seg_dir_of(char *out, size_t cap, const char *brain)
{
    return path_join(out, cap, brain, SFB_SEG_DIR);
}

/* Segment path for a (gen, seq) pair — exactly what the store would write. */
static int seg_name(char *out, size_t cap, uint32_t gen, uint32_t seq)
{
    int n = snprintf(out, cap, "seg-%06u-%06u.qtsq", (unsigned)gen, (unsigned)seq);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Inverse of seg_name(): recover (gen, seq) from a name found on disk. */
static int parse_seg_name(const char *name, uint32_t *gen, uint32_t *seq)
{
    unsigned g = 0, q = 0;
    char tail[16] = "";

    if (sscanf(name, "seg-%6u-%6u.%15s", &g, &q, tail) != 3)
        return -1;
    if (strcmp(tail, "qtsq") != 0)
        return -1;
    *gen = (uint32_t)g;
    *seq = (uint32_t)q;
    return 0;
}

/* Files in `dir` matching seg-<gen>-<seq>.qtsq, sorted.  Zero-padded names sort
 * lexically in generation order, which is what the details print. */
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

static int name_in(char list[][256], int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}

/* How many names in `cur` are absent from `prev`; the first one is copied out.
 * A publish writes exactly one segment here, so 1 means "one new file". */
static int new_files(char prev[][256], int nprev, char cur[][256], int ncur,
                     char *out, size_t cap)
{
    int i, n = 0;

    for (i = 0; i < ncur; i++) {
        if (name_in(prev, nprev, cur[i])) continue;
        if (out != NULL && n == 0) snprintf(out, cap, "%s", cur[i]);
        n++;
    }
    return n;
}

/* ── planting junk where the next publish wants to write ───────────────── */

#define W1_JUNK_LEN 512u

/* Junk that is deliberately NOT a valid segment: a real segment starts with
 * the SFB_MAGIC_SEGMENT header, so nothing here can be mistaken for data. */
static size_t make_junk(uint8_t *buf, size_t cap, uint32_t tag)
{
    size_t len = cap < W1_JUNK_LEN ? cap : W1_JUNK_LEN;
    size_t i;
    int n = snprintf((char *)buf, len, "PLANTED-JUNK %02u: not a segment",
                     (unsigned)tag);

    if (n < 0) n = 0;
    for (i = (size_t)n; i < len; i++)
        buf[i] = (uint8_t)((i * 37u + tag * 11u + 0x5Au) & 0xFFu);
    return len;
}

static int plant_file(const char *path, const void *buf, size_t len)
{
    ssize_t n;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
                  , 0644);

    if (fd < 0) return -1;
    n = write(fd, buf, len);
    if (close(fd) != 0) return -1;
    return (n == (ssize_t)len) ? 0 : -1;
}

/* Present AND byte-identical.  A rename over the plant would change both the
 * size and the bytes, so one comparison covers (c) either way. */
static int plant_intact(const char *path, const void *want, size_t len)
{
    uint8_t got[W1_JUNK_LEN + 64u];
    struct stat st;
    ssize_t n;
    int fd;

    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if ((size_t)st.st_size != len) return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, got, sizeof got);
    (void)close(fd);
    return (n == (ssize_t)len) && memcmp(got, want, len) == 0;
}

/* ── brain helpers ────────────────────────────────────────────────────── */

static int brain_open_at(const char *path, uint32_t dim, sfb_brain_t **out)
{
    sfb_config_t cfg;

    sfb_config_default(&cfg, path, dim);
    /* A flush must record rows, not forget them: with the default floor a
     * decay pass could demote rows and change what "all records readable"
     * means.  This suite never ticks decay anyway, but the fixture should not
     * depend on that. */
    cfg.decay_floor = 1e-30;
    return sfb_open(&cfg, out);
}

/* Remember `n` rows with a per-phase seed so two phases never produce the same
 * vector (an identical vector plus a similar text could fold under W5.1 dedup
 * and would silently change the expected record count). */
static int seed_rows(sfb_brain_t *b, uint32_t dim, uint32_t n, const char *tag,
                     uint32_t seed0, uint32_t *ids)
{
    char text[192];
    float *v = (float *)malloc((size_t)dim * sizeof(float));
    uint32_t i;
    int rc = SFB_OK;

    if (v == NULL) return SFB_E_NOMEM;
    for (i = 0; i < n; i++) {
        sfb_remember_result_t rr;

        fill_vec(v, dim, seed0 + i);
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

#define W1_DIM    128u
#define W1_N1      40u
#define W1_N2      40u
#define W1_N3       5u
#define W1_TOTAL  (W1_N1 + W1_N2 + W1_N3)
/* The number of contiguous plants in phase A.  The expected seq below is
 * written out literally (4) on purpose: deriving it from this constant would
 * let a change to the plant run make the test agree with itself. */
#define W1_PLANTS   3u

/* ── the test ─────────────────────────────────────────────────────────── */

static void test_w15_name_reuse(void)
{
    sfb_brain_t *b = NULL;
    sfb_flush_result_t fr;
    sfb_stats_t st;
    char path[512], sd[1024], detail[320];
    char before[64][256], prev[64][256], after[64][256], after2[64][256];
    char plain[W1_PLANTS][256];
    char pb[2][256];
    char new_name[256] = "";
    char new2_name[256] = "";
    uint8_t junk[W1_PLANTS][W1_JUNK_LEN];
    uint8_t pjunk[2][W1_JUNK_LEN];
    size_t junk_len[W1_PLANTS] = { 0, 0, 0 };
    size_t pjunk_len[2] = { 0, 0 };
    uint32_t gen_obs = 0, seq_obs = 0, gen_next, gen_next2;
    uint32_t new_gen = 0, new_seq = 0, new2_gen = 0, new2_seq = 0;
    uint64_t man_gen = 0, live = 0;
    int nf = 0, nprev = 0, nfa = 0, nfa2 = 0, nnew = 0, nnew2 = 0;
    int i, rc, frc, created;

    printf("\n── W1.5 name reuse: a publish must never write onto an existing name\n");
    snprintf(path, sizeof path, "%s/w15", g_root);

    rc = brain_open_at(path, W1_DIM, &b);
    check(rc == SFB_OK, "W1.5 fixture open", rc_name(rc));
    if (rc != SFB_OK) return;
    if (seg_dir_of(sd, sizeof sd, path) != 0) { (void)sfb_close(b); return; }

    /* ── 1. get a real segment on disk and READ its name ───────────────── */
    rc = seed_rows(b, W1_DIM, W1_N1, "w15a", 1u, NULL);
    check(rc == SFB_OK, "W1.5 seeded 40 rows", rc_name(rc));
    if (rc != SFB_OK) { (void)sfb_close(b); return; }

    memset(&fr, 0, sizeof fr);
    frc = sfb_flush(b, &fr);
    man_gen = b->store.man.generation;
    snprintf(detail, sizeof detail,
             "sfb_flush %s, segments_sealed=%llu, manifest generation=%llu",
             rc_name(frc), (unsigned long long)fr.segments_sealed,
             (unsigned long long)man_gen);
    check(frc == SFB_OK && fr.segments_sealed == 1u,
          "W1.5 the first flush seals exactly one segment", detail);
    if (frc != SFB_OK || fr.segments_sealed != 1u) { (void)sfb_close(b); return; }

    nf = seg_files(sd, before, 64);
    snprintf(detail, sizeof detail, "%d segment file(s) in %s%s%s", nf, sd,
             nf > 0 ? ": " : "", nf > 0 ? before[0] : "");
    check(nf == 1 && parse_seg_name(before[0], &gen_obs, &seq_obs) == 0,
          "W1.5 the sealed segment's name parses as seg-<gen>-<seq>.qtsq", detail);
    if (nf != 1 || parse_seg_name(before[0], &gen_obs, &seq_obs) != 0) {
        (void)sfb_close(b);
        return;
    }

    snprintf(detail, sizeof detail,
             "observed %s -> gen=%u seq=%u; manifest generation=%llu",
             before[0], (unsigned)gen_obs, (unsigned)seq_obs,
             (unsigned long long)man_gen);
    check((uint64_t)gen_obs == man_gen && seq_obs == 1u,
          "W1.5 the observed name is seg-<manifest generation>-1", detail);

    /* The publish stamps `gen = ++man.generation` and takes the lowest free seq
     * in it (sfb_store.c:893-903), so with no manifest entry for the next
     * generation the natural choice is (man_gen + 1, 1).  This is the name a
     * crash-orphan would occupy, and the name we plant junk at. */
    gen_next = gen_obs + 1u;

    /* ── 2. plant the natural next name and the two above it ───────────── */
    created = 0;
    for (i = 0; i < (int)W1_PLANTS; i++) {
        char full[4096];

        junk_len[i] = make_junk(junk[i], sizeof junk[i], (uint32_t)(i + 1));
        if (seg_name(plain[i], sizeof plain[i], gen_next, (uint32_t)(i + 1)) != 0)
            continue;
        if (path_join(full, sizeof full, sd, plain[i]) != 0) continue;
        if (plant_file(full, junk[i], junk_len[i]) == 0) created++;
    }
    snprintf(detail, sizeof detail, "planted %s %s %s (gen %u, seq 1..%u)",
             plain[0], plain[1], plain[2], (unsigned)gen_next, (unsigned)W1_PLANTS);
    check(created == (int)W1_PLANTS,
          "W1.5 planted junk at the natural next name and one above each", detail);
    if (created != (int)W1_PLANTS) { (void)sfb_close(b); return; }

    /* Snapshot the directory WITH the plants in it: the new-file diff below is
     * "what the publish added", not "what the plants added". */
    nprev = seg_files(sd, prev, 64);

    /* ── 3. publish: the store must route around all three ─────────────── */
    rc = seed_rows(b, W1_DIM, W1_N2, "w15b", 5001u, NULL);
    check(rc == SFB_OK, "W1.5 seeded 40 more rows", rc_name(rc));
    if (rc != SFB_OK) { (void)sfb_close(b); return; }

    memset(&fr, 0, sizeof fr);
    frc = sfb_flush(b, &fr);
    snprintf(detail, sizeof detail,
             "sfb_flush %s, ok=%d, segments_sealed=%llu, records_written=%llu",
             rc_name(frc), fr.ok, (unsigned long long)fr.segments_sealed,
             (unsigned long long)fr.records_written);
    check(frc == SFB_OK && fr.ok == 1 && fr.segments_sealed == 1u &&
              fr.records_written == (uint64_t)W1_N2,
          "(a) the publish that had to skip a planted name SUCCEEDED", detail);

    nfa = seg_files(sd, after, 64);
    nnew = new_files(prev, nprev, after, nfa, new_name, sizeof new_name);
    snprintf(detail, sizeof detail,
             "%d file(s) before the publish (1 segment + %u plants), %d after, "
             "%d new (%s)", nprev, (unsigned)W1_PLANTS, nfa, nnew,
             nnew > 0 ? new_name : "none");
    check(nnew == 1,
          "(b) exactly one new segment file appeared (the publish wrote once)",
          detail);

    snprintf(detail, sizeof detail, "new segment %s; planted %s, %s, %s",
             new_name[0] ? new_name : "(none)", plain[0], plain[1], plain[2]);
    check(nnew == 1 && !name_in(plain, (int)W1_PLANTS, new_name),
          "(b) the new segment's name is NOT the planted name", detail);

    if (nnew == 1 && parse_seg_name(new_name, &new_gen, &new_seq) == 0) {
        snprintf(detail, sizeof detail,
                 "new segment %s -> gen=%u seq=%u; three plants occupy gen %u "
                 "seq 1..%u, so the skip must land on seq 4",
                 new_name, (unsigned)new_gen, (unsigned)new_seq,
                 (unsigned)gen_next, (unsigned)W1_PLANTS);
        check(new_gen == gen_next && new_seq == 4u,
              "(b) the skip walked every plant: new name is seq 4, not seq 1",
              detail);
    } else {
        check(0, "(b) the skip walked every plant: new name is seq 4, not seq 1",
              "the new file's name did not parse");
    }

    /* ── 4. (c) the plants survived, and the manifest never claims one ─── */
    {
        int present = 0, intact = 0;

        for (i = 0; i < (int)W1_PLANTS; i++) {
            char full[4096];

            if (path_join(full, sizeof full, sd, plain[i]) != 0) continue;
            if (access(full, F_OK) == 0) present++;
            if (plant_intact(full, junk[i], junk_len[i]) == 1) intact++;
        }
        snprintf(detail, sizeof detail,
                 "%d/%u planted files present, %d/%u byte-identical",
                 present, (unsigned)W1_PLANTS, intact, (unsigned)W1_PLANTS);
        check(present == (int)W1_PLANTS,
              "(c) every planted file is still there", detail);
        check(intact == (int)W1_PLANTS,
              "(c) every planted file still holds its planted bytes", detail);
    }
    {
        uint32_t k, pg = 0, pq = 0;
        int refs = 0;

        for (i = 0; i < (int)W1_PLANTS; i++) {
            if (parse_seg_name(plain[i], &pg, &pq) != 0) continue;
            for (k = 0; k < b->store.man.nseg; k++) {
                if (b->store.man.segs[k].gen == pg &&
                    b->store.man.segs[k].seq == pq)
                    refs++;
            }
        }
        snprintf(detail, sizeof detail,
                 "%u manifest entries, %d of them on a planted (gen,seq)",
                 b->store.man.nseg, refs);
        check(refs == 0,
              "(c) the manifest does not claim a planted name as its own", detail);
    }

    /* ── 5. a planted name with the HIGHEST seq in its generation ──────── */
    gen_next2 = gen_next + 1u;
    created = 0;
    for (i = 0; i < 2; i++) {
        char full[4096];
        uint32_t seq = (i == 0) ? 1u : 9u;   /* natural candidate + highest */

        pjunk_len[i] = make_junk(pjunk[i], sizeof pjunk[i], (uint32_t)(i + 11));
        if (seg_name(pb[i], sizeof pb[i], gen_next2, seq) != 0) continue;
        if (path_join(full, sizeof full, sd, pb[i]) != 0) continue;
        if (plant_file(full, pjunk[i], pjunk_len[i]) == 0) created++;
    }
    snprintf(detail, sizeof detail, "planted %s (seq 1) and %s (seq 9, the "
             "highest seq any publish has ever reached here)", pb[0], pb[1]);
    check(created == 2,
          "W1.5 planted the natural name AND the highest-seq name", detail);
    nprev = seg_files(sd, prev, 64);        /* with the phase-B plants in it */

    rc = seed_rows(b, W1_DIM, W1_N3, "w15c", 9001u, NULL);
    check(rc == SFB_OK, "W1.5 seeded the last 5 rows", rc_name(rc));
    if (rc != SFB_OK) { (void)sfb_close(b); return; }

    memset(&fr, 0, sizeof fr);
    frc = sfb_flush(b, &fr);
    snprintf(detail, sizeof detail,
             "sfb_flush %s, segments_sealed=%llu, records_written=%llu",
             rc_name(frc), (unsigned long long)fr.segments_sealed,
             (unsigned long long)fr.records_written);
    check(frc == SFB_OK && fr.segments_sealed == 1u,
          "(a) the second publish also succeeds", detail);

    nfa2 = seg_files(sd, after2, 64);
    nnew2 = new_files(prev, nprev, after2, nfa2, new2_name, sizeof new2_name);
    if (nnew2 == 1 && parse_seg_name(new2_name, &new2_gen, &new2_seq) == 0) {
        snprintf(detail, sizeof detail,
                 "new segment %s -> gen=%u seq=%u (plants: %s seq 1, %s seq 9)",
                 new2_name, (unsigned)new2_gen, (unsigned)new2_seq,
                 pb[0], pb[1]);
        check(new2_gen == gen_next2 && new2_seq != 1u && new2_seq != 9u,
              "(b) the highest-seq plant is not reused as a write target", detail);
        snprintf(detail, sizeof detail,
                 "new segment %s has seq %u; the free hole is seq 2, so a skip "
                 "that jumps to seq 10 is a different (wrong) policy",
                 new2_name, (unsigned)new2_seq);
        check(new2_gen == gen_next2 && new2_seq == 2u,
              "(b) the skip fills the hole below the plant (seq 2), not seq 10",
              detail);
    } else {
        check(0, "(b) the highest-seq plant is not reused as a write target",
              "the new file's name did not parse");
        check(0, "(b) the skip fills the hole below the plant (seq 2), not seq 10",
              "the new file's name did not parse");
    }
    {
        char full[4096];
        int intact = 0;

        if (path_join(full, sizeof full, sd, pb[1]) == 0)
            intact = plant_intact(full, pjunk[1], pjunk_len[1]);
        snprintf(detail, sizeof detail, "%s %s (%u bytes)", pb[1],
                 intact ? "still holds its planted bytes" : "CHANGED",
                 (unsigned)pjunk_len[1]);
        check(intact == 1, "(c) the highest-seq planted file is unchanged",
              detail);
    }

    /* ── 6. (d) nothing was lost: reopen and read every row ─────────────
     * The plants are orphans, so this reopen's reclaim sweep may collect them;
     * that is W3.4 doing its job, and it is why no plant check comes after
     * here.  What must survive is the DATA the publishes wrote. */
    (void)sfb_close(b);
    b = NULL;
    rc = brain_open_at(path, W1_DIM, &b);
    check(rc == SFB_OK, "(d) the store reopens after both skips", rc_name(rc));
    if (rc != SFB_OK) return;

    live = 0;
    (void)sfb_count(b, &live);
    (void)sfb_stats_get(b, &st);
    snprintf(detail, sizeof detail,
             "sfb_count=%llu, stats.live=%llu, expected %u (40 + 40 + 5)",
             (unsigned long long)live, (unsigned long long)st.live,
             (unsigned)W1_TOTAL);
    check(live == (uint64_t)W1_TOTAL && st.live == (uint64_t)W1_TOTAL,
          "(d) all 85 records are readable after the reopen", detail);
    snprintf(detail, sizeof detail,
             "records_dropped=%llu, segments_quarantined=%llu, segments=%llu",
             (unsigned long long)st.records_dropped,
             (unsigned long long)st.segments_quarantined,
             (unsigned long long)st.segments);
    check(st.records_dropped == 0 && st.segments_quarantined == 0,
          "(d) the skip lost nothing: no dropped records, no quarantines", detail);

    {
        sfb_hit_t *hits = (sfb_hit_t *)calloc((size_t)W1_TOTAL + 1u, sizeof *hits);
        uint32_t n = 0, ca = 0, cb = 0, cc = 0, other = 0;

        if (hits != NULL) {
            (void)sfb_list(b, 0, W1_TOTAL + 1u, hits, &n);
            for (i = 0; i < (int)n; i++) {
                const char *t = hits[i].text;

                if (t == NULL) { other++; continue; }
                if (strncmp(t, "row w15a ", 9) == 0) ca++;
                else if (strncmp(t, "row w15b ", 9) == 0) cb++;
                else if (strncmp(t, "row w15c ", 9) == 0) cc++;
                else other++;
            }
        }
        snprintf(detail, sizeof detail,
                 "sfb_list returned %u rows: w15a=%u w15b=%u w15c=%u other=%u",
                 n, ca, cb, cc, other);
        check(hits != NULL && n == W1_TOTAL && other == 0 &&
                  ca == W1_N1 && cb == W1_N2 && cc == W1_N3,
              "(d) every row of every publish is present by text", detail);
        free(hits);
    }
    (void)sfb_close(b);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char tmpl[] = "/tmp/sfb_w1_names.XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    printf("── W1.5 name-reuse tests (tmp dir %s)\n", g_root);

    test_w15_name_reuse();

    rm_rf(g_root);
    printf("── %d checks, %d passed, %d failed\n", g_pass + g_fail, g_pass, g_fail);
    if (g_fail != 0) {
        printf("TEST_W1_NAMES FAILED\n");
        return 1;
    }
    printf("ALL W1 NAME TESTS PASSED\n");
    return 0;
}
