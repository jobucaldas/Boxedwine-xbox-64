/* Explicit harness-only x86-64 Linux implementation of the uwph contracts. */
#include "uwph/uwph.h"
#include "../src/linux_backend.h"
#include <sys/syscall.h>
#include <pthread.h>
#include "../src/fake_proc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "counters.h"
extern void uwph_harness_backend_entry(int) __attribute__((weak));
#define COUNT(op) do { if (uwph_harness_backend_entry) uwph_harness_backend_entry(op); } while (0)
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
#ifndef UWPH_HARNESS_NATIVE_FDS
static int slots[SLOTS]; /* native fd + 1; zero = free */
static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static int native(int fd)
{
    if (fd < BASE || fd >= BASE + SLOTS || !slots[fd-BASE]) return -1;
    return slots[fd-BASE]-1;
}
#else
/* Mixed Wine/libc harness only: native descriptors are borrowed for read/map,
 * created by open and consumed by close. Never used by synthetic-fd APIs.
 * This separately compiled adapter preserves O1/O2 private-handle contracts. */
/* No private slot lifetime to pin. Holding an rwlock across a native pipe
 * read would deadlock Wine's other threads trying to open/close descriptors. */
static int native(int fd) { return fd; }
#endif
#ifdef UWPH_HARNESS_NATIVE_FDS
#define RDLOCK() ((void)0)
#define WRLOCK() ((void)0)
#define UNLOCK() ((void)0)
#else
#define RDLOCK() pthread_rwlock_rdlock(&lock)
#define WRLOCK() pthread_rwlock_wrlock(&lock)
#define UNLOCK() pthread_rwlock_unlock(&lock)
#endif
static struct big_file *big_find(int fd);
static long big_generic(struct big_file *, long, long, long, long);
/* Shared namespace readers serialize split operations here; ordinary blocking
 * pipe I/O never takes this mutex. Namespace writers already exclude them. */
