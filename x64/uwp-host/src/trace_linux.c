/* Harness-only libc adapters. Native FILE and pthread objects retain the
 * host ABI, so non-interposed companion libc operations remain compatible. */
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
#define _GNU_SOURCE
#include "internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
void *uwph_linux_symbol(const char *name)
{
    void *(*resolve)(void *, const char *) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    void *p = resolve(RTLD_NEXT, name);
    if (!p) abort(); /* Missing required host ABI: never silently emulate it. */
    return p;
}
int uwph_linux_native_mutex_lock(void *m)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEX_LOCK); UWPH_NATIVE(int, "pthread_mutex_lock", (void *)); UWPH_NATIVE_RETURN(fn(m)); }
int uwph_linux_native_mutex_unlock(void *m)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEX_UNLOCK); UWPH_NATIVE(int, "pthread_mutex_unlock", (void *)); UWPH_NATIVE_RETURN(fn(m)); }
#endif
