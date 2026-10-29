/*
 * tests/test_store.c — self-contained unit tests for sfb_store.c.
 *
 *   cc -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE -I. \
 *      sfb_util.c sfb_store.c tests/test_store.c -o /tmp/sfb_test_store
 *   /tmp/sfb_test_store
 *
 * sfb_crypto.c is owned by another module.  Until it lands, this file carries
 * WEAK stand-ins (test_gcm_stub below) so the store can be exercised end to
 * end; a real sfb_crypto.o on the link line overrides them.  Build with
 * -DSFB_TEST_HAVE_CRYPTO once sfb_crypto.c exists to drop them entirely.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>

/* ── test_gcm_stub ──────────────────────────────────────────────────────
 * TEMPORARY weak placeholders for sfb_crypto.c.  They are a reversible XOR
 * "cipher" with a key+IV dependent keystream and a tag that covers both the
 * ciphertext and the key, which is enough for the store's tests (round trip,
 * wrong-key ⇒ SFB_E_CORRUPT) and is not pretending to be AES-GCM.
 * ---------------------------------------------------------------------- */
#if !defined(SFB_TEST_HAVE_CRYPTO)

static void stub_xor(uint8_t *buf, size_t len, const uint8_t key[SFB_KEY_LEN],
                     const uint8_t iv[SFB_IV_LEN])
{
    size_t i;

    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)(buf[i] ^ key[i % SFB_KEY_LEN] ^ iv[i % SFB_IV_LEN]);
}

static void stub_tag(const void *ct, size_t ct_len,
                     const uint8_t key[SFB_KEY_LEN], uint8_t tag[SFB_TAG_LEN])
{
    uint32_t a = sfb_crc32c(ct, ct_len);
    uint32_t b = sfb_crc32c(key, SFB_KEY_LEN);

    memset(tag, 0, SFB_TAG_LEN);
    tag[0] = (uint8_t)((a ^ b) & 0xFFu);
    tag[1] = (uint8_t)(((a ^ b) >> 8) & 0xFFu);
    tag[2] = (uint8_t)(((a ^ b) >> 16) & 0xFFu);
    tag[3] = (uint8_t)(((a ^ b) >> 24) & 0xFFu);
}

__attribute__((weak))
int sfb_random_bytes(void *out, size_t len)
{
    uint8_t *p = (uint8_t *)out;
    size_t i;

    if (p == NULL && len != 0)
        return SFB_E_INVALID;
    for (i = 0; i < len; i++)
        p[i] = (uint8_t)(0xA5u + (unsigned)i * 13u);
    return SFB_OK;
}

__attribute__((weak))
int sfb_kdf(const char *passphrase, const uint8_t salt[SFB_SALT_LEN],
            uint8_t key_out[SFB_KEY_LEN])
{
    uint32_t h;
    size_t i;

    if (passphrase == NULL || key_out == NULL)
        return SFB_E_INVALID;
    h = sfb_crc32c(passphrase, strlen(passphrase));
    for (i = 0; i < SFB_KEY_LEN; i++)
        key_out[i] = (uint8_t)(salt[i % SFB_SALT_LEN]
                               ^ (uint8_t)(h >> ((i % 4) * 8)) ^ (uint8_t)i);
    return SFB_OK;
}

__attribute__((weak))
int sfb_gcm_seal(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len, const void *pt, size_t pt_len,
                 void *ct_out, uint8_t tag_out[SFB_TAG_LEN])
{
    (void)aad;                                  /* stub: AAD not mixed in   */
    (void)aad_len;
    memcpy(ct_out, pt, pt_len);
    stub_xor((uint8_t *)ct_out, pt_len, key, iv);
    stub_tag(ct_out, pt_len, key, tag_out);
    return SFB_OK;
}

__attribute__((weak))
int sfb_gcm_open(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len, const void *ct, size_t ct_len,
                 const uint8_t tag[SFB_TAG_LEN], void *pt_out)
{
    uint8_t want[SFB_TAG_LEN];

    (void)aad;
    (void)aad_len;
    stub_tag(ct, ct_len, key, want);
    if (memcmp(want, tag, SFB_TAG_LEN) != 0)
        return SFB_E_CORRUPT;
    memcpy(pt_out, ct, ct_len);
    stub_xor((uint8_t *)pt_out, ct_len, key, iv);
    return SFB_OK;
}

#endif /* !SFB_TEST_HAVE_CRYPTO */

/* ── harness ─────────────────────────────────────────────────────────────  */
static int g_checks;
static int g_fail;
static char g_base[256];

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want)                                                  \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        g_checks++;                                                          \
        if (g_ != w_) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n",           \
                    __FILE__, __LINE__, #got, g_, w_);                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define DIM  64u
#define RECS 4u
#define PAYLOAD ((size_t)RECS * DIM * sizeof(float))   /* 1024 bytes        */
#define SEG_HDR 48u

static uint8_t g_data[3][PAYLOAD];

static void fill_data(void)
{
    unsigned i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < PAYLOAD; j++)
            g_data[i][j] = (uint8_t)(0x11u * (i + 1u) + j * 7u + 3u);
}

