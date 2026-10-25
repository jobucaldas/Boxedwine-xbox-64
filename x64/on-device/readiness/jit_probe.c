/* JIT patterns games/engines use, checked against UWP codeGeneration rules:
 * UWP allows RW -> RX flips (VirtualProtectFromApp PAGE_EXECUTE_READ) but rejects PAGE_EXECUTE_READWRITE. */
#include "common.h"
typedef int (*fn)(void);
static unsigned char code42[] = { 0xB8, 42, 0, 0, 0, 0xC3 };     /* mov eax,42; ret */
int main(void)
{
    DWORD old;
    unsigned char *p = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK("alloc RW", p != NULL, "%p", p);
    memcpy(p, code42, sizeof code42);
    BOOL ok = VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4096);
    CHECK("W^X flip RW->RX (UWP-legal)", ok, "err=%lu", ok ? 0 : GetLastError());
    if (ok) CHECK("execute JIT code", ((fn)p)() == 42, "returned %d", ((fn)p)());
    ok = VirtualProtect(p, 4096, PAGE_READWRITE, &old);
    CHECK("flip back RX->RW to patch", ok, "err=%lu", ok ? 0 : GetLastError());
    if (ok) { p[1] = 43; ok = VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old); FlushInstructionCache(GetCurrentProcess(), p, 4096); }
    CHECK("re-execute patched code", ok && ((fn)p)() == 43, "ok=%d", ok);
    /* 1000 flip cycles: cost of W^X discipline */
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    int cyc = 0;
    for (int i = 0; i < 1000; i++) {
        if (!VirtualProtect(p + 4096, 4096, PAGE_READWRITE, &old)) break;
        p[4096] = 0xC3;
        if (!VirtualProtect(p + 4096, 4096, PAGE_EXECUTE_READ, &old)) break;
        cyc++;
    }
    QueryPerformanceCounter(&b);
    CHECK("1000 RW<->RX flip cycles", cyc == 1000, "%d done, %.1f us/cycle", cyc, (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / (cyc ? cyc : 1));
    /* RWX: disallowed on UWP. Under --wx policy we EXPECT failure; on stock Linux Wine it succeeds. */
    void *x = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    INFO("RWX VirtualAlloc %s (UWP would reject this)", x ? "SUCCEEDED" : "failed");
    printf("RWX_ALLOC=%d\n", x != NULL);
    ok = VirtualProtect(p, 4096, PAGE_EXECUTE_READWRITE, &old);
    printf("RWX_PROTECT=%d\n", ok);
    /* Dual mapping (another documented JIT strategy): file mapping with an RW view + RX view */
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 65536, NULL);
    void *w = m ? MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    void *r = m ? MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 4096) : NULL;
    if (w) memcpy(w, code42, sizeof code42);
    CHECK("dual-mapping RW view + RX view", w && r && ((fn)r)() == 42, "m=%p w=%p r=%p", m, w, r);
    SUMMARY("jit_probe");
}
