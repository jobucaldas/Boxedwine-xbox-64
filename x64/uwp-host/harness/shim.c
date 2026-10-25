#define _GNU_SOURCE
#include "uwph/uwph.h"
#include "counters.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
_Static_assert(sizeof(off_t)==sizeof(uwph_off_t),"offset ABI");
_Static_assert(sizeof(ssize_t)==sizeof(uwph_ssize_t),"read ABI");
_Static_assert(O_CREAT==UWPH_O_CREAT && MAP_ANONYMOUS==UWPH_MAP_ANONYMOUS,"flag ABI");
_Static_assert(sizeof(mode_t)==sizeof(unsigned int),"promoted mode ABI");
_Static_assert(O_RDONLY==UWPH_O_RDONLY && O_WRONLY==UWPH_O_WRONLY &&
               O_RDWR==UWPH_O_RDWR && O_EXCL==UWPH_O_EXCL &&
               O_TRUNC==UWPH_O_TRUNC && O_CLOEXEC==UWPH_O_CLOEXEC,"open ABI");
_Static_assert(PROT_READ==UWPH_PROT_READ && PROT_WRITE==UWPH_PROT_WRITE &&
               PROT_EXEC==UWPH_PROT_EXEC && MAP_PRIVATE==UWPH_MAP_PRIVATE &&
               MAP_SHARED==UWPH_MAP_SHARED && MAP_FIXED==UWPH_MAP_FIXED,"mapping ABI");
#define BEGIN(op,fail) if(!count_begin(op,__builtin_return_address(0))) { errno=EDEADLK; return fail; }
#define END(r,failed) do { int e=(failed) ? uwph_errno : saved; count_end(); errno=e; return r; } while(0)
#ifdef UWPH_HARNESS_NATIVE_FDS
static int owned(int fd) { return 1; }
#else
static int owned(int fd) { return fd>=0x100000 && fd<0x100400; }
#endif
static long native_result(long r) { if((unsigned long)r>=(unsigned long)-4095) { errno=-r; return -1; } return r; }
int open(const char *path,int flags,...)
{
    unsigned mode=0;
    /* Only these commands have a mode argument; no speculative va_arg. */
    if((flags&O_CREAT) || (flags&O_TMPFILE)==O_TMPFILE) {
        va_list ap; va_start(ap,flags); mode=va_arg(ap,unsigned int); va_end(ap);
    }
    BEGIN(C_OPEN,-1);
    int saved=errno, r=uwph_open(path,flags,mode);
    END(r,r<0);
}
ssize_t read(int fd,void *buf,size_t len)
{
    BEGIN(C_READ,-1);
    if(!owned(fd)) { count_bypass(C_READ); long r=harness_raw(SYS_read,fd,(long)buf,len,0,0,0); count_end(); return native_result(r); }
    int saved=errno; ssize_t r=uwph_read(fd,buf,len); END(r,r<0);
}
int close(int fd)
{
    BEGIN(C_CLOSE,-1);
    if(!owned(fd)) { count_bypass(C_CLOSE); long r=harness_raw(SYS_close,fd,0,0,0,0,0); count_end(); return native_result(r); }
    int saved=errno, r=uwph_close(fd); END(r,r<0);
}
void *mmap(void *addr,size_t len,int prot,int flags,int fd,off_t off)
{
    BEGIN(C_MMAP,MAP_FAILED);
    /* Borrowed native fds have no uwph ownership: explicit counted passthrough. */
    if(!(flags&MAP_ANONYMOUS) && !owned(fd)) {
        count_bypass(C_MMAP);
        long r=harness_raw(SYS_mmap,(long)addr,len,prot,flags,fd,off);
        count_end(); return (void *)native_result(r);
    }
    int saved=errno; void *r=uwph_mmap(addr,len,prot,flags,fd,off); END(r,r==MAP_FAILED);
}
int munmap(void *addr,size_t len)
{
    BEGIN(C_MUNMAP,-1);
    int saved=errno, r=uwph_munmap(addr,len); END(r,r<0);
}
__attribute__((noreturn)) void _exit(int status)
{
    uwph_shim_report();
    harness_raw(SYS_exit_group,status,0,0,0,0,0);
    __builtin_unreachable();
}

