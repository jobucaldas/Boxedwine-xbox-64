/* Bounded native threading port. Shared state has static lifetime even if a
 * timed-out OS worker has not exited; no stack argument outlives this call. */
#include "common.h"
static CRITICAL_SECTION cs;
static SRWLOCK srw = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static volatile LONG counter, counter2, stop;
static DWORD tls;
static HANDLE ev, ping[2];
static DWORD WINAPI worker(void *arg)
{
    if (!TlsSetValue(tls, arg)) return 1;
    for (int i = 0; i < 10000 && !InterlockedCompareExchange(&stop, 0, 0); i++) {
        EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs);
        AcquireSRWLockExclusive(&srw); counter2++; ReleaseSRWLockExclusive(&srw);
    }
    return WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0 && TlsGetValue(tls) == arg ? 0 : 1;
}
static DWORD WINAPI pingpong(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000 && !InterlockedCompareExchange(&stop, 0, 0); i++) {
        if (WaitForSingleObject(ping[0], 2000) != WAIT_OBJECT_0 || !SetEvent(ping[1])) return 1;
    }
    return 0;
}
int main(void)
{
    enum { N = 64 }; HANDLE threads[N]; int made = 0, live = 0;
    InitializeCriticalSection(&cs);
    tls = TlsAlloc(); ev = CreateEventExW(NULL, NULL, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    CHECK("TLS and release event creation", tls != TLS_OUT_OF_INDEXES && ev, "tls=%lu event=%p err=%lu", tls, ev, GetLastError());
    if (tls == TLS_OUT_OF_INDEXES || !ev) goto cleanup;
    for (int i = 0; i < N; i++) {
        HANDLE h = CreateThread(NULL, 0, worker, (void *)(INT_PTR)(i+1), 0, NULL);
        if (!h) break;
        threads[made++] = h;
    }
    CHECK("create 64 threads", made == N, "made=%d err=%lu", made, GetLastError());
    BOOL released = SetEvent(ev);
    DWORD w = made ? WaitForMultipleObjects(made, threads, TRUE, 10000) : WAIT_FAILED;
    CHECK("bounded worker completion", released && made == N && w == WAIT_OBJECT_0, "wait=%lu", w);
    InterlockedExchange(&stop, 1);
    int tlsok = made == N;
    for (int i = 0; i < made; i++) {
        DWORD code = 1;
        if (WaitForSingleObject(threads[i], 0) != WAIT_OBJECT_0) { live++; tlsok = 0; }
        else if (!GetExitCodeThread(threads[i], &code) || code) tlsok = 0;
        CloseHandle(threads[i]);
    }
    CHECK("per-thread TLS isolation", tlsok, "ok=%d live=%d", tlsok, live);
    CHECK("CS + SRW counters exact", !live && counter == N*10000 && counter2 == N*10000, "%ld %ld", counter, counter2);
    if (live) goto cleanup;
    InterlockedExchange(&stop, 0);
    ping[0] = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    ping[1] = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    HANDLE t = ping[0] && ping[1] ? CreateThread(NULL, 0, pingpong, NULL, 0, NULL) : NULL;
    CHECK("ping-pong event/thread creation", t != NULL, "thread=%p err=%lu", t, GetLastError());
    int rounds = 0;
    ULONGLONG deadline = GetTickCount64() + 10000;
    if (t) {
        for (; rounds < 2000 && GetTickCount64() < deadline; rounds++)
            if (!SetEvent(ping[0]) || WaitForSingleObject(ping[1], 2000) != WAIT_OBJECT_0) break;
        InterlockedExchange(&stop, 1); SetEvent(ping[0]);
        w = WaitForSingleObject(t, 2500);
        DWORD code = 1;
        int ok = w == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 0;
        CHECK("bounded event ping-pong", rounds == 2000 && ok, "rounds=%d wait=%lu exit=%lu", rounds, w, code);
        if (w != WAIT_OBJECT_0) live++;
        CloseHandle(t);
    }
    LARGE_INTEGER freq, before, after;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&before);
    Sleep(16); QueryPerformanceCounter(&after);
    double ms = (after.QuadPart - before.QuadPart) * 1e3 / freq.QuadPart;
    CHECK("Sleep(16) frame pacing", ms >= 15 && ms < 25, "ms=%.2f", ms);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    CHECK("high-resolution waitable timer", timer != NULL, "err=%lu", GetLastError());
    if (timer) CloseHandle(timer);
    HANDLE mutex = CreateMutexExW(NULL, L"Local\\uwp_probe_mutex", 0, MUTEX_ALL_ACCESS);
    CHECK("named Local mutex", mutex != NULL, "err=%lu", GetLastError());
    if (mutex) CloseHandle(mutex);
    AcquireSRWLockExclusive(&srw);
    BOOL cvok = SleepConditionVariableSRW(&cv, &srw, 1, 0);
    DWORD error = GetLastError();
    ReleaseSRWLockExclusive(&srw);
    CHECK("condition variable timeout", !cvok && error == ERROR_TIMEOUT, "err=%lu", error);
    HANDLE sem = CreateSemaphoreExW(NULL, 0, 10, NULL, 0, SEMAPHORE_ALL_ACCESS);
    CHECK("semaphore release/wait", sem && ReleaseSemaphore(sem, 3, NULL) && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0, "handle=%p", sem);
    if (sem) CloseHandle(sem);
cleanup:
    /* A non-responsive worker retains only its own static shared resources.
     * Never terminate a thread or destroy synchronization objects beneath it. */
    if (!live) {
        if (ping[0]) CloseHandle(ping[0]);
        if (ping[1]) CloseHandle(ping[1]);
        if (ev) CloseHandle(ev);
        if (tls != TLS_OUT_OF_INDEXES) TlsFree(tls);
        DeleteCriticalSection(&cs);
    } else INFO("worker timeout: static synchronization resources retained until process exit");
    SUMMARY("thread_probe");
}
