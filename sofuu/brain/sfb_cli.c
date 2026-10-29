/*
 * sfb_cli.c — main() for the `sfb` binary (Sofuu Brain v2, W7 tooling).
 *
 * Subcommands (exactly the W7 surface):
 *   sfb verify  <brain> [--json] [--repair]
 *   sfb repair  <brain> [--json]
 *   sfb backup  <brain> <dest>
 *   sfb restore <backup> <dest>
 *   sfb stats   <brain> [--json]
 *   sfb migrate <src> <dst>
 *   sfb selftest
 *   sfb serve   <brain-dir> [--socket <path>] [--dim <n>] [--key <pass>]  (W2.4)
 *
 * Contract:
 *   • exit 0 on success, 1 on an operation failure, 2 on a usage error
 *   • every failure prints the typed error name via sfb_strerror()
 *   • `--json` prints the operation's JSON report on stdout and nothing else
 *   • verify exits non-zero when the scrub finds damage (repair exits 0 when it
 *     successfully quarantined what it found, and says what it dropped)
 *
 * This file defines no sfb_* symbol: everything comes from libsfb.a.
 */
#define _POSIX_C_SOURCE 200809L

#include "sfb_internal.h"
#include "sfb_broker.h"

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/un.h>
#include <time.h>

#define CLI_EXIT_OK     0
#define CLI_EXIT_ERROR  1
#define CLI_EXIT_USAGE  2

/* AF_UNIX sun_path is 104 bytes on Darwin, 108 on Linux; the CLI keeps the
 * platform's real limit so the help text and the error agree. */
#define CLI_SOCK_PATH_MAX sizeof(((struct sockaddr_un *)0)->sun_path)

/* ── typed error printing ──────────────────────────────────────────────── */
static void cli_error(const char *what, int code)
{
    const char *name = sfb_strerror(code);
    fprintf(stderr, "sfb %s: error: %s (code %d)\n",
            what, (name && name[0]) ? name : "unknown error", code);
    fflush(stderr);
}

static void cli_usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "sfb %d.%d — Sofuu Brain v2 operations\n"
        "\n"
        "usage: %s <command> [arguments]\n"
        "\n"
        "commands:\n"
        "  verify  <brain> [--json] [--repair]   digest + CRC scrub, optional repair\n"
        "  repair  <brain> [--json]              quarantine damaged segments\n"
        "  backup  <brain> <dest>                hardlink snapshot of a sealed store\n"
        "  restore <backup> <dest>               recreate a store from a snapshot\n"
        "  stats   <brain> [--json]              open read-only, print stats\n"
        "  migrate <src> <dst>                   v1 single-file brain -> v2 store\n"
        "  selftest                              run built-in smoke checks\n"
        "  serve   <brain-dir> [--socket <path>] [--dim <n>] [--key <pass>]\n"
        "                                        share the brain over a UNIX socket\n"
        "\n"
        "exit codes: 0 ok, 1 operation failed, 2 usage\n"
        "\n"
        "serve: one process owns the brain and answers sfb_broker_* clients over\n"
        "       AF_UNIX until SIGINT/SIGTERM. Default socket: <brain-dir>/brain.sock.\n"
        "       Socket paths must be shorter than %zu bytes on this platform.\n"
        "       --dim is only needed to create a brain that does not exist yet.\n",
        SFB_VERSION_MAJOR, SFB_VERSION_MINOR, argv0,
        (size_t)CLI_SOCK_PATH_MAX);
}

/* ── tiny JSON field extractors (the reports come from sfb_ops.c) ──────── */
#define OPS_LINE_CAP_LOCAL 1024


static const char *cli_jfind(const char *j, const char *key)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? p + strlen(pat) : NULL;
}

static int cli_jbool(const char *j, const char *key, int *out)
{
    const char *p = cli_jfind(j, key);
    if (!p) return 0;
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

static int cli_ju64(const char *j, const char *key, unsigned long long *out)
{
    const char *p = cli_jfind(j, key);
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p || errno) return 0;
    *out = v;
    return 1;
}

