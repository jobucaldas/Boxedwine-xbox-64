#define _GNU_SOURCE
#include "counters.h"
#include "uwph/uwph.h"
#include <stdatomic.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
/* No constructor, RTLD_NEXT resolver, or interposed I/O in instrumentation. */
long harness_raw(long n,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d, r8 __asm__("r8")=e, r9 __asm__("r9")=f;
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),
                     "r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return r;
}
#define MODULES 32
static struct { char name[512]; unsigned long calls[C_COUNT], backend[C_COUNT]; } modules[MODULES];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long backend[C_COUNT], bypass[C_COUNT], recursion, unattributed, overflow;
static _Thread_local int depth, origin=-1;
static const char *names[]={"open","read","close","mmap","munmap"};
int count_begin(int op,void *caller)
{
    if(depth++) { atomic_fetch_add(&recursion,1); depth--; return 0; }
    int saved=errno;
    Dl_info info={0};
    const char *name=dladdr(caller,&info) && info.dli_fname ? info.dli_fname : "<unknown>";
    pthread_mutex_lock(&lock);
    int i;
    for(i=0;i<MODULES;i++) {
        if(!modules[i].name[0]) {
            if(strlen(name)>=sizeof modules[i].name) { i=MODULES; break; }
            memcpy(modules[i].name,name,strlen(name)+1); break;
        }
        if(!strcmp(modules[i].name,name)) break;
    }
    origin=i<MODULES ? i : -1;
    if(origin>=0) modules[origin].calls[op]++;
    else atomic_fetch_add(&overflow,1);
    pthread_mutex_unlock(&lock);
    errno=saved;
    return 1;
}
void count_end(void) { origin=-1; depth--; }
void count_bypass(int op) { atomic_fetch_add(&bypass[op],1); }
void uwph_harness_backend_entry(int op)
{
    atomic_fetch_add(&backend[op],1);
    pthread_mutex_lock(&lock);
    if(origin>=0) modules[origin].backend[op]++;
    else atomic_fetch_add(&unattributed,1);
    pthread_mutex_unlock(&lock);
}
/* Explicit snapshot API plus _exit hook: destructor is only a final convenience.
 * Report path is trusted harness configuration, never forwarded to uwph.
 * Hex module names avoid JSON escaping ambiguity. snprintf formats memory only
 * (its own O2 trace hits are included in the trace rows); all report I/O uses
 * raw syscalls. */
void uwph_shim_report(void)
{
    int saved=errno;
    if(depth) { atomic_fetch_add(&recursion,1); return; }
    depth++;
    const char *path=getenv("UWPH_REPORT");
    if(!path) { depth--; errno=saved; return; }
    /* Wine can create children even when wineboot's PE is disabled. Separate
     * process reports prevent a child from overwriting the launcher evidence. */
    char pidpath[4096];
    if(getenv("UWPH_REPORT_PID")) {
        int n=snprintf(pidpath,sizeof pidpath,"%s.%ld",path,
                       harness_raw(SYS_getpid,0,0,0,0,0,0));
        if(n<0 || (size_t)n>=sizeof pidpath) { depth--; errno=saved; return; }
        path=pidpath;
    }
    long fd=harness_raw(SYS_openat,AT_FDCWD,(long)path,O_WRONLY|O_CREAT|O_TRUNC,0600,0,0);
    if(fd<0) { depth--; errno=saved; return; }
    char buf[4096]; size_t used=0;
    pthread_mutex_lock(&lock);
#define LINE(...) do { int n=snprintf(buf,sizeof buf,__VA_ARGS__); if(n>0 && (size_t)n<sizeof buf) { size_t pos=0; while(pos<(size_t)n) { long r=harness_raw(SYS_write,fd,(long)(buf+pos),n-pos,0,0,0); if(r==-EINTR) continue; if(r<=0) break; pos+=r; } } } while(0)
    LINE("pid %ld\n",harness_raw(SYS_getpid,0,0,0,0,0,0));
    for(int o=0;o<C_COUNT;o++)
        LINE("total %s %lu %lu\n",names[o],atomic_load(&backend[o]),atomic_load(&bypass[o]));
    for(int i=0;i<MODULES && modules[i].name[0];i++) {
        char hex[1025];
        for(used=0;modules[i].name[used];used++) {
            unsigned char c=modules[i].name[used];
            hex[2*used]="0123456789abcdef"[c>>4]; hex[2*used+1]="0123456789abcdef"[c&15];
        }
        hex[2*used]=0;
        for(int o=0;o<C_COUNT;o++)
            LINE("module %s %s %lu %lu\n",hex,names[o],modules[i].calls[o],modules[i].backend[o]);
    }
    for(int t=0;t<UWPH_TRACE_COUNT;t++)
        LINE("trace %s %llu\n",uwph_trace_name(t),(unsigned long long)uwph_trace_count(t));
    LINE("classification recursion=%lu unattributed=%lu module_overflow=%lu instrumentation_raw_io=1\n",
         atomic_load(&recursion),atomic_load(&unattributed),atomic_load(&overflow));
    pthread_mutex_unlock(&lock);
    harness_raw(SYS_close,fd,0,0,0,0,0);
    depth--; errno=saved;
}
__attribute__((destructor)) static void final_report(void) { uwph_shim_report(); }
