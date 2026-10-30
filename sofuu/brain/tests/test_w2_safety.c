/*
 * tests/test_w2_safety.c — the two W2 closeout gates.
 *
 *   W2.3  a read-only open must not create, truncate, write or unlink anything
 *         in the store directory and must not move a timestamp there — while
 *         still holding a shared flock, still being refused while a writer owns
 *         the brain, and still excluding a writer for as long as it reads.
 *
 *   W2.4  the broker must never read past a client's payload. A remember or
 *         recall payload carrying the wrong number of floats, a payload that is
 *         long for its declared shape, and a truncated frame header or payload
 *         are all answered with a typed error; the server survives every one and
 *         keeps serving valid requests.
 *
 * Every exclusion case forks a real process: flock ownership only means
 * something across processes. The broker runs in a child, as `sfb serve` does.
 *
 * Build:  cc ... sfb_*.c tests/test_w2_safety.c -o /tmp/sfb_test_w2_safety
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"
#include "sfb_broker.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/*
 * The wire opcodes are the framing contract documented at the top of
 * sfb_broker.c. sfb_broker.h deliberately exports only the client helpers, so
 * a test that has to speak the frame itself restates them.
 */
#define SFB_OP_REMEMBER_TEST 1u
#define SFB_OP_RECALL_TEST   2u
#define SFB_OP_FORGET_TEST   6u

/* ── tiny harness ─────────────────────────────────────────────────────── */

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

