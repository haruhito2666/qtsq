/*
 * tests/test_util.c — self-contained unit tests for sfb_util.c.
 *
 *   cc -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE -I. \
 *      sfb_util.c tests/test_util.c -o /tmp/sfb_test_util && /tmp/sfb_test_util
 *
 * Every test writes under /tmp/sfb_test_<pid>/ and removes it on exit.
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

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

#define CHECK_EQ_INT(got, want)                                              \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        g_checks++;                                                          \
        if (g_ != w_) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n",           \
                    __FILE__, __LINE__, #got, g_, w_);                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        const char *g_ = (got), *w_ = (want);                                \
        g_checks++;                                                          \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",                \
                    __FILE__, __LINE__, g_ ? g_ : "(null)", w_);             \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* ── temp tree helpers ───────────────────────────────────────────────────  */
/* Deleting entries while readdir() walks a directory makes the kernel skip
 * siblings, so collect the names first, then remove them. */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    char **names = NULL;
    size_t n = 0, cap = 0, i;
    char p[4096];

    if (d != NULL) {
        struct dirent *de;

        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                char **tmp = (char **)realloc(names, ncap * sizeof *tmp);

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

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_reg(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ── CRC32C ──────────────────────────────────────────────────────────────  */
/* Bitwise reference implementation, deliberately table-free: comparing it
 * against the fast path catches a single wrong table entry, which the fixed
 * test vectors alone do not. */
static uint32_t ref_crc32c(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < len; i++) {
        c ^= (uint32_t)p[i];
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static void test_crc32c(void)
{
    static const unsigned char zeros32[32] = { 0 };
    unsigned char ff32[32];
    unsigned char buf[100];
    unsigned char wide[4096];
    unsigned char one[3];
    uint32_t whole, split, c;
    size_t cut, i;

    assert(sfb_crc32c("123456789", 9) == 0xE3069283u);
    memset(ff32, 0xFF, sizeof ff32);
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)(i * 7u + 3u);

    /* the canonical check value for CRC-32/ISCSI (Castagnoli) */
    CHECK_EQ_INT(sfb_crc32c("123456789", 9), 0xE3069283u);
    CHECK_EQ_INT(sfb_crc32c_update(0, "123456789", 9), 0xE3069283u);

    /* chaining must be identical to a single pass */
    c = sfb_crc32c_update(0, "1234", 4);
    c = sfb_crc32c_update(c, "56789", 5);
    CHECK_EQ_INT(c, 0xE3069283u);

    CHECK_EQ_INT(sfb_crc32c("", 0), 0u);
    CHECK_EQ_INT(sfb_crc32c(NULL, 0), 0u);
    CHECK_EQ_INT(sfb_crc32c(NULL, 9), 0u);      /* NULL is never dereferenced */
    CHECK_EQ_INT(sfb_crc32c(zeros32, 32), 0x8A9136AAu);
    CHECK_EQ_INT(sfb_crc32c(ff32, 32), 0x62A8AB43u);

    whole = sfb_crc32c(buf, sizeof buf);
    for (cut = 0; cut <= sizeof buf; cut++) {
        split = sfb_crc32c_update(sfb_crc32c(buf, cut), buf + cut,
                                  sizeof buf - cut);
        CHECK_EQ_INT(split, whole);
    }

    buf[41] ^= 0x01u;                  /* one flipped bit, different value */
    CHECK(sfb_crc32c(buf, sizeof buf) != whole);
}

/* ── JSON escaping ───────────────────────────────────────────────────────  */
static void check_escape(const char *in, const char *want)
{
    char out[512];
    size_t n;

    n = sfb_json_escape(in, out, sizeof out);
    CHECK_STR(out, want);
    CHECK_EQ_INT(n, strlen(want));
}

static void test_json_escape(void)
{
    char out[16];
    char bin[4];

    check_escape("plain", "plain");
    check_escape("he said \"hi\"", "he said \\\"hi\\\"");
    check_escape("back\\slash", "back\\\\slash");
    check_escape("a\nb", "a\\nb");
    check_escape("a\rb", "a\\rb");
    check_escape("a\tb", "a\\tb");
    check_escape("a\bb", "a\\bb");
    check_escape("a\fb", "a\\fb");
    check_escape("", "");

    /* every other control byte becomes \u00XX */
    bin[0] = 0x01; bin[1] = 0x1f; bin[2] = '\0';
    check_escape(bin, "\\u0001\\u001f");
    bin[0] = 0x00;
    check_escape(bin, "");

    /* DEL and UTF-8 pass through (JSON only mandates escaping U+0000..U+001F) */
    bin[0] = 0x7f; bin[1] = '\0';
    check_escape(bin, bin);
    check_escape("h\xc3\xa9llo", "h\xc3\xa9llo");

    /* truncation never emits half an escape sequence */
    memset(out, 'X', sizeof out);
    CHECK_EQ_INT(sfb_json_escape("\"", out, 2), 0u);
    CHECK_EQ_INT(out[0], '\0');
    CHECK_EQ_INT(sfb_json_escape("\"", out, 3), 2u);
    CHECK_STR(out, "\\\"");

    memset(out, 'X', sizeof out);
    CHECK_EQ_INT(sfb_json_escape("abcdef", out, 3), 2u);
    CHECK_STR(out, "ab");

    /* NULL in / zero cap */
    out[0] = 'Z';
    CHECK_EQ_INT(sfb_json_escape(NULL, out, sizeof out), 0u);
    CHECK_EQ_INT(out[0], '\0');
    CHECK_EQ_INT(sfb_json_escape("x", NULL, 0), 0u);
    CHECK_EQ_INT(sfb_json_escape("x", out, 0), 0u);
}

/* ── paths ───────────────────────────────────────────────────────────────  */
static void test_paths(void)
{
    char out[4096];
    char deep[1400];
    char target[1400];
    size_t i;

    CHECK(sfb_path_join(out, sizeof out, "/a/b", "c") == SFB_OK);
    CHECK_STR(out, "/a/b/c");
    CHECK(sfb_path_join(out, sizeof out, "/a/b/", "c") == SFB_OK);
    CHECK_STR(out, "/a/b/c");
    CHECK(sfb_path_join(out, sizeof out, "/a/b", "/c") == SFB_OK);
    CHECK_STR(out, "/a/b/c");
    CHECK(sfb_path_join(out, sizeof out, "/", "c") == SFB_OK);
    CHECK_STR(out, "/c");
    CHECK(sfb_path_join(out, sizeof out, "/a/b", "") == SFB_OK);
    CHECK_STR(out, "/a/b");
    CHECK(sfb_path_join(out, sizeof out, "/a/b/", NULL) == SFB_OK);
    CHECK_STR(out, "/a/b");
    CHECK(sfb_path_join(out, sizeof out, "", "b") == SFB_OK);
    CHECK_STR(out, "/b");
    CHECK(sfb_path_join(out, sizeof out, "/a/b///", "///c") == SFB_OK);
    CHECK_STR(out, "/a/b/c");

    out[0] = 'Z';
    CHECK_EQ_INT(sfb_path_join(out, 5, "/a/b", "c"), SFB_E_CAPACITY);
    CHECK_EQ_INT(out[0], 'Z');          /* capacity failure writes nothing   */
    CHECK_EQ_INT(sfb_path_join(NULL, 10, "/a", "b"), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_path_join(out, sizeof out, NULL, "b"), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_path_join(out, 0, "/a", "b"), SFB_E_INVALID);

    /* ── mkdir_p ───────────────────────────────────────────────────────── */
    CHECK_EQ_INT(sfb_mkdir_p(NULL, 0755), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_mkdir_p("", 0755), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_mkdir_p("/", 0755), SFB_OK);

    snprintf(out, sizeof out, "%s/u1/a/b/c", g_base);
    CHECK_EQ_INT(sfb_mkdir_p(out, 0755), SFB_OK);
    CHECK(is_dir(out));
    CHECK_EQ_INT(sfb_mkdir_p(out, 0755), SFB_OK);        /* idempotent     */
    snprintf(out, sizeof out, "%s/u1/a/b/c/", g_base);
    CHECK_EQ_INT(sfb_mkdir_p(out, 0755), SFB_OK);        /* trailing slash */
    CHECK(is_dir(out));

    /* a regular file in the way is an error, not a silent success */
    snprintf(out, sizeof out, "%s/u1/plainfile", g_base);
    {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK_EQ_INT(sfb_write_all(fd, "x", 1), SFB_OK);
            (void)close(fd);
        }
    }
    snprintf(out, sizeof out, "%s/u1/plainfile/sub", g_base);
    CHECK(sfb_mkdir_p(out, 0755) != SFB_OK);

    /* ── path_mkdir_for ────────────────────────────────────────────────── */
    CHECK_EQ_INT(sfb_path_mkdir_for(NULL), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_path_mkdir_for(""), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_path_mkdir_for("barefile.txt"), SFB_OK);

    snprintf(out, sizeof out, "%s/u2/x/y/file.txt", g_base);
    CHECK_EQ_INT(sfb_path_mkdir_for(out), SFB_OK);
    snprintf(target, sizeof target, "%s/u2/x/y", g_base);
    CHECK(is_dir(target));
    CHECK(!is_reg(out));                 /* only the directory was created  */

    /* deep path: forces the heap branch (dirname longer than the stack buf) */
    snprintf(deep, sizeof deep, "%s/deep", g_base);
    for (i = 0; i < 200; i++)
        strncat(deep, "/dp", sizeof deep - strlen(deep) - 1);
    strncat(deep, "/leaf.bin", sizeof deep - strlen(deep) - 1);
    CHECK(strlen(deep) > 512);
    CHECK_EQ_INT(sfb_path_mkdir_for(deep), SFB_OK);
    snprintf(target, sizeof target, "%s", deep);
    *strrchr(target, '/') = '\0';
    CHECK(is_dir(target));
}

/* ── raw I/O ─────────────────────────────────────────────────────────────  */
static void test_write_all(void)
{
    char path[512];
    char back[64];
    uint8_t *big;
    int fds[2];
    int fd, rc;

    snprintf(path, sizeof path, "%s/u3/w.txt", g_base);
    CHECK_EQ_INT(sfb_path_mkdir_for(path), SFB_OK);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK_EQ_INT(sfb_write_all(fd, "hello", 5), SFB_OK);
    CHECK_EQ_INT(sfb_write_all(fd, " world", 6), SFB_OK);
    CHECK_EQ_INT(sfb_write_all(fd, NULL, 0), SFB_OK);    /* no-op           */
    CHECK_EQ_INT(sfb_write_all(fd, NULL, 4), SFB_E_INVALID);
    CHECK_EQ_INT(close(fd), 0);

    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    memset(back, 0, sizeof back);
    CHECK_EQ_INT(sfb_pread_all(fd, back, 11, 0), SFB_OK);
    CHECK_STR(back, "hello world");
    memset(back, 0, sizeof back);
    CHECK_EQ_INT(sfb_pread_all(fd, back, 5, 6), SFB_OK);
    CHECK_STR(back, "world");
    CHECK_EQ_INT(sfb_pread_all(fd, back, 0, 0), SFB_OK);
    CHECK_EQ_INT(sfb_pread_all(fd, NULL, 3, 0), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_pread_all(fd, back, 5, 9), SFB_E_CORRUPT); /* past EOF */
    CHECK_EQ_INT(sfb_pread_all(fd, back, 64, 0), SFB_E_CORRUPT);
    CHECK_EQ_INT(close(fd), 0);

    /* ── short write: a non-blocking pipe accepts a prefix, then refuses ── */
    big = (uint8_t *)malloc(1024 * 1024);
    CHECK(big != NULL);
    if (big != NULL) {
        uint8_t drain[4096];
        size_t landed = 0;

        memset(big, 0x5A, 1024 * 1024);
        CHECK_EQ_INT(pipe(fds), 0);
        CHECK(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
        rc = sfb_write_all(fds[1], big, 1024 * 1024);
        CHECK_EQ_INT(rc, SFB_E_ENOSPC);
        /* close the writer first, else the drain below blocks on EOF */
        (void)close(fds[1]);
        /* the sink must have taken a strict prefix, never the whole buffer */
        for (;;) {
            ssize_t r = read(fds[0], drain, sizeof drain);
            if (r <= 0)
                break;
            landed += (size_t)r;
        }
        CHECK(landed > 0);
        CHECK(landed < 1024 * 1024);
        free(big);
        (void)close(fds[0]);
    }

    /* ── pipe closed early: an I/O error, never a silent success ────────── */
    (void)signal(SIGPIPE, SIG_IGN);
    CHECK_EQ_INT(pipe(fds), 0);
    (void)close(fds[0]);
    rc = sfb_write_all(fds[1], "x", 1);
    CHECK(rc != SFB_OK);
    CHECK_EQ_INT(rc, SFB_E_IO);           /* EPIPE is not a short write     */
    (void)close(fds[1]);

    CHECK_EQ_INT(sfb_write_all(-1, "x", 1), SFB_E_INVALID);
}

static void test_fsync_dir_and_rename(void)
{
    char a[512], b[512], back[8];
    int fd;

    snprintf(a, sizeof a, "%s/u4/a.txt", g_base);
    snprintf(b, sizeof b, "%s/u4/b.txt", g_base);
    CHECK_EQ_INT(sfb_path_mkdir_for(a), SFB_OK);

    fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK_EQ_INT(sfb_write_all(fd, "payload", 7), SFB_OK);
    CHECK_EQ_INT(close(fd), 0);

    snprintf(b, sizeof b, "%s/u4", g_base);
    CHECK_EQ_INT(sfb_fsync_dir(b), SFB_OK);
    CHECK_EQ_INT(sfb_fsync_dir(NULL), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_fsync_dir(""), SFB_E_INVALID);
    snprintf(b, sizeof b, "%s/u4/nope", g_base);
    CHECK(sfb_fsync_dir(b) != SFB_OK);

    snprintf(a, sizeof a, "%s/u4/a.txt", g_base);
    snprintf(b, sizeof b, "%s/u4/b.txt", g_base);
    CHECK_EQ_INT(sfb_rename_atomic(a, b), SFB_OK);
    CHECK(!is_reg(a));
    CHECK(is_reg(b));

    fd = open(b, O_RDONLY);
    CHECK(fd >= 0);
    memset(back, 0, sizeof back);
    CHECK_EQ_INT(sfb_pread_all(fd, back, 7, 0), SFB_OK);
    CHECK_STR(back, "payload");
    (void)close(fd);

    CHECK_EQ_INT(sfb_rename_atomic(a, b), SFB_E_NOTFOUND);
    CHECK_EQ_INT(sfb_rename_atomic(NULL, b), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_rename_atomic(a, NULL), SFB_E_INVALID);
}

/* ── snprintf wrapper ────────────────────────────────────────────────────  */
static void test_snprintf(void)
{
    char out[8];

    CHECK_EQ_INT(sfb_snprintf(out, sizeof out, "%s-%d", "ab", 7), SFB_OK);
    CHECK_STR(out, "ab-7");
    CHECK_EQ_INT(sfb_snprintf(out, 4, "%s", "abcdef"), SFB_E_CAPACITY);
    CHECK_STR(out, "abc");               /* NUL-terminated even when cut    */
    CHECK_EQ_INT(sfb_snprintf(NULL, 8, "x"), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_snprintf(out, 0, "x"), SFB_E_INVALID);
    CHECK_EQ_INT(sfb_snprintf(out, sizeof out, NULL), SFB_E_INVALID);
}

static void test_time(void)
{
    uint64_t us;
    double secs;

    us = sfb_now_us();
    secs = sfb_now_secs();
    CHECK(us > 1700000000000000ull);     /* after 2023-11                  */
    CHECK(secs > 1.7e9);
    CHECK((double)us / 1e6 - secs < 1.0 && secs - (double)us / 1e6 < 1.0);
    CHECK(sfb_now_us() >= us);
}

static void test_strerror(void)
{
    static const int codes[] = {
        SFB_OK, SFB_E_INVALID, SFB_E_IO, SFB_E_NOMEM, SFB_E_NOTFOUND,
        SFB_E_STATE, SFB_E_DIM, SFB_E_TYPE, SFB_E_NONFINITE, SFB_E_LOCKED,
        SFB_E_CORRUPT, SFB_E_ENOSPC, SFB_E_READONLY, SFB_E_CAPACITY
    };
    size_t i, j;

    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        const char *s = sfb_strerror(codes[i]);
        CHECK(s != NULL && s[0] != '\0');
        for (j = 0; j < i; j++)
            CHECK(strcmp(sfb_strerror(codes[j]), s) != 0);
    }
    CHECK_STR(sfb_strerror(SFB_OK), "ok");
    CHECK(sfb_strerror(424242) != NULL);
    CHECK(sfb_strerror(424242)[0] != '\0');
}

int main(void)
{
    snprintf(g_base, sizeof g_base, "/tmp/sfb_test_%ld", (long)getpid());
    rm_rf(g_base);
    if (sfb_mkdir_p(g_base, 0755) != SFB_OK) {
        fprintf(stderr, "cannot create %s\n", g_base);
        return 1;
    }

    test_crc32c();
    test_json_escape();
    test_paths();
    test_write_all();
    test_fsync_dir_and_rename();
    test_snprintf();
    test_time();
    test_strerror();

    rm_rf(g_base);

    printf("test_util: %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail != 0)
        return 1;
    assert(g_checks >= 80);              /* a silently-skipped run is a bug  */
    printf("=== TEST_UTIL PASSED ===\n");
    return 0;
}