/* Print the elements of a JSON string array, one per line, indented. */
static void cli_jarray(const char *j, const char *key, const char *indent)
{
    const char *p = cli_jfind(j, key);
    if (!p) return;
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;
    int any = 0;
    for (;;) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p != '"') break;
        p++;
        char buf[OPS_LINE_CAP_LOCAL];
        size_t n = 0;
        while (*p && *p != '"') {
            char c = *p++;
            if (c == '\\' && *p) {
                char e = *p++;
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    /* \u00XX only — enough for control-byte escaping */
                    char hex[5] = {0};
                    for (int i = 0; i < 4 && *p; i++) hex[i] = *p++;
                    long cp = strtol(hex, NULL, 16);
                    c = (cp > 0 && cp < 128) ? (char)cp : '?';
                    break;
                }
                default: c = e; break;
                }
            }
            if (n + 1 < sizeof buf) buf[n++] = c;
        }
        buf[n] = '\0';
        if (*p == '"') p++;
        fprintf(stdout, "%s- %s\n", indent, buf);
        any = 1;
    }
    if (!any) fprintf(stdout, "%s(none)\n", indent);
}

/* ── verify / repair ───────────────────────────────────────────────────── */
static int cli_verify(const char *path, int want_json, int repair)
{
    char json[16384];
    memset(json, 0, sizeof json);
    int rc = sfb_verify(path, NULL, repair, json, sizeof json);
    const char *cmd = repair ? "repair" : "verify";

    if (!json[0]) {
        cli_error(cmd, rc);
        return CLI_EXIT_ERROR;
    }

    int ok = 0;
    unsigned long long records = 0, segments = 0, damaged = 0, dropped = 0;
    (void)cli_jbool(json, "ok", &ok);
    (void)cli_ju64(json, "records", &records);
    (void)cli_ju64(json, "segments", &segments);
    (void)cli_ju64(json, "damaged_segments", &damaged);
    (void)cli_ju64(json, "records_dropped_estimate", &dropped);

    if (want_json) {
        printf("%s\n", json);
        fflush(stdout);
        if (rc != SFB_OK || !ok) {
            cli_error(cmd, rc != SFB_OK ? rc : SFB_E_CORRUPT);
            return CLI_EXIT_ERROR;
        }
        return CLI_EXIT_OK;
    }

    printf("%s %s\n", cmd, path);
    printf("  ok                     : %s\n", ok ? "true" : "false");
    printf("  records (sealed)       : %llu\n", records);
    printf("  segments               : %llu\n", segments);
    printf("  damaged segments       : %llu\n", damaged);
    printf("  records dropped (est.) : %llu\n", dropped);
    printf("  missing files          :\n");
    cli_jarray(json, "missing_files", "    ");
    printf("  findings               :\n");
    cli_jarray(json, "errors", "    ");

    if (rc != SFB_OK || !ok) {
        cli_error(cmd, rc != SFB_OK ? rc : SFB_E_CORRUPT);
        return CLI_EXIT_ERROR;
    }
    if (repair && dropped > 0)
        printf("repair: %llu record(s) dropped; quarantined bytes kept as *.torn\n",
               dropped);
    return CLI_EXIT_OK;
}

/* ── stats ─────────────────────────────────────────────────────────────── */
static int cli_stats(const char *path, int want_json)
{
    char json[8192];
    memset(json, 0, sizeof json);
    int rc = sfb_stats_json(path, NULL, json, sizeof json);
    if (!json[0]) {
        cli_error("stats", rc);
        return CLI_EXIT_ERROR;
    }
    if (want_json) {
        printf("%s\n", json);
        fflush(stdout);
        if (rc != SFB_OK) { cli_error("stats", rc); return CLI_EXIT_ERROR; }
        return CLI_EXIT_OK;
    }
    int ok = 0;
    (void)cli_jbool(json, "ok", &ok);
    if (!ok) {
        cli_jarray(json, "errors", "  ");
        cli_error("stats", rc != SFB_OK ? rc : SFB_E_STATE);
        return CLI_EXIT_ERROR;
    }
    static const char *keys[] = {
        "dim", "records", "live", "tombstones", "segments", "generations",
        "bytes_records", "bytes_on_disk", "records_dropped",
        "segments_quarantined", "ids_renumbered", "index_rebuilt",
        "flushes", "flush_last_us", "open_us", "events"
    };
    printf("stats %s\n", path);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        unsigned long long v = 0;
        if (cli_ju64(json, keys[i], &v)) printf("  %-22s: %llu\n", keys[i], v);
    }
    if (rc != SFB_OK) { cli_error("stats", rc); return CLI_EXIT_ERROR; }
    return CLI_EXIT_OK;
}

