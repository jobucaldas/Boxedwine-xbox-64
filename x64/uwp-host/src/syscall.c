/* syscall() dispatcher for the raw syscalls Wine issues. */
#include "internal.h"
#include <stdarg.h>

UWPH_API long uwph_syscall(long nr, ...)
{
    uwph_trace_hit(UWPH_TRACE_SYSCALL);
    va_list ap;
    int64_t a[6] = {0};   /* not long: long is 32-bit on the Windows build (LLP64) */
    int nargs;
    switch (nr) {
    case UWPH_NR_gettid: return uwph_gettid();
    case UWPH_NR_futex: nargs = 6; break;
    case UWPH_NR_tgkill: nargs = 3; break;
    case UWPH_NR_tkill: case UWPH_NR_arch_prctl: nargs = 2; break;
    case UWPH_NR_utimensat: nargs = 4; break;
    default: return UWPH_FAIL(UWPH_ENOSYS);
    }
    va_start(ap, nr);
    for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, int64_t);
    va_end(ap);

#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(long, "syscall", (long,...));
    UWPH_NATIVE_RETURN(fn(nr,a[0],a[1],a[2],a[3],a[4],a[5]));
#endif
    switch (nr) {
    case UWPH_NR_gettid:
        return uwph_gettid();
    case UWPH_NR_futex:
        return uwph_futex((int *)(intptr_t)a[0], (int)a[1], (int)a[2], (const struct uwph_timespec *)(intptr_t)a[3],
                          (int *)(intptr_t)a[4], (int)a[5]);
    case UWPH_NR_tgkill:
        return uwph_tgkill((uwph_pid_t)a[0], (int)a[1], (int)a[2]);
    case UWPH_NR_tkill:
        return uwph_tgkill(uwph_getpid(), (int)a[0], (int)a[1]);
    case UWPH_NR_arch_prctl:
        return uwph_arch_prctl((int)a[0], (uint64_t)a[1]);
    case UWPH_NR_utimensat:
        return uwph_utimensat((int)a[0], (const char *)(intptr_t)a[1], (const struct uwph_timespec *)(intptr_t)a[2], (int)a[3]);
    default:
        return UWPH_FAIL(UWPH_ENOSYS);
    }
}
