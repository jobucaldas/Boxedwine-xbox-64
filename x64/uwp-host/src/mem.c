/*
 * mmap family on the UWP memory APIs, and the W^X emulation for ranges the
 * guest wants writable and executable at once.
 */
#include "internal.h"
#include "fake_proc.h"

/* ---- W^X tracking (portable) ---- */
#define MAX_WX_REGIONS 256

struct wx_region { uintptr_t base; size_t pages; uint8_t *state; };
static struct wx_region wx[MAX_WX_REGIONS];
static int nwx;
static uwph_lock_t wx_lock = UWPH_LOCK_INIT;

int uwph_mem_page_size(void) { return 4096; }

int uwph_wx_track(void *addr, size_t len)
{
    size_t ps = (size_t)uwph_mem_page_size(), pages = (len + ps - 1) / ps;
    uint8_t *st = malloc(pages);
    if (!st) return UWPH_FAIL(UWPH_ENOMEM);
    memset(st, UWPH_WX_RW, pages);
    uwph_wx_untrack(addr, len);
    uwph_lock(&wx_lock);
    if (nwx == MAX_WX_REGIONS) { uwph_unlock(&wx_lock); free(st); return UWPH_FAIL(UWPH_ENOMEM); }
    wx[nwx].base = (uintptr_t)addr & ~(ps - 1);
    wx[nwx].pages = pages;
    wx[nwx].state = st;
    nwx++;
    uwph_unlock(&wx_lock);
    return 0;
}

void uwph_wx_untrack(void *addr, size_t len)
{
    uintptr_t a = (uintptr_t)addr, e = a + len;
    size_t ps = (size_t)uwph_mem_page_size();
    uwph_lock(&wx_lock);
    for (int i = 0; i < nwx; i++) {
        uintptr_t rb = wx[i].base, re = rb + wx[i].pages * ps;
        if (re <= a || rb >= e) continue;
        /* whole-region removal; partial overlaps drop the region too (the
           caller re-tracks whatever remains RWX) */
        free(wx[i].state);
        wx[i--] = wx[--nwx];
    }
    uwph_unlock(&wx_lock);
}

int uwph_wx_decide(const void *fault_addr, int fault_kind, int *new_state, void **page)
{
    uintptr_t a = (uintptr_t)fault_addr;
    size_t ps = (size_t)uwph_mem_page_size();
    int ret = 0;
    uwph_lock(&wx_lock);
    for (int i = 0; i < nwx; i++) {
        size_t idx;
        if (a < wx[i].base || a >= wx[i].base + wx[i].pages * ps) continue;
        idx = (a - wx[i].base) / ps;
        if (fault_kind == UWPH_FAULT_EXEC && wx[i].state[idx] == UWPH_WX_RW) *new_state = UWPH_WX_RX;
        else if (fault_kind == UWPH_FAULT_WRITE && wx[i].state[idx] == UWPH_WX_RX) *new_state = UWPH_WX_RW;
        else break;     /* a genuine fault inside the region */
        wx[i].state[idx] = (uint8_t)*new_state;
        *page = (void *)(wx[i].base + idx * ps);
        ret = 1;
        break;
    }
    uwph_unlock(&wx_lock);
    return ret;
}

#ifdef UWPH_BACKEND_WIN32
/* VirtualProtectFromApp is resolved at runtime: Wine (used to test this
 * build on Linux) does not export it. The fallback is VirtualProtect, and the
 * W^X rule is enforced here either way: never PAGE_EXECUTE_READWRITE. */
typedef BOOL (WINAPI *vprotect_fn)(PVOID, SIZE_T, ULONG, PULONG);
static vprotect_fn p_vprotect;
static int vprotect_is_fromapp;

static void resolve_vprotect(void)
{
    HMODULE k;
    if (p_vprotect) return;
    if ((k = GetModuleHandleW(L"kernelbase.dll")) &&
        (p_vprotect = (vprotect_fn)(void *)GetProcAddress(k, "VirtualProtectFromApp"))) {
        vprotect_is_fromapp = 1;
        return;
    }
    if ((k = GetModuleHandleW(L"kernel32.dll")) &&
        (p_vprotect = (vprotect_fn)(void *)GetProcAddress(k, "VirtualProtectFromApp"))) {
        vprotect_is_fromapp = 1;
        return;
    }
    if (k) p_vprotect = (vprotect_fn)(void *)GetProcAddress(k, "VirtualProtect");
}

int uwph_mem_protect_is_fromapp(void) { resolve_vprotect(); return vprotect_is_fromapp; }