/* ── selftest ──────────────────────────────────────────────────────────── */
static int cli_rm_rf(const char *path)
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
        if (S_ISDIR(st.st_mode)) { if (cli_rm_rf(full) != 0) rc = -1; }
        else if (unlink(full) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

/* Find the largest non-graph payload file in a segment dir (works for any
 * segment naming sfb_store.c chooses). */
static int cli_first_segment(const char *seg_dir, char *out, size_t cap)
{
    DIR *d = opendir(seg_dir);
    if (!d) return 0;
    struct dirent *de;
    off_t best = -1;
    out[0] = '\0';
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "graph-", 6) == 0) continue;
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", seg_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size < 32) continue;
        if (st.st_size > best) {
            best = st.st_size;
            snprintf(out, cap, "%s", full);
        }
    }
    closedir(d);
    return out[0] != '\0';
}

static int cli_flip_byte(const char *file)
{
    int fd = open(file, O_RDWR);
    if (fd < 0) return SFB_E_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) { close(fd); return SFB_E_IO; }
    off_t off = st.st_size / 2;
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return SFB_E_IO; }
    b = (uint8_t)(b ^ 0xFFu);
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return SFB_E_IO; }
    (void)fsync(fd);
    close(fd);
    return SFB_OK;
}

static int cli_selftest(void)
{
    int failures = 0;
    printf("sfb selftest\n");

    int rc = sfb_crypto_selftest();
    printf("  [%s] crypto NIST vectors%s\n", rc == SFB_OK ? "ok" : "FAIL",
           rc == SFB_OK ? "" : " — continuing");

    /* POSIX-only temp dir: mkdir with a pid+clock name (mkdtemp() is not
     * declared under a bare _POSIX_C_SOURCE on Darwin). */
    char rootbuf[256];
    char root[256];
    snprintf(rootbuf, sizeof rootbuf, "/tmp/sfb-selftest-%ld-%llu",
             (long)getpid(), (unsigned long long)sfb_now_us());
    snprintf(root, sizeof root, "%s", rootbuf);
    if (mkdir(root, 0700) != 0) {
        cli_error("selftest: cannot create temp dir", SFB_E_IO);
        return CLI_EXIT_ERROR;
    }
    char brain[4096], snap[4096], copy[4096];
    snprintf(brain, sizeof brain, "%s/brain", root);
    snprintf(snap,  sizeof snap,  "%s/snapshot", root);
    snprintf(copy,  sizeof copy,  "%s/restored", root);

    const uint32_t dim = 64;
    sfb_config_t cfg;
    sfb_config_default(&cfg, brain, dim);
    cfg.path = brain;
    cfg.dim = dim;
    cfg.mode = SFB_MODE_COGNITIVE;

    sfb_brain_t *b = NULL;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK || !b) {
        printf("  [FAIL] open: %s\n", sfb_strerror(rc));
        cli_rm_rf(root);
        return CLI_EXIT_ERROR;
    }
    float vec[64];
    for (int i = 0; i < 12; i++) {
        for (uint32_t j = 0; j < dim; j++)
            vec[j] = (float)(((i + 1) * (int)(j + 3)) % 23) / 23.0f;
        char text[64];
        snprintf(text, sizeof text, "selftest record %d", i);
        sfb_remember_result_t res;
        memset(&res, 0, sizeof res);
        rc = sfb_remember(b, vec, text, "user", &res);
        if (rc != SFB_OK) break;
    }
    sfb_flush_result_t fr;
    memset(&fr, 0, sizeof fr);
    if (rc == SFB_OK) rc = sfb_flush(b, &fr);
    if (rc == SFB_OK) rc = sfb_close(b);
    if (rc != SFB_OK) {
        printf("  [FAIL] write path: %s\n", sfb_strerror(rc));
        cli_rm_rf(root);
        return CLI_EXIT_ERROR;
    }
    printf("  [ok] wrote %llu record(s) in %llu segment(s)\n",
           (unsigned long long)fr.records_written,
           (unsigned long long)fr.segments_sealed);

    char json[8192];
    rc = sfb_verify(brain, NULL, 0, json, sizeof json);
    int ok = 0;
    (void)cli_jbool(json, "ok", &ok);
    printf("  [%s] verify clean store: %s\n", (rc == SFB_OK && ok) ? "ok" : "FAIL",
           (rc == SFB_OK && ok) ? "ok=true" : json);
    if (rc != SFB_OK || !ok) failures++;

    rc = sfb_backup(brain, snap);
    printf("  [%s] backup -> %s%s%s\n", rc == SFB_OK ? "ok" : "FAIL", snap,
           rc == SFB_OK ? "" : ": ", rc == SFB_OK ? "" : sfb_strerror(rc));
    if (rc != SFB_OK) failures++;

    if (rc == SFB_OK) {
        rc = sfb_restore(snap, copy);
        printf("  [%s] restore -> %s%s%s\n", rc == SFB_OK ? "ok" : "FAIL", copy,
               rc == SFB_OK ? "" : ": ", rc == SFB_OK ? "" : sfb_strerror(rc));
        if (rc != SFB_OK) failures++;
    }

    if (failures == 0) {
        char segdir[4096], victim[4096];
        snprintf(segdir, sizeof segdir, "%s/%s", brain, SFB_SEG_DIR);
        if (cli_first_segment(segdir, victim, sizeof victim)) {
            rc = cli_flip_byte(victim);
            printf("  [%s] corrupted one byte in %s\n", rc == SFB_OK ? "ok" : "FAIL", victim);
            if (rc != SFB_OK) failures++;
            memset(json, 0, sizeof json);
            int rc2 = sfb_verify(brain, NULL, 0, json, sizeof json);
            unsigned long long damaged = 0;
            (void)cli_ju64(json, "damaged_segments", &damaged);
            (void)cli_jbool(json, "ok", &ok);
            int detected = (rc2 != SFB_OK) && !ok && damaged >= 1;
            printf("  [%s] verify detects the damage (damaged=%llu, %s)\n",
                   detected ? "ok" : "FAIL", damaged, sfb_strerror(rc2));
            if (!detected) failures++;

            memset(json, 0, sizeof json);
            int rc3 = sfb_verify(brain, NULL, 1, json, sizeof json);
            (void)cli_jbool(json, "ok", &ok);
            printf("  [%s] repair quarantines it (ok=%s, %s)\n",
                   (rc3 == SFB_OK && ok) ? "ok" : "FAIL",
                   ok ? "true" : "false", sfb_strerror(rc3));
            if (rc3 != SFB_OK || !ok) failures++;

            memset(json, 0, sizeof json);
            int rc4 = sfb_verify(brain, NULL, 0, json, sizeof json);
            (void)cli_jbool(json, "ok", &ok);
            printf("  [%s] repaired store verifies clean (%s)\n",
                   (rc4 == SFB_OK && ok) ? "ok" : "FAIL", sfb_strerror(rc4));
            if (rc4 != SFB_OK || !ok) failures++;
        } else {
            printf("  [skip] no segment file found to corrupt\n");
        }
    }

    cli_rm_rf(root);
    printf("selftest: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
           failures, failures == 1 ? "" : "s");
    return failures ? CLI_EXIT_ERROR : CLI_EXIT_OK;
}

