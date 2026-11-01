/* Standard mingw-w64 CRT program: printf, malloc, argv, TEB access, threads, TLS, SEH-free C. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread int tlsCounter = 5;
static volatile LONG shared;

static DWORD WINAPI worker(LPVOID arg) {
    tlsCounter += (int)(INT_PTR)arg;
    for (int i = 0; i < 100000; i++) InterlockedIncrement(&shared);
    return (DWORD)tlsCounter;
}

int main(int argc, char **argv) {
    printf("hello.exe: native x86-64 Windows PE, argc=%d argv[0]=%s\n", argc, argv[0]);
    void *teb = NtCurrentTeb();
    printf("TEB self-check: %s, sizeof(void*)=%d\n", *(void **)((char *)teb + 0x30) == teb ? "ok" : "BAD", (int)sizeof(void *));
    char *p = malloc(1000);
    strcpy(p, "heap");
    printf("malloc: %s, 64-bit math %llx, double %.3f\n", p, 0x123456789ULL * 0x1000ULL, 3.14159 * 2);
    free(p);
    HANDLE t[2];
    for (int i = 0; i < 2; i++) t[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(2, t, TRUE, INFINITE);
    DWORD c0, c1;
    GetExitCodeThread(t[0], &c0);
    GetExitCodeThread(t[1], &c1);
    printf("threads: shared=%ld exit codes=%lu,%lu main tls=%d\n", (long)shared, c0, c1, tlsCounter);
    return 0;
}
