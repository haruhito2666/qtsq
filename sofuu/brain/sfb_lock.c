/*
 * sfb_lock.c — Sofuu Brain v2: flock-based ownership (W2.1 / W2.2 / W2.3 helper).
 *
 * One lock file per brain (<store>/brain.qtsq.lock). Writers take LOCK_EX,
 * readers LOCK_SH, both non-blocking: a busy lock is reported as SFB_E_LOCKED
 * rather than parking the caller.
 *
 * Stale reclaim is flock-driven, never time-driven. If the previous holder
 * died, the kernel has already dropped its flock, so the next owner's own
 * flock() succeeds; the stale pid record is then simply overwritten by
 * truncate+write. A lock file whose recorded pid is dead but which is still
 * flock()ed (i.e. the pid was recycled or the file is merely leftover
 * metadata) is *not* stolen — flock is the single source of truth.
 *
 * Author: Haruhito
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* sfb_internal.h pins _POSIX_C_SOURCE, which hides BSD flock() on Darwin. */
#define _DARWIN_C_SOURCE 1
#endif

#include "sfb_internal.h"

#include <sys/file.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/*
 * SFB_E_STALE — "a lock file exists, it names a holder, and that pid is dead".
 *
 * sfb.h is frozen and its enum has no stale code, so it is defined here, in the
 * -1008 slot (the frozen enum uses -1007 and then -1012). The #ifndef guard
 * means that if the code is ever promoted into sfb.h, this definition yields to
 * it instead of fighting it.
 *
 * Returned *only* by sfb_lock_holder(); sfb_lock_acquire() transparently
 * reclaims such a lock and returns SFB_OK.
 */
#ifndef SFB_E_STALE
#define SFB_E_STALE ((int)-1008)
#endif

#define SFB_LOCK_RECORD_MAX 512

/* ── small local helpers (this file links without sfb_util.c) ───────────── */

static int lock_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return SFB_E_IO;
        }
        if (n == 0) return SFB_E_ENOSPC;
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return SFB_OK;
}

static void lock_iso8601_now(char *out, size_t cap)
{
    time_t     now = time(NULL);
    struct tm  tm;

    if (cap == 0) return;
    out[0] = '\0';
    if (now == (time_t)-1 || gmtime_r(&now, &tm) == NULL) return;
    if (strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) out[0] = '\0';
}

