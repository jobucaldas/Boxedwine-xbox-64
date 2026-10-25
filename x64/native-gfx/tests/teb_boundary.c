/* Off-device boundary experiment: Windows APIs are Wine stand-ins here. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "uwph/uwph.h"
#define FIELD(t, o) (*(volatile uint64_t *)((char *)(t) + (o)))
#define TEB_ERROR(t) (*(volatile uint32_t *)((char *)(t) + UWPH_TEB_LASTERROR))
static const uint64_t fake_peb = 0x12345000, fake_tls = 0x23456000;
struct test { void *teb; struct uwph_host_teb_state host; DWORD slot; };

/* This is the safe negative observation, not an API that dereferences PEB. */
static __attribute__((noinline)) uintptr_t raw_peb(void)
{
    uintptr_t value;
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r" (value) : : "memory");
    return value;
}
static uintptr_t host_api(void *arg)
{
    struct test *t = arg;
    int ok = raw_peb() == t->host.peb &&
        FIELD(t->teb, UWPH_TEB_TLSPOINTER) == t->host.tls_pointer &&
        GetLastError() == t->host.last_error;
    for (int i = 0; i < 64; ++i)
        ok &= FIELD(t->teb, 0x1480 + i * 8) == t->host.tls_slots[i];
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32);
    ok &= p != NULL;
    if (p) ok &= HeapFree(GetProcessHeap(), 0, p) != 0;
    ok &= TlsGetValue(t->slot) == (void *)(uintptr_t)0x34567000;
    SetLastError(0x4567);
    ok &= GetLastError() == 0x4567;
    return ok;
}
int main(void)
{
    struct test t = {0};
    struct uwph_teb_swap swap = {0};
    t.slot = TlsAlloc();
    if (t.slot >= 64 || !TlsSetValue(t.slot, (void *)(uintptr_t)0x34567000)) {
        puts("TEB BOUNDARY FAIL: inline TLS slot unavailable"); return 1;
    }
    t.teb = uwph_teb_current();
    t.host.peb = FIELD(t.teb, UWPH_TEB_PEB);
    t.host.tls_pointer = FIELD(t.teb, UWPH_TEB_TLSPOINTER);
    for (int i = 0; i < 64; ++i) t.host.tls_slots[i] = FIELD(t.teb, 0x1480 + i * 8);
    SetLastError(0x1234);
    t.host.last_error = TEB_ERROR(t.teb);
    uwph_teb_capture_guest(t.teb, &swap);
    swap.guest[3] = fake_tls;
    swap.guest[4] = fake_peb;
    uwph_teb_enter_guest(t.teb, &swap);
    for (int i = 0; i < 64; ++i) FIELD(t.teb, 0x1480 + i * 8) = 0x50000000u + i;
    TEB_ERROR(t.teb) = 0x9876;
    uintptr_t negative = raw_peb();
    int ok = negative == fake_peb;
    ok &= uwph_host_call(t.teb, &t.host, host_api, &t) == 1;
    ok &= FIELD(t.teb, UWPH_TEB_PEB) == fake_peb;
    ok &= FIELD(t.teb, UWPH_TEB_TLSPOINTER) == fake_tls;
    ok &= TEB_ERROR(t.teb) == 0x9876;
    for (int i = 0; i < 64; ++i) {
        ok &= FIELD(t.teb, 0x1480 + i * 8) == 0x50000000u + i;
        FIELD(t.teb, 0x1480 + i * 8) = t.host.tls_slots[i];
    }
    uwph_teb_leave_guest(t.teb, &swap);
    TEB_ERROR(t.teb) = t.host.last_error;
    TlsFree(t.slot);
    printf("NEGATIVE raw PEB=0x%llx (fake pointer observed, never dereferenced)\n", (unsigned long long)negative);
    if (ok) puts("HOST uwph callback: heap, TLS, LastError PASS; guest state restored");
    puts(ok ? "TEB BOUNDARY PASS" : "TEB BOUNDARY FAIL");
    return !ok;
}
