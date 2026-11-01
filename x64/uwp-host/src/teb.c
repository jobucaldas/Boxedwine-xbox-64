/* TEB swap set (see teb.h). */
#include "internal.h"

static const int swap_offsets[5] = {
    UWPH_TEB_STACKBASE, UWPH_TEB_STACKLIMIT, UWPH_TEB_SELF, UWPH_TEB_TLSPOINTER, UWPH_TEB_PEB
};

#define TEB_FIELD(teb, off) (*(volatile uint64_t *)((char *)(teb) + (off)))

void uwph_teb_capture_guest(const void *wine_teb, struct uwph_teb_swap *s)
{
    for (int i = 0; i < 5; i++) s->guest[i] = TEB_FIELD(wine_teb, swap_offsets[i]);
    /* Self must name the Wine TEB itself, whatever its own copy says */
    s->guest[2] = (uint64_t)(uintptr_t)wine_teb;
}

void uwph_teb_enter_guest(void *teb, struct uwph_teb_swap *s)
{
    if (!teb || s->in_guest) return;
    for (int i = 0; i < 5; i++) {
        s->host[i] = TEB_FIELD(teb, swap_offsets[i]);
        TEB_FIELD(teb, swap_offsets[i]) = s->guest[i];
    }
    s->in_guest = 1;
}

void uwph_teb_leave_guest(void *teb, struct uwph_teb_swap *s)
{
    if (!teb || !s->in_guest) return;
    for (int i = 0; i < 5; i++) {
        s->guest[i] = TEB_FIELD(teb, swap_offsets[i]);   /* guest may have changed them */
        TEB_FIELD(teb, swap_offsets[i]) = s->host[i];
    }
    s->in_guest = 0;
}

void *uwph_teb_current(void)
{
#ifdef UWPH_BACKEND_WIN32
    /* x64 NT_TIB.Self. MinGW's NtCurrentTeb intrinsic models this as a
     * dereference near NULL, triggering GCC's array-bounds diagnostic. */
    void *teb;
    __asm__ volatile ("movq %%gs:0x30, %0" : "=r" (teb) : : "memory");
    return teb;
#else
    return NULL;
#endif
}

UWPH_API int uwph_arch_prctl(int code, uint64_t addr)
{
    struct uwph_thread *t = uwph_thread_current();
    switch (code) {
    case UWPH_ARCH_SET_GS:
        /* GS stays with the OS; remember Wine's TEB for the swap set */
        t->wine_teb = addr;
        if (addr) uwph_teb_capture_guest((const void *)(uintptr_t)addr, &t->teb_swap);
        return 0;
    case UWPH_ARCH_GET_GS:
        *(uint64_t *)(uintptr_t)addr = (uint64_t)(uintptr_t)uwph_teb_current();
        return 0;
    case UWPH_ARCH_GET_FS:
        *(uint64_t *)(uintptr_t)addr = 0;    /* FS base is unused by Windows x64 */
        return 0;
    case UWPH_ARCH_SET_FS:
        return addr ? UWPH_FAIL(UWPH_EPERM) : 0;
    default:
        return UWPH_FAIL(UWPH_EINVAL);
    }
}

/* No CRT or system calls while guest fields are installed. */
UWPH_API uintptr_t uwph_host_call(void *teb,
    const struct uwph_host_teb_state *host,
    uintptr_t (*callback)(void *), void *arg)
{
    struct uwph_host_teb_state guest;
    uintptr_t result;
    volatile uint32_t *error = (volatile uint32_t *)((char *)teb + UWPH_TEB_LASTERROR);
    guest.peb = TEB_FIELD(teb, UWPH_TEB_PEB);
    guest.tls_pointer = TEB_FIELD(teb, UWPH_TEB_TLSPOINTER);
    guest.last_error = *error;
    for (int i = 0; i < 64; ++i) {
        guest.tls_slots[i] = TEB_FIELD(teb, 0x1480 + i * 8);
        TEB_FIELD(teb, 0x1480 + i * 8) = host->tls_slots[i];
    }
    TEB_FIELD(teb, UWPH_TEB_PEB) = host->peb;
    TEB_FIELD(teb, UWPH_TEB_TLSPOINTER) = host->tls_pointer;
    *error = host->last_error;
    result = callback(arg);
    TEB_FIELD(teb, UWPH_TEB_PEB) = guest.peb;
    TEB_FIELD(teb, UWPH_TEB_TLSPOINTER) = guest.tls_pointer;
    for (int i = 0; i < 64; ++i)
        TEB_FIELD(teb, 0x1480 + i * 8) = guest.tls_slots[i];
    *error = guest.last_error;
    return result;
}