static int read_file(const char *path, char *out, size_t cap)
{
    int     fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) return -1;
    n = read(fd, out, cap - 1);
    (void)close(fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return (int)n;
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

/* ── store-directory snapshot ─────────────────────────────────────────── */
/*
 * The acceptance for W2.3 is "the directory is byte-identical after a read-only
 * open": names, sizes, mtimes and inode numbers, plus the directory's own inode
 * and mtime (a create/unlink inside it moves those). atime is deliberately not
 * compared: reading a manifest legitimately touches it, and the defect under
 * test is a *write*.
 */
typedef struct {
    char           name[256];
    dev_t          dev;
    ino_t          ino;
    mode_t         mode;
    off_t          size;
    struct timespec mtime;
    struct timespec ctime;
} snap_entry_t;

#define SNAP_MAX 512

typedef struct {
    dev_t          dev;
    ino_t          ino;
    off_t          size;
    struct timespec mtime;
    snap_entry_t   e[SNAP_MAX];
    int            n;
} snap_t;

static void st_mtimes(const struct stat *st, struct timespec *mt, struct timespec *ct)
{
#if defined(__APPLE__)
    *mt = st->st_mtimespec;
    *ct = st->st_ctimespec;
#else
    *mt = st->st_mtim;
    *ct = st->st_ctim;
#endif
}

static int snap_cmp(const void *a, const void *b)
{
    return strcmp(((const snap_entry_t *)a)->name, ((const snap_entry_t *)b)->name);
}

static int snapshot_dir(const char *dir, snap_t *s)
{
    struct stat st;
    struct timespec ignored;
    struct dirent *de;
    DIR *d;

    memset(s, 0, sizeof *s);
    if (stat(dir, &st) != 0) return -1;
    s->dev = st.st_dev;
    s->ino = st.st_ino;
    s->size = st.st_size;
    st_mtimes(&st, &s->mtime, &ignored);

    d = opendir(dir);
    if (d == NULL) return -1;
    while ((de = readdir(d)) != NULL) {
        char        path[4096];
        struct stat est;
        snap_entry_t *e;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (s->n >= SNAP_MAX) { (void)closedir(d); return -1; }
        if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path) {
            (void)closedir(d);
            return -1;
        }
        if (lstat(path, &est) != 0) { (void)closedir(d); return -1; }
        e = &s->e[s->n++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->dev = est.st_dev;
        e->ino = est.st_ino;
        e->mode = est.st_mode;
        e->size = est.st_size;
        st_mtimes(&est, &e->mtime, &e->ctime);
    }
    (void)closedir(d);
    qsort(s->e, (size_t)s->n, sizeof s->e[0], snap_cmp);
    return 0;
}

static int snap_has(const snap_t *s, const char *name)
{
    int i;

    for (i = 0; i < s->n; i++)
        if (strcmp(s->e[i].name, name) == 0) return 1;
    return 0;
}

/* 1 = identical; otherwise writes the first difference into why. */
static int snap_same(const snap_t *a, const snap_t *b, char *why, size_t cap)
{
    int i;

    why[0] = '\0';
    if (a->dev != b->dev || a->ino != b->ino) {
        snprintf(why, cap, "store directory was replaced");
        return 0;
    }
    if (a->size != b->size) {
        snprintf(why, cap, "store directory size %lld -> %lld",
                 (long long)a->size, (long long)b->size);
        return 0;
    }
    if (a->mtime.tv_sec != b->mtime.tv_sec || a->mtime.tv_nsec != b->mtime.tv_nsec) {
        snprintf(why, cap, "store directory mtime moved");
        return 0;
    }
    if (a->n != b->n) {
        snprintf(why, cap, "entry count %d -> %d", a->n, b->n);
        return 0;
    }
    for (i = 0; i < a->n; i++) {
        const snap_entry_t *x = &a->e[i], *y = &b->e[i];

        if (strcmp(x->name, y->name) != 0) {
            snprintf(why, cap, "entry #%d is '%s' before, '%s' after", i, x->name, y->name);
            return 0;
        }
        if (x->ino != y->ino || x->dev != y->dev) {
            snprintf(why, cap, "%s: inode changed", x->name);
            return 0;
        }
        if (x->size != y->size) {
            snprintf(why, cap, "%s: size %lld -> %lld", x->name,
                     (long long)x->size, (long long)y->size);
            return 0;
        }
        if (x->mode != y->mode) {
            snprintf(why, cap, "%s: mode changed", x->name);
            return 0;
        }
        if (x->mtime.tv_sec != y->mtime.tv_sec || x->mtime.tv_nsec != y->mtime.tv_nsec) {
            snprintf(why, cap, "%s: mtime moved", x->name);
            return 0;
        }
        if (x->ctime.tv_sec != y->ctime.tv_sec || x->ctime.tv_nsec != y->ctime.tv_nsec) {
            snprintf(why, cap, "%s: ctime moved", x->name);
            return 0;
        }
    }
    return 1;
}

/* ── lock helpers (same shape as tests/test_lock.c) ───────────────────── */

static int child_try(const char *path, int shared, int no_lock)
{
    pid_t pid = fork();
    int   status = 0;
    int   code;

    if (pid < 0) return -9999;
    if (pid == 0) {
        sfb_lock_t l;
        int        rc = sfb_lock_acquire(&l, path, shared, no_lock);

        if (rc == SFB_OK) {
            (void)sfb_lock_release(&l);
            _exit(0);
        }
        _exit(rc == SFB_E_LOCKED ? 10 : 11);
    }
    if (waitpid(pid, &status, 0) != pid) return -9998;
    if (!WIFEXITED(status)) return -9997;
    code = WEXITSTATUS(status);
    if (code == 0)  return SFB_OK;
    if (code == 10) return SFB_E_LOCKED;
    return -9996;
}

static pid_t dead_pid(void)
{
    pid_t pid = fork();

    if (pid == 0) _exit(0);
    if (pid < 0) return (pid_t)-1;
    (void)waitpid(pid, NULL, 0);
    return pid;
}

static int plant_record(const char *path, pid_t pid, const char *mode)
{
    char buf[256];
    int  fd;
    int  n;

    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return -1;
    n = snprintf(buf, sizeof buf, "pid %ld\nstarted 2020-01-01T00:00:00Z\nmode %s\n",
                 (long)pid, mode);
    if (n <= 0 || write(fd, buf, (size_t)n) != n) {
        (void)close(fd);
        return -1;
    }
    return close(fd);
}

/* ── W2.3: a read-only open leaves the store untouched ────────────────── */

#define DIM 128u

/* Build a small, clean brain. The writer's clean release removes the lock file,
 * so the reader below runs against a store with no lock file at all — the case
 * that used to be created out of thin air. */
static int build_store(const char *brain, uint32_t dim, uint32_t nrec)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    float        v[4096];
    uint32_t     i;

    if (dim > 4096u) return SFB_E_INVALID;
    sfb_config_default(&cfg, brain, dim);
    if (sfb_open(&cfg, &b) != SFB_OK) return SFB_E_IO;
    for (i = 0; i < nrec; i++) {
        char                 text[64];
        sfb_remember_result_t r;

        fill_vec(v, dim, i + 1u);
        snprintf(text, sizeof text, "stored memory %u", i);
        if (sfb_remember(b, v, text, "user", &r) != SFB_OK) {
            (void)sfb_close(b);
            return SFB_E_IO;
        }
    }
    if (sfb_flush(b, NULL) != SFB_OK) {
        (void)sfb_close(b);
        return SFB_E_IO;
    }
    return sfb_close(b);
}