/* Copy the value of a "<key> <value>\n" line into out. Returns 1 if found. */
static int lock_field(const char *buf, const char *key, char *out, size_t cap)
{
    size_t      klen = strlen(key);
    const char *line = buf;

    while (line != NULL && *line != '\0') {
        const char *nl = strchr(line, '\n');
        size_t      len = nl != NULL ? (size_t)(nl - line) : strlen(line);

        if (len > klen && strncmp(line, key, klen) == 0 && line[klen] == ' ') {
            const char *val  = line + klen + 1;
            size_t      vlen = len - klen - 1;

            if (out != NULL && cap > 0) {
                size_t copy = vlen < cap - 1 ? vlen : cap - 1;

                memcpy(out, val, copy);
                out[copy] = '\0';
            }
            return 1;
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    return 0;
}

/* Parse "pid <n>" out of a lock record. Returns 1 on success. */
static int lock_parse_pid(const char *buf, pid_t *out)
{
    char        tmp[32];
    char       *end = NULL;
    long        v;

    if (!lock_field(buf, "pid", tmp, sizeof tmp)) return 0;
    errno = 0;
    v = strtol(tmp, &end, 10);
    if (errno != 0 || end == tmp || *end != '\0' || v <= 0) return 0;
    if (v > (long)INT32_MAX) return 0;
    *out = (pid_t)v;
    return 1;
}

/*
 * Do we still own this lock file? Two conditions, both cheap:
 *   - the path still resolves to the very inode we hold open, and
 *   - the recorded pid is ours (nobody overwrote our record).
 * The inode check stops us unlinking a file that a racing reclaim replaced.
 */
static int lock_owns_file(int fd, const char *path, pid_t pid)
{
    struct stat fst, pst;
    char        buf[SFB_LOCK_RECORD_MAX];
    ssize_t     n;
    pid_t       recorded = 0;

    if (fstat(fd, &fst) != 0) return 0;
    if (stat(path, &pst) != 0) return 0;
    if (fst.st_dev != pst.st_dev || fst.st_ino != pst.st_ino) return 0;

    n = pread(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';
    if (!lock_parse_pid(buf, &recorded)) return 0;
    return recorded == pid;
}

/* ── acquire ───────────────────────────────────────────────────────────── */

int sfb_lock_acquire(sfb_lock_t *l, const char *path, int shared, int no_lock)
{
    char        started[64];
    char        record[SFB_LOCK_RECORD_MAX];
    int         fd;
    int         rc;
    size_t      plen;
    const char *mode;

    if (l == NULL || path == NULL || path[0] == '\0') return SFB_E_INVALID;

    plen = strlen(path);
    if (plen >= sizeof l->path) return SFB_E_INVALID;

    memset(l, 0, sizeof *l);
    l->fd     = -1;
    l->pid    = getpid();
    l->shared = shared ? 1 : 0;
    memcpy(l->path, path, plen + 1);

    /* O_CLOEXEC: a lock fd must not survive an exec in a spawned child, or the
     * child would keep the brain locked after the real owner has exited. */
    fd = open(path, O_CREAT | O_RDWR
#if defined(O_CLOEXEC)
              | O_CLOEXEC
#endif
              , 0600);
    if (fd < 0) return SFB_E_IO;

    if (!no_lock) {
        int op = (l->shared ? LOCK_SH : LOCK_EX) | LOCK_NB;

        if (flock(fd, op) != 0) {
            int err = errno;
            (void)close(fd);
            return (err == EWOULDBLOCK || err == EAGAIN) ? SFB_E_LOCKED : SFB_E_IO;
        }
        /*
         * Reaching here means we hold the lock. If the file carries a dead
         * holder's record, the kernel already released that flock and we just
         * took it: truncating below is the whole reclaim path.
         */
    }

    l->fd  = fd;
    mode   = l->shared ? "reader" : "writer";
    lock_iso8601_now(started, sizeof started);

    rc = snprintf(record, sizeof record, "pid %ld\nstarted %s\nmode %s\n",
                  (long)l->pid, started, mode);
    if (rc < 0 || (size_t)rc >= sizeof record) {
        (void)sfb_lock_release(l);
        return SFB_E_INVALID;
    }

    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        (void)sfb_lock_release(l);
        return SFB_E_IO;
    }
    rc = lock_write_all(fd, record, (size_t)rc);
    if (rc != SFB_OK) {
        (void)sfb_lock_release(l);
        return rc;
    }

    return SFB_OK;
}

/* ── release ───────────────────────────────────────────────────────────── */

int sfb_lock_release(sfb_lock_t *l)
{
    int rc = SFB_OK;

    if (l == NULL || l->fd < 0) return SFB_OK;

    /* Writers remove the file so a clean release leaves no metadata behind;
     * readers never do (they must not disturb a concurrent writer's record). */
    if (!l->shared && lock_owns_file(l->fd, l->path, l->pid)) {
        if (unlink(l->path) != 0 && errno != ENOENT) rc = SFB_E_IO;
    }
    if (flock(l->fd, LOCK_UN) != 0) rc = SFB_E_IO;
    if (close(l->fd) != 0) rc = SFB_E_IO;

    l->fd = -1;
    return rc;
}

/* ── inspect ───────────────────────────────────────────────────────────── */

int sfb_lock_holder(const char *path, pid_t *pid, char *started_at, size_t cap)
{
    char    buf[SFB_LOCK_RECORD_MAX];
    size_t  used = 0;
    pid_t   recorded = 0;
    int     fd;
    int     rc = SFB_OK;

    if (started_at != NULL && cap > 0) started_at[0] = '\0';
    if (path == NULL || path[0] == '\0') return SFB_E_INVALID;

    fd = open(path, O_RDONLY);
    if (fd < 0) return (errno == ENOENT) ? SFB_E_NOTFOUND : SFB_E_IO;

    while (used < sizeof buf - 1) {
        ssize_t n = read(fd, buf + used, sizeof buf - 1 - used);

        if (n < 0) {
            if (errno == EINTR) continue;
            (void)close(fd);
            return SFB_E_IO;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    (void)close(fd);
    buf[used] = '\0';

    if (!lock_parse_pid(buf, &recorded)) return SFB_E_NOTFOUND;

    /*
     * Liveness probe. kill(pid, 0) == 0 means the pid exists; EPERM means it
     * exists but belongs to someone else (still alive); only ESRCH is proof of
     * death. Anything else is treated as alive — never call a live holder stale.
     */
    errno = 0;
    if (kill(recorded, 0) != 0 && errno == ESRCH) return SFB_E_STALE;

    if (pid != NULL) *pid = recorded;
    if (started_at != NULL && cap > 0) {
        if (!lock_field(buf, "started", started_at, cap)) started_at[0] = '\0';
    }

    return rc;
}
