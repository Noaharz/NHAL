/*
 * Dünne Thread-Schicht für Backends (intern): Mutex, Bedingungsvariable,
 * Thread, Anzahl Kerne. POSIX (pthreads) und Windows.
 *
 * NHAL_THREADS_INLINE ist gesetzt, wenn es keine Threads gibt (Web ohne
 * pthreads). Features laufen dann im aufrufenden Thread und melden das als
 * NHAL_SUPPORT_EMULATED.
 */
#ifndef NHAL_COMMON_THREADS_H
#define NHAL_COMMON_THREADS_H

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define NHAL_THREADS_INLINE 1
#endif

#if defined(NHAL_THREADS_INLINE)

typedef int nhal_mutex;
typedef int nhal_cond;
static inline void nhal_mutex_init(nhal_mutex* m) { (void)m; }
static inline void nhal_mutex_destroy(nhal_mutex* m) { (void)m; }
static inline void nhal_mutex_lock(nhal_mutex* m) { (void)m; }
static inline void nhal_mutex_unlock(nhal_mutex* m) { (void)m; }
static inline void nhal_cond_init(nhal_cond* c) { (void)c; }
static inline void nhal_cond_destroy(nhal_cond* c) { (void)c; }
static inline void nhal_cond_signal(nhal_cond* c) { (void)c; }
static inline void nhal_cond_broadcast(nhal_cond* c) { (void)c; }
static inline unsigned nhal_cpu_count(void) { return 1; }

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef CRITICAL_SECTION nhal_mutex;
typedef CONDITION_VARIABLE nhal_cond;
typedef HANDLE nhal_thread;
#define NHAL_THREAD_FN(name, arg) static DWORD WINAPI name(LPVOID arg)
#define NHAL_THREAD_RETURN return 0

static inline void nhal_mutex_init(nhal_mutex* m) { InitializeCriticalSection(m); }
static inline void nhal_mutex_destroy(nhal_mutex* m) { DeleteCriticalSection(m); }
static inline void nhal_mutex_lock(nhal_mutex* m) { EnterCriticalSection(m); }
static inline void nhal_mutex_unlock(nhal_mutex* m) { LeaveCriticalSection(m); }
static inline void nhal_cond_init(nhal_cond* c) { InitializeConditionVariable(c); }
static inline void nhal_cond_destroy(nhal_cond* c) { (void)c; }
static inline void nhal_cond_wait(nhal_cond* c, nhal_mutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void nhal_cond_signal(nhal_cond* c) { WakeConditionVariable(c); }
static inline void nhal_cond_broadcast(nhal_cond* c) { WakeAllConditionVariable(c); }
static inline int nhal_thread_start(nhal_thread* t, LPTHREAD_START_ROUTINE fn, void* arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}
static inline void nhal_thread_join(nhal_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static inline unsigned nhal_cpu_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? (unsigned)info.dwNumberOfProcessors : 1u;
}

#else

#include <pthread.h>
#include <unistd.h>

typedef pthread_mutex_t nhal_mutex;
typedef pthread_cond_t nhal_cond;
typedef pthread_t nhal_thread;
#define NHAL_THREAD_FN(name, arg) static void* name(void* arg)
#define NHAL_THREAD_RETURN return NULL

static inline void nhal_mutex_init(nhal_mutex* m) { pthread_mutex_init(m, NULL); }
static inline void nhal_mutex_destroy(nhal_mutex* m) { pthread_mutex_destroy(m); }
static inline void nhal_mutex_lock(nhal_mutex* m) { pthread_mutex_lock(m); }
static inline void nhal_mutex_unlock(nhal_mutex* m) { pthread_mutex_unlock(m); }
static inline void nhal_cond_init(nhal_cond* c) { pthread_cond_init(c, NULL); }
static inline void nhal_cond_destroy(nhal_cond* c) { pthread_cond_destroy(c); }
static inline void nhal_cond_wait(nhal_cond* c, nhal_mutex* m) { pthread_cond_wait(c, m); }
static inline void nhal_cond_signal(nhal_cond* c) { pthread_cond_signal(c); }
static inline void nhal_cond_broadcast(nhal_cond* c) { pthread_cond_broadcast(c); }
static inline int nhal_thread_start(nhal_thread* t, void* (*fn)(void*), void* arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void nhal_thread_join(nhal_thread t) { pthread_join(t, NULL); }
static inline unsigned nhal_cpu_count(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

#endif

#endif /* NHAL_COMMON_THREADS_H */
