/* pthread.h — QTSQ Windows shim (header-only, Win32-backed).
 *
 * Shadows the system <pthread.h> for Windows builds because the shim include
 * directory sits first on the include path, and include/qtsq_ionic.h includes
 * <pthread.h> unconditionally (line 26) — with pthread_t / pthread_mutex_t /
 * pthread_cond_t embedded BY VALUE in the public qtsq_ionic_t struct, so the
 * types must exist on Windows.
 *
 * Implemented with Win32 primitives:
 *   pthread_t      = HANDLE (CreateThread)
 *   pthread_mutex_t = CRITICAL_SECTION
 *   pthread_cond_t  = CONDITION_VARIABLE (InitializeConditionVariable et al.)
 *
 * On x64 there is exactly one calling convention, so a
 * `void *(*)(void *)` start routine is safely cast to LPTHREAD_START_ROUTINE
 * for CreateThread.  All QTSQ call sites pass attr=NULL to create and NULL
 * retval to join (census verified), so only those shapes are supported.
 *
 * Only include <windows.h> when the QTSQ headers themselves haven't already
 * (argon2/randombytes include winbase.h/wincrypt.h guarded by the same
 * WIN32_LEAN_AND_MEAN/NOMINMAX defines — no conflict either way).
 */
#ifndef QTSQ_SHIM_PTHREAD_H
#define QTSQ_SHIM_PTHREAD_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void pthread_attr_t;
typedef int pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} qtsq_pthread_stub_t;

/* CreateThread wants DWORD WINAPI fn(LPVOID); x64 has one calling
 * convention, so a plain cast of the POSIX-shaped routine is safe. */
static inline DWORD WINAPI qtsq_pthread_trampoline(LPVOID param)
{
    qtsq_pthread_stub_t stub = *(qtsq_pthread_stub_t *)param;
    free(param);
    (void)stub.start_routine(stub.arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg)
{
    qtsq_pthread_stub_t *stub;
    HANDLE h;
    (void)attr;
    if (!thread || !start_routine)
        return 1;
    stub = (qtsq_pthread_stub_t *)malloc(sizeof(*stub));
    if (!stub)
        return 1;
    stub->start_routine = start_routine;
    stub->arg = arg;
    h = CreateThread(NULL, 0, qtsq_pthread_trampoline, stub, 0, NULL);
    if (!h) {
        free(stub);
        return 1;
    }
    *thread = h;
    return 0;
}

/* All callers pass retval==NULL. */
static inline int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    return 0;
}

static inline void pthread_exit(void *retval)
{
    (void)retval;
    ExitThread(0);
}

/* ---- mutex ---- */

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    InitializeCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    DeleteCriticalSection(m);
    (void)m;
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    EnterCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    LeaveCriticalSection(m);
    return 0;
}

/* ---- condition variables (untimed waits only — census verified) ---- */

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c; /* CONDITION_VARIABLE needs no teardown */
    return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    /* Returns nonzero when signaled; callers re-check predicates, so any
     * return value is treated as "recheck" — map both to POSIX success. */
    SleepConditionVariableCS(c, m, INFINITE);
    return 0;
}
static inline int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(c);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* QTSQ_SHIM_PTHREAD_H */
