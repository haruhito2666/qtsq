/*
 * test_keys.c — encrypted-brain safety (W2.6 / D-02 class)
 *
 * The dangerous failure is not "wrong key is refused", it is "wrong key looks
 * like an empty brain": the user sees zero records, keeps working, and the only
 * copy of the data ends up renamed to *.quarantine with no counter moved.
 * These tests pin the difference.
 *
 * Author: Haruhito
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CK_MSG(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t rs = 777;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0; }
static void vfill(float *v, uint32_t d) {
    for (uint32_t i = 0; i < d; i++) v[i] = (float)(rnd() * 2.0 - 1.0);
}
/* count files in <brain>/brain.qtsq.d whose name ends with `suffix` */
static int count_suffix(const char *dir, const char *suffix)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/brain.qtsq.d", dir);
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0; struct dirent *e;
    size_t sl = strlen(suffix);
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0) n++;
    }
    closedir(d);
    return n;
}

int main(void)
{
    const uint32_t dim = 64, nrec = 120;
    const char *dir = "/tmp/sfb_keys_test";
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* best effort */ }

    printf("── sfb encrypted-brain tests\n");

    /* 1. create an encrypted brain with 120 records */
    sfb_config_t cfg; sfb_config_default(&cfg, dir, dim);
    cfg.key = "correct horse battery staple";
    sfb_brain_t *b = NULL;
    int rc = sfb_open(&cfg, &b);
    CK_MSG(rc == SFB_OK, "open for create rc=%d (%s)", rc, sfb_strerror(rc));
    if (rc != SFB_OK) { printf("cannot continue\n"); return 1; }

    float v[64]; char text[64];
    for (uint32_t i = 0; i < nrec; i++) {
        vfill(v, dim);
        snprintf(text, sizeof text, "secret record %u %u", i, i * 7u);
        sfb_remember_result_t r;
        rc = sfb_remember(b, v, text, "user", &r);
        CK_MSG(rc == SFB_OK, "remember %u rc=%d", i, rc);
    }
    sfb_flush_result_t fr;
    CK(sfb_flush(b, &fr) == SFB_OK);
    {
        uint64_t live = 0;
        CK(sfb_count(b, &live) == SFB_OK);
        CK(live == nrec);
    }
    sfb_close(b);

    /* 2. the ciphertext really is ciphertext: a known text must not appear */
    {
        char path[4096];
        snprintf(path, sizeof path, "%s/brain.qtsq.d", dir);
        DIR *d = opendir(path);
        int found_plain = 0;
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                if (strncmp(e->d_name, "seg-", 4) != 0) continue;
                char fp[4096];
                snprintf(fp, sizeof fp, "%s/%s", path, e->d_name);
                FILE *f = fopen(fp, "rb");
                if (!f) continue;
                static char buf[1 << 20];
                size_t got = fread(buf, 1, sizeof buf, f);
                fclose(f);
                for (size_t i = 0; i + 13 <= got; i++)
                    if (memcmp(buf + i, "secret record", 13) == 0) { found_plain = 1; break; }
            }
            closedir(d);
        }
        CK_MSG(!found_plain, "plaintext visible in a sealed segment");
    }

    /* 3. THE REGRESSION: wrong key must be an error, not an empty brain */
    int quar_before = count_suffix(dir, ".quarantine");
    sfb_config_t wrong; sfb_config_default(&wrong, dir, dim);
    wrong.key = "wrong passphrase";
    sfb_brain_t *wb = NULL;
    rc = sfb_open(&wrong, &wb);
    CK_MSG(rc != SFB_OK, "WRONG KEY OPENED SUCCESSFULLY (rc=0) — silent data loss path is live");
    CK_MSG(wb == NULL, "wrong-key open handed back a brain handle");
    if (rc == SFB_OK) sfb_close(wb);

    /* and the ciphertext must still be there, not quarantined away */
    int quar_after = count_suffix(dir, ".quarantine");
    CK_MSG(quar_after == quar_before,
           "wrong-key open renamed %d segment(s) to .quarantine", quar_after - quar_before);
    {
        char path[4096];
        snprintf(path, sizeof path, "%s/brain.qtsq.d", dir);
        DIR *d = opendir(path);
        int segs = 0; struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) if (strncmp(e->d_name, "seg-", 4) == 0) segs++;
            closedir(d);
        }
        CK_MSG(segs > 0, "no seg-* files left after a wrong-key open");
    }

    /* 4. missing key on an encrypted brain is also refused */
    sfb_config_t nokey; sfb_config_default(&nokey, dir, dim);
    sfb_brain_t *nb = NULL;
    rc = sfb_open(&nokey, &nb);
    CK_MSG(rc != SFB_OK, "encrypted brain opened with NO key (rc=0)");
    if (rc == SFB_OK) sfb_close(nb);

    /* 5. recovery: the correct key still returns every record */
    sfb_brain_t *rb = NULL;
    rc = sfb_open(&cfg, &rb);
    CK_MSG(rc == SFB_OK, "reopen with the correct key failed rc=%d (%s)", rc, sfb_strerror(rc));
    if (rc == SFB_OK) {
        uint64_t live = 0;
        CK(sfb_count(rb, &live) == SFB_OK);
        CK_MSG(live == nrec, "records after wrong-key attempts: %llu (expected %u)",
               (unsigned long long)live, nrec);
        sfb_stats_t st; sfb_stats_get(rb, &st);
        CK_MSG(st.records_dropped == 0, "records_dropped=%llu after wrong-key attempts",
               (unsigned long long)st.records_dropped);
        /* and recall still works through the decrypted path */
        float q[64]; uint32_t hits = 0; sfb_hit_t h[10];
        vfill(q, dim);
        CK(sfb_recall(rb, q, 10, h, &hits) == SFB_OK);
        CK(hits > 0);
        sfb_close(rb);
    }

    /* 6. read_only open of an encrypted brain with the right key */
    sfb_config_t ro; sfb_config_default(&ro, dir, dim);
    ro.key = "correct horse battery staple";
    ro.read_only = 1;
    sfb_brain_t *rob = NULL;
    rc = sfb_open(&ro, &rob);
    CK_MSG(rc == SFB_OK, "read-only keyed open rc=%d (%s)", rc, sfb_strerror(rc));
    if (rc == SFB_OK) {
        {
            uint64_t live = 0;
            CK(sfb_count(rob, &live) == SFB_OK);
            CK(live == nrec);
        }
        sfb_remember_result_t r;
        CK_MSG(sfb_remember(rob, v, "nope", "user", &r) == SFB_E_READONLY,
               "read-only brain accepted a write");
        sfb_close(rob);
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* best effort */ }

    printf("test_keys: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_KEYS FAILED\n"); return 1; }
    printf("=== TEST_KEYS PASSED ===\n");
    return 0;
}