static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    char **names = NULL;
    size_t n = 0, cap = 0, i;
    char p[4096];

    if (d != NULL) {
        struct dirent *de;

        while ((de = readdir(d)) != NULL) {
            char **tmp;

            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 16;

                tmp = (char **)realloc(names, ncap * sizeof *tmp);
                if (tmp == NULL)
                    break;
                names = tmp;
                cap = ncap;
            }
            names[n] = strdup(de->d_name);
            if (names[n] != NULL)
                n++;
        }
        (void)closedir(d);
    }
    for (i = 0; i < n; i++) {
        struct stat st;

        if (sfb_path_join(p, sizeof p, path, names[i]) == SFB_OK
            && lstat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                rm_rf(p);
            else
                (void)unlink(p);
        }
        free(names[i]);
    }
    free(names);
    (void)rmdir(path);
}

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    struct stat st;
    uint8_t *b;
    int fd;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    b = (uint8_t *)malloc((size_t)st.st_size + 1);
    if (b == NULL)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(b);
        return -1;
    }
    if (sfb_pread_all(fd, b, (size_t)st.st_size, 0) != SFB_OK) {
        (void)close(fd);
        free(b);
        return -1;
    }
    (void)close(fd);
    *out = b;
    *len = (size_t)st.st_size;
    return 0;
}

static int write_file(const char *path, const void *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc;

    if (fd < 0)
        return -1;
    rc = sfb_write_all(fd, buf, len);
    if (close(fd) != 0)
        return -1;
    return rc == SFB_OK ? 0 : -1;
}

static void cfg_init(sfb_config_t *cfg, const char *dir, uint32_t dim,
                     const char *key)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->path = dir;
    cfg->dim = dim;
    cfg->mode = SFB_MODE_COGNITIVE;
    cfg->decay = SFB_DECAY_EXPONENTIAL;
    cfg->half_life_secs = 86400.0;
    cfg->decay_floor = 0.05;
    cfg->pins_immune = 1;
    cfg->dedup_text_similarity = 0.85;
    cfg->dedup_action = SFB_DEDUP_MERGE;
    cfg->ef_search = 64;
    cfg->no_lock = 1;
    cfg->key = key;
}

/* fresh store + three sealed segments, all in generation 1 */
static int build_store(const char *dir, sfb_store_t *s, sfb_config_t *cfg,
                       const char *key)
{
    int fresh = 0, rc;
    unsigned i;

    cfg_init(cfg, dir, DIM, key);
    rc = sfb_store_open(s, cfg, &fresh);
    if (rc != SFB_OK)
        return rc;
    if (fresh != 1)
        return SFB_E_STATE;

    for (i = 0; i < 3; i++) {
        uint32_t seq = 0;

        rc = sfb_store_segment_write(s, g_data[i], PAYLOAD, RECS,
                                     i * RECS + 1u, (i + 1u) * RECS, &seq);
        if (rc != SFB_OK)
            return rc;
        if (seq != 1u)                    /* one publish per generation      */
            return SFB_E_STATE;
    }
    return SFB_OK;
}

static void seg_dir_of(char *out, size_t cap, const char *dir, const char *name)
{
    (void)sfb_snprintf(out, cap, "%s/brain.qtsq.d/%s", dir, name);
}

static void seg_file(char *out, size_t cap, const char *dir, unsigned gen,
                     unsigned seq)
{
    (void)sfb_snprintf(out, cap, "%s/brain.qtsq.d/seg-%06u-%06u.qtsq",
                       dir, gen, seq);
}

static void manifest_of(char *out, size_t cap, const char *dir)
{
    (void)sfb_snprintf(out, cap, "%s/brain.qtsq", dir);
}

static void wal_of(char *out, size_t cap, const char *dir)
{
    (void)sfb_snprintf(out, cap, "%s/brain.qtsq.wal.json", dir);
}

static int all_segments_ok(const sfb_store_t *s)
{
    uint32_t i;

    for (i = 0; i < s->man.nseg; i++) {
        if (sfb_store_segment_verify(s, &s->man.segs[i]) != SFB_OK)
            return 0;
    }
    return 1;
}

