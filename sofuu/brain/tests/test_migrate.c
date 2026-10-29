/*
 * test_migrate.c — v1 QTSQ brain -> v2 store (W7.5)
 *
 * What this suite proves, against a REAL v1 brain:
 *   - the v1 container is recognised and routed to the importer;
 *   - an encrypted container without a passphrase is refused with
 *     "v1_key_required" and nothing is created or overwritten;
 *   - a wrong passphrase is refused with "v1_decrypt_failed";
 *   - an unrecognised single-file "brain" is refused cleanly;
 *   - a pre-migration backup is never written when the migration fails.
 *
 * What it cannot prove here, and why: the successful import needs a v1
 * container this process can decrypt. QTSQ writes v1 brains encrypted via the
 * OS keychain (qtsq_brain_save() returns -15 without it), password-based
 * container encryption does not round-trip through a file (qtsq_encrypt() then
 * qtsq_read()+qtsq_decrypt() returns -9 from the core), and container plaintext
 * writes are refused (-8) because pack() already carries security state. The
 * happy path therefore runs only when the caller supplies a v1 file whose
 * passphrase is known; the refusals below are the reachable half.
 *
 * Author: Haruhito
 */
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include "../../include/qtsq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CK_MSG(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "  [FAIL] %s:%d %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Run sfb_migrate() with stderr captured, so the machine-readable report can be
 * asserted instead of merely printed. */
static int migrate_capture(const char *src, const char *dst, const char *key,
                           char *report, size_t cap)
{
    char tmp[] = "/tmp/sfb_mig_stderr.XXXXXX";
    int fd = mkstemp(tmp);
    int rc;
    FILE *rd;
    int saved;

    if (fd < 0) { if (cap) report[0] = '\0'; return sfb_migrate(src, dst, key); }
    fflush(stderr);
    saved = dup(fileno(stderr));
    (void)dup2(fd, fileno(stderr));

    rc = sfb_migrate(src, dst, key);
    fflush(stderr);
    (void)dup2(saved, fileno(stderr));
    close(saved);
    close(fd);

    report[0] = '\0';
    rd = fopen(tmp, "rb");
    if (rd) {
        size_t n = fread(report, 1, cap - 1, rd);
        report[n] = '\0';
        fclose(rd);
    }
    (void)unlink(tmp);
    return rc;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }


/* A genuine v1 container this process can read: packed plaintext through
 * qtsq_container_pack_write(). qtsq_brain_save() cannot be used because it
 * encrypts to the OS keystore, which is not reachable from a headless CLI. */
static int build_v1_fixture(const char *path, size_t rows, size_t dim,
                            const float *mems)
{
    qtsq_context_t c, sub;
    uint32_t dims[2];
    int rc;

    dims[0] = (uint32_t)rows;
    dims[1] = (uint32_t)dim;
    if (qtsq_container_create(&c) != QTSQ_OK)
        return -1;
    qtsq_init(&sub);
    rc = qtsq_compress_tensor(&sub, mems, rows * dim, dims, 2, 0);
    if (rc == QTSQ_OK)
        rc = qtsq_container_add_stream_take(&c, &sub, "memories");
    qtsq_free(&sub);
    if (rc == QTSQ_OK)
        rc = qtsq_container_pack_write(&c, path, 1);
    qtsq_free(&c);
    return rc == QTSQ_OK ? 0 : -1;
}

