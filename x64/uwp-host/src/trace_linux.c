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
int uwph_linux_native_mutex_init(void *m, const void *a)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEX_INIT); UWPH_NATIVE(int, "pthread_mutex_init", (void *, const void *)); UWPH_NATIVE_RETURN(fn(m,a)); }
int uwph_linux_native_mutexattr_init(void *a)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEXATTR_INIT); UWPH_NATIVE(int, "pthread_mutexattr_init", (void *)); UWPH_NATIVE_RETURN(fn(a)); }
int uwph_linux_native_mutexattr_destroy(void *a)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEXATTR_DESTROY); UWPH_NATIVE(int, "pthread_mutexattr_destroy", (void *)); UWPH_NATIVE_RETURN(fn(a)); }
int uwph_linux_native_mutexattr_settype(void *a, int t)
{ uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEXATTR_SETTYPE); UWPH_NATIVE(int, "pthread_mutexattr_settype", (void *, int)); UWPH_NATIVE_RETURN(fn(a,t)); }
long uwph_linux_syscall_entry(long n,long a,long b,long c,long d,long e,long f)
{ uwph_trace_hit(UWPH_TRACE_SYSCALL); UWPH_NATIVE(long, "syscall", (long,...)); UWPH_NATIVE_RETURN(fn(n,a,b,c,d,e,f)); }
int uwph_linux_prctl_entry(int n,unsigned long a,unsigned long b,unsigned long c,unsigned long d)
{ return uwph_prctl(n,a,b,c,d); }
#endif
