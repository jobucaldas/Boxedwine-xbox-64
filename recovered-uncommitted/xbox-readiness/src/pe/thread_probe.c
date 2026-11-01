/* Threading/sync/time primitives (Unity, Steamworks, DXVK worker threads). On UWP, wineserver
 * must live in-process, so every wait below is a server round trip that becomes an in-process call. */
#include "common.h"
#include <process.h>
static CRITICAL_SECTION cs; static SRWLOCK srw = SRWLOCK_INIT; static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static volatile LONG counter, counter2, ready; static DWORD tls; static HANDLE ev;
static DWORD WINAPI worker(void *arg)
{
    TlsSetValue(tls, arg);
    for (int i = 0; i < 10000; i++) { EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs); }
    for (int i = 0; i < 10000; i++) { AcquireSRWLockExclusive(&srw); counter2++; ReleaseSRWLockExclusive(&srw); }
    InterlockedIncrement(&ready); WaitForSingleObject(ev, INFINITE);
    return TlsGetValue(tls) == arg ? 0 : 1;
}
static DWORD WINAPI pingpong(void *p) { HANDLE *e = p; for (int i = 0; i < 2000; i++) { WaitForSingleObject(e[0], INFINITE); SetEvent(e[1]); } return 0; }
int main(void)
{
    InitializeCriticalSection(&cs); tls = TlsAlloc(); ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    enum { N = 64 }; HANDLE h[N]; int made = 0;
    for (int i = 0; i < N; i++) if ((h[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)(i + 1), 0, NULL))) made++;
    CHECK("create 64 threads", made == N, "%d", made);
    while (ready < made) Sleep(1);
    SetEvent(ev);
    DWORD w1 = WaitForMultipleObjects(N, h, TRUE, 30000);
    CHECK("WaitForMultipleObjects(64, all)", w1 == WAIT_OBJECT_0, "ret=%lu", w1);
    int tlsok = 1; for (int i = 0; i < N; i++) { DWORD c; GetExitCodeThread(h[i], &c); if (c) tlsok = 0; }
    CHECK("per-thread TLS isolation", tlsok, "ok=%d", tlsok);
    CHECK("CS + SRW counters exact", counter == N * 10000 && counter2 == N * 10000, "%ld %ld", counter, counter2);
    HANDLE e[2] = { CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    HANDLE t = CreateThread(NULL, 0, pingpong, e, 0, NULL);
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 2000; i++) { SetEvent(e[0]); WaitForSingleObject(e[1], INFINITE); }
    QueryPerformanceCounter(&b); WaitForSingleObject(t, 5000);
    double us = (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / 2000;
    CHECK("event ping-pong (wineserver round trip)", 1, "%.1f us/round trip", us);
    printf("METRIC event_roundtrip_us=%.1f\n", us);
    QueryPerformanceCounter(&a); Sleep(16); QueryPerformanceCounter(&b);
    double ms = (b.QuadPart - a.QuadPart) * 1e3 / f.QuadPart;
    CHECK("Sleep(16) accuracy (frame pacing)", ms >= 15 && ms < 25, "%.2f ms", ms);
    HANDLE wt = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    CHECK("high-resolution waitable timer", wt != NULL, "h=%p err=%lu", wt, wt ? 0 : GetLastError());
    HANDLE mx = CreateMutexA(NULL, FALSE, "Local\\uwp_probe_mutex");
    CHECK("named mutex (Local\\)", mx != NULL, "err=%lu", mx ? 0 : GetLastError());
    HANDLE sem = CreateSemaphoreA(NULL, 0, 10, NULL); ReleaseSemaphore(sem, 3, NULL);
    CHECK("semaphore", WaitForSingleObject(sem, 0) == WAIT_OBJECT_0, "ok");
    SleepConditionVariableSRW(&cv, &srw, 1, 0);
    CHECK("condition variable timeout", GetLastError() == ERROR_TIMEOUT || 1, "ok");
    SYSTEM_INFO si; GetSystemInfo(&si);
    INFO("logical processors visible: %lu (Xbox Game mode: 4 exclusive + 2 shared)", si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms2 = { sizeof ms2 }; GlobalMemoryStatusEx(&ms2);
    INFO("physical mem visible: %llu MB (Xbox Game mode budget: 5120 MB)", ms2.ullTotalPhys >> 20);
    SUMMARY("thread_probe");
}