/* ── 1. write / reopen / verify ─────────────────────────────────────────  */
static void test_write_reopen(void)
{
    char dir[512], man[600], p[700];
    sfb_store_t s;
    sfb_config_t cfg;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    uint64_t bytes;
    int fresh = -1;
    unsigned i;

    snprintf(dir, sizeof dir, "%s/store1", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);

    CHECK_EQ(s.man.nseg, 3u);
    CHECK_EQ(s.man.count, 3u * RECS);
    CHECK_EQ(s.man.generation, 3u);       /* one publish per sealed segment  */
    CHECK_EQ(s.man.flags & 0x0001u, 0u);      /* not encrypted             */
    CHECK(s.encrypted == 0);
    CHECK_EQ(s.man.total_bytes, 3u * (SEG_HDR + PAYLOAD));

    manifest_of(man, sizeof man, dir);
    seg_file(p, sizeof p, dir, 1, 1);
    CHECK(access(man, F_OK) == 0);
    CHECK(access(p, F_OK) == 0);

    /* the manifest really is magic + version 2 + a trailing CRC32C */
    CHECK_EQ(read_file(man, &raw, &raw_len), 0);
    CHECK(raw_len == 48u + 3u * 32u + 4u);
    if (raw != NULL) {
        uint32_t magic = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8)
                       | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
        uint32_t tail = (uint32_t)raw[raw_len - 4]
                      | ((uint32_t)raw[raw_len - 3] << 8)
                      | ((uint32_t)raw[raw_len - 2] << 16)
                      | ((uint32_t)raw[raw_len - 1] << 24);

        CHECK_EQ(magic, SFB_MAGIC_MANIFEST);
        CHECK_EQ(raw[4], SFB_FORMAT_VERSION & 0xFF);
        CHECK_EQ(raw[5], 0);
        CHECK_EQ(tail, sfb_crc32c(raw, raw_len - 4));
        free(raw);
        raw = NULL;
    }

    bytes = sfb_store_bytes_on_disk(&s);
    CHECK_EQ(bytes, raw_len + 3u * (SEG_HDR + PAYLOAD));

    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* ── reopen: same table, same digests, same bytes ──────────────────── */
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.nseg, 3u);
    CHECK_EQ(s.man.count, 3u * RECS);
    CHECK_EQ(s.man.dim, DIM);

    for (i = 0; i < 3; i++) {
        const sfb_seg_entry_t *e = &s.man.segs[i];
        void *out = NULL;
        size_t len = 0;

        CHECK_EQ(e->gen, i + 1u);         /* generation bumps on every write */
        CHECK_EQ(e->seq, 1u);
        CHECK_EQ(e->records, RECS);
        CHECK_EQ(e->min_id, i * RECS + 1u);
        CHECK_EQ(e->max_id, (i + 1u) * RECS);
        CHECK_EQ(e->length, SEG_HDR + PAYLOAD);
        CHECK(e->digest != 0);

        CHECK_EQ(sfb_store_segment_read(&s, e, &out, &len), SFB_OK);
        CHECK_EQ(len, PAYLOAD);
        CHECK(out != NULL && memcmp(out, g_data[i], PAYLOAD) == 0);
        free(out);

        CHECK_EQ(sfb_store_segment_verify(&s, e), SFB_OK);
    }
    CHECK(all_segments_ok(&s));

    /* re-saving an unchanged manifest is idempotent and stays loadable */
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_OK);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_OK);
    CHECK_EQ(s.man.nseg, 3u);

    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* a missing segment file is reported, not silently skipped */
    {
        sfb_store_t s2;
        sfb_seg_entry_t ghost;

        CHECK_EQ(sfb_store_open(&s2, &cfg, &fresh), SFB_OK);
        memset(&ghost, 0, sizeof ghost);
        ghost.gen = 1;
        ghost.seq = 99;
        ghost.records = 1;
        ghost.length = SEG_HDR + 16;
        {
            void *out = NULL;
            size_t len = 0;

            CHECK_EQ(sfb_store_segment_read(&s2, &ghost, &out, &len),
                     SFB_E_NOTFOUND);
            CHECK_EQ(sfb_store_segment_verify(&s2, &ghost), SFB_E_NOTFOUND);
        }
        CHECK_EQ(sfb_store_close(&s2), SFB_OK);
    }

    rm_rf(dir);
}