static pthread_mutex_t big_operation_lock = PTHREAD_MUTEX_INITIALIZER;
static long fdcall(long nr, int fd, long b, long c, long d)
{
    RDLOCK();
    int n = native(fd);
    struct big_file *split = big_find(n);
    if(split) pthread_mutex_lock(&big_operation_lock);
    long r = n < 0 ? -UWPH_EBADF : split ? big_generic(split,nr,b,c,d) : raw(nr,n,b,c,d,0,0);
    if(split) pthread_mutex_unlock(&big_operation_lock);
    UNLOCK();
    return result(r);
}
/* Split implementation uses raw syscalls, under the descriptor lock. */
#include "bigfile.inc"
/* Handling is separate from the descriptor/negative errno result. */
static long proc_open(const char *path, int flags, int *handled)
{
    char *data; size_t len;
    *handled = path && (!strcmp(path, "/proc") || !strncmp(path, "/proc/", 6));
    if (!*handled) return 0;
    if (!strcmp(path, "/proc")) return -UWPH_ENOENT;
    if (flags & (UWPH_O_ACCMODE | UWPH_O_CREAT | UWPH_O_TRUNC)) return -UWPH_EACCES;
    if (!strcmp(path, "/proc/self/exe")) {
        char exe[1024];
        if (uwph_fake_proc_exe(exe, sizeof(exe)) < 0) return -uwph_errno;
        return raw(SYS_openat, UWPH_AT_FDCWD, PTR(exe), flags, 0, 0, 0);
    }
    int rc = uwph_fake_proc_snapshot(path, &data, &len);
    if (rc < 0) return -uwph_errno;
    long fd = raw(SYS_memfd_create, PTR("uwph-proc"), 3, 0, 0, 0, 0);
    if (fd >= 0) {
        size_t done = 0;
        while (done < len) {
            long n = raw(SYS_write, fd, PTR(data + done), len - done, 0, 0, 0);
            if (n <= 0) { raw(SYS_close, fd, 0, 0, 0, 0, 0); fd = n < 0 ? n : -UWPH_EIO; break; }
            done += n;
        }
        if (fd >= 0) {
            raw(SYS_lseek, fd, 0, 0, 0, 0, 0);
            /* Seal writes/growth/shrink before handing the descriptor out. */
            long r = raw(SYS_fcntl, fd, 1033, 15, 0, 0, 0);
            if (r < 0) { raw(SYS_close, fd, 0, 0, 0, 0, 0); fd = r; }
        }
    }
    free(data);
    return fd;
}
int uwph_linux_openat(int dirfd, const char *path, int flags, unsigned mode)
{
    WRLOCK();
    /* Linux ignores dirfd for absolute paths; relative paths still require
     * an owned descriptor (or AT_FDCWD), never an arbitrary native fd. */
    int dir = path && path[0] == '/' ? UWPH_AT_FDCWD :
              dirfd == UWPH_AT_FDCWD ? dirfd : native(dirfd);
    int handled;
    long r = proc_open(path, flags, &handled);
    if (!handled) {
        r = big_open(dir, path, flags, mode, &handled);
        if (!handled) r = dir == -1 ? -UWPH_EBADF : raw(SYS_openat,dir,PTR(path),flags,mode,0,0);
    }
#ifndef UWPH_HARNESS_NATIVE_FDS
    if (r >= 0) {
        int i; for (i=0;i<SLOTS && slots[i];i++);
        if (i==SLOTS) { big_close(r); raw(SYS_close,r,0,0,0,0,0); r=-UWPH_EMFILE; }
        else { slots[i]=r+1; r=BASE+i; }
    }
#endif
    UNLOCK();
    return result(r);
}
int uwph_linux_open(const char *path, int flags, unsigned mode)
{
    COUNT(C_OPEN);
    return uwph_linux_openat(UWPH_AT_FDCWD,path,flags,mode);
}
int uwph_linux_close(int fd)
{
    COUNT(C_CLOSE);
    WRLOCK();
    int n=native(fd);
    big_close(n);
    long r=n<0 ? -UWPH_EBADF : raw(SYS_close,n,0,0,0,0,0);
#ifndef UWPH_HARNESS_NATIVE_FDS
    if(n>=0) slots[fd-BASE]=0; /* Linux close releases fd even on EINTR. */
#endif
    UNLOCK(); return result(r);
}
uwph_ssize_t uwph_linux_read(int fd, void *buf, size_t len) { COUNT(C_READ); return fdcall(SYS_read,fd,PTR(buf),len,0); }
uwph_ssize_t uwph_linux_write(int fd, const void *buf, size_t len) { return fdcall(SYS_write,fd,PTR(buf),len,0); }
uwph_ssize_t uwph_linux_pread(int fd, void *buf, size_t len, uwph_off_t off) { return big_call(fd, buf, len, off, 0); }
uwph_ssize_t uwph_linux_pwrite(int fd, const void *buf, size_t len, uwph_off_t off) { return big_call(fd, (void *)buf, len, off, 1); }
uwph_off_t uwph_linux_lseek(int fd, uwph_off_t off, int whence) { return fdcall(SYS_lseek,fd,off,whence,0); }
int uwph_linux_fstat(int fd, struct uwph_stat *st) {
    RDLOCK(); int n = native(fd);
    long r = n < 0 ? -UWPH_EBADF : raw(SYS_fstat,n,PTR(st),0,0,0,0);
    struct big_file *b = big_find(n);
    if (!r && b) {
        pthread_mutex_lock(&big_operation_lock);
        long size=big_size(b); if(size<0) r=size; else st->st_size=size;
        pthread_mutex_unlock(&big_operation_lock);
    }
    UNLOCK(); return result(r);
}
int uwph_linux_ftruncate(int fd, uwph_off_t len) {
    WRLOCK(); int n = native(fd); struct big_file *b = big_find(n);
    long r = b ? big_resize(b, len) : -UWPH_ENOSYS;
    UNLOCK(); return result(r);
}
int uwph_linux_fstatat(int dirfd, const char *path, struct uwph_stat *st, int flags)
{
    RDLOCK();
    int dir=path && path[0]=='/' ? UWPH_AT_FDCWD :
            dirfd==UWPH_AT_FDCWD ? dirfd : native(dirfd);
    long r=dir==-1 ? -UWPH_EBADF : raw(SYS_newfstatat,dir,PTR(path),PTR(st),flags,0,0);
    UNLOCK(); return result(r);
}
int uwph_linux_stat(const char *path, struct uwph_stat *st) { return uwph_linux_fstatat(UWPH_AT_FDCWD,path,st,0); }
int uwph_linux_lstat(const char *path, struct uwph_stat *st) { return uwph_linux_fstatat(UWPH_AT_FDCWD,path,st,0x100); }
void *uwph_linux_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off)
{
    COUNT(C_MMAP);
    RDLOCK();
    int n=(flags & UWPH_MAP_ANONYMOUS) ? -1 : native(fd);
    struct big_file *b = (flags & UWPH_MAP_ANONYMOUS) ? NULL : big_find(n);
    if (b) {
        pthread_mutex_lock(&big_operation_lock);
        long r = big_map(b, addr, len, prot, flags, off);
        pthread_mutex_unlock(&big_operation_lock);
        UNLOCK(); return (void *)result(r);
    }
    long r=(!(flags & UWPH_MAP_ANONYMOUS) && n<0) ? -UWPH_EBADF : raw(SYS_mmap,PTR(addr),len,prot,flags,n,off);
    UNLOCK(); return (void *)result(r);
}
int uwph_linux_munmap(void *addr, size_t len) { COUNT(C_MUNMAP); return result(raw(SYS_munmap,PTR(addr),len,0,0,0,0)); }
int uwph_linux_mprotect(void *addr, size_t len, int prot) { COUNT(C_PROTECT); return result(raw(SYS_mprotect,PTR(addr),len,prot,0,0,0)); }
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
uwph_ssize_t uwph_linux_writev(int fd, const struct uwph_iovec *iov, int cnt) { return fdcall(SYS_writev,fd,PTR(iov),cnt,0); }
int uwph_linux_fcntl(int fd, int cmd, long arg)
{
#ifndef UWPH_HARNESS_NATIVE_FDS
    /* A duplicated native fd would escape the private slot namespace. */
    if (cmd == UWPH_F_DUPFD || cmd == UWPH_F_DUPFD_CLOEXEC) { uwph_errno=UWPH_EINVAL; return -1; }
#endif
    return fdcall(SYS_fcntl,fd,cmd,arg,0);
}