/* Exercise everything a reader does: open, count, recall, list, stats, close. */
static int read_only_session(const char *brain, uint32_t dim, uint64_t *live_out)
{
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    sfb_hit_t    hits[8];
    sfb_stats_t  st;
    uint64_t     live = 0;
    float        q[4096];
    uint32_t     n = 0;
    int          rc;

    sfb_config_default(&cfg, brain, dim);
    cfg.read_only = 1;
    rc = sfb_open(&cfg, &b);
    if (rc != SFB_OK) return rc;

    if ((rc = sfb_count(b, &live)) != SFB_OK) goto out;
    fill_vec(q, dim, 7u);
    if ((rc = sfb_recall(b, q, 4, hits, &n)) != SFB_OK) goto out;
    if ((rc = sfb_list(b, 0, 8, hits, &n)) != SFB_OK) goto out;
    if ((rc = sfb_stats_get(b, &st)) != SFB_OK) goto out;
    /* a reader may not write */
    {
        sfb_remember_result_t rr;

        if (sfb_remember(b, q, "reader must not write", "user", &rr) != SFB_E_READONLY) {
            rc = SFB_E_STATE;
            goto out;
        }
        if (sfb_forget(b, 0) != SFB_E_READONLY) {
            rc = SFB_E_STATE;
            goto out;
        }
    }

out:
    if (live_out != NULL) *live_out = live;
    (void)sfb_close(b);
    return rc;
}

static void test_reader_leaves_clean_store_untouched(void)
{
    char  brain[600], lock[700], why[512];
    snap_t before, after;
    uint64_t live = 0;
    int   rc;

    printf("W2.3a read-only open of a clean store: nothing appears, nothing moves\n");
    snprintf(brain, sizeof brain, "%s/clean", g_root);
    snprintf(lock, sizeof lock, "%s/%s", brain, SFB_LOCK_NAME);

    rc = build_store(brain, DIM, 3u);
    check(rc == SFB_OK, "built and closed a clean writer brain", rc_name(rc));
    if (rc != SFB_OK) return;

    check(!file_exists(lock), "a clean writer release left no lock file (precondition)", NULL);
    check(snapshot_dir(brain, &before) == 0, "snapshotted the store directory", NULL);
    check(!snap_has(&before, SFB_LOCK_NAME), "no lock file in the snapshot (precondition)", NULL);

    rc = read_only_session(brain, DIM, &live);
    check(rc == SFB_OK, "read-only open + count + recall + list + stats + close", rc_name(rc));
    check(live == 3u, "the reader saw all three records", NULL);

    check(!file_exists(lock), "no lock file was created by the read-only open", NULL);

    check(snapshot_dir(brain, &after) == 0, "re-snapshotted the store directory", NULL);
    check(snap_same(&before, &after, why, sizeof why),
          "store directory byte-identical after the read-only open (names, sizes, mtimes, inodes)",
          why);

    (void)rmdir(brain);
}

static void test_reader_leaves_existing_lock_file_untouched(void)
{
    char  brain[600], lock[700], why[512];
    char  body_before[256], body_after[256];
    snap_t before, after;
    pid_t dead;
    uint64_t live = 0;
    int   rc;

    printf("W2.3b read-only open with a leftover lock file: it is not rewritten\n");
    snprintf(brain, sizeof brain, "%s/leftover", g_root);
    snprintf(lock, sizeof lock, "%s/%s", brain, SFB_LOCK_NAME);

    rc = build_store(brain, DIM, 2u);
    check(rc == SFB_OK, "built and closed a clean writer brain", rc_name(rc));
    if (rc != SFB_OK) return;

    dead = dead_pid();
    check(dead > 0, "harvested a dead pid", NULL);
    check(plant_record(lock, dead, "writer") == 0, "planted a leftover writer record", NULL);
    check(read_file(lock, body_before, sizeof body_before) > 0, "read the leftover record", NULL);

    check(snapshot_dir(brain, &before) == 0, "snapshotted the store directory", NULL);

    rc = read_only_session(brain, DIM, &live);
    check(rc == SFB_OK, "read-only open succeeds with a leftover lock file", rc_name(rc));

    check(read_file(lock, body_after, sizeof body_after) > 0, "the lock file is still there", NULL);
    check(strcmp(body_before, body_after) == 0,
          "the reader did not truncate or rewrite the lock file", NULL);
    check(strstr(body_after, "mode writer") != NULL,
          "the leftover writer record still says mode writer", NULL);

    check(snapshot_dir(brain, &after) == 0, "re-snapshotted the store directory", NULL);
    check(snap_same(&before, &after, why, sizeof why),
          "store directory byte-identical after the read-only open", why);

    (void)unlink(lock);
    (void)rmdir(brain);
}