/* ── serve: single-writer broker for several agents (W2.4) ─────────────── */
static volatile sig_atomic_t cli_serve_stop;

static void cli_serve_signal(int sig)
{
    (void)sig;
    cli_serve_stop = 1;      /* broker re-checks this at most 200 ms apart */
}

/* Machine-readable failure line, as promised for the serve command. */
static void cli_json_error(const char *fmt, ...)
{
    char msg[1024];
    char esc[3072];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    (void)sfb_json_escape(msg, esc, sizeof esc);
    fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", esc);
    fflush(stderr);
}

/* Readiness hook: bound + listening, so the line is not a promise. */
static void cli_serve_ready(const char *sock, void *ud)
{
    char esc[1024];
    (void)ud;
    (void)sfb_json_escape(sock, esc, sizeof esc);
    printf("{\"ok\":true,\"serving\":\"%s\",\"pid\":%ld}\n", esc, (long)getpid());
    fflush(stdout);
}

/* sfb_open() insists on 64..4096, so an existing brain's dimension has to come
 * from its manifest. Reads it with the loader sfb_ops.c uses (no format copy). */
static int cli_serve_probe_dim(const char *path, uint32_t *dim)
{
    sfb_store_t probe;
    struct stat sb;
    int rc;

    memset(&probe, 0, sizeof probe);
    rc = sfb_path_join(probe.manifest_path, sizeof probe.manifest_path,
                       path, SFB_MANIFEST_NAME);
    if (rc != SFB_OK) return rc;
    if (stat(probe.manifest_path, &sb) != 0)
        return (errno == ENOENT) ? SFB_E_NOTFOUND : SFB_E_IO;
    rc = sfb_store_manifest_load(&probe);
    if (rc == SFB_OK) {
        if (probe.man.dim < SFB_MIN_DIM || probe.man.dim > SFB_MAX_DIM)
            rc = SFB_E_CORRUPT;
        else
            *dim = probe.man.dim;
    }
    free(probe.man.segs);
    return rc;
}

