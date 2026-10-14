/* qtsq_win_compat.c — implementations for the QTSQ Windows compatibility
 * layer (see qtsq_win_compat.h for the macro surface).
 *
 * Compiled into the qtsq static library by windows/CMakeLists.txt.
 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "qtsq_win_compat.h"
#include "dirent.h"

/* ---- rename: POSIX atomic-replace semantics --------------------------- */

int qtsq_win_rename(const char *oldpath, const char *newpath)
{
    /* MOVEFILE_REPLACE_EXISTING gives the POSIX guarantee that the call
     * succeeds even when newpath already exists (keystore tmp-file swap). */
    if (!MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
        return -1;
    return 0;
}

/* ---- sysconf ----------------------------------------------------------- */

long qtsq_win_sysconf(int name)
{
    SYSTEM_INFO si;
    if (name == _SC_NPROCESSORS_ONLN) {
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return -1;
}

/* ---- clock_gettime ------------------------------------------------------ */

int qtsq_win_clock_gettime(int clock_id, struct timespec *tp)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    if (!tp)
        return -1;

    if (clock_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, counter;
        if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter))
            return -1;
        /* Convert to (sec, nsec) without overflowing 64-bit intermediate:
         * seconds first, then the remainder scaled to ns. */
        tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
        {
            double frac = (double)(counter.QuadPart % freq.QuadPart) /
                          (double)freq.QuadPart;
            tp->tv_nsec = (long)(frac * 1000000000.0);
        }
        return 0;
    }

    /* CLOCK_REALTIME: FILETIME is 100ns ticks since 1601-01-01. */
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL; /* 1601 → 1970 epoch, in 100ns */
    tp->tv_sec = (time_t)(u.QuadPart / 10000000ULL);
    tp->tv_nsec = (long)((u.QuadPart % 10000000ULL) * 100ULL);
    return 0;
}

/* ---- gettimeofday -------------------------------------------------------- */

int qtsq_win_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    if (tz) {
        /* QTSQ callers pass NULL; provide a zeroed struct so a stray
         * non-NULL call doesn't read garbage. */
        memset(tz, 0, sizeof(*tz));
    }
    if (!tv)
        return 0;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL;
    tv->tv_sec = (long)(u.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((u.QuadPart % 10000000ULL) / 10ULL);
    return 0;
}

/* ---- dirent (FindFirstFileA/FindNextFileA) ----------------------------- */

struct qtsq_win_DIR {
    HANDLE find;
    WIN32_FIND_DATAA data;
    struct dirent cur;
    int first;
};

DIR *opendir(const char *name)
{
    DIR *dir;
    char pattern[MAX_PATH];
    size_t len;

    if (!name)
        return NULL;
    len = strlen(name);
    if (len + 3 >= MAX_PATH)
        return NULL;
    memcpy(pattern, name, len);
    /* Strip trailing separators before appending "\\*" — FindFirstFileA
     * rejects a pattern ending in a lone slash. */
    while (len > 0 && (pattern[len - 1] == '\\' || pattern[len - 1] == '/'))
        len--;
    pattern[len] = '\0';
    memcpy(pattern + len, "\\*", 3);

    dir = (DIR *)calloc(1, sizeof(*dir));
    if (!dir)
        return NULL;
    dir->find = FindFirstFileA(pattern, &dir->data);
    if (dir->find == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (!dir || dir->find == INVALID_HANDLE_VALUE)
        return NULL;

    for (;;) {
        if (!dir->first) {
            if (!FindNextFileA(dir->find, &dir->data)) {
                /* GetLastError()==ERROR_NO_MORE_FILES means clean EOF. */
                return NULL;
            }
        }
        dir->first = 0;
        /* POSIX readdir returns dot entries; qtsq_container.c skips them.
         * Keep the exact POSIX shape (skip nothing here) so any consumer
         * ported later behaves identically. */
        strncpy(dir->cur.d_name, dir->data.cFileName,
                sizeof(dir->cur.d_name) - 1);
        dir->cur.d_name[sizeof(dir->cur.d_name) - 1] = '\0';
        return &dir->cur;
    }
}

int closedir(DIR *dir)
{
    if (!dir)
        return -1;
    if (dir->find != INVALID_HANDLE_VALUE)
        FindClose(dir->find);
    free(dir);
    return 0;
}

#endif /* _WIN32 */