int main(void)
{
    char report[2048];
    char cmd[1024];
    const char *src = "/tmp/sfb_mig_src.qtsq";
    const char *dst = "/tmp/sfb_mig_dst";
    int rc;

    printf("── sfb v1 -> v2 migration\n");

    /* ── 0. the happy path, on a real v1 container ─────────────────────── */
    {
        const size_t rows = 50, dim = 128;
        float *mems = (float *)malloc(rows * dim * sizeof(float));
        CK(mems != NULL);
        if (mems != NULL) {
            for (size_t i = 0; i < rows * dim; i++)
                mems[i] = (float)(0.5 - (double)((i * 2654435761u) % 1000) / 1000.0);

            CK_MSG(build_v1_fixture(src, rows, dim, mems) == 0, "cannot build the v1 fixture");
            snprintf(cmd, sizeof cmd, "rm -rf %s %s.v1backup", dst, src);
            CK(system(cmd) == 0);

            rc = migrate_capture(src, dst, NULL, report, sizeof report);
            CK_MSG(rc == SFB_OK, "happy-path migrate rc=%d (%s)", rc, sfb_strerror(rc));
            CK_MSG(strstr(report, "\"ok\":true") != NULL, "no ok report: %s", report);

            {
                char bak[600];
                snprintf(bak, sizeof bak, "%s.v1backup", src);
                CK_MSG(file_exists(bak), "pre-migration backup missing");
            }

            sfb_config_t cfg;
            sfb_brain_t *b = NULL;
            sfb_config_default(&cfg, dst, (uint32_t)dim);
            rc = sfb_open(&cfg, &b);
            CK_MSG(rc == SFB_OK, "open migrated store rc=%d (%s)", rc, sfb_strerror(rc));
            if (rc == SFB_OK) {
                uint64_t live = 0;
                CK(sfb_count(b, &live) == SFB_OK);
                CK_MSG(live == rows, "imported %llu records, expected %zu",
                       (unsigned long long)live, rows);

                /* content fidelity: rows must recall themselves */
                uint32_t ok = 0;
                for (size_t r = 0; r < 10; r++) {
                    sfb_hit_t h[3];
                    uint32_t n = 0;
                    size_t row = (r * 7u) % rows;
                    if (sfb_recall(b, mems + row * dim, 3, h, &n) == SFB_OK && n > 0) {
                        char want[64];
                        snprintf(want, sizeof want, "v1-memory-%zu", row);
                        if (h[0].text && strstr(h[0].text, want) != NULL)
                            ok++;
                    }
                }
                CK_MSG(ok == 10, "self-recall after import: %u/10", ok);
                sfb_close(b);
            }

            /* migrating again onto the populated store is refused */
            rc = migrate_capture(src, dst, NULL, report, sizeof report);
            CK_MSG(rc != SFB_OK, "second migrate onto an existing store returned %d", rc);

            snprintf(cmd, sizeof cmd, "rm -rf %s %s %s.v1backup", src, dst, src);
            CK(system(cmd) == 0);
            free(mems);
        }
    }

    printf("   (refusal paths)\n");

    /* ── 1. a real v1 brain, if this machine has one ───────────────────── */
    const char *real = "/Users/priyanshuboruah/projects/black-hole-disk/.sofuu/brain/brain.qtsq";
    if (file_exists(real)) {
        snprintf(cmd, sizeof cmd, "cp '%s' %s && rm -rf %s %s.v1backup", real, src, dst, src);
        CK(system(cmd) == 0);

        FILE *f = fopen(src, "rb");
        unsigned char h[4] = {0};
        CK(f != NULL);
        if (f) { CK(fread(h, 1, 4, f) == 4); fclose(f); }
        CK_MSG(h[0] == 'Q' && h[1] == 'T' && h[2] == 'S' && h[3] == 0x01,
               "fixture magic %02x%02x%02x%02x is not a QTSQ v1 container",
               h[0], h[1], h[2], h[3]);

        /* no passphrase: refused, and the reason names the key rather than a
         * vague "invalid argument" */
        rc = migrate_capture(src, dst, NULL, report, sizeof report);
        CK_MSG(rc == SFB_E_INVALID, "no-key migrate rc=%d", rc);
        CK_MSG(strstr(report, "v1_key_required") != NULL,
               "report did not name v1_key_required: %s", report);
        CK_MSG(!file_exists(dst), "destination was created despite the refusal");
        {
            char bak[600];
            snprintf(bak, sizeof bak, "%s.v1backup", src);
            CK_MSG(!file_exists(bak), "a backup was written for a failed migration");
        }

        /* wrong passphrase: refused as a decrypt failure */
        rc = migrate_capture(src, dst, "definitely-not-the-passphrase", report, sizeof report);
        CK_MSG(rc == SFB_E_INVALID, "wrong-key migrate rc=%d", rc);
        CK_MSG(strstr(report, "v1_decrypt_failed") != NULL,
               "report did not name v1_decrypt_failed: %s", report);
        CK_MSG(!file_exists(dst), "destination was created despite a bad passphrase");
    } else {
        printf("   (no v1 brain on this machine: skipping the real-file checks)\n");
    }

    /* ── 2. an unrecognised single-file brain ──────────────────────────── */
    snprintf(cmd, sizeof cmd, "printf 'NOPE0123456789abcdef' > %s", src);
    CK(system(cmd) == 0);
    rc = migrate_capture(src, dst, NULL, report, sizeof report);
    CK_MSG(rc == SFB_E_INVALID, "unknown-magic migrate rc=%d", rc);
    CK_MSG(strstr(report, "unrecognised_v1_layout") != NULL,
           "report did not name unrecognised_v1_layout: %s", report);
    CK_MSG(!file_exists(dst), "destination created for an unrecognised file");

    /* ── 3. a QTSQ container that is not a usable brain ────────────────── */
    snprintf(cmd, sizeof cmd, "printf 'QTS\\001' > %s && head -c 64 /dev/zero >> %s", src, src);
    CK(system(cmd) == 0);
    rc = migrate_capture(src, dst, "pw", report, sizeof report);
    CK_MSG(rc == SFB_E_INVALID, "truncated-container migrate rc=%d", rc);
    CK_MSG(strstr(report, "v1_read_failed") != NULL,
           "report did not name v1_read_failed: %s", report);

    /* ── 4. argument validation ────────────────────────────────────────── */
    rc = migrate_capture("", dst, NULL, report, sizeof report);
    CK_MSG(rc == SFB_E_INVALID, "empty-src rc=%d", rc);
    rc = migrate_capture(src, src, NULL, report, sizeof report);
    CK_MSG(rc == SFB_E_INVALID && strstr(report, "same_path") != NULL,
           "same-path migrate was not rejected: %s", report);
    rc = migrate_capture("/tmp/definitely-not-here.qtsq", dst, NULL, report, sizeof report);
    CK_MSG(rc == SFB_E_INVALID && strstr(report, "source_missing") != NULL,
           "missing source was not reported: %s", report);

    snprintf(cmd, sizeof cmd, "rm -rf %s %s %s.v1backup", src, dst, src);
    CK(system(cmd) == 0);

    printf("test_migrate: %d checks, %d failures\n", checks, fails);
    if (fails) { printf("TEST_MIGRATE FAILED\n"); return 1; }
    printf("=== TEST_MIGRATE PASSED ===\n");
    return 0;
}
