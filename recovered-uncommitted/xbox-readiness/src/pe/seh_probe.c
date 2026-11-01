/* Structured exceptions games/runtimes rely on. On Linux Wine these arrive as POSIX signals
 * (SIGSEGV/SIGFPE/SIGILL/SIGTRAP); on a UWP host they would arrive as native Windows exceptions. */
#include "common.h"
static volatile LONG hits; static DWORD last_code;
static LONG CALLBACK veh(EXCEPTION_POINTERS *e)
{
    last_code = e->ExceptionRecord->ExceptionCode; InterlockedIncrement(&hits);
    if (last_code == EXCEPTION_ACCESS_VIOLATION || last_code == EXCEPTION_INT_DIVIDE_BY_ZERO || last_code == EXCEPTION_ILLEGAL_INSTRUCTION || last_code == EXCEPTION_PRIV_INSTRUCTION) {
        e->ContextRecord->Rip += 2;   /* every probe fault below is a 2-byte instruction */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (last_code == EXCEPTION_BREAKPOINT) { e->ContextRecord->Rip += 1; return EXCEPTION_CONTINUE_EXECUTION; }
    if (last_code == EXCEPTION_GUARD_PAGE || last_code == 0xE0000001) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI thr(void *p) { for (int i = 0; i < 100; i++) __asm__ volatile("xor %%eax,%%eax; movl (%%rax), %%eax" ::: "eax", "rax"); return 0; }
int main(void)
{
    AddVectoredExceptionHandler(1, veh);
    hits = 0; __asm__ volatile("xor %%eax,%%eax; movl (%%rax), %%eax" ::: "eax", "rax");  /* 8b 00 = 2 bytes; handler adds 3? */
    CHECK("access violation -> VEH", hits == 1 && last_code == EXCEPTION_ACCESS_VIOLATION, "hits=%ld code=%08lx", hits, last_code);
    hits = 0; __asm__ volatile("xor %%ecx,%%ecx; mov $1,%%eax; cltd; idivl %%ecx" ::: "eax", "ecx", "edx");
    CHECK("int divide by zero -> VEH", hits == 1 && last_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "hits=%ld code=%08lx", hits, last_code);
    hits = 0; __asm__ volatile("ud2");
    CHECK("ud2 -> illegal instruction", hits == 1 && last_code == EXCEPTION_ILLEGAL_INSTRUCTION, "hits=%ld code=%08lx", hits, last_code);
    hits = 0; __asm__ volatile("int3");
    CHECK("int3 -> breakpoint", hits == 1 && last_code == EXCEPTION_BREAKPOINT, "hits=%ld code=%08lx", hits, last_code);
    hits = 0; RaiseException(0xE0000001, 0, 0, NULL);
    CHECK("RaiseException software", hits == 1 && last_code == 0xE0000001, "hits=%ld", hits);
    DWORD old; char *g = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VirtualProtect(g, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    hits = 0; volatile char c = g[0]; (void)c;
    CHECK("guard page -> STATUS_GUARD_PAGE once", hits == 1 && last_code == EXCEPTION_GUARD_PAGE, "hits=%ld code=%08lx", hits, last_code);
    /* 10k faults: cost of exception round trip (Mono/IL2CPP null checks, GC write barriers) */
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    hits = 0; for (int i = 0; i < 10000; i++) __asm__ volatile("xor %%eax,%%eax; movl (%%rax), %%eax" ::: "eax", "rax");
    QueryPerformanceCounter(&b);
    CHECK("10000 AV round trips", hits == 10000, "%.2f us/fault", (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / 10000);
    hits = 0; HANDLE h[4]; for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, thr, NULL, 0, NULL);
    WaitForMultipleObjects(4, h, TRUE, 10000);
    CHECK("faults on 4 threads concurrently", hits == 400, "hits=%ld", hits);
    /* deep recursion -> stack overflow is recoverable only with guard pages; just probe stack commit */
    volatile char big[512 * 1024]; big[0] = 1; big[sizeof big - 1] = 2;
    CHECK("512KB stack probe (guard-page growth)", big[0] == 1, "ok");
    SUMMARY("seh_probe");
}
