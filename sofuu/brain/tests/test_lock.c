/*
 * tests/test_lock.c — flock ownership tests for Sofuu Brain v2 (W2.1/W2.2/W2.3).
 *
 * Every exclusion case forks a real child process: flock ownership is only
 * meaningful across processes, so an in-process test would prove nothing.
 *
 * Build:  cc ... sfb_lock.c tests/test_lock.c -o /tmp/sfb_test_lock
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/* sfb.h is frozen and has no stale code; sfb_lock.c defines it as -1008. */
#ifndef SFB_E_STALE
#define SFB_E_STALE ((int)-1008)
#endif

static int  g_fail;
static int  g_pass;
static char g_dir[512];
static char g_path[600];

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  [ok]   %s\n", what);
        g_pass++;
    } else {
        printf("  [FAIL] %s\n", what);
        g_fail++;
    }
}

static const char *rc_name(int rc)
{
    switch (rc) {
    case SFB_OK:         return "SFB_OK";
    case SFB_E_INVALID:  return "SFB_E_INVALID";
    case SFB_E_IO:       return "SFB_E_IO";
    case SFB_E_NOTFOUND: return "SFB_E_NOTFOUND";
    case SFB_E_LOCKED:   return "SFB_E_LOCKED";
    case SFB_E_STALE:    return "SFB_E_STALE";
    default:             return "other";
    }
}

/*
 * Fork a child that tries to take the lock, then report its verdict through the
 * exit status. The child must use _exit(): exiting normally would flush the
 * parent's inherited stdio buffers and duplicate output.
 */
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

/* Write a lock record by hand (simulates a crashed holder's leftover file). */
static int plant_stale_record(const char *path, pid_t dead_pid)
{
    char    buf[256];
    int     fd;
    int     n;

    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return -1;

    n = snprintf(buf, sizeof buf, "pid %ld\nstarted 2020-01-01T00:00:00Z\nmode writer\n",
                 (long)dead_pid);
    if (n > 0 && write(fd, buf, (size_t)n) != n) {
        (void)close(fd);
        return -1;
    }
    return close(fd);
}

/* Harvest a pid that is known dead, by forking a child that immediately exits. */
static pid_t dead_pid(void)
{
    pid_t pid = fork();

    if (pid == 0) _exit(0);
    if (pid < 0) return (pid_t)-1;
    (void)waitpid(pid, NULL, 0);
    return pid;
}

/* ── tests ─────────────────────────────────────────────────────────────── */

static void test_writer_excludes_writer(void)
{
    sfb_lock_t l;
    int        rc;
    int        child;

    printf("writer lock excludes a second writer\n");
    memset(&l, 0, sizeof l);
    l.fd = -1;

    rc = sfb_lock_acquire(&l, g_path, 0, 0);
    check(rc == SFB_OK, "parent acquires writer lock");

    child = child_try(g_path, 0, 0);
    if (child != SFB_E_LOCKED) {
        printf("         (child returned %d %s)\n", child, rc_name(child));
    }
    check(child == SFB_E_LOCKED, "child writer is refused with SFB_E_LOCKED");

    child = child_try(g_path, 1, 0);
    check(child == SFB_E_LOCKED, "child reader is refused while a writer holds it");

    rc = sfb_lock_release(&l);
    check(rc == SFB_OK, "parent releases writer lock");

    child = child_try(g_path, 0, 0);
    check(child == SFB_OK, "after release another writer succeeds");
}

static void test_readers_share(void)
{
    sfb_lock_t l;
    int        rc;

    printf("reader locks are shared\n");
    memset(&l, 0, sizeof l);
    l.fd = -1;

    rc = sfb_lock_acquire(&l, g_path, 1, 0);
    check(rc == SFB_OK, "parent acquires reader lock");

    check(child_try(g_path, 1, 0) == SFB_OK, "second reader locks concurrently");
    check(child_try(g_path, 1, 0) == SFB_OK, "third reader locks concurrently");
    check(child_try(g_path, 0, 0) == SFB_E_LOCKED, "a writer is excluded while readers hold it");

    (void)sfb_lock_release(&l);
}

static void test_reader_excludes_writer_then_vice_versa(void)
{
    sfb_lock_t l;
    int        rc;

    printf("writer/reader exclusion in both directions\n");
    memset(&l, 0, sizeof l);
    l.fd = -1;

    rc = sfb_lock_acquire(&l, g_path, 0, 0);
    check(rc == SFB_OK, "parent acquires writer lock");
    check(child_try(g_path, 1, 0) == SFB_E_LOCKED, "reader excluded by writer");
    (void)sfb_lock_release(&l);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, g_path, 1, 0);
    check(rc == SFB_OK, "parent acquires reader lock");
    check(child_try(g_path, 0, 0) == SFB_E_LOCKED, "writer excluded by reader");
    (void)sfb_lock_release(&l);
}

