/* clocks and sleeps */
#include "internal.h"

#ifdef UWPH_BACKEND_WIN32
static int64_t qpc_freq;

UWPH_API int uwph_clock_gettime(uwph_clockid_t id, struct uwph_timespec *ts)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_clock_gettime(id, ts);
#else
    switch (id) {
    case UWPH_CLOCK_REALTIME:
    case UWPH_CLOCK_REALTIME_COARSE: {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimePreciseAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= 116444736000000000ULL;      /* 1601 -> 1970, 100ns units */
        ts->tv_sec = (int64_t)(u.QuadPart / 10000000);
        ts->tv_nsec = (int64_t)(u.QuadPart % 10000000) * 100;
        return 0;
    }
    case UWPH_CLOCK_MONOTONIC:
    case UWPH_CLOCK_MONOTONIC_RAW:
    case UWPH_CLOCK_MONOTONIC_COARSE:
    case UWPH_CLOCK_BOOTTIME: {
        LARGE_INTEGER c;
        if (!qpc_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpc_freq = f.QuadPart; }
        QueryPerformanceCounter(&c);
        ts->tv_sec = c.QuadPart / qpc_freq;
        ts->tv_nsec = (c.QuadPart % qpc_freq) * 1000000000 / qpc_freq;
        return 0;
    }
    default:
        return UWPH_FAIL(UWPH_EINVAL);   /* CPU-time clocks: GetThreadTimes, next step */
    }
#endif
}

UWPH_API int uwph_clock_getres(uwph_clockid_t id, struct uwph_timespec *ts)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_clock_getres(id, ts);
#else
    if (id != UWPH_CLOCK_REALTIME && id != UWPH_CLOCK_MONOTONIC && id != UWPH_CLOCK_MONOTONIC_RAW &&
        id != UWPH_CLOCK_BOOTTIME && id != UWPH_CLOCK_REALTIME_COARSE && id != UWPH_CLOCK_MONOTONIC_COARSE)
        return UWPH_FAIL(UWPH_EINVAL);
    ts->tv_sec = 0;
    ts->tv_nsec = 100;
    return 0;
#endif
}

UWPH_API int uwph_nanosleep(const struct uwph_timespec *req, struct uwph_timespec *rem)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_nanosleep(req, rem);
#else
    struct uwph_thread *t = uwph_thread_current();
    uint64_t pend = 0, total = (uint64_t)req->tv_sec * 1000 + (uint64_t)req->tv_nsec / 1000000;
    uint64_t start = GetTickCount64(), el;
    if (!total && req->tv_nsec) { SwitchToThread(); return 0; }
    /* sleep on our pending-signal word so tgkill interrupts it */
    while ((el = GetTickCount64() - start) < total) {
        if (t->pending) {
            if (rem) { uint64_t left = total - el; rem->tv_sec = (int64_t)(left / 1000); rem->tv_nsec = (int64_t)(left % 1000) * 1000000; }
            return UWPH_FAIL(UWPH_EINTR);
        }
        WaitOnAddress((volatile void *)&t->pending, &pend, sizeof(pend), (DWORD)(total - el));
    }
    return 0;
#endif
}
#else
UWPH_API int uwph_clock_gettime(uwph_clockid_t id, struct uwph_timespec *ts) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_clock_gettime(id, ts);
#else
    (void)id; (void)ts; return UWPH_ENOSYS_INT();
#endif
}
UWPH_API int uwph_clock_getres(uwph_clockid_t id, struct uwph_timespec *ts) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_clock_getres(id, ts);
#else
    (void)id; (void)ts; return UWPH_ENOSYS_INT();
#endif
}
UWPH_API int uwph_nanosleep(const struct uwph_timespec *req, struct uwph_timespec *rem) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_nanosleep(req, rem);
#else
    (void)req; (void)rem; return UWPH_ENOSYS_INT();
#endif
}
#endif

UWPH_API int uwph_gettimeofday(struct uwph_timeval *tv, void *tz)
{
    struct uwph_timespec ts;
    (void)tz;
    if (uwph_clock_gettime(UWPH_CLOCK_REALTIME, &ts) < 0) return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

UWPH_API uwph_time_t uwph_time(uwph_time_t *t)
{
    struct uwph_timespec ts;
    if (uwph_clock_gettime(UWPH_CLOCK_REALTIME, &ts) < 0) return -1;
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}

UWPH_API int uwph_usleep(uint32_t usec)
{
    struct uwph_timespec ts = { usec / 1000000, (int64_t)(usec % 1000000) * 1000 };
    return uwph_nanosleep(&ts, NULL);
}