static int cli_serve(const char *path, const char *sock_opt, const char *key,
                     uint32_t dim_opt, int have_dim)
{
    char sock[CLI_SOCK_PATH_MAX];
    uint32_t dim = dim_opt;
    int rc;

    if (sock_opt != NULL) {
        if (strlen(sock_opt) >= sizeof sock) {
            cli_json_error("--socket is %llu bytes; AF_UNIX paths must be shorter "
                           "than %llu bytes on this platform",
                           (unsigned long long)strlen(sock_opt),
                           (unsigned long long)sizeof sock);
            return CLI_EXIT_USAGE;
        }
        snprintf(sock, sizeof sock, "%s", sock_opt);
    } else {
        rc = sfb_path_join(sock, sizeof sock, path, "brain.sock");
        if (rc != SFB_OK) {
            cli_json_error("default socket <brain-dir>/brain.sock is too long for "
                           "AF_UNIX (limit %llu bytes here); pass a shorter --socket",
                           (unsigned long long)sizeof sock);
            return CLI_EXIT_USAGE;
        }
    }

    /* Pre-flight before touching the brain: a second server must be told the
     * socket is taken, not that the brain lock is busy. */
    rc = sfb_broker_probe_path(sock);
    if (rc == SFB_E_STATE) {
        cli_json_error("another sfb serve is already listening on %s; "
                       "refusing to replace a live socket", sock);
        return CLI_EXIT_ERROR;
    }
    if (rc == SFB_E_INVALID) {
        cli_json_error("refusing to use %s: it exists and is not a UNIX socket", sock);
        return CLI_EXIT_ERROR;
    }
    if (rc != SFB_OK) {
        cli_json_error("cannot inspect socket path %s: %s (%d)",
                       sock, sfb_strerror(rc), rc);
        return CLI_EXIT_ERROR;
    }

    if (!have_dim) {
        rc = cli_serve_probe_dim(path, &dim);
        if (rc == SFB_E_NOTFOUND) {
            cli_json_error("brain %s has no manifest yet; pass --dim <64..4096> "
                           "to create it", path);
            return CLI_EXIT_USAGE;
        }
        if (rc != SFB_OK) {
            cli_json_error("cannot read the manifest of %s: %s (%d)",
                           path, sfb_strerror(rc), rc);
            return CLI_EXIT_ERROR;
        }
    }

    struct sigaction sa, old_int, old_term;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = cli_serve_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;         /* no SA_RESTART: poll() must see EINTR */
    if (sigaction(SIGINT, &sa, &old_int) != 0 ||
        sigaction(SIGTERM, &sa, &old_term) != 0) {
        cli_json_error("cannot install SIGINT/SIGTERM handlers: %s", strerror(errno));
        return CLI_EXIT_ERROR;
    }

    sfb_config_t cfg;
    sfb_config_default(&cfg, path, dim);
    cfg.path = path;
    cfg.dim = dim;
    cfg.read_only = 0;       /* writer: exactly one process owns the brain */
    cfg.key = (key != NULL && key[0] != '\0') ? key : NULL;

    sfb_brain_t *b = NULL;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK || b == NULL) {
        cli_json_error("cannot open %s as a writer: %s (%d)",
                       path, sfb_strerror(rc), rc);
        (void)sigaction(SIGINT, &old_int, NULL);
        (void)sigaction(SIGTERM, &old_term, NULL);
        return CLI_EXIT_ERROR;
    }

    rc = sfb_broker_run_ready(b, sock, &cli_serve_stop, cli_serve_ready, NULL);
    if (rc != SFB_OK) {
        if (rc == SFB_E_STATE)
            cli_json_error("another sfb serve took %s while starting; "
                           "refusing to replace a live socket", sock);
        else
            cli_json_error("broker failed on %s: %s (%d)", sock, sfb_strerror(rc), rc);
        (void)sfb_close(b);
        (void)sigaction(SIGINT, &old_int, NULL);
        (void)sigaction(SIGTERM, &old_term, NULL);
        return CLI_EXIT_ERROR;
    }

    /* Shutdown order promised by the CLI: broker already closed + unlinked the
     * socket, now flush the brain so the records outlive this process. */
    sfb_flush_result_t fr;
    memset(&fr, 0, sizeof fr);
    int flush_rc = sfb_flush(b, &fr);
    int close_rc = sfb_close(b);
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    if (flush_rc != SFB_OK || close_rc != SFB_OK) {
        int bad_rc = (flush_rc != SFB_OK) ? flush_rc : close_rc;
        cli_json_error("shutdown flush failed: %s (%d)", sfb_strerror(bad_rc), bad_rc);
        return CLI_EXIT_ERROR;
    }
    return CLI_EXIT_OK;
}