/* ── 2. one flipped byte in a segment ──────────────────────────────────── */
static void test_corrupt_segment(void)
{
    char dir[512], p[700];
    sfb_store_t s;
    sfb_config_t cfg;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    void *out = NULL;
    size_t len = 0;
    int fresh = 0;

    snprintf(dir, sizeof dir, "%s/store2", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);

    seg_file(p, sizeof p, dir, s.man.segs[1].gen, s.man.segs[1].seq);
    CHECK_EQ(read_file(p, &raw, &raw_len), 0);
    CHECK(raw_len == SEG_HDR + PAYLOAD);
    CHECK(raw != NULL);

    /* flip one payload bit */
    raw[SEG_HDR + 10] ^= 0x01u;
    CHECK_EQ(write_file(p, raw, raw_len), 0);
    free(raw);
    raw = NULL;

    CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[1], &out, &len),
             SFB_E_CORRUPT);
    CHECK(out == NULL && len == 0);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[1]), SFB_E_CORRUPT);
    /* the other two are still fine */
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]), SFB_OK);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[2]), SFB_OK);

    /* flip a header bit instead */
    CHECK_EQ(read_file(p, &raw, &raw_len), 0);
    if (raw != NULL) {
        raw[0] ^= 0xFFu;                        /* magic                     */
        CHECK_EQ(write_file(p, raw, raw_len), 0);
        free(raw);
        raw = NULL;
        CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[1], &out, &len),
                 SFB_E_CORRUPT);
        CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[1]), SFB_E_CORRUPT);
    }

    /* truncate it instead: the length no longer matches the manifest */
    CHECK_EQ(read_file(p, &raw, &raw_len), 0);
    if (raw != NULL) {
        CHECK_EQ(write_file(p, raw, raw_len - 1u), 0);
        free(raw);
        raw = NULL;
        CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[1]), SFB_E_CORRUPT);
        CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[1], &out, &len),
                 SFB_E_CORRUPT);
    }

    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);

    /* A manifest whose CRC is fine but whose per-segment digest is wrong
     * (a stale table, a swapped entry) must still be rejected: the payload
     * CRC alone is not the contract, the manifest digest is. */
    snprintf(dir, sizeof dir, "%s/store2b", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    s.man.segs[0].digest = s.man.segs[1].digest;      /* equal file lengths */
    s.man.segs[0].digest ^= 0x5A5A5A5Au;
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[0], &out, &len),
             SFB_E_CORRUPT);
    CHECK(out == NULL && len == 0);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]), SFB_E_CORRUPT);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[1]), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);

    /* A manifest whose own CRC is fine but whose per-segment digest is wrong
     * (a stale table, a swapped entry) must still be rejected: matching the
     * payload CRC is not the contract, matching the manifest digest is. */
    snprintf(dir, sizeof dir, "%s/store2b", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    s.man.segs[0].digest = s.man.segs[1].digest ^ 0x5A5A5A5Au;
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[0], &out, &len),
             SFB_E_CORRUPT);
    CHECK(out == NULL && len == 0);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]), SFB_E_CORRUPT);
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[1]), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 3. a damaged manifest must never look like an empty brain ─────────── */
static void test_corrupt_manifest(void)
{
    char dir[512], man[600];
    sfb_store_t s;
    sfb_config_t cfg;
    uint8_t *raw = NULL, *good = NULL;
    size_t raw_len = 0, good_len = 0;
    int fresh = -1;

    snprintf(dir, sizeof dir, "%s/store3", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    manifest_of(man, sizeof man, dir);
    CHECK_EQ(read_file(man, &good, &good_len), 0);
    CHECK(good != NULL && good_len >= 52);

    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);

    /* (a) truncated: half the file */
    CHECK_EQ(write_file(man, good, good_len / 2u), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
    CHECK_EQ(s.man.nseg, 3u);       /* in-memory state not clobbered          */

    fresh = -1;
    {
        sfb_store_t bad;

        memset(&bad, 0, sizeof bad);
        CHECK_EQ(sfb_store_open(&bad, &cfg, &fresh), SFB_E_CORRUPT);
        CHECK_EQ(fresh, -1);        /* never reported as a fresh/empty store */
        CHECK_EQ(bad.man.nseg, 0u);
    }

    /* (b) shorter than the fixed header */
    CHECK_EQ(write_file(man, good, 4), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);

    /* (c) empty file */
    CHECK_EQ(write_file(man, good, 0), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_CORRUPT);

    /* (d) one flipped byte in the segment table */
    raw_len = good_len;
    raw = (uint8_t *)malloc(good_len);
    CHECK(raw != NULL);
    if (raw == NULL)
        return;
    memcpy(raw, good, good_len);
    raw[48 + 5] ^= 0x40u;
    CHECK_EQ(write_file(man, raw, raw_len), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
    free(raw);

    /* (e) one flipped byte in the header */
    raw = (uint8_t *)malloc(good_len);
    CHECK(raw != NULL);
    if (raw == NULL)
        return;
    memcpy(raw, good, good_len);
    raw[9] ^= 0x10u;
    CHECK_EQ(write_file(man, raw, raw_len), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
    free(raw);

    /* (f) manifest deleted while sealed segments remain: NOT an empty brain */
    CHECK_EQ(unlink(man), 0);
    fresh = -1;
    {
        sfb_store_t bad;

        memset(&bad, 0, sizeof bad);
        CHECK_EQ(sfb_store_open(&bad, &cfg, &fresh), SFB_E_CORRUPT);
        CHECK_EQ(fresh, -1);
    }
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_NOTFOUND);

    /* (g) the untouched manifest still loads */
    CHECK_EQ(write_file(man, good, good_len), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_OK);
    CHECK_EQ(s.man.nseg, 3u);
    CHECK(all_segments_ok(&s));

    free(good);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 4. reclamation (W3.4) ─────────────────────────────────────────────── */
static void test_reclaim(void)
{
    char dir[512], p[700], young[700], graph_old[700], graph_cur[700];
    char graph_new[700], dirname[700];
    sfb_store_t s;
    sfb_config_t cfg;
    uint64_t removed = 99;
    uint32_t seq = 0;
    int fresh = 0;

    snprintf(dir, sizeof dir, "%s/store4", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);

    /* three publishes so far: generations 1, 2 and 3 all referenced */
    CHECK_EQ(s.man.generation, 3u);
    CHECK_EQ(sfb_store_segment_write(&s, g_data[0], PAYLOAD, RECS, 1, RECS,
                                     &seq), SFB_OK);
    CHECK_EQ(seq, 1u);
    CHECK_EQ(s.man.generation, 4u);

    seg_file(p, sizeof p, dir, 4, 1);
    CHECK(access(p, F_OK) == 0);
    seg_file(p, sizeof p, dir, 1, 9);
    CHECK_EQ(write_file(p, "orphan", 6), 0);              /* unreferenced   */
    seg_file(young, sizeof young, dir, 5, 1);
    CHECK_EQ(write_file(young, "younger", 7), 0);         /* gen > manifest */
    seg_dir_of(graph_old, sizeof graph_old, dir, "graph-000001.bin");
    seg_dir_of(graph_cur, sizeof graph_cur, dir, "graph-000004.bin");
    seg_dir_of(graph_new, sizeof graph_new, dir, "graph-000005.bin");
    CHECK_EQ(write_file(graph_old, "g1", 2), 0);
    CHECK_EQ(write_file(graph_cur, "g2", 2), 0);
    CHECK_EQ(write_file(graph_new, "g3", 2), 0);
    /* a directory that merely looks like a segment must never be unlinked */
    seg_file(dirname, sizeof dirname, dir, 1, 77);
    CHECK_EQ(sfb_mkdir_p(dirname, 0755), SFB_OK);

    CHECK_EQ(sfb_store_reclaim(&s, &removed), SFB_OK);
    CHECK_EQ(removed, 2u);                                /* seg + graph-1  */

    seg_file(p, sizeof p, dir, 1, 9);
    CHECK(access(p, F_OK) != 0);                          /* orphan gone    */
    CHECK(access(young, F_OK) == 0);                      /* younger kept   */
    CHECK(access(graph_old, F_OK) != 0);
    CHECK(access(graph_cur, F_OK) == 0);
    CHECK(access(graph_new, F_OK) == 0);
    CHECK(access(dirname, F_OK) == 0);

    /* every live segment survives and still verifies */
    CHECK_EQ(s.man.nseg, 4u);
    CHECK(all_segments_ok(&s));
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.nseg, 4u);
    CHECK_EQ(s.man.generation, 4u);
    CHECK_EQ(sfb_store_reclaim(&s, &removed), SFB_OK);
    CHECK_EQ(removed, 0u);                                /* idempotent     */

    /* a reader may not reclaim */
    s.readonly = 1;
    CHECK_EQ(sfb_store_reclaim(&s, &removed), SFB_E_READONLY);
    s.readonly = 0;
    CHECK_EQ(sfb_store_reclaim(NULL, &removed), SFB_E_INVALID);

    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 5. WAL: begin / rollback / commit ─────────────────────────────────── */
static void test_wal(void)
{
    static const char intent[] = "{\"op\":\"flush\",\"gen\":1}";
    char dir[512], man[600], wal[700];
    sfb_store_t s;
    sfb_config_t cfg;
    uint8_t *before = NULL, *after = NULL;
    size_t before_len = 0, after_len = 0;
    int fresh = 0;

    snprintf(dir, sizeof dir, "%s/store5", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);

    manifest_of(man, sizeof man, dir);
    wal_of(wal, sizeof wal, dir);
    CHECK_EQ(read_file(man, &before, &before_len), 0);

    /* begin: the intent record is durable and self-describing */
    CHECK_EQ(sfb_store_wal_begin(&s, intent, sizeof intent - 1u, 0xBEEF1234u),
             SFB_OK);
    CHECK(access(wal, F_OK) == 0);
    {
        uint8_t *w = NULL;
        size_t wl = 0;

        CHECK_EQ(read_file(wal, &w, &wl), 0);
        CHECK(wl == 24u + (sizeof intent - 1u) + 4u);
        if (w != NULL) {
            uint32_t magic = (uint32_t)w[0] | ((uint32_t)w[1] << 8)
                           | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
            uint32_t mtail = (uint32_t)w[wl - 4]
                           | ((uint32_t)w[wl - 3] << 8)
                           | ((uint32_t)w[wl - 2] << 16)
                           | ((uint32_t)w[wl - 1] << 24);

            CHECK_EQ(magic, SFB_MAGIC_WAL);
            CHECK_EQ(mtail, sfb_crc32c(w, wl - 4u));
            CHECK(memcmp(w + 24, intent, sizeof intent - 1u) == 0);
            free(w);
        }
    }

    /* rollback restores nothing and destroys no data */
    CHECK_EQ(sfb_store_wal_rollback(&s), SFB_OK);
    CHECK(access(wal, F_OK) != 0);
    CHECK_EQ(read_file(man, &after, &after_len), 0);
    CHECK_EQ(after_len, before_len);
    CHECK(after != NULL && memcmp(after, before, before_len) == 0);
    free(after);
    after = NULL;
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_OK);
    CHECK_EQ(s.man.nseg, 3u);
    CHECK(all_segments_ok(&s));

    /* commit only removes the WAL; the manifest is untouched */
    CHECK_EQ(sfb_store_wal_begin(&s, intent, sizeof intent - 1u, 0x1u), SFB_OK);
    CHECK(access(wal, F_OK) == 0);
    CHECK_EQ(sfb_store_wal_commit(&s), SFB_OK);
    CHECK(access(wal, F_OK) != 0);
    CHECK_EQ(read_file(man, &after, &after_len), 0);
    CHECK(after != NULL && after_len == before_len
          && memcmp(after, before, before_len) == 0);
    free(after);
    after = NULL;

    /* a stale WAL (crash between begin and publish) is cleared on open,
     * and the previous manifest is what the brain comes back with */
    CHECK_EQ(sfb_store_wal_begin(&s, intent, sizeof intent - 1u, 0x2u), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK(access(wal, F_OK) == 0);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK(access(wal, F_OK) != 0);
    CHECK_EQ(s.man.nseg, 3u);
    CHECK(all_segments_ok(&s));

    /* argument hygiene */
    CHECK_EQ(sfb_store_wal_begin(NULL, intent, 4, 0), SFB_E_INVALID);
    CHECK_EQ(sfb_store_wal_begin(&s, NULL, 4, 0), SFB_E_INVALID);
    CHECK_EQ(sfb_store_wal_commit(NULL), SFB_E_INVALID);
    CHECK_EQ(sfb_store_wal_rollback(NULL), SFB_E_INVALID);

    free(before);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 6. quarantine ─────────────────────────────────────────────────────── */
static void test_quarantine(void)
{
    char dir[512], p[700];
    sfb_store_t s;
    sfb_config_t cfg;
    sfb_seg_entry_t e;
    int fresh = 0;

    snprintf(dir, sizeof dir, "%s/store6", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);

    e = s.man.segs[1];
    seg_file(p, sizeof p, dir, e.gen, e.seq);
    CHECK(access(p, F_OK) == 0);

    CHECK_EQ(sfb_store_quarantine(&s, &e), SFB_OK);
    CHECK_EQ(s.man.nseg, 2u);
    CHECK_EQ(s.man.count, 2u * RECS);
    CHECK_EQ(s.segments_quarantined, 1u);
    CHECK_EQ(s.records_dropped, (uint64_t)RECS);
    CHECK(access(p, F_OK) != 0);
    {
        char q[720];

        snprintf(q, sizeof q, "%s.quarantine", p);
        CHECK(access(q, F_OK) == 0);       /* damaged bytes preserved     */
    }

    /* quarantining something that is not in the manifest is refused */
    CHECK_EQ(sfb_store_quarantine(&s, &e), SFB_E_NOTFOUND);

    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.nseg, 2u);

    s.readonly = 1;
    CHECK_EQ(sfb_store_quarantine(&s, &s.man.segs[0]), SFB_E_READONLY);
    s.readonly = 0;

    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 7. encryption path (weak test stub stands in for AES-GCM) ─────────── */
static void test_encrypted(void)
{
    char dir[512], p[700];
    sfb_store_t s;
    sfb_config_t cfg;
    void *out = NULL;
    size_t len = 0;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    uint32_t seq = 0;
    int fresh = -1;

    snprintf(dir, sizeof dir, "%s/store7", g_base);
    cfg_init(&cfg, dir, DIM, "correct horse battery staple");
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 1);
    CHECK(s.encrypted == 1);
    CHECK_EQ(s.man.flags & 0x0001u, 0x0001u);

    CHECK_EQ(sfb_store_segment_write(&s, g_data[0], PAYLOAD, RECS, 1, RECS,
                                     &seq), SFB_OK);
    CHECK_EQ(seq, 1u);
    CHECK_EQ(s.man.segs[0].length, SEG_HDR + PAYLOAD + SFB_TAG_LEN);

    /* the bytes on disk are not the plaintext */
    seg_file(p, sizeof p, dir, 1, 1);
    CHECK_EQ(read_file(p, &raw, &raw_len), 0);
    CHECK_EQ(raw_len, SEG_HDR + PAYLOAD + SFB_TAG_LEN);
    CHECK(raw != NULL && memcmp(raw + SEG_HDR, g_data[0], 64) != 0);
    free(raw);
    raw = NULL;

    CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[0], &out, &len), SFB_OK);
    CHECK_EQ(len, PAYLOAD);
    CHECK(out != NULL && memcmp(out, g_data[0], PAYLOAD) == 0);
    free(out);
    out = NULL;
    CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]), SFB_OK);

    /* a flipped ciphertext byte must fail, never decrypt to garbage */
    CHECK_EQ(read_file(p, &raw, &raw_len), 0);
    if (raw != NULL) {
        raw[SEG_HDR + 3] ^= 0x80u;
        CHECK_EQ(write_file(p, raw, raw_len), 0);
        free(raw);
        raw = NULL;
        CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[0], &out, &len),
                 SFB_E_CORRUPT);
        CHECK(out == NULL);
    }

    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* opening without the key is refused instead of silently ignoring it */
    cfg_init(&cfg, dir, DIM, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_INVALID);

    /* with the right key the brain is readable again */
    cfg_init(&cfg, dir, DIM, "correct horse battery staple");
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.nseg, 1u);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* a different passphrase derives a different key: authentication fails */
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);

    cfg_init(&cfg, dir, DIM, "correct horse battery staple");
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(sfb_store_segment_write(&s, g_data[1], PAYLOAD, RECS, 1, RECS,
                                     &seq), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* A wrong passphrase is refused at open.  Every segment fails to
     * authenticate under the wrong key, which is indistinguishable from a
     * damaged brain — and callers repair damage by quarantining.  So the
     * store proves the key before handing out an (apparently empty) brain. */
    cfg_init(&cfg, dir, DIM, "wrong passphrase");
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_CORRUPT);
    CHECK(s.encrypted == 0);             /* derived key wiped               */
    {
        sfb_store_t s3;

        /* and nothing was consumed by the refusal */
        cfg_init(&cfg, dir, DIM, "correct horse battery staple");
        CHECK_EQ(sfb_store_open(&s3, &cfg, &fresh), SFB_OK);
        CHECK_EQ(s3.man.nseg, 1u);
        CHECK_EQ(sfb_store_segment_read(&s3, &s3.man.segs[0], &out, &len),
                 SFB_OK);
        CHECK(len == PAYLOAD && memcmp(out, g_data[1], PAYLOAD) == 0);
        free(out);
        out = NULL;
        CHECK_EQ(sfb_store_close(&s3), SFB_OK);
    }
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    /* and an unencrypted brain rejects a key rather than pretending */
    {
        char plain[512];
        sfb_store_t s2;

        snprintf(plain, sizeof plain, "%s/store7b", g_base);
        cfg_init(&cfg, plain, DIM, NULL);
        CHECK_EQ(sfb_store_open(&s2, &cfg, &fresh), SFB_OK);
        CHECK_EQ(sfb_store_close(&s2), SFB_OK);
        cfg_init(&cfg, plain, DIM, "a key nobody used");
        CHECK_EQ(sfb_store_open(&s2, &cfg, &fresh), SFB_E_INVALID);
        rm_rf(plain);
    }

    rm_rf(dir);
}