static int host_protect(void *addr, size_t len, DWORD prot)
{
    ULONG old;
    if (prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY) return UWPH_FAIL(UWPH_EACCES);
    resolve_vprotect();
    if (!p_vprotect || !p_vprotect(addr, len, prot, &old)) return UWPH_FAIL(UWPH_EACCES);
    if (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE)
        FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 0;
}

int uwph_wx_handle_fault(const void *fault_addr, int fault_kind)
{
    int st;
    void *page;
    if (!uwph_wx_decide(fault_addr, fault_kind, &st, &page)) return 0;
    return host_protect(page, (size_t)uwph_mem_page_size(),
                        st == UWPH_WX_RX ? PAGE_EXECUTE_READ : PAGE_READWRITE) == 0;
}

/* initial protection for VirtualAllocFromApp (never executable) */
static DWORD alloc_protect(int prot)
{
    if (prot & UWPH_PROT_WRITE) return PAGE_READWRITE;
    if (prot & (UWPH_PROT_READ | UWPH_PROT_EXEC)) return PAGE_READWRITE;  /* RX/R set after fill */
    return PAGE_NOACCESS;
}

static int apply_prot(void *addr, size_t len, int prot)
{
    int rwx = (prot & UWPH_PROT_WRITE) && (prot & UWPH_PROT_EXEC);
    if (rwx) {
        if (uwph_wx_track(addr, len) < 0) return -1;
        return host_protect(addr, len, PAGE_READWRITE);
    }
    uwph_wx_untrack(addr, len);
    if (prot & UWPH_PROT_EXEC) return host_protect(addr, len, (prot & UWPH_PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (prot & UWPH_PROT_WRITE) return host_protect(addr, len, PAGE_READWRITE);
    if (prot & UWPH_PROT_READ) return host_protect(addr, len, PAGE_READONLY);
    return host_protect(addr, len, PAGE_NOACCESS);
}

UWPH_API void *uwph_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_mmap(addr, len, prot, flags, fd, off);
#else
    int fixed = flags & (UWPH_MAP_FIXED | UWPH_MAP_FIXED_NOREPLACE);
    int reserve_only = prot == UWPH_PROT_NONE && (flags & UWPH_MAP_NORESERVE);
    ULONG type = reserve_only ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT;
    void *p;
    (void)off;

    if (!len) return uwph_errno = UWPH_EINVAL, UWPH_MAP_FAILED;
    if (!(flags & UWPH_MAP_ANONYMOUS) || fd != -1)
        /* file-backed views: ENODEV makes Wine's virtual.c fall back to
           reading the file into anonymous memory (map_file_into_view). Shared
           file views (CreateFileMappingFromApp/MapViewOfFileFromApp) are the
           next step. */
        return uwph_errno = UWPH_ENOSYS, UWPH_MAP_FAILED;

    if (addr == UWPH_USER_SHARED_DATA && fixed) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) return addr;  /* the OS's page */
    }

    p = VirtualAllocFromApp(addr, len, type, reserve_only ? PAGE_NOACCESS : alloc_protect(prot));
    if (!p && addr && (flags & UWPH_MAP_FIXED)) {
        /* inside a range we reserved earlier: replace = decommit + commit */
        VirtualFree(addr, len, MEM_DECOMMIT);
        p = reserve_only ? addr : VirtualAllocFromApp(addr, len, MEM_COMMIT, alloc_protect(prot));
    }
    if (!p && addr && !fixed) p = VirtualAllocFromApp(NULL, len, type, reserve_only ? PAGE_NOACCESS : alloc_protect(prot));
    if (!p) return uwph_errno = (flags & UWPH_MAP_FIXED_NOREPLACE) ? UWPH_EEXIST : UWPH_ENOMEM, UWPH_MAP_FAILED;
    if (fixed && p != addr) { VirtualFree(p, 0, MEM_RELEASE); return uwph_errno = UWPH_EEXIST, UWPH_MAP_FAILED; }
    if (!reserve_only && alloc_protect(prot) != PAGE_NOACCESS && apply_prot(p, len, prot) < 0) {
        int e = uwph_errno;
        VirtualFree(p, 0, MEM_RELEASE);
        return uwph_errno = e, UWPH_MAP_FAILED;
    }
    uwph_proc_mapping(p, len, 1);
    return p;
#endif
}