static void test_reader_still_excludes_a_writer(void)
{
    char  brain[600], lock[700];
    sfb_lock_t l;
    int   rc;

    printf("W2.3c a reader with no lock file still excludes a writer\n");
    snprintf(brain, sizeof brain, "%s/exclude", g_root);
    snprintf(lock, sizeof lock, "%s/%s", brain, SFB_LOCK_NAME);
    check(mkdir(brain, 0755) == 0, "made a store directory", NULL);
    (void)unlink(lock);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, lock, 1, 0);
    check(rc == SFB_OK, "parent takes a reader lock on a store with no lock file", rc_name(rc));
    check(!file_exists(lock), "taking the reader lock created no lock file", NULL);
    check(child_try(lock, 1, 0) == SFB_OK, "a second reader is admitted", NULL);
    check(child_try(lock, 0, 0) == SFB_E_LOCKED,
          "a writer is refused (SFB_E_LOCKED) while the reader holds the store", NULL);
    check(!file_exists(lock), "the refused writer created no lock file", NULL);

    check(sfb_lock_release(&l) == SFB_OK, "parent releases the reader lock", NULL);
    check(child_try(lock, 0, 0) == SFB_OK, "once the reader is gone a writer succeeds", NULL);
    check(!file_exists(lock), "the writer's clean release removed the lock file", NULL);
    (void)rmdir(brain);
}

static void test_reader_refused_while_writer_owns_brain(void)
{
    char  brain[600];
    sfb_config_t cfg;
    sfb_brain_t *b = NULL;
    pid_t pid;
    int   ready[2], go[2];
    int   rc, st = 0;
    char  byte = 0;
    ssize_t got;

    printf("W2.3d a read-only open is refused while a writer owns the brain\n");
    snprintf(brain, sizeof brain, "%s/writer", g_root);
    rc = build_store(brain, DIM, 1u);
    check(rc == SFB_OK, "built a brain to hold", rc_name(rc));
    if (rc != SFB_OK) return;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        check(0, "pipes for the writer child", NULL);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) { check(0, "fork a writer child", NULL); return; }
    if (pid == 0) {                              /* child: own the brain */
        sfb_config_t wc;
        sfb_brain_t *cb = NULL;

        close(ready[0]);
        close(go[1]);
        sfb_config_default(&wc, brain, DIM);
        if (sfb_open(&wc, &cb) != SFB_OK) _exit(20);
        if (write(ready[1], "R", 1) != 1) _exit(21);
        close(ready[1]);
        while (read(go[0], &byte, 1) < 0 && errno == EINTR) { }
        close(go[0]);
        (void)sfb_close(cb);
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    got = read(ready[0], &byte, 1);
    close(ready[0]);
    check(got == 1, "the writer child holds the brain", NULL);
    if (got != 1) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
        return;
    }

    sfb_config_default(&cfg, brain, DIM);
    cfg.read_only = 1;
    rc = sfb_open(&cfg, &b);
    check(rc == SFB_E_LOCKED && b == NULL,
          "read-only open is refused with SFB_E_LOCKED by a live writer", rc_name(rc));
    if (b != NULL) (void)sfb_close(b);

    (void)write(go[1], "G", 1);
    close(go[1]);
    (void)waitpid(pid, &st, 0);

    b = NULL;
    rc = sfb_open(&cfg, &b);
    check(rc == SFB_OK, "the same read-only open succeeds once the writer is gone", rc_name(rc));
    if (b != NULL) (void)sfb_close(b);
    (void)remove(brain);
}