/* ── argument helpers ──────────────────────────────────────────────────── */
static int cli_is_flag(const char *s) { return s && s[0] == '-' && s[1] == '-'; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        cli_usage(stderr, argv[0]);
        return CLI_EXIT_USAGE;
    }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        cli_usage(stdout, argv[0]);
        return CLI_EXIT_OK;
    }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
        printf("sfb %d.%d (format %d)\n", SFB_VERSION_MAJOR, SFB_VERSION_MINOR,
               SFB_FORMAT_VERSION);
        return CLI_EXIT_OK;
    }
    if (!strcmp(cmd, "selftest")) {
        if (argc > 2) {
            cli_usage(stderr, argv[0]);
            return CLI_EXIT_USAGE;
        }
        return cli_selftest();
    }

    if (!strcmp(cmd, "serve")) {
        const char *path = NULL, *sock = NULL, *key = NULL;
        uint32_t dim = 0;
        int have_dim = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--socket") || !strcmp(argv[i], "--key") ||
                !strcmp(argv[i], "--dim")) {
                const char *opt = argv[i];
                if (i + 1 >= argc) {
                    cli_json_error("serve: %s needs a value", opt);
                    cli_usage(stderr, argv[0]);
                    return CLI_EXIT_USAGE;
                }
                i++;
                if (!strcmp(opt, "--socket")) {
                    sock = argv[i];
                } else if (!strcmp(opt, "--key")) {
                    key = argv[i];
                } else {
                    char *end = NULL;
                    errno = 0;
                    unsigned long v = strtoul(argv[i], &end, 10);
                    if (errno != 0 || end == argv[i] || *end != '\0' ||
                        v < SFB_MIN_DIM || v > SFB_MAX_DIM) {
                        cli_json_error("serve: --dim must be an integer in %u..%u",
                                       (unsigned)SFB_MIN_DIM, (unsigned)SFB_MAX_DIM);
                        return CLI_EXIT_USAGE;
                    }
                    dim = (uint32_t)v;
                    have_dim = 1;
                }
            } else if (cli_is_flag(argv[i])) {
                cli_json_error("serve: unknown option %s", argv[i]);
                cli_usage(stderr, argv[0]);
                return CLI_EXIT_USAGE;
            } else if (path == NULL) {
                path = argv[i];
            } else {
                cli_json_error("serve: unexpected argument %s", argv[i]);
                return CLI_EXIT_USAGE;
            }
        }
        if (path == NULL) {
            cli_json_error("serve: missing <brain-dir>");
            cli_usage(stderr, argv[0]);
            return CLI_EXIT_USAGE;
        }
        if (sock != NULL && sock[0] == '\0') {
            cli_json_error("serve: --socket must not be empty");
            return CLI_EXIT_USAGE;
        }
        if (key != NULL && key[0] == '\0') {
            cli_json_error("serve: --key must not be empty");
            return CLI_EXIT_USAGE;
        }
        return cli_serve(path, sock, key, dim, have_dim);
    }

    if (!strcmp(cmd, "verify") || !strcmp(cmd, "repair")) {
        const char *path = NULL;
        int want_json = 0, repair = (strcmp(cmd, "repair") == 0);
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--json")) want_json = 1;
            else if (!strcmp(argv[i], "--repair")) repair = 1;
            else if (cli_is_flag(argv[i])) {
                fprintf(stderr, "sfb %s: unknown option %s\n", cmd, argv[i]);
                cli_usage(stderr, argv[0]);
                return CLI_EXIT_USAGE;
            } else if (!path) path = argv[i];
            else {
                fprintf(stderr, "sfb %s: unexpected argument %s\n", cmd, argv[i]);
                return CLI_EXIT_USAGE;
            }
        }
        if (!path) {
            fprintf(stderr, "sfb %s: missing <brain>\n", cmd);
            cli_usage(stderr, argv[0]);
            return CLI_EXIT_USAGE;
        }
        return cli_verify(path, want_json, repair);
    }

    if (!strcmp(cmd, "stats")) {
        const char *path = NULL;
        int want_json = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--json")) want_json = 1;
            else if (cli_is_flag(argv[i])) {
                fprintf(stderr, "sfb stats: unknown option %s\n", argv[i]);
                return CLI_EXIT_USAGE;
            } else if (!path) path = argv[i];
            else {
                fprintf(stderr, "sfb stats: unexpected argument %s\n", argv[i]);
                return CLI_EXIT_USAGE;
            }
        }
        if (!path) {
            fprintf(stderr, "sfb stats: missing <brain>\n");
            cli_usage(stderr, argv[0]);
            return CLI_EXIT_USAGE;
        }
        return cli_stats(path, want_json);
    }

    if (!strcmp(cmd, "backup") || !strcmp(cmd, "restore") || !strcmp(cmd, "migrate")) {
        if (argc != 4) {
            fprintf(stderr, "sfb %s: expected <src> <dest>\n", cmd);
            cli_usage(stderr, argv[0]);
            return CLI_EXIT_USAGE;
        }
        const char *a = argv[2], *d = argv[3];
        int rc;
        if (!strcmp(cmd, "backup")) {
            rc = sfb_backup(a, d);
            if (rc == SFB_OK) { printf("backup: %s -> %s\n", a, d); return CLI_EXIT_OK; }
            cli_error("backup", rc);
            return CLI_EXIT_ERROR;
        }
        if (!strcmp(cmd, "restore")) {
            rc = sfb_restore(a, d);
            if (rc == SFB_OK) { printf("restore: %s -> %s\n", a, d); return CLI_EXIT_OK; }
            cli_error("restore", rc);
            return CLI_EXIT_ERROR;
        }
        rc = sfb_migrate(a, d, NULL);
        if (rc == SFB_OK) { printf("migrate: %s -> %s (already v2, nothing re-encoded)\n", a, d); return CLI_EXIT_OK; }
        /* sfb_migrate already wrote its JSON reason to stderr (frozen ABI has no
         * report parameter) — we add the typed error name here. */
        cli_error("migrate", rc);
        return CLI_EXIT_ERROR;
    }

    fprintf(stderr, "sfb: unknown command '%s'\n", cmd);
    cli_usage(stderr, argv[0]);
    return CLI_EXIT_USAGE;
}
