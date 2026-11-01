/*
 * GS / TEB ownership.
 *
 * On Linux, Wine points GS at its own TEB with arch_prctl(ARCH_SET_GS)
 * (signal_x86_64.c call_init_thunk). Guest PE code then reads gs:[0x30]
 * (TEB self), gs:[0x58] (TLS array), gs:[0x60] (PEB), gs:[0x08/0x10] (stack).
 *
 * A Windows host cannot move GS: the OS kernel owns it and points it at the
 * host thread's real TEB, which the host's own ntdll/kernelbase keep using.
 * macOS has the same problem (GS = pthread TSD) and Wine solves it there by
 * writing only Tib.Self and ThreadLocalStoragePointer into the gs block.
 * uwph follows that precedent with a "swap set": the guest-visible fields of
 * the OS TEB are replaced with Wine's values while guest code runs and put
 * back whenever the thread enters host code (Wine's syscall dispatcher,
 * exception entry, APCs). See NOTES-x64.md, "TEB/GS".
 */
#ifndef UWPH_TEB_H
#define UWPH_TEB_H
#ifdef __cplusplus
extern "C" {
#endif

/* offsets in the x64 TEB that guest code reaches directly through gs: */
#define UWPH_TEB_STACKBASE   0x08
#define UWPH_TEB_STACKLIMIT  0x10
#define UWPH_TEB_SELF        0x30
#define UWPH_TEB_TLSPOINTER  0x58
#define UWPH_TEB_PEB         0x60
#define UWPH_TEB_LASTERROR   0x68

struct uwph_teb_swap {
    uint64_t host[5];   /* saved OS values: StackBase, StackLimit, Self, TLS ptr, PEB */
    uint64_t guest[5];  /* Wine's values for the same fields */
    int      in_guest;
};

/* Install Wine's values into the TEB block at `teb` (host -> guest) and back. */
void uwph_teb_enter_guest(void *teb, struct uwph_teb_swap *s);
void uwph_teb_leave_guest(void *teb, struct uwph_teb_swap *s);
/* Fill s->guest from a Wine TEB (reads the same offsets from it). */
void uwph_teb_capture_guest(const void *wine_teb, struct uwph_teb_swap *s);

/* Snapshot these fields while host state is installed, before entering guest.
 * This bounded helper supplements the original five-field swap with inline TLS
 * and LastError. Normal-return callbacks only; exceptions must not escape it.
 * TLS expansion slots and other TEB fields are not covered. */
struct uwph_host_teb_state {
    uint64_t peb, tls_pointer, tls_slots[64];
    uint32_t last_error;
};
UWPH_API uintptr_t uwph_host_call(void *teb,
    const struct uwph_host_teb_state *host,
    uintptr_t (*callback)(void *), void *arg);

/* Base of the current thread's gs block (host TEB on Windows, 0 on Linux stub). */
void *uwph_teb_current(void);

/* arch_prctl replacement: ARCH_SET_GS records the Wine TEB for the swap set
 * (GS itself does not move); ARCH_GET_FS returns 0 on Windows x64. */
UWPH_API int uwph_arch_prctl(int code, uint64_t addr);

#ifdef __cplusplus
}
#endif
#endif
