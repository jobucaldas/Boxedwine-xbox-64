/*
 * Virtual memory: mmap family used by ntdll.so's virtual.c (views, reserved
 * areas, image mapping for Wine's own PE loader, thread stacks) and by
 * wineserver (shared mappings).
 *
 * UWP mapping (documented APIs, codeGeneration capability):
 *   reserve/commit RW      VirtualAllocFromApp (cannot create executable pages)
 *   make executable        VirtualProtectFromApp PAGE_EXECUTE_READ (needs codeGeneration;
 *                          PAGE_EXECUTE_READWRITE is rejected by the API)
 *   file / shared views    CreateFileMappingFromApp + MapViewOfFileFromApp
 *                          (FILE_MAP_EXECUTE needs codeGeneration)
 *   fixed-address replace  placeholders (VirtualAlloc2FromApp/MapViewOfFile3FromApp,
 *                          docs say "desktop apps only": probed at runtime, see probe.h)
 *
 * PROT_WRITE|PROT_EXEC cannot exist at the same time. Such ranges are tracked
 * as "W^X-emulated": committed RW, flipped to RX on an execute fault and back
 * to RW on a write fault (signal.c calls uwph_wx_handle_fault first).
 */
#ifndef UWPH_MEM_H
#define UWPH_MEM_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API void *uwph_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off);
UWPH_API int   uwph_munmap(void *addr, size_t len);
/* Strict W^X transition: rejects WRITE|EXEC; FromApp only on Windows. */
UWPH_API int   uwph_protect(void *addr, size_t len, int prot);
UWPH_API int   uwph_mprotect(void *addr, size_t len, int prot);
UWPH_API int   uwph_madvise(void *addr, size_t len, int advice);
UWPH_API int   uwph_msync(void *addr, size_t len, int flags);
UWPH_API int   uwph_mlock(const void *addr, size_t len);
UWPH_API int   uwph_munlock(const void *addr, size_t len);

/* ---- W^X emulation policy (portable, unit-tested on both builds) ---- */
enum uwph_wx_state { UWPH_WX_RW = 1, UWPH_WX_RX = 2 };
enum uwph_fault_kind { UWPH_FAULT_READ = 0, UWPH_FAULT_WRITE = 1, UWPH_FAULT_EXEC = 8 }; /* = ExceptionInformation[0] */

/* Register / forget a range that the guest asked to be RWX. */
int  uwph_wx_track(void *addr, size_t len);
void uwph_wx_untrack(void *addr, size_t len);
/* Pure decision: 0 = not ours; otherwise the state the page must flip to. */
int  uwph_wx_decide(const void *fault_addr, int fault_kind, int *new_state, void **page);
/* Decide + apply the flip on the host. 1 = handled (resume), 0 = not ours. */
int  uwph_wx_handle_fault(const void *fault_addr, int fault_kind);

/* Wine maps its user_shared_data at 0x7ffe0000. On a Windows host the real
 * KUSER_SHARED_DATA already lives there (read-only, same layout), so mmap of
 * that page is answered with the existing page (see NOTES-x64.md). */
#define UWPH_USER_SHARED_DATA ((void *)0x7ffe0000)

#ifdef __cplusplus
}
#endif
#endif