static void test_writer_record_and_stale_reclaim(void)
{
    char  brain[600], lock[700], body[256], want[256];
    sfb_lock_t l;
    pid_t dead;
    pid_t seen = 0;
    char  started[64];
    int   rc;

    printf("W2.3e the writer record and stale reclaim are unchanged\n");
    snprintf(brain, sizeof brain, "%s/writer-record", g_root);
    snprintf(lock, sizeof lock, "%s/%s", brain, SFB_LOCK_NAME);
    check(mkdir(brain, 0755) == 0, "made a store directory", NULL);
    (void)unlink(lock);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, lock, 0, 0);
    check(rc == SFB_OK, "writer lock acquired", rc_name(rc));
    check(read_file(lock, body, sizeof body) > 0, "the writer wrote a record", NULL);
    snprintf(want, sizeof want, "pid %ld\nstarted ", (long)getpid());
    check(strncmp(body, want, strlen(want)) == 0, "record starts 'pid <our pid>'", body);
    check(strstr(body, "\nmode writer\n") != NULL, "record says mode writer", NULL);
    check(body[0] != '\0' && body[strlen(body) - 1] == '\n', "record ends with a newline", NULL);
    rc = sfb_lock_holder(lock, &seen, started, sizeof started);
    check(rc == SFB_OK && seen == getpid(), "holder() reports the writer", rc_name(rc));
    check(started[0] != '\0', "holder() reports a start timestamp", NULL);
    check(sfb_lock_release(&l) == SFB_OK, "writer release", NULL);
    check(!file_exists(lock), "a clean writer release removes the lock file", NULL);

    dead = dead_pid();
    check(dead > 0 && kill(dead, 0) != 0 && errno == ESRCH, "harvested a dead pid", NULL);
    check(plant_record(lock, dead, "writer") == 0, "planted a stale writer record", NULL);
    check(sfb_lock_holder(lock, &seen, started, sizeof started) == SFB_E_STALE,
          "holder() calls the dead record stale", NULL);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, lock, 0, 0);
    check(rc == SFB_OK, "a writer reclaims the stale lock", rc_name(rc));
    seen = 0;
    rc = sfb_lock_holder(lock, &seen, started, sizeof started);
    check(rc == SFB_OK && seen == getpid(), "the reclaimed record carries our pid", rc_name(rc));
    check(sfb_lock_release(&l) == SFB_OK, "writer release after reclaim", NULL);
    check(!file_exists(lock), "the reclaimed file is removed on release", NULL);
    (void)rmdir(brain);
}

/* The mirror image of W2.3c: a reader that found a lock file holds *that* file
 * shared, so a writer is refused by the file's flock (not by the directory). */
static void test_reader_on_existing_file_excludes_writer(void)
{
    char  brain[600], lock[700];
    sfb_lock_t l;
    pid_t dead;
    int   rc;

    printf("W2.3f a reader holding an existing lock file excludes a writer\n");
    snprintf(brain, sizeof brain, "%s/file-exclude", g_root);
    snprintf(lock, sizeof lock, "%s/%s", brain, SFB_LOCK_NAME);
    check(mkdir(brain, 0755) == 0, "made a store directory", NULL);
    dead = dead_pid();
    check(dead > 0 && plant_record(lock, dead, "writer") == 0,
          "planted a lock file a reader will open", NULL);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, lock, 1, 0);
    check(rc == SFB_OK, "reader takes the existing lock file shared", rc_name(rc));
    check(child_try(lock, 0, 0) == SFB_E_LOCKED,
          "a writer is refused while a reader holds the existing lock file", NULL);
    check(child_try(lock, 1, 0) == SFB_OK, "another reader still shares it", NULL);
    check(sfb_lock_release(&l) == SFB_OK, "reader release", NULL);
    check(child_try(lock, 0, 0) == SFB_OK, "a writer succeeds once the reader is gone", NULL);
    (void)unlink(lock);
    (void)rmdir(brain);
}

/* ── W2.4: the broker never reads past a client's payload ─────────────── */

#define BROKER_DIM   128u
#define BROKER_SEED  150u          /* > 128: overruns the old 128-hit stack array */

static volatile sig_atomic_t g_stop;

static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

