/* qtsq_win_compat.h — QTSQ Windows compatibility layer (master header).
 *
 * Force-included on every translation unit (/FI on MSVC, -include on other
 * toolchains), so this file must compile in ANY include order. Rule: never
 * #include <windows.h> here — some QTSQ headers need to define
 * WIN32_LEAN_AND_MEAN/NOMINMAX themselves before windows.h, and this header
 * is included before them (shim/pthread.h and shim/dirent.h own windows.h).
 *
 * Every POSIX-ism mapping is guarded by `#if defined(_WIN32)` (not _MSC_VER)
 * so MinGW/zig-cc builds exercise the same code paths.
 */
#ifndef QTSQ_WIN_COMPAT_H
#define QTSQ_WIN_COMPAT_H

#if defined(_WIN32)

/* math.h: expose M_PI and friends under strict C11 / MSVC. */
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* io.h/_access, direct.h/_mkdir — used for the unistd.h surface below. */
#include <io.h>
#include <direct.h>

/* Prevent windows.h consumers (shim/pthread.h, shim/dirent.h, argon2,
 * randombytes) from dragging in the whole Win32 world or the min/max
 * macros that break C++-style `a < b` expressions. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/* ---- <unistd.h> function surface -------------------------------------- */

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif

#define access _access
#define chmod  _chmod
#define unlink _unlink
/* MSVC's stdio.h already defines fileno as a macro in newer CRTs. */
#if !defined(fileno)
#define fileno _fileno
#endif

/* mkdir(path, mode) → _mkdir(path); POSIX mode bits have no Windows
 * equivalent (ACLs decide access). */
#define mkdir(path, mode) _mkdir(path)

/* fsync(fd) → _commit(fd) — flushes CRT file buffers to disk. */
#define fsync(fd) _commit(fd)

/* POSIX case-insensitive string compares. */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* rename() on Windows fails if the destination exists; real QTSQ usage
 * (keystore tmp-file replace) needs POSIX atomic-replace semantics, so
 * route through MoveFileExA(MOVEFILE_REPLACE_EXISTING) in compat.c. */
int qtsq_win_rename(const char *oldpath, const char *newpath);
#define rename qtsq_win_rename

/* sysconf(_SC_NPROCESSORS_ONLN) — only consumer is qtsq_gravitational.c. */
#define _SC_NPROCESSORS_ONLN 1
long qtsq_win_sysconf(int name);
#define sysconf(name) qtsq_win_sysconf(name)

/* ---- time surface ------------------------------------------------------ */

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#if !defined(_TIMESPEC_DEFINED) && !defined(__struct_timespec_defined)
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#define _TIMESPEC_DEFINED
#endif

int qtsq_win_clock_gettime(int clock_id, struct timespec *tp);
#define clock_gettime(clock_id, tp) qtsq_win_clock_gettime((clock_id), (tp))

#if !defined(_TIMEVAL_DEFINED) && !defined(_STRUCT_TIMEVAL)
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;   /* seconds */
    long tv_usec;  /* microseconds */
};
#endif
struct timezone;
int qtsq_win_gettimeofday(struct timeval *tv, struct timezone *tz);
#define gettimeofday(tv, tz) qtsq_win_gettimeofday((tv), (tz))

/* gmtime_r(timer, result) — CRT gmtime_s takes the args in the opposite
 * order; macro reorders. */
#ifndef gmtime_r
#define gmtime_r(timer, result) gmtime_s((result), (timer))
#endif

/* sincosf: MSVC's UCRT has no such helper (MinGW provides its own, hence
 * the _MSC_VER-only guard). 6 compressor members use it. */
#if defined(_MSC_VER) && !defined(__clang__)
static inline void sincosf(float x, float *sinp, float *cosp)
{
    *sinp = sinf(x);
    *cosp = cosf(x);
}
#endif

#endif /* _WIN32 */
#endif /* QTSQ_WIN_COMPAT_H */