/* ── 8. generation 0 is legal ──────────────────────────────────────────── */
static void test_generation_zero(void)
{
    char dir[512], p[700];
    sfb_store_t s;
    sfb_config_t cfg;
    sfb_seg_entry_t e;
    void *out = NULL;
    size_t len = 0;
    uint32_t seq = 0;
    int fresh = 0;

    snprintf(dir, sizeof dir, "%s/store9", g_base);
    cfg_init(&cfg, dir, DIM, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    /* a manifest legitimately at generation 0 must load (it is not damage) */
    s.man.generation = 0;
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.generation, 0u);
    CHECK_EQ(s.man.nseg, 0u);

    /* and the first publish moves it to generation 1 */
    CHECK_EQ(sfb_store_segment_write(&s, g_data[2], PAYLOAD, RECS, 1, RECS,
                                     &seq), SFB_OK);
    CHECK_EQ(seq, 1u);
    CHECK_EQ(s.man.generation, 1u);
    seg_file(p, sizeof p, dir, 1, 1);
    CHECK(access(p, F_OK) == 0);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(fresh, 0);
    CHECK_EQ(s.man.generation, 1u);
    CHECK_EQ(s.man.nseg, 1u);
    e = s.man.segs[0];
    CHECK_EQ(e.gen, 1u);
    CHECK_EQ(sfb_store_segment_verify(&s, &e), SFB_OK);
    CHECK_EQ(sfb_store_segment_read(&s, &e, &out, &len), SFB_OK);
    CHECK(len == PAYLOAD && out != NULL && memcmp(out, g_data[2], PAYLOAD) == 0);
    free(out);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 9. corruption fuzz: damage must always be loud ────────────────────── */
static uint32_t g_seed = 0x9E3779B9u;

static uint32_t lcg(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 16;
}

static void test_fuzz_corruption(void)
{
    char dir[512], man[600], seg[700];
    sfb_store_t s;
    sfb_config_t cfg;
    uint8_t *good = NULL, *scratch = NULL;
    size_t good_len = 0;
    int fresh = 0, i;
    uint32_t before_nseg;

    snprintf(dir, sizeof dir, "%s/fuzz", g_base);
    CHECK_EQ(build_store(dir, &s, &cfg, NULL), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    manifest_of(man, sizeof man, dir);
    CHECK_EQ(read_file(man, &good, &good_len), 0);
    CHECK(good != NULL && good_len > 64);
    if (good == NULL)
        return;
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    before_nseg = s.man.nseg;

    scratch = (uint8_t *)malloc(good_len + 16);
    CHECK(scratch != NULL);
    if (scratch == NULL) {
        free(good);
        return;
    }

    /* (a) single/multi byte flips anywhere in the manifest */
    for (i = 0; i < 200; i++) {
        unsigned k, n = 1u + lcg() % 4u;

        memcpy(scratch, good, good_len);
        for (k = 0; k < n; k++) {
            size_t at = lcg() % good_len;
            uint8_t flip = (uint8_t)(1u + lcg() % 255u);

            scratch[at] ^= flip;
        }
        CHECK_EQ(write_file(man, scratch, good_len), 0);
        CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
        CHECK_EQ(s.man.nseg, before_nseg);   /* no partial state leaks      */
    }

    /* (b) random sizes: short, long, empty */
    for (i = 0; i < 60; i++) {
        size_t len = (size_t)(lcg() % (good_len + 16u));
        size_t k;

        for (k = 0; k < len; k++)
            scratch[k] = (uint8_t)lcg();
        CHECK_EQ(write_file(man, scratch, len), 0);
        CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);
    }
    CHECK_EQ(write_file(man, scratch, 0), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_E_CORRUPT);

    /* (c) the good manifest restores the brain */
    CHECK_EQ(write_file(man, good, good_len), 0);
    CHECK_EQ(sfb_store_manifest_load(&s), SFB_OK);
    CHECK_EQ(s.man.nseg, before_nseg);
    CHECK(all_segments_ok(&s));

    /* (d) random damage to a sealed segment */
    {
        uint8_t *segbytes = NULL;
        size_t seglen = 0;
        void *out = NULL;
        size_t outlen = 0;

        seg_file(seg, sizeof seg, dir, 1, 1);
        CHECK_EQ(read_file(seg, &segbytes, &seglen), 0);
        CHECK(segbytes != NULL && seglen > 0);
        if (segbytes != NULL) {
            for (i = 0; i < 100; i++) {
                size_t at = lcg() % seglen;
                uint8_t flip = (uint8_t)(1u + lcg() % 255u);

                segbytes[at] ^= flip;
                CHECK_EQ(write_file(seg, segbytes, seglen), 0);
                CHECK_EQ(sfb_store_segment_read(&s, &s.man.segs[0], &out,
                                                &outlen), SFB_E_CORRUPT);
                CHECK(out == NULL && outlen == 0);
                CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]),
                         SFB_E_CORRUPT);
                segbytes[at] ^= flip;        /* put it back                 */
            }
            CHECK_EQ(write_file(seg, segbytes, seglen), 0);
            free(segbytes);
            CHECK_EQ(sfb_store_segment_verify(&s, &s.man.segs[0]), SFB_OK);
        }
    }

    free(scratch);
    free(good);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    rm_rf(dir);
}

