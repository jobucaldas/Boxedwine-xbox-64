/* errno, init, logging and the internal sync primitives. */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static __thread int thread_errno;

UWPH_API int *uwph___errno_location(void) { return &thread_errno; }

UWPH_API const char *uwph_backend_name(void)
{
#ifdef UWPH_BACKEND_WIN32
    return "win32 (UWP APP partition)";
#elif defined(UWPH_BACKEND_LINUX_PASSTHROUGH)
    return "linux-passthrough";
#else
    return "stub (Linux, host calls -> ENOSYS)";
#endif
}

void uwph_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef UWPH_BACKEND_WIN32
    OutputDebugStringA(buf);
#endif
    fputs(buf, stderr);
}

UWPH_API int uwph_init(const char *local_root, const char *install_root)
{
    static int done;
    if (done) return 0;
    done = 1;
    uwph_fd_init();
    uwph_fs_mount("/", local_root ? local_root : ".", 0);
    if (install_root) uwph_fs_mount("/app", install_root, UWPH_MOUNT_RO);
    uwph_probe_run();
    return uwph_signal_init();
}

/* ---- sync ---- */
#ifdef UWPH_BACKEND_WIN32
void uwph_lock(uwph_lock_t *l)   { AcquireSRWLockExclusive(l); }
void uwph_unlock(uwph_lock_t *l) { ReleaseSRWLockExclusive(l); }
int uwph_cond_wait(uwph_cond_t *c, uwph_lock_t *l, int timeout_ms)
{
    if (SleepConditionVariableSRW(c, l, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? UWPH_ETIMEDOUT : 0;
}
void uwph_cond_broadcast(uwph_cond_t *c) { WakeAllConditionVariable(c); }
uint64_t uwph_monotonic_ms(void) { return GetTickCount64(); }

struct uwph_host_thread { HANDLE h; void *(*fn)(void *); void *arg; void *ret; };
static DWORD WINAPI host_thread_entry(LPVOID p)
{
    uwph_host_thread *t = p;
    t->ret = t->fn(t->arg);
    return 0;
}
uwph_host_thread *uwph_host_thread_start(void *(*fn)(void *), void *arg)
{
    uwph_host_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (!(t->h = CreateThread(NULL, 0, host_thread_entry, t, 0, NULL))) { free(t); return NULL; }
    return t;
}
void *uwph_host_thread_join(uwph_host_thread *t)
{
    void *ret;
    WaitForSingleObjectEx(t->h, INFINITE, FALSE);
    CloseHandle(t->h);
    ret = t->ret;
    free(t);
    return ret;
}
#else
void uwph_lock(uwph_lock_t *l)   { pthread_mutex_lock(l); }
void uwph_unlock(uwph_lock_t *l) { pthread_mutex_unlock(l); }
int uwph_cond_wait(uwph_cond_t *c, uwph_lock_t *l, int timeout_ms)
{
    struct timespec ts;
    if (timeout_ms < 0) { pthread_cond_wait(c, l); return 0; }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, l, &ts) ? UWPH_ETIMEDOUT : 0;
}
void uwph_cond_broadcast(uwph_cond_t *c) { pthread_cond_broadcast(c); }
uint64_t uwph_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct uwph_host_thread { pthread_t t; };
uwph_host_thread *uwph_host_thread_start(void *(*fn)(void *), void *arg)
{
    uwph_host_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (pthread_create(&t->t, NULL, fn, arg)) { free(t); return NULL; }
    return t;
}
void *uwph_host_thread_join(uwph_host_thread *t)
{
    void *ret = NULL;
    pthread_join(t->t, &ret);
    free(t);
    return ret;
}
#endif

/* ---- objects ---- */
void uwph_obj_init(struct uwph_object *o, void (*destroy)(struct uwph_object *))
{
    o->refs = 1;
    o->destroy = destroy;
    o->fd_closed = NULL;
}

struct uwph_object *uwph_obj_ref(struct uwph_object *o)
{
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    return o;
}

void uwph_obj_unref(struct uwph_object *o)
{
    if (o && !__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) && o->destroy) o->destroy(o);
}