static void ready_cb(const char *sock_path, void *ud)
{
    int fd = *(int *)ud;

    (void)sock_path;
    if (write(fd, "R", 1) != 1) { /* the parent will notice the missing byte */ }
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* A socket with bounded send/recv, so a wedged server fails the test instead of
 * hanging it. */
static int dial(const char *sock_path)
{
    struct sockaddr_un addr;
    struct timeval     tv;
    int                fd;

    if (strlen(sock_path) >= sizeof addr.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Read a whole response frame: i32 status | u32 len | body. */
static int read_reply(int fd, int32_t *status, void **body, uint32_t *blen)
{
    uint32_t hdr[2] = {0, 0};
    size_t   got = 0;
    void    *p = NULL;

    while (got < sizeof hdr) {
        ssize_t n = read(fd, (char *)hdr + got, sizeof hdr - got);

        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;                 /* server closed without a reply */
        got += (size_t)n;
    }
    *status = (int32_t)hdr[0];
    *blen = hdr[1];
    if (hdr[1] > 0u) {
        p = malloc(hdr[1]);
        if (p == NULL) return -1;
        got = 0;
        while (got < hdr[1]) {
            ssize_t n = read(fd, (char *)p + got, hdr[1] - got);

            if (n < 0) {
                if (errno == EINTR) continue;
                free(p);
                return -1;
            }
            if (n == 0) { free(p); return -1; }
            got += (size_t)n;
        }
    }
    if (body != NULL) *body = p; else free(p);
    return 0;
}

/* A well-formed frame: header + full payload. 0 = answered, -1 = transport. */
static int frame_call(const char *sock_path, uint32_t op, const void *payload,
                      uint32_t plen, int32_t *status, void **body, uint32_t *blen)
{
    uint32_t hdr[2];
    int      fd = dial(sock_path);
    int      rc;

    if (fd < 0) return -1;
    hdr[0] = op;
    hdr[1] = plen;
    if (send_all(fd, hdr, sizeof hdr) != 0 ||
        (plen != 0u && send_all(fd, payload, plen) != 0)) {
        (void)close(fd);
        return -1;
    }
    (void)shutdown(fd, SHUT_WR);
    rc = read_reply(fd, status, body, blen);
    (void)close(fd);
    return rc;
}

/*
 * A deliberately truncated frame: send `hdr_bytes` of the 8-byte header and
 * `send_bytes` of the payload, then half-close. The server must answer with a
 * typed error rather than hang, crash or treat the fragment as a request.
 */
static int frame_truncated(const char *sock_path, uint32_t op, uint32_t plen,
                           const void *payload, size_t send_bytes,
                           size_t hdr_bytes, int32_t *status)
{
    uint32_t hdr[2];
    uint32_t blen = 0;
    int      fd = dial(sock_path);
    int      rc;

    if (fd < 0) return -1;
    hdr[0] = op;
    hdr[1] = plen;
    if (send_all(fd, hdr, hdr_bytes) != 0) { (void)close(fd); return -1; }
    if (send_bytes != 0u && send_all(fd, payload, send_bytes) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)shutdown(fd, SHUT_WR);
    rc = read_reply(fd, status, NULL, &blen);
    (void)close(fd);
    return rc;
}

/* Build a remember payload whose vector carries `floats` floats (not the
 * brain's dim) and a short label. */
static unsigned char *remember_payload(uint32_t floats, const char *text,
                                      uint32_t *plen_out)
{
    uint32_t       tl = (uint32_t)strlen(text);
    uint32_t       plen = 4u + floats * 4u + 4u + tl;
    unsigned char *p = (unsigned char *)malloc(plen);
    float         *v = (float *)(void *)(p + 4);
    uint32_t       i;

    if (p == NULL) return NULL;
    memcpy(p, &floats, 4);
    for (i = 0; i < floats; i++) v[i] = ((float)(i % 17u) - 8.0f) / 8.0f;
    memcpy(p + 4 + floats * 4u, &tl, 4);
    memcpy(p + 4 + floats * 4u + 4u, text, tl);
    *plen_out = plen;
    return p;
}

/* Expect a typed refusal: an answered frame whose status is the wanted code. */
static void expect_refusal(const char *sock_path, const char *what, uint32_t op,
                           const void *payload, uint32_t plen, int32_t want)
{
    int32_t  status = 0;
    void    *body = NULL;
    uint32_t blen = 0;
    char     detail[160];
    int      rc;

    rc = frame_call(sock_path, op, payload, plen, &status, &body, &blen);
    free(body);
    if (rc != 0) {
        snprintf(detail, sizeof detail, "no response frame at all (server crashed, closed early or hung)");
        check(0, what, detail);
        return;
    }
    snprintf(detail, sizeof detail, "status=%d %s (want %s)", (int)status, rc_name((int)status),
             rc_name((int)want));
    check(status == want, what, detail);
}

static void test_broker_payload_validation(void)
{
    char  brain[600], sock[700];
    int   ready[2];
    pid_t pid = -1;
    int   st = 0, i;
    char  byte = 0;
    uint64_t live = 0;
    int32_t  status = 0;
    void    *body = NULL;
    uint32_t blen = 0;
    int   rc;

    printf("W2.4 the broker refuses wrong-length and truncated payloads\n");
    snprintf(brain, sizeof brain, "%s/broker", g_root);
    snprintf(sock, sizeof sock, "%s/brain.sock", brain);
    (void)mkdir(brain, 0755);

    if (pipe(ready) != 0) {
        check(0, "pipe for the broker readiness byte", NULL);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) { check(0, "fork the broker child", NULL); return; }
    if (pid == 0) {                             /* child: own + serve the brain */
        sfb_config_t cfg;
        sfb_brain_t *cb = NULL;
        struct sigaction sa;
        int crc;

        close(ready[0]);
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigterm;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGTERM, &sa, NULL);
        sfb_config_default(&cfg, brain, BROKER_DIM);
        if (sfb_open(&cfg, &cb) != SFB_OK) _exit(30);
        crc = sfb_broker_run_ready(cb, sock, &g_stop, ready_cb, &ready[1]);
        (void)sfb_close(cb);
        _exit(crc == SFB_OK ? 0 : 31);
    }
    close(ready[1]);
    if (read(ready[0], &byte, 1) != 1) {
        check(0, "the broker child bound the socket", "no readiness byte");
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
        close(ready[0]);
        return;
    }
    close(ready[0]);
    check(byte == 'R', "the broker child bound the socket", NULL);

    /* ── valid traffic still works ─────────────────────────────────────── */
    {
        float                v[BROKER_DIM];
        sfb_remember_result_t rr;

        fill_vec(v, BROKER_DIM, 1u);
        rc = sfb_broker_remember(sock, v, BROKER_DIM, "seed 0", &rr);
        check(rc == SFB_OK, "a well-formed remember through the client is accepted", rc_name(rc));
    }
    for (i = 1; i < (int)BROKER_SEED; i++) {
        char                 text[64];
        float                v[BROKER_DIM];
        sfb_remember_result_t rr;

        fill_vec(v, BROKER_DIM, (uint32_t)i + 1u);
        snprintf(text, sizeof text, "seed record %d", i);
        if (sfb_broker_remember(sock, v, BROKER_DIM, text, &rr) != SFB_OK) break;
    }
    check(i == (int)BROKER_SEED, "seeded 150 records through the socket", NULL);
    rc = sfb_broker_count(sock, &live);
    check(rc == SFB_OK && live >= BROKER_SEED, "count over the socket matches the seed",
          rc_name(rc));

    /* ── remember with the wrong number of floats ──────────────────────── */
    {
        uint32_t       plen = 0;
        unsigned char *p;

        p = remember_payload(BROKER_DIM - 1u, "short vector", &plen);
        if (p != NULL) {
            expect_refusal(sock, "remember with dim-1 floats -> SFB_E_DIM",
                           SFB_OP_REMEMBER_TEST, p, plen, SFB_E_DIM);
            free(p);
        }
        p = remember_payload(BROKER_DIM + 1u, "long vector", &plen);
        if (p != NULL) {
            expect_refusal(sock, "remember with dim+1 floats -> SFB_E_DIM",
                           SFB_OP_REMEMBER_TEST, p, plen, SFB_E_DIM);
            free(p);
        }
        p = remember_payload(SFB_MIN_DIM, "far too short a vector", &plen);
        if (p != NULL) {
            expect_refusal(sock, "remember with 64 floats (half a brain dim) -> SFB_E_DIM",
                           SFB_OP_REMEMBER_TEST, p, plen, SFB_E_DIM);
            free(p);
        }
    }

    /* ── a payload that is long for its declared shape ─────────────────── */
    {
        uint32_t       plen = 0;
        unsigned char *p = remember_payload(BROKER_DIM, "trailing junk", &plen);

        if (p != NULL) {
            unsigned char *q = (unsigned char *)realloc(p, plen + 16u);
            if (q != NULL) {
                memset(q + plen, 0x41, 16u);
                expect_refusal(sock, "remember with 16 trailing bytes -> SFB_E_INVALID",
                               SFB_OP_REMEMBER_TEST, q, plen + 16u, SFB_E_INVALID);
                free(q);
            } else {
                free(p);
            }
        }
    }

    /* ── truncated frames ─────────────────────────────────────────────── */
    {
        uint32_t       plen = 0;
        unsigned char *p = remember_payload(BROKER_DIM, "will be cut", &plen);

        if (p != NULL) {
            int32_t  s2 = 0;
            int      trc;

            /* payload cut in half */
            trc = frame_truncated(sock, SFB_OP_REMEMBER_TEST, plen, p, plen / 2u,
                                  sizeof(uint32_t) * 2u, &s2);
            check(trc == 0 && s2 == SFB_E_INVALID,
                  "a truncated remember payload is answered with SFB_E_INVALID",
                  trc != 0 ? "no response frame at all" : rc_name((int)s2));

            /* header cut short */
            trc = frame_truncated(sock, SFB_OP_REMEMBER_TEST, plen, p, 0u, 3u, &s2);
            check(trc == 0 && s2 == SFB_E_INVALID,
                  "a truncated frame header is answered with SFB_E_INVALID",
                  trc != 0 ? "no response frame at all" : rc_name((int)s2));

            /* a wholly empty frame */
            trc = frame_truncated(sock, SFB_OP_REMEMBER_TEST, plen, p, 0u, 0u, &s2);
            check(trc == 0 && s2 == SFB_E_INVALID,
                  "an empty frame is answered with SFB_E_INVALID",
                  trc != 0 ? "no response frame at all" : rc_name((int)s2));
            free(p);
        }
    }

    /* ── recall: wrong dim, absurd dim, oversized k ───────────────────── */
    {
        unsigned char buf[16 + 4096 * 4];
        uint32_t      k, dim;
        uint32_t      i7;

        k = 4; dim = BROKER_DIM - 1u;
        memcpy(buf, &k, 4); memcpy(buf + 4, &dim, 4);
        for (i7 = 0; i7 < dim; i7++) { float f = 0.25f; memcpy(buf + 8 + i7 * 4, &f, 4); }
        expect_refusal(sock, "recall with dim-1 floats -> SFB_E_DIM", SFB_OP_RECALL_TEST,
                       buf, 8u + dim * 4u, SFB_E_DIM);

        /* 0xFFFFFFFF floats: the old length check overflowed to a tiny value */
        dim = 0xFFFFFFFFu;
        memcpy(buf, &k, 4); memcpy(buf + 4, &dim, 4);
        expect_refusal(sock, "recall with an absurd dim -> SFB_E_INVALID", SFB_OP_RECALL_TEST,
                       buf, 8u, SFB_E_INVALID);

        /* k larger than any record set: the old code sized hits[] at 128 */
        k = 2000u; dim = BROKER_DIM;
        memcpy(buf, &k, 4); memcpy(buf + 4, &dim, 4);
        for (i7 = 0; i7 < dim; i7++) { float f = 0.5f; memcpy(buf + 8 + i7 * 4, &f, 4); }
        expect_refusal(sock, "recall with k beyond the limit -> SFB_E_INVALID", SFB_OP_RECALL_TEST,
                       buf, 8u + dim * 4u, SFB_E_INVALID);

        /* a legal k that still asks for more hits than the old 128-slot array,
         * against 150 records: the old build overran its stack here */
        k = 200u;
        memcpy(buf, &k, 4); memcpy(buf + 4, &dim, 4);
        rc = frame_call(sock, SFB_OP_RECALL_TEST, buf, 8u + dim * 4u, &status, &body, &blen);
        if (rc != 0) {
            check(0, "recall k=200 over 150 records is served", "no response frame at all");
        } else {
            uint32_t n = 0;
            if (blen >= 4u) memcpy(&n, body, 4);
            check(status == SFB_OK && n <= 200u,
                  "recall k=200 over 150 records is served", rc_name((int)status));
        }
        free(body);
        body = NULL;
    }

    /* ── forget: truncated payload ────────────────────────────────────── */
    {
        unsigned char buf[8];
        uint32_t id = 1;

        memcpy(buf, &id, 4);
        memcpy(buf + 4, &id, 4);
        expect_refusal(sock, "forget with a 2-byte payload -> SFB_E_INVALID", SFB_OP_FORGET_TEST,
                       buf, 2u, SFB_E_INVALID);
    }

    /* ── the server survived all of it ───────────────────────────────── */
    live = 0;
    rc = sfb_broker_count(sock, &live);
    check(rc == SFB_OK && live >= BROKER_SEED,
          "after every malformed frame the server still answers count", rc_name(rc));
    {
        float                 v[BROKER_DIM];
        sfb_remember_result_t rr;

        fill_vec(v, BROKER_DIM, 99u);
        rc = sfb_broker_remember(sock, v, BROKER_DIM, "after the malformed frames", &rr);
        check(rc == SFB_OK, "a valid remember still succeeds after the malformed frames",
              rc_name(rc));
    }
    {
        uint32_t id = 1;

        status = 0;
        rc = frame_call(sock, SFB_OP_FORGET_TEST, &id, 4u, &status, &body, &blen);
        free(body);
        check(rc == 0 && status == SFB_OK, "a well-formed forget is still served",
              rc_name((int)status));
    }

    (void)kill(pid, SIGTERM);
    for (i = 0; i < 100; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = 20L * 1000L * 1000L;
            (void)nanosleep(&ts, NULL);
        }
    }
    if (i >= 100) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
    }
    check(i < 100 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the broker child stopped cleanly after all malformed frames", NULL);

    (void)unlink(sock);
    (void)remove(brain);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char tmpl[] = "/tmp/sfb_w2_safety.XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    printf("── W2.3/W2.4 safety tests (tmp dir %s)\n", g_root);

    test_reader_leaves_clean_store_untouched();
    test_reader_leaves_existing_lock_file_untouched();
    test_reader_still_excludes_a_writer();
    test_reader_refused_while_writer_owns_brain();
    test_writer_record_and_stale_reclaim();
    test_reader_on_existing_file_excludes_writer();
    test_broker_payload_validation();

    rm_rf(g_root);
    printf("── %d passed, %d failed\n", g_pass, g_fail);    if (g_fail != 0) {
        printf("W2 SAFETY TESTS FAILED\n");
        return 1;
    }
    printf("ALL W2 SAFETY TESTS PASSED\n");
    return 0;
}
