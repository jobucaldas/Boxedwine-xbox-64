/* Desktop diagnostic only. Launched THROUGH run_via_wine + Wine's own loader;
 * never console acceptance evidence. Fault wrappers affect only this test PE. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int thread_calls, event_calls, fail_thread, fail_event;
HANDLE WINAPI probe_CreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T n, LPTHREAD_START_ROUTINE f, LPVOID p, DWORD flags, LPDWORD id)
{
    if (++thread_calls == fail_thread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateThread(a, n, f, p, flags, id);
}
HANDLE WINAPI probe_CreateEventExW(LPSECURITY_ATTRIBUTES a, LPCWSTR name, DWORD flags, DWORD access)
{
    if (++event_calls == fail_event) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateEventExW(a, name, flags, access);
}
/* Header dllimport declarations in the instrumented object use IAT symbols. */
HANDLE (WINAPI *__imp_probe_CreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) = probe_CreateThread;
HANDLE (WINAPI *__imp_probe_CreateEventExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) = probe_CreateEventExW;
int readiness_thread_main(void);
int readiness_seh_main(void);
int readiness_fs_main(const WCHAR *);
int readiness_net_main(void);
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int rc;
    if (!strcmp(argv[1], "thread")) {
        fail_thread = argc > 2 ? atoi(argv[2]) : 0;
        fail_event = argc > 3 ? atoi(argv[3]) : 0;
        rc = readiness_thread_main();
        printf("DESKTOP diagnostic thread exit=%d injected_thread=%d injected_event=%d\n", rc, fail_thread, fail_event);
        return (fail_thread || fail_event) ? (rc == 1 ? 0 : 1) : rc;
    }
    if (!strcmp(argv[1], "seh")) rc = readiness_seh_main();
    else if (!strcmp(argv[1], "fs")) { CreateDirectoryW(L"B02-check-state", NULL); rc = readiness_fs_main(L"B02-check-state"); }
    else if (!strcmp(argv[1], "net")) rc = readiness_net_main();
    else return 2;
    printf("DESKTOP diagnostic %s exit=%d; no Xbox PASS\n", argv[1], rc);
    return rc;
}