UWPH_API int uwph_munmap(void *addr, size_t len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_munmap(addr, len);
#else
    MEMORY_BASIC_INFORMATION mbi;
    uwph_wx_untrack(addr, len);
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return UWPH_FAIL(UWPH_EINVAL);
    if (mbi.AllocationBase == addr && mbi.RegionSize >= len && VirtualFree(addr, 0, MEM_RELEASE)) {
        uwph_proc_mapping(addr, mbi.RegionSize, 0); return 0;
    }
    /* partial unmap of a reservation: Windows cannot release part of it, so
       the range goes back to reserved (Wine treats it as free again) */
    if (!VirtualFree(addr, len, MEM_DECOMMIT)) return UWPH_FAIL(UWPH_EINVAL);
    uwph_proc_mapping(addr, len, 0); return 0;
#endif
}

UWPH_API int uwph_mprotect(void *addr, size_t len, int prot)
{
    uwph_trace_hit(UWPH_TRACE_MPROTECT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_mprotect(addr, len, prot);
#else
    if (prot != UWPH_PROT_NONE) {
        MEMORY_BASIC_INFORMATION mbi;
        /* Wine commits by mprotect()ing reserved PROT_NONE ranges */
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE &&
            !VirtualAllocFromApp(addr, len, MEM_COMMIT, PAGE_READWRITE))
            return UWPH_FAIL(UWPH_ENOMEM);
    }
    return apply_prot(addr, len, prot);
#endif
}

UWPH_API int uwph_madvise(void *addr, size_t len, int advice)
{
    if (advice == UWPH_MADV_DONTNEED) {
        /* private anonymous: contents become zero (DiscardVirtualMemory is
           the cheaper option where available) */
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return UWPH_FAIL(UWPH_EINVAL);
        VirtualFree(addr, len, MEM_DECOMMIT);
        if (!VirtualAllocFromApp(addr, len, MEM_COMMIT, PAGE_READWRITE)) return UWPH_FAIL(UWPH_ENOMEM);
        if (mbi.Protect != PAGE_READWRITE) {
            ULONG old;
            resolve_vprotect();
            if (p_vprotect && mbi.Protect != PAGE_EXECUTE_READWRITE) p_vprotect(addr, len, mbi.Protect, &old);
        }
    }
    return 0;
}

UWPH_API int uwph_msync(void *addr, size_t len, int flags)
{
    (void)flags;
    return FlushViewOfFile(addr, len) || GetLastError() == ERROR_INVALID_ADDRESS ? 0 : UWPH_FAIL(UWPH_EINVAL);
}
#else
int uwph_mem_protect_is_fromapp(void) { return 0; }
int uwph_wx_handle_fault(const void *fault_addr, int fault_kind)
{
    int st;
    void *page;
    return uwph_wx_decide(fault_addr, fault_kind, &st, &page);   /* no host to flip */
}
UWPH_API void *uwph_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_mmap(addr, len, prot, flags, fd, off);
#else
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return uwph_errno = UWPH_ENOSYS, UWPH_MAP_FAILED;
#endif
}
UWPH_API int uwph_munmap(void *addr, size_t len) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_munmap(addr, len);
#else
    (void)addr; (void)len; return UWPH_ENOSYS_INT();
#endif
}
UWPH_API int uwph_mprotect(void *addr, size_t len, int prot) {
    uwph_trace_hit(UWPH_TRACE_MPROTECT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_mprotect(addr, len, prot);
#else
    (void)addr; (void)len; (void)prot; return UWPH_ENOSYS_INT();
#endif
}
UWPH_API int uwph_madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_msync(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return UWPH_ENOSYS_INT(); }
#endif

/* VirtualLock is not in the APP partition; locking is advisory for Wine */
UWPH_API int uwph_mlock(const void *addr, size_t len)   { (void)addr; (void)len; return 0; }
UWPH_API int uwph_munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

/* Explicit loader transition; unlike the legacy mprotect emulation this never
 * tracks RWX or falls back to desktop VirtualProtect. codeGeneration is needed
 * for executable protection in an actual UWP package. */
UWPH_API int uwph_protect(void *addr, size_t len, int prot)
{
    if (!len || ((uintptr_t)addr & 4095) ||
        (prot & ~(UWPH_PROT_READ | UWPH_PROT_WRITE | UWPH_PROT_EXEC)))
        return UWPH_FAIL(UWPH_EINVAL);
    if ((prot & UWPH_PROT_WRITE) && (prot & UWPH_PROT_EXEC))
        return UWPH_FAIL(UWPH_EACCES);
#ifdef UWPH_BACKEND_WIN32
    resolve_vprotect();
    if (!vprotect_is_fromapp) return UWPH_FAIL(UWPH_ENOSYS);
#endif
    return uwph_mprotect(addr, len, prot);
}