/* O2 traced entries. Native FILE/pthread ABI bridges live inside the Linux
 * backend; Windows uses the existing uwph object implementation. */
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <locale.h>
#include <time.h>
extern int uwph_linux_native_mutex_lock(void *);
extern int uwph_linux_native_mutex_unlock(void *);
int snprintf(char *b, size_t n, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); /* formatting through uwph needs a va_list entry */
  int r = uwph_vsnprintf(b, n, fmt, ap); va_end(ap); return r; }
FILE *fopen(const char *p, const char *m) { FILE *r = (FILE *)uwph_fopen(p,m); if (!r) errno=uwph_errno; return r; }
int fclose(FILE *f) { int r=uwph_fclose((uwph_FILE *)f); if(r<0) errno=uwph_errno; return r; }
int feof(FILE *f) { return uwph_feof((uwph_FILE *)f); }
int __isoc23_fscanf(FILE *f, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=uwph_vfscanf((uwph_FILE *)f,fmt,ap); va_end(ap); if(r<0) errno=uwph_errno; return r; }
void *pthread_getspecific(pthread_key_t k) { return uwph_pthread_getspecific(k); }
int pthread_sigmask(int how, const sigset_t *s, sigset_t *o)
{ return uwph_pthread_sigmask(how,(const uwph_sigset_t *)s,(uwph_sigset_t *)o); }
int stat(const char *p, struct stat *s)
{ int r=uwph_stat(p,(struct uwph_stat *)s); if(r<0) errno=uwph_errno; return r; }
ssize_t write(int fd, const void *b, size_t n)
{ ssize_t r=uwph_write(fd,b,n); if(r<0) errno=uwph_errno; return r; }
int pthread_mutex_lock(pthread_mutex_t *m) { return uwph_linux_native_mutex_lock(m); }
int pthread_mutex_unlock(pthread_mutex_t *m) { return uwph_linux_native_mutex_unlock(m); }
ssize_t writev(int fd, const struct iovec *v, int n)
{ ssize_t r=uwph_writev(fd,(const struct uwph_iovec *)v,n); if(r<0) errno=uwph_errno; return r; }
char *getenv(const char *n) { return uwph_getenv(n); }
int asprintf(char **b, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=uwph_vasprintf(b,fmt,ap); va_end(ap); if(r<0) errno=uwph_errno; return r; }
int sigaddset(sigset_t *s, int n)
{ int r=uwph_sigaddset((uwph_sigset_t *)s,n); if(r<0) errno=uwph_errno; return r; }
int fcntl(int fd, int cmd, ...)
{
    intptr_t arg=0; va_list ap;
    if (cmd != F_GETFD && cmd != F_GETFL) {
        va_start(ap,cmd);
        if (cmd==F_GETLK || cmd==F_SETLK || cmd==F_SETLKW) arg=(intptr_t)va_arg(ap,void *);
        else arg=va_arg(ap,int);
        va_end(ap);
    }
    int r;
    if (cmd==F_GETLK || cmd==F_SETLK || cmd==F_SETLKW) r=uwph_fcntl(fd,cmd,(void *)arg);
    else r=uwph_fcntl(fd,cmd,(int)arg);
    if(r<0) errno=uwph_errno;
    return r;
}
char *setlocale(int c, const char *l) { return uwph_setlocale(c,l); }
time_t time(time_t *t) { return uwph_time((uwph_time_t *)t); }
uid_t getuid(void) { return uwph_getuid(); }
char *realpath(const char *p, char *b) { char *r=uwph_realpath(p,b); if(!r) errno=uwph_errno; return r; }
_Static_assert(sizeof(struct stat)==sizeof(struct uwph_stat),"stat ABI");
_Static_assert(sizeof(sigset_t)==sizeof(uwph_sigset_t),"sigset ABI");
