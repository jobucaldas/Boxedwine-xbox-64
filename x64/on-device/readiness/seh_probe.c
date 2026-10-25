/* App-container port: frame-based SEH, no vectored handler or desktop API.
 * MinGW __try1 emits the documented x64 scope table for __C_specific_handler. */
#include "common.h"
#include <excpt.h>
static volatile LONG hits;
static volatile DWORD last_code;
LONG seh_filter(EXCEPTION_POINTERS *e, void *frame)
{
    (void)frame;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_BREAKPOINT || code == 0xE0000001) {
        last_code = code;
        InterlockedIncrement(&hits);
        if (code != 0xE0000001)
            e->ContextRecord->Rip = (DWORD64)e->ExceptionRecord->ExceptionAddress +
                                   (code == EXCEPTION_BREAKPOINT ? 1 : 2);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
/* Keep all switch arms inside the assembler scope labels. */
__attribute__((noinline, optimize("O0"))) static void fault(int kind)
{
    __try1(seh_filter)
    switch (kind) {
    case 0: __asm__ volatile("xor %%eax,%%eax; movl (%%rax),%%eax" ::: "rax", "memory"); break;
    case 1: __asm__ volatile("xor %%ecx,%%ecx; mov $1,%%eax; cltd; idivl %%ecx" ::: "eax", "ecx", "edx"); break;
    case 2: __asm__ volatile("ud2"); break;
    case 3: __asm__ volatile("int3"); break;
    default: RaiseException(0xE0000001, 0, 0, NULL); break;
    }
    __except1
}
int main(void)
{
    const DWORD codes[] = {EXCEPTION_ACCESS_VIOLATION, EXCEPTION_INT_DIVIDE_BY_ZERO,
        EXCEPTION_ILLEGAL_INSTRUCTION, EXCEPTION_BREAKPOINT, 0xE0000001};
    for (int i = 0; i < 5; ++i) {
        hits = 0; last_code = 0; fault(i);
        CHECK("frame SEH exception and resume", hits == 1 && last_code == codes[i],
              "kind=%d hits=%ld code=%08lx", i, hits, last_code);
    }
    hits = 0;
    for (int i = 0; i < 10000; ++i) fault(0);
    CHECK("10000 AV round trips", hits == 10000, "hits=%ld", hits);
    volatile char big[512 * 1024]; big[0] = 1; big[sizeof(big)-1] = 2;
    CHECK("512KB stack growth", big[0] == 1 && big[sizeof(big)-1] == 2, "ok");
    INFO("LIMIT: frame SEH replaces VEH; guard-page and concurrent VEH checks are not covered");
    SUMMARY("seh_probe");
}