/* Hold the namespace lock through bridge creation/destruction. fdopen borrows
 * the existing descriptor; fclose consumes it, exactly as in POSIX. */
#include <stdio.h>
#include <errno.h>
static int stream_slot(int n, int create)
{
#ifndef UWPH_HARNESS_NATIVE_FDS
    int free_slot = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i] == n + 1) return BASE + i;
        if (!slots[i] && free_slot < 0) free_slot = i;
    }
    if (create && free_slot >= 0) {
        slots[free_slot] = n + 1;
        return BASE + free_slot;
    }
    uwph_errno = create ? UWPH_EMFILE : UWPH_EBADF;
    return -1;
#else
    return n;
#endif
}
uwph_FILE *uwph_linux_fdopen(int fd, const char *mode)
{
    FILE *(*fn)(int, const char *) = uwph_linux_symbol("fdopen");
    WRLOCK();
    int n = native(fd);
    FILE *f = n < 0 ? NULL : fn(n, mode);
    uwph_errno = n < 0 ? UWPH_EBADF : errno;
    UNLOCK();
    return (uwph_FILE *)f;
}
uwph_FILE *uwph_linux_fopen(const char *path, const char *mode)
{
    FILE *(*fn)(const char *, const char *) = uwph_linux_symbol("fopen");
    int (*number)(FILE *) = uwph_linux_symbol("fileno");
    int (*destroy)(FILE *) = uwph_linux_symbol("fclose");
    WRLOCK();
    FILE *f;
    int handled;
    long proc = proc_open(path, !strcmp(mode, "r") || !strcmp(mode, "rb") ? 0 : UWPH_O_WRONLY, &handled);
    if (!handled) f = fn(path, mode);
    else if (proc < 0) { errno = -proc; f = NULL; }
    else {
        FILE *(*wrap)(int, const char *) = uwph_linux_symbol("fdopen");
        f = wrap(proc, mode);
        if (!f) raw(SYS_close, proc, 0, 0, 0, 0, 0);
    }
    uwph_errno = errno;
    if (f && stream_slot(number(f), 1) < 0) { destroy(f); f = NULL; }
    UNLOCK();
    return (uwph_FILE *)f;
}
int uwph_linux_fileno(uwph_FILE *f)
{
    int (*number)(FILE *) = uwph_linux_symbol("fileno");
    WRLOCK();
    int n = number((FILE *)f);
    /* Standard streams remain borrowed; never adopt arbitrary native FILEs. */
    int fd = n < 0 ? (uwph_errno = errno, -1) : stream_slot(n, 0);
    UNLOCK();
    return fd;
}
int uwph_linux_fclose(uwph_FILE *f)
{
    int (*number)(FILE *) = uwph_linux_symbol("fileno");
    int (*destroy)(FILE *) = uwph_linux_symbol("fclose");
    WRLOCK();
#ifndef UWPH_HARNESS_NATIVE_FDS
    int n = number((FILE *)f);
    if (n >= 0) {
        int fd = stream_slot(n, 0);
        if (fd >= 0) slots[fd - BASE] = 0;
    }
#else
    (void)number;
#endif
    int r = destroy((FILE *)f);
    uwph_errno = errno;
    UNLOCK();
    return r;
}

int uwph_linux_isatty(int fd)
{
    RDLOCK();
    int n = native(fd);
    unsigned char term[64];
    long r = n < 0 ? -UWPH_EBADF : raw(SYS_ioctl,n,0x5401,PTR(term),0,0,0);
    UNLOCK();
    return result(r) == 0;
}
int uwph_linux_pipe2(int fds[2], int flags)
{
    int pair[2];
    WRLOCK();
    long r = raw(SYS_pipe2,PTR(pair),flags,0,0,0,0);
#ifndef UWPH_HARNESS_NATIVE_FDS
    if (r == 0) {
        int a, b;
        for (a=0; a<SLOTS && slots[a]; a++);
        for (b=a+1; b<SLOTS && slots[b]; b++);
        if (b>=SLOTS) {
            raw(SYS_close,pair[0],0,0,0,0,0); raw(SYS_close,pair[1],0,0,0,0,0);
            r=-UWPH_EMFILE;
        } else {
            slots[a]=pair[0]+1; slots[b]=pair[1]+1;
            pair[0]=BASE+a; pair[1]=BASE+b;
        }
    }
#endif
    if (r==0) { fds[0]=pair[0]; fds[1]=pair[1]; }
    UNLOCK();
    return result(r);
}

int uwph_linux_native_fds(void)
{
#ifdef UWPH_HARNESS_NATIVE_FDS
    return 1;
#else
    return 0;
#endif
}
