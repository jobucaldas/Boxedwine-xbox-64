/* Missing imports exercise Wine's loader-generated stubs without calling them. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(dllimport) void UWPHMissingOne(void);
__declspec(dllimport) void UWPHMissingTwo(void);
int main(void)
{
    MEMORY_BASIC_INFORMATION a, b;
    void *one = (void *)(uintptr_t)UWPHMissingOne;
    void *two = (void *)(uintptr_t)UWPHMissingTwo;
    if (!VirtualQuery(one, &a, sizeof(a)) || !VirtualQuery(two, &b, sizeof(b))) return 1;
    printf("stub_probe: first=%p protect=%lx second=%p protect=%lx\n",
           one, (unsigned long)a.Protect, two, (unsigned long)b.Protect);
    if (a.Protect != PAGE_EXECUTE_READ || b.Protect != PAGE_EXECUTE_READ ||
        a.AllocationBase == b.AllocationBase) return 2;
    puts("PASS: two missing-import stubs published on separate RX allocations");
    return 0;
}