static void test_record_format_and_holder(void)
{
    sfb_lock_t l;
    pid_t      pid = 0;
    char       started[64];
    char       body[256];
    ssize_t    n;
    int        fd;
    struct stat st;
    int        rc;

    printf("lock file records pid/started/mode and holder() reads it back\n");
    memset(&l, 0, sizeof l);
    l.fd = -1;

    (void)unlink(g_path);
    rc = sfb_lock_acquire(&l, g_path, 0, 0);
    check(rc == SFB_OK, "acquire writer lock");

    fd = open(g_path, O_RDONLY);
    check(fd >= 0, "lock file exists after acquire");
    if (fd >= 0) {
        memset(body, 0, sizeof body);
        n = read(fd, body, sizeof body - 1);
        check(n > 0, "lock file has content");
        check(strncmp(body, "pid ", 4) == 0, "record starts with 'pid '");
        check(strstr(body, "\nstarted ") != NULL, "record has 'started' line");
        check(strstr(body, "\nmode writer\n") != NULL, "record has 'mode writer' line");

        if (fstat(fd, &st) == 0) {
            check((st.st_mode & 0777) == 0600, "lock file mode is 0600");
        } else {
            check(0, "fstat lock file");
        }
        (void)close(fd);
    }

    rc = sfb_lock_holder(g_path, &pid, started, sizeof started);
    check(rc == SFB_OK, "holder() reports a live holder");
    check(pid == getpid(), "holder() reports our own pid");
    check(started[0] != '\0', "holder() reports a start timestamp");

    (void)sfb_lock_release(&l);

    rc = sfb_lock_holder(g_path, &pid, started, sizeof started);
    check(rc == SFB_E_NOTFOUND, "holder() reports SFB_E_NOTFOUND once the writer is gone");
}

static void test_holder_missing_file(void)
{
    char missing[640];
    pid_t pid = 0;

    printf("holder() on a missing file\n");
    snprintf(missing, sizeof missing, "%s/does-not-exist.lock", g_dir);
    check(sfb_lock_holder(missing, &pid, NULL, 0) == SFB_E_NOTFOUND,
          "SFB_E_NOTFOUND for a path that does not exist");
}

static void test_stale_reclaim(void)
{
    sfb_lock_t l;
    pid_t      dead;
    pid_t      seen = 0;
    char       started[64];
    int        rc;

    printf("stale lock file with a dead pid is reclaimed\n");

    dead = dead_pid();
    check(dead > 0, "forked a child that is now dead");
    if (dead <= 0) return;

    errno = 0;
    if (kill(dead, 0) != 0 && errno == ESRCH) {
        check(1, "harvested pid is ESRCH (genuinely dead)");
    } else {
        check(0, "harvested pid is still alive; cannot test stale reclaim");
        return;
    }

    (void)unlink(g_path);
    check(plant_stale_record(g_path, dead) == 0, "planted a lock file naming the dead pid");

    rc = sfb_lock_holder(g_path, &seen, started, sizeof started);
    printf("         (holder() on stale file returned %d %s)\n", rc, rc_name(rc));
    check(rc == SFB_E_STALE, "holder() returns SFB_E_STALE for a dead holder");

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, g_path, 0, 0);
    check(rc == SFB_OK, "acquire reclaims the stale lock");

    seen = 0;
    rc = sfb_lock_holder(g_path, &seen, started, sizeof started);
    check(rc == SFB_OK, "holder() now reports the live owner");
    check(seen == getpid(), "reclaimed record carries our pid");

    (void)sfb_lock_release(&l);
}

static void test_no_lock_still_records(void)
{
    sfb_lock_t l;
    pid_t      pid = 0;
    char       nolock_path[640];
    int        rc;

    printf("no_lock=1 skips flock but still records the pid\n");
    snprintf(nolock_path, sizeof nolock_path, "%s/nolock.lock", g_dir);
    (void)unlink(nolock_path);

    memset(&l, 0, sizeof l);
    l.fd = -1;
    rc = sfb_lock_acquire(&l, nolock_path, 0, 1);
    check(rc == SFB_OK, "no_lock acquire succeeds without flock");

    pid = 0;
    rc = sfb_lock_holder(nolock_path, &pid, NULL, 0);
    check(rc == SFB_OK, "holder() sees the no_lock record");
    check(pid == getpid(), "no_lock still recorded our pid");

    check(child_try(nolock_path, 0, 1) == SFB_OK,
          "another process may also take it while no_lock is set");

    (void)sfb_lock_release(&l);
    (void)unlink(nolock_path);
}

int main(void)
{
    char tmpl[] = "/tmp/sfb_lock_test.XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    snprintf(g_path, sizeof g_path, "%s/brain.qtsq.lock", g_dir);

    printf("── sfb_lock tests (tmp dir %s)\n", g_dir);

    test_writer_excludes_writer();
    test_readers_share();
    test_reader_excludes_writer_then_vice_versa();
    test_record_format_and_holder();
    test_holder_missing_file();
    test_stale_reclaim();
    test_no_lock_still_records();

    (void)unlink(g_path);
    (void)rmdir(g_dir);

    printf("── %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("LOCK TESTS FAILED\n");
        return 1;
    }
    printf("ALL LOCK TESTS PASSED\n");
    return 0;
}
