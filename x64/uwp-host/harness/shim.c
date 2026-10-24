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
static int owned(int fd) { return fd>=0x100000 && fd<0x100400; }
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
