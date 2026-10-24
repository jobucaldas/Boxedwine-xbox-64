/* Explicit harness-only x86-64 Linux implementation of the uwph contracts. */
#include "uwph/uwph.h"
#include "../src/linux_backend.h"
#include <sys/syscall.h>
#include <pthread.h>
#if !defined(__linux__) || !defined(__x86_64__) || !defined(UWPH_BACKEND_LINUX_PASSTHROUGH)
#error This backend is only for the selected Linux x86-64 harness
#endif
/* No libc syscall() dependency: future libc interposition cannot recurse. */
static long raw(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}
static long result(long r)
{
    if ((unsigned long)r >= (unsigned long)-4095) { uwph_errno = -r; return -1; }
    return r;
}
#define PTR(p) ((long)(p))
/* Private owned descriptor slots, never native fd numbers or synthetic fds.
 * rwlock pins the slot throughout operations; close takes exclusive ownership.
 * No adoption of borrowed host descriptors, including stdin/out/err. */
#define BASE 0x100000
#define SLOTS 1024
static int slots[SLOTS]; /* native fd + 1; zero = free */
static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static int native(int fd)
{
    if (fd < BASE || fd >= BASE + SLOTS || !slots[fd-BASE]) return -1;
    return slots[fd-BASE]-1;
}
static long fdcall(long nr, int fd, long b, long c, long d)
{
    pthread_rwlock_rdlock(&lock);
    int n = native(fd);
    long r = n < 0 ? -UWPH_EBADF : raw(nr,n,b,c,d,0,0);
    pthread_rwlock_unlock(&lock);
    return result(r);
}
int uwph_linux_openat(int dirfd, const char *path, int flags, unsigned mode)
{
    pthread_rwlock_wrlock(&lock);
    int dir = dirfd == UWPH_AT_FDCWD ? dirfd : native(dirfd);
    long r = dir == -1 ? -UWPH_EBADF : raw(SYS_openat,dir,PTR(path),flags,mode,0,0);
    if (r >= 0) {
        int i; for (i=0;i<SLOTS && slots[i];i++);
        if (i==SLOTS) { raw(SYS_close,r,0,0,0,0,0); r=-UWPH_EMFILE; }
        else { slots[i]=r+1; r=BASE+i; }
    }
    pthread_rwlock_unlock(&lock);
    return result(r);
}
int uwph_linux_open(const char *path, int flags, unsigned mode)
{
    return uwph_linux_openat(UWPH_AT_FDCWD,path,flags,mode);
}
int uwph_linux_close(int fd)
{
    pthread_rwlock_wrlock(&lock);
    int n=native(fd);
    long r=n<0 ? -UWPH_EBADF : raw(SYS_close,n,0,0,0,0,0);
    if(n>=0) slots[fd-BASE]=0; /* Linux close releases fd even on EINTR. */
    pthread_rwlock_unlock(&lock); return result(r);
}
uwph_ssize_t uwph_linux_read(int fd, void *buf, size_t len) { return fdcall(SYS_read,fd,PTR(buf),len,0); }
uwph_ssize_t uwph_linux_write(int fd, const void *buf, size_t len) { return fdcall(SYS_write,fd,PTR(buf),len,0); }
uwph_ssize_t uwph_linux_pread(int fd, void *buf, size_t len, uwph_off_t off) { return fdcall(SYS_pread64,fd,PTR(buf),len,off); }
uwph_ssize_t uwph_linux_pwrite(int fd, const void *buf, size_t len, uwph_off_t off) { return fdcall(SYS_pwrite64,fd,PTR(buf),len,off); }
uwph_off_t uwph_linux_lseek(int fd, uwph_off_t off, int whence) { return fdcall(SYS_lseek,fd,off,whence,0); }
int uwph_linux_fstat(int fd, struct uwph_stat *st) { return fdcall(SYS_fstat,fd,PTR(st),0,0); }
int uwph_linux_fstatat(int dirfd, const char *path, struct uwph_stat *st, int flags)
{
    pthread_rwlock_rdlock(&lock);
    int dir=dirfd==UWPH_AT_FDCWD ? dirfd : native(dirfd);
    long r=dir==-1 ? -UWPH_EBADF : raw(SYS_newfstatat,dir,PTR(path),PTR(st),flags,0,0);
    pthread_rwlock_unlock(&lock); return result(r);
}
int uwph_linux_stat(const char *path, struct uwph_stat *st) { return uwph_linux_fstatat(UWPH_AT_FDCWD,path,st,0); }
int uwph_linux_lstat(const char *path, struct uwph_stat *st) { return uwph_linux_fstatat(UWPH_AT_FDCWD,path,st,0x100); }
void *uwph_linux_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off)
{
    pthread_rwlock_rdlock(&lock);
    int n=(flags & UWPH_MAP_ANONYMOUS) ? -1 : native(fd);
    long r=(!(flags & UWPH_MAP_ANONYMOUS) && n<0) ? -UWPH_EBADF : raw(SYS_mmap,PTR(addr),len,prot,flags,n,off);
    pthread_rwlock_unlock(&lock); return (void *)result(r);
}
int uwph_linux_munmap(void *addr, size_t len) { return result(raw(SYS_munmap,PTR(addr),len,0,0,0,0)); }
int uwph_linux_mprotect(void *addr, size_t len, int prot) { return result(raw(SYS_mprotect,PTR(addr),len,prot,0,0,0)); }
int uwph_linux_clock_gettime(uwph_clockid_t id, struct uwph_timespec *ts) { return result(raw(SYS_clock_gettime,id,PTR(ts),0,0,0,0)); }
int uwph_linux_clock_getres(uwph_clockid_t id, struct uwph_timespec *ts) { return result(raw(SYS_clock_getres,id,PTR(ts),0,0,0,0)); }
int uwph_linux_nanosleep(const struct uwph_timespec *req, struct uwph_timespec *rem) { return result(raw(SYS_nanosleep,PTR(req),PTR(rem),0,0,0,0)); }
int uwph_linux_gettid(void) { return result(raw(SYS_gettid,0,0,0,0,0,0)); }
long uwph_linux_futex(int *uaddr, int op, int val, const struct uwph_timespec *timeout, int *uaddr2, int val3)
{
    /* Bounded typed WAIT/WAKE surface; never generic variadic forwarding. */
    int cmd=op & ~UWPH_FUTEX_PRIVATE_FLAG;
    if(cmd != UWPH_FUTEX_WAIT && cmd != UWPH_FUTEX_WAKE) { uwph_errno=UWPH_ENOSYS; return -1; }
    return result(raw(SYS_futex,PTR(uaddr),op,val,PTR(timeout),PTR(uaddr2),val3));
}