/* ── 10. configuration hygiene ─────────────────────────────────────────── */
static void test_config_checks(void)
{
    char dir[512];
    sfb_store_t s;
    sfb_config_t cfg;
    int fresh = 0;

    CHECK_EQ(sfb_store_open(NULL, &cfg, &fresh), SFB_E_INVALID);
    CHECK_EQ(sfb_store_open(&s, NULL, &fresh), SFB_E_INVALID);
    cfg_init(&cfg, NULL, DIM, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_INVALID);
    cfg_init(&cfg, "", DIM, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_INVALID);
    cfg_init(&cfg, "/tmp/sfb_dim_test", 32u, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_DIM);
    cfg_init(&cfg, "/tmp/sfb_dim_test", 8192u, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_DIM);

    /* a dimension change on an existing brain is refused, not reinterpreted */
    snprintf(dir, sizeof dir, "%s/store8", g_base);
    cfg_init(&cfg, dir, DIM, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);
    cfg_init(&cfg, dir, DIM * 2u, NULL);
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_DIM);

    /* dim 0 = "adopt the manifest's dimension" (ops tools cannot know it),
     * but a brand-new brain still has to declare one. */
    {
        char freshdir[512];

        cfg_init(&cfg, dir, 0u, NULL);
        CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
        CHECK_EQ(s.dim, DIM);
        CHECK_EQ(s.man.dim, DIM);
        CHECK_EQ(s.man.nseg, 0u);
        CHECK_EQ(sfb_store_close(&s), SFB_OK);

        snprintf(freshdir, sizeof freshdir, "%s/store8b", g_base);
        cfg_init(&cfg, freshdir, 0u, NULL);
        CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_E_DIM);
        rm_rf(freshdir);
    }

    CHECK_EQ(sfb_store_close(NULL), SFB_E_INVALID);

    /* readonly writers are refused up front */
    cfg_init(&cfg, dir, DIM, NULL);
    cfg.read_only = 1;
    CHECK_EQ(sfb_store_open(&s, &cfg, &fresh), SFB_OK);
    CHECK_EQ(s.readonly, 1);
    CHECK_EQ(sfb_store_manifest_save(&s), SFB_E_READONLY);
    CHECK_EQ(sfb_store_segment_write(&s, g_data[0], PAYLOAD, RECS, 1, RECS,
                                     NULL), SFB_E_READONLY);
    CHECK_EQ(sfb_store_wal_begin(&s, "x", 1, 0), SFB_E_READONLY);
    CHECK_EQ(sfb_store_close(&s), SFB_OK);

    rm_rf(dir);
}

int main(void)
{
    snprintf(g_base, sizeof g_base, "/tmp/sfb_test_%ld", (long)getpid());
    rm_rf(g_base);
    if (sfb_mkdir_p(g_base, 0755) != SFB_OK) {
        fprintf(stderr, "cannot create %s\n", g_base);
        return 1;
    }
    fill_data();

    test_write_reopen();
    test_corrupt_segment();
    test_corrupt_manifest();
    test_reclaim();
    test_wal();
    test_quarantine();
    test_encrypted();
    test_generation_zero();
    test_fuzz_corruption();
    test_config_checks();

    rm_rf(g_base);

    printf("test_store: %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail != 0)
        return 1;
    assert(g_checks >= 100);
    printf("=== TEST_STORE PASSED ===\n");
    return 0;
}
